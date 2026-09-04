/**************************************************************************/
/*  mcp_contracts.cpp                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "mcp_contracts.h"

#include "editor_action_driver.h"
#include "editor_automation_types.h"
#include "editor_event_log.h"
#include "editor_selector.h"
#include "editor_snapshot.h"
#include "editor_wait_manager.h"
#include "mcp_schema.h"

#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

// What each action route's "ok" asserts. A delivery route promises the input reached the exact
// requested target and nothing about the editor's response; an effect route promises the requested
// state holds and verifies it before reporting success. Every advertised action appears here, and a
// new one without an entry publishes no claim, which a contract test refuses.
struct ActionClaimRule {
	const char *action;
	const char *claim;
};

const ActionClaimRule ACTION_CLAIM_RULES[] = {
		{"click", MCPContracts::CLAIM_DELIVERY},
		{"submit", MCPContracts::CLAIM_DELIVERY},
		{"type_text", MCPContracts::CLAIM_DELIVERY},
		{"focus", MCPContracts::CLAIM_EFFECT},
		{"set_text", MCPContracts::CLAIM_EFFECT},
		{"set_checked", MCPContracts::CLAIM_EFFECT},
		{"set_value", MCPContracts::CLAIM_EFFECT},
		{"select_item", MCPContracts::CLAIM_EFFECT},
		{"select_tab", MCPContracts::CLAIM_EFFECT},
		{"scroll", MCPContracts::CLAIM_EFFECT},
};

Dictionary status_output_schema() {
	Dictionary schema = MCPSchema::object("Local server, protocol, and lifecycle status.");
	MCPSchema::add_property(schema, "name", MCPSchema::string("Server name."), true);
	MCPSchema::add_property(schema, "version", MCPSchema::string("Server version."), true);
	MCPSchema::add_property(schema, "protocol_version", MCPSchema::string("Negotiated MCP protocol version."), true);
	MCPSchema::add_property(schema, "initialized", MCPSchema::boolean("Whether the MCP lifecycle completed."), true);
	MCPSchema::add_property(
			schema, "local_only", MCPSchema::boolean("Always true; the server binds loopback only."), true);
	MCPSchema::add_property(schema, "endpoint", MCPSchema::string("Loopback MCP endpoint URL."), true);
	MCPSchema::add_property(schema, "port", MCPSchema::integer("Bound loopback TCP port."), true);
	MCPSchema::add_property(schema, "automation_enabled",
			MCPSchema::boolean("Whether editor mutation was enabled before startup. Frozen for the life of "
							   "the server."),
			true);
	return schema;
}

Dictionary project_output_schema() {
	Dictionary schema = MCPSchema::object("Stock Godot project, version, scene, and play state.");
	MCPSchema::add_property(schema, "project_name", MCPSchema::string("Configured project name."), true);
	MCPSchema::add_property(schema, "project_path", MCPSchema::string("Absolute path of the project root."), true);
	MCPSchema::add_property(schema, "godot_version", MCPSchema::open_object("Engine version info dictionary."), true);
	MCPSchema::add_property(schema, "current_scene",
			MCPSchema::string("Scene file path of the edited scene root, or an empty string."), true);
	MCPSchema::add_property(schema, "is_playing", MCPSchema::boolean("Whether the editor is playing a scene."), true);
	return schema;
}

constexpr const char *UI_ELEMENT_DEFINITION = "ui_element";
constexpr const char *UI_MATCH_DEFINITION = "ui_match";

// One field set describes an element everywhere it is published. A match list carries the same fields
// with an always-empty "children" array, so a single page can never carry a whole subtree.
Dictionary ui_element_schema(bool p_recursive = true) {
	Dictionary schema = MCPSchema::object("One semantic editor control or window.");
	MCPSchema::add_property(schema, "id",
			MCPSchema::string("Snapshot-scoped element id, published for correlation inside one capture. "
							  "It is not a selector field and is not replayable; use \"handle\" for identity."),
			true);
	MCPSchema::add_property(
			schema, "handle", MCPSchema::string("Durable opaque handle for stable public identity."), true);
	MCPSchema::add_property(schema, "role",
			MCPSchema::enum_string(EditorSnapshot::role_vocabulary(), "Semantic role of the element."), true);
	MCPSchema::add_property(
			schema, "name", MCPSchema::string("Accessibility name, text, tooltip, or node name."), true);
	MCPSchema::add_property(schema, "text", MCPSchema::string("Bounded displayed or edited text."), true);
	MCPSchema::add_property(schema, "class", MCPSchema::string("Public Godot class name."), true);
	MCPSchema::add_property(schema, "path", MCPSchema::string("Bounded hierarchy path, for diagnostics only."), true);
	MCPSchema::add_property(schema, "visible", MCPSchema::boolean("Whether the element is visible in the tree."), true);
	MCPSchema::add_property(schema, "enabled", MCPSchema::boolean("Whether the element accepts interaction."), true);
	MCPSchema::add_property(schema, "focused", MCPSchema::boolean("Whether the element currently holds focus."), true);
	MCPSchema::add_property(schema, "internal",
			MCPSchema::boolean("Whether the element is an internal child, visible only with include_internal."), true);
	MCPSchema::add_property(schema, "truncated",
			MCPSchema::boolean("Whether children were omitted because a published limit was reached."), true);
	MCPSchema::add_property(schema, "bounds",
			MCPSchema::array(MCPSchema::number(), "Bounds in the owning window as [x, y, width, height]."), true);
	MCPSchema::add_property(schema, "actions",
			MCPSchema::array(MCPSchema::enum_string(EditorSnapshot::action_vocabulary()),
					"Actions this specific element can support through public APIs."),
			true);
	MCPSchema::add_property(schema, "state", MCPSchema::open_object("Bounded public state for this role."), true);
	MCPSchema::add_property(schema, "children",
			p_recursive ? MCPSchema::array(MCPSchema::reference(UI_ELEMENT_DEFINITION), "Child elements, recursively.")
						: MCPSchema::array(MCPSchema::object(), "Always empty; read a subtree resource for children."),
			true);
	return schema;
}

Dictionary selector_input_schema() {
	String description = "Semantic selector. Every constraint must hold; an absent constraint never "
						 "broadens the match. Fields: ";
	const PackedStringArray fields = EditorSelector::field_vocabulary();
	for (int i = 0; i < fields.size(); i++) {
		description += (i == 0 ? String() : String(", ")) + fields[i];
	}
	description += ". EditorSelector owns this vocabulary and rejects an unknown field, a field of the "
				   "wrong type, and an empty selector with the invalid_selector status.";
	return MCPSchema::open_object(description);
}

Dictionary find_input_schema() {
	Dictionary schema = MCPSchema::object("One bounded semantic selector query over a UI capture.");
	MCPSchema::add_property(schema, "selector", selector_input_schema(), false);
	MCPSchema::add_property(schema, "limit",
			MCPSchema::ranged_integer(EditorSelectorLimits::MIN_LIMIT, EditorSelectorLimits::MAX_LIMIT,
					"Maximum matches returned in one page."),
			false);
	MCPSchema::add_property(schema, "cursor", MCPSchema::string("Opaque cursor returned by a previous page."), false);
	MCPSchema::add_property(schema, "require_unique",
			MCPSchema::boolean("Fail with ambiguous_selector when more than one element matches."), false);
	MCPSchema::add_property(schema, "max_depth",
			MCPSchema::integer("Maximum traversal depth; clamped to the documented range."), false);
	MCPSchema::add_property(schema, "max_elements",
			MCPSchema::integer("Maximum captured elements; clamped to the documented range."), false);
	MCPSchema::add_property(schema, "include_internal",
			MCPSchema::boolean("Include internal children, which are hidden by default."), false);
	return schema;
}

Dictionary find_output_schema() {
	Dictionary schema = MCPSchema::object("Result of one bounded selector query.");
	MCPSchema::add_property(schema, "ok", MCPSchema::boolean("True only when the status is 'ok'."), true);
	MCPSchema::add_property(schema, "status",
			MCPSchema::enum_string(EditorSelector::status_vocabulary(), "Selector status for this query."), true);
	MCPSchema::add_property(
			schema, "message", MCPSchema::string("Bounded diagnostic, empty when the query succeeded."), true);
	MCPSchema::add_property(schema, "generation",
			MCPSchema::integer("Capture this result was matched against, or 0 when nothing was captured."), true);
	MCPSchema::add_property(schema, "match_count", MCPSchema::integer("Total matches in the capture."), true);
	MCPSchema::add_property(schema, "returned_count", MCPSchema::integer("Matches carried by this page."), true);
	MCPSchema::add_property(schema, "offset", MCPSchema::integer("Offset of this page within the matches."), true);
	MCPSchema::add_property(schema, "limit", MCPSchema::integer("Page size applied to this query."), true);
	MCPSchema::add_property(schema, "truncated",
			MCPSchema::boolean("Whether matches or the underlying capture were cut by a published limit."), true);
	MCPSchema::add_property(schema, "next_cursor",
			MCPSchema::string("Cursor for the next page, or an empty string when this page is the last."), true);
	MCPSchema::add_definition(schema, UI_MATCH_DEFINITION, ui_element_schema(false));
	MCPSchema::add_property(schema, "matches",
			MCPSchema::array(MCPSchema::reference(UI_MATCH_DEFINITION), "Matched elements in document order."), true);
	return schema;
}

Dictionary ui_snapshot_output_schema() {
	Dictionary limits = MCPSchema::object("Limits applied to this capture.");
	MCPSchema::add_property(limits, "max_depth", MCPSchema::integer("Clamped maximum traversal depth."), true);
	MCPSchema::add_property(limits, "max_elements", MCPSchema::integer("Clamped maximum element budget."), true);
	MCPSchema::add_property(limits, "max_elements_applied",
			MCPSchema::integer("Element budget that produced this payload after size-driven truncation."), true);
	MCPSchema::add_property(
			limits, "include_internal", MCPSchema::boolean("Whether internal children were included."), true);
	MCPSchema::add_property(
			limits, "depth_truncated", MCPSchema::boolean("Whether any subtree was cut by max_depth."), true);
	MCPSchema::add_property(limits, "element_limit_reached",
			MCPSchema::boolean("Whether the element budget stopped the traversal."), true);
	MCPSchema::add_property(limits, "traversal_limit_reached",
			MCPSchema::boolean("Whether the absolute node-traversal depth stopped the traversal."), true);
	MCPSchema::add_property(
			limits, "payload_limit_bytes", MCPSchema::integer("Serialized payload budget in bytes."), true);

	Dictionary schema = MCPSchema::object("Bounded semantic snapshot of the stock editor UI.");
	MCPSchema::add_property(schema, "generation", MCPSchema::integer("Monotonic capture generation."), true);
	MCPSchema::add_property(schema, "focused_element_id",
			MCPSchema::string("Element id holding focus, or an empty string when none was captured."), true);
	MCPSchema::add_property(schema, "element_count", MCPSchema::integer("Number of captured elements."), true);
	MCPSchema::add_property(
			schema, "truncated", MCPSchema::boolean("Whether any limit truncated this snapshot."), true);
	MCPSchema::add_property(schema, "limits", limits, true);
	// One recursive definition describes every element at every depth, so the advertised contract and
	// the contract enforced against the tool's own output stay the same document.
	MCPSchema::add_definition(schema, UI_ELEMENT_DEFINITION, ui_element_schema());
	MCPSchema::add_property(schema, "tree",
			MCPSchema::array(MCPSchema::reference(UI_ELEMENT_DEFINITION),
					"Snapshot roots, starting at the editor base control."),
			true);
	return schema;
}

Dictionary ui_snapshot_input_schema() {
	Dictionary schema = MCPSchema::object("Bounded options for one UI capture.");
	MCPSchema::add_property(schema, "max_depth",
			MCPSchema::integer("Maximum traversal depth; clamped to the documented range."), false);
	MCPSchema::add_property(schema, "max_elements",
			MCPSchema::integer("Maximum captured elements; clamped to the documented range."), false);
	MCPSchema::add_property(schema, "include_internal",
			MCPSchema::boolean("Include internal children, which are hidden by default."), false);
	return schema;
}

Dictionary string_list_schema(const String &p_description) {
	Dictionary schema = MCPSchema::object(p_description);
	MCPSchema::add_property(schema, "count", MCPSchema::integer("Total number of entries before bounding."), true);
	MCPSchema::add_property(schema, "items", MCPSchema::array(MCPSchema::string(), "Bounded published entries."), true);
	MCPSchema::add_property(
			schema, "truncated", MCPSchema::boolean("Whether entries were omitted to stay bounded."), true);
	return schema;
}

Dictionary editor_state_output_schema() {
	Dictionary scenes = MCPSchema::object("Open, unsaved, and edited scene paths.");
	MCPSchema::add_property(scenes, "open", string_list_schema("Scene files open in the editor."), true);
	MCPSchema::add_property(scenes, "unsaved", string_list_schema("Open scene files with unsaved changes."), true);
	MCPSchema::add_property(
			scenes, "current", MCPSchema::string("Scene file path of the edited scene root, or empty."), true);

	Dictionary node = MCPSchema::object("One selected scene node.");
	MCPSchema::add_property(node, "name", MCPSchema::string("Bounded node name."), true);
	MCPSchema::add_property(node, "class", MCPSchema::string("Public Godot class name."), true);
	MCPSchema::add_property(node, "path", MCPSchema::string("Bounded scene path, for diagnostics only."), true);
	Dictionary selection = MCPSchema::object("Current editor scene selection.");
	MCPSchema::add_property(selection, "count", MCPSchema::integer("Total number of selected nodes."), true);
	MCPSchema::add_property(selection, "nodes", MCPSchema::array(node, "Bounded published selection."), true);
	MCPSchema::add_property(selection, "truncated", MCPSchema::boolean("Whether selected nodes were omitted."), true);

	Dictionary script = MCPSchema::object("Script editor state.");
	MCPSchema::add_property(
			script, "current", MCPSchema::string("Resource path of the current script, or empty."), true);
	MCPSchema::add_property(script, "open", string_list_schema("Resource paths of open scripts."), true);
	MCPSchema::add_property(script, "unsaved", string_list_schema("Open script files with unsaved changes."), true);

	Dictionary filesystem = MCPSchema::object("Editor resource filesystem state.");
	MCPSchema::add_property(filesystem, "scanning", MCPSchema::boolean("Whether the filesystem is scanning."), true);
	MCPSchema::add_property(filesystem, "importing", MCPSchema::boolean("Whether the filesystem is importing."), true);
	MCPSchema::add_property(filesystem, "scan_progress", MCPSchema::number("Scan progress between 0.0 and 1.0."), true);

	Dictionary play = MCPSchema::object("Editor play state.");
	MCPSchema::add_property(play, "is_playing", MCPSchema::boolean("Whether the editor is playing a scene."), true);
	MCPSchema::add_property(play, "playing_scene", MCPSchema::string("Scene file path being played, or empty."), true);

	Dictionary schema = MCPSchema::object("Stable public editor and scene state.");
	MCPSchema::add_property(schema, "project", project_output_schema(), true);
	MCPSchema::add_property(schema, "scenes", scenes, true);
	MCPSchema::add_property(schema, "selection", selection, true);
	MCPSchema::add_property(schema, "script", script, true);
	MCPSchema::add_property(schema, "filesystem", filesystem, true);
	MCPSchema::add_property(schema, "play", play, true);
	return schema;
}

Dictionary act_input_schema() {
	Dictionary schema = MCPSchema::object(
			"One action against the single editor element a selector names. The selector must name "
			"exactly one element: zero matches, several matches, and a truncated capture all refuse to "
			"act. Every action-specific field belongs to exactly one action, and a field the requested "
			"action does not use is rejected rather than ignored.");
	MCPSchema::add_property(schema, "selector", selector_input_schema(), true);
	MCPSchema::add_property(schema, "action",
			MCPSchema::enum_string(EditorSnapshot::action_vocabulary(),
					"Action to perform. It must appear in the target element's advertised 'actions'. Each "
					"action declares a claim in the tool's '_meta' action_claims entries and repeats it in "
					"the result."),
			true);
	MCPSchema::add_property(schema, "text",
			MCPSchema::string("Text for set_text and type_text. set_text accepts at most " +
					String::num_int64(EditorActionLimits::MAX_TEXT_LENGTH) + " characters and type_text at most " +
					String::num_int64(EditorActionLimits::MAX_TYPED_LENGTH) +
					", because typing pushes one public "
					"input event per character. Text longer than the target field's own published "
					"'max_length' is rejected rather than truncated."),
			false);
	MCPSchema::add_property(schema, "value",
			MCPSchema::number("Numeric value for set_value. A value the element cannot hold exactly, whether "
							  "outside its published range or off its published step, is rejected rather than "
							  "clamped or snapped."),
			false);
	MCPSchema::add_property(schema, "checked", MCPSchema::boolean("Checked state for set_checked."), false);
	MCPSchema::add_property(schema, "index",
			MCPSchema::ranged_integer(EditorActionLimits::MIN_INDEX, EditorActionLimits::MAX_INDEX,
					"Zero-based item or tab index for select_item and select_tab."),
			false);
	MCPSchema::add_property(schema, "scroll_axis",
			MCPSchema::enum_string(EditorActionDriver::scroll_axis_vocabulary(), "Axis for scroll."), false);
	MCPSchema::add_property(schema, "scroll_offset",
			MCPSchema::ranged_integer(EditorActionLimits::MIN_SCROLL_OFFSET, EditorActionLimits::MAX_SCROLL_OFFSET,
					"Scroll offset in pixels for scroll. An offset the element cannot reach is rejected and "
					"leaves the element where it was, rather than reported as a scroll to a clamped offset."),
			false);
	MCPSchema::add_property(schema, "max_depth",
			MCPSchema::integer("Maximum traversal depth of the capture the target is resolved against; clamped "
							   "to the documented range."),
			false);
	MCPSchema::add_property(schema, "max_elements",
			MCPSchema::integer("Maximum captured elements; clamped to the documented range."), false);
	MCPSchema::add_property(schema, "include_internal",
			MCPSchema::boolean("Include internal children, which are hidden by default."), false);
	return schema;
}

Dictionary act_output_schema() {
	Dictionary schema = MCPSchema::object("Result of one editor UI action.");
	MCPSchema::add_property(schema, "ok", MCPSchema::boolean("True only when the status is 'ok'."), true);
	MCPSchema::add_property(schema, "status",
			MCPSchema::enum_string(EditorActionDriver::status_vocabulary(), "Action status for this request."), true);
	MCPSchema::add_property(
			schema, "message", MCPSchema::string("Bounded diagnostic, empty when the action succeeded."), true);
	MCPSchema::add_property(schema, "action",
			MCPSchema::enum_string(EditorSnapshot::action_vocabulary(),
					"The requested action. Absent whenever the request was refused before it was parsed, "
					"which covers both a request that named no advertised action and one refused by the "
					"session gate or the per-request mutation budget before any action was read."),
			false);
	MCPSchema::add_property(schema, "route",
			MCPSchema::enum_string(EditorActionDriver::route_vocabulary(),
					"Public route the action used: a direct control method, a pushed input event, or none "
					"when nothing was performed."),
			true);
	MCPSchema::add_property(schema, "claim",
			MCPSchema::enum_string(MCPContracts::claim_vocabulary(),
					"What 'ok' asserts for the requested action. 'delivery' asserts only that the input "
					"reached the exact requested target, never that the editor did anything in response, so "
					"the client must observe the editor itself; 'effect' asserts the requested state holds "
					"and was verified. Absent exactly where 'action' is absent, because a claim is read from "
					"the named action and is never guessed for a request that was never parsed."),
			false);
	MCPSchema::add_property(schema, "changed",
			MCPSchema::boolean("Whether the acted element's own published fields differ between the capture the "
							   "action was decided on and the capture taken after it."),
			true);
	MCPSchema::add_property(schema, "generation",
			MCPSchema::integer("Capture taken after the action, or the capture the request was refused against, "
							   "or 0 when nothing was captured."),
			true);
	MCPSchema::add_property(schema, "handle",
			MCPSchema::string("Handle of the element acted on, or an empty string when none was resolved."), true);
	MCPSchema::add_property(schema, "trace",
			MCPSchema::array(MCPSchema::string(),
					"Bounded record of how far the request got, published only on a refusal. It is a "
					"diagnostic, never a second contract: an entry is a short human-readable step, and both "
					"the number of entries and their length are capped."),
			false);
	MCPSchema::add_definition(schema, UI_MATCH_DEFINITION, ui_element_schema(false));
	MCPSchema::add_property(schema, "element", MCPSchema::reference(UI_MATCH_DEFINITION), false);
	return schema;
}

Dictionary wait_condition_schema() {
	Dictionary schema = MCPSchema::object(
			"One wait condition. Every field belongs to exactly one condition type, and a field the named "
			"type does not use is rejected rather than ignored.");
	MCPSchema::add_property(schema, "type",
			MCPSchema::enum_string(EditorWaitManager::condition_vocabulary(),
					"What this wait observes. 'frames_elapsed' counts editor process frames; "
					"'selector_appears' and 'selector_disappears' watch a semantic selector, and absence is "
					"only ever reported from a capture and a walk that were not cut short; 'selector_state' "
					"waits until the single element 'selector' names also satisfies 'state'; "
					"'play_state' waits for the requested play state; 'filesystem_settles' waits until the "
					"editor resource filesystem is neither scanning nor importing."),
			true);
	MCPSchema::add_property(schema, "frames",
			MCPSchema::ranged_integer(EditorWaitLimits::MIN_FRAMES, EditorWaitLimits::MAX_FRAMES,
					"Editor process frames frames_elapsed waits for."),
			false);
	MCPSchema::add_property(schema, "selector", selector_input_schema(), false);
	MCPSchema::add_property(schema, "state",
			MCPSchema::open_object("Extra constraints the element named by 'selector' must satisfy, for "
								   "selector_state. It takes the same fields as a selector except 'handle': "
								   "'selector' names the element and 'state' names what must become true of "
								   "it, so a handle here is rejected rather than overwritten."),
			false);
	MCPSchema::add_property(schema, "playing", MCPSchema::boolean("Play state play_state waits for."), false);
	MCPSchema::add_property(schema, "require_start",
			MCPSchema::boolean("For filesystem_settles: wait until the editor is first observed busy and only "
							   "then wait for it to become idle. A scan raises its own busy flag "
							   "asynchronously, so a drain-only wait against a scan that has been requested "
							   "but has not started yet would report a settle that never happened. When the "
							   "prime window passes without the editor ever being observed busy the wait ends "
							   "as 'not_started', never as 'complete'. Leave it false for an operation that "
							   "completes synchronously and only might trigger a tail scan, because priming "
							   "one of those would always report 'not_started'."),
			false);
	MCPSchema::add_property(schema, "prime_ms",
			MCPSchema::ranged_integer(EditorWaitLimits::MIN_PRIME_MS, EditorWaitLimits::MAX_PRIME_MS,
					"How long a primed filesystem_settles wait will watch for the editor to become busy "
					"before it reports 'not_started'. It applies only when 'require_start' is true and must "
					"not exceed 'timeout_ms'."),
			false);
	return schema;
}

Dictionary wait_input_schema() {
	Dictionary schema = MCPSchema::object(
			"One cooperative wait request. Exactly one of 'condition', which starts a wait, and 'wait_id', "
			"which polls or cancels one, must be present. Nothing here blocks the editor: a wait advances "
			"only on later editor frames, so a pending wait is polled rather than slept on.");
	MCPSchema::add_property(schema, "condition", wait_condition_schema(), false);
	MCPSchema::add_property(schema, "wait_id",
			MCPSchema::string("Opaque wait id this server issued. It is unique to this server session and is "
							  "never reissued."),
			false);
	MCPSchema::add_property(schema, "cancel",
			MCPSchema::boolean("Cancel the named wait instead of polling it. Cancelling is idempotent inside "
							   "the published retention window."),
			false);
	MCPSchema::add_property(schema, "timeout_ms",
			MCPSchema::ranged_integer(EditorWaitLimits::MIN_TIMEOUT_MS, EditorWaitLimits::MAX_TIMEOUT_MS,
					"Deadline for a wait this request starts. It belongs to a start request only; a wait keeps "
					"the deadline it was created with."),
			false);
	return schema;
}

Dictionary wait_output_schema() {
	Dictionary schema = MCPSchema::object("Result of one cooperative wait request.");
	MCPSchema::add_property(schema, "ok",
			MCPSchema::boolean("True only when the status is 'complete'. It asserts exactly one thing: the "
							   "waited-for condition was observed to hold."),
			true);
	MCPSchema::add_property(schema, "status",
			MCPSchema::enum_string(EditorWaitManager::status_vocabulary(),
					"Wait status. 'pending' means poll again; 'complete' means the condition was observed; "
					"'not_started' means a primed settle never observed the editor become busy, so settling "
					"was never confirmed; 'wait_timeout' and 'wait_cancelled' end the wait; 'wait_not_found' "
					"answers an id this session does not hold."),
			true);
	MCPSchema::add_property(
			schema, "message", MCPSchema::string("Bounded diagnostic, empty when nothing was refused."), true);
	MCPSchema::add_property(schema, "wait_id",
			MCPSchema::string("Opaque handle to poll or cancel, or an empty string when no handle exists: a "
							  "condition already satisfied completes without creating one, and a refused "
							  "request never creates one."),
			true);
	MCPSchema::add_property(schema, "condition",
			MCPSchema::enum_string(EditorWaitManager::reported_condition_vocabulary(),
					"The condition this result is about, or 'none' when the request was refused before any "
					"condition could be read."),
			true);
	MCPSchema::add_property(schema, "elapsed_ms", MCPSchema::integer("Milliseconds since the wait was created."), true);
	MCPSchema::add_property(schema, "remaining_ms",
			MCPSchema::integer("Milliseconds left before the deadline, 0 once terminal."), true);
	MCPSchema::add_property(schema, "timeout_ms", MCPSchema::integer("Deadline the wait was created with."), true);
	MCPSchema::add_property(schema, "frames_observed",
			MCPSchema::integer("Editor process frames this wait has been evaluated on."), true);
	MCPSchema::add_property(
			schema, "active_waits", MCPSchema::integer("Wait handles this session currently holds."), true);
	MCPSchema::add_property(schema, "detail",
			MCPSchema::open_object("Bounded observation from the last evaluation, such as the match count or "
								   "whether the editor was observed busy."),
			true);
	MCPSchema::add_property(schema, "limits",
			MCPSchema::open_object("Published wait limits: handle count, deadline range, prime range, frame "
								   "range, evaluation interval, and how long a terminal outcome stays readable."),
			true);
	return schema;
}

constexpr const char *BARISTA_EVENT_DEFINITION = "barista_event";

Dictionary barista_event_schema() {
	Dictionary schema = MCPSchema::object("One Barista-owned event.");
	MCPSchema::add_property(schema, "index",
			MCPSchema::integer("Monotonic index for this server session. The next marker is this index plus one."),
			true);
	MCPSchema::add_property(
			schema, "time_ms", MCPSchema::integer("Engine millisecond tick when this was recorded."), true);
	MCPSchema::add_property(schema, "type",
			MCPSchema::enum_string(EditorEventLog::type_vocabulary(), "What kind of Barista operation this was."),
			true);
	MCPSchema::add_property(
			schema, "operation", MCPSchema::string("Bounded name of the operation, empty when it has none."), true);
	MCPSchema::add_property(schema, "outcome",
			MCPSchema::string("Bounded outcome, empty on a begin event because the outcome is not known yet."), true);
	MCPSchema::add_property(schema, "detail", MCPSchema::open_object("Bounded detail for this event."), true);
	return schema;
}

Dictionary events_input_schema() {
	Dictionary schema = MCPSchema::object("One bounded page of the Barista-owned event ring.");
	MCPSchema::add_property(schema, "marker",
			MCPSchema::ranged_integer(EditorEventLimits::MIN_MARKER, EditorEventLimits::MAX_MARKER,
					"Resume from this marker, which is the index of the first event to return. Absent starts "
					"at the earliest event still stored. Only a marker this server issued names a page."),
			false);
	MCPSchema::add_property(schema, "limit",
			MCPSchema::ranged_integer(
					EditorEventLimits::MIN_LIMIT, EditorEventLimits::MAX_LIMIT, "Maximum events returned in one page."),
			false);
	return schema;
}

Dictionary events_output_schema() {
	Dictionary schema = MCPSchema::object(
			"One bounded page of Barista-owned events. Indices are monotonic for the life of one server "
			"session, and a page never repeats an event for a valid marker.");
	MCPSchema::add_property(schema, "ok", MCPSchema::boolean("True only when the status is 'ok'."), true);
	MCPSchema::add_property(schema, "status",
			MCPSchema::enum_string(EditorEventLog::status_vocabulary(),
					"'marker_expired' means the bounded ring dropped the events that marker named, and "
					"'marker' then carries the earliest marker still available; 'invalid_marker' means the "
					"marker was never issued by this session."),
			true);
	MCPSchema::add_property(
			schema, "message", MCPSchema::string("Bounded diagnostic, empty when the page was served."), true);
	MCPSchema::add_property(schema, "marker",
			MCPSchema::integer("Marker to pass next. After a served page it names the event after the last one "
							   "returned; after an expiry it names the earliest event still stored."),
			true);
	MCPSchema::add_property(
			schema, "earliest_marker", MCPSchema::integer("Earliest marker the ring can still serve."), true);
	MCPSchema::add_property(
			schema, "latest_marker", MCPSchema::integer("Marker naming the next event this session will issue."), true);
	MCPSchema::add_property(schema, "count", MCPSchema::integer("Events carried by this page."), true);
	MCPSchema::add_property(
			schema, "has_more", MCPSchema::boolean("Whether events remain after this page's marker."), true);
	MCPSchema::add_property(schema, "dropped",
			MCPSchema::integer("Events this bounded ring evicted since the session started, so an empty page "
							   "can be told apart from a page that was lost."),
			true);
	MCPSchema::add_property(schema, "limit", MCPSchema::integer("Page size applied to this request."), true);
	MCPSchema::add_property(schema, "limits",
			MCPSchema::open_object("Published event limits: ring capacity and page-size range."), true);
	MCPSchema::add_definition(schema, BARISTA_EVENT_DEFINITION, barista_event_schema());
	MCPSchema::add_property(schema, "events",
			MCPSchema::array(MCPSchema::reference(BARISTA_EVENT_DEFINITION), "Events in index order."), true);
	return schema;
}

Dictionary make_tool(const String &p_name, const String &p_description, const Dictionary &p_output_schema,
		const Dictionary &p_input_schema = MCPSchema::object()) {
	Dictionary tool;
	tool["name"] = p_name;
	tool["description"] = p_description;
	tool["inputSchema"] = p_input_schema;
	tool["outputSchema"] = p_output_schema;
	return tool;
}

Dictionary make_resource(
		const String &p_uri, const String &p_name, const String &p_title, const String &p_description) {
	Dictionary resource;
	resource["uri"] = p_uri;
	resource["name"] = p_name;
	resource["title"] = p_title;
	resource["description"] = p_description;
	resource["mimeType"] = MCPContracts::JSON_MIME_TYPE;
	return resource;
}

} // namespace

PackedStringArray MCPContracts::claim_vocabulary() {
	PackedStringArray claims;
	claims.push_back(CLAIM_DELIVERY);
	claims.push_back(CLAIM_EFFECT);
	return claims;
}

String MCPContracts::action_claim(const String &p_action) {
	for (const ActionClaimRule &rule : ACTION_CLAIM_RULES) {
		if (p_action == rule.action) {
			return rule.claim;
		}
	}
	return String();
}

Array MCPContracts::build_action_claims() {
	// Driven by the advertised action vocabulary, never by the rule table, so an action added without
	// a declared claim publishes an entry with an empty claim rather than disappearing unnoticed.
	Array entries;
	const PackedStringArray actions = EditorSnapshot::action_vocabulary();
	for (int i = 0; i < actions.size(); i++) {
		Dictionary entry;
		entry["action"] = actions[i];
		entry["claim"] = action_claim(actions[i]);
		entries.push_back(entry);
	}
	return entries;
}

Array MCPContracts::build_tools_list(bool p_mutation_enabled) {
	Array tools;
	tools.push_back(make_tool("barista_status",
			"Report the local BaristaMCP server and protocol status without exposing its bearer token.",
			status_output_schema()));
	tools.push_back(make_tool("get_project_info",
			"Read stock Godot version, project, edited scene, and play-state information.", project_output_schema()));
	tools.push_back(make_tool("find_editor_ui",
			"Find editor UI elements by semantic selector, with bounded pages and durable handles.",
			find_output_schema(), find_input_schema()));
	tools.push_back(make_tool("read_editor_state",
			"Read stable public editor state: project, scenes, selection, script, filesystem, and play.",
			editor_state_output_schema()));
	tools.push_back(make_tool("inspect_editor_ui",
			"Capture a bounded semantic snapshot of the stock Godot editor UI through public APIs.",
			ui_snapshot_output_schema(), ui_snapshot_input_schema()));
	tools.push_back(make_tool("wait_for_editor",
			"Start, poll, or cancel one bounded cooperative wait on observable editor state. Nothing blocks "
			"the editor: a wait advances only on later editor frames, so a pending wait is polled rather "
			"than slept on. A settle wait can be primed, so an operation that never started reports "
			"'not_started' instead of a settle that never happened.",
			wait_output_schema(), wait_input_schema()));
	tools.push_back(make_tool("poll_barista_events",
			"Read a bounded page of Barista-owned events: lifecycle, actions, and waits Barista itself "
			"performed or observed through public APIs. It never claims to mirror the internal Godot editor "
			"log.",
			events_output_schema(), events_input_schema()));
	if (p_mutation_enabled) {
		Dictionary act = make_tool(ACT_TOOL_NAME,
				"Act on the single editor UI element a selector names, through documented public Godot APIs. "
				"Every action declares a claim: on a 'delivery' route (click, submit, type_text) 'ok' means "
				"the input was delivered to the exact requested target, not that the editor did anything in "
				"response, so the client must observe the editor itself, while on an 'effect' route 'ok' "
				"means the requested state was verified to hold. "
				"Mutating: enabled only when this session opted in before startup.",
				act_output_schema(), act_input_schema());
		Dictionary meta;
		meta["action_claims"] = build_action_claims();
		act["_meta"] = meta;
		tools.push_back(act);
	}
	return tools;
}

bool MCPContracts::find_tool(const String &p_name, Dictionary &r_tool, bool p_mutation_enabled) {
	const Array tools = build_tools_list(p_mutation_enabled);
	for (int i = 0; i < tools.size(); i++) {
		const Dictionary tool = tools[i];
		if (String(tool.get("name", Variant())) == p_name) {
			r_tool = tool;
			return true;
		}
	}
	return false;
}

Array MCPContracts::build_resources_list() {
	Array resources;
	resources.push_back(make_resource(PROJECT_INFO_RESOURCE_URI, "project_info", "Project info",
			"Stock Godot version, project, edited scene, and play-state information."));
	resources.push_back(make_resource(UI_TREE_RESOURCE_URI, "ui_tree", "Editor UI tree",
			"Bounded semantic snapshot of the stock Godot editor UI, identical to inspect_editor_ui."));
	resources.push_back(make_resource(EDITOR_STATE_RESOURCE_URI, "editor_state", "Editor state",
			"Stable public editor state, identical to read_editor_state."));
	resources.push_back(make_resource(ACTIVE_SCENE_RESOURCE_URI, "active_scene", "Active scene",
			"Summary of the edited scene root, open scenes, and play state."));
	resources.push_back(make_resource(SCENE_TREE_RESOURCE_URI, "scene_tree", "Scene tree",
			"Bounded traversal of the edited scene, published with the limits that produced it."));
	return resources;
}

Array MCPContracts::build_resource_templates_list() {
	Array templates;
	Dictionary element = make_resource(UI_ELEMENT_TEMPLATE_URI, "ui_element", "Editor UI element",
			"One editor UI element named by a durable handle Barista issued.");
	element.erase("uri");
	element["uriTemplate"] = UI_ELEMENT_TEMPLATE_URI;
	templates.push_back(element);
	Dictionary subtree = make_resource(UI_SUBTREE_TEMPLATE_URI, "ui_subtree", "Editor UI subtree",
			"One editor UI element and its bounded subtree, named by a durable handle Barista issued.");
	subtree.erase("uri");
	subtree["uriTemplate"] = UI_SUBTREE_TEMPLATE_URI;
	templates.push_back(subtree);
	return templates;
}

bool MCPContracts::resolve_resource(const String &p_uri, Dictionary &r_resource, String &r_handle) {
	r_handle = String();
	if (find_resource(p_uri, r_resource)) {
		return true;
	}

	const Array templates = build_resource_templates_list();
	for (int i = 0; i < templates.size(); i++) {
		const Dictionary entry = templates[i];
		const String uri_template = entry.get("uriTemplate", Variant());
		const String prefix = uri_template.trim_suffix("{handle}");
		if (prefix == uri_template || !p_uri.begins_with(prefix)) {
			continue;
		}
		const String segment = p_uri.substr(prefix.length());
		if (segment.is_empty() || segment.length() > MAX_TEMPLATE_SEGMENT_LENGTH || segment.contains("/")) {
			return false;
		}
		const String handle = segment.uri_decode();
		// The decoded segment must be a handle and nothing else: no separators, no second escape, and
		// no characters outside the published handle grammar.
		const String prefix_text = EditorHandleLimits::HANDLE_PREFIX;
		if (!handle.begins_with(prefix_text) || handle.contains("/") || handle.contains("%")) {
			return false;
		}
		const String instance_text = handle.substr(prefix_text.length());
		if (instance_text.is_empty() || instance_text.length() > 20) {
			return false;
		}
		for (int character = 0; character < instance_text.length(); character++) {
			if (instance_text[character] < '0' || instance_text[character] > '9') {
				return false;
			}
		}
		Dictionary resource = entry;
		resource.erase("uriTemplate");
		resource["uri"] = p_uri;
		r_resource = resource;
		r_handle = handle;
		return true;
	}
	return false;
}

bool MCPContracts::find_resource(const String &p_uri, Dictionary &r_resource) {
	const Array resources = build_resources_list();
	for (int i = 0; i < resources.size(); i++) {
		const Dictionary resource = resources[i];
		if (String(resource.get("uri", Variant())) == p_uri) {
			r_resource = resource;
			return true;
		}
	}
	return false;
}

Dictionary MCPContracts::list_params_schema() {
	// The server never issues pagination cursors, so any client-supplied property is unknown.
	return MCPSchema::object("Listing requests take no parameters.");
}

Dictionary MCPContracts::resource_read_params_schema() {
	Dictionary schema = MCPSchema::object("Read one advertised BaristaMCP resource.");
	MCPSchema::add_property(schema, "uri", MCPSchema::string("URI of an advertised resource."), true);
	return schema;
}

} // namespace godot
