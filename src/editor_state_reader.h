/**************************************************************************/
/*  editor_state_reader.h                                                 */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_STATE_READER_H
#define BARISTA_MCP_EDITOR_STATE_READER_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class EditorInterface;

// One parsed run_editor_action request. Every operation-specific field is explicit: an absent field
// is never defaulted into a value, and a field the requested operation does not use is a rejection.
struct EditorOperationRequest {
	String operation;
	bool has_path = false;
	String path;
	bool has_grab_focus = false;
	bool grab_focus = false;
};

// Single source of truth for public editor and scene state. Tools and resources call these same
// readers, so a tool payload and a resource payload can never describe the editor differently. Every
// reader is side-effect free and bounded: nothing here mutates the editor or the edited scene.
class EditorStateReader {
public:
	static Dictionary project_info(EditorInterface *p_editor_interface);
	// The full editor state, with stable project, scenes, selection, script, filesystem, and play
	// sections. Sections are always present; an unavailable editor yields empty, honest sections.
	static Dictionary editor_state(EditorInterface *p_editor_interface);
	// Summary of the edited scene root without traversing it.
	static Dictionary active_scene(EditorInterface *p_editor_interface);
	// Bounded traversal of the edited scene, published with the limits that produced it.
	static Dictionary scene_tree(EditorInterface *p_editor_interface);

	// What one run_editor_action request may report. Every value is handled or rejected by every
	// consumer, and this is the whole vocabulary.
	enum class OperationStatus {
		OK,
		// The editor accepted the request but the requested observable state does not hold yet, so the
		// client must observe the published predicate itself. It is never reported as success.
		PENDING,
		AUTOMATION_DISABLED,
		INVALID_ARGUMENTS,
		INVALID_RESOURCE_PATH,
		INVALID_RESOURCE_TYPE,
		// Entry-time editor state makes the request unsafe to perform, so nothing was performed. It is
		// what keeps a non-idempotent operation from being blindly repeated.
		CONFLICTING_STATE,
		OPERATION_FAILED,
		UNSUPPORTED_CAPABILITY,
		MUTATION_ALREADY_HANDLED,
	};

	// Outcome of validating one client-supplied resource path. Nothing here touches the editor.
	enum class PathStatus {
		OK,
		// The path is not a normalized res:// path, contains traversal, or names nothing.
		INVALID_PATH,
		// The path names a real resource of a type the operation cannot use.
		WRONG_TYPE,
	};

	// The operations run_editor_action advertises. MCPContracts derives its operation enum from this
	// one vocabulary rather than repeating it, so the two can never drift.
	static PackedStringArray operation_vocabulary();
	static PackedStringArray operation_status_vocabulary();
	static String operation_status_name(OperationStatus p_status);
	// The read_editor_state field one operation's postcondition is verified against. Every operation
	// names a field read_editor_state publishes, so the client can observe the same predicate Barista
	// verified rather than a private one.
	static String operation_observable_field(const String &p_operation);
	// One sentence saying what this operation's "ok" was verified to mean, published per operation.
	static String operation_postcondition(const String &p_operation);
	// The resource type an operation's path must name, empty when any existing file is acceptable.
	static String operation_path_type(const String &p_operation);
	static bool operation_uses_path(const String &p_operation);
	static bool operation_uses_focus(const String &p_operation);

	// Parses one operation request. It reads no editor state, so arguments are settled before anything
	// can be captured or mutated. Returns false with a bounded diagnostic for an unknown operation, a
	// missing operation-specific field, and a field the operation does not use.
	static bool parse_operation(const Dictionary &p_arguments, EditorOperationRequest &r_request, String &r_message);
	// The single validator every operation runs against a client-supplied path, before any
	// EditorInterface call. A path that is not a normalized res:// path, contains traversal, names
	// nothing, or names a resource of the wrong type never reaches the editor.
	static PathStatus check_resource_path(const String &p_path, const String &p_type, String &r_message);

	// Observable readings the operation postconditions are decided against. All are side-effect free.
	static String current_scene_path(EditorInterface *p_editor_interface);
	static PackedStringArray open_scene_paths(EditorInterface *p_editor_interface);
	// Only the unsaved scenes that are backed by a res:// file. A scene that has never been saved to a
	// path cannot be saved without naming one, so it is never part of a save postcondition.
	static PackedStringArray unsaved_scene_paths(EditorInterface *p_editor_interface);
	// True when some open scene has unsaved changes and no res:// path yet. Saving such a scene needs
	// a path the client never supplied, and the editor asks for one with a modal dialog, so it is a
	// state a save operation refuses in rather than acts in.
	static bool has_unsaved_untitled_scene(EditorInterface *p_editor_interface);
	static String current_filesystem_path(EditorInterface *p_editor_interface);
	static String current_script_path(EditorInterface *p_editor_interface);

	// The run_editor_action payload for a request that performed nothing. Every failure a client can
	// provoke is published in this one shape.
	static Dictionary operation_failure(
			OperationStatus p_status, const String &p_message, const String &p_operation = String());
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_STATE_READER_H
