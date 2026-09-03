/**************************************************************************/
/*  editor_event_log.h                                                    */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_EVENT_LOG_H
#define BARISTA_MCP_EDITOR_EVENT_LOG_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <deque>

namespace godot {

// Bounds the Barista-owned event ring and every page read out of it. Storage is fixed: the ring
// evicts its oldest entry rather than growing, and a marker naming an evicted entry is answered with
// an explicit expiry and the earliest marker still available.
struct EditorEventLimits {
	static constexpr int MAX_EVENTS = 256;
	static constexpr int MIN_LIMIT = 1;
	static constexpr int MAX_LIMIT = 100;
	static constexpr int DEFAULT_LIMIT = 50;
	// Longest published operation, outcome, and detail string. Longer values are truncated before
	// they enter the ring, so one pathological record cannot crowd out the rest of a page.
	static constexpr int MAX_STRING_LENGTH = 200;
	// Most detail entries one event may carry.
	static constexpr int MAX_DETAIL_ENTRIES = 8;
	// The largest marker a client may name. Indices are issued from 1 and one session cannot reach
	// this bound, but the advertised range is explicit so an unrepresentable JSON number is rejected
	// at the boundary rather than converted.
	static constexpr int64_t MAX_MARKER = 9007199254740992LL;
};

// One Barista-owned event. It describes only what Barista itself did or observed through public
// APIs; it never claims to mirror the internal Godot editor log.
struct EditorEvent {
	uint64_t index = 0;
	uint64_t time_ms = 0;
	String type;
	String operation;
	String outcome;
	Dictionary detail;
};

// Single source of truth for event indices, ring capacity, and the marker vocabulary. Indices are
// monotonic for the life of one server session, and a page never repeats an event for a valid marker.
class EditorEventLog {
public:
	enum class Status {
		OK,
		MARKER_EXPIRED,
		INVALID_MARKER,
	};

	static constexpr const char *TYPE_LIFECYCLE = "lifecycle";
	static constexpr const char *TYPE_ACTION_BEGIN = "action_begin";
	static constexpr const char *TYPE_ACTION_END = "action_end";
	static constexpr const char *TYPE_WAIT_BEGIN = "wait_begin";
	static constexpr const char *TYPE_WAIT_END = "wait_end";

	static PackedStringArray status_vocabulary();
	static PackedStringArray type_vocabulary();
	static String status_name(Status p_status);

	// Appends one bounded event and returns the index it was issued. Strings and detail entries are
	// bounded here, so nothing that reaches the ring can be larger than the published limits.
	uint64_t record(const String &p_type, const String &p_operation, const String &p_outcome,
			const Dictionary &p_detail = Dictionary());
	// Reads one bounded page. p_has_marker false starts at the earliest event still stored.
	Dictionary poll(bool p_has_marker, int64_t p_marker, int p_limit) const;
	// The marker that names the next event this log will issue.
	int64_t next_marker() const;
	void clear();

	static Dictionary limits();

private:
	std::deque<EditorEvent> events;
	uint64_t next_index = 1;
	// Events evicted by the ring since this session started, published so a client can tell an empty
	// page from a page it lost.
	uint64_t dropped = 0;
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_EVENT_LOG_H
