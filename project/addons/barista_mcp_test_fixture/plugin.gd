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

var _panel: PanelContainer = null


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
	column.add_child(brew)

	var disabled := Button.new()
	disabled.name = "Grind Button"
	disabled.text = "Grind"
	disabled.disabled = true
	column.add_child(disabled)

	var recipes := LinkButton.new()
	recipes.name = "Recipes Link"
	recipes.text = "Recipes"
	column.add_child(recipes)

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
	column.add_child(order)

	var notes := TextEdit.new()
	notes.name = "Notes"
	notes.custom_minimum_size = Vector2(0, 60)
	column.add_child(notes)

	var decaf := CheckBox.new()
	decaf.name = "Decaf Check"
	decaf.text = "Decaf"
	column.add_child(decaf)

	var shots := SpinBox.new()
	shots.name = "Shots"
	shots.min_value = 1.0
	shots.max_value = 4.0
	shots.step = 1.0
	shots.value = 2.0
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
	column.add_child(stations)

	base_control.add_child(_panel)


func _exit_tree() -> void:
	if _panel == null:
		return
	if is_instance_valid(_panel):
		if _panel.get_parent() != null:
			_panel.get_parent().remove_child(_panel)
		_panel.queue_free()
	_panel = null
