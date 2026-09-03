# test_mcp_ui_selector.py
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
FIXTURE_NAME = "Barista Test Fixture"

SELECTOR_STATUSES = (
    "ok",
    "no_match",
    "ambiguous_selector",
    "invalid_selector",
    "stale_handle",
    "invalid_cursor",
)
SELECTOR_FIELDS = (
    "handle",
    "role",
    "name",
    "text",
    "text_contains",
    "class",
    "visible",
    "enabled",
    "focused",
    "pressed",
    "selected",
    "within",
)


class BaristaMCPSelectorTests(unittest.TestCase):
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

    def find(self, **arguments: Any) -> dict[str, Any]:
        return self.client.structured_tool("find_editor_ui", arguments)

    def _tool(self) -> dict[str, Any]:
        for tool in self.client.rpc("tools/list", {})["result"]["tools"]:
            if tool["name"] == "find_editor_ui":
                return tool
        raise AssertionError("find_editor_ui is not advertised")

    def test_advertised_selector_vocabulary_is_closed(self) -> None:
        tool = self._tool()
        status = tool["outputSchema"]["properties"]["status"]
        self.assertEqual(tuple(status["enum"]), SELECTOR_STATUSES)
        description = tool["inputSchema"]["properties"]["selector"]["description"]
        for field in SELECTOR_FIELDS:
            with self.subTest(field=field):
                self.assertIn(field, description)
        limit = tool["inputSchema"]["properties"]["limit"]
        self.assertEqual((limit["minimum"], limit["maximum"]), (1, 200))

    def test_exact_selector_matches_one_element(self) -> None:
        result = self.find(selector={"role": "button", "name": "Brew"}, limit=20)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["match_count"], 1)
        self.assertEqual(result["returned_count"], 1)
        self.assertEqual(result["offset"], 0)
        self.assertEqual(result["next_cursor"], "")
        self.assertGreater(result["generation"], 0)
        match = result["matches"][0]
        self.assertEqual(match["class"], "Button")
        self.assertEqual(match["text"], "Brew")
        self.assertTrue(match["handle"].startswith("el:"))
        # A page carries no subtrees, so one page can never expand into the whole tree.
        self.assertEqual(match["children"], [])

    def test_selector_is_side_effect_free_and_repeatable(self) -> None:
        first = self.find(selector={"role": "button", "name": "Brew"})
        second = self.find(selector={"role": "button", "name": "Brew"})
        self.assertEqual(first["match_count"], second["match_count"])
        self.assertEqual(first["matches"][0]["handle"], second["matches"][0]["handle"])
        # Identity is public engine identity, not the capture it was observed in.
        self.assertNotEqual(first["generation"], second["generation"])

    def test_nested_and_state_selectors(self) -> None:
        nested = self.find(
            selector={
                "role": "button",
                "name": "Brew",
                "within": {"role": "control", "name": FIXTURE_NAME},
            }
        )
        self.assertEqual(nested["match_count"], 1)

        outside = self.find(
            selector={
                "role": "button",
                "name": "Brew",
                "within": {"role": "button", "name": "Grind"},
            }
        )
        self.assertEqual(outside["status"], "no_match")

        disabled = self.find(selector={"role": "button", "enabled": False, "name": "Grind"})
        self.assertEqual(disabled["match_count"], 1)

        unpressed = self.find(selector={"role": "checkbox", "pressed": False})
        self.assertGreaterEqual(unpressed["match_count"], 1)
        # A state constraint never matches an element that does not publish that state.
        self.assertEqual(
            self.find(selector={"class": "LinkButton", "pressed": True})["status"], "no_match"
        )
        self.assertEqual(
            self.find(selector={"class": "Label", "selected": False})["status"], "no_match"
        )

        by_class = self.find(selector={"class": "LinkButton"})
        self.assertEqual(by_class["match_count"], 1)
        self.assertEqual(by_class["matches"][0]["text"], "Recipes")

        contains = self.find(selector={"role": "button", "text_contains": "Bre"})
        self.assertEqual(contains["match_count"], 1)

    def test_no_match_and_ambiguity_fail_closed(self) -> None:
        missing = self.find(selector={"name": "No Such Element Anywhere"})
        self.assertFalse(missing["ok"])
        self.assertEqual(missing["status"], "no_match")
        self.assertEqual(missing["match_count"], 0)
        self.assertEqual(missing["matches"], [])

        ambiguous = self.find(selector={"role": "button"})
        self.assertGreater(ambiguous["match_count"], 1)
        self.assertTrue(ambiguous["ok"])

        rejected = self.find(selector={"role": "button"}, require_unique=True)
        self.assertFalse(rejected["ok"])
        self.assertEqual(rejected["status"], "ambiguous_selector")
        # Never pick the first: an ambiguous query returns no element at all.
        self.assertEqual(rejected["matches"], [])
        self.assertGreater(rejected["match_count"], 1)

        # Uniqueness can only be certified against a capture that omitted nothing.
        unique = self.find(
            selector={"role": "button", "name": "Brew"}, require_unique=True, max_depth=32
        )
        self.assertTrue(unique["ok"], unique)
        self.assertIs(unique["truncated"], False)

        truncated = self.find(selector={"role": "button", "name": "Brew"}, require_unique=True)
        self.assertFalse(truncated["ok"], truncated)
        self.assertEqual(truncated["status"], "ambiguous_selector")
        self.assertIs(truncated["truncated"], True)

    def test_malformed_selectors_are_rejected(self) -> None:
        for selector in (
            {},
            {"unknown_field": "x"},
            {"role": 1},
            {"role": None},
            {"visible": "yes"},
            {"pressed": 1},
            {"name": "x" * 513},
            {"within": {"within": {"within": {"within": {"within": {"role": "button"}}}}}},
            {"within": {}},
            {"within": "button"},
        ):
            with self.subTest(selector=selector):
                result = self.find(selector=selector)
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["status"], "invalid_selector")
                self.assertEqual(result["matches"], [])
                self.assertTrue(result["message"])

        absent = self.find()
        self.assertEqual(absent["status"], "invalid_selector")
        self.assertEqual(absent["generation"], 0)

        # A non-object selector never reaches the selector engine: the boundary schema rejects it.
        rejected = self.client.rpc(
            "tools/call", {"name": "find_editor_ui", "arguments": {"selector": "button"}}
        )["result"]
        self.assertIs(rejected["isError"], True)
        self.assertEqual(rejected["structuredContent"]["error"], "invalid_arguments")

    def test_id_is_not_a_selector_field(self) -> None:
        # Element ids are published for correlation only (src/mcp_contracts.cpp ui_element_schema);
        # they are descoped as a selector input by cafecito-games/BaristaMCP#17, so "id" is an unknown
        # field and fails closed like any other unknown field rather than widening the match.
        tool = self._tool()
        self.assertNotIn(
            "id, ", tool["inputSchema"]["properties"]["selector"]["description"]
        )

        issued = self.find(selector={"role": "button", "name": "Brew"})
        element_id = issued["matches"][0]["id"]
        self.assertTrue(element_id.startswith(f"s{issued['generation']}:"), element_id)

        for selector in (
            {"id": element_id},
            {"id": "s1:1"},
            {"id": ""},
            {"id": 1},
            {"role": "button", "id": element_id},
            {"role": "button", "within": {"id": element_id}},
        ):
            with self.subTest(selector=selector):
                rejected = self.find(selector=selector)
                self.assertFalse(rejected["ok"], rejected)
                self.assertEqual(rejected["status"], "invalid_selector", rejected)
                self.assertEqual(rejected["matches"], [])
                self.assertTrue(rejected["message"])

    def test_unissued_handles_fail_closed(self) -> None:
        first = self.find(selector={"role": "button", "name": "Brew"})
        handle = first["matches"][0]["handle"]

        # A new capture retires the previous snapshot ids without retiring durable handles.
        self.find(selector={"role": "button", "name": "Grind"})

        durable = self.find(selector={"handle": handle})
        self.assertEqual(durable["status"], "ok")
        self.assertEqual(durable["matches"][0]["handle"], handle)

        for unissued in ("el:999999999999", "el:0", "not-a-handle", "el:", "el:-3", ""):
            with self.subTest(handle=unissued):
                result = self.find(selector={"handle": unissued})
                self.assertEqual(result["status"], "stale_handle", result)
                self.assertEqual(result["matches"], [])

    def test_handles_nested_in_within_are_validated(self) -> None:
        issued = self.find(selector={"role": "button", "name": "Brew"})["matches"][0]["handle"]

        # A handle nested inside "within" is checked against the issued-handle registry too.
        nested = self.find(
            selector={"role": "button", "within": {"handle": "el:999999999999"}}
        )
        self.assertEqual(nested["status"], "stale_handle", nested)
        self.assertEqual(nested["matches"], [])

        resolvable = self.find(selector={"role": "button", "within": {"handle": issued}})
        self.assertIn(resolvable["status"], ("ok", "no_match"))

    def test_a_resolvable_handle_with_a_failing_constraint_is_not_stale(self) -> None:
        handle = self.find(selector={"role": "button", "name": "Brew"})["matches"][0]["handle"]
        result = self.find(selector={"handle": handle, "name": "Not The Brew Button"})
        # The handle resolved; another constraint failed, and saying "stale" would be a wrong verdict.
        self.assertEqual(result["status"], "no_match", result)

    def test_pagination_is_bounded_and_cursors_fail_closed(self) -> None:
        selector = {"role": "button"}
        first = self.find(selector=selector, limit=1)
        self.assertTrue(first["ok"])
        self.assertEqual(first["returned_count"], 1)
        self.assertGreater(first["match_count"], 1)
        self.assertTrue(first["truncated"])
        cursor = first["next_cursor"]
        self.assertTrue(cursor)

        second = self.find(selector=selector, cursor=cursor)
        self.assertTrue(second["ok"], second)
        self.assertEqual(second["offset"], 1)
        self.assertEqual(second["generation"], first["generation"])
        self.assertEqual(second["match_count"], first["match_count"])
        self.assertNotEqual(
            second["matches"][0]["handle"], first["matches"][0]["handle"]
        )

        for arguments, reason in (
            ({"selector": selector, "cursor": "not-a-cursor"}, "malformed"),
            ({"selector": selector, "cursor": ""}, "empty"),
            ({"selector": selector, "cursor": cursor + "AAAA"}, "trailing"),
            ({"selector": {"role": "checkbox"}, "cursor": cursor}, "other selector"),
            ({"selector": selector, "cursor": cursor, "include_internal": True}, "options"),
            ({"selector": selector, "cursor": cursor, "limit": 5}, "page size"),
        ):
            with self.subTest(reason=reason):
                result = self.find(**arguments)
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["status"], "invalid_cursor")
                # A rejected cursor never silently restarts at offset zero.
                self.assertEqual(result["matches"], [])
                self.assertEqual(result["match_count"], 0)

        # A cursor is bound to the capture that issued it; a newer capture retires it.
        self.find(selector=selector)
        retired = self.find(selector=selector, cursor=cursor)
        self.assertEqual(retired["status"], "invalid_cursor")

    def test_cursor_identity_survives_selector_field_injection(self) -> None:
        """A field value can never impersonate the separators of the canonical selector form."""
        paged = {"role": "button", "visible": True}
        # Serialized naively, this single value produces the same text as the two fields above.
        colliding = {"role": "button;visible=1"}
        first = self.find(selector=paged, limit=1)
        self.assertTrue(first["ok"], first)
        self.assertGreater(first["match_count"], 1)
        cursor = first["next_cursor"]
        self.assertTrue(cursor)

        reused = self.find(selector=colliding, cursor=cursor)
        self.assertFalse(reused["ok"], reused)
        self.assertEqual(reused["status"], "invalid_cursor")
        self.assertEqual(reused["matches"], [])

    def test_every_status_is_reachable_and_handled(self) -> None:
        """Vocabulary closure: every advertised status is produced by a real request."""
        observed = {
            "ok": self.find(selector={"role": "button", "name": "Brew"}),
            "no_match": self.find(selector={"name": "No Such Element Anywhere"}),
            "ambiguous_selector": self.find(selector={"role": "button"}, require_unique=True),
            "invalid_selector": self.find(selector={}),
            "stale_handle": self.find(selector={"handle": "el:999999999999"}),
            "invalid_cursor": self.find(selector={"role": "button"}, cursor="forged"),
        }
        self.assertEqual(sorted(observed), sorted(SELECTOR_STATUSES))
        for status, result in observed.items():
            with self.subTest(status=status):
                self.assertEqual(result["status"], status, result)
                self.assertIs(result["ok"], status == "ok")
                self.assertIsInstance(result["message"], str)
                self.assertIsInstance(json.dumps(result), str)


class BaristaMCPSelectorPortabilityTests(unittest.TestCase):
    def test_pinned_extension_api_covers_selector_dependencies(self) -> None:
        """Portability evidence: every engine symbol the selector calls exists in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))
        self.assertIn("Marshalls", profile["enabled_classes"])
        available = {method["name"] for method in classes["Marshalls"].get("methods", [])}
        for method in ("utf8_to_base64", "base64_to_utf8"):
            with self.subTest(method=method):
                self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
