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
#include "editor_selector.h"
#include "editor_snapshot.h"
#include "mcp_schema.h"

#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

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
					"Action to perform. It must appear in the target element's advertised 'actions'."),
			true);
	MCPSchema::add_property(schema, "text",
			MCPSchema::string("Text for set_text and type_text. set_text accepts at most " +
					String::num_int64(EditorActionLimits::MAX_TEXT_LENGTH) + " characters and type_text at most " +
					String::num_int64(EditorActionLimits::MAX_TYPED_LENGTH) +
					", because typing pushes one public "
					"input event per character."),
			false);
	MCPSchema::add_property(schema, "value",
			MCPSchema::number("Numeric value for set_value. A value outside the element's published range is "
							  "rejected rather than clamped."),
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
					"Scroll offset in pixels for scroll."),
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
					"The requested action, absent when the request never named an advertised one."),
			false);
	MCPSchema::add_property(schema, "route",
			MCPSchema::enum_string(EditorActionDriver::route_vocabulary(),
					"Public route the action used: a direct control method, a pushed input event, or none "
					"when nothing was performed."),
			true);
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
	MCPSchema::add_definition(schema, UI_MATCH_DEFINITION, ui_element_schema(false));
	MCPSchema::add_property(schema, "element", MCPSchema::reference(UI_MATCH_DEFINITION), false);
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
	if (p_mutation_enabled) {
		tools.push_back(make_tool(ACT_TOOL_NAME,
				"Act on the single editor UI element a selector names, through documented public Godot APIs. "
				"Mutating: enabled only when this session opted in before startup.",
				act_output_schema(), act_input_schema()));
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
