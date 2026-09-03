/**************************************************************************/
/*  editor_event_log.cpp                                                  */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_event_log.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

String bounded(const String &p_value) {
	if (p_value.length() <= EditorEventLimits::MAX_STRING_LENGTH) {
		return p_value;
	}
	return p_value.substr(0, EditorEventLimits::MAX_STRING_LENGTH);
}

// Copies at most MAX_DETAIL_ENTRIES entries, bounding every string value. Anything else is published
// as its own type, so a detail entry can never carry a nested structure of unbounded size.
Dictionary bounded_detail(const Dictionary &p_detail) {
	Dictionary result;
	const Array keys = p_detail.keys();
	for (int i = 0; i < keys.size() && result.size() < EditorEventLimits::MAX_DETAIL_ENTRIES; i++) {
		const String key = keys[i];
		const Variant value = p_detail.get(keys[i], Variant());
		switch (value.get_type()) {
			case Variant::STRING:
				result[bounded(key)] = bounded(value);
				break;
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
				result[bounded(key)] = value;
				break;
			default:
				result[bounded(key)] = bounded(String(value));
				break;
		}
	}
	return result;
}

Dictionary serialize_event(const EditorEvent &p_event) {
	Dictionary entry;
	entry["index"] = (int64_t)p_event.index;
	entry["time_ms"] = (int64_t)p_event.time_ms;
	entry["type"] = p_event.type;
	entry["operation"] = p_event.operation;
	entry["outcome"] = p_event.outcome;
	entry["detail"] = p_event.detail;
	return entry;
}

} // namespace

PackedStringArray EditorEventLog::status_vocabulary() {
	PackedStringArray statuses;
	statuses.push_back("ok");
	statuses.push_back("marker_expired");
	statuses.push_back("invalid_marker");
	return statuses;
}

PackedStringArray EditorEventLog::type_vocabulary() {
	PackedStringArray types;
	types.push_back(TYPE_LIFECYCLE);
	types.push_back(TYPE_ACTION_BEGIN);
	types.push_back(TYPE_ACTION_END);
	types.push_back(TYPE_WAIT_BEGIN);
	types.push_back(TYPE_WAIT_END);
	return types;
}

String EditorEventLog::status_name(Status p_status) {
	switch (p_status) {
		case Status::OK:
			return "ok";
		case Status::MARKER_EXPIRED:
			return "marker_expired";
		case Status::INVALID_MARKER:
			return "invalid_marker";
	}
	return "invalid_marker";
}

Dictionary EditorEventLog::limits() {
	Dictionary published;
	published["max_events"] = EditorEventLimits::MAX_EVENTS;
	published["max_limit"] = EditorEventLimits::MAX_LIMIT;
	published["default_limit"] = EditorEventLimits::DEFAULT_LIMIT;
	return published;
}

uint64_t EditorEventLog::record(
		const String &p_type, const String &p_operation, const String &p_outcome, const Dictionary &p_detail) {
	EditorEvent event;
	event.index = next_index++;
	Time *time = Time::get_singleton();
	event.time_ms = time == nullptr ? 0 : time->get_ticks_msec();
	event.type = bounded(p_type);
	event.operation = bounded(p_operation);
	event.outcome = bounded(p_outcome);
	event.detail = bounded_detail(p_detail);
	events.push_back(event);
	while ((int)events.size() > EditorEventLimits::MAX_EVENTS) {
		events.pop_front();
		dropped++;
	}
	return event.index;
}

int64_t EditorEventLog::next_marker() const {
	return (int64_t)next_index;
}

Dictionary EditorEventLog::poll(bool p_has_marker, int64_t p_marker, int p_limit) const {
	const int64_t earliest = events.empty() ? (int64_t)next_index : (int64_t)events.front().index;
	const int64_t latest = (int64_t)next_index;

	Dictionary payload;
	payload["earliest_marker"] = earliest;
	payload["latest_marker"] = latest;
	payload["dropped"] = (int64_t)dropped;
	payload["limit"] = p_limit;
	payload["limits"] = limits();

	// A marker the server never issued is not a marker. Only [earliest, latest] can name a page, and
	// anything below earliest names events the ring has already evicted.
	const int64_t start = p_has_marker ? p_marker : earliest;
	if (start > latest) {
		payload["ok"] = false;
		payload["status"] = status_name(Status::INVALID_MARKER);
		payload["message"] = "Marker " + String::num_int64(start) + " was never issued by this server session.";
		payload["marker"] = latest;
		payload["count"] = 0;
		payload["has_more"] = false;
		payload["events"] = Array();
		return payload;
	}
	if (start < earliest) {
		payload["ok"] = false;
		payload["status"] = status_name(Status::MARKER_EXPIRED);
		payload["message"] = "Marker " + String::num_int64(start) +
				" named events this bounded ring has already dropped; resume from the earliest marker.";
		payload["marker"] = earliest;
		payload["count"] = 0;
		payload["has_more"] = earliest < latest;
		payload["events"] = Array();
		return payload;
	}

	Array page;
	int64_t next = start;
	for (const EditorEvent &event : events) {
		if ((int64_t)event.index < start) {
			continue;
		}
		if ((int)page.size() >= p_limit) {
			break;
		}
		page.push_back(serialize_event(event));
		next = (int64_t)event.index + 1;
	}
	payload["ok"] = true;
	payload["status"] = status_name(Status::OK);
	payload["message"] = String();
	payload["marker"] = next;
	payload["count"] = (int)page.size();
	payload["has_more"] = next < latest;
	payload["events"] = page;
	return payload;
}

void EditorEventLog::clear() {
	events.clear();
	dropped = 0;
}

} // namespace godot
