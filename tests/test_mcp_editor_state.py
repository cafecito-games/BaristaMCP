# test_mcp_editor_state.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import ROOT, EditorProcess, MCPClient  # noqa: E402

PINNED_EXTENSION_API = ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json"
BUILD_PROFILE = ROOT / "build_profile.json"

STATE_SECTIONS = ("project", "scenes", "selection", "script", "filesystem", "play")
STRING_LIST_FIELDS = ("count", "items", "truncated")


class BaristaMCPEditorStateTests(unittest.TestCase):
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

    def _resource(self, uri: str) -> dict[str, Any]:
        read = self.client.rpc("resources/read", {"uri": uri})
        self.assertIn("result", read, read)
        return json.loads(read["result"]["contents"][0]["text"])

    def _assert_string_list(self, value: dict[str, Any]) -> None:
        self.assertEqual(sorted(value), sorted(STRING_LIST_FIELDS))
        self.assertIsInstance(value["count"], int)
        self.assertIsInstance(value["truncated"], bool)
        for item in value["items"]:
            self.assertIsInstance(item, str)
        # Bounding is published, never silent.
        self.assertEqual(value["truncated"], len(value["items"]) < value["count"])

    def test_editor_state_sections_are_stable(self) -> None:
        state = self.client.structured_tool("read_editor_state", {})
        self.assertEqual(sorted(state), sorted(STATE_SECTIONS))

        project = state["project"]
        self.assertEqual(project["project_name"], "BaristaMCP Test Project")
        self.assertEqual(
            (project["godot_version"]["major"], project["godot_version"]["minor"]), (4, 7)
        )

        self._assert_string_list(state["scenes"]["open"])
        self._assert_string_list(state["scenes"]["unsaved"])
        self.assertIsInstance(state["scenes"]["current"], str)

        selection = state["selection"]
        self.assertIsInstance(selection["count"], int)
        self.assertIsInstance(selection["truncated"], bool)
        for node in selection["nodes"]:
            self.assertEqual(sorted(node), ["class", "name", "path"])

        script = state["script"]
        self.assertIsInstance(script["current"], str)
        self._assert_string_list(script["open"])
        self._assert_string_list(script["unsaved"])

        filesystem = state["filesystem"]
        self.assertIsInstance(filesystem["scanning"], bool)
        self.assertIsInstance(filesystem["importing"], bool)
        self.assertIsInstance(filesystem["scan_progress"], (int, float))
        # Missing optional state serializes as an empty value rather than disappearing.
        self.assertIsInstance(filesystem["current_path"], str)

        play = state["play"]
        self.assertIs(play["is_playing"], False)
        self.assertIsInstance(play["playing_scene"], str)

    def test_tool_and_resource_state_cannot_drift(self) -> None:
        """Single source of truth: the resource and the tool call the same reader."""
        self.assertEqual(
            self._resource("barista://editor/state"),
            self.client.structured_tool("read_editor_state", {}),
        )
        # The project section is the very same payload the project tool and resource publish.
        state = self.client.structured_tool("read_editor_state", {})
        self.assertEqual(state["project"], self.client.structured_tool("get_project_info", {}))
        self.assertEqual(state["project"], self._resource("barista://project/info"))

    def test_scene_resources_are_bounded_and_honest(self) -> None:
        active = self._resource("barista://scene/active")
        self.assertEqual(
            sorted(active),
            ["child_count", "has_scene", "play", "root_class", "root_name", "scene_path", "scenes"],
        )
        # The shared test project opens no scene, so the readers must say so rather than guess.
        self.assertIs(active["has_scene"], False)
        self.assertEqual(active["scene_path"], "")
        self.assertEqual(active["child_count"], 0)

        tree = self._resource("barista://scene/tree")
        self.assertEqual(
            sorted(tree),
            ["has_scene", "limits", "node_count", "scene_path", "tree", "truncated"],
        )
        self.assertIs(tree["has_scene"], False)
        self.assertEqual(tree["tree"], [])
        self.assertEqual(tree["node_count"], 0)
        self.assertIs(tree["truncated"], False)
        self.assertEqual(
            sorted(tree["limits"]),
            [
                "depth_truncated",
                "max_depth",
                "max_nodes",
                "node_limit_reached",
                "traversal_limit_reached",
            ],
        )
        self.assertGreater(tree["limits"]["max_nodes"], 0)

    def test_state_reads_are_side_effect_free(self) -> None:
        first = self.client.structured_tool("read_editor_state", {})
        self.assertEqual(first, self.client.structured_tool("read_editor_state", {}))
        self.assertEqual(
            self._resource("barista://scene/tree"), self._resource("barista://scene/tree")
        )

    def test_state_tool_rejects_unknown_arguments(self) -> None:
        for arguments in ({"extra": 1}, {"project": {}}, {"scenes": None}):
            with self.subTest(arguments=arguments):
                result = self.client.rpc(
                    "tools/call", {"name": "read_editor_state", "arguments": arguments}
                )["result"]
                self.assertIs(result["isError"], True)
                self.assertEqual(result["structuredContent"]["error"], "invalid_arguments")


class BaristaMCPEditorStatePortabilityTests(unittest.TestCase):
    def test_pinned_extension_api_covers_state_dependencies(self) -> None:
        """Portability evidence: every engine symbol the state reader calls exists in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))

        required_classes = ("EditorFileSystem", "EditorSelection", "Resource", "Script", "ScriptEditor")
        for class_name in required_classes:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, profile["enabled_classes"])
                self.assertIn(class_name, classes)

        required_methods = {
            "EditorInterface": (
                "get_current_path",
                "get_open_scenes",
                "get_playing_scene",
                "get_resource_filesystem",
                "get_script_editor",
                "get_selection",
                "get_unsaved_scenes",
            ),
            "EditorSelection": ("get_selected_nodes",),
            "EditorFileSystem": ("get_scanning_progress", "is_importing", "is_scanning"),
            "ScriptEditor": ("get_current_script", "get_open_scripts", "get_unsaved_files"),
            "Resource": ("get_path",),
            "Node": ("get_child", "get_child_count", "get_name", "get_path", "get_scene_file_path"),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
