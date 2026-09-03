/**************************************************************************/
/*  mcp_schema.h                                                          */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_SCHEMA_H
#define BARISTA_MCP_SCHEMA_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

// Builders for the JSON Schema subset BaristaMCP advertises, plus the validator that enforces the
// very same documents at the MCP boundary so advertised and enforced contracts cannot diverge.
class MCPSchema {
public:
	static Dictionary object(const String &p_description = String());
	static Dictionary open_object(const String &p_description = String());
	static Dictionary array(const Dictionary &p_items, const String &p_description = String());
	static Dictionary string(const String &p_description = String());
	static Dictionary integer(const String &p_description = String());
	static Dictionary number(const String &p_description = String());
	static Dictionary boolean(const String &p_description = String());
	static void add_property(
			Dictionary &r_schema, const String &p_name, const Dictionary &p_property, bool p_required = false);

	// Returns false and fills r_error when p_value does not satisfy p_schema. Unknown properties are
	// rejected wherever the schema sets additionalProperties to false; nothing is ever coerced.
	static bool validate(const Dictionary &p_schema, const Variant &p_value, String &r_error);
};

} // namespace godot

#endif // BARISTA_MCP_SCHEMA_H
