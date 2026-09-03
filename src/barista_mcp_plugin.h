/**************************************************************************/
/*  barista_mcp_plugin.h                                                  */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_PLUGIN_H
#define BARISTA_MCP_PLUGIN_H

#include <godot_cpp/classes/editor_plugin.hpp>

#include "mcp_server.h"

namespace godot {

class BaristaMCPPlugin : public EditorPlugin {
	GDCLASS(BaristaMCPPlugin, EditorPlugin)

	MCPServer server;

	void _define_project_settings();
	void _start_server();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;
};

} // namespace godot

#endif // BARISTA_MCP_PLUGIN_H
