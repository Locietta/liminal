"""Protocol-level coverage for the deterministic development MCP server."""

import json
import os
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]


def find_binary():
    executable = "liminal-dev-mcp.exe" if os.name == "nt" else "liminal-dev-mcp"
    for mode in ("releasedbg", "release"):
        for platform_dir in (REPO_ROOT / "build").glob(f"*/*/{mode}"):
            candidate = platform_dir / executable
            if candidate.exists():
                return candidate
    return None


BINARY = find_binary()
pytestmark = pytest.mark.skipif(
    BINARY is None, reason="liminal-dev-mcp not built (run `pixi run build`)"
)


def request(identifier, method, params=None):
    return {
        "jsonrpc": "2.0",
        "id": identifier,
        "method": method,
        "params": params or {},
    }


def test_headless_session_can_be_discovered_reproduced_and_inspected():
    messages = [
        request("discover", "server/discover"),
        request("legacy", "initialize", {"protocolVersion": "2025-11-25"}),
        request("list", "tools/list"),
        request(
            "create",
            "tools/call",
            {
                "name": "session_create",
                "arguments": {
                    "driver": "tui.headless",
                    "columns": 24,
                    "rows": 8,
                    "now_ms": 100,
                },
            },
        ),
        request(
            "apply",
            "tools/call",
            {
                "name": "session_apply",
                "arguments": {
                    "session_id": "session-1",
                    "actions": [
                        {"type": "insert", "text": "hello 👩‍💻"},
                        {"type": "submit"},
                        {"type": "assistant_delta", "text": "world"},
                        {"type": "advance_time", "milliseconds": 16},
                    ],
                },
            },
        ),
        request(
            "inspect",
            "tools/call",
            {
                "name": "session_inspect",
                "arguments": {"session_id": "session-1"},
            },
        ),
        request(
            "close",
            "tools/call",
            {"name": "session_close", "arguments": {"session_id": "session-1"}},
        ),
    ]
    result = subprocess.run(
        [str(BINARY)],
        input="".join(json.dumps(message) + "\n" for message in messages),
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=20,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert not result.stderr
    responses = {
        response["id"]: response
        for response in map(json.loads, result.stdout.splitlines())
    }

    assert responses["discover"]["result"]["supportedVersions"] == [
        "2026-07-28",
        "2025-11-25",
    ]
    assert responses["legacy"]["result"]["protocolVersion"] == "2025-11-25"
    assert [tool["name"] for tool in responses["list"]["result"]["tools"]] == [
        "session_create",
        "session_apply",
        "session_inspect",
        "session_close",
    ]

    created = responses["create"]["result"]["structuredContent"]
    assert created["session_id"] == "session-1"
    assert created["capabilities"]["process"] is False
    snapshot = responses["inspect"]["result"]["structuredContent"]["snapshot"]
    assert snapshot["now_ms"] == 116
    assert snapshot["render_count"] == 2
    assert snapshot["render_pending"] is False
    assert snapshot["semantic_state"] == "streaming"
    assert [block["text"] for block in snapshot["blocks"]] == ["hello 👩‍💻", "world"]
    assert snapshot["ansi_operations"]
    assert snapshot["cells"]
    assert responses["close"]["result"]["structuredContent"]["closed"] is True
