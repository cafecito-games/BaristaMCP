/**************************************************************************/
/*  editor_state_reader.h                                                 */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_STATE_READER_H
#define BARISTA_MCP_EDITOR_STATE_READER_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class EditorInterface;

// Single source of truth for public editor and scene state. Tools and resources call these same
// readers, so a tool payload and a resource payload can never describe the editor differently. Every
// reader is side-effect free and bounded: nothing here mutates the editor or the edited scene.
class EditorStateReader {
public:
	static Dictionary project_info(EditorInterface *p_editor_interface);
	// The full editor state, with stable project, scenes, selection, script, filesystem, and play
	// sections. Sections are always present; an unavailable editor yields empty, honest sections.
	static Dictionary editor_state(EditorInterface *p_editor_interface);
	// Summary of the edited scene root without traversing it.
	static Dictionary active_scene(EditorInterface *p_editor_interface);
	// Bounded traversal of the edited scene, published with the limits that produced it.
	static Dictionary scene_tree(EditorInterface *p_editor_interface);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_STATE_READER_H
