#include "barista_mcp_plugin.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

constexpr const char *SETTING_ENABLED = "barista_mcp/server/enabled";
constexpr const char *SETTING_PORT = "barista_mcp/server/port";
constexpr const char *SETTING_REQUEST_TIMEOUT_MS = "barista_mcp/server/request_timeout_ms";
constexpr const char *SETTING_MAX_REQUEST_BYTES = "barista_mcp/server/max_request_bytes";

void define_setting(const String &p_name, const Variant &p_default, Variant::Type p_type,
		PropertyHint p_hint = PROPERTY_HINT_NONE, const String &p_hint_string = String()) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings->has_setting(p_name)) {
		settings->set_setting(p_name, p_default);
	}
	settings->set_initial_value(p_name, p_default);

	Dictionary property;
	property["name"] = p_name;
	property["type"] = p_type;
	property["hint"] = p_hint;
	property["hint_string"] = p_hint_string;
	settings->add_property_info(property);
}

} // namespace

void BaristaMCPPlugin::_bind_methods() {}

void BaristaMCPPlugin::_define_project_settings() {
	define_setting(SETTING_ENABLED, true, Variant::BOOL);
	define_setting(SETTING_PORT, 0, Variant::INT, PROPERTY_HINT_RANGE, "0,65535,1");
	define_setting(SETTING_REQUEST_TIMEOUT_MS, 30000, Variant::INT, PROPERTY_HINT_RANGE, "1,300000,1");
	define_setting(SETTING_MAX_REQUEST_BYTES, 8 * 1024 * 1024, Variant::INT, PROPERTY_HINT_RANGE, "1024,16777216,1");
}

void BaristaMCPPlugin::_start_server() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!(bool)settings->get_setting(SETTING_ENABLED, true)) {
		return;
	}

	const int64_t port = settings->get_setting(SETTING_PORT, 0);
	const int64_t request_timeout_ms = settings->get_setting(SETTING_REQUEST_TIMEOUT_MS, 30000);
	const int64_t max_request_bytes = settings->get_setting(SETTING_MAX_REQUEST_BYTES, 8 * 1024 * 1024);
	if (port < 0 || port > 65535 || request_timeout_ms <= 0 || request_timeout_ms > 300000 ||
			max_request_bytes < 1024 || max_request_bytes > 16 * 1024 * 1024) {
		UtilityFunctions::printerr("BaristaMCP: invalid server project settings; refusing to start.");
		return;
	}

	const Error error = server.start((uint16_t)port, (uint64_t)request_timeout_ms, (int)max_request_bytes);
	if (error != OK) {
		UtilityFunctions::printerr("BaristaMCP: failed to bind MCP server (error ", (int)error, ").");
		return;
	}

	Dictionary discovery;
	discovery["transport"] = "mcp";
	discovery["endpoint"] = server.get_endpoint();
	discovery["token"] = server.get_token();
	discovery["local_only"] = true;
	UtilityFunctions::print("BARISTA_MCP " + JSON::stringify(discovery, "", false));
}

void BaristaMCPPlugin::_enter_tree() {
	_define_project_settings();
	_start_server();
	set_process(server.is_listening());
}

void BaristaMCPPlugin::_exit_tree() {
	set_process(false);
	server.stop();
}

void BaristaMCPPlugin::_process(double p_delta) {
	(void)p_delta;
	server.poll();
}

} // namespace godot
