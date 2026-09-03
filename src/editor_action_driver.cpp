/**************************************************************************/
/*  editor_action_driver.cpp                                              */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_action_driver.h"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
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

// Pushes one already-built event through the control's own viewport, which is the documented public
// input route. Returns false when the control is not inside a viewport.
bool push_event(Control *p_control, const Ref<InputEvent> &p_event) {
	Viewport *viewport = p_control->get_viewport();
	if (viewport == nullptr) {
		return false;
	}
	viewport->push_input(p_event, true);
	return true;
}

// Moves the pointer over the element and reports whether the element, or one of its descendants, is
// what the editor would actually deliver a click to. A click that would land on another control is
// refused rather than mutating the wrong element.
bool hover_reaches(Control *p_control, const Vector2 &p_point) {
	Ref<InputEventMouseMotion> motion;
	motion.instantiate();
	motion->set_position(p_point);
	motion->set_global_position(p_point);
	if (!push_event(p_control, motion)) {
		return false;
	}
	Viewport *viewport = p_control->get_viewport();
	Control *hovered = viewport->gui_get_hovered_control();
	if (hovered == nullptr) {
		return false;
	}
	return hovered == p_control || p_control->is_ancestor_of(hovered);
}

bool push_click(Control *p_control, const Vector2 &p_point) {
	for (int press = 1; press >= 0; press--) {
		Ref<InputEventMouseButton> event;
		event.instantiate();
		event->set_button_index(MOUSE_BUTTON_LEFT);
		event->set_button_mask(press == 1 ? MOUSE_BUTTON_MASK_LEFT : MouseButtonMask(0));
		event->set_pressed(press == 1);
		event->set_position(p_point);
		event->set_global_position(p_point);
		if (!push_event(p_control, event)) {
			return false;
		}
	}
	return true;
}

bool push_key(Control *p_control, Key p_keycode, char32_t p_unicode) {
	for (int press = 1; press >= 0; press--) {
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_pressed(press == 1);
		event->set_keycode(p_keycode);
		event->set_unicode(p_unicode);
		if (!push_event(p_control, event)) {
			return false;
		}
	}
	return true;
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
		p_control->grab_focus();
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
		const Vector2 point = rect.get_center();
		if (!hover_reaches(p_control, point)) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element does not receive pointer input at its own center.";
			return false;
		}
		if (!push_click(p_control, point)) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element is not inside a viewport that accepts input.";
			return false;
		}
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
			line_edit->set_text(p_request.text);
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
			text_edit->set_text(p_request.text);
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
		if (p_control->get_focus_mode() == Control::FOCUS_NONE) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element cannot take keyboard focus.";
			return false;
		}
		p_control->grab_focus();
		if (!p_control->has_focus()) {
			r_status = Status::ELEMENT_NOT_INTERACTABLE;
			r_message = "The element refused keyboard focus.";
			return false;
		}
		if (action == ACTION_SUBMIT) {
			if (!push_key(p_control, KEY_ENTER, 0)) {
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The element is not inside a viewport that accepts input.";
				return false;
			}
			r_route = Route::INPUT_EVENT;
			return true;
		}
		// The character count was charged against MAX_TYPED_LENGTH before this ran, so the event loop
		// below is bounded by an already-validated argument.
		for (int i = 0; i < p_request.text.length(); i++) {
			if (!push_key(p_control, KEY_NONE, p_request.text[i])) {
				r_status = Status::ELEMENT_NOT_INTERACTABLE;
				r_message = "The element is not inside a viewport that accepts input.";
				return false;
			}
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
		base_button->set_pressed(p_request.checked);
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
		// Range clamps silently, so an out-of-range request is refused rather than answered with a
		// value the client never asked for.
		if (p_request.value < range->get_min() || p_request.value > range->get_max()) {
			r_status = Status::INVALID_ARGUMENTS;
			r_message = "Value is outside the published range of the element.";
			return false;
		}
		range->set_value(p_request.value);
		r_route = Route::CONTROL_METHOD;
		return true;
	}

	if (action == ACTION_SELECT_ITEM) {
		ItemList *item_list = Object::cast_to<ItemList>(p_control);
		if (item_list == nullptr) {
			// Tree selection has no bounded public route that does not walk TreeItem objects, so it is
			// omitted from this build rather than approximated.
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
		item_list->select(p_request.index);
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
		if (tab_container != nullptr) {
			tab_container->set_current_tab(p_request.index);
		} else {
			tab_bar->set_current_tab(p_request.index);
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
		if (p_request.scroll_axis == AXIS_VERTICAL) {
			scroll_container->set_v_scroll(p_request.scroll_offset);
		} else {
			scroll_container->set_h_scroll(p_request.scroll_offset);
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
