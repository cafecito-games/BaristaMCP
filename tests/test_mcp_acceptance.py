from __future__ import annotations

import json
import http.client
import os
import queue
import re
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

    def request(self, payload: object) -> tuple[int, str]:
        body = json.dumps(payload, separators=(",", ":"))
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        try:
            connection.request(
                "POST",
                self.path,
                body=body,
                headers={
                    "Authorization": f"Bearer {self.token}",
                    "Content-Type": "application/json",
                    "Accept": "application/json",
                },
            )
            response = connection.getresponse()
            return response.status, response.read().decode("utf-8")
        finally:
            connection.close()

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


class BaristaMCPAcceptanceTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
