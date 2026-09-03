/**************************************************************************/
/*  mcp_contracts.cpp                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "mcp_contracts.h"

#include "editor_automation_types.h"
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

Dictionary ui_element_schema() {
	Dictionary schema = MCPSchema::object("One semantic editor control or window.");
	MCPSchema::add_property(schema, "id", MCPSchema::string("Snapshot-scoped element id."), true);
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
			MCPSchema::array(MCPSchema::open_object(), "Child elements, recursively using this element shape."), true);
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
	MCPSchema::add_property(schema, "tree",
			MCPSchema::array(ui_element_schema(), "Snapshot roots, starting at the editor base control."), true);
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

Array MCPContracts::build_tools_list() {
	Array tools;
	tools.push_back(make_tool("barista_status",
			"Report the local BaristaMCP server and protocol status without exposing its bearer token.",
			status_output_schema()));
	tools.push_back(make_tool("get_project_info",
			"Read stock Godot version, project, edited scene, and play-state information.", project_output_schema()));
	tools.push_back(make_tool("inspect_editor_ui",
			"Capture a bounded semantic snapshot of the stock Godot editor UI through public APIs.",
			ui_snapshot_output_schema(), ui_snapshot_input_schema()));
	return tools;
}

bool MCPContracts::find_tool(const String &p_name, Dictionary &r_tool) {
	const Array tools = build_tools_list();
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
	return resources;
}

Array MCPContracts::build_resource_templates_list() {
	return Array();
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
