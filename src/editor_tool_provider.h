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

	static Dictionary _tool_result(const Dictionary &p_structured, bool p_is_error);
	static Dictionary _tool_error(const String &p_error, const String &p_message);

public:
	void configure(EditorInterface *p_editor_interface, EditorAutomationService *p_automation_service,
			const String &p_endpoint, int p_port);
	Dictionary call(const String &p_name, const Dictionary &p_arguments, bool p_initialized) const;

	Dictionary status(bool p_initialized) const;
	Dictionary project_info() const;
	// Fills r_payload with the structured content of an advertised resource. Returns false for any URI
	// the provider does not serve, so unknown URIs can never be mapped onto another resource.
	bool read_resource(const String &p_uri, Dictionary &r_payload) const;
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_TOOL_PROVIDER_H
