# Stock Godot Editor Automation Design

## Summary

BaristaMCP will provide semantic automation of the stock Godot 4.7 editor from a GDExtension. The extension will expose editor state, visible UI, selectors, common UI actions, cooperative waits, explicit editor operations, screenshots, and bounded diagnostics over its existing authenticated loopback MCP transport. A later Python stdio bridge will launch or connect to a normal Godot process and proxy the in-editor MCP surface to agent clients.

The implementation will reuse proven, portable algorithms and structures from Cafecito Games' existing editor-automation implementation while replacing every engine-private dependency with public Godot APIs. BaristaMCP's product API, runtime types, naming, and architecture remain standalone and contain no dependency on another engine or project.

## Goals

- Port as much proven editor-automation functionality as stock Godot 4.7 and `godot-cpp` can support.
- Preserve the existing secure, loopback-only MCP transport and strict protocol validation.
- Give agents a reliable semantic loop: inspect, select, act, wait, and verify.
- Prefer stable public editor APIs over traversal of undocumented editor implementation details.
- Deliver the work as independently useful vertical slices.
- Package the approved specification and implementation plan as one GitHub epic with native sub-issues.
- Keep all implementation compatible with an official Godot 4.7 executable.

## Non-goals

- Modifying Godot core, relying on a custom engine build, or including engine-private headers.
- Reproducing fork-specific workspaces, boards, scene panes, private canvas zoom, or related state.
- Maintaining API or behavioral compatibility with any other automation implementation.
- Exposing shell execution, arbitrary filesystem access, arbitrary object method calls, or arbitrary signal emission.
- Claiming access to the complete Godot editor log when no public extension API provides it.
- Enumerating or invoking existing command-palette commands through private editor registries.
- Adding SSE, remote binding, or unauthenticated access.

## Portability boundary

Every production dependency must be present in the public Godot 4.7 GDExtension API and available through the pinned `godot-cpp` bindings. The implementation must not use runtime reflection to call undocumented editor methods. Public `Object`, `Node`, `Control`, and editor-class methods may be used normally, including public methods reached through a typed `godot-cpp` wrapper.

| Capability | Treatment |
| --- | --- |
| MCP lifecycle, HTTP transport, authentication, and tools | Extend the existing BaristaMCP implementation. |
| Resources and resource templates | Add as first-class MCP methods. |
| Semantic UI snapshots | Traverse the public editor `Control` and `Window` hierarchy starting from `EditorInterface::get_base_control()`. |
| Stable selectors and paginated subtrees | Implement entirely in extension-owned code. |
| Common-control actions | Use documented control methods and public input-event delivery. |
| Editor and scene state | Read through `EditorInterface`, `EditorSelection`, `ScriptEditor`, `EditorFileSystem`, and scene APIs. |
| Cooperative waits | Own wait handles in the extension and advance them during editor process frames. |
| Scene tree, list, menu, and tab items | Expose only the enumeration, state, and actions supported by public APIs. |
| Screenshots | Use public `DisplayServer`, `Viewport`, `Texture2D`, and `Image` APIs after platform probes pass. |
| Events | Maintain a Barista-owned bounded event ring for lifecycle, actions, waits, and observable state changes. |
| Agent stdio bridge | Implement as a separate Python process that launches or connects to Godot. |
| Full editor log history | Exclude unless Godot adds an appropriate public API. |
| Generic command enumeration and execution | Exclude; use semantic UI actions and explicit `EditorInterface` operations instead. |
| Private modal ownership | Approximate only when public window state is sufficient; otherwise report the capability as unavailable. |
| Fork-specific editor features | Exclude entirely. |

Uncertain capabilities must pass a focused acceptance probe against an official Godot 4.7 build before BaristaMCP publishes a stable contract for them. The initial probes cover editor-tree traversal, input delivery, common-control behavior, embedded and native subwindows, screenshot capture, popup interaction, tree items, and filesystem scan state.

## Reuse strategy

The existing implementation is a donor of working code, not merely an informal behavioral reference. Porting will happen by dependency class so portable logic is retained and engine-bound code is adapted deliberately.

### Mechanically portable

- JSON Schema construction and typed boundary parsers
- MCP result and structured-error formatting
- Selector matching, disambiguation, pagination, and stale-handle semantics
- Automation action/result value types
- Event markers, bounded event storage, and action traces
- Resource routing and serialization patterns
- Python HTTP client, editor session, and stdio proxy structure

### Portable with public-API adapters

- UI snapshot capture and role/name/text extraction
- Control and virtual-item actions
- Editor and scene state capture
- Wait conditions and idle-state polling
- Screenshot acquisition and cropping
- Failure-context assembly

### Rewritten or omitted

- Engine startup and internal editor-server ownership
- `EditorNode`, private dock, and private workspace access
- Internal editor-log access
- Private command-palette and shortcut registries
- Fork-specific panes, boards, scene tabs, and canvas zoom
- Native engine test-driver and internal C++ test framework integration

Useful donor areas are the editor automation contracts, dispatcher, snapshot, selector, action, input, state, wait, event, screenshot, and Python MCP client/session/stdio modules. Implementation sub-issues will identify exact donor files and the private dependencies that must be removed. Required source provenance remains in legal notices and source license headers; it does not influence BaristaMCP runtime identifiers or public contracts.

## Architecture

The extension will use these responsibility boundaries:

```text
MCPServer
  -> MCPDispatcher
       -> MCPContracts
       -> EditorAutomationService
            -> EditorSnapshot
            -> EditorSelector
            -> EditorActionDriver
            -> EditorStateReader
            -> EditorWaitManager
            -> EditorEventLog
            -> EditorScreenshot
```

`MCPServer` continues to own TCP/HTTP framing, authentication, request and response limits, and connection lifecycle. It knows nothing about editor workflows.

`MCPDispatcher` remains socket-free. It validates JSON-RPC and MCP lifecycle state, routes tools and resources, and converts service results into MCP results. It does not traverse or mutate editor UI directly.

`MCPContracts` becomes the single source of truth for tool schemas, output schemas, resource definitions, enums, and typed argument validation. Schemas and parsers must reject unknown fields and evolve together.

`EditorAutomationService` owns the editor-facing components and is configured with the plugin's public `EditorInterface`. The plugin advances the service and the server once per `_process()` frame and cancels outstanding operations during shutdown.

The Python bridge is outside the extension. It runs as a standard-library stdio MCP server, starts `godot --editor --path <project>` when requested, reads the `BARISTA_MCP` discovery record, initializes the loopback HTTP session, proxies tools and resources, and reliably terminates only subprocesses it owns.

## Semantic UI model

Snapshot capture begins at `EditorInterface::get_base_control()` and includes visible public `Control` and `Window` descendants. It recognizes documented control types such as buttons, check boxes, labels, line and text editors, code editors, trees, item lists, option buttons, popup menus, spin boxes, sliders, scroll containers, tab bars, tab containers, dialogs, and subviewport containers.

Each element contains:

- A snapshot-scoped ID
- A durable opaque handle when safe public object identity exists
- Role, name, text, and Godot class
- A hierarchy path used for diagnostics, not as the preferred selector
- Visible, enabled, focused, pressed, and selected state when publicly readable
- Bounds in the element's owning window
- Advertised actions supported for that specific element
- Bounded metadata and children

Names prefer public accessibility names, visible text, window titles, and tooltip text before falling back to node names. Internal children of normal controls are hidden by default. Internal window children needed to operate public dialogs may be included when the public node traversal API exposes them. An `include_internal` option is explicit, and elements visible only through that option are marked internal.

Selectors can match by ID, handle, role, name, text, class, visible/enabled/focused/selected state, ancestry, and containment. They may return zero, one, or many elements. Mutation requires exactly one match and never chooses arbitrarily. Large trees and result sets are paginated with opaque cursors tied to the snapshot options that created them.

Virtual elements represent public item models that are not separate nodes: tree rows, list items, popup-menu items, and tabs. Each virtual type advertises only actions supportable through public APIs. Missing row bounds or unavailable activation methods narrow the advertised actions instead of triggering undocumented calls.

## MCP surface and delivery phases

### Phase 0: Portability gates

Add focused real-editor probes for the uncertain public API paths. Expand the `godot-cpp` build profile only with classes required by a passing feature. A failed probe either removes the feature from the planned public surface or narrows its contract explicitly.

### Phase 1: Semantic inspection

Add these read-only tools:

- `inspect_editor_ui`: return a paginated semantic UI tree.
- `find_editor_ui`: resolve a semantic selector to bounded element summaries.
- `read_editor_state`: return project, scene, selection, script, play, filesystem selection, and unsaved-scene state available through public APIs.

Add `resources/list`, `resources/templates/list`, and `resources/read` with these initial resources:

- `barista://ui/tree`
- `barista://editor/state`
- `barista://scene/active`
- `barista://scene/tree`
- `barista://ui/element/{handle}`
- `barista://ui/subtree/{handle}`

### Phase 2: Core interaction loop

Add these tools behind explicit automation opt-in:

- `act_on_editor_ui`: perform an advertised action on exactly one selected element.
- `wait_for_editor`: start, poll, or cancel a bounded cooperative wait.

The initial action set covers focus, click or activate, replace or type text, submit, select, expand or collapse, set or adjust numeric values, scroll, and select tabs when those operations are reliable through public APIs.

Waits cover bounded frame delay, selector appearance/disappearance/state changes, focus changes, modal stability observable through public windows, play-state changes, and filesystem scan completion. An action may request a post-action wait but still uses the same cooperative handle machinery.

### Phase 3: Reliable editor operations

Add `run_editor_action`, whose operation field is a strict enum backed by documented `EditorInterface` methods:

- Save the current scene or all scenes
- Open or reload a `res://` scene
- Play the current, main, or specified scene
- Stop play
- Select a `res://` filesystem path
- Switch the main editor screen
- Open a `res://` script at a line and column

These explicit operations replace unavailable generic command-registry execution. Each operation validates its arguments and reports completion or observable pending state without accepting arbitrary methods.

### Phase 4: Rich interaction and diagnostics

After their individual probes pass, add:

- Virtual tree, list, menu, and tab items
- Higher-fidelity keyboard, pointer, drag, and popup interaction
- `capture_editor_screenshot`
- `poll_barista_events`
- Bounded action traces and failure context
- Optional bounded screenshot attachments for failed UI actions
- Additional selector, focus, modal, play, and filesystem waits

The event stream contains only Barista-owned operations and state transitions that Barista can observe through public APIs. It never claims to mirror the internal editor log.

### Phase 5: Agent bridge

Add a Python stdio server that can:

- Launch an official stock Godot editor with Barista automation explicitly enabled
- Connect to an existing Barista endpoint and token
- Initialize, report status, and disconnect
- Proxy every editor-side tool and resource
- Own and clean up launched editor subprocesses

The data path is:

```text
MCP client -> Python stdio bridge -> authenticated loopback HTTP -> BaristaMCP GDExtension -> public Godot APIs
```

The HTTP endpoint remains independently usable. The bridge adds process management and client convenience, not a second automation engine.

## Request and state flow

For an inspection call, the server authenticates and parses the request, the dispatcher validates typed arguments, and the service captures a fresh snapshot with the requested visibility options. The selector and pagination layers operate only on that snapshot and return its generation with every result.

For an action call, the service captures a snapshot, resolves exactly one element, confirms that the element advertises the requested action, performs it through a documented public route, and records a bounded event and trace. If the action requests settling, the result includes a wait handle advanced on later plugin frames.

For a wait call, the manager evaluates the condition immediately. If incomplete, it stores a deadline, condition, and relevant baseline state under an opaque ID. `_process()` reevaluates active waits without blocking. Polling returns pending, complete, failed, timed out, or cancelled state. MCP or plugin shutdown cancels all handles.

Resources use the same snapshot and state readers as tools so read paths cannot drift into inconsistent representations.

## Activation and security

The existing read-only MCP server retains its project-level server setting. Mutating automation requires separate explicit opt-in through either:

- `barista_mcp/automation/enabled`, default `false`; or
- A documented per-launch user argument intended for the bridge and test harness.

The bridge supplies the per-launch argument only when asked to launch an automation-enabled editor. Mutating tools are omitted from `tools/list` when automation is disabled, and direct calls to their names return a stable disabled-capability failure.

The following protections remain mandatory:

- IPv4 loopback binding only
- Fresh cryptographically random bearer token per editor session
- Local-origin validation
- Strict HTTP framing and JSON-RPC validation
- Bounded request, response, tree, result, event, trace, wait, and image sizes
- Incremental non-blocking response writes
- No bearer token in ordinary status results, errors, or committed fixtures
- No shell execution or arbitrary filesystem paths
- Normalized `res://` validation for file-oriented operations
- No client-provided raw object IDs, arbitrary method calls, or arbitrary signal emission

## Error model

Malformed JSON-RPC or MCP envelopes produce JSON-RPC errors. A syntactically valid tool call that cannot complete returns a normal MCP tool result with `isError: true` and structured content.

Stable tool-level error codes include:

- `automation_disabled`
- `invalid_arguments`
- `unsupported_capability`
- `invalid_selector`
- `no_match`
- `ambiguous_selector`
- `stale_handle`
- `unsupported_action`
- `element_not_interactable`
- `invalid_resource_path`
- `wait_not_found`
- `wait_timeout`
- `wait_cancelled`
- `capture_unavailable`
- `payload_too_large`

Failures include a concise message, snapshot generation when applicable, and bounded relevant details. Selector failures include bounded candidate summaries. Action failures may include the focused element, visible modal summaries, action trace, recent Barista events, and an optional screenshot when enabled and within its byte limit.

## Testing strategy

Verification centers on an official Godot 4.7 executable. Build success is necessary but cannot establish editor-automation behavior.

The current Python acceptance suite will be split into shared process/client support plus focused test modules for:

- Transport, authentication, lifecycle, and malformed input
- Typed contracts and resources
- Semantic snapshots and selectors
- Automation opt-in and common-control actions
- Cooperative waits, cancellation, and events
- Editor state and explicit editor operations
- Virtual items and synthesized input
- Screenshot encoding, cropping, limits, and platform capability
- Python bridge protocol, proxying, launch, and subprocess cleanup

A test-only editor fixture plugin will add deterministic buttons, text fields, text areas, ranges, trees, lists, menus, tabs, dialogs, and multiple windows to the runnable test project. Acceptance tests will also exercise real stock editor workflows: opening a scene, selecting a node, editing through visible editor UI where supported, saving, starting play, observing state, and stopping play.

Every externally visible feature requires:

- Success and rejection coverage
- Stale-state and ambiguous-selector coverage where relevant
- Automation-disabled and automation-enabled coverage for mutation
- Bounded payload and timeout coverage
- Editor termination in `finally` blocks
- No leaked processes, connections, waits, or tokens
- A documented unavailable result when a platform cannot support the capability

Screenshot tests separate deterministic PNG encoding and crop validation from platform GUI capture. Headless CI does not fail merely because a display capture backend is unavailable; it must still verify the stable `capture_unavailable` result.

The authoritative verification sequence remains a debug GDExtension build followed by Python `unittest` discovery using an official Godot 4.7 executable. Each GitHub sub-issue will narrow that command to the exact new tests while retaining a final full-suite gate.

## GitHub epic decomposition

The approved implementation plan will become one epic, **Stock Godot editor automation through BaristaMCP**, with these native sub-issues:

1. Typed contracts, resources, and portability probes
2. Semantic UI snapshot model
3. Selector engine, handles, and pagination
4. Automation gate and common-control actions
5. Cooperative waits, events, and diagnostics
6. Public editor state and explicit editor operations
7. Virtual items and higher-fidelity input
8. Screenshot capture and failure attachments
9. Python stdio launcher and agent bridge

Every sub-issue contains exact scope and exclusions, allowed public Godot APIs, reusable donor files, expected BaristaMCP files, dependency links, acceptance tests, verification commands, and an independent definition of done. The implementation plan maps its task groups to these issue boundaries so each sub-issue can be implemented and reviewed as a focused pull request.

The epic owns cross-cutting architecture, security rules, the portability matrix, phase ordering, and the overall completion checklist. A capability rejected by a portability probe is closed with the evidence recorded in the relevant sub-issue and reflected in the epic matrix; it is not silently replaced with a private-API workaround.

## Completion criteria

The design is complete when all accepted public-API capabilities are implemented, documented, and covered by real-editor acceptance tests; rejected capabilities have recorded probe evidence; mutating tools require explicit opt-in; the Python bridge can launch and clean up an official Godot 4.7 editor; and the full acceptance suite passes without requiring a custom Godot build.
