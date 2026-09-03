# test_mcp_resources.py
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

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import (  # noqa: E402
    ROOT,
    EditorProcess,
    MCPClient,
    write_test_project,
)

PINNED_EXTENSION_API = ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json"
BUILD_PROFILE = ROOT / "build_profile.json"
RESOURCE_TOO_LARGE = -32003
INVALID_PARAMS = -32602
SERVER_NOT_INITIALIZED = -32002


class BaristaMCPResourceTests(unittest.TestCase):
    def test_resources_are_advertised_and_read(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            result = client.initialize()
            self.assertEqual(
                result["capabilities"]["resources"],
                {"subscribe": False, "listChanged": False},
            )

            listed = client.rpc("resources/list", {})["result"]
            resources = listed["resources"]
            self.assertNotIn("nextCursor", listed)
            self.assertIn("barista://project/info", [entry["uri"] for entry in resources])

            templates = client.rpc("resources/templates/list", {})["result"]
            self.assertEqual(templates, {"resourceTemplates": []})

            # Vocabulary closure: every advertised resource must be readable and self-describing.
            for entry in resources:
                with self.subTest(uri=entry["uri"]):
                    for field in ("uri", "name", "title", "description", "mimeType"):
                        self.assertIsInstance(entry[field], str, entry)
                    self.assertEqual(entry["mimeType"], "application/json")
                    read = client.rpc("resources/read", {"uri": entry["uri"]})
                    self.assertIn("result", read, read)
                    contents = read["result"]["contents"]
                    self.assertEqual(len(contents), 1)
                    self.assertEqual(contents[0]["uri"], entry["uri"])
                    self.assertEqual(contents[0]["mimeType"], entry["mimeType"])
                    self.assertIsInstance(json.loads(contents[0]["text"]), dict)

            read = client.rpc("resources/read", {"uri": "barista://project/info"})
            payload = json.loads(read["result"]["contents"][0]["text"])
            self.assertEqual(payload["project_name"], "BaristaMCP Test Project")
            # The resource must reuse the same provider method as the tool.
            self.assertEqual(payload, client.structured_tool("get_project_info", {}))

            version = payload["godot_version"]
            self.assertEqual((version["major"], version["minor"]), (4, 7))
        finally:
            editor.stop()

    def test_resource_requests_fail_closed(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())

            for method, params in (
                ("resources/list", {}),
                ("resources/templates/list", {}),
                ("resources/read", {"uri": "barista://project/info"}),
            ):
                with self.subTest(method=method):
                    response = client.rpc(method, params)
                    self.assertEqual(response["error"]["code"], SERVER_NOT_INITIALIZED)

            client.initialize()

            for uri in (
                "barista://missing",
                "barista://project/info/",
                "barista://PROJECT/INFO",
                "barista://ui/element/1",
                "file:///etc/passwd",
                "",
            ):
                with self.subTest(uri=uri):
                    response = client.rpc("resources/read", {"uri": uri})
                    self.assertNotIn("result", response, response)
                    self.assertEqual(response["error"]["code"], INVALID_PARAMS)

            for params in (
                {},
                {"uri": 1},
                {"uri": None},
                {"uri": ["barista://project/info"]},
                {"uri": {"value": "barista://project/info"}},
                {"uri": "barista://project/info", "extra": True},
                {"URI": "barista://project/info"},
            ):
                with self.subTest(params=params):
                    response = client.rpc("resources/read", params)
                    self.assertNotIn("result", response, response)
                    self.assertEqual(response["error"]["code"], INVALID_PARAMS)

            for method in ("resources/list", "resources/templates/list", "tools/list"):
                with self.subTest(method=method):
                    self.assertIn("result", client.rpc(method, {}))
                    unknown = client.rpc(method, {"cursor": "forged"})
                    self.assertNotIn("result", unknown, unknown)
                    self.assertEqual(unknown["error"]["code"], INVALID_PARAMS)
                    non_object = client.rpc(method, "not-an-object")
                    self.assertEqual(non_object["error"]["code"], INVALID_PARAMS)
        finally:
            editor.stop()

    def test_tool_schemas_are_advertised_and_enforced(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            client.initialize()

            tools = client.rpc("tools/list", {})["result"]["tools"]
            self.assertEqual(
                [tool["name"] for tool in tools],
                ["barista_status", "get_project_info", "find_editor_ui", "inspect_editor_ui"],
            )
            for tool in tools:
                with self.subTest(tool=tool["name"]):
                    input_schema = tool["inputSchema"]
                    self.assertEqual(input_schema["type"], "object")
                    self.assertIs(input_schema["additionalProperties"], False)
                    output_schema = tool["outputSchema"]
                    self.assertEqual(output_schema["type"], "object")
                    self.assertIs(output_schema["additionalProperties"], False)
                    self.assertTrue(output_schema["required"])

                    structured = client.structured_tool(tool["name"], {})
                    for name in output_schema["required"]:
                        self.assertIn(name, structured)
                    self.assertEqual(
                        sorted(structured), sorted(output_schema["properties"])
                    )

                    rejected = client.rpc(
                        "tools/call", {"name": tool["name"], "arguments": {"extra": 1}}
                    )["result"]
                    self.assertIs(rejected["isError"], True)
                    self.assertEqual(
                        rejected["structuredContent"]["error"], "invalid_arguments"
                    )

            unknown = client.rpc("tools/call", {"name": "resources/read", "arguments": {}})["result"]
            self.assertIs(unknown["isError"], True)
            self.assertEqual(unknown["structuredContent"]["error"], "unknown_tool")
        finally:
            editor.stop()

    def test_oversized_resource_payload_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-large-resource-") as temporary:
            project_dir = Path(temporary)
            write_test_project(project_dir, config_name="B" * 600_000)
            editor = EditorProcess(project_dir)
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery())
                client.initialize()

                response = client.rpc("resources/read", {"uri": "barista://project/info"})
                self.assertNotIn("result", response, response)
                error = response["error"]
                self.assertEqual(error["code"], RESOURCE_TOO_LARGE)
                self.assertEqual(error["data"]["error"], "resource_too_large")
                self.assertEqual(error["data"]["uri"], "barista://project/info")
                self.assertGreater(error["data"]["size_bytes"], error["data"]["limit_bytes"])
            finally:
                editor.stop()

    def test_pinned_extension_api_covers_every_declared_dependency(self) -> None:
        """Portability evidence: every engine symbol BaristaMCP calls exists in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        header = api["header"]
        self.assertEqual((header["version_major"], header["version_minor"]), (4, 7))

        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))
        for class_name in profile["enabled_classes"]:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, classes)

        required_methods = {
            "ProjectSettings": ("get_setting", "globalize_path"),
            "Engine": ("get_version_info",),
            "EditorInterface": ("get_edited_scene_root", "is_playing_scene"),
            "JSON": ("stringify",),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
