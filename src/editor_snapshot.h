/**************************************************************************/
/*  editor_snapshot.h                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_SNAPSHOT_H
#define BARISTA_MCP_EDITOR_SNAPSHOT_H

#include "editor_automation_types.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

class EditorInterface;

// Single source of truth for semantic element production and serialization. The role and action
// vocabularies advertised by MCPContracts come from here, so a snapshot can never emit a role or an
// action that the boundary contract does not declare.
class EditorSnapshot {
public:
	static PackedStringArray role_vocabulary();
	static PackedStringArray action_vocabulary();

	// Captures the visible public control tree below the editor base control. Returns false when the
	// editor interface or its base control is unavailable, without dereferencing anything.
	static bool capture(EditorInterface *p_editor_interface, uint64_t p_generation,
			const EditorSnapshotOptions &p_options, EditorSnapshotData &r_data);

	static Dictionary serialize(const EditorSnapshotData &p_data);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_SNAPSHOT_H
