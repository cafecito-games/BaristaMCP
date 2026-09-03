/**************************************************************************/
/*  mcp_contracts.h                                                       */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_CONTRACTS_H
#define BARISTA_MCP_CONTRACTS_H

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Single source of truth for the BaristaMCP tool and resource vocabulary. Advertised schemas and the
// schemas enforced at the request boundary are the same documents.
class MCPContracts {
public:
	static constexpr const char *PROTOCOL_VERSION = "2025-11-25";
	static constexpr const char *SERVER_NAME = "BaristaMCP";
	static constexpr const char *SERVER_VERSION = "0.1.0";
	static constexpr const char *PROJECT_INFO_RESOURCE_URI = "barista://project/info";
	static constexpr const char *JSON_MIME_TYPE = "application/json";
	// Serialized resource payloads larger than this are refused instead of overrunning the transport
	// response cap enforced by MCPServer.
	static constexpr int MAX_RESOURCE_PAYLOAD_BYTES = 512 * 1024;

	static Array build_tools_list();
	static bool find_tool(const String &p_name, Dictionary &r_tool);

	static Array build_resources_list();
	static Array build_resource_templates_list();
	static bool find_resource(const String &p_uri, Dictionary &r_resource);

	static Dictionary list_params_schema();
	static Dictionary resource_read_params_schema();
};

} // namespace godot

#endif // BARISTA_MCP_CONTRACTS_H
