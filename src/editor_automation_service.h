/**************************************************************************/
/*  editor_automation_service.h                                           */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
#define BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H

#include "editor_action_driver.h"
#include "editor_automation_types.h"
#include "editor_event_log.h"
#include "editor_state_reader.h"
#include "editor_wait_manager.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <map>
#include <vector>

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
		// The options the capture that issued this handle actually applied. A later capture is not
		// automatically a superset of an earlier one, so resolution is widened to cover them.
		EditorSnapshotOptions options;
	};

	EditorInterface *editor_interface = nullptr;
	// Frozen at server startup by the plugin and never reconsidered while the server runs, so no
	// request can widen what this session is allowed to do.
	bool automation_enabled = false;
	uint64_t generation = 0;
	std::map<uint64_t, IssuedHandle> issued_handles;
	// The capture a cursor can be resumed against. Pagination reuses it instead of recapturing, so a
	// page boundary always refers to the generation its cursor names.
	EditorSnapshotData cached_snapshot;
	bool has_cached_snapshot = false;
	// The canonical form of the selector that produced the cached capture. A cursor is resumable only
	// for that exact query, so identity never rests on a hash alone.
	String cached_selector;
	// The bounded event ring and the cooperative wait handles this session owns. Waits are advanced
	// only from process frames, so no request ever blocks the editor thread waiting for the editor.
	EditorEventLog event_log;
	EditorWaitManager wait_manager;

	// Captures a snapshot inside the published payload budget and registers every handle it issued.
	bool _capture(const EditorSnapshotOptions &p_requested, EditorSnapshotData &r_data, Dictionary &r_payload,
			String &r_error, String &r_message);
	void _register_handles(const EditorSnapshotData &p_data);
	void _register_elements(const std::vector<EditorElement> &p_elements, uint64_t p_generation,
			const EditorSnapshotOptions &p_options, int &r_budget);
	// Resolves a handle against the issued-handle registry and a freshly captured snapshot. Returns
	// false with "stale_handle" for a handle Barista never issued, one that no longer resolves, and one
	// whose object is no longer of the recorded type.
	bool _resolve_handle(const String &p_handle, const EditorSnapshotOptions &p_options, EditorSnapshotData &r_data,
			const EditorElement **r_element, String &r_error, String &r_message);
	// Resolves the single element a selector names, against entry-time registry state and one fresh
	// capture. Returns false with the act payload already filled in for every selector failure.
	bool _resolve_target(const Dictionary &p_arguments, const EditorActionRequest &p_request,
			EditorSnapshotData &r_data, const EditorElement **r_element, Dictionary &r_failure, String &r_error,
			String &r_message);
	// The run_editor_action body. It reads every entry-time observable before any branch that can
	// mutate, so no request is ever validated against state that same request created.
	Dictionary _run_operation(const EditorOperationRequest &p_request);
	// The act_on_editor_ui body. It appends one bounded trace entry per decision it makes, so a
	// failure can publish how far the request got without the caller reconstructing it.
	Dictionary _act_ui(const Dictionary &p_arguments, std::vector<String> &r_trace, String &r_error, String &r_message);
	// Assembles the editor reading one evaluation tick is decided against. A capture is taken only
	// when p_capture is true, and it never touches the public snapshot generation, the issued-handle
	// registry, or the cached capture a selector cursor resumes against.
	void _build_wait_context(
			bool p_capture, uint64_t p_now_ms, EditorWaitContext &r_context, EditorSnapshotData &r_snapshot);

public:
	void configure(EditorInterface *p_editor_interface, bool p_automation_enabled);
	bool is_automation_enabled() const;

	// Returns the structured inspect_editor_ui payload. On failure r_error holds a stable failure
	// code and the returned dictionary holds a bounded message.
	Dictionary inspect_ui(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured find_editor_ui payload. Selector, cursor, and handle problems are reported
	// inside the payload as a selector status; r_error is set only when the editor cannot be inspected.
	Dictionary find_ui(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured act_on_editor_ui payload. Every client-provokable failure is reported
	// inside the payload as an action status; r_error is set only when the editor cannot be inspected.
	Dictionary act_ui(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured run_editor_action payload: one explicit editor operation, performed
	// through the documented EditorInterface method that corresponds to it. Every client-provokable
	// failure is reported inside the payload as an operation status; r_error is set only when the
	// editor cannot be reached at all.
	Dictionary run_operation(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured wait_for_editor payload: start, poll, or cancel one bounded cooperative
	// wait. Every client-provokable failure is reported inside the payload as a wait status.
	Dictionary wait_for_editor(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns the structured poll_barista_events payload: one bounded page of the Barista-owned event
	// ring, resumed from a marker this server issued.
	Dictionary poll_events(const Dictionary &p_arguments, String &r_error, String &r_message);
	// Returns one element, with or without its subtree, for the barista://ui/element and
	// barista://ui/subtree resource templates.
	Dictionary read_element(const String &p_handle, bool p_include_children, String &r_error, String &r_message);

	void process(double p_delta);
	void shutdown();

	static EditorSnapshotOptions parse_options(const Dictionary &p_arguments);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
