#include "mcp_server.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

MCPServer::MCPServer() {
	listener.instantiate();
}

MCPServer::~MCPServer() {
	stop();
}

Error MCPServer::start(uint16_t p_port, uint64_t p_request_timeout_ms, int p_max_request_bytes) {
	if (listener.is_null()) {
		return ERR_UNCONFIGURED;
	}
	if (listener->is_listening()) {
		return ERR_ALREADY_IN_USE;
	}

	Ref<Crypto> crypto;
	crypto.instantiate();
	if (crypto.is_null()) {
		return ERR_CANT_CREATE;
	}
	token = Marshalls::get_singleton()->raw_to_base64(crypto->generate_random_bytes(32));
	if (token.is_empty()) {
		return ERR_CANT_CREATE;
	}

	const Error error = listener->listen(p_port, "127.0.0.1");
	if (error != OK) {
		token = String();
		return error;
	}

	port = listener->get_local_port();
	request_timeout_ms = p_request_timeout_ms;
	max_request_bytes = p_max_request_bytes;
	dispatcher.configure_tools(editor_interface, get_endpoint(), port);
	return OK;
}

void MCPServer::set_editor_interface(EditorInterface *p_editor_interface) {
	editor_interface = p_editor_interface;
}

void MCPServer::_accept_connection() {
	if (!listener->is_connection_available()) {
		return;
	}
	connection = listener->take_connection();
	request_buffer.clear();
	header_end = -1;
	content_length = -1;
	connection_started_ms = Time::get_singleton()->get_ticks_msec();
}

void MCPServer::_reset_connection() {
	if (connection.is_valid()) {
		connection->disconnect_from_host();
	}
	connection.unref();
	request_buffer.clear();
	header_end = -1;
	content_length = -1;
	connection_started_ms = 0;
}

bool MCPServer::_parse_headers(HTTPRequest &r_request) {
	const String header_text = request_buffer.slice(0, header_end).get_string_from_utf8();
	const PackedStringArray lines = header_text.split("\r\n");
	if (lines.is_empty()) {
		return false;
	}

	const PackedStringArray request_line = lines[0].split(" ", false);
	if (request_line.size() != 3 || !request_line[2].begins_with("HTTP/1.")) {
		return false;
	}
	r_request.method = request_line[0].to_upper();
	String target = request_line[1];
	const int query = target.find("?");
	r_request.path = query >= 0 ? target.substr(0, query) : target;

	for (int i = 1; i < lines.size(); i++) {
		const String line = lines[i];
		if (line.is_empty()) {
			continue;
		}
		const int colon = line.find(":");
		if (colon <= 0) {
			return false;
		}
		const String key = line.substr(0, colon).strip_edges().to_lower();
		const String value = line.substr(colon + 1).strip_edges();
		if (key.is_empty() || r_request.headers.has(key)) {
			return false;
		}
		r_request.headers[key] = value;
	}
	return true;
}

Dictionary MCPServer::_json_rpc_error(int p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = Variant();
	response["error"] = error;
	return response;
}

MCPServer::HTTPResponse MCPServer::_process_json(const String &p_body) {
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(p_body) != OK) {
		HTTPResponse response;
		response.body = JSON::stringify(_json_rpc_error(MCPDispatcher::PARSE_ERROR, "Parse error."), "", false);
		return response;
	}

	const Variant data = json->get_data();
	if (data.get_type() == Variant::ARRAY) {
		const Array batch = data;
		if (batch.is_empty()) {
			HTTPResponse response;
			response.body = JSON::stringify(
					_json_rpc_error(MCPDispatcher::INVALID_REQUEST, "A JSON-RPC batch must not be empty."), "", false);
			return response;
		}

		Array responses;
		for (int i = 0; i < batch.size(); i++) {
			if (batch[i].get_type() != Variant::DICTIONARY) {
				responses.push_back(_json_rpc_error(MCPDispatcher::INVALID_REQUEST, "Batch entries must be objects."));
				continue;
			}
			bool has_response = false;
			const Dictionary response = dispatcher.handle_message(batch[i], has_response);
			if (has_response) {
				responses.push_back(response);
			}
		}
		if (responses.is_empty()) {
			HTTPResponse response;
			response.status = 202;
			response.reason = "Accepted";
			return response;
		}
		HTTPResponse response;
		response.body = JSON::stringify(responses, "", false);
		return response;
	}
	if (data.get_type() != Variant::DICTIONARY) {
		HTTPResponse response;
		response.body = JSON::stringify(
				_json_rpc_error(MCPDispatcher::INVALID_REQUEST, "Invalid JSON-RPC request."), "", false);
		return response;
	}

	bool has_response = false;
	const Dictionary body = dispatcher.handle_message(data, has_response);
	if (!has_response) {
		HTTPResponse response;
		response.status = 202;
		response.reason = "Accepted";
		return response;
	}
	HTTPResponse response;
	response.body = JSON::stringify(body, "", false);
	return response;
}

bool MCPServer::_is_local_origin(const String &p_origin) {
	if (p_origin.is_empty() || p_origin == "null") {
		return false;
	}
	const int scheme_separator = p_origin.find("://");
	if (scheme_separator < 0) {
		return false;
	}
	const String scheme = p_origin.substr(0, scheme_separator).to_lower();
	if (scheme != "http" && scheme != "https") {
		return false;
	}

	String authority = p_origin.substr(scheme_separator + 3);
	const int slash = authority.find("/");
	if (slash >= 0) {
		authority = authority.substr(0, slash);
	}
	String host = authority;
	if (host.begins_with("[")) {
		const int close = host.find("]");
		if (close < 0) {
			return false;
		}
		host = host.substr(1, close - 1);
	} else {
		const int colon = host.find(":");
		if (colon >= 0) {
			host = host.substr(0, colon);
		}
	}
	host = host.to_lower();
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

MCPServer::HTTPResponse MCPServer::_process_request(const HTTPRequest &p_request) {
	if (p_request.path != "/mcp") {
		HTTPResponse response;
		response.status = 404;
		response.reason = "Not Found";
		response.body = "{\"error\":\"not_found\"}";
		return response;
	}
	if (p_request.method != "POST") {
		HTTPResponse response;
		response.status = 405;
		response.reason = "Method Not Allowed";
		response.body = "{\"error\":\"method_not_allowed\"}";
		return response;
	}
	if (p_request.headers.has("origin") && !_is_local_origin(p_request.headers.get("origin", String()))) {
		HTTPResponse response;
		response.status = 403;
		response.reason = "Forbidden";
		response.body = "{\"error\":\"forbidden_origin\"}";
		return response;
	}
	const String authorization = p_request.headers.get("authorization", String());
	if (token.is_empty() || authorization != "Bearer " + token) {
		HTTPResponse response;
		response.status = 401;
		response.reason = "Unauthorized";
		response.body = "{\"error\":\"unauthorized\"}";
		return response;
	}
	return _process_json(p_request.body);
}

void MCPServer::_send_response(const HTTPResponse &p_response) {
	if (connection.is_null()) {
		return;
	}
	const PackedByteArray body = p_response.body.to_utf8_buffer();
	String response = "HTTP/1.1 " + String::num_int64(p_response.status) + " " + p_response.reason + "\r\n";
	response += "Content-Type: application/json\r\n";
	response += "Content-Length: " + String::num_int64(body.size()) + "\r\n";
	response += "Connection: close\r\n\r\n";
	PackedByteArray bytes = response.to_utf8_buffer();
	bytes.append_array(body);
	connection->put_data(bytes);
	_reset_connection();
}

void MCPServer::_send_error(int p_status, const String &p_reason, const String &p_code) {
	HTTPResponse response;
	response.status = p_status;
	response.reason = p_reason;
	response.body = "{\"error\":\"" + p_code + "\"}";
	_send_response(response);
}

void MCPServer::_finish_request() {
	HTTPRequest request;
	if (!_parse_headers(request)) {
		_send_error(400, "Bad Request", "bad_request");
		return;
	}
	const int body_size = request_buffer.size() - header_end;
	if (body_size > 0) {
		request.body = request_buffer.slice(header_end, request_buffer.size()).get_string_from_utf8();
	}
	_send_response(_process_request(request));
}

void MCPServer::poll() {
	if (!is_listening()) {
		return;
	}
	if (connection.is_null()) {
		_accept_connection();
		if (connection.is_null()) {
			return;
		}
	}

	connection->poll();
	const StreamPeerSocket::Status status = connection->get_status();
	if (status == StreamPeerSocket::STATUS_ERROR || status == StreamPeerSocket::STATUS_NONE) {
		_reset_connection();
		return;
	}
	if (status != StreamPeerSocket::STATUS_CONNECTED) {
		return;
	}
	if (Time::get_singleton()->get_ticks_msec() - connection_started_ms > request_timeout_ms) {
		_send_error(408, "Request Timeout", "request_timeout");
		return;
	}

	const int available = connection->get_available_bytes();
	if (available > 0) {
		if (request_buffer.size() + available > max_request_bytes) {
			_send_error(413, "Content Too Large", "request_too_large");
			return;
		}
		const Array received = connection->get_data(available);
		if ((Error)(int64_t)received[0] != OK) {
			_reset_connection();
			return;
		}
		request_buffer.append_array(received[1]);
	}

	if (header_end < 0) {
		for (int i = 0; i + 3 < request_buffer.size(); i++) {
			if (request_buffer[i] == '\r' && request_buffer[i + 1] == '\n' && request_buffer[i + 2] == '\r' &&
					request_buffer[i + 3] == '\n') {
				header_end = i + 4;
				break;
			}
		}
		if (header_end < 0) {
			return;
		}

		HTTPRequest headers;
		if (!_parse_headers(headers)) {
			_send_error(400, "Bad Request", "bad_request");
			return;
		}
		const Variant raw_content_length = headers.headers.get("content-length", Variant());
		if (raw_content_length.get_type() != Variant::STRING || !String(raw_content_length).is_valid_int()) {
			_send_error(400, "Bad Request", "invalid_content_length");
			return;
		}
		const int64_t parsed_length = String(raw_content_length).to_int();
		if (parsed_length < 0) {
			_send_error(400, "Bad Request", "invalid_content_length");
			return;
		}
		if (parsed_length > max_request_bytes - header_end) {
			_send_error(413, "Content Too Large", "request_too_large");
			return;
		}
		content_length = (int)parsed_length;
	}

	if (request_buffer.size() - header_end >= content_length) {
		_finish_request();
	}
}

void MCPServer::stop() {
	_reset_connection();
	if (listener.is_valid() && listener->is_listening()) {
		listener->stop();
	}
	token = String();
	port = 0;
	dispatcher.reset();
}

bool MCPServer::is_listening() const {
	return listener.is_valid() && listener->is_listening();
}

int MCPServer::get_port() const {
	return port;
}

String MCPServer::get_endpoint() const {
	return "http://127.0.0.1:" + String::num_int64(port) + "/mcp";
}

const String &MCPServer::get_token() const {
	return token;
}

} // namespace godot
