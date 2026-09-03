#ifndef BARISTA_MCP_DISPATCHER_H
#define BARISTA_MCP_DISPATCHER_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class MCPDispatcher {
	bool initialized = false;

	static Dictionary _make_error(const Variant &p_id, int p_code, const String &p_message);
	static Dictionary _make_result(const Variant &p_id, const Variant &p_result);

public:
	enum ErrorCode {
		PARSE_ERROR = -32700,
		INVALID_REQUEST = -32600,
		METHOD_NOT_FOUND = -32601,
		INVALID_PARAMS = -32602,
	};

	Dictionary handle_message(const Dictionary &p_message, bool &r_has_response);
	bool is_initialized() const;
	void reset();
};

} // namespace godot

#endif // BARISTA_MCP_DISPATCHER_H
