# test_mcp_acceptance.py
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
import tempfile
import threading
import time
import unittest
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


class BaristaMCPAcceptanceTests(unittest.TestCase):
    @staticmethod
    def write_test_project(project_dir: Path, settings: str = "") -> None:
        (project_dir / "addons").symlink_to(
            PROJECT_DIR / "addons", target_is_directory=True
        )
        (project_dir / "bin").symlink_to(PROJECT_DIR / "bin", target_is_directory=True)
        (project_dir / "project.godot").write_text(
            f"""config_version=5

[application]
config/name="BaristaMCP Temporary Test"
config/features=PackedStringArray("4.7")

{settings}

[editor_plugins]
enabled=PackedStringArray("res://addons/barista_mcp/plugin.cfg")
""",
            encoding="utf-8",
        )

    def initialize_client(self, client: MCPClient) -> None:
        response = client.rpc(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "barista-test", "version": "0.1"},
            },
        )
        self.assertNotIn("error", response)
        status, body = client.request(
            {
                "jsonrpc": "2.0",
                "method": "notifications/initialized",
                "params": {},
            }
        )
        self.assertEqual((status, body), (202, ""))

    def test_discovery(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            discovery = editor.wait_for_discovery()
        finally:
            editor.stop()

        self.assertEqual(discovery["transport"], "mcp")
        self.assertIs(discovery["local_only"], True)
        self.assertRegex(str(discovery["endpoint"]), r"^http://127\.0\.0\.1:\d+/mcp$")
        self.assertGreater(len(str(discovery["token"])), 20)

    def test_initialize_and_ping(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            try:
                initialize = client.rpc(
                    "initialize",
                    {
                        "protocolVersion": "2025-11-25",
                        "capabilities": {},
                        "clientInfo": {"name": "barista-test", "version": "0.1"},
                    },
                )
                self.assertEqual(initialize["result"]["protocolVersion"], "2025-11-25")
                self.assertEqual(
                    initialize["result"]["serverInfo"],
                    {"name": "BaristaMCP", "version": "0.1.0"},
                )
                self.assertEqual(
                    initialize["result"]["capabilities"]["tools"]["listChanged"],
                    False,
                )

                status, body = client.request(
                    {
                        "jsonrpc": "2.0",
                        "method": "notifications/initialized",
                        "params": {},
                    }
                )
                self.assertEqual(status, 202)
                self.assertEqual(body, "")
                self.assertEqual(client.rpc("ping", {})["result"], {})
            except (OSError, TimeoutError, json.JSONDecodeError) as error:
                self.fail(f"MCP initialize request failed: {error}")
        finally:
            editor.stop()

    def test_http_security_and_protocol_errors(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            ping = json.dumps({"jsonrpc": "2.0", "id": 10, "method": "ping", "params": {}})

            status, _ = client.raw_request("POST", client.path, ping)
            self.assertEqual(status, 401)
            status, _ = client.raw_request(
                "POST",
                client.path,
                ping,
                {"Authorization": "Bearer incorrect"},
            )
            self.assertEqual(status, 401)

            authorized = {"Authorization": f"Bearer {client.token}"}
            status, _ = client.raw_request("GET", client.path, "", authorized)
            self.assertEqual(status, 405)
            status, _ = client.raw_request("POST", "/wrong", ping, authorized)
            self.assertEqual(status, 404)
            status, _ = client.raw_request(
                "POST",
                client.path,
                ping,
                {**authorized, "Origin": "https://attacker.example"},
            )
            self.assertEqual(status, 403)
            status, _ = client.raw_request(
                "POST",
                client.path,
                ping,
                {**authorized, "Origin": "https://[::1].attacker.example"},
            )
            self.assertEqual(status, 403)

            status, body = client.raw_request("POST", client.path, "{", authorized)
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["error"]["code"], -32700)
            status, body = client.raw_request("POST", client.path, "[]", authorized)
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["error"]["code"], -32600)

            unknown = client.rpc("unknown/method", {})
            self.assertEqual(unknown["error"]["code"], -32601)
            invalid = client.rpc("initialize", {})
            self.assertEqual(invalid["error"]["code"], -32602)

            status, _ = client.raw_socket_request(
                b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: nope\r\n\r\n"
            )
            self.assertEqual(status, 400)
            status, _ = client.raw_socket_request(
                b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 9000000\r\n\r\n"
            )
            self.assertEqual(status, 413)
            status, _ = client.raw_socket_request(b"GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
            self.assertEqual(status, 405)

            encoded_ping = ping.encode("utf-8")
            status, _ = client.raw_socket_request(
                (
                    f"POST /mcp HTTP/1.1\r\n"
                    f"Host: 127.0.0.1\r\n"
                    f"Authorization: Bearer {client.token}\r\n"
                    f"Content-Length: {len(encoded_ping)}\r\n"
                    f"Transfer-Encoding: chunked\r\n\r\n"
                ).encode("ascii")
                + encoded_ping
            )
            self.assertEqual(status, 400)

            status, body = client.raw_socket_request(
                (
                    f"POST /mcp HTTP/1.1\r\n"
                    f"Host: 127.0.0.1\r\n"
                    f"Authorization: Bearer {client.token}\r\n"
                    f"Content-Length: {len(encoded_ping)}\r\n\r\n"
                ).encode("ascii")
                + encoded_ping
                + b"trailing bytes are outside the declared body"
            )
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["id"], 10)
        finally:
            editor.stop()

    def test_tools_list_and_call(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            self.initialize_client(client)

            listed = client.rpc("tools/list", {})
            self.assertIn("result", listed, listed)
            tools = listed["result"]["tools"]
            self.assertEqual([tool["name"] for tool in tools], ["barista_status", "get_project_info"])
            for tool in tools:
                self.assertEqual(tool["inputSchema"]["type"], "object")
                self.assertEqual(tool["inputSchema"]["properties"], {})
                self.assertEqual(tool["inputSchema"]["required"], [])
                self.assertIs(tool["inputSchema"]["additionalProperties"], False)

            status_result = client.rpc(
                "tools/call", {"name": "barista_status", "arguments": {}}
            )["result"]
            self.assertIs(status_result["isError"], False)
            self.assertEqual(
                json.loads(status_result["content"][0]["text"]),
                status_result["structuredContent"],
            )
            status = status_result["structuredContent"]
            self.assertEqual(status["name"], "BaristaMCP")
            self.assertEqual(status["version"], "0.1.0")
            self.assertEqual(status["protocol_version"], "2025-11-25")
            self.assertIs(status["initialized"], True)
            self.assertIs(status["local_only"], True)
            self.assertEqual(status["endpoint"], f"http://127.0.0.1:{status['port']}/mcp")
            self.assertNotIn("token", status)

            project_result = client.rpc(
                "tools/call", {"name": "get_project_info", "arguments": {}}
            )["result"]
            self.assertIs(project_result["isError"], False)
            project = project_result["structuredContent"]
            self.assertEqual(project["project_name"], "BaristaMCP Test Project")
            self.assertEqual(Path(project["project_path"]).resolve(), PROJECT_DIR.resolve())
            self.assertRegex(project["godot_version"]["string"], r"^4\.7(?:\.|$)")
            self.assertEqual(project["current_scene"], "")
            self.assertIs(project["is_playing"], False)

            missing_name = client.rpc("tools/call", {"arguments": {}})
            self.assertEqual(missing_name["error"]["code"], -32602)
            wrong_arguments = client.rpc(
                "tools/call", {"name": "barista_status", "arguments": []}
            )
            self.assertEqual(wrong_arguments["error"]["code"], -32602)
            unknown = client.rpc("tools/call", {"name": "unknown", "arguments": {}})["result"]
            self.assertIs(unknown["isError"], True)
            rejected = client.rpc(
                "tools/call", {"name": "barista_status", "arguments": {"extra": True}}
            )["result"]
            self.assertIs(rejected["isError"], True)
        finally:
            editor.stop()

    def test_json_rpc_batches_and_integer_ids(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            status, body = client.request(
                [
                    {"jsonrpc": "2.0", "id": 42, "method": "ping", "params": {}},
                    {
                        "jsonrpc": "2.0",
                        "method": "notifications/initialized",
                        "params": {},
                    },
                ]
            )
            self.assertEqual(status, 200)
            responses = json.loads(body)
            self.assertIsInstance(responses, list)
            self.assertEqual(len(responses), 1)
            self.assertIsInstance(responses[0]["id"], int)
            self.assertEqual(responses[0]["id"], 42)
            self.assertEqual(responses[0]["result"], {})

            status, body = client.request(
                [{"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}]
            )
            self.assertEqual((status, body), (202, ""))

            status, body = client.request({"jsonrpc": "2.0"})
            self.assertEqual(status, 200)
            invalid = json.loads(body)
            self.assertIsNone(invalid["id"])
            self.assertEqual(invalid["error"]["code"], -32600)
        finally:
            editor.stop()

    def test_protocol_lifecycle_and_envelope_validation(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())

            pre_initialize = client.rpc("tools/list", {})
            self.assertEqual(pre_initialize["error"]["code"], -32002)

            status, body = client.request(
                {
                    "jsonrpc": "2.0",
                    "method": "notifications/initialized",
                    "params": {},
                }
            )
            self.assertEqual((status, body), (202, ""))
            still_uninitialized = client.rpc("tools/list", {})
            self.assertEqual(still_uninitialized["error"]["code"], -32002)

            for invalid_id in (True, [1], {"value": 1}, 1.5, 1e100):
                status, body = client.request(
                    {
                        "jsonrpc": "2.0",
                        "id": invalid_id,
                        "method": "ping",
                        "params": {},
                    }
                )
                self.assertEqual(status, 200)
                invalid = json.loads(body)
                self.assertIsNone(invalid["id"])
                self.assertEqual(invalid["error"]["code"], -32600)

            status, body = client.request(
                {"jsonrpc": "2.0", "id": 99, "method": "ping", "params": []}
            )
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["error"]["code"], -32602)

            for incomplete in (
                {"protocolVersion": "2025-11-25", "clientInfo": {"name": "test", "version": "1"}},
                {"protocolVersion": "2025-11-25", "capabilities": {}},
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "test"},
                },
            ):
                response = client.rpc("initialize", incomplete)
                self.assertEqual(response["error"]["code"], -32602)

            response = client.rpc(
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"},
                },
            )
            self.assertIn("result", response)
            repeated = client.rpc(
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"},
                },
            )
            self.assertEqual(repeated["error"]["code"], -32600)
            before_notification = client.rpc("tools/list", {})
            self.assertEqual(before_notification["error"]["code"], -32002)

            status, body = client.request(
                {
                    "jsonrpc": "2.0",
                    "method": "notifications/initialized",
                    "params": {},
                }
            )
            self.assertEqual((status, body), (202, ""))
            self.assertIn("result", client.rpc("tools/list", {}))
            invalid_list_params = client.rpc("tools/list", "not-an-object")
            self.assertEqual(invalid_list_params["error"]["code"], -32602)
        finally:
            editor.stop()

    def test_oversized_response_is_bounded(self) -> None:
        editor = EditorProcess()
        try:
            editor.start()
            client = MCPClient(editor.wait_for_discovery())
            huge_method = "x" * (2 * 1024 * 1024)
            status, body = client.request(
                {"jsonrpc": "2.0", "id": 1, "method": huge_method, "params": {}}
            )
            self.assertEqual(status, 500)
            self.assertEqual(json.loads(body), {"error": "response_too_large"})
        finally:
            editor.stop()

    def test_invalid_project_setting_types_refuse_start(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-invalid-settings-") as temporary:
            project_dir = Path(temporary)
            self.write_test_project(project_dir, '[barista_mcp]\nserver/port="0"')

            editor = EditorProcess(project_dir)
            try:
                editor.start()
                editor.wait_for_output(
                    "BaristaMCP: invalid server project settings; refusing to start."
                )
                self.assertNotIn(DISCOVERY_PREFIX, "".join(editor.output))
            finally:
                editor.stop()

    def test_partial_request_times_out(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-timeout-") as temporary:
            project_dir = Path(temporary)
            self.write_test_project(
                project_dir, "[barista_mcp]\nserver/request_timeout_ms=200"
            )
            editor = EditorProcess(project_dir)
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery())
                partial_body = b'{"jsonrpc":"2.0"'
                status, body = client.raw_socket_request(
                    (
                        f"POST /mcp HTTP/1.1\r\n"
                        f"Host: 127.0.0.1\r\n"
                        f"Authorization: Bearer {client.token}\r\n"
                        f"Content-Length: {len(partial_body) + 20}\r\n\r\n"
                    ).encode("ascii")
                    + partial_body,
                    shutdown_write=False,
                )
                self.assertEqual(status, 408)
                self.assertEqual(json.loads(body), {"error": "request_timeout"})
            finally:
                editor.stop()

    def test_unread_large_response_does_not_block_editor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="barista-mcp-write-timeout-") as temporary:
            project_dir = Path(temporary)
            self.write_test_project(
                project_dir, "[barista_mcp]\nserver/request_timeout_ms=500"
            )
            editor = EditorProcess(project_dir)
            slow_connection: socket.socket | None = None
            try:
                editor.start()
                client = MCPClient(editor.wait_for_discovery())
                batch = [
                    {
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "method": "x" * 650,
                        "params": {},
                    }
                    for request_id in range(1300)
                ]
                encoded_batch = json.dumps(batch, separators=(",", ":")).encode("utf-8")
                slow_connection = socket.create_connection((client.host, client.port), timeout=3)
                slow_connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                slow_connection.sendall(
                    (
                        f"POST /mcp HTTP/1.1\r\n"
                        f"Host: 127.0.0.1\r\n"
                        f"Authorization: Bearer {client.token}\r\n"
                        f"Content-Length: {len(encoded_batch)}\r\n\r\n"
                    ).encode("ascii")
                    + encoded_batch
                )

                time.sleep(0.8)
                self.assertEqual(client.rpc("ping", {})["result"], {})
            finally:
                if slow_connection is not None:
                    slow_connection.close()
                editor.stop()

    def test_bind_failure_is_reported_without_discovery(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            port = occupied.getsockname()[1]
            with tempfile.TemporaryDirectory(prefix="barista-mcp-bind-") as temporary:
                project_dir = Path(temporary)
                self.write_test_project(
                    project_dir, f"[barista_mcp]\nserver/port={port}"
                )
                editor = EditorProcess(project_dir)
                try:
                    editor.start()
                    editor.wait_for_output("BaristaMCP: failed to bind MCP server")
                    self.assertNotIn(DISCOVERY_PREFIX, "".join(editor.output))
                finally:
                    editor.stop()

    def test_editor_can_shut_down_cleanly_after_startup(self) -> None:
        editor = EditorProcess(extra_args=("--quit-after", "180"))
        try:
            editor.start()
            editor.wait_for_discovery()
            assert editor.process is not None
            return_code = editor.process.wait(timeout=10)
            if editor.reader is not None:
                editor.reader.join(timeout=2)
            self.assertEqual(return_code, 0, "".join(editor.output))
        finally:
            editor.stop()


if __name__ == "__main__":
    unittest.main()
