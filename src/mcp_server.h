#ifndef BARISTA_MCP_SERVER_H
#define BARISTA_MCP_SERVER_H

#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class MCPServer {
	Ref<TCPServer> listener;
	String token;
	int port = 0;
	uint64_t request_timeout_ms = 30000;
	int max_request_bytes = 8 * 1024 * 1024;

public:
	MCPServer();
	~MCPServer();

	Error start(uint16_t p_port, uint64_t p_request_timeout_ms, int p_max_request_bytes);
	void poll();
	void stop();

	bool is_listening() const;
	int get_port() const;
	String get_endpoint() const;
	const String &get_token() const;
};

} // namespace godot

#endif // BARISTA_MCP_SERVER_H
