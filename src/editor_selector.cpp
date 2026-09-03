/**************************************************************************/
/*  editor_selector.cpp                                                   */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_selector.h"

#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

namespace {

constexpr const char *FIELD_ID = "id";
constexpr const char *FIELD_HANDLE = "handle";
constexpr const char *FIELD_ROLE = "role";
constexpr const char *FIELD_NAME = "name";
constexpr const char *FIELD_TEXT = "text";
constexpr const char *FIELD_TEXT_CONTAINS = "text_contains";
constexpr const char *FIELD_CLASS = "class";
constexpr const char *FIELD_VISIBLE = "visible";
constexpr const char *FIELD_ENABLED = "enabled";
constexpr const char *FIELD_FOCUSED = "focused";
constexpr const char *FIELD_PRESSED = "pressed";
constexpr const char *FIELD_SELECTED = "selected";
constexpr const char *FIELD_WITHIN = "within";

// Every field the selector vocabulary accepts, in the order it is advertised and digested.
const char *const SELECTOR_FIELDS[] = {FIELD_ID, FIELD_HANDLE, FIELD_ROLE, FIELD_NAME, FIELD_TEXT, FIELD_TEXT_CONTAINS,
		FIELD_CLASS, FIELD_VISIBLE, FIELD_ENABLED, FIELD_FOCUSED, FIELD_PRESSED, FIELD_SELECTED, FIELD_WITHIN};

constexpr const char *CURSOR_MAGIC = "bcur1";
constexpr const char *CURSOR_SEPARATOR = "|";
constexpr int CURSOR_FIELD_COUNT = 8;

bool read_string_field(
		const Dictionary &p_selector, const char *p_field, bool &r_has, String &r_value, String &r_message) {
	if (!p_selector.has(p_field)) {
		return true;
	}
	const Variant value = p_selector.get(p_field, Variant());
	if (value.get_type() != Variant::STRING && value.get_type() != Variant::STRING_NAME) {
		r_message = String("Selector field '") + p_field + "' must be a string.";
		return false;
	}
	const String text = value;
	if (text.length() > EditorSelectorLimits::MAX_VALUE_LENGTH) {
		r_message = String("Selector field '") + p_field + "' is longer than any element field can be.";
		return false;
	}
	r_has = true;
	r_value = text;
	return true;
}

bool read_bool_field(const Dictionary &p_selector, const char *p_field, bool &r_has, bool &r_value, String &r_message) {
	if (!p_selector.has(p_field)) {
		return true;
	}
	const Variant value = p_selector.get(p_field, Variant());
	if (value.get_type() != Variant::BOOL) {
		r_message = String("Selector field '") + p_field + "' must be a boolean.";
		return false;
	}
	r_has = true;
	r_value = value;
	return true;
}

bool parse_query(const Variant &p_selector, EditorSelectorQuery &r_query, int p_depth, String &r_message) {
	if (p_depth > EditorSelectorLimits::MAX_NESTING_DEPTH) {
		r_message = "Selector nests 'within' deeper than the published limit.";
		return false;
	}
	if (p_selector.get_type() != Variant::DICTIONARY) {
		r_message = "Selector must be an object.";
		return false;
	}
	const Dictionary selector = p_selector;
	if (selector.is_empty()) {
		r_message = "Selector must name at least one constraint.";
		return false;
	}

	const Array keys = selector.keys();
	for (int i = 0; i < keys.size(); i++) {
		const Variant key = keys[i];
		if (key.get_type() != Variant::STRING && key.get_type() != Variant::STRING_NAME) {
			r_message = "Selector must use string field names.";
			return false;
		}
		const String name = key;
		bool known = false;
		for (const char *field : SELECTOR_FIELDS) {
			if (name == field) {
				known = true;
				break;
			}
		}
		if (!known) {
			r_message = "Unknown selector field '" + name + "'.";
			return false;
		}
	}

	if (!read_string_field(selector, FIELD_ID, r_query.has_id, r_query.id, r_message) ||
			!read_string_field(selector, FIELD_HANDLE, r_query.has_handle, r_query.handle, r_message) ||
			!read_string_field(selector, FIELD_ROLE, r_query.has_role, r_query.role, r_message) ||
			!read_string_field(selector, FIELD_NAME, r_query.has_name, r_query.name, r_message) ||
			!read_string_field(selector, FIELD_TEXT, r_query.has_text, r_query.text, r_message) ||
			!read_string_field(
					selector, FIELD_TEXT_CONTAINS, r_query.has_text_contains, r_query.text_contains, r_message) ||
			!read_string_field(selector, FIELD_CLASS, r_query.has_class_name, r_query.class_name, r_message) ||
			!read_bool_field(selector, FIELD_VISIBLE, r_query.has_visible, r_query.visible, r_message) ||
			!read_bool_field(selector, FIELD_ENABLED, r_query.has_enabled, r_query.enabled, r_message) ||
			!read_bool_field(selector, FIELD_FOCUSED, r_query.has_focused, r_query.focused, r_message) ||
			!read_bool_field(selector, FIELD_PRESSED, r_query.has_pressed, r_query.pressed, r_message) ||
			!read_bool_field(selector, FIELD_SELECTED, r_query.has_selected, r_query.selected, r_message)) {
		return false;
	}

	if (selector.has(FIELD_WITHIN)) {
		std::unique_ptr<EditorSelectorQuery> within(new EditorSelectorQuery());
		if (!parse_query(selector.get(FIELD_WITHIN, Variant()), *within, p_depth + 1, r_message)) {
			return false;
		}
		r_query.within = std::move(within);
	}
	return true;
}

// Length-framed so that no value can impersonate a field separator: "role=button;visible=1" as one
// value and role plus visible as two fields must never serialize to the same canonical string.
void append_field(String &r_canonical, const char *p_field, const String &p_value) {
	r_canonical += String(p_field) + "=" + String::num_int64(p_value.length()) + ":" + p_value + ";";
}

void digest_into(const EditorSelectorQuery &p_query, String &r_canonical) {
	r_canonical += "(";
	if (p_query.has_id) {
		append_field(r_canonical, FIELD_ID, p_query.id);
	}
	if (p_query.has_handle) {
		append_field(r_canonical, FIELD_HANDLE, p_query.handle);
	}
	if (p_query.has_role) {
		append_field(r_canonical, FIELD_ROLE, p_query.role);
	}
	if (p_query.has_name) {
		append_field(r_canonical, FIELD_NAME, p_query.name);
	}
	if (p_query.has_text) {
		append_field(r_canonical, FIELD_TEXT, p_query.text);
	}
	if (p_query.has_text_contains) {
		append_field(r_canonical, FIELD_TEXT_CONTAINS, p_query.text_contains);
	}
	if (p_query.has_class_name) {
		append_field(r_canonical, FIELD_CLASS, p_query.class_name);
	}
	if (p_query.has_visible) {
		append_field(r_canonical, FIELD_VISIBLE, p_query.visible ? "1" : "0");
	}
	if (p_query.has_enabled) {
		append_field(r_canonical, FIELD_ENABLED, p_query.enabled ? "1" : "0");
	}
	if (p_query.has_focused) {
		append_field(r_canonical, FIELD_FOCUSED, p_query.focused ? "1" : "0");
	}
	if (p_query.has_pressed) {
		append_field(r_canonical, FIELD_PRESSED, p_query.pressed ? "1" : "0");
	}
	if (p_query.has_selected) {
		append_field(r_canonical, FIELD_SELECTED, p_query.selected ? "1" : "0");
	}
	if (p_query.within) {
		r_canonical += String(FIELD_WITHIN) + "=";
		digest_into(*p_query.within, r_canonical);
	}
	r_canonical += ")";
}

// A selection constraint only applies to an element that publishes a selection, so a constraint on
// state an element does not have can never be satisfied in either direction.
bool element_selection_state(const EditorElement &p_element, bool &r_selected) {
	if (p_element.state.has("selected_items")) {
		const Variant items = p_element.state.get("selected_items", Variant());
		if (items.get_type() != Variant::ARRAY) {
			return false;
		}
		r_selected = ((Array)items).size() > 0;
		return true;
	}
	if (p_element.state.has("selected_index")) {
		const Variant index = p_element.state.get("selected_index", Variant());
		if (index.get_type() != Variant::INT) {
			return false;
		}
		r_selected = (int64_t)index >= 0;
		return true;
	}
	return false;
}

bool matches_fields(const EditorElement &p_element, const EditorSelectorQuery &p_query) {
	if (p_query.has_id && p_element.id != p_query.id) {
		return false;
	}
	if (p_query.has_handle && p_element.handle != p_query.handle) {
		return false;
	}
	if (p_query.has_role && p_element.role != p_query.role) {
		return false;
	}
	if (p_query.has_name && p_element.name != p_query.name) {
		return false;
	}
	if (p_query.has_text && p_element.text != p_query.text) {
		return false;
	}
	if (p_query.has_text_contains && !p_element.text.contains(p_query.text_contains)) {
		return false;
	}
	if (p_query.has_class_name && p_element.class_name != p_query.class_name) {
		return false;
	}
	if (p_query.has_visible && p_element.visible != p_query.visible) {
		return false;
	}
	if (p_query.has_enabled && p_element.enabled != p_query.enabled) {
		return false;
	}
	if (p_query.has_focused && p_element.focused != p_query.focused) {
		return false;
	}
	if (p_query.has_pressed) {
		const Variant pressed = p_element.state.get("pressed", Variant());
		if (pressed.get_type() != Variant::BOOL || (bool)pressed != p_query.pressed) {
			return false;
		}
	}
	if (p_query.has_selected) {
		bool selected = false;
		if (!element_selection_state(p_element, selected) || selected != p_query.selected) {
			return false;
		}
	}
	return true;
}

struct MatchWalker {
	const EditorSelectorQuery &query;
	int offset = 0;
	int limit = 0;
	int total = 0;
	int visits = 0;
	bool visit_limit_reached = false;
	std::vector<const EditorElement *> ancestors;
	std::vector<const EditorElement *> *page = nullptr;

	explicit MatchWalker(const EditorSelectorQuery &p_query) : query(p_query) {}

	// Charges one unit of the published budget before doing any work with it.
	bool spend() {
		if (visits >= EditorSelectorLimits::MAX_MATCH_VISITS) {
			visit_limit_reached = true;
			return false;
		}
		visits++;
		return true;
	}

	bool matches_with_ancestors(
			const EditorElement &p_element, int p_ancestor_count, const EditorSelectorQuery &p_query) {
		if (!matches_fields(p_element, p_query)) {
			return false;
		}
		if (!p_query.within) {
			return true;
		}
		for (int i = p_ancestor_count - 1; i >= 0; i--) {
			if (!spend()) {
				return false;
			}
			if (matches_with_ancestors(*ancestors[i], i, *p_query.within)) {
				return true;
			}
		}
		return false;
	}

	void walk(const std::vector<EditorElement> &p_elements) {
		for (const EditorElement &element : p_elements) {
			if (!spend()) {
				return;
			}
			if (matches_with_ancestors(element, (int)ancestors.size(), query)) {
				if (total >= offset && (int)page->size() < limit) {
					page->push_back(&element);
				}
				total++;
			}
			ancestors.push_back(&element);
			walk(element.children);
			ancestors.pop_back();
			if (visit_limit_reached) {
				return;
			}
		}
	}
};

} // namespace

PackedStringArray EditorSelector::status_vocabulary() {
	PackedStringArray statuses;
	statuses.push_back("ok");
	statuses.push_back("no_match");
	statuses.push_back("ambiguous_selector");
	statuses.push_back("invalid_selector");
	statuses.push_back("stale_handle");
	statuses.push_back("invalid_cursor");
	return statuses;
}

String EditorSelector::status_name(Status p_status) {
	return status_vocabulary()[(int)p_status];
}

PackedStringArray EditorSelector::field_vocabulary() {
	PackedStringArray fields;
	for (const char *field : SELECTOR_FIELDS) {
		fields.push_back(field);
	}
	return fields;
}

bool EditorSelector::parse(const Variant &p_selector, EditorSelectorQuery &r_query, String &r_message) {
	r_message = String();
	return parse_query(p_selector, r_query, 0, r_message);
}

uint64_t EditorSelector::digest(const EditorSelectorQuery &p_query) {
	String canonical;
	digest_into(p_query, canonical);
	// The canonical form is injective, so folding its length into the hash keeps selectors of different
	// shapes apart even where the 32-bit string hash would collide.
	return ((uint64_t)(uint32_t)canonical.hash()) | ((uint64_t)(uint32_t)canonical.length() << 32);
}

EditorSelector::Status EditorSelector::match(const EditorSnapshotData &p_data, const EditorSelectorQuery &p_query,
		int p_offset, int p_limit, std::vector<const EditorElement *> &r_page, int &r_total,
		bool &r_visit_limit_reached) {
	r_page.clear();
	r_total = 0;
	r_visit_limit_reached = false;

	// A pinned element id names the capture it came from. Resolving it against any other capture would
	// silently answer about a different element, so it fails closed instead.
	if (p_query.has_id) {
		if (!p_query.id.begins_with("s")) {
			return Status::STALE_HANDLE;
		}
		const int separator = p_query.id.find(":");
		if (separator < 2) {
			return Status::STALE_HANDLE;
		}
		const String generation_text = p_query.id.substr(1, separator - 1);
		if (!generation_text.is_valid_int() || (uint64_t)generation_text.to_int() != p_data.generation) {
			return Status::STALE_HANDLE;
		}
	}

	MatchWalker walker(p_query);
	walker.offset = p_offset;
	walker.limit = p_limit;
	walker.page = &r_page;
	walker.walk(p_data.roots);

	r_total = walker.total;
	r_visit_limit_reached = walker.visit_limit_reached;
	if (walker.total == 0) {
		return Status::NO_MATCH;
	}
	return Status::OK;
}

String EditorSelector::encode_cursor(const EditorSelectorCursor &p_cursor) {
	String payload = String(CURSOR_MAGIC);
	payload += CURSOR_SEPARATOR + String::num_uint64(p_cursor.generation);
	payload += CURSOR_SEPARATOR + String::num_int64(p_cursor.options.max_depth);
	payload += CURSOR_SEPARATOR + String::num_int64(p_cursor.options.max_elements);
	payload += CURSOR_SEPARATOR + String(p_cursor.options.include_internal ? "1" : "0");
	payload += CURSOR_SEPARATOR + String::num_int64(p_cursor.offset);
	payload += CURSOR_SEPARATOR + String::num_int64(p_cursor.limit);
	payload += CURSOR_SEPARATOR + String::num_uint64(p_cursor.selector_digest);
	return Marshalls::get_singleton()->utf8_to_base64(payload);
}

bool EditorSelector::decode_cursor(const String &p_cursor, EditorSelectorCursor &r_cursor) {
	if (p_cursor.is_empty() || p_cursor.length() > EditorSelectorLimits::MAX_CURSOR_LENGTH) {
		return false;
	}
	// Reject anything that is not base64 before handing it to the engine decoder, so a malformed
	// cursor is a plain rejection rather than an engine-level decode failure.
	if ((p_cursor.length() % 4) != 0) {
		return false;
	}
	for (int i = 0; i < p_cursor.length(); i++) {
		const char32_t character = p_cursor[i];
		const bool base64_character = (character >= 'A' && character <= 'Z') ||
				(character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '+' ||
				character == '/' || character == '=';
		if (!base64_character) {
			return false;
		}
	}
	const String payload = Marshalls::get_singleton()->base64_to_utf8(p_cursor);
	// A cursor has exactly one spelling. Requiring the round trip rejects every mutation that decodes
	// to the same payload, such as trailing groups that add bytes past the encoded text.
	if (Marshalls::get_singleton()->utf8_to_base64(payload) != p_cursor) {
		return false;
	}
	const PackedStringArray fields = payload.split(CURSOR_SEPARATOR, true);
	if (fields.size() != CURSOR_FIELD_COUNT || fields[0] != CURSOR_MAGIC) {
		return false;
	}
	for (int i = 1; i < CURSOR_FIELD_COUNT; i++) {
		if (!fields[i].is_valid_int()) {
			return false;
		}
	}
	const int64_t generation = fields[1].to_int();
	const int64_t max_depth = fields[2].to_int();
	const int64_t max_elements = fields[3].to_int();
	const int64_t include_internal = fields[4].to_int();
	const int64_t offset = fields[5].to_int();
	const int64_t limit = fields[6].to_int();
	if (generation < 1 || max_depth < EditorSnapshotLimits::MIN_MAX_DEPTH ||
			max_depth > EditorSnapshotLimits::MAX_MAX_DEPTH || max_elements < EditorSnapshotLimits::MIN_MAX_ELEMENTS ||
			max_elements > EditorSnapshotLimits::MAX_MAX_ELEMENTS || (include_internal != 0 && include_internal != 1) ||
			offset < EditorSelectorLimits::MIN_OFFSET || offset > EditorSelectorLimits::MAX_OFFSET ||
			limit < EditorSelectorLimits::MIN_LIMIT || limit > EditorSelectorLimits::MAX_LIMIT) {
		return false;
	}
	r_cursor.generation = (uint64_t)generation;
	r_cursor.options.max_depth = (int)max_depth;
	r_cursor.options.max_elements = (int)max_elements;
	r_cursor.options.include_internal = include_internal == 1;
	r_cursor.offset = (int)offset;
	r_cursor.limit = (int)limit;
	r_cursor.selector_digest = (uint64_t)fields[7].to_int();
	return true;
}

} // namespace godot
