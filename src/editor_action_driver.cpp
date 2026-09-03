/**************************************************************************/
/*  editor_action_driver.cpp                                              */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_action_driver.h"

#include "mcp_contracts.h"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

constexpr const char *ACTION_CLICK = "click";
constexpr const char *ACTION_FOCUS = "focus";
constexpr const char *ACTION_SET_TEXT = "set_text";
constexpr const char *ACTION_TYPE_TEXT = "type_text";
constexpr const char *ACTION_SUBMIT = "submit";
constexpr const char *ACTION_SET_CHECKED = "set_checked";
constexpr const char *ACTION_SET_VALUE = "set_value";
constexpr const char *ACTION_SELECT_ITEM = "select_item";
constexpr const char *ACTION_SELECT_TAB = "select_tab";
constexpr const char *ACTION_SCROLL = "scroll";

/// Lowest unicode point a typed key event can carry and still be inserted by LineEdit or TextEdit.
/// Anything below is dropped by both, so it is not deliverable text.
constexpr char32_t ACCEPTED_TYPED_UNICODE_FLOOR = 0x20;

constexpr const char *ARGUMENT_TEXT = "text";
constexpr const char *ARGUMENT_VALUE = "value";
constexpr const char *ARGUMENT_CHECKED = "checked";
constexpr const char *ARGUMENT_INDEX = "index";
constexpr const char *ARGUMENT_SCROLL_AXIS = "scroll_axis";
constexpr const char *ARGUMENT_SCROLL_OFFSET = "scroll_offset";

constexpr const char *AXIS_VERTICAL = "vertical";
constexpr const char *AXIS_HORIZONTAL = "horizontal";

// Every action-specific argument an action requires. An action requires exactly these and accepts
// nothing else, so a field that belongs to another action is a rejection rather than dead weight.
struct ActionArguments {
	const char *action;
	const char *arguments[3];
};

const ActionArguments ACTION_ARGUMENTS[] = {
		{ACTION_CLICK, {nullptr, nullptr, nullptr}},
		{ACTION_FOCUS, {nullptr, nullptr, nullptr}},
		{ACTION_SET_TEXT, {ARGUMENT_TEXT, nullptr, nullptr}},
		{ACTION_TYPE_TEXT, {ARGUMENT_TEXT, nullptr, nullptr}},
		{ACTION_SUBMIT, {nullptr, nullptr, nullptr}},
		{ACTION_SET_CHECKED, {ARGUMENT_CHECKED, nullptr, nullptr}},
		{ACTION_SET_VALUE, {ARGUMENT_VALUE, nullptr, nullptr}},
		{ACTION_SELECT_ITEM, {ARGUMENT_INDEX, nullptr, nullptr}},
		{ACTION_SELECT_TAB, {ARGUMENT_INDEX, nullptr, nullptr}},
		{ACTION_SCROLL, {ARGUMENT_SCROLL_AXIS, ARGUMENT_SCROLL_OFFSET, nullptr}},
};

bool action_requires(const ActionArguments &p_entry, const String &p_argument) {
	for (const char *argument : p_entry.arguments) {
		if (argument == nullptr) {
			return false;
		}
		if (p_argument == argument) {
			return true;
		}
	}
	return false;
}

// Deepest node chain the geometric hit test walks. The editor's own control tree is far shallower,
// and the bound keeps a pathological tree from recursing without limit.
constexpr int MAX_HIT_TEST_DEPTH = 256;

// The editor's GUI hit test, reproduced through public API only so that deciding to refuse a click
// costs no dispatched input. It mirrors the engine's search order: a node's children first in reverse
// draw order, then the node itself. An invisible node hides its whole subtree, a control that clips
// its contents hides whatever falls outside its own rect, a control whose effective mouse filter
// ignores input is never a target, and a Window owns a viewport of its own so its subtree belongs to
// a different hit test. Point containment uses the control's global rect, which is what
// Control::has_point tests unless a control overrides it; the engine's own answer is still consulted
// before any press is delivered, so an override or a canvas transform this walk cannot see can only
// turn an accepted click into a refusal, never into a click on the wrong element.
Control *topmost_control_at(Node *p_node, const Vector2 &p_point, int p_depth) {
	if (p_node == nullptr || p_depth > MAX_HIT_TEST_DEPTH || Object::cast_to<Window>(p_node) != nullptr) {
		return nullptr;
	}
	CanvasItem *item = Object::cast_to<CanvasItem>(p_node);
	if (item != nullptr && !item->is_visible()) {
		return nullptr;
	}
	Control *control = Object::cast_to<Control>(p_node);
	if (control != nullptr && control->is_clipping_contents() && !control->get_global_rect().has_point(p_point)) {
		return nullptr;
	}
	for (int i = p_node->get_child_count(true) - 1; i >= 0; i--) {
		Control *hit = topmost_control_at(p_node->get_child(i, true), p_point, p_depth + 1);
		if (hit != nullptr) {
			return hit;
		}
	}
	if (control != nullptr && control->get_mouse_filter_with_override() != Control::MOUSE_FILTER_IGNORE &&
			control->get_global_rect().has_point(p_point)) {
		return control;
	}
	return nullptr;
}

// The control this viewport would hand a press at this point to, decided without dispatching
// anything. A refusal built on this answer leaves no input behind in any control.
Control *hit_target(Viewport *p_viewport, const Vector2 &p_point) {
	for (int i = p_viewport->get_child_count(true) - 1; i >= 0; i--) {
		Control *hit = topmost_control_at(p_viewport->get_child(i, true), p_point, 0);
		if (hit != nullptr) {
			return hit;
		}
	}
	return nullptr;
}

// Moves the pointer over the element and reports whether that element itself is what the editor
// would deliver a click to. Identity is exact: a descendant that stops propagation would consume the
// press, so accepting one would activate a control the client never selected while reporting a
// successful click on the one it did. This probe is itself a real mouse-motion event, so it is only
// ever reached after hit_target() has already named this element.
bool hover_reaches(Viewport *p_viewport, Control *p_control, const Vector2 &p_point) {
	Ref<InputEventMouseMotion> motion;
	motion.instantiate();
	motion->set_position(p_point);
	motion->set_global_position(p_point);
	p_viewport->push_input(motion, true);
	Control *hovered = p_viewport->gui_get_hovered_control();
	if (hovered == nullptr) {
		return false;
	}
	return hovered == p_control;
}

// Puts focus back where the editor had it. A route that grabs focus and then cannot complete must
// leave the editor focused where it was, never on the element whose action was refused.
void restore_focus(Control *p_previous_focus, Control *p_control) {
	if (p_previous_focus != nullptr && p_previous_focus != p_control) {
		p_previous_focus->grab_focus();
	}
}

// Every input route resolves its viewport once, before the first event is pushed, so a multi-event
// action can never fail part way through and leave the editor half mutated.
void push_click(Viewport *p_viewport, const Vector2 &p_point) {
	for (int press = 1; press >= 0; press--) {
		Ref<InputEventMouseButton> event;
		event.instantiate();
		event->set_button_index(MOUSE_BUTTON_LEFT);
		event->set_button_mask(press == 1 ? MOUSE_BUTTON_MASK_LEFT : MouseButtonMask(0));
		event->set_pressed(press == 1);
		event->set_position(p_point);
		event->set_global_position(p_point);
		p_viewport->push_input(event, true);
	}
}

void push_key(Viewport *p_viewport, Key p_keycode, char32_t p_unicode) {
	for (int press = 1; press >= 0; press--) {
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_pressed(press == 1);
		event->set_keycode(p_keycode);
		event->set_unicode(p_unicode);
		p_viewport->push_input(event, true);
	}
}

} // namespace

PackedStringArray EditorActionDriver::status_vocabulary() {
	PackedStringArray statuses;
	statuses.push_back("ok");
	statuses.push_back("automation_disabled");
	statuses.push_back("invalid_arguments");
	statuses.push_back("invalid_selector");
	statuses.push_back("no_match");
	statuses.push_back("ambiguous_selector");
	statuses.push_back("stale_handle");
	statuses.push_back("unsupported_action");
	statuses.push_back("element_not_interactable");
	statuses.push_back("unsupported_capability");
	statuses.push_back("mutation_already_handled");
	return statuses;
}

PackedStringArray EditorActionDriver::route_vocabulary() {
	PackedStringArray routes;
	routes.push_back("none");
	routes.push_back("control_method");
	routes.push_back("input_event");
	return routes;
}

PackedStringArray EditorActionDriver::argument_vocabulary() {
	PackedStringArray arguments;
	arguments.push_back(ARGUMENT_TEXT);
	arguments.push_back(ARGUMENT_VALUE);
	arguments.push_back(ARGUMENT_CHECKED);
	arguments.push_back(ARGUMENT_INDEX);
	arguments.push_back(ARGUMENT_SCROLL_AXIS);
	arguments.push_back(ARGUMENT_SCROLL_OFFSET);
	return arguments;
}

PackedStringArray EditorActionDriver::scroll_axis_vocabulary() {
	PackedStringArray axes;
	axes.push_back(AXIS_VERTICAL);
	axes.push_back(AXIS_HORIZONTAL);
	return axes;
}

String EditorActionDriver::status_name(Status p_status) {
	switch (p_status) {
		case Status::OK:
			return "ok";
		case Status::AUTOMATION_DISABLED:
			return "automation_disabled";
		case Status::INVALID_ARGUMENTS:
			return "invalid_arguments";
		case Status::INVALID_SELECTOR:
			return "invalid_selector";
		case Status::NO_MATCH:
			return "no_match";
		case Status::AMBIGUOUS_SELECTOR:
			return "ambiguous_selector";
		case Status::STALE_HANDLE:
			return "stale_handle";
		case Status::UNSUPPORTED_ACTION:
			return "unsupported_action";
		case Status::ELEMENT_NOT_INTERACTABLE:
			return "element_not_interactable";
		case Status::UNSUPPORTED_CAPABILITY:
			return "unsupported_capability";
		case Status::MUTATION_ALREADY_HANDLED:
			return "mutation_already_handled";
	}
	return "invalid_arguments";
}

String EditorActionDriver::route_name(Route p_route) {
	switch (p_route) {
		case Route::CONTROL_METHOD:
			return "control_method";
		case Route::INPUT_EVENT:
			return "input_event";
		case Route::NONE:
			break;
	}
	return "none";
}

Dictionary EditorActionDriver::failure(Status p_status, const String &p_message, const String &p_action) {
	Dictionary payload;
	payload["ok"] = false;
	payload["status"] = status_name(p_status);
	payload["message"] = p_message;
	if (!p_action.is_empty()) {
		payload["action"] = p_action;
		// The claim travels with every result, refusals included, so a client reads the standard this
		// route was held to from the same payload that reports the outcome.
		const String claim = MCPContracts::action_claim(p_action);
		if (!claim.is_empty()) {
			payload["claim"] = claim;
		}
	}
	payload["route"] = route_name(Route::NONE);
	payload["changed"] = false;
	payload["generation"] = (int64_t)0;
	payload["handle"] = String();
	return payload;
}

bool EditorActionDriver::parse(const Dictionary &p_arguments, EditorActionRequest &r_request, String &r_message) {
	r_request = EditorActionRequest();
	const Variant action_value = p_arguments.get("action", Variant());
	if (!p_arguments.has("action") || action_value.get_type() != Variant::STRING) {
		r_message = "act_on_editor_ui requires a string 'action'.";
		return false;
	}
	const String action = action_value;

	const ActionArguments *entry = nullptr;
	for (const ActionArguments &candidate : ACTION_ARGUMENTS) {
		if (action == candidate.action) {
			entry = &candidate;
			break;
		}
	}
	if (entry == nullptr) {
		r_message = "Action '" + action + "' is not an advertised action.";
		return false;
	}
	r_request.action = action;

	// Every action-specific field is checked against this action, so a field that belongs to another
	// action is refused instead of being carried into a mutation it does not describe.
	const PackedStringArray arguments = argument_vocabulary();
	for (int i = 0; i < arguments.size(); i++) {
		const String name = arguments[i];
		const bool required = action_requires(*entry, name);
		if (!p_arguments.has(name)) {
			if (required) {
				r_message = "Action '" + action + "' requires '" + name + "'.";
				return false;
			}
			continue;
		}
		if (!required) {
			r_message = "Action '" + action + "' does not accept '" + name + "'.";
			return false;
		}
		// Types are enforced by the boundary schema before this runs; nothing here coerces a value.
		if (name == ARGUMENT_TEXT) {
			r_request.has_text = true;
			r_request.text = p_arguments.get(name, Variant());
		} else if (name == ARGUMENT_VALUE) {
			r_request.has_value = true;
			r_request.value = p_arguments.get(name, Variant());
		} else if (name == ARGUMENT_CHECKED) {
			r_request.has_checked = true;
			r_request.checked = p_arguments.get(name, Variant());
		} else if (name == ARGUMENT_INDEX) {
			r_request.has_index = true;
			r_request.index = (int)(int64_t)p_arguments.get(name, Variant());
		} else if (name == ARGUMENT_SCROLL_AXIS) {
			r_request.has_scroll_axis = true;
			r_request.scroll_axis = p_arguments.get(name, Variant());
		} else if (name == ARGUMENT_SCROLL_OFFSET) {
			r_request.has_scroll_offset = true;
			r_request.scroll_offset = (int)(int64_t)p_arguments.get(name, Variant());
		}
	}

	// Text length is charged before any engine call, because typing turns every character into one
	// public input event.
	if (r_request.has_text) {
		const int limit =
				action == ACTION_TYPE_TEXT ? EditorActionLimits::MAX_TYPED_LENGTH : EditorActionLimits::MAX_TEXT_LENGTH;
		if (r_request.text.length() > limit) {
			r_message = "Action '" + action + "' accepts at most " + String::num_int64(limit) + " characters.";
			return false;
		}
	}
	if (r_request.has_value && !Math::is_finite(r_request.value)) {
		r_message = "Action '" + action + "' requires a finite 'value'.";
		return false;
	}
	if (r_request.has_scroll_axis && !scroll_axis_vocabulary().has(r_request.scroll_axis)) {
		r_message = "Scroll axis '" + r_request.scroll_axis + "' is not an advertised axis.";
		return false;
	}
	return true;
}

bool EditorActionDriver::perform(Control *p_control, const EditorElement &p_element,
		const EditorActionRequest &p_request, Route &r_route, Status &r_status, String &r_message) {
	r_route = Route::NONE;
	r_status = Status::OK;
	r_message = String();

	// Entry-time state, read before anything is mutated: an element the capture did not advertise the
	// action for, and a control that is no longer live in the tree, both stop here.
	if (p_control == nullptr || !p_control->is_visible_in_tree()) {
		r_status = Status::ELEMENT_NOT_INTERACTABLE;
		r_message = "The element is no longer visible in the editor tree.";
		return false;
	}
	if (!p_element.enabled) {
		r_status = Status::ELEMENT_NOT_INTERACTABLE;
		r_message = "The element is disabled and accepts no interaction.";
		return false;
	}
	if (!p_element.actions.has(p_request.action)) {
		r_status = Status::UNSUPPORTED_ACTION;
		r_message = "The element does not advertise the action '" + p_request.action + "'.";
		return false;
	}

	const String &action = p_request.action;

	if (action == ACTION_FOCUS) {
		// An ancestor that disables focus recursively overrides the element's own focus mode, and
		// grab_focus() on such a control returns without focusing anything, so the request is refused
		// before it is attempted.
		if (p_control->get_focus_mode_with_override() == Control::FOCUS_NONE) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element cannot take keyboard focus.";
			return false;
		}
		Viewport *viewport = p_control->get_viewport();
		if (viewport == nullptr) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element is not inside a viewport that accepts input.";
			return false;
		}
		Control *previous_focus = viewport->gui_get_focus_owner();
		p_control->grab_focus();
		// The requested postcondition is that this exact element owns focus. Anything less is a
		// failure, never a success reported against an element that never took focus, and the refusal
		// puts focus back where the editor had it rather than leaving it moved.
		if (!p_control->has_focus()) {
			restore_focus(previous_focus, p_control);
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element refused keyboard focus.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_CLICK) {
		const Rect2 rect = p_control->get_global_rect();
		if (rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element has no on-screen area to click.";
			return false;
		}
		Viewport *viewport = p_control->get_viewport();
		if (viewport == nullptr) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element is not inside a viewport that accepts input.";
			return false;
		}
		// A button publishes the mouse buttons it reacts to. BaseButton drops a press whose button is
		// outside that mask before it reaches any handler, so delivering one is a delivery the element
		// does not accept, not a click the editor merely chose to ignore.
		BaseButton *clicked_button = Object::cast_to<BaseButton>(p_control);
		if (clicked_button != nullptr && !clicked_button->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT)) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element does not accept the left mouse button.";
			return false;
		}
		const Vector2 point = rect.get_center();
		// Refusal is decided geometrically first, dispatching nothing, so a click Barista will not
		// perform leaves no input behind in whatever control covers this one.
		if (hit_target(viewport, point) != p_control) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element does not receive pointer input at its own center.";
			return false;
		}
		// The engine's own answer decides delivery. This probe is a real mouse-motion event, so it runs
		// only once the geometric test has already named this element: the one residual, when the engine
		// disagrees with the geometry, is a single motion event at a point the geometry attributed to
		// this element, and never a button event.
		if (!hover_reaches(viewport, p_control, point)) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element does not receive pointer input at its own center.";
			return false;
		}
		push_click(viewport, point);
		r_route = Route::INPUT_EVENT;
		return true;
	}

	if (action == ACTION_SET_TEXT) {
		LineEdit *line_edit = Object::cast_to<LineEdit>(p_control);
		if (line_edit != nullptr) {
			if (!line_edit->is_editable()) {
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The text field is not editable.";
				return false;
			}
			// LineEdit truncates silently to its own published max_length, so text the field can never
			// hold is refused before anything is written rather than answered with a shorter value.
			const int max_length = line_edit->get_max_length();
			if (max_length > 0 && p_request.text.length() > max_length) {
				r_status = Status::INVALID_ARGUMENTS;
				r_message = "The text field holds at most " + String::num_int64(max_length) + " characters.";
				return false;
			}
			const String previous_text = line_edit->get_text();
			line_edit->set_text(p_request.text);
			if (line_edit->get_text() != p_request.text) {
				// The refusal puts the field back the way it was: a request that cannot be reported as
				// performed must not leave the editor holding a value nobody asked for.
				line_edit->set_text(previous_text);
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The text field did not take the requested text.";
				return false;
			}
			r_route = Route::CONTROL_METHOD;
			return true;
		}
		TextEdit *text_edit = Object::cast_to<TextEdit>(p_control);
		if (text_edit != nullptr) {
			if (!text_edit->is_editable()) {
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The text area is not editable.";
				return false;
			}
			const String previous_text = text_edit->get_text();
			text_edit->set_text(p_request.text);
			if (text_edit->get_text() != p_request.text) {
				// TextEdit republishes its content line by line, so text it normalizes is text it did not
				// take. The refusal restores what the area held rather than leaving the normalized value.
				text_edit->set_text(previous_text);
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The text area did not take the requested text.";
				return false;
			}
			r_route = Route::CONTROL_METHOD;
			return true;
		}
		r_status = Status::UNSUPPORTED_ACTION;
		r_message = "Class '" + p_element.class_name + "' has no public text route.";
		return false;
	}

	if (action == ACTION_TYPE_TEXT || action == ACTION_SUBMIT) {
		// Typing and submitting are keyboard routes, so they are delivered to the focus owner: the
		// element takes focus first, exactly as it would for a person at the keyboard.
		if (p_control->get_focus_mode_with_override() == Control::FOCUS_NONE) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element cannot take keyboard focus.";
			return false;
		}
		// Typing delivers characters to the focused element, so the element must be one that accepts
		// them. Both refusals below are decided before a single event is pushed and before focus moves.
		if (action == ACTION_TYPE_TEXT) {
			LineEdit *typed_field = Object::cast_to<LineEdit>(p_control);
			TextEdit *typed_area = Object::cast_to<TextEdit>(p_control);
			// Both text targets insert a typed key only when the event carries a unicode point at or
			// above U+0020, and drop everything below it. Established against Godot 4.7.2 by typing each
			// of U+0001, U+0008, U+0009, U+000A, U+000B, U+000C, U+000D, U+001B and U+001F into the
			// fixture's "Order" (LineEdit) and "Notes" (TextEdit) fields: every one left the field empty,
			// while U+0020 and every point above it, U+007F included, was inserted. The two classes do
			// not differ here: a TextEdit line break comes from the Enter keycode, not from a U+000A
			// character, so a typed U+000A is dropped there too. Characters the target will not accept
			// are refused before a single event is pushed, because delivering them would report a
			// delivery the target demonstrably rejected.
			for (int i = 0; i < p_request.text.length(); i++) {
				if (p_request.text[i] < ACCEPTED_TYPED_UNICODE_FLOOR) {
					r_status = Status::INVALID_ARGUMENTS;
					r_message = "The text contains a character the element will not accept: text fields take "
								"only typed characters from U+0020 upward.";
					return false;
				}
			}
			// A read-only field drops every character handed to it, so typing into one is input the
			// target does not accept, not a delivery the editor merely chose to ignore.
			if ((typed_field != nullptr && !typed_field->is_editable()) ||
					(typed_area != nullptr && !typed_area->is_editable())) {
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The text field is not editable.";
				return false;
			}
			// LineEdit charges max_length against the whole field, not against the keystrokes: typing
			// inserts at the caret and replaces the current selection, so what the field can still accept
			// is the cap less what it already holds outside that selection. A field already at its cap
			// accepts nothing, and typing into it would report a success with no character entered.
			if (typed_field != nullptr) {
				const int max_length = typed_field->get_max_length();
				if (max_length > 0) {
					int replaced = 0;
					if (typed_field->has_selection()) {
						replaced = typed_field->get_selection_to_column() - typed_field->get_selection_from_column();
						if (replaced < 0) {
							replaced = 0;
						}
					}
					int retained = typed_field->get_text().length() - replaced;
					if (retained < 0) {
						retained = 0;
					}
					if (retained + p_request.text.length() > max_length) {
						r_status = Status::INVALID_ARGUMENTS;
						r_message = "The text field holds at most " + String::num_int64(max_length) +
								" characters and already holds " + String::num_int64(retained) +
								" the typed text would not replace.";
						return false;
					}
				}
			}
		}
		Viewport *viewport = p_control->get_viewport();
		if (viewport == nullptr) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element is not inside a viewport that accepts input.";
			return false;
		}
		Control *previous_focus = viewport->gui_get_focus_owner();
		p_control->grab_focus();
		if (!p_control->has_focus()) {
			restore_focus(previous_focus, p_control);
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element refused keyboard focus.";
			return false;
		}
		if (action == ACTION_SUBMIT) {
			push_key(viewport, KEY_ENTER, 0);
			r_route = Route::INPUT_EVENT;
			return true;
		}
		// The character count was charged against MAX_TYPED_LENGTH before this ran, so the event loop
		// below is bounded by an already-validated argument.
		for (int i = 0; i < p_request.text.length(); i++) {
			push_key(viewport, KEY_NONE, p_request.text[i]);
		}
		r_route = Route::INPUT_EVENT;
		return true;
	}

	if (action == ACTION_SET_CHECKED) {
		BaseButton *base_button = Object::cast_to<BaseButton>(p_control);
		if (base_button == nullptr || !base_button->is_toggle_mode()) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element has no toggleable checked state.";
			return false;
		}
		const bool previous_pressed = base_button->is_pressed();
		base_button->set_pressed(p_request.checked);
		if (base_button->is_pressed() != p_request.checked) {
			base_button->set_pressed(previous_pressed);
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element did not take the requested checked state.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_SET_VALUE) {
		Range *range = Object::cast_to<Range>(p_control);
		if (range == nullptr) {
			r_status = Status::UNSUPPORTED_ACTION;
			r_message = "Class '" + p_element.class_name + "' has no public value route.";
			return false;
		}
		// Range publishes the grid its value must land on and snaps to it silently, so a value the
		// element cannot hold exactly is refused rather than answered with a value the client never
		// asked for. This mirrors Range::set_value: snap to step, round, then clamp to the published
		// bounds, where the reachable maximum is max - page unless the element allows more.
		double representable = p_request.value;
		const double step = range->get_step();
		if (step > 0.0) {
			representable = Math::round((representable - range->get_min()) / step) * step + range->get_min();
		}
		if (range->is_using_rounded_values()) {
			representable = Math::round(representable);
		}
		const bool below = !range->is_lesser_allowed() && representable < range->get_min();
		const bool above = !range->is_greater_allowed() && representable > range->get_max() - range->get_page();
		if (below || above || !Math::is_equal_approx(representable, p_request.value)) {
			r_status = Status::INVALID_ARGUMENTS;
			r_message = "Value is not one the element can hold: it lies outside the published range or off "
						"the published step.";
			return false;
		}
		const double previous_value = range->get_value();
		range->set_value(p_request.value);
		if (!Math::is_equal_approx(range->get_value(), p_request.value)) {
			range->set_value(previous_value);
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element did not take the requested value.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_SELECT_ITEM) {
		ItemList *item_list = Object::cast_to<ItemList>(p_control);
		if (item_list == nullptr) {
			r_status = Status::UNSUPPORTED_ACTION;
			r_message = "Class '" + p_element.class_name + "' has no public item-selection route.";
			return false;
		}
		if (p_request.index >= item_list->get_item_count()) {
			r_status = Status::INVALID_ARGUMENTS;
			r_message = "Item index is outside the element's item count.";
			return false;
		}
		if (!item_list->is_item_selectable(p_request.index) || item_list->is_item_disabled(p_request.index)) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The item is disabled or not selectable.";
			return false;
		}
		const PackedInt32Array previous_selection = item_list->get_selected_items();
		item_list->select(p_request.index);
		if (!item_list->is_selected(p_request.index)) {
			// The refusal puts the selection back: an action reported as not performed must not leave the
			// list selecting something the client never asked for.
			item_list->deselect_all();
			for (int i = 0; i < previous_selection.size(); i++) {
				item_list->select(previous_selection[i], i == 0);
			}
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element did not select the requested item.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_SELECT_TAB) {
		TabContainer *tab_container = Object::cast_to<TabContainer>(p_control);
		TabBar *tab_bar = Object::cast_to<TabBar>(p_control);
		const int tab_count = tab_container != nullptr ? tab_container->get_tab_count()
													   : (tab_bar != nullptr ? tab_bar->get_tab_count() : 0);
		if (tab_container == nullptr && tab_bar == nullptr) {
			r_status = Status::UNSUPPORTED_ACTION;
			r_message = "Class '" + p_element.class_name + "' has no public tab route.";
			return false;
		}
		if (p_request.index >= tab_count) {
			r_status = Status::INVALID_ARGUMENTS;
			r_message = "Tab index is outside the element's tab count.";
			return false;
		}
		const bool disabled = tab_container != nullptr
				? (tab_container->is_tab_disabled(p_request.index) || tab_container->is_tab_hidden(p_request.index))
				: (tab_bar->is_tab_disabled(p_request.index) || tab_bar->is_tab_hidden(p_request.index));
		if (disabled) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The tab is disabled or hidden.";
			return false;
		}
		const int previous_tab =
				tab_container != nullptr ? tab_container->get_current_tab() : tab_bar->get_current_tab();
		if (tab_container != nullptr) {
			tab_container->set_current_tab(p_request.index);
		} else {
			tab_bar->set_current_tab(p_request.index);
		}
		const int current = tab_container != nullptr ? tab_container->get_current_tab() : tab_bar->get_current_tab();
		if (current != p_request.index) {
			// The refusal puts the element back on the tab it was showing rather than leaving it on one
			// the action could not be reported as having reached.
			if (tab_container != nullptr) {
				tab_container->set_current_tab(previous_tab);
			} else {
				tab_bar->set_current_tab(previous_tab);
			}
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element did not move to the requested tab.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_SCROLL) {
		ScrollContainer *scroll_container = Object::cast_to<ScrollContainer>(p_control);
		if (scroll_container == nullptr) {
			r_status = Status::UNSUPPORTED_ACTION;
			r_message = "Class '" + p_element.class_name + "' has no public scroll route.";
			return false;
		}
		// A container clamps a scroll offset to whatever its content actually allows, so the offset it
		// reached is read back. An offset the element cannot hold is refused with the element left at
		// the offset it had, never reported as a scroll to somewhere the client never asked for.
		const bool vertical = p_request.scroll_axis == AXIS_VERTICAL;
		const int previous = vertical ? scroll_container->get_v_scroll() : scroll_container->get_h_scroll();
		if (vertical) {
			scroll_container->set_v_scroll(p_request.scroll_offset);
		} else {
			scroll_container->set_h_scroll(p_request.scroll_offset);
		}
		const int reached = vertical ? scroll_container->get_v_scroll() : scroll_container->get_h_scroll();
		if (reached != p_request.scroll_offset) {
			if (vertical) {
				scroll_container->set_v_scroll(previous);
			} else {
				scroll_container->set_h_scroll(previous);
			}
			r_status = Status::INVALID_ARGUMENTS;
			r_message = "Scroll offset is outside the range the element can reach.";
			return false;
		}
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	// An advertised action without a route must fail closed rather than fall through as a success.
	r_status = Status::UNSUPPORTED_ACTION;
	r_message = "Action '" + action + "' has no route in this build.";
	return false;
}

} // namespace godot
