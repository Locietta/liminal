"""Integration tests: drive the liminal binary against in-process API mocks.

Hermetic: mock servers bind ephemeral localhost ports, credentials are
hardcoded fakes (no real API keys involved), stdin is piped. Run via
`pixi run test-integration` or plain `python -m pytest tests/integration`.

The liminal binary is located from LIMINAL_BIN or the default xmake build
output; tests skip with a clear message if it has not been built.
"""

import errno
import json
import os
import select
import shlex
import stat
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mock_anthropic
import mock_codex_auth
import mock_openai
from binaries import find_binary

REPO_ROOT = Path(__file__).resolve().parents[2]
TIMEOUT = 120
PROMPT_MARKER = "›".encode()


BINARY = find_binary(REPO_ROOT, "liminal", "LIMINAL_BIN")
pytestmark = pytest.mark.skipif(
    BINARY is None, reason="liminal binary not built (run `xmake build liminal`)"
)


@pytest.fixture
def anthropic_mock():
    server, state = mock_anthropic.make_server()
    import threading

    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}", state
    server.shutdown()


def openai_mock_fixture(**kwargs):
    @pytest.fixture
    def fixture():
        server, state = mock_openai.make_server(**kwargs)
        import threading

        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}/v1", state
        server.shutdown()

    return fixture


openai_mock = openai_mock_fixture()
openai_slow_mock = openai_mock_fixture(chunk_delay=0.02)
openai_mock_no_compact = openai_mock_fixture(compact_404=True)


@pytest.fixture
def codex_auth_mock():
    server, state = mock_codex_auth.make_server()
    import threading

    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}", state
    server.shutdown()


def configured_provider(api, base_url, api_key, models):
    return {
        "api": api,
        "base_url": base_url,
        "api_key": api_key,
        "models": models,
    }


def terminal_test_environment(
    tmp_path, model_count=1, base_url="http://127.0.0.1:1/v1", api_key="unused-key"
):
    providers_file = tmp_path / "providers.json"
    providers_file.write_text(
        json.dumps(
            {
                "providers": {
                    "terminal-test": configured_provider(
                        "openai-responses",
                        base_url,
                        api_key,
                        [
                            {
                                "id": "test-model",
                                "context_window": 128_000,
                                "max_output_tokens": 8_192,
                            }
                        ]
                        + [
                            {
                                "id": f"extra-model-{index}",
                                "context_window": 128_000,
                                "max_output_tokens": 8_192,
                            }
                            for index in range(model_count - 1)
                        ],
                    )
                }
            }
        )
    )
    env = os.environ.copy()
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_AUTH_FILE"] = str(tmp_path / "missing-auth.json")
    env["LIMINAL_MODEL"] = "test-model"
    env["LIMINAL_SYSTEM_PROMPT"] = "Test system policy."
    env["LIMINAL_DEVELOPER_PROMPT"] = "Test developer policy."
    editor = tmp_path / "external_editor.py"
    editor.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "Path(sys.argv[-1]).write_text('edited externally')\n"
    )
    editor_command = [sys.executable, str(editor)]
    env["VISUAL"] = (
        subprocess.list2cmdline(editor_command)
        if os.name == "nt"
        else shlex.join(editor_command)
    )
    return env


def read_conpty_until(process, output, marker, timeout):
    deadline = time.monotonic() + timeout
    while marker not in output:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"{marker!r} did not appear: {output!r}"
        chunk = process.read(remaining)
        assert chunk, (
            f"ConPTY produced no output before {marker!r}; "
            f"exit={process.poll()}, output={output!r}"
        )
        output.extend(chunk)


def read_conpty_until_fresh(process, output, marker, timeout):
    deadline = time.monotonic() + timeout
    start = len(output)
    while marker not in output[start:]:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"fresh {marker!r} did not appear: {output!r}"
        chunk = process.read(remaining)
        assert chunk, f"ConPTY produced no fresh output before {marker!r}: {output!r}"
        output.extend(chunk)


def read_conpty_frame_without(process, output, marker, timeout):
    deadline = time.monotonic() + timeout
    start = len(output)
    while True:
        fresh = output[start:]
        if b"\x1b[?25l" in fresh and b"\x1b[?25h" in fresh and marker not in fresh:
            return
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"ConPTY diff did not clear {marker!r}: {output!r}"
        chunk = process.read(remaining)
        assert chunk, f"ConPTY closed before clearing {marker!r}: {output!r}"
        output.extend(chunk)


def read_pty_until(master, output, marker, timeout):
    deadline = time.monotonic() + timeout
    while marker not in output:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"{marker!r} did not appear: {output!r}"
        readable, _, _ = select.select([master], [], [], remaining)
        assert readable, f"PTY produced no output before {marker!r}: {output!r}"
        output.extend(os.read(master, 4096))


def read_pty_until_fresh(master, output, marker, timeout):
    deadline = time.monotonic() + timeout
    start = len(output)
    while marker not in output[start:]:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"fresh {marker!r} did not appear: {output!r}"
        readable, _, _ = select.select([master], [], [], remaining)
        assert readable, f"PTY produced no fresh output before {marker!r}: {output!r}"
        output.extend(os.read(master, 4096))


def read_pty_frame_without(master, output, marker, timeout):
    deadline = time.monotonic() + timeout
    start = len(output)
    while True:
        fresh = output[start:]
        if b"\x1b[?25l" in fresh and b"\x1b[?25h" in fresh and marker not in fresh:
            return
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"PTY diff did not clear {marker!r}: {output!r}"
        readable, _, _ = select.select([master], [], [], remaining)
        assert readable, f"PTY produced no frame clearing {marker!r}: {output!r}"
        output.extend(os.read(master, 4096))


def check_conpty_terminal_session(tmp_path, base_url):
    from liminal.dev_mcp.windows_pty import ConPtyProcess

    process = ConPtyProcess(
        [BINARY],
        cwd=REPO_ROOT,
        env=terminal_test_environment(
            tmp_path, model_count=13, base_url=base_url, api_key=mock_openai.API_KEY
        ),
        columns=40,
        rows=8,
    )
    output = bytearray()
    try:
        read_conpty_until(process, output, PROMPT_MARKER, 10)
        assert output.count(b"\x1b[?1049h") == 1
        assert output.index(b"\x1b[?2004h") < output.index(b"\x1b[?1049h")
        assert b"\x1b[?1000h\x1b[?1006h" in output

        process.write(b"\x07")
        read_conpty_until(process, output, PROMPT_MARKER + b" edited externally", 10)
        assert output.count(b"\x1b[?1049h") == 1
        assert output.count(b"\x1b[?1049l") == 0

        process.write(b"\x7f" * len("edited externally") + b"/model\r")
        read_conpty_until(process, output, b"select with /model", 5)

        process.write(b"\x1b[1;5A")
        read_conpty_until(process, output, PROMPT_MARKER + b" /model", 5)
        process.write(b"\x1b[1;5B")
        read_conpty_until_fresh(process, output, PROMPT_MARKER, 5)

        process.write(b"\x1b[A")
        read_conpty_until_fresh(process, output, PROMPT_MARKER + b" /model", 5)
        process.write(b"\x1b[B")
        read_conpty_until_fresh(process, output, PROMPT_MARKER, 5)
        process.write(b"\x1b[<64;1;1M")
        read_conpty_until_fresh(process, output, b"history", 5)
        process.write(b"\x1b[<65;1;1M")
        read_conpty_until_fresh(process, output, b"test-model", 5)

        process.write(b"\x1b[200~a\nb\x1b[201~")
        read_conpty_until(process, output, PROMPT_MARKER + b" a", 5)

        redraws = output.count(b"\x1b[H")
        process.resize(50, 10)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[H") == redraws:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"resize did not redraw the ConPTY frame: {output!r}"
            chunk = process.read(remaining)
            assert chunk, f"ConPTY output closed during resize: {output!r}"
            output.extend(chunk)

        # Clear the paste, submit a real turn, then prove input and resize are
        # handled while its deliberately slow SSE stream is still active.
        process.write(b"\x7f\x7f\x7finspect\r")
        read_conpty_until(process, output, b"Let me inspect the repository.", 10)
        redraws = output.count(b"\x1b[H")
        process.resize(60, 12)
        process.write(b"queued")
        deadline = time.monotonic() + 5
        while b"queued" not in output or output.count(b"\x1b[H") == redraws:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"input/resize stalled during turn: {output!r}"
            chunk = process.read(remaining)
            assert chunk, f"ConPTY output closed during input/resize: {output!r}"
            output.extend(chunk)
        read_conpty_until(
            process, output, b"The working directory is the liminal repository.", 10
        )
        assert b"Context " in output

        process.write(b"\x7f\x7f\x7f\x7f\x7f\x7f/quit\r")
        assert process.wait(10) == 0
        while chunk := process.read(0.1):
            output.extend(chunk)
        assert output.count(b"\x1b[?1049l") == 1
        assert output.index(b"\x1b[?1049h") < output.index(b"\x1b[?1049l")
        assert output.rindex(b"\x1b[?1049l") < output.rindex(b"\x1b[?2004l")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(5)
        process.close()


def check_conpty_terminal_restores_after_interrupt(tmp_path):
    from liminal.dev_mcp.windows_pty import ConPtyProcess

    process = ConPtyProcess(
        [BINARY],
        cwd=REPO_ROOT,
        env=terminal_test_environment(tmp_path),
        columns=40,
        rows=8,
    )
    output = bytearray()
    try:
        read_conpty_until(process, output, PROMPT_MARKER, 10)
        process.write(b"\x03")
        assert process.wait(10) == 130
        while chunk := process.read(0.1):
            output.extend(chunk)
        assert output.count(b"\x1b[?1049l") == 1
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(5)
        process.close()


def check_conpty_ctrl_c_routes_by_state(tmp_path, base_url):
    from liminal.dev_mcp.windows_pty import ConPtyProcess

    process = ConPtyProcess(
        [BINARY],
        cwd=REPO_ROOT,
        env=terminal_test_environment(
            tmp_path, base_url=base_url, api_key=mock_openai.API_KEY
        ),
        columns=60,
        rows=12,
    )
    output = bytearray()
    try:
        read_conpty_until(process, output, PROMPT_MARKER, 10)
        process.write(b"inspect\r")
        read_conpty_until(process, output, b"Let me inspect the repository.", 10)
        process.write(b"queued")
        read_conpty_until(process, output, b"queued", 5)

        cleared_at = len(output)
        process.write(b"\x03")
        read_conpty_frame_without(process, output, b"queued", 5)
        assert b"queued" not in output[cleared_at:]
        assert b"Turn cancelled" not in output[cleared_at:]
        assert process.poll() is None

        process.write(b"\x03")
        read_conpty_until(process, output, b"Turn cancelled", 10)
        assert process.poll() is None

        process.write(b"\x03")
        assert process.wait(10) == 130
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(5)
        process.close()


def run_liminal(stdin, providers, tmp_path, selector="test-model"):
    providers_file = tmp_path / "providers.json"
    providers_file.write_text(json.dumps({"providers": providers}))
    env = os.environ.copy()
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_AUTH_FILE"] = str(tmp_path / "missing-auth.json")
    env["LIMINAL_MODEL"] = selector
    env["LIMINAL_SYSTEM_PROMPT"] = "Test system policy."
    env["LIMINAL_DEVELOPER_PROMPT"] = "Test developer policy."
    result = subprocess.run(
        [str(BINARY)],
        input=stdin,
        env=env,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=TIMEOUT,
        check=False,
    )
    assert result.returncode == 0, (
        f"liminal exited {result.returncode}\nstderr: {result.stderr}\nstdout: {result.stdout}"
    )
    assert "\x1b[" not in result.stdout, (
        "redirected output must not contain terminal controls"
    )
    return result.stdout


def check(state, expected_log):
    assert not state["errors"], "\n".join(state["errors"])
    assert state["log"] == expected_log, f"scenario order mismatch: {state['log']}"


def test_redirected_eof_exits_cleanly(tmp_path):
    out = run_liminal(
        "",
        {
            "offline": configured_provider(
                "openai-responses",
                "http://127.0.0.1:1/v1",
                "unused-key",
                [{"id": "test-model"}],
            )
        },
        tmp_path,
    )
    assert "liminal - model: test-model" in out


def test_anthropic_full_cycle(anthropic_mock, tmp_path):
    """429 retry, SSE reassembly, thinking replay, parallel tools, compaction."""
    url, state = anthropic_mock
    out = run_liminal(
        "what directory are we in?\n/compact\nwhat did we find so far?\n/quit\n",
        {
            "anthropic": configured_provider(
                "anthropic-messages",
                url,
                mock_anthropic.API_KEY,
                [{"id": "test-model"}],
            )
        },
        tmp_path,
    )
    check(
        state,
        ["429", "tools-turn", "continuation", "compact-summarizer", "post-compact"],
    )
    assert "Let me check the current directory." in out  # streamed text reached stdout
    assert "[history compacted]" in out
    assert "Continuing from the compacted context." in out


def test_openai_full_cycle_remote_compact(openai_mock, tmp_path):
    """429 retry, encrypted reasoning replay, parallel tools, native /responses/compact."""
    url, state = openai_mock
    out = run_liminal(
        "check the working directory and readme\n/compact\nwhat did we find?\n/quit\n",
        {
            "openai": configured_provider(
                "openai-responses", url, mock_openai.API_KEY, [{"id": "test-model"}]
            )
        },
        tmp_path,
    )
    check(
        state, ["429", "tools-turn", "continuation", "compact-remote", "post-compact"]
    )
    assert "[history compacted]" in out
    assert "Continuing from the compacted context." in out  # encrypted item replayed


def test_openai_gateway_compact_fallback(openai_mock_no_compact, tmp_path):
    """A gateway without /responses/compact pushes onto local summarization."""
    url, state = openai_mock_no_compact
    out = run_liminal(
        "check the working directory and readme\n/compact\n/quit\n",
        {
            "openai": configured_provider(
                "openai-responses", url, mock_openai.API_KEY, [{"id": "test-model"}]
            )
        },
        tmp_path,
    )
    check(
        state,
        ["429", "tools-turn", "continuation", "compact-404", "compact-summarizer"],
    )
    assert "[history compacted]" in out


def test_model_selection_carries_history_and_applies_effort(
    anthropic_mock, openai_mock, tmp_path
):
    """A model selection may change internal routing without provider UI state."""
    anthropic_url, anthropic_state = anthropic_mock
    openai_url, openai_state = openai_mock
    out = run_liminal(
        "what directory are we in?\n/model manual-model@high\ncheck the working directory and readme\n/quit\n",
        {
            "anthropic": configured_provider(
                "anthropic-messages",
                anthropic_url,
                mock_anthropic.API_KEY,
                [{"id": "test-model"}],
            ),
            "openai": configured_provider(
                "openai-responses",
                openai_url,
                mock_openai.API_KEY,
                [
                    {
                        "id": "manual-model",
                        "name": "Manual Model",
                        "reasoning_efforts": ["low", "high"],
                        "default_reasoning_effort": "low",
                    }
                ],
            ),
        },
        tmp_path,
    )
    check(anthropic_state, ["429", "tools-turn", "continuation"])
    check(openai_state, ["429", "tools-turn", "continuation"])
    assert "[model: manual-model@high]" in out
    assert "provider:" not in out
    assert all(
        body["model"] == "manual-model" for body in openai_state["request_bodies"]
    )
    assert all(
        body["reasoning"]["effort"] == "high" for body in openai_state["request_bodies"]
    )


def test_codex_subscription_device_login(codex_auth_mock, tmp_path):
    url, state = codex_auth_mock
    auth_file = tmp_path / "auth.json"
    env = os.environ.copy()
    env["LIMINAL_AUTH_FILE"] = str(auth_file)
    env["LIMINAL_CODEX_AUTH_BASE_URL"] = url
    result = subprocess.run(
        [str(BINARY), "login", "codex"],
        env=env,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=TIMEOUT,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert f"Open {url}/codex/device and enter code TEST-CODE" in result.stdout
    assert "Codex subscription login saved." in result.stdout
    assert auth_file.exists(), result.stderr
    stored = json.loads(auth_file.read_text())["codex"]
    assert stored["type"] == "oauth"
    assert stored["access_token"] == mock_codex_auth.ACCESS_TOKEN
    assert stored["refresh_token"] == "refresh-token"
    assert stored["account_id"] == "account-123"
    if os.name != "nt":
        assert stat.S_IMODE(auth_file.stat().st_mode) == 0o600
    assert state["log"] == ["start", "poll", "exchange"]

    stored["expires_at"] = 0
    auth_file.write_text(json.dumps({"codex": stored}))

    env["LIMINAL_CODEX_API_BASE_URL"] = f"{url}/codex"
    env["LIMINAL_MODEL"] = "codex/gpt-5.6-sol"
    session = subprocess.run(
        [str(BINARY)],
        input="hello\n/quit\n",
        env=env,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=TIMEOUT,
        check=False,
    )
    assert session.returncode == 0, session.stderr
    assert "CODEX_STREAM_OK" in session.stdout
    assert state["log"] == [
        "start",
        "poll",
        "exchange",
        "refresh",
        "responses",
    ]


def test_terminal_session_restores_state(tmp_path, openai_slow_mock):
    """The interactive backend uses and restores an alternate terminal screen."""
    base_url, _ = openai_slow_mock
    if os.name == "nt":
        check_conpty_terminal_session(tmp_path, base_url)
        return

    import fcntl
    import pty
    import signal
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
    original = termios.tcgetattr(slave)
    env = terminal_test_environment(
        tmp_path,
        model_count=13,
        base_url=base_url,
        api_key=mock_openai.API_KEY,
    )

    process = subprocess.Popen(
        [str(BINARY)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
        cwd=REPO_ROOT,
        close_fds=True,
    )
    output = bytearray()
    try:
        deadline = time.monotonic() + 10
        while PROMPT_MARKER not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"prompt did not appear: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"prompt did not appear: {output!r}"
            output.extend(os.read(master, 4096))

        assert output.count(b"\x1b[?1049h") == 1
        assert output.index(b"\x1b[?2004h") < output.index(b"\x1b[?1049h")
        assert b"\x1b[?1000h\x1b[?1006h" in output

        os.kill(process.pid, signal.SIGTSTP)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[?1049l") < 1:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"suspend did not restore terminal: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"suspend produced no terminal restoration: {output!r}"
            output.extend(os.read(master, 4096))
        deadline = time.monotonic() + 5
        stopped_status = None
        while stopped_status is None:
            waited_pid, status = os.waitpid(process.pid, os.WUNTRACED | os.WNOHANG)
            if waited_pid == process.pid:
                stopped_status = status
                break
            assert time.monotonic() < deadline, (
                "process did not enter the stopped state"
            )
            time.sleep(0.01)
        assert os.WIFSTOPPED(stopped_status)
        os.kill(process.pid, signal.SIGCONT)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[?1049h") < 2:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"resume did not re-enter terminal: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"resume produced no terminal redraw: {output!r}"
            output.extend(os.read(master, 4096))

        os.write(master, b"\x07")
        read_pty_until(master, output, PROMPT_MARKER + b" edited externally", 10)
        assert output.count(b"\x1b[?1049h") == 2
        assert output.count(b"\x1b[?1049l") == 1

        os.write(master, b"\x7f" * len("edited externally") + b"/model\r")
        deadline = time.monotonic() + 5
        while b"select with /model" not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"model catalog did not enter the viewport: {output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"model catalog did not enter the viewport: {output!r}"
            output.extend(os.read(master, 4096))

        os.write(master, b"\x1b[1;5A")
        read_pty_until(master, output, PROMPT_MARKER + b" /model", 5)
        os.write(master, b"\x1b[1;5B")
        read_pty_frame_without(master, output, PROMPT_MARKER + b" /model", 5)

        os.write(master, b"\x1b[A")
        read_pty_until_fresh(master, output, PROMPT_MARKER + b" /model", 5)
        os.write(master, b"\x1b[B")
        read_pty_until_fresh(master, output, PROMPT_MARKER, 5)
        os.write(master, b"\x1b[<64;1;1M")
        read_pty_until_fresh(master, output, b"history", 5)
        os.write(master, b"\x1b[<65;1;1M")
        read_pty_until_fresh(master, output, b"test-model", 5)

        os.write(master, b"\x1b[200~a\nb\x1b[201~")
        read_pty_until(master, output, PROMPT_MARKER + b" a", 5)

        redraws = output.count(b"\x1b[H")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 50, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[H") == redraws:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"resize did not redraw the application-owned frame: {output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, (
                f"resize did not redraw the application-owned frame: {output!r}"
            )
            output.extend(os.read(master, 4096))

        os.write(master, b"\x7f\x7f\x7finspect\r")
        deadline = time.monotonic() + 10
        while b"Let me inspect the repository." not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"slow turn did not start: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"slow turn produced no output: {output!r}"
            output.extend(os.read(master, 4096))
        redraws = output.count(b"\x1b[H")
        os.write(master, b"queued")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 60, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        deadline = time.monotonic() + 5
        while b"queued" not in output or output.count(b"\x1b[H") == redraws:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"input/resize stalled during turn: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"input/resize produced no redraw during turn: {output!r}"
            output.extend(os.read(master, 4096))
        deadline = time.monotonic() + 10
        while b"The working directory is the liminal repository." not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"slow turn did not complete: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"slow turn stopped producing output: {output!r}"
            output.extend(os.read(master, 4096))
        assert b"Context " in output

        os.write(master, b"\x7f\x7f\x7f\x7f\x7f\x7f/quit\r")
        assert process.wait(timeout=10) == 0
        while True:
            readable, _, _ = select.select([master], [], [], 0.1)
            if not readable:
                break
            try:
                chunk = os.read(master, 4096)
            except OSError as error:
                assert error.errno == errno.EIO
                break
            if not chunk:
                break
            output.extend(chunk)
        assert output.count(b"\x1b[?1049l") == 2
        assert output.index(b"\x1b[?1049h") < output.index(b"\x1b[?1049l")
        assert output.rindex(b"\x1b[?1049l") < output.rindex(b"\x1b[?2004l")
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


@pytest.mark.skipif(os.name == "nt", reason="POSIX PTY stream-routing test")
def test_redirected_stdout_keeps_interactive_tui_on_stderr(tmp_path):
    """A redirected data stream must not disable terminal input or emit VT."""
    import fcntl
    import pty
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
    original = termios.tcgetattr(slave)
    env = terminal_test_environment(tmp_path)
    process = subprocess.Popen(
        [str(BINARY)],
        stdin=slave,
        stdout=subprocess.PIPE,
        stderr=slave,
        env=env,
        cwd=REPO_ROOT,
        close_fds=True,
    )
    terminal_output = bytearray()
    try:
        deadline = time.monotonic() + 10
        while PROMPT_MARKER not in terminal_output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"stderr TUI prompt did not appear: {terminal_output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"stderr TUI produced no prompt: {terminal_output!r}"
            terminal_output.extend(os.read(master, 4096))
        os.write(master, b"/quit\r")
        stdout, _ = process.communicate(timeout=10)
        assert b"\x1b[" not in stdout
        assert b"liminal - model: test-model" in stdout
        assert terminal_output.count(b"\x1b[?1049h") == 1
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


def test_terminal_restores_after_interrupt(tmp_path):
    if os.name == "nt":
        check_conpty_terminal_restores_after_interrupt(tmp_path)
        return

    import fcntl
    import pty
    import signal
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
    original = termios.tcgetattr(slave)
    providers_file = tmp_path / "providers.json"
    providers_file.write_text(
        json.dumps(
            {
                "providers": {
                    "terminal-test": configured_provider(
                        "openai-responses",
                        "http://127.0.0.1:1/v1",
                        "unused-key",
                        [{"id": "test-model"}],
                    )
                }
            }
        )
    )
    env = os.environ.copy()
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_AUTH_FILE"] = str(tmp_path / "missing-auth.json")
    env["LIMINAL_MODEL"] = "test-model"
    process = subprocess.Popen(
        [str(BINARY)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
        cwd=REPO_ROOT,
        close_fds=True,
    )
    output = bytearray()
    try:
        deadline = time.monotonic() + 10
        while PROMPT_MARKER not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"prompt did not appear: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"prompt did not appear: {output!r}"
            output.extend(os.read(master, 4096))

        os.kill(process.pid, signal.SIGINT)
        assert process.wait(timeout=10) == 130
        while True:
            readable, _, _ = select.select([master], [], [], 0.1)
            if not readable:
                break
            try:
                chunk = os.read(master, 4096)
            except OSError as error:
                assert error.errno == errno.EIO
                break
            if not chunk:
                break
            output.extend(chunk)

        assert output.count(b"\x1b[?1049l") == 1
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


def test_ctrl_c_routes_by_ui_state(openai_slow_mock, tmp_path):
    base_url, _ = openai_slow_mock
    if os.name == "nt":
        check_conpty_ctrl_c_routes_by_state(tmp_path, base_url)
        return

    import fcntl
    import pty
    import signal
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 60, 0, 0))
    original = termios.tcgetattr(slave)
    process = subprocess.Popen(
        [str(BINARY)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=terminal_test_environment(
            tmp_path, base_url=base_url, api_key=mock_openai.API_KEY
        ),
        cwd=REPO_ROOT,
        close_fds=True,
    )
    output = bytearray()
    try:
        read_pty_until(master, output, PROMPT_MARKER, 10)
        os.write(master, b"inspect\r")
        read_pty_until(master, output, b"Let me inspect the repository.", 10)
        os.write(master, b"queued")
        read_pty_until(master, output, b"queued", 5)

        cleared_at = len(output)
        os.kill(process.pid, signal.SIGINT)
        read_pty_frame_without(master, output, b"queued", 5)
        assert b"queued" not in output[cleared_at:]
        assert b"Turn cancelled" not in output[cleared_at:]
        assert process.poll() is None

        os.kill(process.pid, signal.SIGINT)
        read_pty_until(master, output, b"Turn cancelled", 10)
        assert process.poll() is None

        os.kill(process.pid, signal.SIGINT)
        assert process.wait(timeout=10) == 130
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)
