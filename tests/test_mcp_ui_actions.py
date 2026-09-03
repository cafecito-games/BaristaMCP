# test_mcp_ui_actions.py
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
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import (  # noqa: E402
    ROOT,
    EditorProcess,
    MCPClient,
    write_test_project,
)

PINNED_EXTENSION_API = ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json"
BUILD_PROFILE = ROOT / "build_profile.json"
FIXTURE_NAME = "Barista Test Fixture"
COUNTERS_NAME = "Action Counters"
ACT_TOOL = "act_on_editor_ui"
AUTOMATION_ARGUMENT = "--barista-mcp-automation"

READ_ONLY_TOOLS = (
    "barista_status",
    "get_project_info",
    "find_editor_ui",
    "read_editor_state",
    "inspect_editor_ui",
)

# One definition of the action status vocabulary, mirrored from EditorActionDriver::status_vocabulary
# in src/editor_action_driver.cpp. Every consumer below must handle or reject every value.
ACTION_STATUSES = (
    "ok",
    "automation_disabled",
    "invalid_arguments",
    "invalid_selector",
    "no_match",
    "ambiguous_selector",
    "stale_handle",
    "unsupported_action",
    "element_not_interactable",
    "unsupported_capability",
    "mutation_already_handled",
)
ACTION_ROUTES = ("none", "control_method", "input_event")
# Every action EditorSnapshot advertises, mirrored from EditorSnapshot::action_vocabulary.
ADVERTISED_ACTIONS = (
    "click",
    "focus",
    "set_checked",
    "set_text",
    "type_text",
    "submit",
    "set_value",
    "select_item",
    "select_tab",
    "scroll",
)


def within_fixture(**constraints: Any) -> dict[str, Any]:
    """A selector that can only ever name a fixture control, never an editor control of its own."""
    return {**constraints, "within": {"role": "control", "name": FIXTURE_NAME}}


class BaristaMCPAutomationGateTests(unittest.TestCase):
    """A session that did not opt in before startup neither advertises nor performs mutation."""

    editor: EditorProcess
    client: MCPClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.editor = EditorProcess()
        try:
            cls.editor.start()
            cls.client = MCPClient(cls.editor.wait_for_discovery())
            cls.initialize_result = cls.client.initialize()
        except BaseException:
            cls.editor.stop()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.editor.stop()

    def test_disabled_session_omits_the_mutating_tool(self) -> None:
        tools = self.client.rpc("tools/list", {})["result"]["tools"]
        self.assertEqual([tool["name"] for tool in tools], list(READ_ONLY_TOOLS))
        self.assertNotIn(ACT_TOOL, [tool["name"] for tool in tools])
        self.assertIn("read-only", str(self.initialize_result["instructions"]))

    def test_disabled_session_reports_the_frozen_mode(self) -> None:
        status = self.client.structured_tool("barista_status", {})
        self.assertIs(status["automation_enabled"], False)

    def test_direct_call_is_refused_with_a_stable_status(self) -> None:
        response = self.client.rpc(
            "tools/call",
            {
                "name": ACT_TOOL,
                "arguments": {
                    "selector": within_fixture(role="button", name="Brew"),
                    "action": "click",
                },
            },
        )
        result = response["result"]
        self.assertIs(result["isError"], True)
        payload = result["structuredContent"]
        self.assertIs(payload["ok"], False)
        self.assertEqual(payload["status"], "automation_disabled")
        self.assertEqual(payload["route"], "none")
        self.assertIs(payload["changed"], False)
        self.assertEqual(payload["generation"], 0)
        self.assertEqual(payload["handle"], "")
        self.assertNotIn("element", payload)

    def test_refusal_performs_no_action(self) -> None:
        before = read_counters(self.client)
        for arguments in (
            {"selector": within_fixture(role="button", name="Brew"), "action": "click"},
            {"selector": within_fixture(name="Order"), "action": "set_text", "text": "cortado"},
            # A malformed call is refused by the gate before its arguments are even considered:
            # malformed is not absent, and neither one is ever allowed to act.
            {"action": "click"},
            {},
        ):
            with self.subTest(arguments=arguments):
                payload = self.client.structured_tool(ACT_TOOL, arguments)
                self.assertEqual(payload["status"], "automation_disabled")
        self.assertEqual(read_counters(self.client), before)
        order = find_one(self.client, within_fixture(name="Order"))
        self.assertEqual(order["text"], "")


class BaristaMCPAutomationSettingTests(unittest.TestCase):
    """The project setting is a second, independent route into the same frozen mutation mode."""

    def test_project_setting_enables_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-automation-setting-") as temporary:
            project_dir = Path(temporary)
            write_test_project(
                project_dir,
                "[barista_mcp]\nautomation/enabled=true",
                extra_plugins=("barista_mcp_test_fixture",),
            )
            editor = EditorProcess(project_dir)
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery())
                client.initialize()
                tools = [tool["name"] for tool in client.rpc("tools/list", {})["result"]["tools"]]
                self.assertIn(ACT_TOOL, tools)
                self.assertIs(client.structured_tool("barista_status", {})["automation_enabled"], True)
            finally:
                editor.stop()

    def test_non_boolean_setting_is_not_an_opt_in(self) -> None:
        """Malformed is not absent and unknown is not default: only a boolean true enables mutation."""
        with tempfile.TemporaryDirectory(prefix="barista-mcp-automation-bad-setting-") as temporary:
            project_dir = Path(temporary)
            write_test_project(
                project_dir,
                '[barista_mcp]\nautomation/enabled="true"',
                extra_plugins=("barista_mcp_test_fixture",),
            )
            editor = EditorProcess(project_dir)
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery())
                client.initialize()
                tools = [tool["name"] for tool in client.rpc("tools/list", {})["result"]["tools"]]
                self.assertNotIn(ACT_TOOL, tools)
                self.assertIs(client.structured_tool("barista_status", {})["automation_enabled"], False)
            finally:
                editor.stop()

    def test_a_near_miss_argument_is_not_an_opt_in(self) -> None:
        editor = EditorProcess(extra_args=("--", f"{AUTOMATION_ARGUMENT}=1"))
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            client.initialize()
            self.assertIs(client.structured_tool("barista_status", {})["automation_enabled"], False)
        finally:
            editor.stop()


def read_counters(client: MCPClient) -> dict[str, int]:
    """Reads the fixture's own signal counters out of a fresh snapshot."""
    label = find_one(client, {"name": COUNTERS_NAME})
    counters: dict[str, int] = {}
    for entry in str(label["text"]).split(" "):
        name, _, value = entry.partition("=")
        counters[name] = int(value)
    return counters


def find_one(client: MCPClient, selector: dict[str, Any]) -> dict[str, Any]:
    result = client.structured_tool(
        "find_editor_ui", {"selector": selector, "require_unique": True}
    )
    if not result["ok"]:
        raise AssertionError(f"Selector {selector!r} did not name one element: {result!r}")
    return result["matches"][0]


class BaristaMCPActionTests(unittest.TestCase):
    """An opted-in session performs every advertised action through a documented public route."""

    editor: EditorProcess
    client: MCPClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.editor = EditorProcess(extra_args=("--", AUTOMATION_ARGUMENT))
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

    def act(self, **arguments: Any) -> dict[str, Any]:
        return self.client.structured_tool(ACT_TOOL, arguments)

    def counters(self) -> dict[str, int]:
        return read_counters(self.client)

    def _tool(self) -> dict[str, Any]:
        for tool in self.client.rpc("tools/list", {})["result"]["tools"]:
            if tool["name"] == ACT_TOOL:
                return tool
        raise AssertionError(f"{ACT_TOOL} is not advertised")

    def test_enabled_session_advertises_a_closed_contract(self) -> None:
        tool = self._tool()
        self.assertIs(tool["inputSchema"]["additionalProperties"], False)
        self.assertEqual(tool["inputSchema"]["required"], ["selector", "action"])
        self.assertEqual(tuple(tool["inputSchema"]["properties"]["action"]["enum"]), ADVERTISED_ACTIONS)
        self.assertEqual(
            tuple(tool["inputSchema"]["properties"]["scroll_axis"]["enum"]), ("vertical", "horizontal")
        )
        index = tool["inputSchema"]["properties"]["index"]
        self.assertEqual((index["minimum"], index["maximum"]), (0, 4095))
        offset = tool["inputSchema"]["properties"]["scroll_offset"]
        self.assertEqual((offset["minimum"], offset["maximum"]), (0, 1000000))
        output = tool["outputSchema"]
        self.assertEqual(tuple(output["properties"]["status"]["enum"]), ACTION_STATUSES)
        self.assertEqual(tuple(output["properties"]["route"]["enum"]), ACTION_ROUTES)
        self.assertIs(self.client.structured_tool("barista_status", {})["automation_enabled"], True)

    def test_focus_uses_the_control_method_route(self) -> None:
        # Focus somewhere else first, so this test never depends on where focus already was.
        self.act(selector=within_fixture(name="Notes"), action="focus")
        result = self.act(selector=within_fixture(name="Order"), action="focus")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["route"], "control_method")
        self.assertIs(result["changed"], True)
        self.assertTrue(result["handle"].startswith("el:"))
        self.assertGreater(result["generation"], 0)
        self.assertIs(result["element"]["focused"], True)
        # The action is observed through a fresh capture, not through the tool's report of itself.
        self.assertIs(find_one(self.client, within_fixture(name="Order"))["focused"], True)

    def test_set_text_and_type_text_and_submit(self) -> None:
        cleared = self.act(selector=within_fixture(name="Order"), action="set_text", text="")
        self.assertTrue(cleared["ok"], cleared)
        self.assertEqual(cleared["route"], "control_method")

        written = self.act(selector=within_fixture(name="Order"), action="set_text", text="cortado")
        self.assertEqual(written["route"], "control_method")
        self.assertIs(written["changed"], True)
        self.assertEqual(written["element"]["text"], "cortado")
        self.assertEqual(find_one(self.client, within_fixture(name="Order"))["text"], "cortado")

        self.act(selector=within_fixture(name="Order"), action="set_text", text="")
        typed = self.act(selector=within_fixture(name="Order"), action="type_text", text="flat")
        self.assertTrue(typed["ok"], typed)
        # Typing is a user-fidelity route: one public input event per character.
        self.assertEqual(typed["route"], "input_event")
        self.assertEqual(typed["element"]["text"], "flat")
        self.assertEqual(find_one(self.client, within_fixture(name="Order"))["text"], "flat")

        before = self.counters()
        submitted = self.act(selector=within_fixture(name="Order"), action="submit")
        self.assertTrue(submitted["ok"], submitted)
        self.assertEqual(submitted["route"], "input_event")
        self.assertEqual(self.counters()["submits"], before["submits"] + 1)

        area = self.act(selector=within_fixture(name="Notes"), action="set_text", text="pour over")
        self.assertTrue(area["ok"], area)
        self.assertEqual(area["element"]["text"], "pour over")

        # "changed" reports what Barista can verify, so writing the same text again is not a change.
        # Element identity that is scoped to a capture must never leak into that verdict.
        repeated = self.act(selector=within_fixture(name="Notes"), action="set_text", text="pour over")
        self.assertTrue(repeated["ok"], repeated)
        self.assertIs(repeated["changed"], False)

    def test_click_uses_the_public_input_route(self) -> None:
        before = self.counters()
        result = self.act(selector=within_fixture(role="button", name="Brew"), action="click")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["route"], "input_event")
        self.assertEqual(self.counters()["clicks"], before["clicks"] + 1)

    def test_set_checked_and_set_value_and_tabs_and_items_and_scroll(self) -> None:
        before = self.counters()
        checked = self.act(selector=within_fixture(name="Decaf"), action="set_checked", checked=True)
        self.assertTrue(checked["ok"], checked)
        self.assertEqual(checked["route"], "control_method")
        self.assertIs(checked["element"]["state"]["pressed"], True)
        self.assertEqual(self.counters()["toggles"], before["toggles"] + 1)

        value = self.act(selector=within_fixture(name="Shots"), action="set_value", value=3)
        self.assertTrue(value["ok"], value)
        self.assertEqual(value["element"]["state"]["value"], 3)
        self.assertEqual(self.counters()["value_changes"], before["value_changes"] + 1)

        tab = self.act(selector=within_fixture(name="Stations"), action="select_tab", index=1)
        self.assertTrue(tab["ok"], tab)
        self.assertEqual(tab["element"]["state"]["current_tab"], 1)
        self.assertEqual(self.counters()["tab_changes"], before["tab_changes"] + 1)

        item = self.act(selector=within_fixture(name="Beans"), action="select_item", index=1)
        self.assertTrue(item["ok"], item)
        self.assertEqual(item["element"]["state"]["selected_items"], [1])

        scrolled = self.act(
            selector=within_fixture(name="Menu Scroll"),
            action="scroll",
            scroll_axis="vertical",
            scroll_offset=12,
        )
        self.assertTrue(scrolled["ok"], scrolled)
        self.assertEqual(scrolled["element"]["state"]["scroll_vertical"], 12)

    def test_selector_failures_are_preserved_and_never_act(self) -> None:
        before = self.counters()
        cases = {
            "no_match": {"selector": {"name": "No Such Element Anywhere"}, "action": "focus"},
            "ambiguous_selector": {"selector": {"role": "button"}, "action": "click"},
            "invalid_selector": {"selector": {}, "action": "focus"},
            "stale_handle": {"selector": {"handle": "el:99999999999"}, "action": "focus"},
        }
        for status, arguments in cases.items():
            with self.subTest(status=status):
                result = self.act(**arguments)
                self.assertIs(result["ok"], False, result)
                self.assertEqual(result["status"], status)
                self.assertEqual(result["route"], "none")
                self.assertIs(result["changed"], False)
                self.assertNotIn("element", result)
                # A failure decided after a capture names the capture it was decided against; one
                # refused before any capture names none.
                if status in ("no_match", "ambiguous_selector"):
                    self.assertGreater(result["generation"], 0, result)
                else:
                    self.assertEqual(result["generation"], 0, result)
        # A truncated capture cannot show a selector names exactly one element, so it never acts.
        truncated = self.act(
            selector=within_fixture(role="button", name="Brew"), action="click", max_depth=8
        )
        self.assertEqual(truncated["status"], "ambiguous_selector")
        self.assertEqual(self.counters(), before)

    def test_unadvertised_and_uninteractable_elements_are_refused(self) -> None:
        before = self.counters()
        # A text field advertises no click; the element's own advertised actions are the contract.
        unsupported = self.act(selector=within_fixture(name="Order"), action="click")
        self.assertEqual(unsupported["status"], "unsupported_action")
        self.assertNotEqual(unsupported["handle"], "")
        self.assertIs(unsupported["changed"], False)

        label = self.act(selector={"name": COUNTERS_NAME}, action="focus")
        self.assertEqual(label["status"], "unsupported_action")

        # A Tree does not advertise select_item, because this build has no bounded public route for it:
        # an element never advertises a capability the driver would deterministically refuse.
        tree_element = find_one(self.client, within_fixture(name="Roasts"))
        self.assertNotIn("select_item", tree_element["actions"])
        tree = self.act(selector=within_fixture(name="Roasts"), action="select_item", index=0)
        self.assertEqual(tree["status"], "unsupported_action")

        disabled = self.act(selector=within_fixture(role="button", name="Grind"), action="click")
        self.assertEqual(disabled["status"], "element_not_interactable")

        not_editable = self.act(selector=within_fixture(name="Grind Size"), action="set_value", value=6)
        self.assertEqual(not_editable["status"], "element_not_interactable")
        self.assertEqual(self.counters(), before)

    def test_invalid_arguments_never_reach_the_editor(self) -> None:
        before = self.counters()
        order = within_fixture(name="Order")
        for description, arguments in (
            ("missing selector", {"action": "focus"}),
            ("unknown action", {"selector": order, "action": "explode"}),
            ("unknown property", {"selector": order, "action": "focus", "bogus": 1}),
            ("missing action field", {"selector": order, "action": "set_text"}),
            ("field of another action", {"selector": order, "action": "click", "text": "x"}),
            ("wrong field type", {"selector": order, "action": "set_text", "text": 7}),
            ("over-long typing", {"selector": order, "action": "type_text", "text": "x" * 257}),
            ("index out of schema range", {"selector": order, "action": "select_item", "index": 4096}),
            (
                "value outside the element range",
                {"selector": within_fixture(name="Shots"), "action": "set_value", "value": 99},
            ),
            (
                "item index outside the element",
                {"selector": within_fixture(name="Beans"), "action": "select_item", "index": 99},
            ),
            (
                "unknown scroll axis",
                {
                    "selector": within_fixture(name="Menu Scroll"),
                    "action": "scroll",
                    "scroll_axis": "diagonal",
                    "scroll_offset": 1,
                },
            ),
        ):
            with self.subTest(case=description):
                result = self.act(**arguments)
                self.assertIs(result["ok"], False, result)
                self.assertEqual(result["status"], "invalid_arguments", result)
                self.assertEqual(result["route"], "none")
                self.assertIs(result["changed"], False)
        self.assertEqual(self.counters(), before)

    def test_one_accepted_request_performs_at_most_one_mutation(self) -> None:
        before = self.counters()
        click = {
            "name": ACT_TOOL,
            "arguments": {
                "selector": within_fixture(role="button", name="Brew"),
                "action": "click",
            },
        }
        status, body = self.client.request(
            [
                {"jsonrpc": "2.0", "id": 9001, "method": "tools/call", "params": click},
                {"jsonrpc": "2.0", "id": 9002, "method": "tools/call", "params": click},
            ]
        )
        self.assertEqual(status, 200, body)
        responses = {entry["id"]: entry["result"] for entry in json.loads(body)}
        first = responses[9001]["structuredContent"]
        second = responses[9002]["structuredContent"]
        self.assertTrue(first["ok"], first)
        self.assertIs(second["ok"], False, second)
        self.assertEqual(second["status"], "mutation_already_handled")
        self.assertEqual(second["route"], "none")
        # The budget is spent on the attempt, so the batch performed exactly one click.
        self.assertEqual(self.counters()["clicks"], before["clicks"] + 1)

        # A later request carries its own budget: the limit is per accepted request, not per session.
        again = self.act(selector=within_fixture(role="button", name="Brew"), action="click")
        self.assertTrue(again["ok"], again)
        self.assertEqual(self.counters()["clicks"], before["clicks"] + 2)

    def test_real_client_bytes_are_accepted_verbatim(self) -> None:
        """Real-input evidence: the exact bytes MCPClient.request emits (mcp_test_support.py:134)."""
        before = self.counters()
        body = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 4242,
                "method": "tools/call",
                "params": {
                    "name": ACT_TOOL,
                    "arguments": {
                        "selector": within_fixture(role="button", name="Brew"),
                        "action": "click",
                    },
                },
            },
            separators=(",", ":"),
        ).encode("utf-8")
        request = (
            b"POST " + self.client.path.encode("utf-8") + b" HTTP/1.1\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Authorization: Bearer " + self.client.token.encode("utf-8") + b"\r\n"
            b"Content-Type: application/json\r\n"
            b"Accept: application/json\r\n"
            b"Content-Length: " + str(len(body)).encode("utf-8") + b"\r\n\r\n" + body
        )
        status, response_body = self.client.raw_socket_request(request)
        self.assertEqual(status, 200, response_body)
        payload = json.loads(response_body)["result"]["structuredContent"]
        self.assertTrue(payload["ok"], payload)
        self.assertEqual(payload["action"], "click")
        self.assertEqual(self.counters()["clicks"], before["clicks"] + 1)

    def test_every_advertised_status_has_one_consumer_verdict(self) -> None:
        """Vocabulary closure: every advertised status is either provoked here or explained here."""
        provoked = {
            "ok": {"selector": within_fixture(name="Order"), "action": "focus"},
            "invalid_arguments": {"selector": within_fixture(name="Order"), "action": "set_text"},
            "invalid_selector": {"selector": {}, "action": "focus"},
            "no_match": {"selector": {"name": "No Such Element Anywhere"}, "action": "focus"},
            "ambiguous_selector": {"selector": {"role": "button"}, "action": "focus"},
            "stale_handle": {"selector": {"handle": "el:99999999999"}, "action": "focus"},
            "unsupported_action": {"selector": {"name": COUNTERS_NAME}, "action": "focus"},
            "element_not_interactable": {
                "selector": within_fixture(role="button", name="Grind"),
                "action": "click",
            },
        }
        for status, arguments in provoked.items():
            with self.subTest(status=status):
                self.assertEqual(self.act(**arguments)["status"], status)
        # These three are reachable only from states a client cannot request from this session:
        # automation_disabled and mutation_already_handled are covered by their own tests above, and
        # unsupported_capability requires an editor session with no automation service at all.
        covered = set(provoked) | {
            "automation_disabled",
            "mutation_already_handled",
            "unsupported_capability",
        }
        self.assertEqual(covered, set(ACTION_STATUSES))


class BaristaMCPActionPortabilityTests(unittest.TestCase):
    def test_pinned_extension_api_covers_every_action_route(self) -> None:
        """Portability evidence: every engine symbol an action route calls is in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))
        for class_name in profile["enabled_classes"]:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, classes)

        required_classes = (
            "InputEvent",
            "InputEventKey",
            "InputEventMouse",
            "InputEventMouseButton",
            "InputEventMouseMotion",
            "ScrollContainer",
            "Viewport",
        )
        for class_name in required_classes:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, profile["enabled_classes"])

        required_methods = {
            "BaseButton": ("is_toggle_mode", "set_pressed"),
            "Control": ("get_focus_mode", "get_global_rect", "grab_focus", "has_focus"),
            "CanvasItem": ("is_visible_in_tree",),
            "InputEventKey": ("set_keycode", "set_pressed", "set_unicode"),
            "InputEventMouse": ("set_button_mask", "set_global_position", "set_position"),
            "InputEventMouseButton": ("set_button_index", "set_pressed"),
            "ItemList": ("get_item_count", "is_item_disabled", "is_item_selectable", "select"),
            "LineEdit": ("is_editable", "set_text"),
            "Node": ("get_viewport", "is_ancestor_of"),
            "OS": ("get_cmdline_user_args",),
            "ProjectSettings": ("get_setting",),
            "Range": ("get_max", "get_min", "set_value"),
            "ScrollContainer": ("get_h_scroll", "get_v_scroll", "set_h_scroll", "set_v_scroll"),
            "TabBar": ("get_tab_count", "is_tab_disabled", "is_tab_hidden", "set_current_tab"),
            "TabContainer": ("get_tab_count", "is_tab_disabled", "is_tab_hidden", "set_current_tab"),
            "TextEdit": ("is_editable", "set_text"),
            "Viewport": ("gui_get_hovered_control", "push_input"),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)

        utilities = {entry["name"] for entry in api["utility_functions"]}
        self.assertIn("instance_from_id", utilities)


if __name__ == "__main__":
    unittest.main()
