#ifndef BARISTA_MCP_SERVER_H
#define BARISTA_MCP_SERVER_H

#include "mcp_dispatcher.h"

#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class EditorInterface;

class MCPServer {
	struct HTTPRequest {
		String method;
		String path;
		Dictionary headers;
		String body;
	};

	struct HTTPResponse {
		int status = 200;
		String reason = "OK";
		String body;
	};

	Ref<TCPServer> listener;
	Ref<StreamPeerTCP> connection;
	String token;
	int port = 0;
	uint64_t request_timeout_ms = 30000;
	int max_request_bytes = 8 * 1024 * 1024;
	PackedByteArray request_buffer;
	int header_end = -1;
	int content_length = -1;
	uint64_t connection_started_ms = 0;
	MCPDispatcher dispatcher;
	EditorInterface *editor_interface = nullptr;

	void _accept_connection();
	void _reset_connection();
	bool _parse_headers(HTTPRequest &r_request);
	void _finish_request();
	void _send_response(const HTTPResponse &p_response);
	void _send_error(int p_status, const String &p_reason, const String &p_code);
	HTTPResponse _process_request(const HTTPRequest &p_request);
	HTTPResponse _process_json(const String &p_body);
	static bool _is_local_origin(const String &p_origin);
	static Dictionary _json_rpc_error(int p_code, const String &p_message);

public:
	MCPServer();
	~MCPServer();

	Error start(uint16_t p_port, uint64_t p_request_timeout_ms, int p_max_request_bytes);
	void set_editor_interface(EditorInterface *p_editor_interface);
	void poll();
	void stop();

	bool is_listening() const;
	int get_port() const;
	String get_endpoint() const;
	const String &get_token() const;
};

} // namespace godot

#endif // BARISTA_MCP_SERVER_H
