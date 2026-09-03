/**************************************************************************/
/*  editor_snapshot.cpp                                                   */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_snapshot.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/link_button.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/slider.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <unordered_set>

namespace godot {

namespace {

// Documented public control types mapped to Barista roles, most derived first. Any control that
// matches no rule is reported as the generic "control" role rather than guessing semantics.
struct RoleRule {
	const char *class_name;
	const char *role;
};

const RoleRule ROLE_RULES[] = {
		{"CheckBox", "checkbox"},
		{"CheckButton", "checkbox"},
		{"OptionButton", "option_button"},
		{"MenuButton", "menu_button"},
		{"LinkButton", "button"},
		{"Button", "button"},
		{"BaseButton", "button"},
		{"CodeEdit", "code_editor"},
		{"TextEdit", "text_area"},
		{"LineEdit", "text_field"},
		{"RichTextLabel", "label"},
		{"Label", "label"},
		{"Tree", "tree"},
		{"ItemList", "list"},
		{"SpinBox", "spin_box"},
		{"Slider", "slider"},
		{"ProgressBar", "progress_bar"},
		{"TabContainer", "tab_container"},
		{"TabBar", "tab_bar"},
		{"PopupMenu", "menu"},
		{"ScrollContainer", "scroll_container"},
		{"SubViewportContainer", "viewport_container"},
		{"AcceptDialog", "dialog"},
		{"Window", "window"},
		{"Control", "control"},
};

// Actions a role can support through documented public APIs. Element-level state narrows this set;
// it is never widened.
struct ActionRule {
	const char *role;
	const char *actions[5];
};

const ActionRule ACTION_RULES[] = {
		{"button", {"click", "focus", nullptr, nullptr, nullptr}},
		{"checkbox", {"click", "focus", "set_checked", nullptr, nullptr}},
		{"option_button", {"click", "focus", nullptr, nullptr, nullptr}},
		{"menu_button", {"click", "focus", nullptr, nullptr, nullptr}},
		{"text_field", {"focus", "set_text", "type_text", "submit", nullptr}},
		{"text_area", {"focus", "set_text", "type_text", nullptr, nullptr}},
		{"code_editor", {"focus", "set_text", "type_text", nullptr, nullptr}},
		{"spin_box", {"focus", "set_value", nullptr, nullptr, nullptr}},
		{"slider", {"focus", "set_value", nullptr, nullptr, nullptr}},
		{"list", {"focus", "select_item", nullptr, nullptr, nullptr}},
		{"tree", {"focus", "select_item", nullptr, nullptr, nullptr}},
		{"tab_container", {"focus", "select_tab", nullptr, nullptr, nullptr}},
		{"tab_bar", {"focus", "select_tab", nullptr, nullptr, nullptr}},
		{"scroll_container", {"scroll", nullptr, nullptr, nullptr, nullptr}},
};

String bounded(const String &p_value, int p_limit) {
	if (p_value.length() <= p_limit) {
		return p_value;
	}
	return p_value.substr(0, p_limit);
}

String role_for(Object *p_object) {
	for (const RoleRule &rule : ROLE_RULES) {
		if (p_object->is_class(rule.class_name)) {
			return rule.role;
		}
	}
	return "control";
}

PackedStringArray actions_for(const String &p_role, bool p_enabled, bool p_focusable) {
	PackedStringArray actions;
	if (!p_enabled) {
		return actions;
	}
	for (const ActionRule &rule : ACTION_RULES) {
		if (p_role != rule.role) {
			continue;
		}
		for (const char *action : rule.actions) {
			if (action == nullptr) {
				break;
			}
			if (!p_focusable && String(action) == "focus") {
				continue;
			}
			actions.push_back(action);
		}
		break;
	}
	return actions;
}

// Names prefer the public accessibility name, then displayed text or a window title, then the
// tooltip, and only then the node name. Editable text is deliberately excluded because it changes
// as the editor is used.
String display_text(Control *p_control, const String &p_role) {
	if (p_role == "text_field") {
		LineEdit *line_edit = Object::cast_to<LineEdit>(p_control);
		if (line_edit == nullptr) {
			return String();
		}
		// A secret field is masked on screen; a snapshot must not disclose what the editor hides.
		return line_edit->is_secret() ? String() : line_edit->get_text();
	}
	if (p_role == "text_area" || p_role == "code_editor") {
		TextEdit *text_edit = Object::cast_to<TextEdit>(p_control);
		return text_edit != nullptr ? text_edit->get_text() : String();
	}
	Button *button = Object::cast_to<Button>(p_control);
	if (button != nullptr) {
		return button->get_text();
	}
	// LinkButton is a sibling of Button under BaseButton, so its label needs its own accessor.
	LinkButton *link_button = Object::cast_to<LinkButton>(p_control);
	if (link_button != nullptr) {
		return link_button->get_text();
	}
	Label *label = Object::cast_to<Label>(p_control);
	if (label != nullptr) {
		return label->get_text();
	}
	RichTextLabel *rich_label = Object::cast_to<RichTextLabel>(p_control);
	if (rich_label != nullptr) {
		return rich_label->get_text();
	}
	return String();
}

bool text_is_a_name(const String &p_role) {
	return p_role != "text_field" && p_role != "text_area" && p_role != "code_editor";
}

Dictionary control_state(Control *p_control, const String &p_role) {
	Dictionary state;
	BaseButton *base_button = Object::cast_to<BaseButton>(p_control);
	if (base_button != nullptr) {
		state["pressed"] = base_button->is_pressed();
		state["toggle_mode"] = base_button->is_toggle_mode();
	}
	OptionButton *option_button = Object::cast_to<OptionButton>(p_control);
	if (option_button != nullptr) {
		state["selected_index"] = option_button->get_selected();
		state["item_count"] = option_button->get_item_count();
	}
	MenuButton *menu_button = Object::cast_to<MenuButton>(p_control);
	if (menu_button != nullptr) {
		state["item_count"] = menu_button->get_item_count();
	}
	LineEdit *line_edit = Object::cast_to<LineEdit>(p_control);
	if (line_edit != nullptr && p_role == "text_field") {
		state["editable"] = line_edit->is_editable();
		state["secret"] = line_edit->is_secret();
		state["text_length"] = line_edit->get_text().length();
	}
	TextEdit *text_edit = Object::cast_to<TextEdit>(p_control);
	if (text_edit != nullptr) {
		state["editable"] = text_edit->is_editable();
		state["text_length"] = text_edit->get_text().length();
	}
	// SpinBox, sliders, and progress bars all derive from Range, so one accessor covers them.
	Range *range = Object::cast_to<Range>(p_control);
	if (range != nullptr) {
		state["value"] = range->get_value();
		state["min_value"] = range->get_min();
		state["max_value"] = range->get_max();
		state["step"] = range->get_step();
	}
	ItemList *item_list = Object::cast_to<ItemList>(p_control);
	if (item_list != nullptr) {
		state["item_count"] = item_list->get_item_count();
		// get_selected_items() is the only public accessor for the selection and it returns a fresh
		// array, so the cap below can only be applied after the fact. The array it builds is bounded by
		// one control's item count and no request argument widens it.
		const PackedInt32Array selected = item_list->get_selected_items();
		Array selected_items;
		for (int i = 0; i < selected.size() && i < EditorSnapshotLimits::MAX_SELECTED_ITEMS; i++) {
			selected_items.push_back(selected[i]);
		}
		state["selected_items"] = selected_items;
	}
	TabContainer *tab_container = Object::cast_to<TabContainer>(p_control);
	if (tab_container != nullptr) {
		state["tab_count"] = tab_container->get_tab_count();
		state["current_tab"] = tab_container->get_current_tab();
	}
	TabBar *tab_bar = Object::cast_to<TabBar>(p_control);
	if (tab_bar != nullptr) {
		state["tab_count"] = tab_bar->get_tab_count();
		state["current_tab"] = tab_bar->get_current_tab();
	}
	return state;
}

Dictionary window_state(Window *p_window) {
	Dictionary state;
	state["title"] = bounded(p_window->get_title(), EditorSnapshotLimits::MAX_STRING_LENGTH);
	// PopupMenu and AcceptDialog derive from Window, so their public item and message state is only
	// reachable on this branch.
	PopupMenu *popup_menu = Object::cast_to<PopupMenu>(p_window);
	if (popup_menu != nullptr) {
		state["item_count"] = popup_menu->get_item_count();
	}
	AcceptDialog *dialog = Object::cast_to<AcceptDialog>(p_window);
	if (dialog != nullptr) {
		state["message"] = bounded(dialog->get_text(), EditorSnapshotLimits::MAX_STRING_LENGTH);
	}
	return state;
}

bool control_is_enabled(Control *p_control) {
	BaseButton *base_button = Object::cast_to<BaseButton>(p_control);
	if (base_button != nullptr) {
		return !base_button->is_disabled();
	}
	SpinBox *spin_box = Object::cast_to<SpinBox>(p_control);
	if (spin_box != nullptr) {
		return spin_box->is_editable();
	}
	Slider *slider = Object::cast_to<Slider>(p_control);
	if (slider != nullptr) {
		return slider->is_editable();
	}
	LineEdit *line_edit = Object::cast_to<LineEdit>(p_control);
	if (line_edit != nullptr) {
		return line_edit->is_editable();
	}
	TextEdit *text_edit = Object::cast_to<TextEdit>(p_control);
	if (text_edit != nullptr) {
		return text_edit->is_editable();
	}
	return true;
}

class SnapshotBuilder {
	const EditorSnapshotOptions &options;
	EditorSnapshotData &data;
	int visited_nodes = 0;

public:
	SnapshotBuilder(const EditorSnapshotOptions &p_options, EditorSnapshotData &r_data)
			: options(p_options), data(r_data) {}

	// Collects the emitted descendants of p_parent. Nodes that are neither controls nor windows are
	// transparent: their children are collected at the same depth so no public control is lost.
	void collect_children(Node *p_parent, int p_depth, int p_node_depth, std::vector<EditorElement> &r_children,
			const String &p_path, bool p_inherited_internal, bool &r_truncated) {
		if (p_parent == nullptr) {
			return;
		}
		// Nodes that emit no element still consume traversal budget, so an unusually deep chain of
		// plain nodes cannot recurse without bound.
		if (p_node_depth >= EditorSnapshotLimits::MAX_TRAVERSAL_DEPTH) {
			data.traversal_limit_reached = true;
			r_truncated = true;
			return;
		}
		// Scanning one parent's child list is itself work proportional to that list, so it is charged
		// and bounded before any listing is walked. Otherwise a parent with an enormous child list
		// keeps costing after the traversal budget is spent.
		if (visited_nodes >= EditorSnapshotLimits::MAX_VISITED_NODES) {
			data.traversal_limit_reached = true;
			r_truncated = true;
			return;
		}
		visited_nodes++;
		const int remaining_budget = EditorSnapshotLimits::MAX_VISITED_NODES - visited_nodes;

		// Children are read one index at a time rather than through get_children(), which would build a
		// listing of every child before the budget above could bound it. get_child_count() is a counter
		// read, so no container proportional to the child list is ever materialized.
		std::unordered_set<uint64_t> public_children;
		if (options.include_internal) {
			const int32_t visible_count = p_parent->get_child_count(false);
			const int scanned = visible_count < remaining_budget ? (int)visible_count : remaining_budget;
			for (int i = 0; i < scanned; i++) {
				Node *child = p_parent->get_child(i, false);
				if (child != nullptr) {
					public_children.insert((uint64_t)child->get_instance_id());
				}
			}
			if (scanned < visible_count) {
				// An incomplete public listing cannot classify the remaining children, so stop here
				// rather than publish a guess about which of them are internal.
				data.traversal_limit_reached = true;
				r_truncated = true;
				return;
			}
		}

		const int32_t child_count = p_parent->get_child_count(options.include_internal);
		const int visitable = child_count < remaining_budget ? (int)child_count : remaining_budget;
		if (visitable < child_count) {
			data.traversal_limit_reached = true;
			r_truncated = true;
		}
		for (int i = 0; i < visitable; i++) {
			// A node can leave the tree between the count and this read; skip it instead of
			// dereferencing a stale entry.
			Node *child = p_parent->get_child(i, options.include_internal);
			if (child == nullptr) {
				continue;
			}
			// Anything reachable only because include_internal was set stays marked internal, including
			// the public subtree of an internal node and children flattened through non-control nodes.
			const bool is_internal = p_inherited_internal ||
					(options.include_internal &&
							public_children.find((uint64_t)child->get_instance_id()) == public_children.end());
			add_node(child, p_depth, p_node_depth, r_children, p_path, is_internal, r_truncated);
			if (data.element_limit_reached || data.traversal_limit_reached) {
				r_truncated = true;
				return;
			}
		}
	}

	void add_node(Node *p_node, int p_depth, int p_node_depth, std::vector<EditorElement> &r_children,
			const String &p_path, bool p_internal, bool &r_truncated) {
		// Every visited node costs budget, whether or not it becomes an element.
		if (visited_nodes >= EditorSnapshotLimits::MAX_VISITED_NODES) {
			data.traversal_limit_reached = true;
			r_truncated = true;
			return;
		}
		visited_nodes++;

		Control *control = Object::cast_to<Control>(p_node);
		Window *window = Object::cast_to<Window>(p_node);
		if (control == nullptr && window == nullptr) {
			collect_children(p_node, p_depth, p_node_depth + 1, r_children, p_path, p_internal, r_truncated);
			return;
		}
		if (control != nullptr && !control->is_visible_in_tree()) {
			return;
		}
		if (window != nullptr && !window->is_visible()) {
			return;
		}
		if (data.element_count >= options.max_elements) {
			data.element_limit_reached = true;
			r_truncated = true;
			return;
		}

		EditorElement element;
		const uint64_t instance_id = (uint64_t)p_node->get_instance_id();
		element.handle = "el:" + String::num_uint64(instance_id);
		element.id = "s" + String::num_uint64(data.generation) + ":" + String::num_uint64(instance_id);
		element.class_name = p_node->get_class();
		element.internal = p_internal;
		element.path =
				bounded(p_path + String("/") + String(p_node->get_name()), EditorSnapshotLimits::MAX_PATH_LENGTH);

		if (control != nullptr) {
			element.role = role_for(control);
			const String text = display_text(control, element.role);
			element.text = bounded(text, EditorSnapshotLimits::MAX_STRING_LENGTH);
			String name = control->get_accessibility_name();
			if (name.is_empty() && text_is_a_name(element.role)) {
				name = text;
			}
			if (name.is_empty()) {
				name = control->get_tooltip_text();
			}
			if (name.is_empty()) {
				name = String(p_node->get_name());
			}
			element.name = bounded(name, EditorSnapshotLimits::MAX_STRING_LENGTH);
			element.visible = true;
			element.enabled = control_is_enabled(control);
			element.focused = control->has_focus();
			element.bounds = control->get_global_rect();
			element.state = control_state(control, element.role);
			element.actions =
					actions_for(element.role, element.enabled, control->get_focus_mode() != Control::FOCUS_NONE);
		} else {
			element.role = role_for(window);
			String name = window->get_title();
			if (name.is_empty()) {
				name = String(p_node->get_name());
			}
			element.name = bounded(name, EditorSnapshotLimits::MAX_STRING_LENGTH);
			element.text = bounded(window->get_title(), EditorSnapshotLimits::MAX_STRING_LENGTH);
			element.visible = true;
			element.enabled = true;
			element.focused = window->has_focus();
			element.bounds = Rect2(window->get_position(), window->get_size());
			element.state = window_state(window);
		}

		data.element_count++;
		if (element.focused) {
			data.focused_element_id = element.id;
		}

		if (p_depth + 1 >= options.max_depth) {
			if (p_node->get_child_count(options.include_internal) > 0) {
				element.truncated = true;
				data.depth_truncated = true;
			}
		} else {
			bool child_truncated = false;
			collect_children(
					p_node, p_depth + 1, p_node_depth + 1, element.children, element.path, p_internal, child_truncated);
			element.truncated = child_truncated;
		}
		r_children.push_back(element);
	}
};

Array serialize_elements(const std::vector<EditorElement> &p_elements) {
	Array serialized;
	for (const EditorElement &element : p_elements) {
		serialized.push_back(EditorSnapshot::serialize_element(element, true));
	}
	return serialized;
}

} // namespace

Dictionary EditorSnapshot::serialize_element(const EditorElement &p_element, bool p_include_children) {
	const EditorElement &element = p_element;
	Dictionary entry;
	entry["id"] = element.id;
	entry["handle"] = element.handle;
	entry["role"] = element.role;
	entry["name"] = element.name;
	entry["text"] = element.text;
	entry["class"] = element.class_name;
	entry["path"] = element.path;
	entry["visible"] = element.visible;
	entry["enabled"] = element.enabled;
	entry["focused"] = element.focused;
	entry["internal"] = element.internal;
	entry["truncated"] = element.truncated;
	Array bounds;
	bounds.push_back(element.bounds.position.x);
	bounds.push_back(element.bounds.position.y);
	bounds.push_back(element.bounds.size.x);
	bounds.push_back(element.bounds.size.y);
	entry["bounds"] = bounds;
	Array actions;
	for (int i = 0; i < element.actions.size(); i++) {
		actions.push_back(element.actions[i]);
	}
	entry["actions"] = actions;
	entry["state"] = element.state;
	entry["children"] = p_include_children ? serialize_elements(element.children) : Array();
	return entry;
}

PackedStringArray EditorSnapshot::role_vocabulary() {
	PackedStringArray roles;
	for (const RoleRule &rule : ROLE_RULES) {
		if (!roles.has(rule.role)) {
			roles.push_back(rule.role);
		}
	}
	return roles;
}

PackedStringArray EditorSnapshot::action_vocabulary() {
	PackedStringArray actions;
	for (const ActionRule &rule : ACTION_RULES) {
		for (const char *action : rule.actions) {
			if (action == nullptr) {
				break;
			}
			if (!actions.has(action)) {
				actions.push_back(action);
			}
		}
	}
	return actions;
}

bool EditorSnapshot::capture(EditorInterface *p_editor_interface, uint64_t p_generation,
		const EditorSnapshotOptions &p_options, EditorSnapshotData &r_data) {
	if (p_editor_interface == nullptr) {
		return false;
	}
	Control *base_control = p_editor_interface->get_base_control();
	if (base_control == nullptr) {
		return false;
	}

	r_data = EditorSnapshotData();
	r_data.generation = p_generation;
	r_data.applied_options = p_options;

	SnapshotBuilder builder(p_options, r_data);
	bool truncated = false;
	std::vector<EditorElement> roots;
	// The base control is the single snapshot root, so every element hangs off one stable public node.
	builder.add_node(base_control, 0, 0, roots, String(), false, truncated);
	r_data.roots = roots;
	return true;
}

Dictionary EditorSnapshot::serialize(const EditorSnapshotData &p_data) {
	Dictionary limits;
	limits["max_depth"] = p_data.requested_options.max_depth;
	limits["max_elements"] = p_data.requested_options.max_elements;
	limits["max_elements_applied"] = p_data.applied_options.max_elements;
	limits["include_internal"] = p_data.requested_options.include_internal;
	limits["depth_truncated"] = p_data.depth_truncated;
	limits["element_limit_reached"] = p_data.element_limit_reached;
	limits["traversal_limit_reached"] = p_data.traversal_limit_reached;
	limits["payload_limit_bytes"] = EditorSnapshotLimits::MAX_PAYLOAD_BYTES;

	Dictionary snapshot;
	snapshot["generation"] = (int64_t)p_data.generation;
	snapshot["focused_element_id"] = p_data.focused_element_id;
	snapshot["element_count"] = p_data.element_count;
	snapshot["truncated"] = p_data.depth_truncated || p_data.element_limit_reached || p_data.traversal_limit_reached;
	snapshot["limits"] = limits;
	snapshot["tree"] = serialize_elements(p_data.roots);
	return snapshot;
}

} // namespace godot
