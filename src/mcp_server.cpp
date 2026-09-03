#include "mcp_server.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/marshalls.hpp>

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
	return OK;
}

void MCPServer::poll() {}

void MCPServer::stop() {
	if (listener.is_valid() && listener->is_listening()) {
		listener->stop();
	}
	token = String();
	port = 0;
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
