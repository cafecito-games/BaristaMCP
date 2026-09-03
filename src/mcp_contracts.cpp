/**************************************************************************/
/*  mcp_contracts.cpp                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "mcp_contracts.h"

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

Dictionary make_tool(const String &p_name, const String &p_description, const Dictionary &p_output_schema) {
	Dictionary tool;
	tool["name"] = p_name;
	tool["description"] = p_description;
	tool["inputSchema"] = MCPSchema::object();
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
