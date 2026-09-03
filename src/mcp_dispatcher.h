/**************************************************************************/
/*  mcp_dispatcher.h                                                      */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_DISPATCHER_H
#define BARISTA_MCP_DISPATCHER_H

#include "editor_tool_provider.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class EditorAutomationService;

class MCPDispatcher {
	enum LifecycleState {
		UNINITIALIZED,
		INITIALIZE_RESPONDED,
		INITIALIZED,
	};

	LifecycleState lifecycle_state = UNINITIALIZED;
	EditorToolProvider tool_provider;
	// Frozen at server startup: whether this session advertises the mutating tool at all.
	bool mutation_enabled = false;
	// Spent by the first mutating call inside one accepted HTTP request. A batch that repeats a
	// mutating call is refused rather than performed twice.
	bool mutation_handled = false;

	static Dictionary _make_error(
			const Variant &p_id, int p_code, const String &p_message, const Dictionary &p_data = Dictionary());
	static bool _validate_params(const Dictionary &p_message, const Dictionary &p_schema, String &r_error);
	static Dictionary _make_result(const Variant &p_id, const Variant &p_result);

public:
	enum ErrorCode {
		PARSE_ERROR = -32700,
		INVALID_REQUEST = -32600,
		METHOD_NOT_FOUND = -32601,
		INVALID_PARAMS = -32602,
		SERVER_NOT_INITIALIZED = -32002,
		RESOURCE_TOO_LARGE = -32003,
	};

	Dictionary handle_message(const Dictionary &p_message, bool &r_has_response);
	// Restores the one mutation an accepted HTTP request may perform. The transport calls it once per
	// request body, before any message in that body is handled.
	void begin_http_request();
	void configure_tools(EditorInterface *p_editor_interface, EditorAutomationService *p_automation_service,
			const String &p_endpoint, int p_port, bool p_mutation_enabled);
	bool is_initialized() const;
	void reset();
};

} // namespace godot

#endif // BARISTA_MCP_DISPATCHER_H
