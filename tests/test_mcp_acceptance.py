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
import unittest
from pathlib import Path
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
PROJECT_DIR = ROOT / "project"
DISCOVERY_PREFIX = "BARISTA_MCP "


class EditorProcess:
    def __init__(self) -> None:
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
            [godot_bin, "--headless", "--editor", "--path", str(PROJECT_DIR)],
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

    def rpc(self, method: str, params: dict[str, object]) -> dict[str, object]:
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

    def raw_socket_request(self, request: bytes) -> tuple[int, str]:
        with socket.create_connection((self.host, self.port), timeout=3) as connection:
            connection.sendall(request)
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
        finally:
            editor.stop()


if __name__ == "__main__":
    unittest.main()
