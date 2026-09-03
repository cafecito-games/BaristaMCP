# Repository Guidelines

## Project Structure & Module Organization

Native C++17 extension code lives in `src/`. Keep responsibilities separated: plugin lifecycle in `barista_mcp_plugin.*`, HTTP transport in `mcp_server.*`, JSON-RPC routing in `mcp_dispatcher.*`, protocol schemas in `mcp_contracts.*`, and editor-facing tools in `editor_tool_provider.*`. The `project/` directory is the runnable Godot test project; its addon bootstrap is under `project/addons/barista_mcp/` and the GDExtension manifest is under `project/bin/`. Acceptance tests live in `tests/test_mcp_acceptance.py`. Treat `godot-cpp/` as a pinned submodule, not vendored project code.

## Build, Test, and Development Commands

- `git submodule update --init --recursive` initializes the bindings.
- `scons target=template_debug` performs the authoritative development build and copies the library into `project/bin/<platform>/`.
- `scons target=template_debug arch=arm64` provides a faster Apple Silicon build.
- `cmake -S . -B cmake-build-debug -DGODOTCPP_TARGET=template_debug && cmake --build cmake-build-debug --parallel` creates an optional IDE-oriented build.
- `godot --editor --path project` opens the enabled test plugin.
- `GODOT_BIN=/path/to/godot python3 -m unittest discover -s tests -v` runs the real-editor acceptance suite. Use an official Godot 4.7 executable for compatibility verification.

## Coding Style & Naming Conventions

Follow `.editorconfig`: tabs and width 4 by default, four spaces for Python and SCons, two spaces for YAML. Format C++ with `clang-format -i src/*.h src/*.cpp`; the checked-in style uses tabs, attached braces, and a 120-column limit. Use `PascalCase` for C++ classes, `snake_case` for files and methods, `_leading_underscore` for private helpers, and `UPPER_SNAKE_CASE` for constants.

## Testing Guidelines

Add acceptance coverage for externally visible MCP behavior, transport security, configuration, and lifecycle changes. Name tests `test_<behavior>`. Build the debug extension before running tests. Exercise both success and rejection paths, and ensure editor processes shut down in `finally` blocks.

## Commit & Pull Request Guidelines

History follows concise Conventional Commit prefixes such as `feat:`, `fix:`, `docs:`, and `chore:`. Keep each commit focused. PRs should explain behavior and security implications, list exact verification commands, link relevant issues, and include screenshots only for visible editor UI changes.

## Security & Compatibility

Preserve stock Godot 4.7 compatibility. Adapt ideas from Foundry without depending on fork-only headers or private engine APIs. Keep the server loopback-only, never log or commit bearer tokens, validate untrusted protocol input, and do not commit generated binaries, `.godot/`, CMake output, or `docs/superpowers/`.
