/**************************************************************************/
/*  editor_state_reader.cpp                                               */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_state_reader.h"

#include "editor_automation_types.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

String bounded(const String &p_value, int p_limit) {
	if (p_value.length() <= p_limit) {
		return p_value;
	}
	return p_value.substr(0, p_limit);
}

// Publishes at most MAX_LIST_ITEMS entries and says so, rather than silently capping a list.
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
	if (p_editor_interface != nullptr) {
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

} // namespace godot
