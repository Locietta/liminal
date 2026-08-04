"""Protocol-level coverage for the deterministic development MCP server."""

import ctypes
import json
import os
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from ctypes import wintypes
from pathlib import Path

import pytest


sys.path.insert(0, str(Path(__file__).parent))
import mock_openai


REPO_ROOT = Path(__file__).resolve().parents[2]


def find_binary():
    configured = os.environ.get("LIMINAL_DEV_MCP_BINARY")
    if configured:
        candidate = Path(configured)
        if candidate.exists():
            return candidate
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


@contextmanager
def running_server(*, environment=None):
    process = subprocess.Popen(
        [str(BINARY)],
        cwd=REPO_ROOT,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    try:
        yield process
    finally:
        if process.poll() is None:
            process.stdin.close()
        try:
            return_code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            return_code = process.wait(timeout=5)
        stderr = process.stderr.read()
        assert return_code == 0, stderr
        assert not stderr


def exchange(process, message):
    process.stdin.write(json.dumps(message) + "\n")
    process.stdin.flush()
    line = process.stdout.readline()
    assert line, process.stderr.read()
    return json.loads(line)


def tool_content(response):
    return response["result"]["structuredContent"]


def process_exists(process_id):
    if os.name == "nt":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, wintypes.LPDWORD]
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        process = kernel32.OpenProcess(0x1000, False, process_id)
        if not process:
            return False
        try:
            exit_code = wintypes.DWORD()
            return (
                bool(kernel32.GetExitCodeProcess(process, ctypes.byref(exit_code)))
                and exit_code.value == 259
            )
        finally:
            kernel32.CloseHandle(process)

    try:
        os.kill(process_id, 0)
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def wait_for_process_exit(process_id, timeout=5):
    deadline = time.monotonic() + timeout
    while process_exists(process_id):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)
    return True


@pytest.fixture
def openai_mock():
    server, state = mock_openai.make_server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}/v1", state
    server.shutdown()


@pytest.fixture
def slow_openai_mock():
    server, state = mock_openai.make_server(chunk_delay=0.05)
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
    environment["LIMINAL_TOOL_MODE"] = "unrestricted"
    environment["LIMINAL_SYSTEM_PROMPT"] = "Test system policy."
    environment["LIMINAL_DEVELOPER_PROMPT"] = "Test developer policy."
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


def test_live_session_bounds_are_preflighted_and_closed_processes_are_reaped(
    openai_mock, tmp_path
):
    base_url, _ = openai_mock
    environment = live_environment(tmp_path, base_url)

    with running_server(environment=environment) as process:
        process_ids = []
        for index in range(4):
            response = exchange(
                process,
                request(
                    f"create-{index}",
                    "tools/call",
                    {
                        "name": "session_create",
                        "arguments": {
                            "driver": "liminal.pty",
                            "cwd": str(REPO_ROOT),
                        },
                    },
                ),
            )
            process_ids.append(tool_content(response)["snapshot"]["process_id"])

        rejected = exchange(
            process,
            request(
                "create-over-limit",
                "tools/call",
                {
                    "name": "session_create",
                    "arguments": {
                        "driver": "liminal.pty",
                        "cwd": str(REPO_ROOT),
                    },
                },
            ),
        )["result"]
        assert rejected["isError"] is True
        assert rejected["structuredContent"]["error"] == "live session limit exceeded"

        started = time.monotonic()
        rejected = exchange(
            process,
            request(
                "apply-over-budget",
                "tools/call",
                {
                    "name": "session_apply",
                    "arguments": {
                        "session_id": "session-1",
                        "actions": [
                            {"type": "write", "text": "MUST_NOT_BE_APPLIED"},
                            {"type": "wait", "milliseconds": 20_000},
                            {"type": "wait", "milliseconds": 10_001},
                        ],
                    },
                },
            ),
        )["result"]
        assert time.monotonic() - started < 2
        assert rejected["isError"] is True
        assert rejected["structuredContent"]["error"] == "total wait budget exceeded"

        inspected = exchange(
            process,
            request(
                "inspect",
                "tools/call",
                {
                    "name": "session_inspect",
                    "arguments": {"session_id": "session-1"},
                },
            ),
        )
        assert (
            "MUST_NOT_BE_APPLIED" not in tool_content(inspected)["snapshot"]["output"]
        )

        for index, process_id in enumerate(process_ids, start=1):
            closed = exchange(
                process,
                request(
                    f"close-{index}",
                    "tools/call",
                    {
                        "name": "session_close",
                        "arguments": {"session_id": f"session-{index}"},
                    },
                ),
            )
            assert tool_content(closed)["closed"] is True
            assert wait_for_process_exit(process_id)


def test_server_eof_reaps_live_process(openai_mock, tmp_path):
    base_url, _ = openai_mock
    environment = live_environment(tmp_path, base_url)

    with running_server(environment=environment) as process:
        created = exchange(
            process,
            request(
                "create",
                "tools/call",
                {
                    "name": "session_create",
                    "arguments": {
                        "driver": "liminal.pty",
                        "cwd": str(REPO_ROOT),
                    },
                },
            ),
        )
        process_id = tool_content(created)["snapshot"]["process_id"]
        assert process_exists(process_id)
        process.stdin.close()
        assert process.wait(timeout=10) == 0
        assert wait_for_process_exit(process_id)


def test_close_reaps_live_process_during_active_turn(slow_openai_mock, tmp_path):
    base_url, state = slow_openai_mock
    environment = live_environment(tmp_path, base_url)

    with running_server(environment=environment) as process:
        created = exchange(
            process,
            request(
                "create",
                "tools/call",
                {
                    "name": "session_create",
                    "arguments": {
                        "driver": "liminal.pty",
                        "cwd": str(REPO_ROOT),
                    },
                },
            ),
        )
        process_id = tool_content(created)["snapshot"]["process_id"]
        exchange(
            process,
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
                            {"type": "wait", "milliseconds": 1_000},
                        ],
                    },
                },
            ),
        )
        assert state["calls"] >= 2

        closed = exchange(
            process,
            request(
                "close",
                "tools/call",
                {
                    "name": "session_close",
                    "arguments": {"session_id": "session-1"},
                },
            ),
        )
        assert tool_content(closed)["closed"] is True
        assert wait_for_process_exit(process_id)


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
