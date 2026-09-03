#ifndef BARISTA_MCP_CONTRACTS_H
#define BARISTA_MCP_CONTRACTS_H

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

class MCPContracts {
public:
	static constexpr const char *PROTOCOL_VERSION = "2025-11-25";
	static constexpr const char *SERVER_NAME = "BaristaMCP";
	static constexpr const char *SERVER_VERSION = "0.1.0";

	static Dictionary empty_object_schema();
	static Array build_tools_list();
};

} // namespace godot

#endif // BARISTA_MCP_CONTRACTS_H
