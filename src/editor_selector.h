/**************************************************************************/
/*  editor_selector.h                                                     */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_SELECTOR_H
#define BARISTA_MCP_EDITOR_SELECTOR_H

#include "editor_automation_types.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <vector>

namespace godot {

// One parsed selector. Every constraint is explicit: an absent constraint never widens a match, and a
// constraint on state an element does not publish never matches.
struct EditorSelectorQuery {
	bool has_id = false;
	String id;
	bool has_handle = false;
	String handle;
	bool has_role = false;
	String role;
	bool has_name = false;
	String name;
	bool has_text = false;
	String text;
	bool has_text_contains = false;
	String text_contains;
	bool has_class_name = false;
	String class_name;
	bool has_visible = false;
	bool visible = false;
	bool has_enabled = false;
	bool enabled = false;
	bool has_focused = false;
	bool focused = false;
	bool has_pressed = false;
	bool pressed = false;
	bool has_selected = false;
	bool selected = false;
	// An ancestor constraint. The element matches only when some ancestor satisfies this query.
	std::unique_ptr<EditorSelectorQuery> within;
};

// Single source of truth for the selector vocabulary, the selector status vocabulary, the match
// algorithm, and the pagination cursors issued for a match. Matching is a pure function of a captured
// snapshot: it reads no engine state and mutates nothing.
class EditorSelector {
public:
	enum class Status {
		OK,
		NO_MATCH,
		AMBIGUOUS_SELECTOR,
		INVALID_SELECTOR,
		STALE_HANDLE,
		INVALID_CURSOR,
	};

	static PackedStringArray status_vocabulary();
	static PackedStringArray field_vocabulary();
	static String status_name(Status p_status);

	// Parses one selector object. Returns false with a bounded diagnostic for an empty selector, an
	// unknown field, a field of the wrong type, an over-long value, or excessive "within" nesting.
	static bool parse(const Variant &p_selector, EditorSelectorQuery &r_query, String &r_message);

	// The injective canonical form of a parsed selector. Two selectors are the same query exactly when
	// their canonical forms are equal, so cursor identity is checked against this text, not a hash.
	static String canonical(const EditorSelectorQuery &p_query);
	// A cheap digest of the canonical form carried inside a cursor. It narrows a mismatch early; the
	// canonical form is what actually decides identity.
	static uint64_t digest(const EditorSelectorQuery &p_query);
	// True when the query pins at least one element id and every id it names, including the ones
	// nested in "within", belongs to p_generation. A query that pins no id, or that pins one from any
	// other capture, answers false.
	static bool pins_generation(const EditorSelectorQuery &p_query, uint64_t p_generation);
	// Every handle named anywhere in the query, including inside "within".
	static PackedStringArray handles(const EditorSelectorQuery &p_query);

	// Collects matching elements in document order. r_total counts every match; r_page holds the
	// matches inside [p_offset, p_offset + p_limit). Returns STALE_HANDLE when the selector pins an id
	// from another capture, NO_MATCH when nothing matched, and OK otherwise.
	static Status match(const EditorSnapshotData &p_data, const EditorSelectorQuery &p_query, int p_offset, int p_limit,
			std::vector<const EditorElement *> &r_page, int &r_total, bool &r_visit_limit_reached);

	// Cursors are opaque to clients and self-describing to the server: an unparsable cursor, or one
	// whose generation, snapshot options, or selector do not match the request, is rejected.
	static String encode_cursor(const EditorSelectorCursor &p_cursor);
	static bool decode_cursor(const String &p_cursor, EditorSelectorCursor &r_cursor);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_SELECTOR_H
