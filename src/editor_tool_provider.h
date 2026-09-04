/**************************************************************************/
/*  editor_tool_provider.h                                                */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_TOOL_PROVIDER_H
#define BARISTA_MCP_EDITOR_TOOL_PROVIDER_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class EditorAutomationService;
class EditorInterface;

class EditorToolProvider {
	EditorInterface *editor_interface = nullptr;
	EditorAutomationService *automation_service = nullptr;
	String endpoint;
	int port = 0;
	// Frozen at server startup: whether this session advertises and performs the mutating tool.
	bool mutation_enabled = false;

	static Dictionary _tool_result(const Dictionary &p_structured, bool p_is_error);
	static Dictionary _tool_error(const String &p_error, const String &p_message);
	// Wraps one act_on_editor_ui refusal, validated against the tool's advertised output schema.
	static Dictionary _act_result(const Dictionary &p_payload);
	// Wraps one run_editor_action refusal, validated against that tool's advertised output schema.
	static Dictionary _operation_result(const Dictionary &p_payload);
	// Validates one mutating tool's refusal against its own advertised output schema, so a session
	// that never advertises the tool still cannot answer outside its published shape.
	static Dictionary _mutating_result(const String &p_tool, const Dictionary &p_payload);

public:
	// True for a tool that can mutate the editor. The transport uses it to spend the one mutation an
	// accepted HTTP request is allowed, so the answer must not depend on session state.
	static bool is_mutating_tool(const String &p_name);

	void configure(EditorInterface *p_editor_interface, EditorAutomationService *p_automation_service,
			const String &p_endpoint, int p_port, bool p_mutation_enabled);
	// p_mutation_allowed is false once this HTTP request has already handled a mutating call, so a
	// batched retry is refused instead of acting twice.
	Dictionary call(
			const String &p_name, const Dictionary &p_arguments, bool p_initialized, bool p_mutation_allowed) const;

	Dictionary status(bool p_initialized) const;
	Dictionary project_info() const;
	Dictionary editor_state() const;
	// Fills r_payload with the structured content of an advertised resource. p_handle is the validated
	// handle segment of a templated URI and is empty otherwise. Returns false for any URI the provider
	// does not serve and for any handle that no longer resolves, so an unknown URI can never be mapped
	// onto another resource and a stale handle can never answer with another element.
	bool read_resource(const String &p_uri, const String &p_handle, Dictionary &r_payload, String &r_error,
			String &r_message) const;
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_TOOL_PROVIDER_H
