/**************************************************************************/
/*  editor_wait_manager.cpp                                               */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_wait_manager.h"

#include "editor_event_log.h"

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <utility>
#include <vector>

namespace godot {

namespace {

constexpr const char *FIELD_TYPE = "type";
constexpr const char *FIELD_FRAMES = "frames";
constexpr const char *FIELD_SELECTOR = "selector";
constexpr const char *FIELD_STATE = "state";
constexpr const char *FIELD_PLAYING = "playing";
constexpr const char *FIELD_REQUIRE_START = "require_start";
constexpr const char *FIELD_PRIME_MS = "prime_ms";

const char HEX_DIGITS[] = "0123456789abcdef";

// Longest client-supplied id a diagnostic will echo. A well-formed id is far shorter, and echoing an
// arbitrary one verbatim would let an accepted request turn its own refusal into a response too large
// for the transport to send, which would answer with a transport error instead of the wait status.
constexpr int MAX_ECHOED_WAIT_ID_LENGTH = 64;

String echoed_wait_id(const String &p_wait_id) {
	if (p_wait_id.length() <= MAX_ECHOED_WAIT_ID_LENGTH) {
		return p_wait_id;
	}
	return p_wait_id.substr(0, MAX_ECHOED_WAIT_ID_LENGTH) + "...";
}

bool snapshot_truncated(const EditorSnapshotData &p_data) {
	return p_data.depth_truncated || p_data.element_limit_reached || p_data.traversal_limit_reached;
}

// Counts the matches of one query against one capture. The walk is asked for at most p_limit matches,
// because a count is only ever needed as "none", "exactly one", or "at least one".
int count_matches(const EditorSnapshotData &p_data, const EditorSelectorQuery &p_query, int p_limit,
		std::vector<const EditorElement *> &r_page, bool &r_visit_limit_reached) {
	int total = 0;
	r_visit_limit_reached = false;
	EditorSelector::match(p_data, p_query, 0, p_limit, r_page, total, r_visit_limit_reached);
	return total;
}

} // namespace

PackedStringArray EditorWaitManager::status_vocabulary() {
	PackedStringArray statuses;
	statuses.push_back("pending");
	statuses.push_back("complete");
	statuses.push_back("not_started");
	statuses.push_back("wait_timeout");
	statuses.push_back("wait_cancelled");
	statuses.push_back("wait_not_found");
	statuses.push_back("invalid_arguments");
	statuses.push_back("capacity_reached");
	statuses.push_back("unsupported_capability");
	return statuses;
}

PackedStringArray EditorWaitManager::condition_vocabulary() {
	PackedStringArray types;
	types.push_back(CONDITION_FRAMES_ELAPSED);
	types.push_back(CONDITION_SELECTOR_APPEARS);
	types.push_back(CONDITION_SELECTOR_DISAPPEARS);
	types.push_back(CONDITION_SELECTOR_STATE);
	types.push_back(CONDITION_FOCUS_CHANGED);
	types.push_back(CONDITION_PLAY_STATE);
	types.push_back(CONDITION_FILESYSTEM_SETTLES);
	return types;
}

PackedStringArray EditorWaitManager::reported_condition_vocabulary() {
	// Derived from the requestable vocabulary, never listed a second time, so a condition added later
	// is reportable by construction.
	PackedStringArray types;
	types.push_back(CONDITION_NONE);
	types.append_array(condition_vocabulary());
	return types;
}

PackedStringArray EditorWaitManager::condition_field_vocabulary() {
	PackedStringArray fields;
	fields.push_back(FIELD_TYPE);
	fields.push_back(FIELD_FRAMES);
	fields.push_back(FIELD_SELECTOR);
	fields.push_back(FIELD_STATE);
	fields.push_back(FIELD_PLAYING);
	fields.push_back(FIELD_REQUIRE_START);
	fields.push_back(FIELD_PRIME_MS);
	return fields;
}

String EditorWaitManager::status_name(Status p_status) {
	switch (p_status) {
		case Status::PENDING:
			return "pending";
		case Status::COMPLETE:
			return "complete";
		case Status::NOT_STARTED:
			return "not_started";
		case Status::WAIT_TIMEOUT:
			return "wait_timeout";
		case Status::WAIT_CANCELLED:
			return "wait_cancelled";
		case Status::WAIT_NOT_FOUND:
			return "wait_not_found";
		case Status::INVALID_ARGUMENTS:
			return "invalid_arguments";
		case Status::CAPACITY_REACHED:
			return "capacity_reached";
		case Status::UNSUPPORTED_CAPABILITY:
			return "unsupported_capability";
	}
	return "invalid_arguments";
}

Dictionary EditorWaitManager::limits() {
	Dictionary published;
	published["max_waits"] = EditorWaitLimits::MAX_WAITS;
	published["min_timeout_ms"] = EditorWaitLimits::MIN_TIMEOUT_MS;
	published["max_timeout_ms"] = EditorWaitLimits::MAX_TIMEOUT_MS;
	published["default_timeout_ms"] = EditorWaitLimits::DEFAULT_TIMEOUT_MS;
	published["max_prime_ms"] = EditorWaitLimits::MAX_PRIME_MS;
	published["default_prime_ms"] = EditorWaitLimits::DEFAULT_PRIME_MS;
	published["max_frames"] = EditorWaitLimits::MAX_FRAMES;
	published["evaluation_interval_ms"] = EditorWaitLimits::EVALUATION_INTERVAL_MS;
	published["retention_ms"] = EditorWaitLimits::RETENTION_MS;
	return published;
}

bool EditorWaitManager::_condition_needs_snapshot(const String &p_type) {
	return p_type == CONDITION_SELECTOR_APPEARS || p_type == CONDITION_SELECTOR_DISAPPEARS ||
			p_type == CONDITION_SELECTOR_STATE || p_type == CONDITION_FOCUS_CHANGED;
}

bool EditorWaitManager::is_wait_id(const String &p_id) {
	const String prefix = EditorWaitLimits::WAIT_ID_PREFIX;
	if (!p_id.begins_with(prefix)) {
		return false;
	}
	const String body = p_id.substr(prefix.length());
	if (body.length() != EditorWaitLimits::WAIT_ID_BYTES * 2) {
		return false;
	}
	for (int i = 0; i < body.length(); i++) {
		const char32_t character = body[i];
		const bool is_hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
		if (!is_hex) {
			return false;
		}
	}
	return true;
}

namespace {

// Reports the first condition field present that the requested type does not use, so a field is
// rejected rather than silently ignored.
bool reject_unused_fields(
		const Dictionary &p_condition, const PackedStringArray &p_allowed, const String &p_type, String &r_message) {
	const Array keys = p_condition.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		if (key == FIELD_TYPE) {
			continue;
		}
		bool allowed = false;
		for (int field = 0; field < p_allowed.size(); field++) {
			if (p_allowed[field] == key) {
				allowed = true;
				break;
			}
		}
		if (!allowed) {
			r_message = "Condition '" + p_type + "' does not use the field '" + key + "'.";
			return false;
		}
	}
	return true;
}

// Reads one integer condition field. JSON carries every number as a double, so an integral float is
// a valid integer here exactly as it is at the request boundary; nothing else is coerced.
bool read_integer(const Dictionary &p_condition, const char *p_field, const String &p_condition_type, int64_t p_minimum,
		int64_t p_maximum, int &r_value, String &r_message) {
	const Variant value = p_condition.get(p_field, Variant());
	int64_t number = 0;
	if (value.get_type() == Variant::INT) {
		number = value;
	} else if (value.get_type() == Variant::FLOAT) {
		const double raw = value;
		if (!Math::is_finite(raw) || raw != Math::floor(raw) || raw < (double)p_minimum || raw > (double)p_maximum) {
			r_message = "Condition '" + p_condition_type + "' field '" + String(p_field) +
					"' is outside the published range.";
			return false;
		}
		number = (int64_t)raw;
	} else {
		r_message = "Condition '" + p_condition_type + "' field '" + String(p_field) + "' has the wrong type.";
		return false;
	}
	if (number < p_minimum || number > p_maximum) {
		r_message =
				"Condition '" + p_condition_type + "' field '" + String(p_field) + "' is outside the published range.";
		return false;
	}
	r_value = (int)number;
	return true;
}

bool require_field(const Dictionary &p_condition, const char *p_field, Variant::Type p_type,
		const String &p_condition_type, String &r_message) {
	if (!p_condition.has(p_field)) {
		r_message = "Condition '" + p_condition_type + "' requires the field '" + String(p_field) + "'.";
		return false;
	}
	if (p_condition.get(p_field, Variant()).get_type() != p_type) {
		r_message = "Condition '" + p_condition_type + "' field '" + String(p_field) + "' has the wrong type.";
		return false;
	}
	return true;
}

bool parse_selector_field(const Dictionary &p_condition, const char *p_field, const String &p_condition_type,
		std::shared_ptr<EditorSelectorQuery> &r_query, String &r_message) {
	if (!p_condition.has(p_field)) {
		r_message = "Condition '" + p_condition_type + "' requires the field '" + String(p_field) + "'.";
		return false;
	}
	r_query = std::make_shared<EditorSelectorQuery>();
	String selector_message;
	if (!EditorSelector::parse(p_condition.get(p_field, Variant()), *r_query, selector_message)) {
		r_query.reset();
		r_message = "Condition '" + p_condition_type + "' field '" + String(p_field) + "': " + selector_message;
		return false;
	}
	return true;
}

} // namespace

bool EditorWaitManager::parse_condition(
		const Variant &p_condition, EditorWaitCondition &r_condition, String &r_message) {
	if (p_condition.get_type() != Variant::DICTIONARY) {
		r_message = "A wait condition must be an object.";
		return false;
	}
	const Dictionary condition = p_condition;
	if (!condition.has(FIELD_TYPE) || condition.get(FIELD_TYPE, Variant()).get_type() != Variant::STRING) {
		r_message = "A wait condition requires a string 'type'.";
		return false;
	}
	const String type = condition.get(FIELD_TYPE, Variant());
	const PackedStringArray types = condition_vocabulary();
	bool known = false;
	for (int i = 0; i < types.size(); i++) {
		if (types[i] == type) {
			known = true;
			break;
		}
	}
	if (!known) {
		r_message = "Unknown wait condition type '" + type + "'.";
		return false;
	}
	r_condition = EditorWaitCondition();
	r_condition.type = type;

	PackedStringArray allowed;
	if (type == CONDITION_FRAMES_ELAPSED) {
		allowed.push_back(FIELD_FRAMES);
		if (!reject_unused_fields(condition, allowed, type, r_message) || !condition.has(FIELD_FRAMES)) {
			if (r_message.is_empty()) {
				r_message = "Condition 'frames_elapsed' requires the field 'frames'.";
			}
			return false;
		}
		// The advertised range is enforced by the boundary schema; it is re-checked here so the
		// condition is bounded even where it is reached from outside that boundary.
		return read_integer(condition, FIELD_FRAMES, type, EditorWaitLimits::MIN_FRAMES, EditorWaitLimits::MAX_FRAMES,
				r_condition.frames, r_message);
	}
	if (type == CONDITION_SELECTOR_APPEARS || type == CONDITION_SELECTOR_DISAPPEARS) {
		allowed.push_back(FIELD_SELECTOR);
		return reject_unused_fields(condition, allowed, type, r_message) &&
				parse_selector_field(condition, FIELD_SELECTOR, type, r_condition.selector, r_message);
	}
	if (type == CONDITION_SELECTOR_STATE) {
		allowed.push_back(FIELD_SELECTOR);
		allowed.push_back(FIELD_STATE);
		if (!reject_unused_fields(condition, allowed, type, r_message) ||
				!parse_selector_field(condition, FIELD_SELECTOR, type, r_condition.selector, r_message) ||
				!parse_selector_field(condition, FIELD_STATE, type, r_condition.state, r_message)) {
			return false;
		}
		// "state" names constraints, never identity: the manager binds the element resolved by
		// "selector" into that field itself, so a client-supplied handle there would be overwritten.
		if (r_condition.state->has_handle) {
			r_message = "Condition 'selector_state' field 'state' must not carry a handle; 'selector' names "
						"the element and 'state' names the constraints it must satisfy.";
			return false;
		}
		return true;
	}
	if (type == CONDITION_FOCUS_CHANGED) {
		return reject_unused_fields(condition, allowed, type, r_message);
	}
	if (type == CONDITION_PLAY_STATE) {
		allowed.push_back(FIELD_PLAYING);
		if (!reject_unused_fields(condition, allowed, type, r_message) ||
				!require_field(condition, FIELD_PLAYING, Variant::BOOL, type, r_message)) {
			return false;
		}
		r_condition.playing = condition.get(FIELD_PLAYING, Variant());
		return true;
	}

	allowed.push_back(FIELD_REQUIRE_START);
	allowed.push_back(FIELD_PRIME_MS);
	if (!reject_unused_fields(condition, allowed, type, r_message)) {
		return false;
	}
	if (condition.has(FIELD_REQUIRE_START)) {
		if (condition.get(FIELD_REQUIRE_START, Variant()).get_type() != Variant::BOOL) {
			r_message = "Condition 'filesystem_settles' field 'require_start' has the wrong type.";
			return false;
		}
		r_condition.require_start = condition.get(FIELD_REQUIRE_START, Variant());
	}
	if (condition.has(FIELD_PRIME_MS)) {
		// Priming is what tells a never-started operation apart from a settled one. Without it there
		// is no prime window to size, so accepting the field would publish a bound nothing applies.
		if (!r_condition.require_start) {
			r_message = "Condition 'filesystem_settles' field 'prime_ms' applies only when 'require_start' is true.";
			return false;
		}
		if (!read_integer(condition, FIELD_PRIME_MS, type, EditorWaitLimits::MIN_PRIME_MS,
					EditorWaitLimits::MAX_PRIME_MS, r_condition.prime_ms, r_message)) {
			return false;
		}
	}
	return true;
}

void EditorWaitManager::configure(EditorEventLog *p_event_log) {
	event_log = p_event_log;
}

String EditorWaitManager::_mint_id() {
	if (crypto.is_null()) {
		crypto.instantiate();
	}
	if (crypto.is_null()) {
		return String();
	}
	for (int attempt = 0; attempt < 4; attempt++) {
		const PackedByteArray bytes = crypto->generate_random_bytes(EditorWaitLimits::WAIT_ID_BYTES);
		if (bytes.size() != EditorWaitLimits::WAIT_ID_BYTES) {
			return String();
		}
		String body;
		for (int i = 0; i < bytes.size(); i++) {
			const uint8_t value = bytes[i];
			body += String::chr(HEX_DIGITS[value >> 4]);
			body += String::chr(HEX_DIGITS[value & 0x0f]);
		}
		const String id = String(EditorWaitLimits::WAIT_ID_PREFIX) + body;
		if (waits.find(id) == waits.end()) {
			return id;
		}
	}
	return String();
}

void EditorWaitManager::_finish(EditorWait &r_wait, Status p_status, uint64_t p_now_ms) {
	r_wait.status = p_status;
	r_wait.terminal_ms = p_now_ms;
	if (event_log != nullptr) {
		Dictionary detail;
		detail["wait_id"] = r_wait.id;
		detail["frames_observed"] = r_wait.frames_observed;
		event_log->record(EditorEventLog::TYPE_WAIT_END, r_wait.condition.type, status_name(p_status), detail);
	}
}

void EditorWaitManager::_advance_time(uint64_t p_now_ms) {
	for (auto &entry : waits) {
		EditorWait &wait = entry.second;
		if (wait.status != Status::PENDING) {
			continue;
		}
		// The prime window owns its own expiry, and it is checked before the deadline. A primed
		// settle whose prime window has passed never observed the editor become busy, so the honest
		// outcome is "never started" no matter how the two windows are sized against each other;
		// reporting a timeout there would hide exactly the case priming exists to detect.
		if (wait.priming && p_now_ms >= wait.prime_deadline_ms) {
			wait.detail["observed_busy"] = false;
			_finish(wait, Status::NOT_STARTED, p_now_ms);
			continue;
		}
		if (p_now_ms >= wait.deadline_ms) {
			_finish(wait, Status::WAIT_TIMEOUT, p_now_ms);
		}
	}
	for (auto entry = waits.begin(); entry != waits.end();) {
		const EditorWait &wait = entry->second;
		const bool retired = wait.status != Status::PENDING &&
				p_now_ms - wait.terminal_ms >= (uint64_t)EditorWaitLimits::RETENTION_MS;
		if (retired) {
			entry = waits.erase(entry);
		} else {
			++entry;
		}
	}
}

bool EditorWaitManager::_evaluate(EditorWait &r_wait, const EditorWaitContext &p_context) {
	const EditorWaitCondition &condition = r_wait.condition;

	if (condition.type == CONDITION_FRAMES_ELAPSED) {
		r_wait.detail["frames_required"] = condition.frames;
		return r_wait.frames_observed >= condition.frames;
	}
	if (condition.type == CONDITION_PLAY_STATE) {
		r_wait.detail["is_playing"] = p_context.is_playing;
		return p_context.is_playing == condition.playing;
	}
	if (condition.type == CONDITION_FILESYSTEM_SETTLES) {
		r_wait.detail["busy"] = p_context.filesystem_busy;
		if (r_wait.priming) {
			// Prime, then drain. A scan's own busy flag is raised asynchronously, so a loop that only
			// drains exits at once and reports a settle that never happened. The prime window's own
			// expiry is decided in _advance_time, so this only ever observes the transition.
			if (p_context.filesystem_busy) {
				r_wait.priming = false;
				r_wait.detail["observed_busy"] = true;
			}
			return false;
		}
		return !p_context.filesystem_busy;
	}
	if (!p_context.has_snapshot || p_context.snapshot == nullptr) {
		return false;
	}
	const EditorSnapshotData &data = *p_context.snapshot;
	const bool truncated = snapshot_truncated(data);
	r_wait.detail["truncated"] = truncated;

	if (condition.type == CONDITION_FOCUS_CHANGED) {
		r_wait.detail["focused_handle"] = p_context.focused_handle;
		if (p_context.focused_handle == r_wait.baseline_focus_handle) {
			return false;
		}
		// Focus arriving at an element this capture actually reached is positive evidence, and a
		// bounded walk cannot falsify it. Focus reading as "nothing" is a different claim: a capture
		// that was cut short may simply have omitted the element that still holds focus, so absence
		// of focus is only ever reported from a capture that was not truncated.
		return !p_context.focused_handle.is_empty() || !truncated;
	}
	if (condition.selector == nullptr) {
		return false;
	}

	std::vector<const EditorElement *> page;
	bool visit_limit_reached = false;
	const int total = count_matches(
			data, *condition.selector, condition.type == CONDITION_SELECTOR_STATE ? 2 : 1, page, visit_limit_reached);
	r_wait.detail["match_count"] = total;
	r_wait.detail["visit_limit_reached"] = visit_limit_reached;

	if (condition.type == CONDITION_SELECTOR_APPEARS) {
		// One match is proof of presence even from a bounded walk, so truncation never falsifies it.
		return total >= 1;
	}
	if (condition.type == CONDITION_SELECTOR_DISAPPEARS) {
		// Absence is only ever provable from a walk that was not cut short. A truncated capture and a
		// truncated walk both count a lower bound, so neither can show that nothing matched.
		return total == 0 && !visit_limit_reached && !truncated;
	}
	// selector_state: the identity selector must name exactly one element before its state can be
	// judged, and a bounded walk can never show that one match is the only one.
	if (total != 1 || visit_limit_reached || truncated || page.empty()) {
		return false;
	}
	if (condition.state == nullptr) {
		return false;
	}
	condition.state->has_handle = true;
	condition.state->handle = page[0]->handle;
	std::vector<const EditorElement *> state_page;
	bool state_visit_limit_reached = false;
	const int state_total = count_matches(data, *condition.state, 1, state_page, state_visit_limit_reached);
	r_wait.detail["state_match_count"] = state_total;
	return state_total >= 1;
}

Dictionary EditorWaitManager::_payload(const EditorWait *p_wait, Status p_status, const String &p_message,
		const String &p_condition, uint64_t p_now_ms) const {
	Dictionary payload;
	// "ok" asserts exactly one thing: the waited-for condition was observed to hold. A wait that is
	// still pending, timed out, cancelled, or never started is not that, so none of them is "ok".
	payload["ok"] = p_status == Status::COMPLETE;
	payload["status"] = status_name(p_status);
	payload["message"] = p_message;
	payload["wait_id"] = p_wait == nullptr ? String() : p_wait->id;
	payload["condition"] = p_condition;
	payload["elapsed_ms"] = p_wait == nullptr ? 0 : (int64_t)(p_now_ms - p_wait->created_ms);
	payload["remaining_ms"] = (p_wait == nullptr || p_status != Status::PENDING || p_now_ms >= p_wait->deadline_ms)
			? (int64_t)0
			: (int64_t)(p_wait->deadline_ms - p_now_ms);
	payload["timeout_ms"] = p_wait == nullptr ? 0 : p_wait->timeout_ms;
	payload["frames_observed"] = p_wait == nullptr ? 0 : p_wait->frames_observed;
	payload["active_waits"] = (int)waits.size();
	payload["detail"] = p_wait == nullptr ? Dictionary() : p_wait->detail;
	payload["limits"] = limits();
	return payload;
}

Dictionary EditorWaitManager::failure(Status p_status, const String &p_message) const {
	return _payload(nullptr, p_status, p_message, CONDITION_NONE, 0);
}

Dictionary EditorWaitManager::start(
		EditorWaitCondition p_condition, int p_timeout_ms, const EditorWaitContext &p_context) {
	_advance_time(p_context.now_ms);

	EditorWait candidate;
	candidate.condition = std::move(p_condition);
	candidate.created_ms = p_context.now_ms;
	candidate.timeout_ms = p_timeout_ms;
	candidate.deadline_ms = p_context.now_ms + (uint64_t)p_timeout_ms;
	candidate.priming = candidate.condition.type == CONDITION_FILESYSTEM_SETTLES && candidate.condition.require_start;
	candidate.prime_deadline_ms = p_context.now_ms + (uint64_t)candidate.condition.prime_ms;
	candidate.baseline_focus_handle = p_context.focused_handle;
	const String condition_type = candidate.condition.type;

	// A condition Barista cannot observe at all is refused outright rather than parked on a handle
	// that could only ever time out.
	if (_condition_needs_snapshot(condition_type) && !p_context.has_snapshot) {
		return _payload(nullptr, Status::UNSUPPORTED_CAPABILITY,
				"The editor UI could not be captured, so this condition cannot be evaluated.", condition_type,
				p_context.now_ms);
	}
	// A focus wait is judged against the focus this capture saw. A capture that was cut short may
	// have omitted the element that holds focus, so it cannot establish a baseline and the wait is
	// refused rather than parked on a baseline that was never true.
	if (condition_type == CONDITION_FOCUS_CHANGED && p_context.snapshot != nullptr &&
			snapshot_truncated(*p_context.snapshot)) {
		return _payload(nullptr, Status::UNSUPPORTED_CAPABILITY,
				"The editor UI capture was truncated, so the element holding focus cannot be established as a "
				"baseline.",
				condition_type, p_context.now_ms);
	}
	if (condition_type == CONDITION_FILESYSTEM_SETTLES && !p_context.filesystem_available) {
		return _payload(nullptr, Status::UNSUPPORTED_CAPABILITY,
				"The editor resource filesystem is unavailable, so settling cannot be observed.", condition_type,
				p_context.now_ms);
	}

	if (_evaluate(candidate, p_context)) {
		// An already satisfied condition needs no handle at all, so nothing is created and nothing is
		// left for a client to poll or cancel.
		if (event_log != nullptr) {
			event_log->record(EditorEventLog::TYPE_WAIT_BEGIN, condition_type, String());
			event_log->record(EditorEventLog::TYPE_WAIT_END, condition_type, status_name(Status::COMPLETE));
		}
		return _payload(&candidate, Status::COMPLETE, String(), condition_type, p_context.now_ms);
	}

	// Capacity is charged before anything is minted, so a refused request creates no handle, consumes
	// no capacity, and records no event.
	if ((int)waits.size() >= EditorWaitLimits::MAX_WAITS) {
		return _payload(nullptr, Status::CAPACITY_REACHED,
				"This session already holds " + String::num_int64(waits.size()) +
						" wait handles, which is the published maximum; poll or cancel one before starting another.",
				condition_type, p_context.now_ms);
	}
	const String id = _mint_id();
	if (id.is_empty()) {
		return _payload(nullptr, Status::UNSUPPORTED_CAPABILITY, "No wait handle could be issued in this session.",
				condition_type, p_context.now_ms);
	}
	candidate.id = id;
	waits[id] = candidate;
	if (event_log != nullptr) {
		Dictionary detail;
		detail["wait_id"] = id;
		detail["timeout_ms"] = p_timeout_ms;
		event_log->record(EditorEventLog::TYPE_WAIT_BEGIN, condition_type, String(), detail);
	}
	return _payload(&waits[id], Status::PENDING, String(), condition_type, p_context.now_ms);
}

Dictionary EditorWaitManager::poll(const String &p_wait_id, uint64_t p_now_ms) {
	_advance_time(p_now_ms);
	// The grammar is checked before the table is, so a malformed id is never turned into a lookup.
	if (!is_wait_id(p_wait_id) || waits.find(p_wait_id) == waits.end()) {
		return failure(Status::WAIT_NOT_FOUND,
				"No wait handle named '" + echoed_wait_id(p_wait_id) + "' is live in this session.");
	}
	const auto entry = waits.find(p_wait_id);
	if (entry->second.status == Status::PENDING) {
		return _payload(&entry->second, Status::PENDING, String(), entry->second.condition.type, p_now_ms);
	}
	// A terminal outcome is delivered exactly once: the poll that reports it destroys the handle.
	const EditorWait consumed = entry->second;
	waits.erase(entry);
	return _payload(&consumed, consumed.status, String(), consumed.condition.type, p_now_ms);
}

Dictionary EditorWaitManager::cancel(const String &p_wait_id, uint64_t p_now_ms) {
	_advance_time(p_now_ms);
	if (!is_wait_id(p_wait_id) || waits.find(p_wait_id) == waits.end()) {
		return failure(Status::WAIT_NOT_FOUND,
				"No wait handle named '" + echoed_wait_id(p_wait_id) + "' is live in this session.");
	}
	const auto entry = waits.find(p_wait_id);
	EditorWait &wait = entry->second;
	if (wait.status == Status::PENDING) {
		_finish(wait, Status::WAIT_CANCELLED, p_now_ms);
	}
	// Cancelling is idempotent inside the published retention window: a wait that already reached an
	// outcome keeps it and reports it again rather than being overwritten by a second cancel.
	return _payload(&wait, wait.status, String(), wait.condition.type, p_now_ms);
}

bool EditorWaitManager::has_waits() const {
	return !waits.empty();
}

bool EditorWaitManager::needs_snapshot(uint64_t p_now_ms) const {
	for (const auto &entry : waits) {
		if (entry.second.status != Status::PENDING || !_condition_needs_snapshot(entry.second.condition.type)) {
			continue;
		}
		return !has_evaluated_snapshot ||
				p_now_ms - last_snapshot_evaluation_ms >= (uint64_t)EditorWaitLimits::EVALUATION_INTERVAL_MS;
	}
	return false;
}

void EditorWaitManager::process(const EditorWaitContext &p_context) {
	_advance_time(p_context.now_ms);
	if (p_context.has_snapshot) {
		last_snapshot_evaluation_ms = p_context.now_ms;
		has_evaluated_snapshot = true;
	}
	for (auto &entry : waits) {
		EditorWait &wait = entry.second;
		if (wait.status != Status::PENDING) {
			continue;
		}
		wait.frames_observed++;
		if (_condition_needs_snapshot(wait.condition.type) && !p_context.has_snapshot) {
			continue;
		}
		if (_evaluate(wait, p_context)) {
			_finish(wait, Status::COMPLETE, p_context.now_ms);
		}
	}
}

void EditorWaitManager::shutdown() {
	for (auto &entry : waits) {
		if (entry.second.status == Status::PENDING) {
			_finish(entry.second, Status::WAIT_CANCELLED, entry.second.created_ms);
		}
	}
	waits.clear();
	crypto.unref();
	last_snapshot_evaluation_ms = 0;
	has_evaluated_snapshot = false;
}

} // namespace godot
