/**************************************************************************/
/*  editor_automation_service.h                                           */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
#define BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H

#include "editor_automation_types.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <map>

namespace godot {

class EditorInterface;

// Editor-facing facade owned by the plugin. It holds the monotonic snapshot generation and keeps
// every capture inside the published limits; it never mutates the editor.
class EditorAutomationService {
	// One handle Barista has issued, remembered so a client-supplied handle is never turned into a raw
	// object lookup. The recorded class is re-checked on every resolution, so a reused instance id
	// belonging to a different object reads as stale rather than as the original element.
	struct IssuedHandle {
		String class_name;
		uint64_t generation = 0;
	};

	EditorInterface *editor_interface = nullptr;
	uint64_t generation = 0;
	std::map<uint64_t, IssuedHandle> issued_handles;
	// The capture a cursor can be resumed against. Pagination reuses it instead of recapturing, so a
	// page boundary always refers to the generation its cursor names.
	EditorSnapshotData cached_snapshot;
	bool has_cached_snapshot = false;

	// Captures a snapshot inside the published payload budget and registers every handle it issued.
	bool _capture(const EditorSnapshotOptions &p_requested, EditorSnapshotData &r_data, Dictionary &r_payload,
			String &r_error, String &r_message);
	void _register_handles(const EditorSnapshotData &p_data);
	void _register_elements(const std::vector<EditorElement> &p_elements, uint64_t p_generation, int &r_budget);
	// Resolves a handle against the issued-handle registry and a freshly captured snapshot. Returns
	// false with "stale_handle" for a handle Barista never issued, one that no longer resolves, and one
	// whose object is no longer of the recorded type.
	bool _resolve_handle(const String &p_handle, const EditorSnapshotOptions &p_options, EditorSnapshotData &r_data,
			const EditorElement **r_element, String &r_error, String &r_message);

public:
	void configure(EditorInterface *p_editor_interface);

	// Returns the structured inspect_editor_ui payload. On failure r_error holds a stable failure
	// code and the returned dictionary holds a bounded message.
	Dictionary inspect_ui(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured find_editor_ui payload. Selector, cursor, and handle problems are reported
	// inside the payload as a selector status; r_error is set only when the editor cannot be inspected.
	Dictionary find_ui(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns one element, with or without its subtree, for the barista://ui/element and
	// barista://ui/subtree resource templates.
	Dictionary read_element(const String &p_handle, bool p_include_children, String &r_error, String &r_message);

	void process(double p_delta);
	void shutdown();

	static EditorSnapshotOptions parse_options(const Dictionary &p_arguments);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
