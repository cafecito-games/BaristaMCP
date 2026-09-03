/**************************************************************************/
/*  editor_tool_provider.cpp                                              */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_tool_provider.h"

#include "editor_automation_service.h"
#include "mcp_contracts.h"
#include "mcp_schema.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

void EditorToolProvider::configure(EditorInterface *p_editor_interface, EditorAutomationService *p_automation_service,
		const String &p_endpoint, int p_port) {
	editor_interface = p_editor_interface;
	automation_service = p_automation_service;
	endpoint = p_endpoint;
	port = p_port;
}

Dictionary EditorToolProvider::_tool_result(const Dictionary &p_structured, bool p_is_error) {
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = JSON::stringify(p_structured, "", false);
	Array content;
	content.push_back(content_item);

	Dictionary result;
	result["content"] = content;
	result["structuredContent"] = p_structured;
	result["isError"] = p_is_error;
	return result;
}

Dictionary EditorToolProvider::status(bool p_initialized) const {
	Dictionary status;
	status["name"] = MCPContracts::SERVER_NAME;
	status["version"] = MCPContracts::SERVER_VERSION;
	status["protocol_version"] = MCPContracts::PROTOCOL_VERSION;
	status["initialized"] = p_initialized;
	status["local_only"] = true;
	status["endpoint"] = endpoint;
	status["port"] = port;
	return status;
}

Dictionary EditorToolProvider::project_info() const {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	Dictionary info;
	info["project_name"] = project_settings->get_setting("application/config/name", "");
	info["project_path"] = project_settings->globalize_path("res://").trim_suffix("/");
	info["godot_version"] = Engine::get_singleton()->get_version_info();

	String current_scene;
	bool is_playing = false;
	if (editor_interface != nullptr) {
		Node *root = editor_interface->get_edited_scene_root();
		if (root != nullptr) {
			current_scene = root->get_scene_file_path();
		}
		is_playing = editor_interface->is_playing_scene();
	}
	info["current_scene"] = current_scene;
	info["is_playing"] = is_playing;
	return info;
}

Dictionary EditorToolProvider::_tool_error(const String &p_error, const String &p_message) {
	Dictionary error;
	error["error"] = p_error;
	error["message"] = p_message;
	return _tool_result(error, true);
}

bool EditorToolProvider::read_resource(const String &p_uri, Dictionary &r_payload) const {
	if (p_uri != MCPContracts::PROJECT_INFO_RESOURCE_URI) {
		return false;
	}
	r_payload = project_info();
	return true;
}

Dictionary EditorToolProvider::call(const String &p_name, const Dictionary &p_arguments, bool p_initialized) const {
	Dictionary tool;
	if (!MCPContracts::find_tool(p_name, tool)) {
		return _tool_error("unknown_tool", "Unknown tool '" + p_name + "'.");
	}

	String schema_error;
	if (!MCPSchema::validate(tool.get("inputSchema", Dictionary()), p_arguments, schema_error)) {
		return _tool_error("invalid_arguments", "Tool '" + p_name + "' rejected its arguments: " + schema_error);
	}

	Dictionary structured;
	if (p_name == "barista_status") {
		structured = status(p_initialized);
	} else if (p_name == "get_project_info") {
		structured = project_info();
	} else if (p_name == "inspect_editor_ui") {
		if (automation_service == nullptr) {
			return _tool_error("unsupported_capability", "Editor automation is unavailable in this session.");
		}
		String automation_error;
		String automation_message;
		structured = automation_service->inspect_ui(p_arguments, automation_error, automation_message);
		if (!automation_error.is_empty()) {
			return _tool_error(automation_error, automation_message);
		}
	} else {
		// An advertised tool without an implementation must fail closed rather than answer with
		// another tool's payload.
		return _tool_error("unsupported_capability", "Tool '" + p_name + "' has no implementation in this build.");
	}

	// The advertised outputSchema is a promise about structuredContent; never emit a payload that breaks it.
	if (!MCPSchema::validate(tool.get("outputSchema", Dictionary()), structured, schema_error)) {
		return _tool_error("output_contract_violation",
				"Tool '" + p_name + "' produced output outside its advertised schema: " + schema_error);
	}
	return _tool_result(structured, false);
}

} // namespace godot
