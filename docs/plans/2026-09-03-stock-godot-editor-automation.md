# Stock Godot Editor Automation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the largest reliable semantic editor-automation surface that a stock Godot 4.7 GDExtension can support, plus an optional stdio launcher and proxy for agent clients.

**Architecture:** Keep HTTP and JSON-RPC concerns in the existing server and dispatcher, and introduce an extension-owned automation service composed of snapshot, selector, action, state, wait/event, and screenshot units. Gate all mutation separately from the read-only server, validate every contract at the MCP boundary, and add capabilities only after they pass real-editor tests with an official Godot 4.7 executable.

**Tech Stack:** C++17, `godot-cpp` pinned to the Godot 4.7 API, GDExtension, MCP/JSON-RPC over authenticated loopback HTTP, Python 3.8+ standard library, `unittest`, SCons.

**Design:** `docs/designs/2026-09-03-stock-godot-editor-automation-design.md`

---

## File map

- `src/mcp_schema.{h,cpp}`: reusable JSON Schema construction.
- `src/mcp_contracts.{h,cpp}`: Barista tool/resource catalog and typed boundary parsers.
- `src/mcp_dispatcher.{h,cpp}`: MCP lifecycle and socket-free tool/resource routing.
- `src/editor_automation_types.h`: shared element, selector, action, wait, event, and failure value types.
- `src/editor_automation_service.{h,cpp}`: editor-facing façade and per-frame coordination.
- `src/editor_snapshot.{h,cpp}`: public-API UI capture and serialization.
- `src/editor_selector.{h,cpp}`: selector matching, reconciliation, and pagination.
- `src/editor_action_driver.{h,cpp}`: advertised common-control actions.
- `src/editor_wait_manager.{h,cpp}`: cooperative wait lifecycle and polling.
- `src/editor_event_log.{h,cpp}`: bounded Barista event ring and action traces.
- `src/editor_state_reader.{h,cpp}`: public editor/scene state and explicit editor operations.
- `src/editor_screenshot.{h,cpp}`: bounded capture, crop, and PNG encoding.
- `src/editor_tool_provider.{h,cpp}`: existing status/project tools; delegates automation tools to the service.
- `src/barista_mcp_plugin.{h,cpp}`: project settings, launch opt-in, service ownership, and frame pumping.
- `src/mcp_server.{h,cpp}`: transport configuration only; injects the automation service into the dispatcher.
- `build_profile.json`: minimum public Godot classes required by accepted capabilities.
- `project/addons/barista_mcp_test_fixture/`: deterministic editor controls used only by acceptance tests.
- `tests/mcp_test_support.py`: shared editor-process and HTTP MCP client helpers.
- `tests/test_mcp_*.py`: focused real-editor acceptance modules.
- `scripts/barista_mcp/`: standard-library HTTP client, session, and stdio implementation.
- `scripts/barista_mcp_server.py`: executable stdio entry point.
- `README.md`: activation, tools, resources, bridge setup, and limitations.

Each numbered task below is one native GitHub sub-issue and should land as one focused pull request.

## Task 1: Typed contracts, resources, and portability probes

**Depends on:** None

**Reference implementation:** `editor/automation/editor_automation_mcp_schemas.*`, `editor_automation_mcp_contracts.*`, and the resource portions of `editor_automation_mcp_dispatcher.*` in the donor repository. Retain portable schema/parser logic; remove engine macros and editor-core types.

**Files:**

- Create: `src/mcp_schema.h`
- Create: `src/mcp_schema.cpp`
- Modify: `src/mcp_contracts.h`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/mcp_dispatcher.h`
- Modify: `src/mcp_dispatcher.cpp`
- Modify: `src/editor_tool_provider.h`
- Modify: `src/editor_tool_provider.cpp`
- Create: `tests/mcp_test_support.py`
- Create: `tests/test_mcp_resources.py`
- Modify: `tests/test_mcp_acceptance.py`

- [ ] **Step 1: Extract the shared acceptance helpers without changing behavior**

Move `EditorProcess`, `MCPClient`, `ROOT`, `PROJECT_DIR`, and `DISCOVERY_PREFIX` from `tests/test_mcp_acceptance.py` into `tests/mcp_test_support.py`. Add this checked tool helper for all later modules:

```python
def structured_tool(self, name: str, arguments: dict[str, object]) -> dict[str, object]:
    response = self.rpc("tools/call", {"name": name, "arguments": arguments})
    if "error" in response:
        raise AssertionError(f"JSON-RPC tool failure: {response!r}")
    result = response["result"]
    structured = result.get("structuredContent")
    if not isinstance(structured, dict):
        raise AssertionError(f"Missing structuredContent: {result!r}")
    return structured
```

Import the helpers back with:

```python
from mcp_test_support import MCPClient, EditorProcess, PROJECT_DIR
```

Run:

```bash
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_acceptance -v
```

Expected: the existing suite passes unchanged.

- [ ] **Step 2: Write failing resource and schema tests**

Create `tests/test_mcp_resources.py` with real-editor assertions for `resources/list`, `resources/templates/list`, `resources/read`, unknown URIs, non-object params, and output schemas:

```python
def test_project_resources_are_listed_and_read(self) -> None:
    listed = self.client.rpc("resources/list", {})["result"]["resources"]
    self.assertIn("barista://project/info", [entry["uri"] for entry in listed])
    read = self.client.rpc("resources/read", {"uri": "barista://project/info"})
    payload = json.loads(read["result"]["contents"][0]["text"])
    self.assertEqual(payload["project_name"], "BaristaMCP Test Project")

def test_unknown_resource_is_rejected(self) -> None:
    response = self.client.rpc("resources/read", {"uri": "barista://missing"})
    self.assertEqual(response["error"]["code"], -32602)
```

Run the module and expect failures with method-not-found errors.

- [ ] **Step 3: Port the portable schema builder and typed resource contracts**

Define the extension-native API in `mcp_schema.h`:

```cpp
class MCPSchema {
public:
	enum Type { OBJECT, ARRAY, STRING, INTEGER, NUMBER, BOOLEAN };
	static Dictionary object(const String &p_description = String());
	static Dictionary array(const Dictionary &p_items, const String &p_description = String());
	static Dictionary string(const String &p_description = String());
	static Dictionary integer(const String &p_description = String());
	static Dictionary number(const String &p_description = String());
	static Dictionary boolean(const String &p_description = String());
	static Dictionary enum_string(const PackedStringArray &p_values, const String &p_description = String());
	static void add_property(Dictionary &r_schema, const String &p_name, const Dictionary &p_property, bool p_required = false);
};
```

Add `build_resources_list()`, `build_resource_templates_list()`, and tool `outputSchema` fields to `MCPContracts`. Keep `additionalProperties: false` on all object inputs.

- [ ] **Step 4: Implement MCP resource routing over the existing project provider**

Advertise resource capabilities during initialization:

```cpp
Dictionary resources;
resources["subscribe"] = false;
resources["listChanged"] = false;
capabilities["resources"] = resources;
```

Route `resources/list`, `resources/templates/list`, and `resources/read`. `barista://project/info` must call the same provider method as `get_project_info`, serialize one `application/json` content entry, and reject unsupported URIs with `INVALID_PARAMS`.

- [ ] **Step 5: Run formatting, build, and focused tests**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_acceptance tests.test_mcp_resources -v
```

Expected: build succeeds and both modules pass.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat: add typed MCP resources"
```

## Task 2: Semantic UI snapshot model

**Depends on:** Task 1

**Reference implementation:** `editor_automation_types.h` and `editor_automation_snapshot.*`. Port generation, element serialization, role mapping, durable handles, and internal-child policy. Replace `EditorNode` roots and all private editor casts with `EditorInterface::get_base_control()` plus public nodes.

**Files:**

- Create: `src/editor_automation_types.h`
- Create: `src/editor_automation_service.h`
- Create: `src/editor_automation_service.cpp`
- Create: `src/editor_snapshot.h`
- Create: `src/editor_snapshot.cpp`
- Modify: `src/editor_tool_provider.h`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/mcp_server.h`
- Modify: `src/mcp_server.cpp`
- Modify: `src/barista_mcp_plugin.h`
- Modify: `src/barista_mcp_plugin.cpp`
- Modify: `build_profile.json`
- Create: `project/addons/barista_mcp_test_fixture/plugin.cfg`
- Create: `project/addons/barista_mcp_test_fixture/plugin.gd`
- Modify: `project/project.godot`
- Create: `tests/test_mcp_ui_snapshot.py`

- [ ] **Step 1: Add a deterministic editor fixture and failing snapshot tests**

The fixture plugin must add a named panel below the editor base control containing `Button`, `LineEdit`, `TextEdit`, `CheckBox`, `SpinBox`, `ItemList`, `Tree`, and `TabContainer`, then remove it in `_exit_tree()`. Enable both `barista_mcp` and `barista_mcp_test_fixture` in `project/project.godot`; temporary security-test projects continue enabling only `barista_mcp`.

Assert the future tool returns semantic records:

```python
ui = self.client.structured_tool("inspect_editor_ui", {"max_depth": 8})
self.assertGreater(ui["generation"], 0)
fixture = find_one(ui["tree"], role="control", name="Barista Test Fixture")
button = find_one([fixture], role="button", name="Brew")
self.assertIn("click", button["actions"])
self.assertEqual(len(button["bounds"]), 4)
```

Expected before implementation: `unknown_tool`.

- [ ] **Step 2: Define snapshot types and service ownership**

Define `EditorElement`, `EditorSnapshotData`, and `EditorSnapshotOptions` in `editor_automation_types.h`. Define this service boundary:

```cpp
class EditorAutomationService {
	EditorInterface *editor_interface = nullptr;
public:
	void configure(EditorInterface *p_editor_interface);
	Dictionary inspect_ui(const Dictionary &p_arguments) const;
	void process(double p_delta);
	void shutdown();
};
```

Inject one service instance from `BaristaMCPPlugin` through `MCPServer` to `EditorToolProvider`; do not create global singletons.

- [ ] **Step 3: Port public-control snapshot capture**

Implement `EditorSnapshot::capture(EditorInterface *, const EditorSnapshotOptions &)`. Walk public children from `get_base_control()`, include visible `Control` and `Window` nodes, and map documented types to roles. Element names use accessibility name, displayed text/title, tooltip, then node name. IDs use generation plus instance ID; handles never accept arbitrary client-provided object IDs.

Explicitly omit all donor branches containing `EditorNode`, `EditorProperty`, scene docks, inspector docks, workspace panes, boards, private scene tabs, or private item-layout methods.

- [ ] **Step 4: Add bounded tree serialization and the inspection contract**

`inspect_editor_ui` accepts only:

```json
{
  "max_depth": 8,
  "max_elements": 1000,
  "include_internal": false
}
```

Return `generation`, `focused_element_id`, `tree`, `element_count`, and `limits`. Clamp limits server-side and set truncation flags instead of exceeding the response cap.

- [ ] **Step 5: Expand the build profile and verify stock compilation**

Add only the control classes used by `editor_snapshot.cpp` to `build_profile.json`. Initialize the pinned submodule and run:

```bash
git submodule update --init --recursive
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_ui_snapshot -v
```

Expected: the fixture controls appear with stable roles, state, actions, bounds, and bounded output.

- [ ] **Step 6: Commit**

```bash
git add build_profile.json project/addons/barista_mcp_test_fixture src tests
git commit -m "feat: expose semantic editor snapshots"
```

## Task 3: Selector engine, handles, pagination, and UI resources

**Depends on:** Task 2

**Reference implementation:** `editor_automation_selector.*` and selector/resource code in `editor_automation_mcp_dispatcher.*`. Retain pure selector algorithms and diagnostic shapes; use Barista contract names and public snapshot types.

**Files:**

- Create: `src/editor_selector.h`
- Create: `src/editor_selector.cpp`
- Create: `src/editor_state_reader.h`
- Create: `src/editor_state_reader.cpp`
- Modify: `src/editor_automation_types.h`
- Modify: `src/editor_automation_service.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/mcp_dispatcher.cpp`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `build_profile.json`
- Create: `tests/test_mcp_ui_selector.py`
- Create: `tests/test_mcp_editor_state.py`
- Modify: `tests/test_mcp_resources.py`

- [ ] **Step 1: Write failing selector and resource tests**

Cover exact role/name, text contains, class, boolean state, `within`, ambiguity, no match, stale snapshot IDs, durable handles, result limits, subtree cursors, and malformed selectors:

```python
result = self.client.structured_tool(
    "find_editor_ui",
    {"selector": {"role": "button", "name": "Brew"}, "limit": 20},
)
self.assertTrue(result["ok"])
self.assertEqual(result["match_count"], 1)

ambiguous = self.client.structured_tool(
    "find_editor_ui", {"selector": {"role": "button"}}
)
self.assertGreater(ambiguous["match_count"], 1)
```

- [ ] **Step 2: Port selector parsing and matching**

Implement one typed selector parser accepting `id`, `handle`, `role`, `name`, `text`, `text_contains`, `class`, `visible`, `enabled`, `focused`, `pressed`, `selected`, and recursive `within`. Reject empty selectors and unknown keys. Return `OK`, `NO_MATCH`, `AMBIGUOUS`, `STALE_ID`, or `INVALID_SELECTOR` without arbitrary selection.

- [ ] **Step 3: Add bounded pagination and handle reconciliation**

Encode cursors with generation, offset, limit, and snapshot option flags. Reject cursors whose generation/options do not match. Reconcile only handles previously issued by the service and still present in the newly captured snapshot.

- [ ] **Step 4: Add UI and scene resources**

Implement `EditorStateReader` with documented methods on `EditorInterface`, `EditorSelection`, `ScriptEditor`, `EditorFileSystem`, and scene nodes. Add `read_editor_state`, then implement `barista://ui/tree`, `barista://editor/state`, `barista://scene/active`, `barista://scene/tree`, `barista://ui/element/{handle}`, and `barista://ui/subtree/{handle}` using the same readers as tools. State output has stable `project`, `scenes`, `selection`, `script`, `filesystem`, and `play` sections. Percent-decode and validate template handles; never turn a URI segment into an unchecked object lookup.

- [ ] **Step 5: Verify**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_ui_selector tests.test_mcp_editor_state tests.test_mcp_resources -v
```

Expected: all selector states and resource routes pass.

- [ ] **Step 6: Commit**

```bash
git add build_profile.json src tests
git commit -m "feat: add semantic UI selectors"
```

## Task 4: Automation gate and common-control actions

**Depends on:** Task 3

**Reference implementation:** `editor_automation_action.*`, the public-control portions of `editor_automation_driver.*`, and `editor_automation_input.*`. Do not port private workspace routes or generic signal/method dispatch.

**Files:**

- Create: `src/editor_action_driver.h`
- Create: `src/editor_action_driver.cpp`
- Modify: `src/editor_automation_service.h`
- Modify: `src/editor_automation_service.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `src/barista_mcp_plugin.h`
- Modify: `src/barista_mcp_plugin.cpp`
- Modify: `build_profile.json`
- Modify: `project/addons/barista_mcp_test_fixture/plugin.gd`
- Create: `tests/test_mcp_ui_actions.py`

- [ ] **Step 1: Write failing opt-in and action tests**

Launch one editor normally and one with `-- --barista-mcp-automation`. Assert the normal session omits `act_on_editor_ui` and rejects direct calls with `automation_disabled`. In the enabled session, test click, focus, set text, type text, submit, checkbox state, numeric value, and invalid/ambiguous/stale selectors.

```python
result = self.client.structured_tool(
    "act_on_editor_ui",
    {
        "selector": {"role": "text_field", "name": "Order"},
        "action": "set_text",
        "value": "cortado",
    },
)
self.assertTrue(result["ok"])
self.assertEqual(result["route"], "control_method")
```

- [ ] **Step 2: Add the separate mutation gate**

Define `barista_mcp/automation/enabled` with default `false`. Enable mutation when that setting is true or `OS::get_singleton()->get_cmdline_user_args()` contains exactly `--barista-mcp-automation`. Freeze this mode at server startup and report it from `barista_status` without exposing the token.

- [ ] **Step 3: Port the common-control driver**

Implement explicit action dispatch for documented control types. Each route validates arguments, confirms visibility/enabled state, performs only the named operation, and reports `ok`, `route`, `changed`, and the new snapshot generation. Use direct public methods for focus/text/value/selection; use public input events for user-fidelity click and typing where the probe proves delivery.

- [ ] **Step 4: Enforce advertised-action and error rules**

The service must reject an action absent from the selected element's `actions`. Stable failures include `automation_disabled`, `invalid_arguments`, `no_match`, `ambiguous_selector`, `stale_handle`, `unsupported_action`, and `element_not_interactable`.

- [ ] **Step 5: Verify**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_ui_actions -v
```

Expected: disabled and enabled sessions behave distinctly and every action is observable through a fresh snapshot.

- [ ] **Step 6: Commit**

```bash
git add build_profile.json project/addons/barista_mcp_test_fixture src tests
git commit -m "feat: add gated editor UI actions"
```

## Task 5: Cooperative waits, events, and diagnostics

**Depends on:** Task 4

**Reference implementation:** `editor_automation_wait.*`, `editor_automation_events.*`, `editor_automation_trace.*`, and portable failure assembly in `editor_automation_diagnostics.*`. Replace engine message-loop pumping with plugin-frame polling.

**Files:**

- Create: `src/editor_wait_manager.h`
- Create: `src/editor_wait_manager.cpp`
- Create: `src/editor_event_log.h`
- Create: `src/editor_event_log.cpp`
- Modify: `src/editor_automation_service.h`
- Modify: `src/editor_automation_service.cpp`
- Modify: `src/editor_action_driver.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `src/barista_mcp_plugin.cpp`
- Create: `tests/test_mcp_waits_events.py`

- [ ] **Step 1: Write failing wait and event tests**

Cover immediate completion, pending/poll/complete, timeout, cancellation, unknown IDs, selector appears/disappears, focus changes, frame delay, action events, marker pagination, and shutdown cleanup.

```python
started = self.client.structured_tool(
    "wait_for_editor",
    {"condition": {"type": "selector_appears", "selector": {"name": "Ready"}}, "timeout_ms": 2000},
)
self.assertIn(started["status"], {"complete", "pending"})
if started["status"] == "pending":
    finished = self.client.structured_tool("wait_for_editor", {"wait_id": started["wait_id"]})
    self.assertIn(finished["status"], {"pending", "complete"})
```

- [ ] **Step 2: Implement bounded event storage**

Store monotonically indexed Barista events in a fixed-capacity ring. Events contain timestamp, type, operation, outcome, and bounded details. `poll_barista_events` accepts a marker and limit, returns `events`, `marker`, and `has_more`, and reports marker expiry explicitly.

- [ ] **Step 3: Implement cooperative wait handles**

`EditorWaitManager::process()` evaluates active conditions once per plugin frame. Support `frames_elapsed`, `selector_appears`, `selector_disappears`, `selector_state`, `focus_changed`, and `play_state`. Clamp timeouts and active-handle counts. Poll and cancel by opaque random wait ID.

- [ ] **Step 4: Integrate action tracing and shutdown**

Record begin/end events around actions and include a bounded trace in failures. `EditorAutomationService::shutdown()` cancels and clears every wait before `MCPServer::stop()` releases the dispatcher.

- [ ] **Step 5: Verify**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_waits_events -v
```

Expected: no request blocks the editor thread; waits advance only through process frames and clean up on shutdown.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat: add cooperative editor waits"
```

## Task 6: Explicit editor operations and expanded public state

**Depends on:** Task 5

**Reference implementation:** Public portions of `editor_automation_state.*`. Do not port internal editor-log, `EditorNode`, workspace, board, or private main-screen state.

**Files:**

- Modify: `src/editor_state_reader.h`
- Modify: `src/editor_state_reader.cpp`
- Modify: `src/editor_automation_service.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `build_profile.json`
- Modify: `tests/test_mcp_editor_state.py`
- Create: `tests/test_mcp_editor_operations.py`

- [ ] **Step 1: Extend the state tests for operation completion**

Add assertions for the state transitions used to verify editor operations: active scene changes, unsaved scenes clear after saving, selected filesystem path changes, current script and caret target update, and play state moves between stopped and running. Missing optional state must continue to serialize as empty values rather than disappear unpredictably.

- [ ] **Step 2: Extend the public state reader with operation result helpers**

Add comparison helpers that evaluate whether an asynchronous editor operation reached its requested observable state. Keep `read_editor_state` and `barista://editor/state` backed by the same versioned reader introduced in Task 3.

- [ ] **Step 3: Write failing editor-operation tests**

In a temporary project, test `save_scene`, `save_all_scenes`, `open_scene`, `reload_scene`, `play_current_scene`, `play_main_scene`, `play_scene`, `stop_play`, `select_file`, `switch_main_screen`, and `edit_script`. Cover automation disabled, non-`res://` paths, traversal attempts, missing resources, and invalid operation names.

- [ ] **Step 4: Implement strict public editor operations**

Define `run_editor_action` with an operation enum and operation-specific fields. Normalize paths, require `res://`, reject `..` traversal, verify resource type before `edit_script`, and call only the corresponding documented `EditorInterface` method. Return an observable completion state or a cooperative wait ID where Godot completes asynchronously.

- [ ] **Step 5: Verify**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_editor_state tests.test_mcp_editor_operations -v
```

Expected: all operations work in temporary projects without accessing paths outside `res://`.

- [ ] **Step 6: Commit**

```bash
git add build_profile.json src tests
git commit -m "feat: expose public editor operations"
```

## Task 7: Virtual items and higher-fidelity input

**Depends on:** Tasks 4 and 6

**Reference implementation:** Public portions of `editor_automation_snapshot.*`, `editor_automation_driver.*`, `editor_automation_input.*`, and workflow metadata helpers. Omit any behavior requiring private row geometry, dock types, command registries, or workspace routes.

**Files:**

- Modify: `src/editor_snapshot.cpp`
- Modify: `src/editor_action_driver.cpp`
- Modify: `src/editor_automation_types.h`
- Modify: `src/mcp_contracts.cpp`
- Modify: `build_profile.json`
- Modify: `project/addons/barista_mcp_test_fixture/plugin.gd`
- Create: `tests/test_mcp_virtual_items.py`
- Create: `tests/test_mcp_input_actions.py`

- [ ] **Step 1: Write failing virtual-element tests**

Cover tree rows, item-list rows, popup-menu items, option items, and tabs. Assert stable text/index metadata, selected/collapsed/disabled state, handles, and only the actions supported by stock public APIs.

- [ ] **Step 2: Port virtual enumeration one type at a time**

Use `Tree`/`TreeItem`, `ItemList`, `PopupMenu`, `OptionButton`, `TabBar`, and `TabContainer` public methods. Do not fabricate bounds when Godot does not expose them. A virtual element without reliable hit geometry must use a documented semantic method or omit pointer actions.

- [ ] **Step 3: Write failing input-fidelity tests**

Test key presses with modifiers, caret typing, pointer click, wheel scrolling, and drag only on fixture controls whose event counters prove the normal input path ran. Tests must distinguish direct semantic routes from `input_key`, `input_pointer`, and `input_drag` routes.

- [ ] **Step 4: Port public input delivery and gate each route**

Use public `Input::parse_input_event()`, `Input::flush_buffered_events()`, or `Viewport::push_input()` only where the portability probe proves delivery in stock Godot. Never call message queues or viewport internals. If drag or native-window coordinates fail on a supported platform, return `unsupported_capability` and omit that action from the element.

- [ ] **Step 5: Verify**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_virtual_items tests.test_mcp_input_actions -v
```

Expected: every advertised route is demonstrated by the fixture; unsupported routes are absent and return stable errors.

- [ ] **Step 6: Commit**

```bash
git add build_profile.json project/addons/barista_mcp_test_fixture src tests
git commit -m "feat: add rich editor UI interaction"
```

## Task 8: Screenshot capture and failure attachments

**Depends on:** Tasks 3 and 5

**Reference implementation:** `editor_automation_screenshot.*` and failure-attachment portions of contracts/diagnostics. Replace engine rendering access with public `DisplayServer` or `Viewport` image APIs.

**Files:**

- Create: `src/editor_screenshot.h`
- Create: `src/editor_screenshot.cpp`
- Modify: `src/editor_automation_service.cpp`
- Modify: `src/mcp_contracts.cpp`
- Modify: `src/editor_tool_provider.cpp`
- Modify: `build_profile.json`
- Create: `tests/test_mcp_screenshot.py`

- [ ] **Step 1: Write failing capture contract tests**

Cover full-window capture, element crop with padding, selector ambiguity, zero-size elements, invalid limits, response-cap enforcement, automation-disabled failure attachments, and deterministic `capture_unavailable` behavior under a dummy/headless display.

```python
capture = self.client.structured_tool(
    "capture_editor_screenshot", {"max_bytes": 524288}
)
if capture["ok"]:
    png = base64.b64decode(capture["image"]["data"])
    self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
else:
    self.assertEqual(capture["code"], "capture_unavailable")
```

- [ ] **Step 2: Implement one bounded capture pipeline**

Resolve the target window/viewport through public APIs, obtain an `Image`, crop to validated element bounds when requested, encode once with `save_png_to_buffer()`, and reject payloads above the caller and server limits. Return MIME type, base64 data, dimensions, capture mode, and crop metadata.

- [ ] **Step 3: Reuse the pipeline for optional failure attachments**

Add `attach_screenshot_on_failure` and `max_screenshot_bytes` to selector/action inputs. Capture only after a failure, never replace the primary failure code, and omit image data with a bounded attachment error when capture is unavailable or too large.

- [ ] **Step 4: Verify headless and GUI-capable paths**

```bash
clang-format -i src/*.h src/*.cpp
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_mcp_screenshot -v
```

Expected: deterministic encoding/crop tests pass; display-less execution returns `capture_unavailable` without crashing or hanging.

- [ ] **Step 5: Commit**

```bash
git add build_profile.json src tests
git commit -m "feat: capture bounded editor screenshots"
```

## Task 9: Python stdio launcher and agent bridge

**Depends on:** Tasks 1-8 for the complete proxy catalog; the generic proxy may begin after Task 1.

**Reference implementation:** `scripts/foundry_mcp/client.py`, `session.py`, `stdio_server.py`, and the top-level server entry point. Rename every product concept, regenerate Barista-native schemas, and preserve stdlib-only process cleanup and structured errors.

**Files:**

- Create: `scripts/__init__.py`
- Create: `scripts/barista_mcp/__init__.py`
- Create: `scripts/barista_mcp/client.py`
- Create: `scripts/barista_mcp/session.py`
- Create: `scripts/barista_mcp/stdio_server.py`
- Create: `scripts/barista_mcp_server.py`
- Create: `tests/test_barista_mcp_bridge.py`
- Create: `tests/test_barista_mcp_bridge_e2e.py`
- Modify: `README.md`

- [ ] **Step 1: Write failing client and bridge protocol tests**

Use a local fake HTTP server to test initialize, notification, tool calls, resources, bearer headers, JSON-RPC errors, timeouts, and malformed responses. Drive the stdio server with compact JSONL and assert `initialize`, `tools/list`, launch/connect/status/disconnect, generic tool proxy, and resource proxy behavior.

- [ ] **Step 2: Port the stdlib HTTP client**

Implement `BaristaMCPClient` with endpoint/token validation, monotonic request IDs, `initialize()`, `call_tool()`, `read_resource()`, and `close()`. Accept only loopback HTTP endpoints by default and preserve MCP structured errors rather than flattening them into strings.

- [ ] **Step 3: Port editor session ownership**

Implement `BaristaEditorSession.launch()` to execute:

```python
[godot_binary, "--editor", "--path", str(project), "--", "--barista-mcp-automation"]
```

Read combined output until one valid `BARISTA_MCP` record appears, validate its loopback endpoint, and initialize the client. `disconnect()` terminates only an owned process, escalating from terminate to kill after bounded waits; attached sessions only close HTTP state.

- [ ] **Step 4: Implement the stdio MCP façade**

Expose `barista_launch_editor`, `barista_connect`, `barista_disconnect`, `barista_status`, `barista_call_tool`, and `barista_read_resource`, plus stable pass-through wrappers for inspection, selectors, actions, waits, state, editor operations, events, and screenshots. Keep `barista_call_tool` as the forward-compatible path for future tools. Serialize one JSON-RPC object per stdout line and send diagnostics only to stderr.

- [ ] **Step 5: Add a real-editor end-to-end test and documentation**

Launch the official Godot 4.7 executable against the test project, call `inspect_editor_ui` through stdio, then disconnect and assert the process exits. Document Claude, Codex, and Cursor registration, launch/connect flows, explicit automation opt-in, tool discovery, and limitations.

- [ ] **Step 6: Verify the bridge and full repository**

```bash
python3 -m unittest tests.test_barista_mcp_bridge -v
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest tests.test_barista_mcp_bridge_e2e -v
scons target=template_debug arch=arm64
GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest discover -s tests -v
```

Expected: fake-server and real-editor tests pass, the launched process is reaped, and the complete acceptance suite remains green.

- [ ] **Step 7: Commit**

```bash
git add README.md scripts tests
git commit -m "feat: add BaristaMCP stdio bridge"
```

## Epic completion gate

- [ ] Every accepted capability compiles against the pinned stock Godot 4.7 `godot-cpp` API.
- [ ] Rejected portability probes are recorded in their native sub-issue with command output and the public API gap.
- [ ] Mutating tools are absent unless automation was explicitly enabled before server startup.
- [ ] All tool and resource inputs reject unknown fields and return stable structured failures.
- [ ] No implementation includes engine-private headers or invokes undocumented editor methods.
- [ ] `clang-format -i src/*.h src/*.cpp` produces no diff.
- [ ] `scons target=template_debug` succeeds.
- [ ] `GODOT_BIN=/path/to/official-godot-4.7 python3 -m unittest discover -s tests -v` passes.
- [ ] The stdio bridge launches, initializes, drives, and cleans up an official Godot 4.7 editor.
- [ ] README documents activation, security boundaries, tools, resources, bridge registration, and unsupported features.
