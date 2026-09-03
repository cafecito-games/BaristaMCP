/**************************************************************************/
/*  editor_automation_types.h                                             */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_AUTOMATION_TYPES_H
#define BARISTA_MCP_EDITOR_AUTOMATION_TYPES_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <vector>

namespace godot {

// Bounds every snapshot capture. Requested values are clamped into these ranges instead of being
// rejected, and the applied values are published with every result.
struct EditorSnapshotLimits {
	static constexpr int MIN_MAX_DEPTH = 1;
	static constexpr int MAX_MAX_DEPTH = 32;
	static constexpr int DEFAULT_MAX_DEPTH = 8;
	static constexpr int MIN_MAX_ELEMENTS = 1;
	static constexpr int MAX_MAX_ELEMENTS = 2000;
	static constexpr int DEFAULT_MAX_ELEMENTS = 1000;
	// Serialized snapshots stay well beneath the resource budget and the transport response cap,
	// which must hold both the structured payload and its text rendering.
	static constexpr int MAX_PAYLOAD_BYTES = 256 * 1024;
	// Nodes that produce no element still consume this budget, so traversal is bounded even through a
	// deep chain of nodes that are neither controls nor windows.
	static constexpr int MAX_TRAVERSAL_DEPTH = 256;
	// Bounded strings keep a single pathological control from consuming the whole payload budget.
	static constexpr int MAX_STRING_LENGTH = 200;
	static constexpr int MAX_PATH_LENGTH = 512;
	static constexpr int MAX_SELECTED_ITEMS = 16;
};

struct EditorSnapshotOptions {
	int max_depth = EditorSnapshotLimits::DEFAULT_MAX_DEPTH;
	int max_elements = EditorSnapshotLimits::DEFAULT_MAX_ELEMENTS;
	bool include_internal = false;
};

// One captured editor control or window. Identity is public engine identity only: never a raw
// pointer, a filesystem path, or a timestamp.
struct EditorElement {
	String id;
	String handle;
	String role;
	String name;
	String text;
	String class_name;
	String path;
	bool visible = true;
	bool enabled = true;
	bool focused = false;
	bool internal = false;
	// True when children were omitted because a depth or element limit was reached.
	bool truncated = false;
	Rect2 bounds;
	PackedStringArray actions;
	Dictionary state;
	std::vector<EditorElement> children;
};

struct EditorSnapshotData {
	uint64_t generation = 0;
	String focused_element_id;
	int element_count = 0;
	bool depth_truncated = false;
	bool element_limit_reached = false;
	bool traversal_limit_reached = false;
	EditorSnapshotOptions requested_options;
	EditorSnapshotOptions applied_options;
	std::vector<EditorElement> roots;
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_AUTOMATION_TYPES_H
