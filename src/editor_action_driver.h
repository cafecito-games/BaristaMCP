/**************************************************************************/
/*  editor_action_driver.h                                                */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_EDITOR_ACTION_DRIVER_H
#define BARISTA_MCP_EDITOR_ACTION_DRIVER_H

#include "editor_automation_types.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class Control;

// Bounds every action. Argument sizes are charged against these before any engine call runs, so a
// single request can never turn into unbounded input work.
struct EditorActionLimits {
	// Longest text one set_text may write.
	static constexpr int MAX_TEXT_LENGTH = 1024;
	// Longest text one type_text may enter. Every character becomes one public input event, so this
	// bound is strictly smaller than the set_text bound.
	static constexpr int MAX_TYPED_LENGTH = 256;
	static constexpr int MIN_INDEX = 0;
	static constexpr int MAX_INDEX = 4095;
	static constexpr int MIN_SCROLL_OFFSET = 0;
	static constexpr int MAX_SCROLL_OFFSET = 1000000;
};

// One parsed action request. Every action-specific field is explicit: an absent field is never
// defaulted into a value, and a field the requested action does not use is a rejection, not noise.
struct EditorActionRequest {
	String action;
	bool has_text = false;
	String text;
	bool has_value = false;
	double value = 0.0;
	bool has_checked = false;
	bool checked = false;
	bool has_index = false;
	int index = 0;
	bool has_scroll_axis = false;
	String scroll_axis;
	bool has_scroll_offset = false;
	int scroll_offset = 0;
};

// Single source of truth for action dispatch and for the routes and statuses one action can report.
// Every route is a documented public Godot API: either a direct public method on the control, or a
// public input event pushed through the control's own viewport.
class EditorActionDriver {
public:
	enum class Status {
		OK,
		AUTOMATION_DISABLED,
		INVALID_ARGUMENTS,
		INVALID_SELECTOR,
		NO_MATCH,
		AMBIGUOUS_SELECTOR,
		STALE_HANDLE,
		UNSUPPORTED_ACTION,
		ELEMENT_NOT_INTERACTABLE,
		UNSUPPORTED_CAPABILITY,
		MUTATION_ALREADY_HANDLED,
	};

	enum class Route {
		NONE,
		CONTROL_METHOD,
		INPUT_EVENT,
	};

	static PackedStringArray status_vocabulary();
	static PackedStringArray route_vocabulary();
	static PackedStringArray argument_vocabulary();
	static PackedStringArray scroll_axis_vocabulary();
	static String status_name(Status p_status);
	static String route_name(Route p_route);

	// Parses one action request. Returns false with a bounded diagnostic for an unknown action, a
	// missing action-specific field, a field the action does not use, and an over-long text value.
	// It reads no engine state, so arguments are settled before anything can be captured or mutated.
	static bool parse(const Dictionary &p_arguments, EditorActionRequest &r_request, String &r_message);

	// Performs one already-validated action against a live control. The element is the entry-time
	// capture of that control and is never widened here: an action the element does not advertise, and
	// a control that is no longer interactable, both fail without touching the editor.
	static bool perform(Control *p_control, const EditorElement &p_element, const EditorActionRequest &p_request,
			Route &r_route, Status &r_status, String &r_message);

	// The act_on_editor_ui payload for a request that performed no action. Every failure a client can
	// provoke is published in this one shape.
	static Dictionary failure(Status p_status, const String &p_message, const String &p_action = String());
};

} // namespace godot

#endif // BARISTA_MCP_EDITOR_ACTION_DRIVER_H
