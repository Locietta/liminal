"""Protocol-level coverage for the deterministic development MCP server."""

import json
import os
import subprocess
import sys
import threading
from pathlib import Path

import pytest


sys.path.insert(0, str(Path(__file__).parent))
import mock_openai


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


def run_server(messages, *, environment=None, timeout=20):
    result = subprocess.run(
        [str(BINARY)],
        input="".join(json.dumps(message) + "\n" for message in messages),
        cwd=REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert not result.stderr
    return {
        response["id"]: response
        for response in map(json.loads, result.stdout.splitlines())
    }


@pytest.fixture
def openai_mock():
    server, state = mock_openai.make_server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/v1", state
    server.shutdown()


def live_environment(tmp_path, base_url):
    providers = tmp_path / "providers.json"
    providers.write_text(
        json.dumps(
            {
                "providers": {
                    "mcp-test": {
                        "api": "openai-responses",
                        "base_url": base_url,
                        "api_key": mock_openai.API_KEY,
                        "discover_models": False,
                        "models": [{"id": "test-model"}],
                    }
                }
            }
        )
    )
    environment = os.environ.copy()
    environment["LIMINAL_PROVIDERS_FILE"] = str(providers)
    environment["LIMINAL_AUTH_FILE"] = str(tmp_path / "missing-auth.json")
    environment["LIMINAL_MODEL"] = "test-model"
    return environment


def test_headless_session_can_be_discovered_reproduced_and_inspected():
    messages = [
        request("discover", "server/discover"),
        request("legacy", "initialize", {"protocolVersion": "2025-11-25"}),
        request("list", "tools/list"),
        request(
            "create",
            "tools/call",
            {
                "_meta": {"x-codex-turn-metadata": {"session_id": "integration-test"}},
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
    responses = run_server(messages)

    assert responses["discover"]["result"]["supportedVersions"] == [
        "2026-07-28",
        "2025-11-25",
    ]
    assert responses["legacy"]["result"]["protocolVersion"] == "2025-11-25"
    tools = responses["list"]["result"]["tools"]
    assert [tool["name"] for tool in tools] == [
        "session_create",
        "session_apply",
        "session_inspect",
        "session_close",
    ]
    assert tools[-1]["annotations"]["destructiveHint"] is False

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


def test_live_session_operates_real_liminal_process(openai_mock, tmp_path):
    base_url, state = openai_mock
    messages = [
        request(
            "create",
            "tools/call",
            {
                "name": "session_create",
                "arguments": {
                    "driver": "liminal.pty",
                    "columns": 48,
                    "rows": 10,
                    "cwd": str(REPO_ROOT),
                },
            },
        ),
        request(
            "prompt",
            "tools/call",
            {
                "name": "session_apply",
                "arguments": {
                    "session_id": "session-1",
                    "actions": [
                        {"type": "wait", "milliseconds": 500},
                        {"type": "prompt", "text": "inspect"},
                        {"type": "wait", "milliseconds": 3000},
                        {"type": "resize", "columns": 60, "rows": 12},
                        {"type": "wait", "milliseconds": 250},
                    ],
                },
            },
        ),
        request(
            "quit",
            "tools/call",
            {
                "name": "session_apply",
                "arguments": {
                    "session_id": "session-1",
                    "actions": [
                        {"type": "prompt", "text": "/quit"},
                        {"type": "wait", "milliseconds": 1000},
                    ],
                },
            },
        ),
        request(
            "close",
            "tools/call",
            {"name": "session_close", "arguments": {"session_id": "session-1"}},
        ),
    ]
    responses = run_server(
        messages,
        environment=live_environment(tmp_path, base_url),
        timeout=30,
    )

    created = responses["create"]["result"]["structuredContent"]
    assert created["capabilities"]["driver"] == "liminal.pty"
    assert created["capabilities"]["process"] is True
    snapshot = responses["prompt"]["result"]["structuredContent"]["snapshot"]
    assert snapshot["columns"] == 60
    assert snapshot["rows"] == 12
    assert snapshot["process_id"] > 0
    assert snapshot["output_encoding"] == "escaped-control-bytes"
    assert "Let me inspect the repository." in snapshot["output"]
    assert "The working directory is the liminal repository." in snapshot["output"]
    assert any(
        "working directory is the liminal" in line for line in snapshot["visible_text"]
    )
    quit_snapshot = responses["quit"]["result"]["structuredContent"]["snapshot"]
    assert quit_snapshot["running"] is False
    assert quit_snapshot["exit_code"] == 0
    assert state["errors"] == []
    assert responses["close"]["result"]["structuredContent"]["closed"] is True


RICH_FIXTURE = """# Rich output
Paragraph with **strong**, *emphasis*, `inline code`, and [docs](https://example.com/docs).
- list item with enough words to wrap at narrow widths
```cpp
if (ready) {
    run();
}
```
```diff
@@ -1 +1 @@
-old value
+new value
```"""


def rich_snapshot(columns):
    responses = run_server(
        [
            request(
                "create",
                "tools/call",
                {
                    "name": "session_create",
                    "arguments": {
                        "driver": "tui.headless",
                        "columns": columns,
                        "rows": 40,
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
                            {"type": "assistant_delta", "text": RICH_FIXTURE},
                            {"type": "assistant_segment_completed"},
                            {
                                "type": "tool_started",
                                "call_id": "one",
                                "name": "read_file",
                            },
                            {
                                "type": "tool_started",
                                "call_id": "two",
                                "name": "run_tests",
                            },
                            {
                                "type": "tool_completed",
                                "call_id": "one",
                                "name": "read_file",
                            },
                            {"type": "flush"},
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
    )
    assert responses["close"]["result"]["structuredContent"]["closed"] is True
    return responses["inspect"]["result"]["structuredContent"]["snapshot"]


def test_rich_output_is_readable_at_narrow_and_wide_sizes():
    narrow = rich_snapshot(28)
    wide = rich_snapshot(72)

    for snapshot in (narrow, wide):
        visible = "\n".join(snapshot["visible_text"])
        styles = {cell["style"] for cell in snapshot["cells"]}
        assert "assistant: Rich output" in visible
        assert "# Rich output" not in visible
        assert "docs" in visible and "<https://example.com/docs>" in visible
        assert "• list item" in visible
        assert "[code: cpp]" in visible and "    run();" in visible
        assert "@@ -1 +1 @@" in visible and "-old value" in visible
        assert "+new value" in visible
        assert {
            "emphasis",
            "italic",
            "code",
            "link",
            "diff_addition",
            "diff_deletion",
            "diff_hunk",
        } <= styles
        assert snapshot["semantic_state"] == "running_tools"
        assert [block["state"] for block in snapshot["blocks"][-2:]] == [
            "completed",
            "running",
        ]

    narrow_lines = sum(bool(line) for line in narrow["visible_text"])
    wide_lines = sum(bool(line) for line in wide["visible_text"])
    assert narrow_lines > wide_lines
