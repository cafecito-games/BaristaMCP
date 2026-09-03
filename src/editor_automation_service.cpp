/**************************************************************************/
/*  editor_automation_service.cpp                                         */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_automation_service.h"

#include "editor_selector.h"
#include "editor_snapshot.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

int clamp_int(int64_t p_value, int p_min, int p_max) {
	if (p_value < p_min) {
		return p_min;
	}
	if (p_value > p_max) {
		return p_max;
	}
	return (int)p_value;
}

int64_t serialized_size(const Dictionary &p_payload) {
	return JSON::stringify(p_payload, "", false).to_utf8_buffer().size();
}

} // namespace

void EditorAutomationService::configure(EditorInterface *p_editor_interface) {
	editor_interface = p_editor_interface;
}

EditorSnapshotOptions EditorAutomationService::parse_options(const Dictionary &p_arguments) {
	// Types and the int64_t representable range are both enforced by the boundary schema before this
	// runs, so these conversions cannot narrow. The documented option ranges are clamped here so a
	// client can never request an unbounded capture.
	EditorSnapshotOptions options;
	if (p_arguments.has("max_depth")) {
		options.max_depth = clamp_int((int64_t)p_arguments.get("max_depth", options.max_depth),
				EditorSnapshotLimits::MIN_MAX_DEPTH, EditorSnapshotLimits::MAX_MAX_DEPTH);
	}
	if (p_arguments.has("max_elements")) {
		options.max_elements = clamp_int((int64_t)p_arguments.get("max_elements", options.max_elements),
				EditorSnapshotLimits::MIN_MAX_ELEMENTS, EditorSnapshotLimits::MAX_MAX_ELEMENTS);
	}
	if (p_arguments.has("include_internal")) {
		options.include_internal = (bool)p_arguments.get("include_internal", false);
	}
	return options;
}

bool EditorAutomationService::_capture(const EditorSnapshotOptions &p_requested, EditorSnapshotData &r_data,
		Dictionary &r_payload, String &r_error, String &r_message) {
	if (editor_interface == nullptr) {
		r_error = "unsupported_capability";
		r_message = "No editor interface is available for UI inspection.";
		return false;
	}

	EditorSnapshotOptions applied = p_requested;
	// Deterministic truncation: halve the element budget until the serialized snapshot fits the
	// published payload limit, and report the budget that actually produced the result.
	while (true) {
		generation++;
		if (!EditorSnapshot::capture(editor_interface, generation, applied, r_data)) {
			r_error = "unsupported_capability";
			r_message = "The editor base control is unavailable; no UI snapshot was produced.";
			return false;
		}
		r_data.requested_options = p_requested;
		r_data.applied_options = applied;
		r_payload = EditorSnapshot::serialize(r_data);
		if (serialized_size(r_payload) <= EditorSnapshotLimits::MAX_PAYLOAD_BYTES ||
				applied.max_elements <= EditorSnapshotLimits::MIN_MAX_ELEMENTS) {
			break;
		}
		applied.max_elements = applied.max_elements / 2;
		if (applied.max_elements < EditorSnapshotLimits::MIN_MAX_ELEMENTS) {
			applied.max_elements = EditorSnapshotLimits::MIN_MAX_ELEMENTS;
		}
	}
	_register_handles(r_data);
	return true;
}

void EditorAutomationService::_register_elements(
		const std::vector<EditorElement> &p_elements, uint64_t p_generation, int &r_budget) {
	for (const EditorElement &element : p_elements) {
		if (r_budget <= 0) {
			return;
		}
		r_budget--;
		const String prefix = EditorHandleLimits::HANDLE_PREFIX;
		if (element.handle.begins_with(prefix)) {
			const String instance_text = element.handle.substr(prefix.length());
			if (instance_text.is_valid_int()) {
				IssuedHandle record;
				record.class_name = element.class_name;
				record.generation = p_generation;
				issued_handles[(uint64_t)instance_text.to_int()] = record;
			}
		}
		_register_elements(element.children, p_generation, r_budget);
	}
}

void EditorAutomationService::_register_handles(const EditorSnapshotData &p_data) {
	// A capture never produces more than MAX_MAX_ELEMENTS elements, so registration is bounded before
	// it starts walking.
	int budget = EditorSnapshotLimits::MAX_MAX_ELEMENTS;
	_register_elements(p_data.roots, p_data.generation, budget);
	if ((int)issued_handles.size() <= EditorHandleLimits::MAX_ISSUED_HANDLES) {
		return;
	}
	// Bounded reconciliation: one pass drops every handle that the newest capture did not reissue.
	for (auto entry = issued_handles.begin(); entry != issued_handles.end();) {
		if (entry->second.generation < p_data.generation) {
			entry = issued_handles.erase(entry);
		} else {
			++entry;
		}
	}
}

namespace {

const EditorElement *find_by_handle(const std::vector<EditorElement> &p_elements, const String &p_handle) {
	for (const EditorElement &element : p_elements) {
		if (element.handle == p_handle) {
			return &element;
		}
		const EditorElement *found = find_by_handle(element.children, p_handle);
		if (found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

bool parse_handle(const String &p_handle, uint64_t &r_instance_id) {
	const String prefix = EditorHandleLimits::HANDLE_PREFIX;
	if (!p_handle.begins_with(prefix)) {
		return false;
	}
	const String instance_text = p_handle.substr(prefix.length());
	if (instance_text.is_empty() || !instance_text.is_valid_int() || instance_text.begins_with("-")) {
		return false;
	}
	r_instance_id = (uint64_t)instance_text.to_int();
	return true;
}

} // namespace

bool EditorAutomationService::_resolve_handle(const String &p_handle, const EditorSnapshotOptions &p_options,
		EditorSnapshotData &r_data, const EditorElement **r_element, String &r_error, String &r_message) {
	*r_element = nullptr;
	uint64_t instance_id = 0;
	// A handle is only ever compared against the registry and against a captured snapshot; it is never
	// used to look an object up.
	if (!parse_handle(p_handle, instance_id)) {
		r_error = "stale_handle";
		r_message = "Handle '" + p_handle + "' was not issued by Barista.";
		return false;
	}
	const auto issued = issued_handles.find(instance_id);
	if (issued == issued_handles.end()) {
		r_error = "stale_handle";
		r_message = "Handle '" + p_handle + "' was not issued by Barista.";
		return false;
	}

	// The capture below reissues handles and may prune the registry, so the recorded class is copied
	// out before it runs.
	const IssuedHandle previous = issued->second;

	Dictionary payload;
	String capture_error;
	String capture_message;
	if (!_capture(p_options, r_data, payload, capture_error, capture_message)) {
		r_error = capture_error;
		r_message = capture_message;
		return false;
	}
	const EditorElement *element = find_by_handle(r_data.roots, p_handle);
	if (element == nullptr) {
		r_error = "stale_handle";
		r_message = "Handle '" + p_handle + "' no longer resolves to a captured element.";
		return false;
	}
	// _capture reissued this handle, so the recorded class is the class the object has now; comparing
	// against the class recorded before the capture is what detects a reused instance id.
	if (element->class_name != previous.class_name) {
		r_error = "stale_handle";
		r_message = "Handle '" + p_handle + "' now refers to a different object type.";
		return false;
	}
	*r_element = element;
	return true;
}

Dictionary EditorAutomationService::inspect_ui(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();
	EditorSnapshotData data;
	Dictionary payload;
	if (!_capture(parse_options(p_arguments), data, payload, r_error, r_message)) {
		return Dictionary();
	}
	cached_snapshot = data;
	has_cached_snapshot = true;
	return payload;
}

Dictionary EditorAutomationService::read_element(
		const String &p_handle, bool p_include_children, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();
	EditorSnapshotData data;
	const EditorElement *element = nullptr;
	EditorSnapshotOptions options;
	options.max_depth = EditorSnapshotLimits::MAX_MAX_DEPTH;
	if (!_resolve_handle(p_handle, options, data, &element, r_error, r_message)) {
		return Dictionary();
	}
	Dictionary payload;
	payload["generation"] = (int64_t)data.generation;
	payload["element"] = EditorSnapshot::serialize_element(*element, p_include_children);
	return payload;
}

namespace {

Dictionary find_payload(bool p_ok, const String &p_status, const String &p_message, uint64_t p_generation,
		int p_match_count, int p_offset, int p_limit, bool p_truncated, const String &p_next_cursor,
		const Array &p_matches) {
	Dictionary payload;
	payload["ok"] = p_ok;
	payload["status"] = p_status;
	payload["message"] = p_message;
	payload["generation"] = (int64_t)p_generation;
	payload["match_count"] = p_match_count;
	payload["returned_count"] = (int)p_matches.size();
	payload["offset"] = p_offset;
	payload["limit"] = p_limit;
	payload["truncated"] = p_truncated;
	payload["next_cursor"] = p_next_cursor;
	payload["matches"] = p_matches;
	return payload;
}

Dictionary find_failure(EditorSelector::Status p_status, const String &p_message, int p_limit) {
	return find_payload(
			false, EditorSelector::status_name(p_status), p_message, 0, 0, 0, p_limit, false, String(), Array());
}

} // namespace

Dictionary EditorAutomationService::find_ui(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();

	int limit = EditorSelectorLimits::DEFAULT_LIMIT;
	if (p_arguments.has("limit")) {
		// The advertised range is enforced by the boundary schema, so this conversion cannot narrow.
		limit = clamp_int((int64_t)p_arguments.get("limit", limit), EditorSelectorLimits::MIN_LIMIT,
				EditorSelectorLimits::MAX_LIMIT);
	}
	const bool require_unique = p_arguments.has("require_unique") && (bool)p_arguments.get("require_unique", false);

	if (!p_arguments.has("selector")) {
		return find_failure(EditorSelector::Status::INVALID_SELECTOR,
				"find_editor_ui requires a selector; an absent selector never matches everything.", limit);
	}
	EditorSelectorQuery query;
	String selector_message;
	if (!EditorSelector::parse(p_arguments.get("selector", Variant()), query, selector_message)) {
		return find_failure(EditorSelector::Status::INVALID_SELECTOR, selector_message, limit);
	}
	const uint64_t selector_digest = EditorSelector::digest(query);

	EditorSnapshotOptions options = parse_options(p_arguments);
	int offset = 0;
	bool resume = false;
	if (p_arguments.has("cursor")) {
		EditorSelectorCursor cursor;
		if (!EditorSelector::decode_cursor(p_arguments.get("cursor", Variant()), cursor)) {
			return find_failure(EditorSelector::Status::INVALID_CURSOR, "Cursor is malformed.", limit);
		}
		if (!has_cached_snapshot || cursor.generation != cached_snapshot.generation) {
			return find_failure(EditorSelector::Status::INVALID_CURSOR,
					"Cursor names a capture that is no longer resumable.", limit);
		}
		// An explicitly supplied option must agree with the cursor: silently re-reading a page under
		// different options would answer about a different capture.
		const bool options_conflict = (p_arguments.has("max_depth") && options.max_depth != cursor.options.max_depth) ||
				(p_arguments.has("max_elements") && options.max_elements != cursor.options.max_elements) ||
				(p_arguments.has("include_internal") && options.include_internal != cursor.options.include_internal);
		if (options_conflict || cursor.options.max_depth != cached_snapshot.applied_options.max_depth ||
				cursor.options.max_elements != cached_snapshot.applied_options.max_elements ||
				cursor.options.include_internal != cached_snapshot.applied_options.include_internal) {
			return find_failure(EditorSelector::Status::INVALID_CURSOR,
					"Cursor snapshot options do not match this request.", limit);
		}
		if (cursor.selector_digest != selector_digest) {
			return find_failure(
					EditorSelector::Status::INVALID_CURSOR, "Cursor was issued for a different selector.", limit);
		}
		if (p_arguments.has("limit") && limit != cursor.limit) {
			return find_failure(
					EditorSelector::Status::INVALID_CURSOR, "Cursor was issued for a different page size.", limit);
		}
		options = cursor.options;
		offset = cursor.offset;
		limit = cursor.limit;
		resume = true;
	}

	// The registry is consulted before this request captures anything. Capturing first would register
	// every live handle, so a handle Barista had never issued would be admitted by the very request
	// that supplied it, and the class recorded for a reused instance id would be overwritten before it
	// could be compared.
	String previous_class;
	if (query.has_handle) {
		uint64_t instance_id = 0;
		if (!parse_handle(query.handle, instance_id)) {
			return find_failure(EditorSelector::Status::STALE_HANDLE,
					"Handle '" + query.handle + "' was not issued by Barista.", limit);
		}
		const auto issued = issued_handles.find(instance_id);
		if (issued == issued_handles.end()) {
			return find_failure(EditorSelector::Status::STALE_HANDLE,
					"Handle '" + query.handle + "' was not issued by Barista.", limit);
		}
		previous_class = issued->second.class_name;
	}

	if (!resume) {
		EditorSnapshotData data;
		Dictionary payload;
		if (!_capture(options, data, payload, r_error, r_message)) {
			return Dictionary();
		}
		cached_snapshot = data;
		has_cached_snapshot = true;
	}

	std::vector<const EditorElement *> page;
	int total = 0;
	bool visit_limit_reached = false;
	const EditorSelector::Status status =
			EditorSelector::match(cached_snapshot, query, offset, limit, page, total, visit_limit_reached);
	if (status == EditorSelector::Status::STALE_HANDLE) {
		return find_failure(EditorSelector::Status::STALE_HANDLE,
				"Element id '" + query.id + "' was issued for another capture.", limit);
	}
	if (status == EditorSelector::Status::NO_MATCH) {
		if (query.has_handle) {
			return find_payload(false, EditorSelector::status_name(EditorSelector::Status::STALE_HANDLE),
					"Handle '" + query.handle + "' no longer resolves to a captured element.",
					cached_snapshot.generation, 0, offset, limit, visit_limit_reached, String(), Array());
		}
		return find_payload(false, EditorSelector::status_name(EditorSelector::Status::NO_MATCH),
				"No element matched the selector.", cached_snapshot.generation, 0, offset, limit, visit_limit_reached,
				String(), Array());
	}
	// A truncated walk only ever counts a lower bound, so uniqueness cannot be established from it.
	if (require_unique && (total > 1 || visit_limit_reached)) {
		const String reason = total > 1
				? "The selector matched " + String::num_int64(total) + " elements where exactly one is required."
				: "The match budget was exhausted, so the selector cannot be shown to match exactly one element.";
		return find_payload(false, EditorSelector::status_name(EditorSelector::Status::AMBIGUOUS_SELECTOR), reason,
				cached_snapshot.generation, total, offset, limit, visit_limit_reached, String(), Array());
	}
	// The capture above reissued this handle, so comparing against the class recorded before the
	// capture is what detects an instance id that now belongs to a different object.
	if (query.has_handle && !page.empty() && page[0]->class_name != previous_class) {
		return find_payload(false, EditorSelector::status_name(EditorSelector::Status::STALE_HANDLE),
				"Handle '" + query.handle + "' now refers to a different object type.", cached_snapshot.generation, 0,
				offset, limit, visit_limit_reached, String(), Array());
	}

	Array matches;
	for (const EditorElement *element : page) {
		matches.push_back(EditorSnapshot::serialize_element(*element, false));
	}
	const int next_offset = offset + (int)page.size();
	String next_cursor;
	if (next_offset < total && !visit_limit_reached) {
		EditorSelectorCursor cursor;
		cursor.generation = cached_snapshot.generation;
		cursor.options = cached_snapshot.applied_options;
		cursor.offset = next_offset;
		cursor.limit = limit;
		cursor.selector_digest = selector_digest;
		next_cursor = EditorSelector::encode_cursor(cursor);
	}
	const bool truncated = next_offset < total || visit_limit_reached || cached_snapshot.depth_truncated ||
			cached_snapshot.element_limit_reached || cached_snapshot.traversal_limit_reached;
	return find_payload(true, EditorSelector::status_name(EditorSelector::Status::OK), String(),
			cached_snapshot.generation, total, offset, limit, truncated, next_cursor, matches);
}

void EditorAutomationService::process(double p_delta) {
	(void)p_delta;
}

void EditorAutomationService::shutdown() {
	editor_interface = nullptr;
	issued_handles.clear();
	cached_snapshot = EditorSnapshotData();
	has_cached_snapshot = false;
}

} // namespace godot
