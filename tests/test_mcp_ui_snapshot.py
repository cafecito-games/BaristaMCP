# test_mcp_ui_snapshot.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import ROOT, EditorProcess, MCPClient  # noqa: E402

PINNED_EXTENSION_API = ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json"
BUILD_PROFILE = ROOT / "build_profile.json"
FIXTURE_NAME = "Barista Test Fixture"
MAX_SNAPSHOT_PAYLOAD_BYTES = 256 * 1024


def walk(elements: list[dict[str, Any]]) -> Iterator[dict[str, Any]]:
    for element in elements:
        yield element
        yield from walk(element["children"])


def find_all(elements: list[dict[str, Any]], **fields: Any) -> list[dict[str, Any]]:
    return [
        element
        for element in walk(elements)
        if all(element.get(key) == value for key, value in fields.items())
    ]


def find_one(elements: list[dict[str, Any]], **fields: Any) -> dict[str, Any]:
    matches = find_all(elements, **fields)
    if len(matches) != 1:
        raise AssertionError(f"Expected exactly one match for {fields!r}, found {len(matches)}")
    return matches[0]


class BaristaMCPUISnapshotTests(unittest.TestCase):
    editor: EditorProcess
    client: MCPClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.editor = EditorProcess()
        try:
            cls.editor.start()
            cls.client = MCPClient(cls.editor.wait_for_discovery())
            cls.client.initialize()
        except BaseException:
            cls.editor.stop()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.editor.stop()

    def _tool_schema(self) -> dict[str, Any]:
        tools = self.client.rpc("tools/list", {})["result"]["tools"]
        for tool in tools:
            if tool["name"] == "inspect_editor_ui":
                return tool
        raise AssertionError(f"inspect_editor_ui is not advertised: {tools!r}")

    def test_fixture_controls_expose_stable_semantics(self) -> None:
        snapshot = self.client.structured_tool("inspect_editor_ui", {"max_depth": 12})
        self.assertGreater(snapshot["generation"], 0)

        fixture = find_one(snapshot["tree"], role="control", name=FIXTURE_NAME)
        self.assertTrue(fixture["visible"])
        self.assertEqual(len(fixture["bounds"]), 4)

        button = find_one([fixture], role="button", name="Brew")
        self.assertEqual(button["class"], "Button")
        self.assertEqual(button["text"], "Brew")
        self.assertTrue(button["enabled"])
        self.assertIn("click", button["actions"])
        self.assertIn("focus", button["actions"])
        self.assertEqual(len(button["bounds"]), 4)
        self.assertTrue(button["handle"])
        self.assertTrue(button["id"].endswith(button["handle"].split(":")[-1]))

        disabled = find_one([fixture], role="button", name="Grind")
        self.assertFalse(disabled["enabled"])
        self.assertEqual(disabled["actions"], [])

        order = find_one([fixture], role="text_field", name="Order")
        self.assertEqual(order["class"], "LineEdit")
        self.assertIn("set_text", order["actions"])

        notes = find_one([fixture], role="text_area", name="Notes")
        self.assertEqual(notes["class"], "TextEdit")

        decaf = find_one([fixture], role="checkbox", name="Decaf")
        self.assertIs(decaf["state"]["pressed"], False)

        shots = find_one([fixture], role="spin_box", name="Shots")
        self.assertEqual(shots["state"]["value"], 2.0)
        self.assertEqual(shots["state"]["min_value"], 1.0)
        self.assertEqual(shots["state"]["max_value"], 4.0)

        beans = find_one([fixture], role="list", name="Beans")
        self.assertEqual(beans["state"]["item_count"], 2)

        find_one([fixture], role="tree", name="Roasts")

        stations = find_one([fixture], role="tab_container", name="Stations")
        self.assertEqual(stations["state"]["tab_count"], 2)

    def test_generation_advances_per_capture(self) -> None:
        first = self.client.structured_tool("inspect_editor_ui", {})
        second = self.client.structured_tool("inspect_editor_ui", {})
        self.assertGreater(second["generation"], first["generation"])
        self.assertNotEqual(first["tree"][0]["id"], second["tree"][0]["id"])
        # Handles are durable public identity, so they survive a new generation.
        self.assertEqual(first["tree"][0]["handle"], second["tree"][0]["handle"])

    def test_every_element_matches_the_advertised_vocabulary(self) -> None:
        tool = self._tool_schema()
        element_schema = tool["outputSchema"]["properties"]["tree"]["items"]
        element_properties = set(element_schema["properties"])
        roles = set(element_schema["properties"]["role"]["enum"])
        actions = set(element_schema["properties"]["actions"]["items"]["enum"])
        self.assertEqual(element_properties, set(element_schema["required"]))

        snapshot = self.client.structured_tool("inspect_editor_ui", {"max_depth": 32})
        self.assertGreater(snapshot["element_count"], 1)
        seen_roles = set()
        for element in walk(snapshot["tree"]):
            # The advertised element shape is recursive; children obey it at every level.
            self.assertEqual(set(element), element_properties, element)
            self.assertIn(element["role"], roles, element)
            seen_roles.add(element["role"])
            for action in element["actions"]:
                self.assertIn(action, actions, element)
            self.assertEqual(len(element["bounds"]), 4)
            self.assertIsInstance(element["state"], dict)
        # Vocabulary closure: every advertised role is a value this build can actually produce
        # or a documented value of the same closed enum.
        self.assertTrue(seen_roles.issubset(roles))
        self.assertIn("control", seen_roles)

        for role in sorted(roles):
            with self.subTest(role=role):
                self.assertIsInstance(role, str)
                self.assertTrue(role)

    def test_internal_children_are_hidden_by_default(self) -> None:
        default = self.client.structured_tool("inspect_editor_ui", {"max_depth": 32})
        self.assertFalse(default["limits"]["include_internal"])
        self.assertEqual([element for element in walk(default["tree"]) if element["internal"]], [])

        internal = self.client.structured_tool(
            "inspect_editor_ui", {"max_depth": 32, "include_internal": True}
        )
        self.assertTrue(internal["limits"]["include_internal"])
        self.assertGreater(internal["element_count"], default["element_count"])
        self.assertTrue([element for element in walk(internal["tree"]) if element["internal"]])

    def test_limits_are_clamped_and_truncation_is_published(self) -> None:
        shallow = self.client.structured_tool("inspect_editor_ui", {"max_depth": 0})
        self.assertEqual(shallow["limits"]["max_depth"], 1)
        self.assertTrue(shallow["limits"]["depth_truncated"])
        self.assertTrue(shallow["truncated"])
        self.assertEqual(len(shallow["tree"]), 1)
        self.assertEqual(shallow["tree"][0]["children"], [])
        self.assertTrue(shallow["tree"][0]["truncated"])

        deep = self.client.structured_tool(
            "inspect_editor_ui", {"max_depth": 4096, "max_elements": 1_000_000}
        )
        self.assertEqual(deep["limits"]["max_depth"], 32)
        self.assertEqual(deep["limits"]["max_elements"], 2000)

        bounded = self.client.structured_tool(
            "inspect_editor_ui", {"max_depth": 32, "max_elements": 3}
        )
        self.assertLessEqual(bounded["element_count"], 3)
        self.assertTrue(bounded["limits"]["element_limit_reached"])
        self.assertTrue(bounded["truncated"])

    def test_snapshot_stays_within_the_transport_budget(self) -> None:
        status, body = self.client.request(
            {
                "jsonrpc": "2.0",
                "id": 9001,
                "method": "tools/call",
                "params": {
                    "name": "inspect_editor_ui",
                    "arguments": {"max_depth": 32, "max_elements": 1_000_000, "include_internal": True},
                },
            }
        )
        self.assertEqual(status, 200)
        response = json.loads(body)
        structured = response["result"]["structuredContent"]
        self.assertIs(response["result"]["isError"], False)
        payload_bytes = len(json.dumps(structured, separators=(",", ":")).encode("utf-8"))
        self.assertLessEqual(payload_bytes, MAX_SNAPSHOT_PAYLOAD_BYTES)
        self.assertEqual(structured["limits"]["payload_limit_bytes"], MAX_SNAPSHOT_PAYLOAD_BYTES)
        self.assertLessEqual(structured["element_count"], structured["limits"]["max_elements"])

    def test_malformed_arguments_are_rejected(self) -> None:
        for arguments in (
            {"max_depth": "8"},
            {"max_depth": True},
            {"max_elements": 1.5},
            {"include_internal": "yes"},
            {"unknown": 1},
            {"max_depth": None},
        ):
            with self.subTest(arguments=arguments):
                result = self.client.rpc(
                    "tools/call", {"name": "inspect_editor_ui", "arguments": arguments}
                )["result"]
                self.assertIs(result["isError"], True)
                self.assertEqual(result["structuredContent"]["error"], "invalid_arguments")


class BaristaMCPUISnapshotPortabilityTests(unittest.TestCase):
    def test_pinned_extension_api_covers_snapshot_dependencies(self) -> None:
        """Portability evidence: every engine symbol the snapshot calls exists in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))
        for class_name in profile["enabled_classes"]:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, classes)

        required_classes = (
            "BaseButton",
            "Button",
            "CheckBox",
            "Control",
            "ItemList",
            "Label",
            "LineEdit",
            "Node",
            "OptionButton",
            "PopupMenu",
            "Range",
            "RichTextLabel",
            "SpinBox",
            "TabBar",
            "TabContainer",
            "TextEdit",
            "Tree",
            "Window",
        )
        for class_name in required_classes:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, profile["enabled_classes"])

        required_methods = {
            "BaseButton": ("is_disabled", "is_pressed", "is_toggle_mode"),
            "Button": ("get_text",),
            "CanvasItem": ("is_visible_in_tree",),
            "Control": (
                "get_accessibility_name",
                "get_focus_mode",
                "get_global_rect",
                "get_tooltip_text",
                "has_focus",
            ),
            "EditorInterface": ("get_base_control",),
            "ItemList": ("get_item_count", "get_selected_items"),
            "Label": ("get_text",),
            "LineEdit": ("get_text", "is_editable"),
            "Node": ("get_child_count", "get_children", "get_name"),
            "Object": ("get_class", "get_instance_id", "is_class"),
            "OptionButton": ("get_selected",),
            "PopupMenu": ("get_item_count",),
            "Range": ("get_max", "get_min", "get_step", "get_value"),
            "RichTextLabel": ("get_text",),
            "TabBar": ("get_current_tab", "get_tab_count"),
            "TabContainer": ("get_current_tab", "get_tab_count"),
            "TextEdit": ("get_text", "is_editable"),
            "Window": ("get_position", "get_size", "get_title", "is_visible"),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
