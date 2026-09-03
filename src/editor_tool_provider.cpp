/**************************************************************************/
/*  editor_tool_provider.cpp                                              */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_tool_provider.h"

#include "editor_action_driver.h"
#include "editor_automation_service.h"
#include "editor_state_reader.h"
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

bool EditorToolProvider::is_mutating_tool(const String &p_name) {
	return p_name == MCPContracts::ACT_TOOL_NAME;
}

void EditorToolProvider::configure(EditorInterface *p_editor_interface, EditorAutomationService *p_automation_service,
		const String &p_endpoint, int p_port, bool p_mutation_enabled) {
	editor_interface = p_editor_interface;
	automation_service = p_automation_service;
	endpoint = p_endpoint;
	port = p_port;
	mutation_enabled = p_mutation_enabled;
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
	status["automation_enabled"] = mutation_enabled;
	return status;
}

Dictionary EditorToolProvider::project_info() const {
	return EditorStateReader::project_info(editor_interface);
}

Dictionary EditorToolProvider::editor_state() const {
	return EditorStateReader::editor_state(editor_interface);
}

Dictionary EditorToolProvider::_act_result(const Dictionary &p_payload) {
	Dictionary tool;
	String schema_error;
	// Even a refusal is validated against the mutating tool's advertised output schema, so a session
	// that never advertises the tool still cannot answer outside its published shape.
	if (MCPContracts::find_tool(MCPContracts::ACT_TOOL_NAME, tool, true) &&
			!MCPSchema::validate(tool.get("outputSchema", Dictionary()), p_payload, schema_error)) {
		return _tool_error("output_contract_violation",
				"Tool '" + String(MCPContracts::ACT_TOOL_NAME) +
						"' produced output outside its advertised schema: " + schema_error);
	}
	return _tool_result(p_payload, true);
}

Dictionary EditorToolProvider::_tool_error(const String &p_error, const String &p_message) {
	Dictionary error;
	error["error"] = p_error;
	error["message"] = p_message;
	return _tool_result(error, true);
}

bool EditorToolProvider::read_resource(
		const String &p_uri, const String &p_handle, Dictionary &r_payload, String &r_error, String &r_message) const {
	r_error = String();
	r_message = String();
	if (p_uri == MCPContracts::PROJECT_INFO_RESOURCE_URI) {
		r_payload = project_info();
		return true;
	}
	if (p_uri == MCPContracts::EDITOR_STATE_RESOURCE_URI) {
		r_payload = editor_state();
		return true;
	}
	if (p_uri == MCPContracts::ACTIVE_SCENE_RESOURCE_URI) {
		r_payload = EditorStateReader::active_scene(editor_interface);
		return true;
	}
	if (p_uri == MCPContracts::SCENE_TREE_RESOURCE_URI) {
		r_payload = EditorStateReader::scene_tree(editor_interface);
		return true;
	}
	if (automation_service == nullptr) {
		return false;
	}
	if (p_uri == MCPContracts::UI_TREE_RESOURCE_URI) {
		r_payload = automation_service->inspect_ui(Dictionary(), r_error, r_message);
		return r_error.is_empty();
	}
	if (p_handle.is_empty()) {
		return false;
	}
	const bool include_children =
			p_uri.begins_with(String(MCPContracts::UI_SUBTREE_TEMPLATE_URI).trim_suffix("{handle}"));
	const bool is_element = p_uri.begins_with(String(MCPContracts::UI_ELEMENT_TEMPLATE_URI).trim_suffix("{handle}"));
	if (!include_children && !is_element) {
		return false;
	}
	r_payload = automation_service->read_element(p_handle, include_children, r_error, r_message);
	return r_error.is_empty();
}

Dictionary EditorToolProvider::call(
		const String &p_name, const Dictionary &p_arguments, bool p_initialized, bool p_mutation_allowed) const {
	// The mutating tool answers in its own advertised shape even where it is not advertised, so a
	// disabled session reports a stable status instead of looking like an unknown tool.
	if (is_mutating_tool(p_name)) {
		if (!mutation_enabled) {
			return _act_result(EditorActionDriver::failure(EditorActionDriver::Status::AUTOMATION_DISABLED,
					"Editor automation is disabled in this session; no action was performed."));
		}
		// Both gates above are read before the arguments are validated, so neither can name an action
		// without trusting an argument nothing has checked yet. Ordering integrity outranks field
		// uniformity here: 'action' and 'claim' are therefore absent from these two refusals, and the
		// result schema documents that absence rather than the fields carrying a guess.
		if (!p_mutation_allowed) {
			return _act_result(EditorActionDriver::failure(EditorActionDriver::Status::MUTATION_ALREADY_HANDLED,
					"This request already handled a mutating call; no second action was performed."));
		}
	}

	Dictionary tool;
	if (!MCPContracts::find_tool(p_name, tool, mutation_enabled)) {
		return _tool_error("unknown_tool", "Unknown tool '" + p_name + "'.");
	}

	String schema_error;
	if (!MCPSchema::validate(tool.get("inputSchema", Dictionary()), p_arguments, schema_error)) {
		if (is_mutating_tool(p_name)) {
			return _act_result(EditorActionDriver::failure(EditorActionDriver::Status::INVALID_ARGUMENTS,
					"act_on_editor_ui rejected its arguments: " + schema_error));
		}
		return _tool_error("invalid_arguments", "Tool '" + p_name + "' rejected its arguments: " + schema_error);
	}

	Dictionary structured;
	if (p_name == "barista_status") {
		structured = status(p_initialized);
	} else if (p_name == "get_project_info") {
		structured = project_info();
	} else if (p_name == "read_editor_state") {
		structured = editor_state();
	} else if (p_name == "find_editor_ui") {
		if (automation_service == nullptr) {
			return _tool_error("unsupported_capability", "Editor automation is unavailable in this session.");
		}
		String automation_error;
		String automation_message;
		structured = automation_service->find_ui(p_arguments, automation_error, automation_message);
		if (!automation_error.is_empty()) {
			return _tool_error(automation_error, automation_message);
		}
	} else if (is_mutating_tool(p_name)) {
		if (automation_service == nullptr) {
			return _act_result(EditorActionDriver::failure(EditorActionDriver::Status::UNSUPPORTED_CAPABILITY,
					"Editor automation is unavailable in this session."));
		}
		String automation_error;
		String automation_message;
		structured = automation_service->act_ui(p_arguments, automation_error, automation_message);
		if (!automation_error.is_empty()) {
			return _tool_error(automation_error, automation_message);
		}
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
	if (is_mutating_tool(p_name)) {
		return _tool_result(structured, !(bool)structured.get("ok", false));
	}
	return _tool_result(structured, false);
}

} // namespace godot
