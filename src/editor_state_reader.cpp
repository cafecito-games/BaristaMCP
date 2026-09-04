/**************************************************************************/
/*  editor_state_reader.cpp                                               */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_state_reader.h"

#include "editor_automation_types.h"
#include "mcp_contracts.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cmath>
#include <filesystem>
#include <system_error>

namespace godot {

namespace {

String bounded(const String &p_value, int p_limit) {
	if (p_value.length() <= p_limit) {
		return p_value;
	}
	return p_value.substr(0, p_limit);
}

// Publishes at most MAX_LIST_ITEMS entries and says so, rather than silently capping a list.
//
// The engine exposes open scenes, unsaved files, the selection, and open scripts only as whole
// collections: EditorInterface::get_open_scenes, EditorInterface::get_unsaved_scenes,
// EditorSelection::get_selected_nodes, and ScriptEditor::get_open_scripts have no indexed accessor in
// the pinned public API, so the collection is built by the engine before Barista can bound it. The
// cost is therefore proportional to editor state the user already created and is not amplifiable by
// any client argument; everything Barista itself does with the collection is bounded here.
Dictionary bounded_string_list(const PackedStringArray &p_values) {
	Array items;
	const int count = p_values.size();
	const int published = count < EditorSceneLimits::MAX_LIST_ITEMS ? count : EditorSceneLimits::MAX_LIST_ITEMS;
	for (int i = 0; i < published; i++) {
		items.push_back(bounded(p_values[i], EditorSnapshotLimits::MAX_PATH_LENGTH));
	}
	Dictionary list;
	list["count"] = count;
	list["items"] = items;
	list["truncated"] = published < count;
	return list;
}

struct SceneWalker {
	int nodes = 0;
	int visited = 0;
	bool depth_truncated = false;
	bool node_limit_reached = false;
	bool traversal_limit_reached = false;

	// Charges one unit of the published budget before any work is done with it.
	bool spend() {
		if (visited >= EditorSceneLimits::MAX_VISITED_NODES) {
			traversal_limit_reached = true;
			return false;
		}
		visited++;
		return true;
	}

	Dictionary walk(Node *p_node, int p_depth, bool &r_added) {
		r_added = false;
		Dictionary entry;
		if (p_node == nullptr || !spend()) {
			return entry;
		}
		if (nodes >= EditorSceneLimits::MAX_NODES) {
			node_limit_reached = true;
			return entry;
		}
		nodes++;
		r_added = true;
		entry["name"] = bounded(p_node->get_name(), EditorSnapshotLimits::MAX_STRING_LENGTH);
		entry["class"] = p_node->get_class();
		entry["path"] = bounded(String(p_node->get_path()), EditorSnapshotLimits::MAX_PATH_LENGTH);
		entry["scene_file_path"] = bounded(p_node->get_scene_file_path(), EditorSnapshotLimits::MAX_PATH_LENGTH);

		const int64_t child_count = p_node->get_child_count(false);
		entry["child_count"] = child_count;
		Array children;
		bool truncated = false;
		if (p_depth >= EditorSceneLimits::MAX_DEPTH) {
			truncated = child_count > 0;
			depth_truncated = depth_truncated || truncated;
		} else {
			// The remaining budget bounds the scan before it starts, and children are read by index so
			// no listing of an enormous child list is ever materialized.
			const int remaining = EditorSceneLimits::MAX_VISITED_NODES - visited;
			const int scanned = child_count < remaining ? (int)child_count : remaining;
			if (scanned < child_count) {
				traversal_limit_reached = true;
				truncated = true;
			}
			for (int i = 0; i < scanned; i++) {
				bool added = false;
				const Dictionary child = walk(p_node->get_child(i, false), p_depth + 1, added);
				if (!added) {
					truncated = true;
					break;
				}
				children.push_back(child);
			}
		}
		entry["truncated"] = truncated;
		entry["children"] = children;
		return entry;
	}
};

Dictionary scenes_section(EditorInterface *p_editor_interface) {
	Dictionary scenes;
	String current;
	if (p_editor_interface != nullptr) {
		Node *root = p_editor_interface->get_edited_scene_root();
		if (root != nullptr) {
			current = root->get_scene_file_path();
		}
		scenes["open"] = bounded_string_list(p_editor_interface->get_open_scenes());
		scenes["unsaved"] = bounded_string_list(p_editor_interface->get_unsaved_scenes());
	} else {
		scenes["open"] = bounded_string_list(PackedStringArray());
		scenes["unsaved"] = bounded_string_list(PackedStringArray());
	}
	scenes["current"] = bounded(current, EditorSnapshotLimits::MAX_PATH_LENGTH);
	return scenes;
}

Dictionary selection_section(EditorInterface *p_editor_interface) {
	Dictionary selection;
	Array nodes;
	int64_t count = 0;
	bool truncated = false;
	if (p_editor_interface != nullptr) {
		EditorSelection *editor_selection = p_editor_interface->get_selection();
		if (editor_selection != nullptr) {
			const TypedArray<Node> selected = editor_selection->get_selected_nodes();
			count = selected.size();
			const int64_t published =
					count < EditorSceneLimits::MAX_LIST_ITEMS ? count : EditorSceneLimits::MAX_LIST_ITEMS;
			truncated = published < count;
			for (int64_t i = 0; i < published; i++) {
				Node *node = Object::cast_to<Node>(selected[i]);
				if (node == nullptr) {
					continue;
				}
				Dictionary entry;
				entry["name"] = bounded(node->get_name(), EditorSnapshotLimits::MAX_STRING_LENGTH);
				entry["class"] = node->get_class();
				entry["path"] = bounded(String(node->get_path()), EditorSnapshotLimits::MAX_PATH_LENGTH);
				nodes.push_back(entry);
			}
		}
	}
	selection["count"] = count;
	selection["nodes"] = nodes;
	selection["truncated"] = truncated;
	return selection;
}

Dictionary script_section(EditorInterface *p_editor_interface) {
	Dictionary script;
	String current;
	Array open;
	int64_t open_count = 0;
	bool truncated = false;
	Dictionary unsaved = bounded_string_list(PackedStringArray());
	if (p_editor_interface != nullptr) {
		ScriptEditor *script_editor = p_editor_interface->get_script_editor();
		if (script_editor != nullptr) {
			const Ref<Script> current_script = script_editor->get_current_script();
			if (current_script.is_valid()) {
				current = current_script->get_path();
			}
			const TypedArray<Script> scripts = script_editor->get_open_scripts();
			open_count = scripts.size();
			const int64_t published =
					open_count < EditorSceneLimits::MAX_LIST_ITEMS ? open_count : EditorSceneLimits::MAX_LIST_ITEMS;
			truncated = published < open_count;
			for (int64_t i = 0; i < published; i++) {
				Ref<Script> entry = scripts[i];
				if (entry.is_null()) {
					continue;
				}
				open.push_back(bounded(entry->get_path(), EditorSnapshotLimits::MAX_PATH_LENGTH));
			}
			unsaved = bounded_string_list(script_editor->get_unsaved_files());
		}
	}
	script["current"] = bounded(current, EditorSnapshotLimits::MAX_PATH_LENGTH);
	Dictionary open_list;
	open_list["count"] = open_count;
	open_list["items"] = open;
	open_list["truncated"] = truncated;
	script["open"] = open_list;
	script["unsaved"] = unsaved;
	return script;
}

Dictionary filesystem_section(EditorInterface *p_editor_interface) {
	Dictionary filesystem;
	bool scanning = false;
	bool importing = false;
	double progress = 0.0;
	String current_path;
	if (p_editor_interface != nullptr) {
		current_path = p_editor_interface->get_current_path();
		EditorFileSystem *file_system = p_editor_interface->get_resource_filesystem();
		if (file_system != nullptr) {
			scanning = file_system->is_scanning();
			importing = file_system->is_importing();
			progress = file_system->get_scanning_progress();
		}
	}
	filesystem["scanning"] = scanning;
	filesystem["importing"] = importing;
	filesystem["scan_progress"] = progress;
	// The filesystem dock selection, published so a client can observe the very predicate the
	// select_file operation verifies rather than a private one.
	filesystem["current_path"] = bounded(current_path, EditorSnapshotLimits::MAX_PATH_LENGTH);
	return filesystem;
}

Dictionary play_section(EditorInterface *p_editor_interface) {
	Dictionary play;
	bool is_playing = false;
	String playing_scene;
	if (p_editor_interface != nullptr) {
		is_playing = p_editor_interface->is_playing_scene();
		playing_scene = p_editor_interface->get_playing_scene();
	}
	play["is_playing"] = is_playing;
	play["playing_scene"] = bounded(playing_scene, EditorSnapshotLimits::MAX_PATH_LENGTH);
	return play;
}

} // namespace

Dictionary EditorStateReader::project_info(EditorInterface *p_editor_interface) {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	Dictionary info;
	info["project_name"] = project_settings->get_setting("application/config/name", "");
	info["project_path"] = project_settings->globalize_path("res://").trim_suffix("/");
	info["godot_version"] = Engine::get_singleton()->get_version_info();

	String current_scene;
	bool is_playing = false;
	if (p_editor_interface != nullptr) {
		Node *root = p_editor_interface->get_edited_scene_root();
		if (root != nullptr) {
			current_scene = root->get_scene_file_path();
		}
		is_playing = p_editor_interface->is_playing_scene();
	}
	info["current_scene"] = current_scene;
	info["is_playing"] = is_playing;
	return info;
}

Dictionary EditorStateReader::editor_state(EditorInterface *p_editor_interface) {
	Dictionary state;
	state["project"] = project_info(p_editor_interface);
	state["scenes"] = scenes_section(p_editor_interface);
	state["selection"] = selection_section(p_editor_interface);
	state["script"] = script_section(p_editor_interface);
	state["filesystem"] = filesystem_section(p_editor_interface);
	state["play"] = play_section(p_editor_interface);
	return state;
}

Dictionary EditorStateReader::active_scene(EditorInterface *p_editor_interface) {
	Dictionary scene;
	Node *root = p_editor_interface == nullptr ? nullptr : p_editor_interface->get_edited_scene_root();
	scene["has_scene"] = root != nullptr;
	scene["scene_path"] =
			root == nullptr ? String() : bounded(root->get_scene_file_path(), EditorSnapshotLimits::MAX_PATH_LENGTH);
	scene["root_name"] =
			root == nullptr ? String() : bounded(root->get_name(), EditorSnapshotLimits::MAX_STRING_LENGTH);
	scene["root_class"] = root == nullptr ? String() : root->get_class();
	scene["child_count"] = root == nullptr ? (int64_t)0 : root->get_child_count(false);
	scene["scenes"] = scenes_section(p_editor_interface);
	scene["play"] = play_section(p_editor_interface);
	return scene;
}

Dictionary EditorStateReader::scene_tree(EditorInterface *p_editor_interface) {
	Node *root = p_editor_interface == nullptr ? nullptr : p_editor_interface->get_edited_scene_root();
	SceneWalker walker;
	Array tree;
	bool added = false;
	if (root != nullptr) {
		const Dictionary entry = walker.walk(root, 0, added);
		if (added) {
			tree.push_back(entry);
		}
	}

	Dictionary limits;
	limits["max_depth"] = EditorSceneLimits::MAX_DEPTH;
	limits["max_nodes"] = EditorSceneLimits::MAX_NODES;
	limits["depth_truncated"] = walker.depth_truncated;
	limits["node_limit_reached"] = walker.node_limit_reached;
	limits["traversal_limit_reached"] = walker.traversal_limit_reached;

	Dictionary payload;
	payload["has_scene"] = root != nullptr;
	payload["scene_path"] =
			root == nullptr ? String() : bounded(root->get_scene_file_path(), EditorSnapshotLimits::MAX_PATH_LENGTH);
	payload["node_count"] = walker.nodes;
	payload["truncated"] = walker.depth_truncated || walker.node_limit_reached || walker.traversal_limit_reached;
	payload["limits"] = limits;
	payload["tree"] = tree;
	return payload;
}

namespace {

// Single definition of every advertised operation: its name, the read_editor_state field its
// postcondition is verified against, what "ok" was verified to mean, the resource type its path must
// name, and which operation-specific fields it uses. Every consumer reads this one table, so an
// operation cannot be advertised with a postcondition or a claim that lives somewhere else.
struct OperationRule {
	const char *name;
	const char *observable;
	const char *postcondition;
	// nullptr when the operation takes no path at all; an empty string when any existing res:// file
	// is acceptable; otherwise the resource type the path must name.
	const char *path_type;
	bool uses_caret;
};

const OperationRule OPERATION_RULES[] = {
		{"save_scene", "scenes.unsaved",
				"The edited scene's own file is no longer listed as unsaved after the request.", nullptr, false},
		{"save_all_scenes", "scenes.unsaved",
				"No file-backed scene that was unsaved when the request arrived is still unsaved.", nullptr, false},
		{"open_scene", "scenes.current", "The requested scene is the edited scene.", "PackedScene", false},
		{"reload_scene", "scenes.unsaved",
				"The requested scene is open and carries no unsaved changes after the reload.", "PackedScene", false},
		{"play_current_scene", "play.is_playing", "The editor reports a scene is playing.", nullptr, false},
		{"play_main_scene", "play.is_playing", "The editor reports a scene is playing.", nullptr, false},
		{"play_scene", "play.playing_scene", "The editor reports the requested scene is the playing scene.",
				"PackedScene", false},
		{"stop_play", "play.is_playing", "The editor reports no scene is playing.", nullptr, false},
		{"select_file", "filesystem.current_path", "The filesystem dock's current path is the requested file.", "",
				false},
		{"edit_script", "script.current",
				"The requested script is the script editor's current script. The optional line and column are "
				"caret hints the public API cannot report back, so they are explicitly not part of this claim.",
				"Script", true},
};

const OperationRule *find_operation_rule(const String &p_operation) {
	for (const OperationRule &rule : OPERATION_RULES) {
		if (p_operation == rule.name) {
			return &rule;
		}
	}
	return nullptr;
}

// Every argument run_editor_action understands. A key outside this list is a rejection, never noise.
const char *OPERATION_ARGUMENT_NAMES[] = {"operation", "path", "line", "column", "grab_focus"};

bool is_known_argument(const String &p_name) {
	for (const char *name : OPERATION_ARGUMENT_NAMES) {
		if (p_name == name) {
			return true;
		}
	}
	return false;
}

// True only when p_path resolves to a real location under the resolved project root. Both sides are
// canonicalized, so a symlink anywhere along the path is followed before the comparison and cannot
// smuggle a target out of the project. A resolution that fails is not inside.
bool is_inside_project(const String &p_path) {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings == nullptr) {
		return false;
	}
	const CharString root_utf8 = project_settings->globalize_path("res://").trim_suffix("/").utf8();
	const CharString file_utf8 = project_settings->globalize_path(p_path).utf8();
	std::error_code root_error;
	std::error_code file_error;
	const std::filesystem::path root =
			std::filesystem::weakly_canonical(std::filesystem::path(root_utf8.get_data()), root_error);
	const std::filesystem::path file =
			std::filesystem::weakly_canonical(std::filesystem::path(file_utf8.get_data()), file_error);
	if (root_error || file_error || root.empty()) {
		return false;
	}
	std::filesystem::path::const_iterator root_part = root.begin();
	std::filesystem::path::const_iterator file_part = file.begin();
	for (; root_part != root.end(); ++root_part, ++file_part) {
		if (file_part == file.end() || *file_part != *root_part) {
			return false;
		}
	}
	// The project root itself is a directory, never an operable file, so a path equal to it is outside
	// the set of files an operation may name.
	return file_part != file.end();
}

// Characters that can never appear in a path Barista will hand to the editor. Percent and backslash
// are refused outright so no encoded or alternately separated form of a traversal can survive, and
// every control character is refused so a path can never be truncated somewhere else into a
// different one.
bool is_forbidden_path_character(char32_t p_character) {
	if (p_character < 0x20 || p_character == 0x7f) {
		return true;
	}
	switch (p_character) {
		case '\\':
		case '%':
		case ':':
		case '*':
		case '?':
		case '"':
		case '<':
		case '>':
		case '|':
			return true;
		default:
			return false;
	}
}

} // namespace

PackedStringArray EditorStateReader::operation_vocabulary() {
	PackedStringArray operations;
	for (const OperationRule &rule : OPERATION_RULES) {
		operations.push_back(rule.name);
	}
	return operations;
}

PackedStringArray EditorStateReader::operation_status_vocabulary() {
	PackedStringArray statuses;
	statuses.push_back(operation_status_name(OperationStatus::OK));
	statuses.push_back(operation_status_name(OperationStatus::PENDING));
	statuses.push_back(operation_status_name(OperationStatus::AUTOMATION_DISABLED));
	statuses.push_back(operation_status_name(OperationStatus::INVALID_ARGUMENTS));
	statuses.push_back(operation_status_name(OperationStatus::INVALID_RESOURCE_PATH));
	statuses.push_back(operation_status_name(OperationStatus::INVALID_RESOURCE_TYPE));
	statuses.push_back(operation_status_name(OperationStatus::CONFLICTING_STATE));
	statuses.push_back(operation_status_name(OperationStatus::OPERATION_FAILED));
	statuses.push_back(operation_status_name(OperationStatus::UNSUPPORTED_CAPABILITY));
	statuses.push_back(operation_status_name(OperationStatus::MUTATION_ALREADY_HANDLED));
	return statuses;
}

String EditorStateReader::operation_status_name(OperationStatus p_status) {
	switch (p_status) {
		case OperationStatus::OK:
			return "ok";
		case OperationStatus::PENDING:
			return "pending";
		case OperationStatus::AUTOMATION_DISABLED:
			return "automation_disabled";
		case OperationStatus::INVALID_ARGUMENTS:
			return "invalid_arguments";
		case OperationStatus::INVALID_RESOURCE_PATH:
			return "invalid_resource_path";
		case OperationStatus::INVALID_RESOURCE_TYPE:
			return "invalid_resource_type";
		case OperationStatus::CONFLICTING_STATE:
			return "conflicting_state";
		case OperationStatus::OPERATION_FAILED:
			return "operation_failed";
		case OperationStatus::UNSUPPORTED_CAPABILITY:
			return "unsupported_capability";
		case OperationStatus::MUTATION_ALREADY_HANDLED:
			return "mutation_already_handled";
	}
	return "operation_failed";
}

String EditorStateReader::operation_observable_field(const String &p_operation) {
	const OperationRule *rule = find_operation_rule(p_operation);
	return rule == nullptr ? String() : String(rule->observable);
}

String EditorStateReader::operation_postcondition(const String &p_operation) {
	const OperationRule *rule = find_operation_rule(p_operation);
	return rule == nullptr ? String() : String(rule->postcondition);
}

String EditorStateReader::operation_path_type(const String &p_operation) {
	const OperationRule *rule = find_operation_rule(p_operation);
	return rule == nullptr || rule->path_type == nullptr ? String() : String(rule->path_type);
}

bool EditorStateReader::operation_uses_path(const String &p_operation) {
	const OperationRule *rule = find_operation_rule(p_operation);
	return rule != nullptr && rule->path_type != nullptr;
}

bool EditorStateReader::operation_uses_caret(const String &p_operation) {
	const OperationRule *rule = find_operation_rule(p_operation);
	return rule != nullptr && rule->uses_caret;
}

bool EditorStateReader::parse_operation(
		const Dictionary &p_arguments, EditorOperationRequest &r_request, String &r_message) {
	r_request = EditorOperationRequest();

	const Array keys = p_arguments.keys();
	for (int i = 0; i < keys.size(); i++) {
		const Variant key = keys[i];
		if (key.get_type() != Variant::STRING || !is_known_argument(String(key))) {
			r_message = "run_editor_action rejected an argument it does not define.";
			return false;
		}
	}

	const Variant operation_value = p_arguments.get("operation", Variant());
	if (operation_value.get_type() != Variant::STRING || find_operation_rule(String(operation_value)) == nullptr) {
		r_message = "run_editor_action requires 'operation' to name one advertised operation.";
		return false;
	}
	r_request.operation = String(operation_value);

	const bool uses_path = operation_uses_path(r_request.operation);
	if (p_arguments.has("path")) {
		const Variant path_value = p_arguments.get("path", Variant());
		if (!uses_path) {
			r_message = "Operation '" + r_request.operation + "' takes no 'path'.";
			return false;
		}
		if (path_value.get_type() != Variant::STRING) {
			r_message = "Operation '" + r_request.operation + "' requires 'path' to be a string.";
			return false;
		}
		r_request.has_path = true;
		r_request.path = String(path_value);
	} else if (uses_path) {
		r_message = "Operation '" + r_request.operation + "' requires a 'path'.";
		return false;
	}

	const bool uses_caret = operation_uses_caret(r_request.operation);
	struct CaretField {
		const char *name;
		int minimum;
		int maximum;
		bool *has_value;
		int *value;
	};
	const CaretField caret_fields[] = {
			{"line", EditorOperationLimits::MIN_LINE, EditorOperationLimits::MAX_LINE, &r_request.has_line,
					&r_request.line},
			{"column", EditorOperationLimits::MIN_COLUMN, EditorOperationLimits::MAX_COLUMN, &r_request.has_column,
					&r_request.column},
	};
	for (const CaretField &field : caret_fields) {
		if (!p_arguments.has(field.name)) {
			continue;
		}
		if (!uses_caret) {
			r_message = "Operation '" + r_request.operation + "' takes no '" + String(field.name) + "'.";
			return false;
		}
		const Variant field_value = p_arguments.get(field.name, Variant());
		// JSON carries every number as a double, so an integral float is a valid JSON Schema integer.
		// The value is range-checked as a double before it is narrowed, so a number no fixed-width
		// integer can hold is rejected rather than converted; NaN and the infinities fail the same
		// comparison and are refused with everything else out of range.
		double raw = 0.0;
		if (field_value.get_type() == Variant::INT) {
			raw = (double)(int64_t)field_value;
		} else if (field_value.get_type() == Variant::FLOAT) {
			raw = (double)field_value;
		} else {
			r_message =
					"Operation '" + r_request.operation + "' requires '" + String(field.name) + "' to be an integer.";
			return false;
		}
		if (!(raw >= (double)field.minimum && raw <= (double)field.maximum) || raw != std::floor(raw)) {
			r_message = "Operation '" + r_request.operation + "' rejected an out-of-range or non-integral '" +
					String(field.name) + "'.";
			return false;
		}
		*field.has_value = true;
		*field.value = (int)raw;
	}

	if (p_arguments.has("grab_focus")) {
		if (!uses_caret) {
			r_message = "Operation '" + r_request.operation + "' takes no 'grab_focus'.";
			return false;
		}
		const Variant focus_value = p_arguments.get("grab_focus", Variant());
		if (focus_value.get_type() != Variant::BOOL) {
			r_message = "Operation '" + r_request.operation + "' requires 'grab_focus' to be a boolean.";
			return false;
		}
		r_request.has_grab_focus = true;
		r_request.grab_focus = (bool)focus_value;
	}

	return true;
}

EditorStateReader::PathStatus EditorStateReader::check_resource_path(
		const String &p_path, const String &p_type, String &r_message) {
	const String prefix = "res://";
	if (p_path.length() == 0 || p_path.length() > EditorSnapshotLimits::MAX_PATH_LENGTH) {
		r_message = "A resource path must be a non-empty res:// path of at most " +
				String::num_int64(EditorSnapshotLimits::MAX_PATH_LENGTH) + " characters.";
		return PathStatus::INVALID_PATH;
	}
	if (!p_path.begins_with(prefix)) {
		r_message = "A resource path must begin with 'res://'; no other scheme and no absolute path is accepted.";
		return PathStatus::INVALID_PATH;
	}

	const String remainder = p_path.substr(prefix.length());
	if (remainder.is_empty()) {
		r_message = "A resource path must name a file inside res://.";
		return PathStatus::INVALID_PATH;
	}
	for (int i = 0; i < remainder.length(); i++) {
		if (is_forbidden_path_character(remainder[i])) {
			r_message = "A resource path may not contain control characters, '%', '\\', or ':'.";
			return PathStatus::INVALID_PATH;
		}
	}

	const PackedStringArray segments = remainder.split("/", true);
	for (int i = 0; i < segments.size(); i++) {
		const String segment = segments[i];
		if (segment.is_empty() || segment == "." || segment == ".." || segment != segment.strip_edges()) {
			r_message = "A resource path must be normalized: empty, '.', '..', and space-padded segments are refused.";
			return PathStatus::INVALID_PATH;
		}
	}
	// A second, independent normalization gate. The segment rules above already refuse traversal, and
	// this refuses anything the engine's own simplification would rewrite, so no path Barista accepts
	// can mean something other than what it says.
	if (p_path != p_path.simplify_path()) {
		r_message = "A resource path must already be in its normalized form.";
		return PathStatus::INVALID_PATH;
	}

	if (!FileAccess::file_exists(p_path)) {
		r_message = "No file exists at '" + p_path + "'.";
		return PathStatus::INVALID_PATH;
	}
	// Lexical normalization alone would still let a symlink inside the project point outside it, so the
	// resolved location is required to stay under the resolved project root. Anything that cannot be
	// resolved is refused rather than assumed to be inside.
	if (!is_inside_project(p_path)) {
		r_message = "'" + p_path + "' resolves outside the project directory.";
		return PathStatus::INVALID_PATH;
	}
	if (!p_type.is_empty()) {
		ResourceLoader *loader = ResourceLoader::get_singleton();
		// ResourceLoader::exists only asks whether some loader recognizes the path, so on its own it
		// would accept a script where a scene is required. The extensions the engine itself publishes
		// for the required type are what decides the type, and exists() then confirms a loader is
		// actually there for it.
		const PackedStringArray extensions =
				loader == nullptr ? PackedStringArray() : loader->get_recognized_extensions_for_type(p_type);
		if (loader == nullptr || !extensions.has(p_path.get_extension().to_lower()) ||
				!loader->exists(p_path, p_type)) {
			r_message = "'" + p_path + "' is not a " + p_type + " resource.";
			return PathStatus::WRONG_TYPE;
		}
	}
	r_message = String();
	return PathStatus::OK;
}

String EditorStateReader::current_scene_path(EditorInterface *p_editor_interface) {
	Node *root = p_editor_interface == nullptr ? nullptr : p_editor_interface->get_edited_scene_root();
	return root == nullptr ? String() : root->get_scene_file_path();
}

PackedStringArray EditorStateReader::open_scene_paths(EditorInterface *p_editor_interface) {
	return p_editor_interface == nullptr ? PackedStringArray() : p_editor_interface->get_open_scenes();
}

PackedStringArray EditorStateReader::unsaved_scene_paths(EditorInterface *p_editor_interface) {
	PackedStringArray unsaved;
	if (p_editor_interface == nullptr) {
		return unsaved;
	}
	const PackedStringArray reported = p_editor_interface->get_unsaved_scenes();
	for (int i = 0; i < reported.size(); i++) {
		if (reported[i].begins_with("res://")) {
			unsaved.push_back(reported[i]);
		}
	}
	return unsaved;
}

String EditorStateReader::current_filesystem_path(EditorInterface *p_editor_interface) {
	return p_editor_interface == nullptr ? String() : p_editor_interface->get_current_path();
}

String EditorStateReader::current_script_path(EditorInterface *p_editor_interface) {
	ScriptEditor *script_editor = p_editor_interface == nullptr ? nullptr : p_editor_interface->get_script_editor();
	if (script_editor == nullptr) {
		return String();
	}
	const Ref<Script> current = script_editor->get_current_script();
	return current.is_valid() ? current->get_path() : String();
}

Dictionary EditorStateReader::operation_failure(
		OperationStatus p_status, const String &p_message, const String &p_operation) {
	Dictionary payload;
	payload["ok"] = false;
	payload["status"] = operation_status_name(p_status);
	payload["message"] = p_message;
	payload["changed"] = false;
	payload["pending"] = p_status == OperationStatus::PENDING;
	if (!p_operation.is_empty()) {
		payload["operation"] = p_operation;
		// The claim travels with every result, refusals included, so a client reads the standard this
		// route was held to from the same payload that reports the outcome.
		const String claim = MCPContracts::operation_claim(p_operation);
		if (!claim.is_empty()) {
			payload["claim"] = claim;
		}
	}
	Dictionary observable;
	observable["field"] = operation_observable_field(p_operation);
	observable["expected"] = String();
	observable["observed"] = String();
	observable["satisfied"] = false;
	payload["observable"] = observable;
	return payload;
}

} // namespace godot
