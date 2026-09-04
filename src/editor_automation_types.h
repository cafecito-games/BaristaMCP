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
	// The editor's own control tree is deeper than a shallow default can reach, and a truncated
	// capture can never certify uniqueness, so the default is the clamp maximum: at 8 the capture is
	// depth-truncated and require_unique always answers ambiguous_selector, while at 32 it is
	// untruncated and stays far inside the element and payload budgets below.
	static constexpr int DEFAULT_MAX_DEPTH = MAX_MAX_DEPTH;
	static constexpr int MIN_MAX_ELEMENTS = 1;
	static constexpr int MAX_MAX_ELEMENTS = 2000;
	static constexpr int DEFAULT_MAX_ELEMENTS = 1000;
	// Serialized snapshots stay well beneath the resource budget and the transport response cap,
	// which must hold both the structured payload and its text rendering.
	static constexpr int MAX_PAYLOAD_BYTES = 256 * 1024;
	// Nodes that produce no element still consume this budget, so traversal is bounded even through a
	// deep chain of nodes that are neither controls nor windows.
	static constexpr int MAX_TRAVERSAL_DEPTH = 256;
	// Total units of traversal work one capture may spend. Every visited node costs one, whether or
	// not it emits an element, and scanning one parent's child list costs one more, so neither a deep
	// chain nor a single enormous child list can stall the editor.
	static constexpr int MAX_VISITED_NODES = 50000;
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

// Bounds every selector query, its match walk, and the pagination cursors issued for it.
struct EditorSelectorLimits {
	// A selector may nest "within" this many times before it is rejected as invalid.
	static constexpr int MAX_NESTING_DEPTH = 4;
	static constexpr int MIN_LIMIT = 1;
	static constexpr int MAX_LIMIT = 200;
	static constexpr int DEFAULT_LIMIT = 50;
	static constexpr int MIN_OFFSET = 0;
	// A snapshot never holds more than MAX_MAX_ELEMENTS elements, so no honest cursor can point past
	// this offset; anything larger is rejected instead of being clamped.
	static constexpr int MAX_OFFSET = EditorSnapshotLimits::MAX_MAX_ELEMENTS;
	// Units of matching work one query may spend. Visiting an element costs one, and every ancestor
	// consulted for a "within" constraint costs one more, so a deep tree cannot stall the editor.
	static constexpr int MAX_MATCH_VISITS = 400000;
	// Longest accepted selector string value. Longer values cannot match a bounded element field.
	static constexpr int MAX_VALUE_LENGTH = 512;
	static constexpr int MAX_CURSOR_LENGTH = 1024;
};

// Bounds the registry of handles Barista has issued. A handle that is not in the registry is stale by
// definition, so the registry can never grow without limit and can never be bypassed.
struct EditorHandleLimits {
	static constexpr int MAX_ISSUED_HANDLES = 20000;
	static constexpr const char *HANDLE_PREFIX = "el:";
};

// Bounds the edited-scene traversal published by EditorStateReader.
struct EditorSceneLimits {
	static constexpr int MAX_DEPTH = 32;
	static constexpr int MAX_NODES = 2000;
	static constexpr int MAX_VISITED_NODES = 50000;
	static constexpr int MAX_LIST_ITEMS = 64;
};

// Bounds every explicit editor operation. Both numeric boundary fields advertise an explicit range,
// so a JSON number that no fixed-width integer can hold is rejected rather than converted.
struct EditorOperationLimits {
	static constexpr int MIN_LINE = 1;
	static constexpr int MAX_LINE = 1000000;
	static constexpr int MIN_COLUMN = 0;
	static constexpr int MAX_COLUMN = 100000;
	// Longest accepted main-screen name. A longer name cannot name a main screen the editor shows.
	static constexpr int MAX_SCREEN_NAME_LENGTH = 64;
};

// One page of selector matches. Cursor identity carries the snapshot generation, the snapshot options,
// and the selector itself, so a page can never be resumed against a different capture or query.
struct EditorSelectorCursor {
	uint64_t generation = 0;
	EditorSnapshotOptions options;
	int offset = 0;
	int limit = EditorSelectorLimits::DEFAULT_LIMIT;
	uint64_t selector_digest = 0;
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
