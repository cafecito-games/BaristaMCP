/**************************************************************************/
/*  mcp_contracts.cpp                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "mcp_contracts.h"

#include <godot_cpp/variant/variant.hpp>

namespace godot {

Dictionary MCPContracts::empty_object_schema() {
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = Dictionary();
	schema["required"] = Array();
	schema["additionalProperties"] = false;
	return schema;
}

Array MCPContracts::build_tools_list() {
	Dictionary status;
	status["name"] = "barista_status";
	status["description"] = "Report the local BaristaMCP server and protocol status without exposing its bearer token.";
	status["inputSchema"] = empty_object_schema();

	Dictionary project;
	project["name"] = "get_project_info";
	project["description"] = "Read stock Godot version, project, edited scene, and play-state information.";
	project["inputSchema"] = empty_object_schema();

	Array tools;
	tools.push_back(status);
	tools.push_back(project);
	return tools;
}

} // namespace godot
