# test_mcp_waits_events.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

"""Acceptance coverage for cooperative waits, the Barista event ring, and action traces."""

from __future__ import annotations

import json
import re
import socket
import sys
import time
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_test_support import (  # noqa: E402
    ROOT,
    EditorProcess,
    MCPClient,
)

PINNED_EXTENSION_API = ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json"
BUILD_PROFILE = ROOT / "build_profile.json"
WAIT_TOOL = "wait_for_editor"
EVENTS_TOOL = "poll_barista_events"
ACT_TOOL = "act_on_editor_ui"
AUTOMATION_ARGUMENT = "--barista-mcp-automation"
FIXTURE_NAME = "Barista Test Fixture"

# One definition of the wait status vocabulary, mirrored from EditorWaitManager::status_vocabulary in
# src/editor_wait_manager.cpp. Every consumer below must handle or reject every value.
WAIT_STATUSES = (
    "pending",
    "complete",
    "not_started",
    "wait_timeout",
    "wait_cancelled",
    "wait_not_found",
    "invalid_arguments",
    "capacity_reached",
    "unsupported_capability",
)
# Mirrored from EditorWaitManager::condition_vocabulary.
CONDITION_TYPES = (
    "frames_elapsed",
    "selector_appears",
    "selector_disappears",
    "selector_state",
    "focus_changed",
    "play_state",
    "filesystem_settles",
)
# Mirrored from EditorWaitManager::reported_condition_vocabulary: every requestable type plus the
# value reported for a request refused before any condition could be read.
REPORTED_CONDITIONS = ("none", *CONDITION_TYPES)
# Mirrored from EditorEventLog::status_vocabulary and EditorEventLog::type_vocabulary.
EVENT_STATUSES = ("ok", "marker_expired", "invalid_marker")
EVENT_TYPES = ("lifecycle", "action_begin", "action_end", "wait_begin", "wait_end")
# Mirrored from EditorWaitLimits in src/editor_wait_manager.h and EditorEventLimits in
# src/editor_event_log.h.
MAX_WAITS = 16
MIN_TIMEOUT_MS = 1
MAX_TIMEOUT_MS = 30000
MAX_FRAMES = 600
MAX_PRIME_MS = 10000
MAX_EVENTS = 256
MAX_EVENT_LIMIT = 100

WAIT_ID_PATTERN = re.compile(r"^wait:[0-9a-f]{32}$")


def within_fixture(**constraints: Any) -> dict[str, Any]:
    """A selector that can only ever name a fixture control, never an editor control of its own."""
    return {**constraints, "within": {"role": "control", "name": FIXTURE_NAME}}


class WaitClientMixin:
    client: MCPClient

    def wait(self, **arguments: Any) -> dict[str, Any]:
        return self.client.structured_tool(WAIT_TOOL, arguments)

    def events(self, **arguments: Any) -> dict[str, Any]:
        return self.client.structured_tool(EVENTS_TOOL, arguments)

    def poll_until_terminal(self, wait_id: str, timeout: float = 15.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last = self.wait(wait_id=wait_id)
            if last["status"] != "pending":
                return last
            time.sleep(0.05)
        raise AssertionError(f"Wait {wait_id!r} never left 'pending': {last!r}")

    def drain(self, wait_id: str) -> None:
        """Consumes a handle so a later test in the same session starts from a clean capacity."""
        if not wait_id:
            return
        self.wait(wait_id=wait_id, cancel=True)
        self.wait(wait_id=wait_id)


class BaristaMCPWaitContractTests(WaitClientMixin, unittest.TestCase):
    """The advertised wait contract is closed, and every boundary field publishes an explicit range."""

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

    def _tool(self, name: str) -> dict[str, Any]:
        for tool in self.client.rpc("tools/list", {})["result"]["tools"]:
            if tool["name"] == name:
                return tool
        raise AssertionError(f"{name} is not advertised")

    def test_wait_tool_advertises_a_closed_ranged_contract(self) -> None:
        tool = self._tool(WAIT_TOOL)
        schema = tool["inputSchema"]
        self.assertIs(schema["additionalProperties"], False)
        self.assertEqual(schema["required"], [])
        self.assertEqual(
            (schema["properties"]["timeout_ms"]["minimum"], schema["properties"]["timeout_ms"]["maximum"]),
            (MIN_TIMEOUT_MS, MAX_TIMEOUT_MS),
        )
        condition = schema["properties"]["condition"]
        self.assertIs(condition["additionalProperties"], False)
        self.assertEqual(condition["required"], ["type"])
        self.assertEqual(tuple(condition["properties"]["type"]["enum"]), CONDITION_TYPES)
        self.assertEqual(
            (condition["properties"]["frames"]["minimum"], condition["properties"]["frames"]["maximum"]),
            (1, MAX_FRAMES),
        )
        self.assertEqual(
            (condition["properties"]["prime_ms"]["minimum"], condition["properties"]["prime_ms"]["maximum"]),
            (1, MAX_PRIME_MS),
        )
        output = tool["outputSchema"]
        self.assertIs(output["additionalProperties"], False)
        self.assertEqual(tuple(output["properties"]["status"]["enum"]), WAIT_STATUSES)
        self.assertEqual(tuple(output["properties"]["condition"]["enum"]), REPORTED_CONDITIONS)

    def test_events_tool_advertises_a_closed_ranged_contract(self) -> None:
        tool = self._tool(EVENTS_TOOL)
        schema = tool["inputSchema"]
        self.assertIs(schema["additionalProperties"], False)
        self.assertEqual(schema["required"], [])
        self.assertEqual(schema["properties"]["marker"]["minimum"], 1)
        self.assertGreater(schema["properties"]["marker"]["maximum"], 0)
        self.assertEqual(
            (schema["properties"]["limit"]["minimum"], schema["properties"]["limit"]["maximum"]),
            (1, MAX_EVENT_LIMIT),
        )
        output = tool["outputSchema"]
        self.assertEqual(tuple(output["properties"]["status"]["enum"]), EVENT_STATUSES)
        event = output["$defs"]["barista_event"]
        self.assertIs(event["additionalProperties"], False)
        self.assertEqual(tuple(event["properties"]["type"]["enum"]), EVENT_TYPES)

    def test_out_of_range_numbers_are_refused_at_the_boundary(self) -> None:
        """An unranged integer once let a value no int64_t can hold reach a narrowing cast."""
        for arguments in (
            {"condition": {"type": "frames_elapsed", "frames": 1}, "timeout_ms": 1e300},
            {"condition": {"type": "frames_elapsed", "frames": 1}, "timeout_ms": MAX_TIMEOUT_MS + 1},
            {"condition": {"type": "frames_elapsed", "frames": 1}, "timeout_ms": 0},
            {"condition": {"type": "frames_elapsed", "frames": MAX_FRAMES + 1}},
            {"condition": {"type": "frames_elapsed", "frames": 0}},
            {"condition": {"type": "filesystem_settles", "require_start": True, "prime_ms": MAX_PRIME_MS + 1}},
        ):
            with self.subTest(arguments=arguments):
                result = self.client.rpc("tools/call", {"name": WAIT_TOOL, "arguments": arguments})["result"]
                self.assertIs(result["isError"], True, result)
                self.assertEqual(result["structuredContent"]["error"], "invalid_arguments")

        # Event indices are issued from 1, so 0 names no event this session could ever have issued.
        for arguments in (
            {"marker": 1e300},
            {"limit": 0},
            {"limit": MAX_EVENT_LIMIT + 1},
            {"marker": -1},
            {"marker": 0},
        ):
            with self.subTest(arguments=arguments):
                result = self.client.rpc("tools/call", {"name": EVENTS_TOOL, "arguments": arguments})["result"]
                self.assertIs(result["isError"], True, result)

    def test_a_satisfied_condition_completes_without_creating_a_handle(self) -> None:
        started = self.wait(condition={"type": "play_state", "playing": False})
        self.assertEqual(started["status"], "complete", started)
        self.assertIs(started["ok"], True)
        self.assertEqual(started["wait_id"], "")
        self.assertEqual(started["condition"], "play_state")

    def test_a_pending_wait_is_polled_to_completion(self) -> None:
        started = self.wait(condition={"type": "frames_elapsed", "frames": 2}, timeout_ms=10000)
        self.assertEqual(started["status"], "pending", started)
        self.assertRegex(started["wait_id"], WAIT_ID_PATTERN)
        self.assertGreater(started["remaining_ms"], 0)
        finished = self.poll_until_terminal(started["wait_id"])
        self.assertEqual(finished["status"], "complete", finished)
        self.assertGreaterEqual(finished["frames_observed"], 2)
        # The outcome is delivered exactly once: the poll that reported it destroyed the handle.
        self.assertEqual(self.wait(wait_id=started["wait_id"])["status"], "wait_not_found")

    def test_polling_is_idempotent_until_terminal_consumption(self) -> None:
        started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=20000)
        self.assertEqual(started["status"], "pending", started)
        for _ in range(3):
            self.assertEqual(self.wait(wait_id=started["wait_id"])["status"], "pending")
        self.drain(started["wait_id"])

    def test_a_deadline_expiry_ends_the_wait(self) -> None:
        started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=200)
        self.assertEqual(started["status"], "pending", started)
        finished = self.poll_until_terminal(started["wait_id"])
        self.assertEqual(finished["status"], "wait_timeout", finished)
        self.assertIs(finished["ok"], False)
        self.assertEqual(finished["remaining_ms"], 0)
        self.assertEqual(self.wait(wait_id=started["wait_id"])["status"], "wait_not_found")

    def test_cancellation_is_idempotent_and_ends_the_wait(self) -> None:
        started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=20000)
        self.assertEqual(started["status"], "pending", started)
        first = self.wait(wait_id=started["wait_id"], cancel=True)
        self.assertEqual(first["status"], "wait_cancelled", first)
        second = self.wait(wait_id=started["wait_id"], cancel=True)
        self.assertEqual(second["status"], "wait_cancelled", second)
        consumed = self.wait(wait_id=started["wait_id"])
        self.assertEqual(consumed["status"], "wait_cancelled", consumed)
        self.assertEqual(self.wait(wait_id=started["wait_id"])["status"], "wait_not_found")

    def test_cancelling_a_finished_wait_never_overwrites_its_outcome(self) -> None:
        started = self.wait(condition={"type": "frames_elapsed", "frames": 2}, timeout_ms=10000)
        self.assertEqual(started["status"], "pending", started)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            cancelled = self.wait(wait_id=started["wait_id"], cancel=True)
            if cancelled["status"] != "pending":
                break
            time.sleep(0.05)
        self.assertIn(cancelled["status"], {"complete", "wait_cancelled"}, cancelled)
        self.assertEqual(self.wait(wait_id=started["wait_id"])["status"], cancelled["status"])

    def test_unknown_malformed_and_consumed_wait_ids_are_refused(self) -> None:
        issued = self.wait(condition={"type": "frames_elapsed", "frames": 2}, timeout_ms=10000)
        self.poll_until_terminal(issued["wait_id"])
        for wait_id in (
            # Well-formed but never issued: a foreign id can never be mistaken for a live one.
            "wait:" + "0" * 32,
            "wait:" + "f" * 32,
            # Malformed: the grammar is checked before any lookup.
            "wait:short",
            "wait:" + "G" * 32,
            "el:1234",
            "",
            # Already consumed, so expired by definition.
            issued["wait_id"],
        ):
            with self.subTest(wait_id=wait_id):
                refused = self.wait(wait_id=wait_id)
                self.assertEqual(refused["status"], "wait_not_found", refused)
                self.assertIs(refused["ok"], False)
                self.assertEqual(refused["condition"], "none")
                self.assertEqual(refused["wait_id"], "")

    def test_malformed_requests_are_refused_without_creating_a_wait(self) -> None:
        before = self.wait(condition={"type": "play_state", "playing": False})["active_waits"]
        for arguments in (
            {},
            {"condition": {"type": "frames_elapsed", "frames": 1}, "wait_id": "wait:" + "0" * 32},
            {"condition": {"type": "frames_elapsed"}},
            {"condition": {"type": "frames_elapsed", "frames": 1, "playing": True}},
            {"condition": {"type": "play_state"}},
            {"condition": {"type": "selector_appears"}},
            {"condition": {"type": "selector_appears", "selector": {}}},
            {"condition": {"type": "selector_state", "selector": {"name": "Order"}}},
            {"condition": {"type": "focus_changed", "frames": 2}},
            {"condition": {"type": "filesystem_settles", "prime_ms": 100}},
            {"condition": {"type": "filesystem_settles", "require_start": True, "prime_ms": 5000}, "timeout_ms": 100},
            {"condition": {"type": "frames_elapsed", "frames": 1}, "cancel": True},
            {"wait_id": "wait:" + "0" * 32, "timeout_ms": 100},
        ):
            with self.subTest(arguments=arguments):
                refused = self.wait(**arguments)
                self.assertIn(refused["status"], {"invalid_arguments", "wait_not_found"}, refused)
                self.assertIs(refused["ok"], False)
                self.assertEqual(refused["wait_id"], "")
        # A request the advertised schema itself refuses never reaches the wait manager at all.
        for arguments in (
            {"condition": "frames_elapsed"},
            {"condition": {}},
            {"condition": {"type": "nope"}},
            {"condition": {"type": "frames_elapsed", "frames": 1, "unknown": 1}},
            {"wait_id": 5},
            {"cancel": "yes"},
        ):
            with self.subTest(boundary=arguments):
                result = self.client.rpc("tools/call", {"name": WAIT_TOOL, "arguments": arguments})["result"]
                self.assertIs(result["isError"], True, result)
                self.assertEqual(result["structuredContent"]["error"], "invalid_arguments")
        after = self.wait(condition={"type": "play_state", "playing": False})["active_waits"]
        self.assertEqual(after, before)

    def test_a_state_condition_never_carries_identity(self) -> None:
        refused = self.wait(
            condition={
                "type": "selector_state",
                "selector": within_fixture(name="Order"),
                "state": {"handle": "el:1"},
            }
        )
        self.assertEqual(refused["status"], "invalid_arguments", refused)
        self.assertIn("handle", refused["message"])

    def test_a_handle_barista_never_issued_is_refused(self) -> None:
        for selector in ({"handle": "el:999999999999"}, {"handle": "not-a-handle"}):
            with self.subTest(selector=selector):
                refused = self.wait(condition={"type": "selector_appears", "selector": selector})
                self.assertEqual(refused["status"], "invalid_arguments", refused)
                self.assertEqual(refused["wait_id"], "")

    def test_selector_conditions_are_evaluated_against_a_real_capture(self) -> None:
        appears = self.wait(condition={"type": "selector_appears", "selector": {"name": FIXTURE_NAME}})
        self.assertEqual(appears["status"], "complete", appears)
        self.assertGreaterEqual(appears["detail"]["match_count"], 1)

        disappears = self.wait(
            condition={"type": "selector_disappears", "selector": {"name": "No Such Editor Control"}}
        )
        self.assertEqual(disappears["status"], "complete", disappears)

        state = self.wait(
            condition={
                "type": "selector_state",
                "selector": within_fixture(name="Order"),
                "state": {"enabled": True},
            }
        )
        self.assertEqual(state["status"], "complete", state)

    def test_a_selector_condition_accepts_a_handle_barista_issued(self) -> None:
        """Real-input fixture: the handle is fed back exactly as the selector tool emitted it."""
        found = self.client.structured_tool(
            "find_editor_ui", {"selector": within_fixture(name="Order"), "require_unique": True}
        )
        self.assertIs(found["ok"], True, found)
        handle = found["matches"][0]["handle"]
        appears = self.wait(condition={"type": "selector_appears", "selector": {"handle": handle}})
        self.assertEqual(appears["status"], "complete", appears)

    def test_a_primed_settle_reports_that_nothing_ever_started(self) -> None:
        """The prime-then-drain outcome a naive drain-only loop would have reported as 'complete'."""
        drain_only = self.wait(condition={"type": "filesystem_settles"}, timeout_ms=10000)
        self.assertIn(drain_only["status"], {"complete", "pending"}, drain_only)
        if drain_only["status"] == "pending":
            self.assertEqual(self.poll_until_terminal(drain_only["wait_id"])["status"], "complete")

        primed = self.wait(
            condition={"type": "filesystem_settles", "require_start": True, "prime_ms": 300},
            timeout_ms=10000,
        )
        self.assertEqual(primed["status"], "pending", primed)
        finished = self.poll_until_terminal(primed["wait_id"])
        self.assertEqual(finished["status"], "not_started", finished)
        self.assertIs(finished["ok"], False)
        self.assertIs(finished["detail"]["observed_busy"], False)

    def test_a_prime_window_that_fills_the_deadline_still_reports_not_started(self) -> None:
        """The prime window owns its own expiry, so a timeout can never hide a never-started scan."""
        primed = self.wait(
            condition={"type": "filesystem_settles", "require_start": True, "prime_ms": 300},
            timeout_ms=300,
        )
        self.assertEqual(primed["status"], "pending", primed)
        self.assertEqual(self.poll_until_terminal(primed["wait_id"])["status"], "not_started")

    def test_an_oversized_wait_id_is_still_refused_in_the_wait_contract(self) -> None:
        """A refusal that echoed its input unbounded would answer with a transport error instead."""
        refused = self.wait(wait_id="wait:" + "0" * 700_000)
        self.assertEqual(refused["status"], "wait_not_found", refused["status"])
        self.assertLess(len(refused["message"]), 400)

    def test_an_oversized_field_name_is_still_refused_in_its_own_contract(self) -> None:
        """Same class as the oversized id: no diagnostic may echo client input unbounded."""
        oversized = "x" * 700_000
        refused = self.wait(condition={"type": "selector_appears", "selector": {oversized: "value"}})
        self.assertEqual(refused["status"], "invalid_arguments", refused["status"])
        self.assertLess(len(refused["message"]), 1000)

        # The same input reaches the shared selector parser through find_editor_ui.
        found = self.client.structured_tool("find_editor_ui", {"selector": {oversized: "value"}})
        self.assertEqual(found["status"], "invalid_selector", found["status"])
        self.assertLess(len(found["message"]), 1000)

        # And the schema validator itself quotes an unknown property no less carefully.
        result = self.client.rpc(
            "tools/call", {"name": WAIT_TOOL, "arguments": {oversized: 1}}
        )["result"]
        self.assertIs(result["isError"], True)
        self.assertEqual(result["structuredContent"]["error"], "invalid_arguments")
        self.assertLess(len(result["structuredContent"]["message"]), 1000)

    def test_wait_ids_are_opaque_and_unique(self) -> None:
        issued = []
        for _ in range(4):
            started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=20000)
            self.assertRegex(started["wait_id"], WAIT_ID_PATTERN)
            issued.append(started["wait_id"])
        self.assertEqual(len(set(issued)), len(issued))
        for wait_id in issued:
            self.drain(wait_id)

    def test_a_pending_wait_never_blocks_editor_frames(self) -> None:
        """A wait that blocked the editor thread would prevent the very state it is waiting on."""
        started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=20000)
        self.assertEqual(started["status"], "pending", started)
        try:
            begin = time.monotonic()
            snapshot = self.client.structured_tool("inspect_editor_ui", {"max_depth": 4})
            elapsed = time.monotonic() - begin
            self.assertGreater(snapshot["element_count"], 0)
            self.assertLess(elapsed, 2.0, f"An unrelated call took {elapsed:.2f}s while a wait was pending")
        finally:
            self.drain(started["wait_id"])

    def test_every_wait_status_is_provoked_or_justified(self) -> None:
        """Vocabulary closure: one definition, and every value is reachable or explicitly not."""
        provoked = set()
        provoked.add(self.wait(condition={"type": "play_state", "playing": False})["status"])
        provoked.add(self.wait()["status"])
        provoked.add(self.wait(wait_id="wait:" + "0" * 32)["status"])
        started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=250)
        provoked.add(started["status"])
        provoked.add(self.poll_until_terminal(started["wait_id"])["status"])
        cancelled = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=20000)
        provoked.add(self.wait(wait_id=cancelled["wait_id"], cancel=True)["status"])
        self.wait(wait_id=cancelled["wait_id"])
        primed = self.wait(
            condition={"type": "filesystem_settles", "require_start": True, "prime_ms": 200}, timeout_ms=10000
        )
        provoked.add(self.poll_until_terminal(primed["wait_id"])["status"])
        # capacity_reached has its own session below, because filling the table would otherwise leave
        # this shared session at its published maximum. unsupported_capability needs an editor session
        # with no base control at all, which a client cannot request.
        covered = provoked | {"capacity_reached", "unsupported_capability"}
        self.assertEqual(covered, set(WAIT_STATUSES))


class BaristaMCPWaitCapacityTests(WaitClientMixin, unittest.TestCase):
    """Filling the published handle table refuses new work instead of growing without limit."""

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

    def test_capacity_is_bounded_and_a_refusal_consumes_none_of_it(self) -> None:
        issued = []
        for index in range(MAX_WAITS):
            started = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=30000)
            self.assertEqual(started["status"], "pending", (index, started))
            self.assertEqual(started["active_waits"], index + 1)
            issued.append(started["wait_id"])
        refused = self.wait(condition={"type": "frames_elapsed", "frames": MAX_FRAMES}, timeout_ms=30000)
        self.assertEqual(refused["status"], "capacity_reached", refused)
        self.assertIs(refused["ok"], False)
        self.assertEqual(refused["wait_id"], "")
        self.assertEqual(refused["active_waits"], MAX_WAITS)
        # The refusal left no ring entry: only the accepted starts are recorded.
        page = self.events(limit=MAX_EVENT_LIMIT)
        begins = [event for event in page["events"] if event["type"] == "wait_begin"]
        self.assertEqual(len(begins), MAX_WAITS)
        for wait_id in issued:
            self.drain(wait_id)
        after = self.wait(condition={"type": "frames_elapsed", "frames": 2}, timeout_ms=10000)
        self.assertEqual(after["status"], "pending", after)
        self.drain(after["wait_id"])


class BaristaMCPEventTests(WaitClientMixin, unittest.TestCase):
    """Event indices are monotonic and a valid marker never re-reads an event it already saw."""

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

    def test_the_ring_publishes_the_session_lifecycle(self) -> None:
        page = self.events()
        self.assertIs(page["ok"], True, page)
        self.assertEqual(page["status"], "ok")
        self.assertEqual(page["earliest_marker"], 1)
        lifecycle = [event for event in page["events"] if event["type"] == "lifecycle"]
        self.assertTrue(lifecycle, page)
        self.assertEqual(lifecycle[0]["operation"], "server_start")
        self.assertEqual(lifecycle[0]["index"], 1)
        for event in page["events"]:
            self.assertIn(event["type"], EVENT_TYPES)

    def test_a_marker_paginates_without_duplicates(self) -> None:
        """Real-input fixture: every marker fed back is the one the server itself emitted."""
        self.wait(condition={"type": "play_state", "playing": False})
        pending = self.wait(condition={"type": "play_state", "playing": True}, timeout_ms=100)
        # A wait left outstanding would record its own end at an unpredictable point later, so it is
        # brought to an outcome here rather than allowed to land in another test's page.
        self.assertEqual(self.poll_until_terminal(pending["wait_id"])["status"], "wait_timeout")

        whole = self.events(limit=MAX_EVENT_LIMIT)
        expected = [event["index"] for event in whole["events"]]
        self.assertEqual(expected, sorted(expected))
        self.assertEqual(len(expected), len(set(expected)))

        seen: list[int] = []
        marker = whole["earliest_marker"]
        for _ in range(len(expected) + 2):
            page = self.events(marker=marker, limit=1)
            self.assertEqual(page["status"], "ok", page)
            self.assertLessEqual(page["count"], 1)
            seen.extend(event["index"] for event in page["events"])
            self.assertGreaterEqual(page["marker"], marker)
            marker = page["marker"]
            if not page["has_more"]:
                break
        self.assertEqual(seen[: len(expected)], expected)
        self.assertEqual(len(seen), len(set(seen)))

    def test_a_marker_at_the_head_returns_only_new_events(self) -> None:
        head = self.events(limit=MAX_EVENT_LIMIT)
        marker = head["latest_marker"]
        empty = self.events(marker=marker)
        self.assertEqual(empty["status"], "ok", empty)
        self.assertEqual(empty["count"], 0)
        self.assertIs(empty["has_more"], False)
        self.assertEqual(empty["marker"], marker)

        self.wait(condition={"type": "play_state", "playing": False})
        fresh = self.events(marker=marker)
        self.assertGreater(fresh["count"], 0, fresh)
        self.assertTrue(all(event["index"] >= marker for event in fresh["events"]))

    def test_a_marker_this_session_never_issued_is_refused(self) -> None:
        head = self.events()
        refused = self.events(marker=head["latest_marker"] + 1000)
        self.assertEqual(refused["status"], "invalid_marker", refused)
        self.assertIs(refused["ok"], False)
        self.assertEqual(refused["count"], 0)

    def test_waits_publish_their_own_begin_and_end(self) -> None:
        marker = self.events(limit=MAX_EVENT_LIMIT)["latest_marker"]
        started = self.wait(condition={"type": "frames_elapsed", "frames": 2}, timeout_ms=10000)
        self.poll_until_terminal(started["wait_id"])
        page = self.events(marker=marker, limit=MAX_EVENT_LIMIT)
        begins = [
            event
            for event in page["events"]
            if event["type"] == "wait_begin" and event["operation"] == "frames_elapsed"
        ]
        ends = [
            event
            for event in page["events"]
            if event["type"] == "wait_end" and event["operation"] == "frames_elapsed"
        ]
        self.assertEqual(len(begins), 1, page)
        self.assertEqual(len(ends), 1, page)
        self.assertEqual(ends[0]["outcome"], "complete")
        self.assertLess(begins[0]["index"], ends[0]["index"])
        self.assertEqual(begins[0]["detail"]["wait_id"], started["wait_id"])


class BaristaMCPEventRingCapacityTests(WaitClientMixin, unittest.TestCase):
    """The ring is fixed storage: it evicts rather than growing, and it says so explicitly.

    This runs in its own editor session because overrunning the ring is destructive to every marker
    another test in the same session might still hold.
    """

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

    def test_an_expired_marker_reports_the_earliest_available(self) -> None:
        fresh = self.events()
        self.assertEqual(fresh["earliest_marker"], 1, fresh)
        # Every wait records a begin and an end, so this overruns the published ring capacity.
        for _ in range((MAX_EVENTS // 2) + 8):
            self.wait(condition={"type": "play_state", "playing": False})
        expired = self.events(marker=1)
        self.assertEqual(expired["status"], "marker_expired", expired)
        self.assertIs(expired["ok"], False)
        self.assertEqual(expired["count"], 0)
        self.assertGreater(expired["earliest_marker"], 1)
        self.assertEqual(expired["marker"], expired["earliest_marker"])
        self.assertGreater(expired["dropped"], 0)
        resumed = self.events(marker=expired["marker"], limit=MAX_EVENT_LIMIT)
        self.assertEqual(resumed["status"], "ok", resumed)
        self.assertGreater(resumed["count"], 0)
        self.assertLessEqual(len(resumed["events"]), MAX_EVENT_LIMIT)

        page = self.events(limit=MAX_EVENT_LIMIT)
        self.assertEqual(page["limits"]["max_events"], MAX_EVENTS)
        self.assertLessEqual(page["latest_marker"] - page["earliest_marker"], MAX_EVENTS)


class BaristaMCPActionTraceTests(WaitClientMixin, unittest.TestCase):
    """An opted-in session records what it did and publishes a bounded trace on a refusal."""

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

    def test_actions_are_recorded_around_their_outcome(self) -> None:
        marker = self.events(limit=MAX_EVENT_LIMIT)["latest_marker"]
        performed = self.act(selector=within_fixture(name="Order"), action="focus")
        self.assertIs(performed["ok"], True, performed)
        page = self.events(marker=marker, limit=MAX_EVENT_LIMIT)
        types = [event["type"] for event in page["events"]]
        self.assertEqual(types[:2], ["action_begin", "action_end"])
        self.assertEqual(page["events"][0]["operation"], "focus")
        self.assertEqual(page["events"][1]["outcome"], "ok")

    def test_a_refusal_publishes_a_bounded_trace_and_a_success_does_not(self) -> None:
        refused = self.act(selector=within_fixture(role="button", name="Grind"), action="click")
        self.assertIs(refused["ok"], False, refused)
        trace = refused["trace"]
        self.assertIsInstance(trace, list)
        self.assertTrue(trace)
        self.assertLessEqual(len(trace), 16)
        for entry in trace:
            self.assertIsInstance(entry, str)
            self.assertLessEqual(len(entry), 200)
        self.assertEqual(trace[0], "gate: automation enabled")

        performed = self.act(selector=within_fixture(name="Order"), action="focus")
        self.assertIs(performed["ok"], True, performed)
        self.assertNotIn("trace", performed)

    def test_a_focus_change_completes_a_focus_wait(self) -> None:
        self.act(selector=within_fixture(name="Order"), action="focus")
        started = self.wait(condition={"type": "focus_changed"}, timeout_ms=10000)
        self.assertEqual(started["status"], "pending", started)
        try:
            self.act(selector=within_fixture(name="Notes"), action="focus")
            finished = self.poll_until_terminal(started["wait_id"])
            self.assertEqual(finished["status"], "complete", finished)
        finally:
            self.drain(started["wait_id"])

    def test_a_state_wait_completes_when_the_named_element_changes(self) -> None:
        started = self.wait(
            condition={
                "type": "selector_state",
                "selector": within_fixture(name="Order"),
                "state": {"text": "cortado"},
            },
            timeout_ms=10000,
        )
        self.assertEqual(started["status"], "pending", started)
        try:
            self.act(selector=within_fixture(name="Order"), action="set_text", text="cortado")
            finished = self.poll_until_terminal(started["wait_id"])
            self.assertEqual(finished["status"], "complete", finished)
        finally:
            self.drain(started["wait_id"])
            self.act(selector=within_fixture(name="Order"), action="set_text", text="")


class BaristaMCPWaitShutdownTests(unittest.TestCase):
    def test_shutdown_leaves_no_waits_or_connections(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            discovery = editor.wait_for_discovery()
            client = MCPClient(discovery)
            client.initialize()
            started = client.structured_tool(
                WAIT_TOOL, {"condition": {"type": "frames_elapsed", "frames": MAX_FRAMES}, "timeout_ms": 30000}
            )
            self.assertEqual(started["status"], "pending", started)
        finally:
            editor.stop()
        self.assertIsNotNone(editor.process)
        assert editor.process is not None
        self.assertIsNotNone(editor.process.poll())
        with self.assertRaises(OSError):
            with socket.create_connection((client.host, client.port), timeout=2):
                pass


class BaristaMCPWaitPortabilityTests(unittest.TestCase):
    def test_pinned_extension_api_covers_every_wait_dependency(self) -> None:
        """Portability evidence: every engine symbol a wait or an event reads is in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))

        required_classes = ("Crypto", "EditorFileSystem", "EditorInterface", "Time")
        for class_name in required_classes:
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, profile["enabled_classes"])
                self.assertIn(class_name, classes)

        required_methods = {
            "Crypto": ("generate_random_bytes",),
            "EditorFileSystem": ("is_importing", "is_scanning"),
            "EditorInterface": ("get_resource_filesystem", "is_playing_scene"),
            "Time": ("get_ticks_msec",),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
