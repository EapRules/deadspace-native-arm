#!/usr/bin/env python3
"""End-to-end smoke test for the stdio MCP server."""

import json
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent


def main() -> int:
    server = subprocess.Popen(
        [sys.executable, str(HERE / "mcp_server.py")],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    assert server.stdin and server.stdout
    serial = 0

    def call(method: str, params: dict) -> dict:
        nonlocal serial
        serial += 1
        message = {"jsonrpc": "2.0", "id": serial, "method": method, "params": params}
        server.stdin.write(json.dumps(message) + "\n")
        server.stdin.flush()
        response = json.loads(server.stdout.readline())
        assert response["id"] == serial, response
        return response["result"]

    try:
        init = call(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "smoke-test", "version": "1"},
            },
        )
        assert init["serverInfo"]["name"] == "deadspace-emulator"

        listed = call("tools/list", {})
        names = {tool["name"] for tool in listed["tools"]}
        assert {
            "run_compatibility_matrix", "compatibility_status",
            "read_compatibility_report",
        } <= names

        started = call(
            "tools/call",
            {"name": "start_emulator", "arguments": {"rebuild": True}},
        )
        assert not started.get("isError"), started

        held = call(
            "tools/call",
            {
                "name": "set_stick",
                "arguments": {"stick": "right", "x": 1.0, "y": 0.0},
            },
        )
        assert not held.get("isError"), held
        released = call(
            "tools/call",
            {
                "name": "set_stick",
                "arguments": {"stick": "right", "x": 0.0, "y": 0.0},
            },
        )
        assert not released.get("isError"), released

        capture = call(
            "tools/call", {"name": "capture_screen", "arguments": {}}
        )
        assert not capture.get("isError"), capture
        image = next(item for item in capture["content"] if item["type"] == "image")
        import base64
        assert base64.b64decode(image["data"]).startswith(b"\x89PNG\r\n\x1a\n")

        stopped = call(
            "tools/call", {"name": "stop_emulator", "arguments": {}}
        )
        assert not stopped.get("isError"), stopped
        print("MCP smoke test PASS: initialize, start, stick, PNG capture, stop")
        return 0
    finally:
        if server.poll() is None:
            server.stdin.close()
            server.wait(timeout=20)


if __name__ == "__main__":
    raise SystemExit(main())
