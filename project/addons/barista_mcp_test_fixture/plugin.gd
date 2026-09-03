# plugin.gd
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

@tool
extends EditorPlugin

## Adds a deterministic panel of stock controls below the editor base control so
## acceptance tests can assert stable roles, names, state, bounds, and actions.

const FIXTURE_NAME := "Barista Test Fixture"
## Accessibility name of the label that publishes the action counters. The label's own text changes
## as actions land, so its stable identity must not come from that text.
const COUNTERS_NAME := "Action Counters"
## Total number of plain nodes in the internal filler subtree. Zero by default so the shared test
## project is untouched; an acceptance project raises it to exercise the traversal budget against
## many parents that each hold a wide child list.
const WIDE_INTERNAL_CHILDREN_SETTING := "barista_mcp_test_fixture/wide_internal_children"
const WIDE_INTERNAL_GROUP_SIZE := 250
## Name of the control that exists only as an internal child of the fixture panel. It is reachable
## through a capture that includes internal children and through no other capture, so a verdict about
## its presence or absence is only sound when the verdict was decided over that wider domain.
const INTERNAL_ONLY_NAME := "Internal Only Field"
## The embedded window and the two focusable fields inside it. Godot reports both an active window
## and the control that owns the keyboard as focused, so focus moving between these two fields must
## be observable as a change of the reported focus owner.
const DIALOG_NAME := "Fixture Dialog"
const DIALOG_TOGGLE_NAME := "Dialog Visible"
const DIALOG_FIELD_A := "Dialog Field A"
const DIALOG_FIELD_B := "Dialog Field B"
## Length cap published by the "Ticket" field, so a test can request text the field can never hold.
const TICKET_MAX_LENGTH := 8

var _panel: PanelContainer = null
var _counters: Label = null
var _dialog: Window = null
## Counts the editor signals the signalling actions are expected to raise, published as label text so
## an acceptance test observes an action through a fresh snapshot rather than through the tool's own
## report of itself. Text actions raise no signal that a control method and an input event share, so
## they are observed through the captured element text instead.
var _counts := {
	"clicks": 0,
	"submits": 0,
	"toggles": 0,
	"value_changes": 0,
	"tab_changes": 0,
	## Input events the covering overlay itself received. A refused click must leave this untouched:
	## the refusal is decided before anything is dispatched, so the overlay observes nothing at all.
	"shield_input": 0,
	## Presses the right-button-only button received, and submissions the read-only field received.
	## Both stay at zero: an action that the target would not accept is refused, never delivered.
	"right_only_clicks": 0,
	"receipt_submits": 0,
}


func _enter_tree() -> void:
	var base_control := EditorInterface.get_base_control()
	if base_control == null:
		push_error("BaristaMCP test fixture: no editor base control available.")
		return

	_panel = PanelContainer.new()
	_panel.name = FIXTURE_NAME
	_panel.set_accessibility_name(FIXTURE_NAME)
	_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_panel.position = Vector2(24, 24)
	_panel.size = Vector2(320, 480)

	var column := VBoxContainer.new()
	column.name = "Fixture Controls"
	_panel.add_child(column)

	var brew := Button.new()
	brew.name = "Brew Button"
	brew.text = "Brew"
	brew.pressed.connect(_bump.bind("clicks"))
	column.add_child(brew)

	var disabled := Button.new()
	disabled.name = "Grind Button"
	disabled.text = "Grind"
	disabled.disabled = true
	column.add_child(disabled)

	## A button whose own centre is covered by a child that stops mouse input. The child, not the
	## button, is what the editor would hand a synthesized click to, so a click on the button must be
	## refused rather than delivered to something the client never selected.
	var shielded := Button.new()
	shielded.name = "Shielded Button"
	shielded.text = "Shielded"
	shielded.custom_minimum_size = Vector2(0, 32)
	var shield := ColorRect.new()
	shield.name = "Shield Overlay"
	shield.color = Color(0.0, 0.0, 0.0, 0.0)
	shield.mouse_filter = Control.MOUSE_FILTER_STOP
	shield.set_anchors_preset(Control.PRESET_FULL_RECT)
	shield.gui_input.connect(func(_event: InputEvent) -> void: _bump("shield_input"))
	shielded.add_child(shield)
	column.add_child(shielded)

	## A button configured to react to the right mouse button only. A synthesized left press is
	## dropped by BaseButton before any handler sees it, so delivering one would be a delivery this
	## target never accepts and the click must be refused instead.
	var right_only := Button.new()
	right_only.name = "Right Only Button"
	right_only.text = "Right Only"
	right_only.button_mask = MOUSE_BUTTON_MASK_RIGHT
	right_only.pressed.connect(_bump.bind("right_only_clicks"))
	column.add_child(right_only)

	var recipes := LinkButton.new()
	recipes.name = "Recipes Link"
	recipes.text = "Recipes"
	column.add_child(recipes)

	var origin := OptionButton.new()
	origin.name = "Origin Select"
	origin.set_accessibility_name("Origin Select")
	origin.add_item("Colombia")
	origin.add_item("Ethiopia")
	column.add_child(origin)

	var actions_menu := MenuButton.new()
	actions_menu.name = "Actions Menu"
	actions_menu.set_accessibility_name("Actions Menu")
	actions_menu.text = "Actions"
	actions_menu.get_popup().add_item("Refill")
	column.add_child(actions_menu)

	var grind := HSlider.new()
	grind.name = "Grind Size"
	grind.min_value = 1.0
	grind.max_value = 10.0
	grind.value = 5.0
	grind.editable = false
	column.add_child(grind)

	var order := LineEdit.new()
	order.name = "Order"
	order.placeholder_text = "Order"
	order.text_submitted.connect(func(_text: String) -> void: _bump("submits"))
	column.add_child(order)

	var passcode := LineEdit.new()
	passcode.name = "Passcode"
	passcode.secret = true
	passcode.text = "roasted-secret"
	column.add_child(passcode)

	## A field that publishes its own length cap. Text longer than the cap can never be held, so an
	## action that wrote it would report a value the client never asked for.
	var ticket := LineEdit.new()
	ticket.name = "Ticket"
	ticket.max_length = TICKET_MAX_LENGTH
	column.add_child(ticket)

	## A read-only field. It drops every character handed to it, so typing into it can never be a
	## delivery it accepts, while a submission still reaches its own key handling.
	var receipt := LineEdit.new()
	receipt.name = "Receipt"
	receipt.editable = false
	receipt.text = "receipt"
	receipt.text_submitted.connect(func(_text: String) -> void: _bump("receipt_submits"))
	column.add_child(receipt)

	## A focusable field under an ancestor that disables focus for its whole subtree. The field still
	## advertises "focus" from its own focus mode, but no call can make it own focus, so a focus
	## action on it must fail rather than report success against an element that never took focus.
	var sealed_group := VBoxContainer.new()
	sealed_group.name = "Sealed Group"
	sealed_group.focus_behavior_recursive = Control.FOCUS_BEHAVIOR_DISABLED
	var sealed_field := LineEdit.new()
	sealed_field.name = "Sealed Field"
	sealed_group.add_child(sealed_field)
	column.add_child(sealed_group)

	var notes := TextEdit.new()
	notes.name = "Notes"
	notes.custom_minimum_size = Vector2(0, 60)
	column.add_child(notes)

	var decaf := CheckBox.new()
	decaf.name = "Decaf Check"
	decaf.text = "Decaf"
	decaf.toggled.connect(func(_pressed: bool) -> void: _bump("toggles"))
	column.add_child(decaf)

	var shots := SpinBox.new()
	shots.name = "Shots"
	shots.min_value = 1.0
	shots.max_value = 4.0
	shots.step = 1.0
	shots.value = 2.0
	shots.value_changed.connect(func(_value: float) -> void: _bump("value_changes"))
	column.add_child(shots)

	var beans := ItemList.new()
	beans.name = "Beans"
	beans.custom_minimum_size = Vector2(0, 60)
	beans.add_item("Arabica")
	beans.add_item("Robusta")
	column.add_child(beans)

	var roasts := Tree.new()
	roasts.name = "Roasts"
	roasts.custom_minimum_size = Vector2(0, 60)
	var root := roasts.create_item()
	root.set_text(0, "Roasts")
	var light := roasts.create_item(root)
	light.set_text(0, "Light")
	column.add_child(roasts)

	var stations := TabContainer.new()
	stations.name = "Stations"
	stations.custom_minimum_size = Vector2(0, 60)
	var espresso := Control.new()
	espresso.name = "Espresso"
	stations.add_child(espresso)
	var pour_over := Control.new()
	pour_over.name = "Pour Over"
	stations.add_child(pour_over)
	stations.tab_changed.connect(func(_tab: int) -> void: _bump("tab_changes"))
	column.add_child(stations)

	var menu := ScrollContainer.new()
	menu.name = "Menu Scroll"
	menu.set_accessibility_name("Menu Scroll")
	menu.custom_minimum_size = Vector2(0, 40)
	var menu_items := VBoxContainer.new()
	menu_items.name = "Menu Items"
	for index in 8:
		var item := Label.new()
		item.name = "Menu Item %d" % index
		item.text = "Menu Item %d" % index
		menu_items.add_child(item)
	menu.add_child(menu_items)
	column.add_child(menu)

	_counters = Label.new()
	_counters.name = "Action Counters"
	_counters.set_accessibility_name(COUNTERS_NAME)
	column.add_child(_counters)
	_publish_counts()

	## Shows and hides the embedded window below. The window stays hidden until a test asks for it, so
	## the default editor surface every other test observes is unchanged.
	var dialog_toggle := CheckBox.new()
	dialog_toggle.name = DIALOG_TOGGLE_NAME
	dialog_toggle.set_accessibility_name(DIALOG_TOGGLE_NAME)
	dialog_toggle.toggled.connect(_set_dialog_visible)
	column.add_child(dialog_toggle)

	## A real control reachable only through a capture that includes internal children. Nothing about
	## it is hidden from the editor: it is simply outside the default capture domain.
	var internal_only := LineEdit.new()
	internal_only.name = INTERNAL_ONLY_NAME
	internal_only.set_accessibility_name(INTERNAL_ONLY_NAME)
	column.add_child(internal_only, false, Node.INTERNAL_MODE_BACK)

	_add_wide_internal_subtree()

	base_control.add_child(_panel)
	_add_dialog(base_control)


## Builds a large internal subtree of plain nodes: many parents, each holding a wide child list.
## Nodes that are neither controls nor windows emit no element yet still consume traversal budget,
## so this is the shape that forces a snapshot to run out of budget part way through a child scan.
func _add_wide_internal_subtree() -> void:
	var total := int(ProjectSettings.get_setting(WIDE_INTERNAL_CHILDREN_SETTING, 0))
	if total <= 0:
		return
	var filler_root := Node.new()
	filler_root.name = "Filler Root"
	var group: Node = null
	for index in total:
		if index % WIDE_INTERNAL_GROUP_SIZE == 0:
			group = Node.new()
			group.name = "Filler Group %d" % (index / WIDE_INTERNAL_GROUP_SIZE)
			filler_root.add_child(group)
		var filler := Node.new()
		filler.name = "Filler %d" % index
		group.add_child(filler)
	_panel.add_child(filler_root, false, Node.INTERNAL_MODE_BACK)


## Builds an embedded window holding two focusable fields. The window is what the engine reports as
## the active window while either field owns the keyboard, so it is the shape that separates "the
## focused window" from "the control that actually holds focus".
func _add_dialog(base_control: Control) -> void:
	_dialog = Window.new()
	_dialog.name = DIALOG_NAME
	_dialog.title = DIALOG_NAME
	_dialog.position = Vector2i(400, 24)
	_dialog.size = Vector2i(240, 120)
	var fields := VBoxContainer.new()
	fields.name = "Dialog Fields"
	var field_a := LineEdit.new()
	field_a.name = DIALOG_FIELD_A
	field_a.set_accessibility_name(DIALOG_FIELD_A)
	fields.add_child(field_a)
	var field_b := LineEdit.new()
	field_b.name = DIALOG_FIELD_B
	field_b.set_accessibility_name(DIALOG_FIELD_B)
	fields.add_child(field_b)
	_dialog.add_child(fields)
	_dialog.hide()
	base_control.add_child(_dialog)


func _set_dialog_visible(visible: bool) -> void:
	if _dialog != null and is_instance_valid(_dialog):
		_dialog.visible = visible


func _bump(counter: String) -> void:
	_counts[counter] = int(_counts[counter]) + 1
	_publish_counts()


func _publish_counts() -> void:
	if _counters == null or not is_instance_valid(_counters):
		return
	var parts: PackedStringArray = []
	for counter in _counts:
		parts.append("%s=%d" % [counter, int(_counts[counter])])
	_counters.text = " ".join(parts)


func _exit_tree() -> void:
	if _dialog != null:
		if is_instance_valid(_dialog):
			if _dialog.get_parent() != null:
				_dialog.get_parent().remove_child(_dialog)
			_dialog.queue_free()
		_dialog = null
	if _panel == null:
		return
	_counters = null
	if is_instance_valid(_panel):
		if _panel.get_parent() != null:
			_panel.get_parent().remove_child(_panel)
		_panel.queue_free()
	_panel = null
