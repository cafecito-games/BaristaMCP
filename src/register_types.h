/**************************************************************************/
/*  register_types.h                                                      */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_REGISTER_TYPES_H
#define BARISTA_MCP_REGISTER_TYPES_H

#include <godot_cpp/core/class_db.hpp>

void initialize_barista_mcp_module(godot::ModuleInitializationLevel p_level);
void uninitialize_barista_mcp_module(godot::ModuleInitializationLevel p_level);

#endif // BARISTA_MCP_REGISTER_TYPES_H
