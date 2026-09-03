/**************************************************************************/
/*  editor_wait_manager.h                                                 */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_WAIT_MANAGER_H
#define BARISTA_MCP_EDITOR_WAIT_MANAGER_H

#include "editor_automation_types.h"
#include "editor_selector.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <map>
#include <memory>

namespace godot {

class EditorEventLog;

// Bounds every cooperative wait. A wait never blocks an editor frame: it is evaluated once per plugin
// process frame against state the plugin already had to read, and every window below is a published
// limit rather than an open-ended loop.
struct EditorWaitLimits {
	// Live handles one session may hold at once, counting both active waits and terminal waits that
	// have not been consumed yet. A request that would exceed it is refused, never queued.
	static constexpr int MAX_WAITS = 16;
	static constexpr int MIN_TIMEOUT_MS = 1;
	static constexpr int MAX_TIMEOUT_MS = 30000;
	static constexpr int DEFAULT_TIMEOUT_MS = 5000;
	static constexpr int MIN_PRIME_MS = 1;
	static constexpr int MAX_PRIME_MS = 10000;
	static constexpr int DEFAULT_PRIME_MS = 1000;
	static constexpr int MIN_FRAMES = 1;
	static constexpr int MAX_FRAMES = 600;
	// A wait that has reached a terminal state stays readable this long, so a poll that arrives after
	// the outcome still learns it instead of reading as an unknown id.
	static constexpr int RETENTION_MS = 60000;
	// Conditions that need a UI capture are re-evaluated at most this often. Frame counting and the
	// cheap engine reads still advance on every frame; only the capture is throttled, so a wait can
	// never turn one editor frame into an unbounded amount of traversal work.
	static constexpr int EVALUATION_INTERVAL_MS = 50;
	// Random bytes behind one opaque wait id, hex encoded.
	static constexpr int WAIT_ID_BYTES = 16;
	static constexpr const char *WAIT_ID_PREFIX = "wait:";
};

// One parsed wait condition. Every field belongs to exactly one condition type, and a field the
// requested type does not use is rejected rather than ignored.
struct EditorWaitCondition {
	String type;
	int frames = 0;
	std::shared_ptr<EditorSelectorQuery> selector;
	// Extra constraints the identified element must satisfy, for selector_state. Its handle field is
	// reserved for the manager: a client-supplied handle inside "state" is rejected at parse time, so
	// binding the resolved element into it can never overwrite a constraint a client asked for.
	std::shared_ptr<EditorSelectorQuery> state;
	bool playing = false;
	bool require_start = false;
	int prime_ms = EditorWaitLimits::DEFAULT_PRIME_MS;
};

// The observable editor state one evaluation tick is decided against. The plugin assembles it once
// per frame and every active wait is evaluated against that same reading, so no wait can force a
// second traversal of the editor and none can observe state another wait created.
struct EditorWaitContext {
	uint64_t now_ms = 0;
	// A snapshot is present only on ticks where some active wait needed one and the published
	// evaluation interval had elapsed.
	bool has_snapshot = false;
	const EditorSnapshotData *snapshot = nullptr;
	String focused_handle;
	bool filesystem_available = false;
	bool filesystem_busy = false;
	bool is_playing = false;
};

// Single source of truth for wait status, wait conditions, and wait-handle lifetime. Handles are
// opaque and unique per server session; nothing here reads or mutates the editor itself.
class EditorWaitManager {
public:
	enum class Status {
		PENDING,
		COMPLETE,
		// The primed settle window expired without the editor ever being observed busy, so the
		// operation was never seen to start and settling was never confirmed. It is deliberately not
		// "complete": a naive drain-only loop would have reported success here.
		NOT_STARTED,
		WAIT_TIMEOUT,
		WAIT_CANCELLED,
		WAIT_NOT_FOUND,
		INVALID_ARGUMENTS,
		CAPACITY_REACHED,
		UNSUPPORTED_CAPABILITY,
	};

	// The condition type reported for a request that never named one.
	static constexpr const char *CONDITION_NONE = "none";
	static constexpr const char *CONDITION_FRAMES_ELAPSED = "frames_elapsed";
	static constexpr const char *CONDITION_SELECTOR_APPEARS = "selector_appears";
	static constexpr const char *CONDITION_SELECTOR_DISAPPEARS = "selector_disappears";
	static constexpr const char *CONDITION_SELECTOR_STATE = "selector_state";
	static constexpr const char *CONDITION_FOCUS_CHANGED = "focus_changed";
	static constexpr const char *CONDITION_PLAY_STATE = "play_state";
	static constexpr const char *CONDITION_FILESYSTEM_SETTLES = "filesystem_settles";

	static PackedStringArray status_vocabulary();
	// The condition types a request may name.
	static PackedStringArray condition_vocabulary();
	// What a result's "condition" field may hold: every requestable type plus "none" for a request
	// that was refused before any condition could be read.
	static PackedStringArray reported_condition_vocabulary();
	static PackedStringArray condition_field_vocabulary();
	static String status_name(Status p_status);
	static Dictionary limits();

	// Parses one condition object. It reads no editor state, so a condition is fully settled before
	// anything can be captured. Returns false with a bounded diagnostic for an unknown type, a missing
	// type-specific field, a field the type does not use, and an out-of-range window.
	static bool parse_condition(const Variant &p_condition, EditorWaitCondition &r_condition, String &r_message);
	// True only for a string shaped like an id this manager issues. It is checked before any lookup,
	// so a malformed id is never turned into a table probe.
	static bool is_wait_id(const String &p_id);

	void configure(EditorEventLog *p_event_log);

	// Starts one wait, evaluating its condition once against p_context. A condition already satisfied
	// completes immediately and creates no handle. A refusal creates no handle, consumes no capacity,
	// and records no event.
	Dictionary start(EditorWaitCondition p_condition, int p_timeout_ms, const EditorWaitContext &p_context);
	// Reads one wait. A wait in a terminal state is consumed by the poll that reports it; a pending
	// wait is unchanged, so polling is idempotent until terminal consumption.
	Dictionary poll(const String &p_wait_id, uint64_t p_now_ms);
	// Cancels one wait. Cancelling is idempotent: an already terminal wait keeps the outcome it has
	// and reports it again rather than being overwritten.
	Dictionary cancel(const String &p_wait_id, uint64_t p_now_ms);
	// The payload for a request that named no wait at all.
	Dictionary failure(Status p_status, const String &p_message) const;

	// True while some handle is still live, so the plugin can skip every per-frame reading when no
	// wait is outstanding.
	bool has_waits() const;
	// True when some pending wait needs a UI capture this tick.
	bool needs_snapshot(uint64_t p_now_ms) const;
	void process(const EditorWaitContext &p_context);
	// Cancels and clears every handle. Nothing survives a shutdown.
	void shutdown();

private:
	struct EditorWait {
		String id;
		EditorWaitCondition condition;
		Status status = Status::PENDING;
		uint64_t created_ms = 0;
		uint64_t deadline_ms = 0;
		uint64_t prime_deadline_ms = 0;
		// True while a primed settle is still waiting to observe the editor become busy.
		bool priming = false;
		uint64_t terminal_ms = 0;
		int timeout_ms = EditorWaitLimits::DEFAULT_TIMEOUT_MS;
		int frames_observed = 0;
		String baseline_focus_handle;
		Dictionary detail;
	};

	EditorEventLog *event_log = nullptr;
	Ref<Crypto> crypto;
	std::map<String, EditorWait> waits;
	uint64_t last_snapshot_evaluation_ms = 0;
	bool has_evaluated_snapshot = false;

	String _mint_id();
	// Retires waits whose deadline passed and drops terminal waits nothing consumed inside the
	// retention window. Time is the only input, so it never reads editor state.
	void _advance_time(uint64_t p_now_ms);
	void _finish(EditorWait &r_wait, Status p_status, uint64_t p_now_ms);
	// Evaluates one condition against one reading. Returns true when the condition was observed to
	// hold; r_settled_status carries a terminal status other than COMPLETE where one applies.
	bool _evaluate(EditorWait &r_wait, const EditorWaitContext &p_context, bool &r_not_started);
	Dictionary _payload(const EditorWait *p_wait, Status p_status, const String &p_message, const String &p_condition,
			uint64_t p_now_ms) const;
	static bool _condition_needs_snapshot(const String &p_type);
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_WAIT_MANAGER_H
