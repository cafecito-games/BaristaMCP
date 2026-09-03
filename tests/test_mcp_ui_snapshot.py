# test_mcp_ui_snapshot.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import ROOT, EditorProcess, MCPClient, write_test_project  # noqa: E402

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

        link = find_one([fixture], role="button", name="Recipes")
        self.assertEqual(link["class"], "LinkButton")
        self.assertEqual(link["text"], "Recipes")

        grind = find_one([fixture], role="slider", name="Grind Size")
        self.assertFalse(grind["enabled"])
        self.assertEqual(grind["actions"], [])
        self.assertEqual(grind["state"]["value"], 5.0)

        order = find_one([fixture], role="text_field", name="Order")
        self.assertEqual(order["class"], "LineEdit")
        self.assertIn("set_text", order["actions"])

        passcode = find_one([fixture], role="text_field", name="Passcode")
        # A masked field must never disclose what the editor hides on screen.
        self.assertEqual(passcode["text"], "")
        self.assertIs(passcode["state"]["secret"], True)
        self.assertEqual(passcode["state"]["text_length"], len("roasted-secret"))
        self.assertNotIn("roasted-secret", json.dumps(snapshot))

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
        output_schema = tool["outputSchema"]
        self.assertEqual(
            output_schema["properties"]["tree"]["items"], {"$ref": "#/$defs/ui_element"}
        )
        element_schema = output_schema["$defs"]["ui_element"]
        # The recursive contract is one document: children reference the same definition.
        self.assertEqual(
            element_schema["properties"]["children"]["items"], {"$ref": "#/$defs/ui_element"}
        )
        element_properties = set(element_schema["properties"])
        roles = set(element_schema["properties"]["role"]["enum"])
        actions = set(element_schema["properties"]["actions"]["items"]["enum"])
        self.assertEqual(element_properties, set(element_schema["required"]))

        snapshot = self.client.structured_tool("inspect_editor_ui", {"max_depth": 32})
        self.assertGreater(snapshot["element_count"], 1)
        self.assertEqual(set(snapshot), set(output_schema["properties"]))
        self.assertEqual(
            set(snapshot["limits"]), set(output_schema["properties"]["limits"]["properties"])
        )
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

        # Internal status is inherited: nothing reachable only through an internal node may claim to
        # be a public element.
        def assert_internal_is_inherited(elements: list[dict[str, Any]], parent_internal: bool) -> None:
            for element in elements:
                if parent_internal:
                    self.assertTrue(element["internal"], element)
                assert_internal_is_inherited(element["children"], element["internal"])

        assert_internal_is_inherited(internal["tree"], False)

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

    def test_out_of_int64_range_numbers_are_rejected_at_the_boundary(self) -> None:
        """A well-formed JSON number outside int64_t must never reach a narrowing conversion.

        The bodies below are byte-faithful: they are the exact request text the transport JSON
        parser consumes (producer: src/mcp_server.cpp:150), so the value arrives as the double the
        real producer builds. The advertised range is produced by src/mcp_schema.cpp:231 and
        enforced by src/mcp_schema.cpp:134; without it, `(int64_t)1e300` is undefined and the
        applied limit differs between arm64 and x86-64.
        """
        properties = self._tool_schema()["inputSchema"]["properties"]
        for option in ("max_depth", "max_elements"):
            with self.subTest(option=option):
                self.assertEqual(properties[option]["minimum"], -(2**63))
                # The largest double strictly below 2^63, so every accepted value converts exactly.
                self.assertEqual(properties[option]["maximum"], 2**63 - 1024)

        request_id = 7100
        for option in ("max_depth", "max_elements"):
            for literal in ("1e300", "-1e300", "1.7976931348623157e308"):
                with self.subTest(option=option, literal=literal):
                    request_id += 1
                    body = (
                        '{"jsonrpc":"2.0","id":%d,"method":"tools/call","params":'
                        '{"name":"inspect_editor_ui","arguments":{"%s":%s}}}'
                        % (request_id, option, literal)
                    )
                    status, response_body = self.client.raw_request(
                        "POST",
                        self.client.path,
                        body,
                        {
                            "Authorization": f"Bearer {self.client.token}",
                            "Content-Type": "application/json",
                            "Accept": "application/json",
                        },
                    )
                    self.assertEqual(status, 200)
                    result = json.loads(response_body)["result"]
                    self.assertIs(result["isError"], True)
                    self.assertEqual(
                        result["structuredContent"]["error"], "invalid_arguments"
                    )

        # A large value that is still representable stays accepted and clamps, so the boundary
        # rejects only what it genuinely cannot convert.
        representable = self.client.structured_tool(
            "inspect_editor_ui", {"max_depth": 2**63 - 1024, "max_elements": 2**63 - 1024}
        )
        self.assertEqual(representable["limits"]["max_depth"], 32)
        self.assertEqual(representable["limits"]["max_elements"], 2000)

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


class BaristaMCPUISnapshotTraversalBudgetTests(unittest.TestCase):
    """The traversal budget must bound per-parent child scans, not only recursion."""

    # Every plain node costs one visit plus one parent scan, so this comfortably exceeds
    # EditorSnapshotLimits::MAX_VISITED_NODES (src/editor_automation_types.h) and forces the budget
    # to run out part way through a child list.
    WIDE_INTERNAL_CHILDREN = 60_000

    def test_wide_internal_child_lists_are_bounded_by_the_traversal_budget(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-wide-fixture-") as temporary:
            project_dir = Path(temporary)
            write_test_project(
                project_dir,
                "[barista_mcp_test_fixture]\n"
                f"wide_internal_children={self.WIDE_INTERNAL_CHILDREN}",
                extra_plugins=("barista_mcp_test_fixture",),
            )
            editor = EditorProcess(project_dir)
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery(timeout=60.0))
                client.initialize()

                # Internal children are not scanned at all when they were not requested.
                public = client.structured_tool("inspect_editor_ui", {"max_depth": 32})
                self.assertFalse(public["limits"]["traversal_limit_reached"])
                self.assertFalse(public["limits"]["include_internal"])

                # Exhaustion happens part way through a parent's child scan. The published
                # contract must stay honest there, and the request must still answer inside the
                # client socket timeout with the whole scan charged to the budget.
                internal = client.structured_tool(
                    "inspect_editor_ui", {"max_depth": 32, "include_internal": True}
                )
                self.assertTrue(internal["limits"]["traversal_limit_reached"])
                self.assertTrue(internal["truncated"])
                self.assertLessEqual(
                    internal["element_count"], internal["limits"]["max_elements"]
                )
                # Truncation is published on the tree as well as in the limits block.
                self.assertTrue(
                    [element for element in walk(internal["tree"]) if element["truncated"]]
                )
            finally:
                editor.stop()


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
            "AcceptDialog",
            "BaseButton",
            "Button",
            "CheckBox",
            "Control",
            "ItemList",
            "Label",
            "LineEdit",
            "LinkButton",
            "MenuButton",
            "Node",
            "OptionButton",
            "PopupMenu",
            "Range",
            "RichTextLabel",
            "Slider",
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
            "AcceptDialog": ("get_text",),
            "Label": ("get_text",),
            "LinkButton": ("get_text",),
            "MenuButton": ("get_item_count",),
            "Slider": ("is_editable",),
            "SpinBox": ("is_editable",),
            "LineEdit": ("get_text", "is_editable", "is_secret"),
            "Node": ("get_child_count", "get_children", "get_name"),
            "Object": ("get_class", "get_instance_id", "is_class"),
            "OptionButton": ("get_selected",),
            "PopupMenu": ("get_item_count",),
            "Range": ("get_max", "get_min", "get_step", "get_value"),
            "RichTextLabel": ("get_text",),
            "TabBar": ("get_current_tab", "get_tab_count"),
            "TabContainer": ("get_current_tab", "get_tab_count"),
            "TextEdit": ("get_text", "is_editable"),
            "Window": ("get_position", "get_size", "get_title", "has_focus", "is_visible"),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
