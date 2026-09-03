#include "mcp_dispatcher.h"

#include "mcp_contracts.h"

#include <godot_cpp/core/math.hpp>

namespace godot {

Dictionary MCPDispatcher::_make_error(const Variant &p_id, int p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;

	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["error"] = error;
	return response;
}

Dictionary MCPDispatcher::_make_result(const Variant &p_id, const Variant &p_result) {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["result"] = p_result;
	return response;
}

Dictionary MCPDispatcher::handle_message(const Dictionary &p_message, bool &r_has_response) {
	r_has_response = true;
	Variant id = p_message.has("id") ? p_message.get("id", Variant()) : Variant();
	if (id.get_type() == Variant::FLOAT) {
		const double value = id;
		if (Math::is_finite(value) && value == Math::floor(value)) {
			id = (int64_t)value;
		}
	}
	const bool notification = !p_message.has("id");

	if (!p_message.has("jsonrpc") || p_message.get("jsonrpc", Variant()).get_type() != Variant::STRING ||
			String(p_message.get("jsonrpc", Variant())) != "2.0") {
		if (notification) {
			r_has_response = false;
			return Dictionary();
		}
		return _make_error(id, INVALID_REQUEST, "Request must use JSON-RPC 2.0.");
	}
	if (!p_message.has("method") || p_message.get("method", Variant()).get_type() != Variant::STRING) {
		if (notification) {
			r_has_response = false;
			return Dictionary();
		}
		return _make_error(id, INVALID_REQUEST, "Request is missing a string 'method'.");
	}

	const String method = p_message.get("method", Variant());
	if (notification) {
		r_has_response = false;
		if (method == "notifications/initialized") {
			initialized = true;
		}
		return Dictionary();
	}

	if (method == "initialize") {
		if (!p_message.has("params") || p_message.get("params", Variant()).get_type() != Variant::DICTIONARY) {
			return _make_error(id, INVALID_PARAMS, "initialize requires object params.");
		}
		const Dictionary params = p_message.get("params", Variant());
		if (!params.has("protocolVersion") || params.get("protocolVersion", Variant()).get_type() != Variant::STRING ||
				String(params.get("protocolVersion", Variant())).is_empty()) {
			return _make_error(id, INVALID_PARAMS, "initialize requires a non-empty protocolVersion.");
		}

		Dictionary tools;
		tools["listChanged"] = false;
		Dictionary capabilities;
		capabilities["tools"] = tools;
		Dictionary server_info;
		server_info["name"] = MCPContracts::SERVER_NAME;
		server_info["version"] = MCPContracts::SERVER_VERSION;
		Dictionary result;
		result["protocolVersion"] = MCPContracts::PROTOCOL_VERSION;
		result["capabilities"] = capabilities;
		result["serverInfo"] = server_info;
		result["instructions"] =
				"Local, read-only Godot editor integration. Use tools/list to inspect available tools.";
		return _make_result(id, result);
	}

	if (method == "ping") {
		return _make_result(id, Dictionary());
	}
	if (method == "tools/list") {
		Dictionary result;
		result["tools"] = MCPContracts::build_tools_list();
		return _make_result(id, result);
	}
	if (method == "tools/call") {
		if (!p_message.has("params") || p_message.get("params", Variant()).get_type() != Variant::DICTIONARY) {
			return _make_error(id, INVALID_PARAMS, "tools/call requires object params.");
		}
		const Dictionary params = p_message.get("params", Variant());
		if (!params.has("name") || params.get("name", Variant()).get_type() != Variant::STRING ||
				String(params.get("name", Variant())).is_empty()) {
			return _make_error(id, INVALID_PARAMS, "tools/call requires a non-empty string 'name'.");
		}
		Dictionary arguments;
		if (params.has("arguments")) {
			if (params.get("arguments", Variant()).get_type() != Variant::DICTIONARY) {
				return _make_error(id, INVALID_PARAMS, "tools/call 'arguments' must be an object.");
			}
			arguments = params.get("arguments", Variant());
		}
		return _make_result(id, tool_provider.call(params.get("name", Variant()), arguments, initialized));
	}

	return _make_error(id, METHOD_NOT_FOUND, "Unknown method '" + method + "'.");
}

void MCPDispatcher::configure_tools(EditorInterface *p_editor_interface, const String &p_endpoint, int p_port) {
	tool_provider.configure(p_editor_interface, p_endpoint, p_port);
}

bool MCPDispatcher::is_initialized() const {
	return initialized;
}

void MCPDispatcher::reset() {
	initialized = false;
}

} // namespace godot
