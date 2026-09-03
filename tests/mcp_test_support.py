# mcp_test_support.py
#
# Copyright (c) 2026-present Cafecito Games LLC.
# This file is part of BaristaMCP, a Godot GDExtension.
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import http.client
import os
import queue
import re
import socket
import subprocess
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
PROJECT_DIR = ROOT / "project"
DISCOVERY_PREFIX = "BARISTA_MCP "


class EditorProcess:
    def __init__(
        self, project_dir: Path = PROJECT_DIR, extra_args: tuple[str, ...] = ()
    ) -> None:
        self.project_dir = project_dir
        self.extra_args = extra_args
        self.lines: queue.Queue[str] = queue.Queue()
        self.output: list[str] = []
        self.process: subprocess.Popen[str] | None = None
        self.reader: threading.Thread | None = None

    def start(self) -> None:
        godot_bin = os.environ.get("GODOT_BIN", "godot")
        version = subprocess.run(
            [godot_bin, "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
        if re.match(r"^4\.7(?:\.|$)", version) is None:
            raise AssertionError(f"GODOT_BIN must be Godot 4.7, got {version!r}")

        self.process = subprocess.Popen(
            [
                godot_bin,
                "--headless",
                "--editor",
                "--path",
                str(self.project_dir),
                *self.extra_args,
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()

    def _read_output(self) -> None:
        assert self.process is not None
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.output.append(line)
            self.lines.put(line)

    def wait_for_discovery(self, timeout: float = 8.0) -> dict[str, object]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.process is not None
            if self.process.poll() is not None:
                break
            try:
                line = self.lines.get(timeout=min(0.25, deadline - time.monotonic()))
            except queue.Empty:
                continue
            marker = line.find(DISCOVERY_PREFIX)
            if marker >= 0:
                return json.loads(line[marker + len(DISCOVERY_PREFIX) :])
        joined = "".join(self.output)
        raise AssertionError(f"Timed out waiting for {DISCOVERY_PREFIX!r}.\nEditor output:\n{joined}")

    def wait_for_output(self, expected: str, timeout: float = 8.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.process is not None
            if expected in "".join(self.output):
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.05)
        joined = "".join(self.output)
        raise AssertionError(f"Timed out waiting for {expected!r}.\nEditor output:\n{joined}")

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.reader is not None:
            self.reader.join(timeout=2)
        if self.process.stdout is not None:
            self.process.stdout.close()


class MCPClient:
    def __init__(self, discovery: dict[str, object]) -> None:
        endpoint = urlsplit(str(discovery["endpoint"]))
        self.host = endpoint.hostname or "127.0.0.1"
        self.port = endpoint.port or 80
        self.path = endpoint.path
        self.token = str(discovery["token"])
        self.next_id = 1

    def raw_request(
        self,
        method: str,
        path: str,
        body: str,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, str]:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        try:
            connection.request(
                method,
                path,
                body=body,
                headers=headers or {},
            )
            response = connection.getresponse()
            return response.status, response.read().decode("utf-8")
        finally:
            connection.close()

    def request(self, payload: object) -> tuple[int, str]:
        return self.raw_request(
            "POST",
            self.path,
            json.dumps(payload, separators=(",", ":")),
            {
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
                "Accept": "application/json",
            },
        )

    def rpc(self, method: str, params: object) -> dict[str, object]:
        request_id = self.next_id
        self.next_id += 1
        status, body = self.request(
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
        )
        if status != 200:
            raise AssertionError(f"Expected HTTP 200, received {status}: {body}")
        response = json.loads(body)
        if response.get("id") != request_id:
            raise AssertionError(f"Mismatched JSON-RPC id: {response!r}")
        return response

    def initialize(self) -> dict[str, object]:
        response = self.rpc(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "barista-test", "version": "0.1"},
            },
        )
        if "error" in response:
            raise AssertionError(f"JSON-RPC initialize failure: {response!r}")
        status, body = self.request(
            {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}
        )
        if (status, body) != (202, ""):
            raise AssertionError(f"Unexpected initialized notification response: {status} {body!r}")
        return response["result"]

    def structured_tool(self, name: str, arguments: dict[str, object]) -> dict[str, object]:
        response = self.rpc("tools/call", {"name": name, "arguments": arguments})
        if "error" in response:
            raise AssertionError(f"JSON-RPC tool failure: {response!r}")
        result = response["result"]
        structured = result.get("structuredContent")
        if not isinstance(structured, dict):
            raise AssertionError(f"Missing structuredContent: {result!r}")
        return structured

    def raw_socket_request(
        self, request: bytes, *, shutdown_write: bool = True
    ) -> tuple[int, str]:
        with socket.create_connection((self.host, self.port), timeout=3) as connection:
            connection.sendall(request)
            if shutdown_write:
                connection.shutdown(socket.SHUT_WR)
            chunks: list[bytes] = []
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        response = b"".join(chunks).decode("utf-8")
        head, _, body = response.partition("\r\n\r\n")
        return int(head.split(" ", 2)[1]), body


def write_test_project(project_dir: Path, settings: str = "", *, config_name: str = "BaristaMCP Temporary Test") -> None:
    (project_dir / "addons").symlink_to(PROJECT_DIR / "addons", target_is_directory=True)
    (project_dir / "bin").symlink_to(PROJECT_DIR / "bin", target_is_directory=True)
    (project_dir / "project.godot").write_text(
        f"""config_version=5

[application]
config/name="{config_name}"
config/features=PackedStringArray("4.7")

{settings}

[editor_plugins]
enabled=PackedStringArray("res://addons/barista_mcp/plugin.cfg")
""",
        encoding="utf-8",
    )
