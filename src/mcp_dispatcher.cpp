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
	const bool notification = !p_message.has("id");
	Variant id;
	if (!notification) {
		id = p_message.get("id", Variant());
	}
	if (!notification && id.get_type() == Variant::FLOAT) {
		const double value = id;
		if (Math::is_finite(value) && value == Math::floor(value) && value >= -9223372036854775808.0 &&
				value < 9223372036854775808.0) {
			id = (int64_t)value;
		}
	}
	if (!notification && id.get_type() != Variant::NIL && id.get_type() != Variant::STRING &&
			id.get_type() != Variant::INT) {
		return _make_error(Variant(), INVALID_REQUEST, "Request id must be a string, integer, or null.");
	}

	if (!p_message.has("jsonrpc") || p_message.get("jsonrpc", Variant()).get_type() != Variant::STRING ||
			String(p_message.get("jsonrpc", Variant())) != "2.0") {
		return _make_error(id, INVALID_REQUEST, "Request must use JSON-RPC 2.0.");
	}
	if (!p_message.has("method") || p_message.get("method", Variant()).get_type() != Variant::STRING) {
		return _make_error(id, INVALID_REQUEST, "Request is missing a string 'method'.");
	}
	if (p_message.has("params") && p_message.get("params", Variant()).get_type() != Variant::DICTIONARY) {
		if (notification) {
			r_has_response = false;
			return Dictionary();
		}
		return _make_error(id, INVALID_PARAMS, "Request params must be an object when present.");
	}

	const String method = p_message.get("method", Variant());
	if (notification) {
		r_has_response = false;
		if (method == "notifications/initialized" && lifecycle_state == INITIALIZE_RESPONDED) {
			lifecycle_state = INITIALIZED;
		}
		return Dictionary();
	}

	if (method == "initialize") {
		if (lifecycle_state != UNINITIALIZED) {
			return _make_error(id, INVALID_REQUEST, "Server has already been initialized.");
		}
		if (!p_message.has("params") || p_message.get("params", Variant()).get_type() != Variant::DICTIONARY) {
			return _make_error(id, INVALID_PARAMS, "initialize requires object params.");
		}
		const Dictionary params = p_message.get("params", Variant());
		if (!params.has("protocolVersion") || params.get("protocolVersion", Variant()).get_type() != Variant::STRING ||
				String(params.get("protocolVersion", Variant())).is_empty()) {
			return _make_error(id, INVALID_PARAMS, "initialize requires a non-empty protocolVersion.");
		}
		if (!params.has("capabilities") || params.get("capabilities", Variant()).get_type() != Variant::DICTIONARY) {
			return _make_error(id, INVALID_PARAMS, "initialize requires object capabilities.");
		}
		if (!params.has("clientInfo") || params.get("clientInfo", Variant()).get_type() != Variant::DICTIONARY) {
			return _make_error(id, INVALID_PARAMS, "initialize requires object clientInfo.");
		}
		const Dictionary client_info = params.get("clientInfo", Variant());
		if (!client_info.has("name") || client_info.get("name", Variant()).get_type() != Variant::STRING ||
				String(client_info.get("name", Variant())).is_empty() || !client_info.has("version") ||
				client_info.get("version", Variant()).get_type() != Variant::STRING ||
				String(client_info.get("version", Variant())).is_empty()) {
			return _make_error(id, INVALID_PARAMS, "clientInfo requires non-empty name and version strings.");
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
		lifecycle_state = INITIALIZE_RESPONDED;
		return _make_result(id, result);
	}

	if (method == "ping") {
		return _make_result(id, Dictionary());
	}
	if ((method == "tools/list" || method == "tools/call") && lifecycle_state != INITIALIZED) {
		return _make_error(id, SERVER_NOT_INITIALIZED, "Server is not initialized.");
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
		return _make_result(
				id, tool_provider.call(params.get("name", Variant()), arguments, lifecycle_state == INITIALIZED));
	}

	return _make_error(id, METHOD_NOT_FOUND, "Unknown method '" + method + "'.");
}

void MCPDispatcher::configure_tools(EditorInterface *p_editor_interface, const String &p_endpoint, int p_port) {
	tool_provider.configure(p_editor_interface, p_endpoint, p_port);
}

bool MCPDispatcher::is_initialized() const {
	return lifecycle_state == INITIALIZED;
}

void MCPDispatcher::reset() {
	lifecycle_state = UNINITIALIZED;
}

} // namespace godot
