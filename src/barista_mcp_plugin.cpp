/**************************************************************************/
/*  barista_mcp_plugin.cpp                                                */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "barista_mcp_plugin.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

constexpr const char *SETTING_ENABLED = "barista_mcp/server/enabled";
constexpr const char *SETTING_PORT = "barista_mcp/server/port";
constexpr const char *SETTING_REQUEST_TIMEOUT_MS = "barista_mcp/server/request_timeout_ms";
constexpr const char *SETTING_MAX_REQUEST_BYTES = "barista_mcp/server/max_request_bytes";
// Editor mutation is a separate opt-in from running the server at all, and it defaults to off.
constexpr const char *SETTING_AUTOMATION_ENABLED = "barista_mcp/automation/enabled";
// The only user argument that enables mutation, matched exactly. It is read once, at startup.
constexpr const char *AUTOMATION_ARGUMENT = "--barista-mcp-automation";

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
	define_setting(SETTING_AUTOMATION_ENABLED, false, Variant::BOOL);
}

bool BaristaMCPPlugin::_read_automation_enabled() {
	// A setting of the wrong type is not an opt-in: only a boolean true enables mutation.
	const Variant automation_value = ProjectSettings::get_singleton()->get_setting(SETTING_AUTOMATION_ENABLED, false);
	if (automation_value.get_type() == Variant::BOOL && (bool)automation_value) {
		return true;
	}
	OS *os = OS::get_singleton();
	if (os == nullptr) {
		return false;
	}
	// Only an exact user argument counts; a prefix or a value-carrying variant of it does not.
	const PackedStringArray user_arguments = os->get_cmdline_user_args();
	for (int i = 0; i < user_arguments.size(); i++) {
		if (user_arguments[i] == AUTOMATION_ARGUMENT) {
			return true;
		}
	}
	return false;
}

void BaristaMCPPlugin::_start_server() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const Variant enabled_value = settings->get_setting(SETTING_ENABLED, true);
	const Variant port_value = settings->get_setting(SETTING_PORT, 0);
	const Variant request_timeout_value = settings->get_setting(SETTING_REQUEST_TIMEOUT_MS, 30000);
	const Variant max_request_bytes_value = settings->get_setting(SETTING_MAX_REQUEST_BYTES, 8 * 1024 * 1024);
	if (enabled_value.get_type() != Variant::BOOL || port_value.get_type() != Variant::INT ||
			request_timeout_value.get_type() != Variant::INT || max_request_bytes_value.get_type() != Variant::INT) {
		UtilityFunctions::printerr("BaristaMCP: invalid server project settings; refusing to start.");
		return;
	}
	if (!(bool)enabled_value) {
		return;
	}

	const int64_t port = port_value;
	const int64_t request_timeout_ms = request_timeout_value;
	const int64_t max_request_bytes = max_request_bytes_value;
	if (port < 0 || port > 65535 || request_timeout_ms <= 0 || request_timeout_ms > 300000 ||
			max_request_bytes < 1024 || max_request_bytes > 16 * 1024 * 1024) {
		UtilityFunctions::printerr("BaristaMCP: invalid server project settings; refusing to start.");
		return;
	}

	// Mutation mode is decided once, here, and is never reconsidered while the server runs.
	automation_service.configure(get_editor_interface(), _read_automation_enabled());
	server.set_editor_interface(get_editor_interface());
	server.set_automation_service(&automation_service);
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
	discovery["automation_enabled"] = automation_service.is_automation_enabled();
	UtilityFunctions::print("BARISTA_MCP " + JSON::stringify(discovery, "", false));
}

void BaristaMCPPlugin::_enter_tree() {
	_define_project_settings();
	_start_server();
	set_process(server.is_listening());
}

void BaristaMCPPlugin::_exit_tree() {
	set_process(false);
	// Every wait handle is cancelled and cleared before the transport releases the dispatcher, so no
	// wait can outlive the service that owns it and none is left for a later frame to advance.
	automation_service.shutdown();
	server.stop();
}

void BaristaMCPPlugin::_process(double p_delta) {
	automation_service.process(p_delta);
	server.poll();
}

} // namespace godot
