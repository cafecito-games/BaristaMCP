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

class EditorInterface;

class EditorToolProvider {
	EditorInterface *editor_interface = nullptr;
	String endpoint;
	int port = 0;

	static Dictionary _tool_result(const Dictionary &p_structured, bool p_is_error);
	Dictionary _status(bool p_initialized) const;
	Dictionary _project_info() const;

public:
	void configure(EditorInterface *p_editor_interface, const String &p_endpoint, int p_port);
	Dictionary call(const String &p_name, const Dictionary &p_arguments, bool p_initialized) const;
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_TOOL_PROVIDER_H
