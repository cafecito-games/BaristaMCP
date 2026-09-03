#!/usr/bin/env python3
# add_license_header.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

"""Add the Cafecito Games copyright header to source files that lack one.

Usage:
    python3 scripts/add_license_header.py [paths...]   # add headers to paths
    python3 scripts/add_license_header.py --check      # report missing headers
    python3 scripts/add_license_header.py --hook       # read a Claude Code hook payload on stdin

With no paths, every tracked source file in the repository is processed.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

COPYRIGHT = "Copyright (c) 2026-present Cafecito Games LLC."
PROJECT = "This file is part of BaristaMCP, a Godot GDExtension."
SPDX = "SPDX-License-Identifier: MIT"
MARKER = "SPDX-License-Identifier"

BLOCK_WIDTH = 76

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

# Directories holding third-party code, build output, or generated sources.
EXCLUDED_DIRECTORIES = {
    ".git",
    ".godot",
    ".worktrees",
    "bin",
    "build",
    "cmake-build-debug",
    "gen",
    "godot-cpp",
    "__pycache__",
}

# Upstream godot-cpp derivatives that keep their original authorship.
EXCLUDED_FILES = {
    "methods.py",
}

C_STYLE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inl"}
HASH_STYLE_SUFFIXES = {".gd", ".py"}
HASH_STYLE_NAMES = {"SConstruct", "SCsub"}

HEADER_LINES = (COPYRIGHT, PROJECT, SPDX)


def c_style_header(filename: str) -> str:
    border = "/*" + "*" * (BLOCK_WIDTH - 4) + "*/"
    lines = [border]
    for text in (filename, "", *HEADER_LINES):
        lines.append("/*  " + text.ljust(BLOCK_WIDTH - 6) + "*/")
    lines.append(border)
    return "\n".join(lines) + "\n"


def hash_style_header(filename: str) -> str:
    lines = ["# " + filename, "#"]
    lines += ["# " + text for text in HEADER_LINES]
    return "\n".join(lines) + "\n"


def header_for(path: Path) -> str | None:
    """Return the header text for `path`, or None if the file type has none."""
    if path.suffix in C_STYLE_SUFFIXES:
        return c_style_header(path.name)
    if path.suffix in HASH_STYLE_SUFFIXES or path.name in HASH_STYLE_NAMES:
        return hash_style_header(path.name)
    return None


def is_excluded(path: Path) -> bool:
    try:
        relative = path.resolve().relative_to(REPOSITORY_ROOT)
    except ValueError:
        return True
    if relative.name in EXCLUDED_FILES:
        return True
    return any(part in EXCLUDED_DIRECTORIES for part in relative.parts)


def has_header(text: str) -> bool:
    """True when one of the leading comment lines already carries the SPDX tag."""
    comment_prefixes = ("#", "//", "/*", "*")
    for line in text.splitlines()[:20]:
        stripped = line.strip()
        if stripped.startswith(comment_prefixes) and MARKER in stripped:
            return True
    return False


def apply_header(path: Path) -> bool:
    """Insert the header into `path`. Returns True when the file was changed."""
    header = header_for(path)
    if header is None or is_excluded(path) or not path.is_file():
        return False

    text = path.read_text(encoding="utf-8")
    if has_header(text):
        return False

    prefix = ""
    body = text
    if body.startswith("#!"):
        shebang, _, remainder = body.partition("\n")
        prefix = shebang + "\n"
        body = remainder.lstrip("\n")

    separator = "\n" if body.strip() else ""
    path.write_text(prefix + header + separator + body, encoding="utf-8")
    return True


def tracked_source_files() -> list[Path]:
    listing = subprocess.run(
        ["git", "ls-files"],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    candidates = (REPOSITORY_ROOT / name for name in listing)
    return [path for path in candidates if header_for(path) and not is_excluded(path)]


def hook_paths() -> list[Path]:
    """Extract the edited file path from a Claude Code PostToolUse payload."""
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return []
    file_path = payload.get("tool_input", {}).get("file_path")
    return [Path(file_path)] if file_path else []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--check", action="store_true", help="report missing headers without editing")
    parser.add_argument("--hook", action="store_true", help="read a Claude Code hook payload on stdin")
    parser.add_argument(
        "--fail-on-change",
        action="store_true",
        help="exit non-zero when a header had to be added (pre-commit style)",
    )
    arguments = parser.parse_args()

    if arguments.hook:
        paths = hook_paths()
    elif arguments.paths:
        paths = arguments.paths
    else:
        paths = tracked_source_files()

    if arguments.check:
        missing = [
            path
            for path in paths
            if header_for(path)
            and not is_excluded(path)
            and path.is_file()
            and not has_header(path.read_text(encoding="utf-8"))
        ]
        for path in missing:
            print(f"missing copyright header: {path}")
        return 1 if missing else 0

    changed = [path for path in paths if apply_header(path)]
    for path in changed:
        print(f"added copyright header: {path}")
    return 1 if changed and arguments.fail_on_change else 0


if __name__ == "__main__":
    sys.exit(main())
