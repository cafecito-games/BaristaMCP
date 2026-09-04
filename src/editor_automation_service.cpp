/**************************************************************************/
/*  editor_automation_service.cpp                                         */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_automation_service.h"

#include "editor_action_driver.h"
#include "editor_selector.h"
#include "editor_snapshot.h"
#include "mcp_contracts.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <utility>

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

void EditorAutomationService::configure(EditorInterface *p_editor_interface, bool p_automation_enabled) {
	editor_interface = p_editor_interface;
	automation_enabled = p_automation_enabled;
	wait_manager.configure(&event_log);
	Dictionary detail;
	detail["automation_enabled"] = p_automation_enabled;
	event_log.record(EditorEventLog::TYPE_LIFECYCLE, "server_start", "ok", detail);
}

bool EditorAutomationService::is_automation_enabled() const {
	return automation_enabled;
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

void EditorAutomationService::_register_elements(const std::vector<EditorElement> &p_elements, uint64_t p_generation,
		const EditorSnapshotOptions &p_options, int &r_budget) {
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
				record.options = p_options;
				issued_handles[(uint64_t)instance_text.to_int()] = record;
			}
		}
		_register_elements(element.children, p_generation, p_options, r_budget);
	}
}

void EditorAutomationService::_register_handles(const EditorSnapshotData &p_data) {
	// A capture never produces more than MAX_MAX_ELEMENTS elements, so registration is bounded before
	// it starts walking.
	int budget = EditorSnapshotLimits::MAX_MAX_ELEMENTS;
	_register_elements(p_data.roots, p_data.generation, p_data.applied_options, budget);
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

bool same_options(const EditorSnapshotOptions &p_left, const EditorSnapshotOptions &p_right) {
	return p_left.max_depth == p_right.max_depth && p_left.max_elements == p_right.max_elements &&
			p_left.include_internal == p_right.include_internal;
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

	// No capture is a superset of another. Internal elements can crowd public ones out of the element
	// budget, and a deeper walk can exhaust that budget before reaching an element a shallower walk
	// reached, so a wider option value is not by itself a wider result. The request's options are first
	// widened to cover the capture that issued the handle, and the issuing options themselves are the
	// last resort, because that capture demonstrably reached this element. Every attempt is bounded.
	EditorSnapshotOptions widened = p_options;
	if (previous.options.max_depth > widened.max_depth) {
		widened.max_depth = previous.options.max_depth;
	}
	if (previous.options.max_elements > widened.max_elements) {
		widened.max_elements = previous.options.max_elements;
	}
	widened.include_internal = p_options.include_internal || previous.options.include_internal;

	std::vector<EditorSnapshotOptions> attempts;
	EditorSnapshotOptions public_first = widened;
	public_first.include_internal = false;
	attempts.push_back(public_first);
	if (widened.include_internal) {
		attempts.push_back(widened);
	}
	if (!same_options(previous.options, public_first) && !same_options(previous.options, widened)) {
		attempts.push_back(previous.options);
	}

	Dictionary payload;
	String capture_error;
	String capture_message;
	const EditorElement *element = nullptr;
	for (const EditorSnapshotOptions &options : attempts) {
		if (!_capture(options, r_data, payload, capture_error, capture_message)) {
			r_error = capture_error;
			r_message = capture_message;
			return false;
		}
		element = find_by_handle(r_data.roots, p_handle);
		if (element != nullptr) {
			break;
		}
	}
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
	// This capture belongs to no selector, so no cursor can be resumed against it.
	cached_selector = String();
	return payload;
}

Dictionary EditorAutomationService::read_element(
		const String &p_handle, bool p_include_children, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();
	EditorSnapshotData data;
	const EditorElement *element = nullptr;
	// Handle resolution must not depend on options the URI cannot express: a handle issued by any
	// capture resolves here, so the widest published capture is used.
	EditorSnapshotOptions options;
	options.max_depth = EditorSnapshotLimits::MAX_MAX_DEPTH;
	options.max_elements = EditorSnapshotLimits::MAX_MAX_ELEMENTS;
	options.include_internal = true;
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
		if (cursor.selector_digest != selector_digest || cached_selector != EditorSelector::canonical(query)) {
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

	// The registry is consulted before this request captures anything, and for every handle the query
	// names, including the ones nested in "within". Capturing first would register every live handle,
	// so a handle Barista had never issued would be admitted by the very request that supplied it, and
	// the class recorded for a reused instance id would be overwritten before it could be compared.
	const PackedStringArray query_handles = EditorSelector::handles(query);
	std::vector<String> previous_classes;
	for (int i = 0; i < query_handles.size(); i++) {
		uint64_t instance_id = 0;
		if (!parse_handle(query_handles[i], instance_id)) {
			return find_failure(EditorSelector::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' was not issued by Barista.", limit);
		}
		const auto issued = issued_handles.find(instance_id);
		if (issued == issued_handles.end()) {
			return find_failure(EditorSelector::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' was not issued by Barista.", limit);
		}
		previous_classes.push_back(issued->second.class_name);
	}

	// Every request that is not resuming a cursor answers about a capture it takes now. A cursor
	// resumes against the cached capture instead, so a page boundary always refers to the generation
	// its cursor names.
	if (!resume) {
		EditorSnapshotData data;
		Dictionary payload;
		if (!_capture(options, data, payload, r_error, r_message)) {
			return Dictionary();
		}
		cached_snapshot = data;
		has_cached_snapshot = true;
	}
	if (!resume) {
		// A cursor issued below is resumable only for this query, whether or not the capture is new.
		cached_selector = EditorSelector::canonical(query);
	}

	// A fresh capture above reissued these handles, so comparing against the class recorded before it
	// is what detects an instance id that now belongs to a different object. On the resumed path the
	// cached capture is the one that issued them, so the comparison is against the class that capture
	// recorded.
	for (int i = 0; i < query_handles.size(); i++) {
		const EditorElement *resolved = find_by_handle(cached_snapshot.roots, query_handles[i]);
		if (resolved != nullptr && resolved->class_name != previous_classes[(size_t)i]) {
			return find_failure(EditorSelector::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' now refers to a different object type.", limit);
		}
	}

	std::vector<const EditorElement *> page;
	int total = 0;
	bool visit_limit_reached = false;
	const EditorSelector::Status status =
			EditorSelector::match(cached_snapshot, query, offset, limit, page, total, visit_limit_reached);
	if (status == EditorSelector::Status::NO_MATCH) {
		// A handle that is present in the capture resolved: some other constraint is what failed, and
		// reporting that as a stale handle would be a wrong verdict.
		for (int i = 0; i < query_handles.size(); i++) {
			if (find_by_handle(cached_snapshot.roots, query_handles[i]) == nullptr) {
				return find_payload(false, EditorSelector::status_name(EditorSelector::Status::STALE_HANDLE),
						"Handle '" + query_handles[i] + "' no longer resolves to a captured element.",
						cached_snapshot.generation, 0, offset, limit, visit_limit_reached, String(), Array());
			}
		}
		return find_payload(false, EditorSelector::status_name(EditorSelector::Status::NO_MATCH),
				"No element matched the selector.", cached_snapshot.generation, 0, offset, limit, visit_limit_reached,
				String(), Array());
	}
	// A truncated walk, or a capture that omitted elements before matching began, only ever counts a
	// lower bound, so uniqueness cannot be established from either.
	const bool snapshot_truncated = cached_snapshot.depth_truncated || cached_snapshot.element_limit_reached ||
			cached_snapshot.traversal_limit_reached;
	if (require_unique && (total > 1 || visit_limit_reached || snapshot_truncated)) {
		const String reason = total > 1
				? "The selector matched " + String::num_int64(total) + " elements where exactly one is required."
				: "The capture was truncated, so the selector cannot be shown to match exactly one element. "
				  "Raise max_depth or max_elements and try again.";
		return find_payload(false, EditorSelector::status_name(EditorSelector::Status::AMBIGUOUS_SELECTOR), reason,
				cached_snapshot.generation, total, offset, limit, visit_limit_reached || snapshot_truncated, String(),
				Array());
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

namespace {

// The comparable form of one element: everything it publishes except the element id, which is scoped
// to the capture that produced it and would otherwise report every surviving element as changed.
String comparable_element(const EditorElement &p_element) {
	Dictionary serialized = EditorSnapshot::serialize_element(p_element, false);
	serialized.erase("id");
	return JSON::stringify(serialized, "", false);
}

} // namespace

bool EditorAutomationService::_resolve_target(const Dictionary &p_arguments, const EditorActionRequest &p_request,
		EditorSnapshotData &r_data, const EditorElement **r_element, Dictionary &r_failure, String &r_error,
		String &r_message) {
	*r_element = nullptr;
	r_failure = Dictionary();

	if (!p_arguments.has("selector")) {
		r_failure = EditorActionDriver::failure(EditorActionDriver::Status::INVALID_SELECTOR,
				"act_on_editor_ui requires a selector; an absent selector never names an element.", p_request.action);
		return false;
	}
	EditorSelectorQuery query;
	String selector_message;
	if (!EditorSelector::parse(p_arguments.get("selector", Variant()), query, selector_message)) {
		r_failure = EditorActionDriver::failure(
				EditorActionDriver::Status::INVALID_SELECTOR, selector_message, p_request.action);
		return false;
	}

	// The registry is consulted before this request captures anything. Capturing first would register
	// every live handle, so a handle Barista had never issued would be admitted by the very request
	// that supplied it, and the class recorded for a reused instance id would be overwritten before it
	// could be compared.
	const PackedStringArray query_handles = EditorSelector::handles(query);
	std::vector<String> previous_classes;
	for (int i = 0; i < query_handles.size(); i++) {
		uint64_t instance_id = 0;
		if (!parse_handle(query_handles[i], instance_id)) {
			r_failure = EditorActionDriver::failure(EditorActionDriver::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' was not issued by Barista.", p_request.action);
			return false;
		}
		const auto issued = issued_handles.find(instance_id);
		if (issued == issued_handles.end()) {
			r_failure = EditorActionDriver::failure(EditorActionDriver::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' was not issued by Barista.", p_request.action);
			return false;
		}
		previous_classes.push_back(issued->second.class_name);
	}

	Dictionary payload;
	if (!_capture(parse_options(p_arguments), r_data, payload, r_error, r_message)) {
		return false;
	}
	// Every failure discovered from here on was decided against this capture, so it names it.
	const int64_t decided_generation = (int64_t)r_data.generation;

	for (int i = 0; i < query_handles.size(); i++) {
		const EditorElement *resolved = find_by_handle(r_data.roots, query_handles[i]);
		if (resolved != nullptr && resolved->class_name != previous_classes[(size_t)i]) {
			r_failure = EditorActionDriver::failure(EditorActionDriver::Status::STALE_HANDLE,
					"Handle '" + query_handles[i] + "' now refers to a different object type.", p_request.action);
			r_failure["generation"] = decided_generation;
			return false;
		}
	}

	std::vector<const EditorElement *> page;
	int total = 0;
	bool visit_limit_reached = false;
	// Two matches are enough to prove ambiguity, so the match walk is never asked for a whole page.
	const EditorSelector::Status status = EditorSelector::match(r_data, query, 0, 2, page, total, visit_limit_reached);
	if (status == EditorSelector::Status::NO_MATCH) {
		for (int i = 0; i < query_handles.size(); i++) {
			if (find_by_handle(r_data.roots, query_handles[i]) == nullptr) {
				r_failure = EditorActionDriver::failure(EditorActionDriver::Status::STALE_HANDLE,
						"Handle '" + query_handles[i] + "' no longer resolves to a captured element.",
						p_request.action);
				r_failure["generation"] = decided_generation;
				return false;
			}
		}
		r_failure = EditorActionDriver::failure(
				EditorActionDriver::Status::NO_MATCH, "No element matched the selector.", p_request.action);
		r_failure["generation"] = decided_generation;
		return false;
	}
	// A truncated walk, or a capture that omitted elements before matching began, only ever counts a
	// lower bound, so a single match cannot be shown to be the only one.
	const bool snapshot_truncated =
			r_data.depth_truncated || r_data.element_limit_reached || r_data.traversal_limit_reached;
	if (total > 1 || visit_limit_reached || snapshot_truncated) {
		const String reason = total > 1
				? "The selector matched " + String::num_int64(total) +
						" elements where exactly one is required; an action never picks the first."
				: "The capture was truncated, so the selector cannot be shown to name exactly one element.";
		r_failure =
				EditorActionDriver::failure(EditorActionDriver::Status::AMBIGUOUS_SELECTOR, reason, p_request.action);
		r_failure["generation"] = decided_generation;
		return false;
	}
	if (page.empty()) {
		r_failure = EditorActionDriver::failure(
				EditorActionDriver::Status::NO_MATCH, "No element matched the selector.", p_request.action);
		r_failure["generation"] = decided_generation;
		return false;
	}
	*r_element = page[0];
	return true;
}

// Bounds the trace one action publishes. A trace is a diagnostic, never a second contract, so it is
// capped in both entry count and entry length before it can reach a payload.
namespace {

constexpr int MAX_TRACE_ENTRIES = 16;
constexpr int MAX_TRACE_LENGTH = 200;

void push_trace(std::vector<String> &r_trace, const String &p_entry) {
	if ((int)r_trace.size() >= MAX_TRACE_ENTRIES) {
		return;
	}
	r_trace.push_back(p_entry.length() <= MAX_TRACE_LENGTH ? p_entry : p_entry.substr(0, MAX_TRACE_LENGTH));
}

} // namespace

Dictionary EditorAutomationService::act_ui(const Dictionary &p_arguments, String &r_error, String &r_message) {
	// The action name was already checked against the advertised vocabulary at the request boundary,
	// so recording it here can only ever record an advertised action or nothing at all.
	const Variant action_value = p_arguments.get("action", Variant());
	const String requested_action = action_value.get_type() == Variant::STRING ? String(action_value) : String();
	event_log.record(EditorEventLog::TYPE_ACTION_BEGIN, requested_action, String());

	std::vector<String> trace;
	Dictionary payload = _act_ui(p_arguments, trace, r_error, r_message);
	if (!r_error.is_empty()) {
		event_log.record(EditorEventLog::TYPE_ACTION_END, requested_action, r_error);
		return payload;
	}
	event_log.record(EditorEventLog::TYPE_ACTION_END, requested_action, payload.get("status", String()));
	if (!(bool)payload.get("ok", false)) {
		Array entries;
		for (const String &entry : trace) {
			entries.push_back(entry);
		}
		payload["trace"] = entries;
	}
	return payload;
}

Dictionary EditorAutomationService::_act_ui(
		const Dictionary &p_arguments, std::vector<String> &r_trace, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();

	// The gate is entry-time state frozen at startup, so it is read before anything else can capture
	// or mutate.
	if (!automation_enabled) {
		push_trace(r_trace, "gate: automation disabled");
		return EditorActionDriver::failure(EditorActionDriver::Status::AUTOMATION_DISABLED,
				"Editor automation is disabled in this session; no action was performed.");
	}
	push_trace(r_trace, "gate: automation enabled");

	EditorActionRequest request;
	String parse_message;
	if (!EditorActionDriver::parse(p_arguments, request, parse_message)) {
		push_trace(r_trace, "parse: rejected");
		return EditorActionDriver::failure(EditorActionDriver::Status::INVALID_ARGUMENTS, parse_message);
	}
	push_trace(r_trace, "parse: action=" + request.action);

	EditorSnapshotData before;
	const EditorElement *element = nullptr;
	Dictionary failure;
	if (!_resolve_target(p_arguments, request, before, &element, failure, r_error, r_message)) {
		push_trace(r_trace, "resolve: " + (r_error.is_empty() ? String(failure.get("status", String())) : r_error));
		return r_error.is_empty() ? failure : Dictionary();
	}
	push_trace(r_trace, "resolve: handle=" + element->handle + " class=" + element->class_name);

	// The element proves a live node with this instance id is in the editor tree, so the lookup below
	// can only return that node; the recorded class is re-checked all the same.
	uint64_t instance_id = 0;
	Object *object =
			parse_handle(element->handle, instance_id) ? UtilityFunctions::instance_from_id(instance_id) : nullptr;
	Control *control = Object::cast_to<Control>(object);
	if (object == nullptr || object->get_class() != element->class_name || control == nullptr) {
		push_trace(r_trace, "bind: element is no longer an interactable control");
		Dictionary payload = EditorActionDriver::failure(EditorActionDriver::Status::ELEMENT_NOT_INTERACTABLE,
				"The element is no longer an interactable control.", request.action);
		payload["handle"] = element->handle;
		payload["generation"] = (int64_t)before.generation;
		return payload;
	}

	const String handle = element->handle;
	const String before_element = comparable_element(*element);

	EditorActionDriver::Route route = EditorActionDriver::Route::NONE;
	EditorActionDriver::Status status = EditorActionDriver::Status::OK;
	String action_message;
	if (!EditorActionDriver::perform(control, *element, request, route, status, action_message)) {
		push_trace(r_trace,
				"perform: " + EditorActionDriver::status_name(status) +
						" route=" + EditorActionDriver::route_name(route));
		Dictionary payload = EditorActionDriver::failure(status, action_message, request.action);
		payload["handle"] = handle;
		payload["generation"] = (int64_t)before.generation;
		return payload;
	}

	push_trace(r_trace, "perform: ok route=" + EditorActionDriver::route_name(route));

	// The result names the snapshot taken after the action, never the one the action was decided on.
	EditorSnapshotData after;
	Dictionary after_payload;
	if (!_capture(parse_options(p_arguments), after, after_payload, r_error, r_message)) {
		return Dictionary();
	}
	const EditorElement *after_element = find_by_handle(after.roots, handle);

	Dictionary payload;
	payload["ok"] = true;
	payload["status"] = EditorActionDriver::status_name(EditorActionDriver::Status::OK);
	payload["message"] = String();
	payload["action"] = request.action;
	// The claim this route was held to travels with the result, so an agent reads from the same payload
	// whether "ok" means the requested state was verified or only that the input reached the target.
	payload["claim"] = MCPContracts::action_claim(request.action);
	payload["route"] = EditorActionDriver::route_name(route);
	// "changed" is only ever what Barista can verify: whether the acted element's own published fields
	// differ between the capture the action was decided on and the capture taken after it. An action
	// whose effect lands elsewhere in the editor reports false rather than claiming a change.
	if (after_element != nullptr) {
		payload["changed"] = comparable_element(*after_element) != before_element;
		payload["element"] = EditorSnapshot::serialize_element(*after_element, false);
	} else {
		payload["changed"] = true;
	}
	payload["generation"] = (int64_t)after.generation;
	payload["handle"] = handle;
	return payload;
}

namespace {

using OperationStatus = EditorStateReader::OperationStatus;
using PathStatus = EditorStateReader::PathStatus;

// Bounds every string an operation result publishes, so no client-supplied path can inflate a
// payload beyond the published transport budget.
String bounded_text(const String &p_value) {
	const int limit = EditorSnapshotLimits::MAX_PATH_LENGTH;
	return p_value.length() <= limit ? p_value : p_value.substr(0, limit);
}

String bool_text(bool p_value) {
	return p_value ? "true" : "false";
}

bool contains_path(const PackedStringArray &p_values, const String &p_path) {
	for (int i = 0; i < p_values.size(); i++) {
		if (p_values[i] == p_path) {
			return true;
		}
	}
	return false;
}

// A wait_for_editor condition that observes the same predicate, for the operations whose predicate
// one can express. Where none can, the client polls read_editor_state instead and nothing is
// published rather than a condition that would observe something else.
Dictionary play_state_condition(bool p_playing) {
	Dictionary condition;
	condition["type"] = EditorWaitManager::CONDITION_PLAY_STATE;
	condition["playing"] = p_playing;
	return condition;
}

// The one shape every performed operation answers in. "ok" is reported only where the published
// postcondition was read back and held; everything else is an observable pending state.
Dictionary operation_outcome(const String &p_operation, const String &p_expected, const String &p_observed,
		bool p_satisfied, bool p_changed, const Dictionary *p_wait_condition) {
	Dictionary observable;
	observable["field"] = EditorStateReader::operation_observable_field(p_operation);
	observable["expected"] = bounded_text(p_expected);
	observable["observed"] = bounded_text(p_observed);
	observable["satisfied"] = p_satisfied;
	if (p_wait_condition != nullptr) {
		observable["wait_condition"] = *p_wait_condition;
	}

	Dictionary payload;
	payload["ok"] = p_satisfied;
	payload["status"] =
			EditorStateReader::operation_status_name(p_satisfied ? OperationStatus::OK : OperationStatus::PENDING);
	payload["message"] = p_satisfied
			? String()
			: String("The editor accepted the request, but the requested state was not observable yet. "
					 "Observe 'observable.field' rather than treating this as a success.");
	payload["operation"] = p_operation;
	payload["claim"] = MCPContracts::operation_claim(p_operation);
	payload["changed"] = p_changed;
	payload["pending"] = !p_satisfied;
	payload["observable"] = observable;
	return payload;
}

String unsaved_text(bool p_unsaved, const String &p_path) {
	return (p_unsaved ? String("includes ") : String("excludes ")) + p_path;
}

} // namespace

Dictionary EditorAutomationService::run_operation(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();

	// The gate is entry-time state frozen at startup, so it is read before anything else can read or
	// mutate the editor. A disabled session names no operation, so it declares no claim either.
	if (!automation_enabled) {
		return EditorStateReader::operation_failure(OperationStatus::AUTOMATION_DISABLED,
				"Editor automation is disabled in this session; no operation was performed.");
	}

	EditorOperationRequest request;
	String parse_message;
	if (!EditorStateReader::parse_operation(p_arguments, request, parse_message)) {
		return EditorStateReader::operation_failure(OperationStatus::INVALID_ARGUMENTS, parse_message);
	}

	event_log.record(EditorEventLog::TYPE_ACTION_BEGIN, request.operation, String());
	const Dictionary payload = _run_operation(request);
	event_log.record(EditorEventLog::TYPE_ACTION_END, request.operation, payload.get("status", String()));
	return payload;
}

Dictionary EditorAutomationService::_run_operation(const EditorOperationRequest &p_request) {
	if (editor_interface == nullptr) {
		return EditorStateReader::operation_failure(OperationStatus::UNSUPPORTED_CAPABILITY,
				"The editor interface is unavailable in this session.", p_request.operation);
	}

	// Every observable this request is decided against is read here, before any branch that can
	// mutate, so nothing below validates the request against state the request itself created.
	const String entry_scene = EditorStateReader::current_scene_path(editor_interface);
	const PackedStringArray entry_open = EditorStateReader::open_scene_paths(editor_interface);
	const PackedStringArray entry_unsaved = EditorStateReader::unsaved_scene_paths(editor_interface);
	const bool entry_playing = editor_interface->is_playing_scene();
	const String entry_playing_scene = editor_interface->get_playing_scene();
	const String entry_filesystem_path = EditorStateReader::current_filesystem_path(editor_interface);
	const String entry_script = EditorStateReader::current_script_path(editor_interface);
	const String entry_screen = EditorStateReader::current_main_screen(editor_interface);
	const PackedStringArray screens = EditorStateReader::main_screen_names(editor_interface);

	// One validator, run by every operation that takes a path, before any EditorInterface call. A
	// rejection here has touched nothing.
	if (p_request.has_path) {
		String path_message;
		const PathStatus path_status = EditorStateReader::check_resource_path(
				p_request.path, EditorStateReader::operation_path_type(p_request.operation), path_message);
		if (path_status == PathStatus::INVALID_PATH) {
			return EditorStateReader::operation_failure(
					OperationStatus::INVALID_RESOURCE_PATH, path_message, p_request.operation);
		}
		if (path_status == PathStatus::WRONG_TYPE) {
			return EditorStateReader::operation_failure(
					OperationStatus::INVALID_RESOURCE_TYPE, path_message, p_request.operation);
		}
	}

	const String operation = p_request.operation;

	if (operation == "save_scene") {
		if (entry_scene.is_empty()) {
			return EditorStateReader::operation_failure(OperationStatus::OPERATION_FAILED,
					"No scene with a res:// path is being edited, so there is nothing to save.", operation);
		}
		const bool was_unsaved = contains_path(entry_unsaved, entry_scene);
		const Error result = editor_interface->save_scene();
		if (result != OK) {
			return EditorStateReader::operation_failure(OperationStatus::OPERATION_FAILED,
					"The editor refused to save the edited scene (error " + String::num_int64((int64_t)result) + ").",
					operation);
		}
		const bool still_unsaved = contains_path(EditorStateReader::unsaved_scene_paths(editor_interface), entry_scene);
		return operation_outcome(operation, unsaved_text(false, entry_scene), unsaved_text(still_unsaved, entry_scene),
				!still_unsaved, was_unsaved && !still_unsaved, nullptr);
	}

	if (operation == "save_all_scenes") {
		editor_interface->save_all_scenes();
		const PackedStringArray after = EditorStateReader::unsaved_scene_paths(editor_interface);
		int remaining = 0;
		for (int i = 0; i < entry_unsaved.size(); i++) {
			if (contains_path(after, entry_unsaved[i])) {
				remaining++;
			}
		}
		return operation_outcome(operation, "excludes every scene that was unsaved at entry",
				"still unsaved: " + String::num_int64(remaining), remaining == 0, entry_unsaved.size() > remaining,
				nullptr);
	}

	if (operation == "open_scene") {
		editor_interface->open_scene_from_path(p_request.path, false);
		const String current = EditorStateReader::current_scene_path(editor_interface);
		return operation_outcome(
				operation, p_request.path, current, current == p_request.path, current != entry_scene, nullptr);
	}

	if (operation == "reload_scene") {
		if (!contains_path(entry_open, p_request.path)) {
			return EditorStateReader::operation_failure(OperationStatus::CONFLICTING_STATE,
					"'" + bounded_text(p_request.path) + "' is not open, so there is nothing to reload.", operation);
		}
		const bool was_unsaved = contains_path(entry_unsaved, p_request.path);
		editor_interface->reload_scene_from_path(p_request.path);
		const bool still_unsaved =
				contains_path(EditorStateReader::unsaved_scene_paths(editor_interface), p_request.path);
		return operation_outcome(operation, unsaved_text(false, p_request.path),
				unsaved_text(still_unsaved, p_request.path), !still_unsaved, was_unsaved && !still_unsaved, nullptr);
	}

	if (operation == "play_current_scene" || operation == "play_main_scene" || operation == "play_scene") {
		// Playing is not idempotent, so entry-time play state decides whether it may run at all. A
		// second play is refused rather than performed twice.
		if (entry_playing) {
			if (operation == "play_scene" && entry_playing_scene == p_request.path) {
				const Dictionary condition = play_state_condition(true);
				return operation_outcome(operation, p_request.path, entry_playing_scene, true, false, &condition);
			}
			return EditorStateReader::operation_failure(OperationStatus::CONFLICTING_STATE,
					"The editor is already playing '" + bounded_text(entry_playing_scene) +
							"'; stop it before starting another scene.",
					operation);
		}
		if (operation == "play_scene") {
			editor_interface->play_custom_scene(p_request.path);
		} else if (operation == "play_main_scene") {
			editor_interface->play_main_scene();
		} else {
			if (entry_scene.is_empty()) {
				return EditorStateReader::operation_failure(OperationStatus::OPERATION_FAILED,
						"No scene with a res:// path is being edited, so there is nothing to play.", operation);
			}
			editor_interface->play_current_scene();
		}
		const Dictionary condition = play_state_condition(true);
		if (operation == "play_scene") {
			const String playing = editor_interface->get_playing_scene();
			return operation_outcome(operation, p_request.path, playing, playing == p_request.path,
					playing != entry_playing_scene, &condition);
		}
		const bool playing = editor_interface->is_playing_scene();
		return operation_outcome(
				operation, bool_text(true), bool_text(playing), playing, playing != entry_playing, &condition);
	}

	if (operation == "stop_play") {
		const Dictionary condition = play_state_condition(false);
		// Stopping what is not playing is a no-op that is already in the requested state, so the
		// editor is never asked to stop twice.
		if (!entry_playing) {
			return operation_outcome(operation, bool_text(false), bool_text(false), true, false, &condition);
		}
		editor_interface->stop_playing_scene();
		const bool playing = editor_interface->is_playing_scene();
		return operation_outcome(operation, bool_text(false), bool_text(playing), !playing, !playing, &condition);
	}

	if (operation == "select_file") {
		editor_interface->select_file(p_request.path);
		const String current = EditorStateReader::current_filesystem_path(editor_interface);
		return operation_outcome(operation, p_request.path, current, current == p_request.path,
				current != entry_filesystem_path, nullptr);
	}

	if (operation == "switch_main_screen") {
		if (screens.is_empty()) {
			return EditorStateReader::operation_failure(OperationStatus::UNSUPPORTED_CAPABILITY,
					"This editor build shows no main screens, so none can be switched to.", operation);
		}
		if (!screens.has(p_request.screen)) {
			return EditorStateReader::operation_failure(OperationStatus::INVALID_ARGUMENTS,
					"'" + bounded_text(p_request.screen) +
							"' is not one of the main screens this editor shows; read "
							"'main_screen.available' from read_editor_state.",
					operation);
		}
		editor_interface->set_main_screen_editor(p_request.screen);
		const String current = EditorStateReader::current_main_screen(editor_interface);
		return operation_outcome(
				operation, p_request.screen, current, current == p_request.screen, current != entry_screen, nullptr);
	}

	if (operation == "edit_script") {
		ScriptEditor *script_editor = editor_interface->get_script_editor();
		if (script_editor == nullptr) {
			return EditorStateReader::operation_failure(OperationStatus::UNSUPPORTED_CAPABILITY,
					"This session has no script editor, so no script can be opened.", operation);
		}
		ResourceLoader *loader = ResourceLoader::get_singleton();
		Ref<Script> script = loader == nullptr ? Ref<Script>() : Ref<Script>(loader->load(p_request.path, "Script"));
		if (script.is_null()) {
			return EditorStateReader::operation_failure(OperationStatus::OPERATION_FAILED,
					"'" + bounded_text(p_request.path) + "' could not be loaded as a script.", operation);
		}
		editor_interface->edit_script(script, p_request.has_line ? p_request.line : -1,
				p_request.has_column ? p_request.column : 0, p_request.grab_focus);
		const String current = EditorStateReader::current_script_path(editor_interface);
		return operation_outcome(
				operation, p_request.path, current, current == p_request.path, current != entry_script, nullptr);
	}

	// Every advertised operation is handled above. An operation that reached here is advertised
	// without an implementation, which must fail closed rather than answer with another one's result.
	return EditorStateReader::operation_failure(OperationStatus::UNSUPPORTED_CAPABILITY,
			"Operation '" + operation + "' has no implementation in this build.", operation);
}

namespace {

// Every handle a wait condition names, across both of its selectors.
PackedStringArray condition_handles(const EditorWaitCondition &p_condition) {
	PackedStringArray handles;
	if (p_condition.selector != nullptr) {
		handles.append_array(EditorSelector::handles(*p_condition.selector));
	}
	if (p_condition.state != nullptr) {
		handles.append_array(EditorSelector::handles(*p_condition.state));
	}
	return handles;
}

} // namespace

Dictionary EditorAutomationService::wait_for_editor(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();

	Time *time = Time::get_singleton();
	const uint64_t now_ms = time == nullptr ? 0 : time->get_ticks_msec();

	const bool has_condition = p_arguments.has("condition");
	const bool has_wait_id = p_arguments.has("wait_id");
	if (has_condition == has_wait_id) {
		return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS,
				"wait_for_editor takes exactly one of 'condition', to start a wait, and 'wait_id', to poll or "
				"cancel one.");
	}

	if (has_wait_id) {
		if (p_arguments.has("timeout_ms")) {
			return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS,
					"'timeout_ms' belongs to a start request; a wait keeps the deadline it was created with.");
		}
		const Variant id_value = p_arguments.get("wait_id", Variant());
		if (id_value.get_type() != Variant::STRING) {
			return wait_manager.failure(
					EditorWaitManager::Status::INVALID_ARGUMENTS, "'wait_id' must be a string this server issued.");
		}
		const bool cancel_requested = p_arguments.has("cancel") && (bool)p_arguments.get("cancel", false);
		// Both routes read a wait id this server issued earlier; neither is validated against anything
		// the current request created.
		return cancel_requested ? wait_manager.cancel(String(id_value), now_ms)
								: wait_manager.poll(String(id_value), now_ms);
	}

	if (p_arguments.has("cancel")) {
		return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS,
				"'cancel' names an existing wait id; a request that starts a wait never cancels one.");
	}

	EditorWaitCondition condition;
	String condition_message;
	if (!EditorWaitManager::parse_condition(p_arguments.get("condition", Variant()), condition, condition_message)) {
		return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS, condition_message);
	}

	int timeout_ms = EditorWaitLimits::DEFAULT_TIMEOUT_MS;
	if (p_arguments.has("timeout_ms")) {
		// The advertised range is enforced by the boundary schema, so this conversion cannot narrow.
		timeout_ms = clamp_int((int64_t)p_arguments.get("timeout_ms", timeout_ms), EditorWaitLimits::MIN_TIMEOUT_MS,
				EditorWaitLimits::MAX_TIMEOUT_MS);
	}
	if (condition.type == EditorWaitManager::CONDITION_FILESYSTEM_SETTLES && condition.require_start &&
			condition.prime_ms > timeout_ms) {
		return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS,
				"'prime_ms' must not exceed 'timeout_ms'; a prime window longer than the deadline could never "
				"be observed.");
	}

	// The issued-handle registry is consulted before this request captures anything, so a handle
	// Barista never issued can never be admitted by the very request that supplied it.
	const PackedStringArray handles = condition_handles(condition);
	for (int i = 0; i < handles.size(); i++) {
		uint64_t instance_id = 0;
		if (!parse_handle(handles[i], instance_id) || issued_handles.find(instance_id) == issued_handles.end()) {
			return wait_manager.failure(EditorWaitManager::Status::INVALID_ARGUMENTS,
					"Handle '" + handles[i] + "' was not issued by Barista.");
		}
	}

	EditorWaitContext context;
	EditorSnapshotData snapshot;
	_build_wait_context(true, now_ms, context, snapshot);
	return wait_manager.start(std::move(condition), timeout_ms, context);
}

Dictionary EditorAutomationService::poll_events(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();
	int limit = EditorEventLimits::DEFAULT_LIMIT;
	if (p_arguments.has("limit")) {
		limit = clamp_int(
				(int64_t)p_arguments.get("limit", limit), EditorEventLimits::MIN_LIMIT, EditorEventLimits::MAX_LIMIT);
	}
	const bool has_marker = p_arguments.has("marker");
	const int64_t marker = has_marker ? (int64_t)p_arguments.get("marker", Variant((int64_t)0)) : 0;
	return event_log.poll(has_marker, marker, limit);
}

void EditorAutomationService::_build_wait_context(
		bool p_capture, uint64_t p_now_ms, EditorWaitContext &r_context, EditorSnapshotData &r_snapshot) {
	r_context.now_ms = p_now_ms;
	if (editor_interface == nullptr) {
		return;
	}
	r_context.is_playing = editor_interface->is_playing_scene();
	EditorFileSystem *file_system = editor_interface->get_resource_filesystem();
	if (file_system != nullptr) {
		r_context.filesystem_available = true;
		// Importing is part of settling: a scan that has finished walking but is still importing has
		// not left the editor in a state an agent can act against.
		r_context.filesystem_busy = file_system->is_scanning() || file_system->is_importing();
	}
	if (!p_capture) {
		return;
	}
	// Wait evaluation must see every element a client can name. A selector reaches internal children
	// and the full published depth through inspect_editor_ui and find_editor_ui, so a wait judged
	// against a narrower capture would decide presence, absence, or uniqueness over a domain that
	// never contained the element the client asked about. The widest published options are used
	// here for the same reason read_element uses them, and truncation still guards every verdict that
	// needs an exhaustive walk. It deliberately does not go through _capture: the public generation,
	// the issued-handle registry, and the cached capture a cursor resumes against are request-owned
	// state that a background frame must never move, and no wait capture is ever serialized, so the
	// payload budget that shrinks a request capture does not apply.
	EditorSnapshotOptions options;
	options.max_depth = EditorSnapshotLimits::MAX_MAX_DEPTH;
	options.max_elements = EditorSnapshotLimits::MAX_MAX_ELEMENTS;
	options.include_internal = true;
	if (!EditorSnapshot::capture(editor_interface, 0, options, r_snapshot)) {
		return;
	}
	r_snapshot.requested_options = options;
	r_snapshot.applied_options = options;
	r_context.has_snapshot = true;
	r_context.snapshot = &r_snapshot;
}

void EditorAutomationService::process(double p_delta) {
	(void)p_delta;
	// No wait outstanding means no reading at all, so an idle editor pays nothing for this feature.
	if (!wait_manager.has_waits()) {
		return;
	}
	Time *time = Time::get_singleton();
	const uint64_t now_ms = time == nullptr ? 0 : time->get_ticks_msec();
	EditorWaitContext context;
	EditorSnapshotData snapshot;
	_build_wait_context(wait_manager.needs_snapshot(now_ms), now_ms, context, snapshot);
	wait_manager.process(context);
}

void EditorAutomationService::shutdown() {
	// Every handle is cancelled and cleared before anything else is released, so a shutdown leaves no
	// wait behind for a later frame or a later request to find.
	wait_manager.shutdown();
	event_log.clear();
	editor_interface = nullptr;
	issued_handles.clear();
	cached_snapshot = EditorSnapshotData();
	has_cached_snapshot = false;
	cached_selector = String();
}

} // namespace godot
