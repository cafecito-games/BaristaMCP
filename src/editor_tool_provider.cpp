#include "editor_tool_provider.h"

#include "mcp_contracts.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

void EditorToolProvider::configure(EditorInterface *p_editor_interface, const String &p_endpoint, int p_port) {
	editor_interface = p_editor_interface;
	endpoint = p_endpoint;
	port = p_port;
}

Dictionary EditorToolProvider::_tool_result(const Dictionary &p_structured, bool p_is_error) {
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = JSON::stringify(p_structured, "", false);
	Array content;
	content.push_back(content_item);

	Dictionary result;
	result["content"] = content;
	result["structuredContent"] = p_structured;
	result["isError"] = p_is_error;
	return result;
}

Dictionary EditorToolProvider::_status(bool p_initialized) const {
	Dictionary status;
	status["name"] = MCPContracts::SERVER_NAME;
	status["version"] = MCPContracts::SERVER_VERSION;
	status["protocol_version"] = MCPContracts::PROTOCOL_VERSION;
	status["initialized"] = p_initialized;
	status["local_only"] = true;
	status["endpoint"] = endpoint;
	status["port"] = port;
	return status;
}

Dictionary EditorToolProvider::_project_info() const {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	Dictionary info;
	info["project_name"] = project_settings->get_setting("application/config/name", "");
	info["project_path"] = project_settings->globalize_path("res://").trim_suffix("/");
	info["godot_version"] = Engine::get_singleton()->get_version_info();

	String current_scene;
	bool is_playing = false;
	if (editor_interface != nullptr) {
		Node *root = editor_interface->get_edited_scene_root();
		if (root != nullptr) {
			current_scene = root->get_scene_file_path();
		}
		is_playing = editor_interface->is_playing_scene();
	}
	info["current_scene"] = current_scene;
	info["is_playing"] = is_playing;
	return info;
}

Dictionary EditorToolProvider::call(const String &p_name, const Dictionary &p_arguments, bool p_initialized) const {
	if (p_name != "barista_status" && p_name != "get_project_info") {
		Dictionary error;
		error["error"] = "unknown_tool";
		error["message"] = "Unknown tool '" + p_name + "'.";
		return _tool_result(error, true);
	}
	if (!p_arguments.is_empty()) {
		Dictionary error;
		error["error"] = "invalid_arguments";
		error["message"] = "Tool '" + p_name + "' does not accept arguments.";
		return _tool_result(error, true);
	}
	if (p_name == "barista_status") {
		return _tool_result(_status(p_initialized), false);
	}
	return _tool_result(_project_info(), false);
}

} // namespace godot
