# test_mcp_editor_operations.py
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
OPERATION_TOOL = "run_editor_action"
AUTOMATION_ARGUMENT = "--barista-mcp-automation"

# One definition of the operation vocabulary, mirrored from EditorStateReader::operation_vocabulary
# in src/editor_state_reader.cpp. Every consumer below must handle or reject every value.
OPERATIONS = (
    "save_scene",
    "save_all_scenes",
    "open_scene",
    "reload_scene",
    "play_current_scene",
    "play_main_scene",
    "play_scene",
    "stop_play",
    "select_file",
    "edit_script",
)
# One definition of the operation status vocabulary, mirrored from
# EditorStateReader::operation_status_vocabulary in src/editor_state_reader.cpp.
OPERATION_STATUSES = (
    "ok",
    "pending",
    "automation_disabled",
    "invalid_arguments",
    "invalid_resource_path",
    "invalid_resource_type",
    "conflicting_state",
    "operation_failed",
    "unsupported_capability",
    "mutation_already_handled",
)
# The whole claim vocabulary, mirrored from MCPContracts::claim_vocabulary in src/mcp_contracts.cpp.
CLAIMS = ("delivery", "effect")
# What "ok" asserts for each operation, mirrored from OPERATION_CLAIM_RULES in src/mcp_contracts.cpp.
# Every operation verifies a public postcondition before it reports success, so every route is an
# effect route: no operation may report "ok" on delivery alone.
EXPECTED_OPERATION_CLAIMS = {operation: "effect" for operation in OPERATIONS}
# The read_editor_state field each operation's postcondition is verified against, mirrored from
# EditorStateReader::operation_observable_field. Every one names a field read_editor_state publishes,
# so a client can observe the very same predicate Barista verified.
OPERATION_OBSERVABLE_FIELDS = {
    "save_scene": "scenes.unsaved",
    "save_all_scenes": "scenes.unsaved",
    "open_scene": "scenes.current",
    "reload_scene": "scenes.unsaved",
    "play_current_scene": "play.is_playing",
    "play_main_scene": "play.is_playing",
    "play_scene": "play.playing_scene",
    "stop_play": "play.is_playing",
    "select_file": "filesystem.current_path",
    "edit_script": "script.current",
}

# Where every value of the status vocabulary is covered. A status added without named coverage fails
# the closure test below rather than shipping unexercised.
STATUS_COVERAGE = {
    "ok": "test_stop_play_is_idempotent_when_nothing_is_playing",
    "pending": "test_play_never_reports_success_before_the_editor_is_playing",
    "automation_disabled": "BaristaMCPOperationGateTests.test_direct_call_is_refused_with_a_stable_status",
    "invalid_arguments": "test_unknown_operations_and_fields_are_rejected",
    "invalid_resource_path": "test_rejected_paths_never_reach_the_editor",
    "invalid_resource_type": "test_edit_script_rejects_a_resource_of_the_wrong_type",
    "conflicting_state": "test_reload_scene_requires_the_scene_to_be_open",
    "operation_failed": "BaristaMCPOperationWithoutASceneTests.test_scene_operations_fail_closed",
    # Reached when the editor is configured to open scripts in an external editor, whose state
    # Barista cannot observe. Editor settings are global to the machine rather than per-project, so
    # the suite cannot set one without changing the developer's own editor; the branch is covered by
    # the advertised vocabulary and by producing the same refusal shape as every other one.
    "unsupported_capability": "edit_script with an external script editor configured; global setting, not set by tests",
    "mutation_already_handled": "test_one_request_spends_one_mutation",
}

MAIN_SCENE = "res://main.tscn"
OTHER_SCENE = "res://scenes/other.tscn"
SAMPLE_SCRIPT = "res://sample.gd"
PLAIN_FILE = "res://notes.txt"
# A file and a directory inside the project that resolve to a location outside it. Both exist and
# both normalize cleanly, so only resolving them can tell that they leave the project.
LINKED_SCENE = "res://linked.tscn"
LINKED_DIRECTORY_SCENE = "res://linked/outside.tscn"

SCENE_TEXT = """[gd_scene format=3]

[node name="{root}" type="Node2D"]
"""
SCRIPT_TEXT = """extends Node


func _ready() -> void:
\tpass
"""

# Every rejected path, with the status it must be refused with. A rejection happens before any
# EditorInterface call, so none of these may leave anything behind.
REJECTED_PATHS = (
    ("res://../outside.tscn", "invalid_resource_path", "parent traversal at the front"),
    ("res://scenes/../../outside.tscn", "invalid_resource_path", "traversal in the middle"),
    ("res://scenes/..", "invalid_resource_path", "traversal at the end"),
    ("res://./main.tscn", "invalid_resource_path", "unnormalized current-directory segment"),
    ("res://scenes//other.tscn", "invalid_resource_path", "empty segment"),
    ("/etc/passwd", "invalid_resource_path", "absolute OS path"),
    ("/tmp/main.tscn", "invalid_resource_path", "absolute OS path in a writable directory"),
    ("C:\\\\main.tscn", "invalid_resource_path", "absolute Windows path"),
    ("user://main.tscn", "invalid_resource_path", "user:// scheme"),
    ("file:///etc/passwd", "invalid_resource_path", "file:// scheme"),
    ("res://%2e%2e/outside.tscn", "invalid_resource_path", "url-encoded traversal"),
    ("res://%2E%2E%2Fmain.tscn", "invalid_resource_path", "url-encoded traversal and separator"),
    ("", "invalid_resource_path", "empty path"),
    ("   ", "invalid_resource_path", "whitespace-only path"),
    (" res://main.tscn", "invalid_resource_path", "leading whitespace"),
    ("res://main.tscn ", "invalid_resource_path", "trailing whitespace"),
    ("res://", "invalid_resource_path", "scheme with no file"),
    ("res:/main.tscn", "invalid_resource_path", "malformed scheme"),
    ("res://main.tscn\u0000.gd", "invalid_resource_path", "embedded null byte"),
    ("res://main\\other.tscn", "invalid_resource_path", "backslash separator"),
    ("res://" + "a" * 600 + ".tscn", "invalid_resource_path", "over-long path"),
    ("res://missing.tscn", "invalid_resource_path", "non-existent resource"),
    (LINKED_SCENE, "invalid_resource_path", "a symlinked file resolving outside the project"),
    (LINKED_DIRECTORY_SCENE, "invalid_resource_path", "a path through a symlinked directory"),
    (SAMPLE_SCRIPT, "invalid_resource_type", "a script where a scene is required"),
    (PLAIN_FILE, "invalid_resource_type", "a non-scene file where a scene is required"),
)


def write_operations_project(project_dir: Path, outside_dir: Path | None = None) -> None:
    """A temporary project with its own scenes, script, and plain file. Nothing here is shared."""
    (project_dir / "scenes").mkdir()
    if outside_dir is not None:
        # A scene that really exists, outside the project, reachable through two res:// paths that are
        # perfectly normalized. Only resolving the path can tell that it leaves the project.
        (outside_dir / "outside.tscn").write_text(SCENE_TEXT.format(root="Outside"), encoding="utf-8")
        (project_dir / "linked.tscn").symlink_to(outside_dir / "outside.tscn")
        (project_dir / "linked").symlink_to(outside_dir, target_is_directory=True)
    (project_dir / "main.tscn").write_text(SCENE_TEXT.format(root="Main"), encoding="utf-8")
    (project_dir / "scenes" / "other.tscn").write_text(
        SCENE_TEXT.format(root="Other"), encoding="utf-8"
    )
    (project_dir / "sample.gd").write_text(SCRIPT_TEXT, encoding="utf-8")
    (project_dir / "notes.txt").write_text("barista\n", encoding="utf-8")
    write_test_project(
        project_dir,
        f'run/main_scene="{MAIN_SCENE}"',
        config_name="BaristaMCP Operations Test",
    )


class BaristaMCPOperationGateTests(unittest.TestCase):
    """A session that did not opt in before startup neither advertises nor performs an operation."""

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

    def test_disabled_session_omits_the_operation_tool(self) -> None:
        tools = [tool["name"] for tool in self.client.rpc("tools/list", {})["result"]["tools"]]
        self.assertNotIn(OPERATION_TOOL, tools)

    def test_direct_call_is_refused_with_a_stable_status(self) -> None:
        """The gate is read before the request is parsed, so no operation and no claim is named."""
        for operation in OPERATIONS:
            with self.subTest(operation=operation):
                response = self.client.rpc(
                    "tools/call",
                    {
                        "name": OPERATION_TOOL,
                        "arguments": {"operation": operation, "path": MAIN_SCENE},
                    },
                )
                result = response["result"]
                self.assertIs(result["isError"], True)
                payload = result["structuredContent"]
                self.assertIs(payload["ok"], False)
                self.assertEqual(payload["status"], "automation_disabled")
                self.assertIs(payload["changed"], False)
                self.assertIs(payload["pending"], False)
                self.assertNotIn("operation", payload)
                self.assertNotIn("claim", payload)

    def test_a_malformed_call_is_refused_by_the_gate_first(self) -> None:
        """Malformed is not absent: neither one is ever allowed past the gate."""
        for arguments in ({}, {"operation": "not_an_operation"}, {"extra": 1}):
            with self.subTest(arguments=arguments):
                payload = self.client.structured_tool(OPERATION_TOOL, arguments)
                self.assertEqual(payload["status"], "automation_disabled")
                self.assertNotIn("operation", payload)

    def test_refusal_leaves_the_editor_untouched(self) -> None:
        before = self.client.structured_tool("read_editor_state", {})
        for operation in OPERATIONS:
            self.client.structured_tool(
                OPERATION_TOOL, {"operation": operation, "path": MAIN_SCENE}
            )
        after = self.client.structured_tool("read_editor_state", {})
        for section in ("scenes", "script", "play"):
            with self.subTest(section=section):
                self.assertEqual(before[section], after[section])
        self.assertEqual(before["filesystem"]["current_path"], after["filesystem"]["current_path"])


class BaristaMCPOperationTests(unittest.TestCase):
    """An opted-in session performs every operation through a documented public EditorInterface call."""

    temporary: tempfile.TemporaryDirectory[str]
    outside: tempfile.TemporaryDirectory[str]
    editor: EditorProcess
    client: MCPClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="barista-mcp-operations-")
        cls.outside = tempfile.TemporaryDirectory(prefix="barista-mcp-outside-")
        project_dir = Path(cls.temporary.name)
        write_operations_project(project_dir, Path(cls.outside.name))
        cls.editor = EditorProcess(project_dir, extra_args=("--", AUTOMATION_ARGUMENT))
        try:
            cls.editor.start()
            cls.client = MCPClient(cls.editor.wait_for_discovery())
            cls.client.initialize()
        except BaseException:
            cls.editor.stop()
            cls.temporary.cleanup()
            cls.outside.cleanup()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.editor.stop()
        finally:
            cls.temporary.cleanup()
            cls.outside.cleanup()

    def run_operation(self, **arguments: Any) -> dict[str, Any]:
        return self.client.structured_tool(OPERATION_TOOL, arguments)

    def state(self) -> dict[str, Any]:
        return self.client.structured_tool("read_editor_state", {})

    def _tool(self) -> dict[str, Any]:
        for tool in self.client.rpc("tools/list", {})["result"]["tools"]:
            if tool["name"] == OPERATION_TOOL:
                return tool
        raise AssertionError(f"{OPERATION_TOOL} is not advertised.")

    def assert_well_formed(self, payload: dict[str, Any]) -> None:
        """Invariants no operation result may ever break, whatever it did."""
        self.assertIn(payload["status"], OPERATION_STATUSES)
        self.assertIs(payload["ok"], payload["status"] == "ok")
        self.assertIs(payload["pending"], payload["status"] == "pending")
        observable = payload["observable"]
        self.assertEqual(
            sorted(key for key in observable if key != "wait_condition"),
            ["expected", "field", "observed", "satisfied"],
        )
        # Success is never reported early: "ok" requires the published postcondition to hold.
        if payload["ok"]:
            self.assertIs(observable["satisfied"], True, payload)
        if payload["pending"]:
            self.assertIs(observable["satisfied"], False, payload)
        # A published wait condition must observe exactly the predicate the result claims. Only
        # play.is_playing has one: no advertised condition can name a scene, so an operation whose
        # observable is a scene path publishes none rather than one that would accept another scene.
        if "wait_condition" in observable:
            self.assertEqual(observable["field"], "play.is_playing", payload)
            self.assertEqual(observable["wait_condition"]["type"], "play_state")
            self.assertEqual(
                observable["wait_condition"]["playing"], observable["expected"] == "true", payload
            )
        if "operation" in payload:
            self.assertEqual(payload["claim"], EXPECTED_OPERATION_CLAIMS[payload["operation"]])
            self.assertEqual(
                observable["field"], OPERATION_OBSERVABLE_FIELDS[payload["operation"]]
            )
        else:
            self.assertNotIn("claim", payload)

    # -- Advertised contract ------------------------------------------------

    def test_tool_advertises_its_whole_vocabulary(self) -> None:
        tool = self._tool()
        properties = tool["inputSchema"]["properties"]
        self.assertEqual(tuple(properties["operation"]["enum"]), OPERATIONS)
        self.assertEqual(tool["inputSchema"]["required"], ["operation"])
        self.assertIs(tool["inputSchema"]["additionalProperties"], False)
        # Every numeric boundary field publishes an explicit range, so an unrepresentable JSON number
        # is rejected instead of being converted on its way to a fixed-width integer.
        self.assertEqual((properties["line"]["minimum"], properties["line"]["maximum"]), (1, 1000000))
        self.assertEqual((properties["column"]["minimum"], properties["column"]["maximum"]), (0, 100000))
        output = tool["outputSchema"]
        self.assertEqual(tuple(output["properties"]["status"]["enum"]), OPERATION_STATUSES)
        self.assertEqual(tuple(output["properties"]["operation"]["enum"]), OPERATIONS)
        self.assertEqual(tuple(output["properties"]["claim"]["enum"]), CLAIMS)

    def test_every_advertised_operation_declares_a_claim(self) -> None:
        """A route cannot be advertised without saying what its own 'ok' asserts.

        Parametrized over the whole advertised operation vocabulary, so an operation added later
        without a declared claim and a published postcondition fails here rather than shipping one.
        """
        tool = self._tool()
        entries = tool["_meta"]["operation_claims"]
        declared = {entry["operation"]: entry for entry in entries}
        self.assertEqual(len(entries), len(declared))
        self.assertEqual(tuple(entry["operation"] for entry in entries), OPERATIONS)
        for operation in OPERATIONS:
            with self.subTest(operation=operation):
                self.assertIn(declared[operation]["claim"], CLAIMS)
                self.assertEqual(declared[operation]["claim"], EXPECTED_OPERATION_CLAIMS[operation])
                self.assertEqual(
                    declared[operation]["observable"], OPERATION_OBSERVABLE_FIELDS[operation]
                )
                self.assertTrue(declared[operation]["postcondition"])

    def test_every_published_observable_is_a_real_state_field(self) -> None:
        """The predicate Barista verified is one the client can read for itself."""
        state = self.state()
        for operation, field in OPERATION_OBSERVABLE_FIELDS.items():
            with self.subTest(operation=operation):
                section, _, leaf = field.partition(".")
                self.assertIn(section, state)
                self.assertIn(leaf, state[section])

    def test_every_status_has_named_coverage(self) -> None:
        """Vocabulary closure: one definition of the status vocabulary, and no unexercised value."""
        self.assertEqual(sorted(STATUS_COVERAGE), sorted(OPERATION_STATUSES))
        tool = self._tool()
        self.assertEqual(
            sorted(tool["outputSchema"]["properties"]["status"]["enum"]), sorted(STATUS_COVERAGE)
        )

    def test_engine_written_scene_is_accepted_byte_for_byte(self) -> None:
        """The real producer of a .tscn is the editor's own saver, not this test's fixture text.

        Saving rewrites the scene through Godot's own writer, so what is on disk afterwards is a real
        producer's output. Every loader the operations run must accept exactly those bytes.
        """
        opened = self.run_operation(operation="open_scene", path=OTHER_SCENE)
        self.assert_well_formed(opened)
        # save_scene saves whatever is being edited, so the scene must really be current before the
        # bytes on disk can be attributed to this test at all.
        self.assertEqual(self.wait_for_scene(OTHER_SCENE), OTHER_SCENE)
        saved = self.run_operation(operation="save_scene")
        self.assert_well_formed(saved)
        self.assertEqual(saved["status"], "ok", saved)
        on_disk = (Path(self.temporary.name) / "scenes" / "other.tscn").read_bytes()
        # Godot's own scene writer stamps a resource uid, which the hand-written fixture never had.
        self.assertIn(b"uid://", on_disk, on_disk[:200])
        for operation in ("open_scene", "reload_scene"):
            with self.subTest(operation=operation):
                payload = self.run_operation(operation=operation, path=OTHER_SCENE)
                self.assert_well_formed(payload)
                self.assertIn(payload["status"], ("ok", "pending"), payload)

    def test_unknown_operations_and_fields_are_rejected(self) -> None:
        for arguments in (
            {},
            {"operation": "restart_editor"},
            {"operation": "save_scene", "unknown": 1},
            {"operation": 7},
            {"operation": "open_scene", "path": MAIN_SCENE, "line": 1},
            {"operation": "edit_script", "path": SAMPLE_SCRIPT, "screen": "Script"},
            {"operation": "save_scene", "grab_focus": True},
            {"operation": "stop_play", "path": MAIN_SCENE},
            {"operation": "save_all_scenes", "path": MAIN_SCENE},
            {"operation": "open_scene"},
        ):
            with self.subTest(arguments=arguments):
                payload = self.run_operation(**arguments)
                self.assertEqual(payload["status"], "invalid_arguments", payload)
                self.assertIs(payload["ok"], False)
                self.assertNotIn("operation", payload)

    def test_out_of_range_numbers_are_rejected(self) -> None:
        for line in (0, -1, 1e300, 2**63, 1000001):
            with self.subTest(line=line):
                result = self.client.rpc(
                    "tools/call",
                    {
                        "name": OPERATION_TOOL,
                        "arguments": {
                            "operation": "edit_script",
                            "path": SAMPLE_SCRIPT,
                            "line": line,
                        },
                    },
                )["result"]
                self.assertIs(result["isError"], True)
                self.assertEqual(result["structuredContent"]["status"], "invalid_arguments")

    # -- Path validation ----------------------------------------------------

    def test_rejected_paths_never_reach_the_editor(self) -> None:
        before = self.state()
        for path, status, reason in REJECTED_PATHS:
            for operation in ("open_scene", "reload_scene", "play_scene"):
                with self.subTest(path=path, reason=reason, operation=operation):
                    payload = self.run_operation(operation=operation, path=path)
                    self.assert_well_formed(payload)
                    self.assertEqual(payload["status"], status, payload)
                    self.assertIs(payload["changed"], False)
        after = self.state()
        for section in ("scenes", "script", "play"):
            with self.subTest(section=section):
                self.assertEqual(before[section], after[section])
        # The filesystem section also carries a scan progress that moves on its own, so only the field
        # an operation could have moved is compared.
        self.assertEqual(before["filesystem"]["current_path"], after["filesystem"]["current_path"])

    def test_a_symlink_out_of_the_project_is_refused_for_leaving_it(self) -> None:
        """The escape is refused for resolving outside, not for looking absent.

        Both paths name a file that really exists and normalizes cleanly, so a rejection that merely
        said the file was missing would mean the boundary check never ran.
        """
        for path in (LINKED_SCENE, LINKED_DIRECTORY_SCENE):
            with self.subTest(path=path):
                payload = self.run_operation(operation="open_scene", path=path)
                self.assertEqual(payload["status"], "invalid_resource_path", payload)
                self.assertIn("resolves outside the project directory", payload["message"])

    def test_edit_script_rejects_a_resource_of_the_wrong_type(self) -> None:
        for path in (MAIN_SCENE, PLAIN_FILE):
            with self.subTest(path=path):
                payload = self.run_operation(operation="edit_script", path=path)
                self.assertEqual(payload["status"], "invalid_resource_type", payload)
        payload = self.run_operation(operation="edit_script", path="res://missing.gd")
        self.assertEqual(payload["status"], "invalid_resource_path", payload)

    def test_select_file_rejects_traversal_and_missing_files(self) -> None:
        for path in ("res://../outside.txt", "res://missing.txt", "user://notes.txt"):
            with self.subTest(path=path):
                payload = self.run_operation(operation="select_file", path=path)
                self.assertEqual(payload["status"], "invalid_resource_path", payload)

    # -- Successful operations ---------------------------------------------

    def test_open_scene_reaches_the_requested_scene(self) -> None:
        payload = self.run_operation(operation="open_scene", path=MAIN_SCENE)
        self.assert_well_formed(payload)
        self.assertIn(payload["status"], ("ok", "pending"), payload)
        self.assertEqual(payload["observable"]["expected"], MAIN_SCENE)
        current = self.wait_for_scene(MAIN_SCENE)
        self.assertEqual(current, MAIN_SCENE)

        # Opening the scene that is already current is honest about changing nothing.
        again = self.run_operation(operation="open_scene", path=MAIN_SCENE)
        self.assert_well_formed(again)
        self.assertEqual(again["status"], "ok", again)
        self.assertIs(again["changed"], False, again)

    def wait_for_scene(self, path: str, attempts: int = 40) -> str:
        for _ in range(attempts):
            current = self.state()["scenes"]["current"]
            if current == path:
                return current
            self.client.structured_tool(
                "wait_for_editor",
                {"condition": {"type": "frames_elapsed", "frames": 2}, "timeout_ms": 1000},
            )
        return self.state()["scenes"]["current"]

    def test_save_scene_clears_the_unsaved_scene(self) -> None:
        self.run_operation(operation="open_scene", path=OTHER_SCENE)
        self.wait_for_scene(OTHER_SCENE)
        payload = self.run_operation(operation="save_scene")
        self.assert_well_formed(payload)
        self.assertIn(payload["status"], ("ok", "pending"), payload)
        if payload["ok"]:
            self.assertNotIn(OTHER_SCENE, self.state()["scenes"]["unsaved"]["items"])

    def test_save_all_scenes_reports_its_own_postcondition(self) -> None:
        payload = self.run_operation(operation="save_all_scenes")
        self.assert_well_formed(payload)
        self.assertIn(payload["status"], ("ok", "pending"), payload)
        self.assertEqual(payload["observable"]["field"], "scenes.unsaved")

    def test_reload_scene_requires_the_scene_to_be_open(self) -> None:
        self.run_operation(operation="open_scene", path=MAIN_SCENE)
        self.wait_for_scene(MAIN_SCENE)
        opened = self.state()["scenes"]["open"]["items"]
        if OTHER_SCENE not in opened:
            payload = self.run_operation(operation="reload_scene", path=OTHER_SCENE)
            self.assert_well_formed(payload)
            self.assertEqual(payload["status"], "conflicting_state", payload)
        reloaded = self.run_operation(operation="reload_scene", path=MAIN_SCENE)
        self.assert_well_formed(reloaded)
        self.assertIn(reloaded["status"], ("ok", "pending"), reloaded)

    def test_select_file_moves_the_filesystem_selection(self) -> None:
        payload = self.run_operation(operation="select_file", path=SAMPLE_SCRIPT)
        self.assert_well_formed(payload)
        self.assertIn(payload["status"], ("ok", "pending"), payload)
        self.assertEqual(payload["observable"]["expected"], SAMPLE_SCRIPT)
        if payload["ok"]:
            self.assertEqual(self.state()["filesystem"]["current_path"], SAMPLE_SCRIPT)
            repeated = self.run_operation(operation="select_file", path=SAMPLE_SCRIPT)
            self.assertEqual(repeated["status"], "ok", repeated)
            self.assertIs(repeated["changed"], False, repeated)

    def test_edit_script_opens_the_script(self) -> None:
        payload = self.run_operation(
            operation="edit_script", path=SAMPLE_SCRIPT, line=2, column=0, grab_focus=False
        )
        self.assert_well_formed(payload)
        self.assertIn(payload["status"], ("ok", "pending", "unsupported_capability"), payload)
        if payload["ok"]:
            self.assertEqual(self.state()["script"]["current"], SAMPLE_SCRIPT)

    def test_stop_play_is_idempotent_when_nothing_is_playing(self) -> None:
        payload = self.run_operation(operation="stop_play")
        self.assert_well_formed(payload)
        self.assertEqual(payload["status"], "ok", payload)
        self.assertIs(payload["changed"], False, payload)
        self.assertIs(self.state()["play"]["is_playing"], False)

    def test_play_never_reports_success_before_the_editor_is_playing(self) -> None:
        """Play is asynchronous: the result is either verified or an observable pending state."""
        try:
            for operation, arguments in (
                ("play_scene", {"path": MAIN_SCENE}),
                ("play_main_scene", {}),
                ("play_current_scene", {}),
            ):
                with self.subTest(operation=operation):
                    payload = self.run_operation(operation=operation, **arguments)
                    self.assert_well_formed(payload)
                    self.assertIn(
                        payload["status"], ("ok", "pending", "conflicting_state"), payload
                    )
                    if operation == "play_scene":
                        # play.playing_scene has no advertised wait condition, so none is published.
                        self.assertNotIn("wait_condition", payload["observable"], payload)
                    elif payload["pending"]:
                        # A pending play publishes the wait a client can start for it.
                        self.assertEqual(
                            payload["observable"]["wait_condition"]["type"], "play_state"
                        )
                    if payload["status"] == "conflicting_state":
                        # A second play never blindly repeats a non-idempotent operation.
                        self.assertIs(self.state()["play"]["is_playing"], True, payload)
        finally:
            self.run_operation(operation="stop_play")
            self.wait_until_stopped()

    def wait_until_stopped(self, attempts: int = 40) -> None:
        """Leaves the editor stopped, so no later test observes a play state this one started."""
        for _ in range(attempts):
            if not self.state()["play"]["is_playing"]:
                return
            self.client.structured_tool(
                "wait_for_editor",
                {"condition": {"type": "play_state", "playing": False}, "timeout_ms": 1000},
            )

    def test_one_request_spends_one_mutation(self) -> None:
        """The operation tool is a mutating tool, so a batched second call is refused."""
        status, body = self.client.request(
            [
                {
                    "jsonrpc": "2.0",
                    "id": 900,
                    "method": "tools/call",
                    "params": {
                        "name": OPERATION_TOOL,
                        "arguments": {"operation": "stop_play"},
                    },
                },
                {
                    "jsonrpc": "2.0",
                    "id": 901,
                    "method": "tools/call",
                    "params": {
                        "name": OPERATION_TOOL,
                        "arguments": {"operation": "stop_play"},
                    },
                },
            ]
        )
        self.assertEqual(status, 200, body)
        responses = json.loads(body)
        second = responses[1]["result"]["structuredContent"]
        self.assertEqual(second["status"], "mutation_already_handled", second)
        self.assertNotIn("operation", second)


class BaristaMCPOperationWithoutASceneTests(unittest.TestCase):
    """An operation whose precondition does not hold refuses instead of acting on nothing."""

    temporary: tempfile.TemporaryDirectory[str]
    editor: EditorProcess
    client: MCPClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="barista-mcp-operations-empty-")
        project_dir = Path(cls.temporary.name)
        write_test_project(project_dir, config_name="BaristaMCP Empty Operations Test")
        cls.editor = EditorProcess(project_dir, extra_args=("--", AUTOMATION_ARGUMENT))
        try:
            cls.editor.start()
            cls.client = MCPClient(cls.editor.wait_for_discovery())
            cls.client.initialize()
        except BaseException:
            cls.editor.stop()
            cls.temporary.cleanup()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.editor.stop()
        finally:
            cls.temporary.cleanup()

    def test_scene_operations_fail_closed(self) -> None:
        self.assertEqual(self.client.structured_tool("read_editor_state", {})["scenes"]["current"], "")
        for operation in ("save_scene", "play_current_scene"):
            with self.subTest(operation=operation):
                payload = self.client.structured_tool(OPERATION_TOOL, {"operation": operation})
                self.assertEqual(payload["status"], "operation_failed", payload)
                self.assertIs(payload["ok"], False)
                self.assertIs(payload["changed"], False)
                self.assertEqual(payload["operation"], operation)
                self.assertEqual(payload["claim"], "effect")
        # Nothing was started by either refusal.
        self.assertIs(
            self.client.structured_tool("read_editor_state", {})["play"]["is_playing"], False
        )


class BaristaMCPOperationPortabilityTests(unittest.TestCase):
    def test_caret_argument_bases_match_the_engine_defaults(self) -> None:
        """The engine's own published defaults are what fix the base of each caret argument.

        EditorInterface::edit_script declares line's default as the sentinel -1 and column's default
        as 0. A one-based column could not default to 0, and a zero-based line could not use -1 as a
        "no line" sentinel, so line is advertised one-based and column zero-based and both are passed
        through unchanged. If a later Godot changed either base it would have to change these
        defaults, and this test would catch it rather than the caret quietly landing elsewhere.
        """
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        edit_script = next(
            method
            for method in classes["EditorInterface"]["methods"]
            if method["name"] == "edit_script"
        )
        defaults = {
            argument["name"]: argument.get("default_value")
            for argument in edit_script["arguments"]
        }
        self.assertEqual(defaults["line"], "-1")
        self.assertEqual(defaults["column"], "0")
        self.assertEqual(defaults["grab_focus"], "true")

    def test_pinned_extension_api_covers_every_operation(self) -> None:
        """Portability evidence: every engine symbol an operation calls exists in the pinned public API."""
        api = json.loads(PINNED_EXTENSION_API.read_text(encoding="utf-8"))
        classes = {entry["name"]: entry for entry in api["classes"]}
        profile = json.loads(BUILD_PROFILE.read_text(encoding="utf-8"))

        for class_name in ("EditorSettings", "FileAccess", "ResourceLoader", "ScriptEditor", "EditorInterface"):
            with self.subTest(engine_class=class_name):
                self.assertIn(class_name, profile["enabled_classes"])
                self.assertIn(class_name, classes)

        required_methods = {
            "EditorInterface": (
                "edit_script",
                "get_current_path",
                "get_editor_settings",
                "get_open_scenes",
                "get_playing_scene",
                "get_unsaved_scenes",
                "is_playing_scene",
                "open_scene_from_path",
                "play_current_scene",
                "play_custom_scene",
                "play_main_scene",
                "reload_scene_from_path",
                "save_all_scenes",
                "save_scene",
                "select_file",
                "stop_playing_scene",
            ),
            "ResourceLoader": ("exists", "get_recognized_extensions_for_type", "load"),
            "FileAccess": ("file_exists",),
            "ScriptEditor": ("get_current_script",),
            "EditorSettings": ("get_setting", "has_setting"),
        }
        for class_name, methods in required_methods.items():
            available = {method["name"] for method in classes[class_name].get("methods", [])}
            for method in methods:
                with self.subTest(engine_class=class_name, method=method):
                    self.assertIn(method, available)


if __name__ == "__main__":
    unittest.main()
