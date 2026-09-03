/**************************************************************************/
/*  editor_automation_service.h                                           */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
#define BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H

#include "editor_automation_types.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

class EditorInterface;

// Editor-facing facade owned by the plugin. It holds the monotonic snapshot generation and keeps
// every capture inside the published limits; it never mutates the editor.
class EditorAutomationService {
	EditorInterface *editor_interface = nullptr;
	uint64_t generation = 0;

public:
	void configure(EditorInterface *p_editor_interface);

	// Returns the structured inspect_editor_ui payload. On failure r_error holds a stable failure
	// code and the returned dictionary holds a bounded message.
	Dictionary inspect_ui(const Dictionary &p_arguments, String &r_error, String &r_message);

	void process(double p_delta);
	void shutdown();

	static EditorSnapshotOptions parse_options(const Dictionary &p_arguments);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_AUTOMATION_SERVICE_H
