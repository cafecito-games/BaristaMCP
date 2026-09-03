# BaristaMCP

BaristaMCP is a stock-compatible Godot 4.7 GDExtension that embeds a local [Model Context Protocol](https://modelcontextprotocol.io/) server in the editor. It provides a native foundation for agent-driven Godot tooling without requiring a custom engine build.

The first vertical slice supports MCP initialization, ping, tool discovery, JSON-RPC batches, and two read-only tools:

- `barista_status` reports the server, endpoint, and protocol state without exposing its bearer token.
- `get_project_info` reports the project name and path, Godot version, current edited scene, and play state.

## Requirements

- Godot 4.7
- Python 3.8 or newer
- SCons 4
- A C++17 compiler supported by `godot-cpp`

The repository pins `godot-cpp` v10 commit `05057de73de4b99f114d36c40d84ca46926c0e25` and explicitly builds against its Godot 4.7 API database.

## Clone and build

Clone recursively, or initialize the bindings after cloning:

```sh
git submodule update --init --recursive
```

Build a debug library for the host platform:

```sh
scons target=template_debug
```

On macOS, the default is a universal binary. For a faster local Apple Silicon build:

```sh
scons target=template_debug arch=arm64
```

Build a release library with:

```sh
scons target=template_release
```

SCons copies libraries into `project/bin/<platform>/`, where `project/bin/barista_mcp.gdextension` resolves them. To generate `compile_commands.json`, add `compiledb=yes` to a build.

CMake is also available for IDE integration:

```sh
cmake -S . -B cmake-build-debug -DGODOTCPP_TARGET=template_debug
cmake --build cmake-build-debug --parallel
```

SCons is the authoritative release and CI build.

## Run

Open the included project in Godot 4.7:

```sh
godot --editor --path project
```

The checked-in test project enables the plugin. In another project, copy `project/addons/barista_mcp/`, `project/bin/barista_mcp.gdextension`, and the matching native libraries, then enable **BaristaMCP** under **Project > Project Settings > Plugins**.

At startup the plugin binds an ephemeral IPv4 loopback port, generates a fresh 32-byte bearer token, and prints one discovery record:

```text
BARISTA_MCP {"endpoint":"http://127.0.0.1:32145/mcp","local_only":true,"token":"...","transport":"mcp"}
```

Every request must be sent to that endpoint with `Authorization: Bearer <token>`. For example:

```sh
curl -sS \
  -H 'Authorization: Bearer TOKEN_FROM_DISCOVERY' \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"example","version":"1.0"}}}' \
  http://127.0.0.1:32145/mcp
```

After `initialize`, send `notifications/initialized`, then use `tools/list` and `tools/call` according to MCP protocol version `2025-11-25`.

## Settings

The plugin defines these project settings:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `barista_mcp/server/enabled` | `true` | Start when the editor plugin loads. |
| `barista_mcp/server/port` | `0` | Use an ephemeral port; values 1–65535 request a fixed port. |
| `barista_mcp/server/request_timeout_ms` | `30000` | Maximum lifetime of a partial request or queued response write. |
| `barista_mcp/server/max_request_bytes` | `8388608` | Maximum complete HTTP request size. |

The listener is always restricted to `127.0.0.1`. There is no setting to expose it to a network interface. Responses are capped at 1 MiB and written incrementally so a client that stops reading cannot block the editor thread.

## Test

Build the debug extension first, then run the standard-library acceptance suite with a Godot 4.7 executable:

```sh
GODOT_BIN=/path/to/godot python3 -m unittest discover -s tests -v
```

The suite launches a real headless editor and verifies discovery, clean shutdown, configuration and bind failures, authentication, local-Origin enforcement, strict HTTP framing and timeouts, non-blocking response writes, MCP lifecycle and envelope validation, ping, JSON-RPC errors and batches, and both live editor tools.

## Current scope

BaristaMCP currently uses stateless HTTP request/response transport. It does not expose SSE, server-push notifications, remote binding, project mutation, shell execution, or Foundry-only editor internals.

The architecture is inspired by and selectively adapts portable patterns from Cafecito Games' Foundry Godot fork, especially its editor automation MCP transport and dispatcher. BaristaMCP does not include Foundry headers, link against Foundry, or use private Godot engine APIs. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution.
