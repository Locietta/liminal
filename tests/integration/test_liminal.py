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
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mock_anthropic
import mock_codex_auth
import mock_openai

REPO_ROOT = Path(__file__).resolve().parents[2]
TIMEOUT = 120


def find_binary():
    if override := os.environ.get("LIMINAL_BIN"):
        return Path(override)
    exe = "liminal.exe" if os.name == "nt" else "liminal"
    for mode in ("releasedbg", "release"):
        for platform_dir in (REPO_ROOT / "build").glob(f"*/*/{mode}"):
            candidate = platform_dir / exe
            if candidate.exists():
                return candidate
    return None


BINARY = find_binary()
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
openai_mock_no_compact = openai_mock_fixture(compact_404=True)
openai_mock_no_models = openai_mock_fixture(models_status=404)


@pytest.fixture
def codex_auth_mock():
    server, state = mock_codex_auth.make_server()
    import threading

    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}", state
    server.shutdown()


def configured_provider(api, base_url, api_key, models, discover_models=False):
    return {
        "api": api,
        "base_url": base_url,
        "api_key": api_key,
        "discover_models": discover_models,
        "models": models,
    }


def run_liminal(stdin, providers, tmp_path, selector="test-model"):
    providers_file = tmp_path / "providers.json"
    providers_file.write_text(json.dumps({"providers": providers}))
    env = os.environ.copy()
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_AUTH_FILE"] = str(tmp_path / "missing-auth.json")
    env["LIMINAL_MODEL"] = selector
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


def test_anthropic_model_effort_uses_adaptive_thinking(anthropic_mock, tmp_path):
    url, state = anthropic_mock
    out = run_liminal(
        "/model adaptive-model@high\nwhat directory are we in?\n/quit\n",
        {
            "anthropic": configured_provider(
                "anthropic-messages",
                url,
                mock_anthropic.API_KEY,
                [
                    {"id": "test-model"},
                    {
                        "id": "adaptive-model",
                        "reasoning_efforts": ["low", "high"],
                    },
                ],
            )
        },
        tmp_path,
    )
    assert "[model: adaptive-model@high]" in out
    check(state, ["429", "tools-turn", "continuation"])
    assert all(body["model"] == "adaptive-model" for body in state["request_bodies"])
    assert all(
        body["thinking"] == {"type": "adaptive"}
        and body["output_config"] == {"effort": "high"}
        for body in state["request_bodies"]
    )


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


def test_model_catalog_discovers_only_opted_in_providers(
    anthropic_mock, openai_mock, tmp_path
):
    anthropic_url, anthropic_state = anthropic_mock
    openai_url, openai_state = openai_mock
    out = run_liminal(
        "/model\n/quit\n",
        {
            "anthropic": configured_provider(
                "anthropic-messages",
                anthropic_url,
                mock_anthropic.API_KEY,
                [{"id": "test-model"}],
                discover_models=True,
            ),
            "openai": configured_provider(
                "openai-responses",
                openai_url,
                mock_openai.API_KEY,
                [{"id": "configured-openai-model"}],
                discover_models=False,
            ),
        },
        tmp_path,
    )
    assert "anthropic/discovered-anthropic-model - Discovered Anthropic Model" in out
    assert "openai/configured-openai-model" in out
    assert "openai/discovered-openai-model" not in out
    assert anthropic_state["model_requests"] == 2  # startup and explicit /model refresh
    assert openai_state["model_requests"] == 0
    check(anthropic_state, [])
    check(openai_state, [])


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


def test_manual_model_survives_missing_models_endpoint(openai_mock_no_models, tmp_path):
    url, state = openai_mock_no_models
    out = run_liminal(
        "/model gateway-model@medium\n/quit\n",
        {
            "openai": configured_provider(
                "openai-responses",
                url,
                mock_openai.API_KEY,
                [
                    {"id": "test-model"},
                    {"id": "gateway-model", "reasoning_efforts": ["medium"]},
                ],
                discover_models=True,
            )
        },
        tmp_path,
    )
    assert "[model warning: openai: api error (status 404):" in out
    assert "[model: gateway-model@medium]" in out
    assert state["model_requests"] == 2
    check(state, [])


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

    providers_file = tmp_path / "providers.json"
    providers_file.write_text(
        json.dumps(
            {
                "providers": {
                    "codex": {
                        "discover_models": True,
                        "models": [
                            {"id": "gpt-5.6-sol", "name": "Customized Sol"},
                            {"id": "account-specific-model"},
                        ],
                    }
                }
            }
        )
    )
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_CODEX_API_BASE_URL"] = f"{url}/codex"
    env["LIMINAL_MODEL"] = "codex/gpt-5.6-sol"
    session = subprocess.run(
        [str(BINARY)],
        input="/model\n/quit\n",
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
    assert "codex/gpt-5.6-sol - Customized Sol" in session.stdout
    assert "codex/gpt-5.6-terra - GPT-5.6-Terra" in session.stdout
    assert "codex/account-specific-model" in session.stdout
    assert "codex/discovered-codex-model - Discovered Codex Model" in session.stdout
    assert state["log"] == ["start", "poll", "exchange", "refresh", "models", "models"]


@pytest.mark.skipif(
    os.name == "nt", reason="POSIX PTY test; Windows needs the planned ConPTY driver"
)
def test_posix_terminal_session_restores_state():
    """The interactive backend uses and restores an alternate terminal screen."""
    import fcntl
    import pty
    import signal
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
    original = termios.tcgetattr(slave)
    temporary = tempfile.TemporaryDirectory()
    providers_file = Path(temporary.name) / "providers.json"
    providers_file.write_text(
        json.dumps(
            {
                "providers": {
                    "terminal-test": configured_provider(
                        "openai-responses",
                        "http://127.0.0.1:1/v1",
                        "fake-terminal-test-key",
                        [{"id": "test-model"}]
                        + [{"id": f"extra-model-{index}"} for index in range(12)],
                    )
                }
            }
        )
    )
    env = os.environ.copy()
    env["LIMINAL_PROVIDERS_FILE"] = str(providers_file)
    env["LIMINAL_AUTH_FILE"] = str(Path(temporary.name) / "missing-auth.json")
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
        while b" > " not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"prompt did not appear: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"prompt did not appear: {output!r}"
            output.extend(os.read(master, 4096))

        assert output.count(b"\x1b[?1049h") == 1
        assert output.index(b"\x1b[?2004h") < output.index(b"\x1b[?1049h")

        os.write(master, b"/model\r")
        deadline = time.monotonic() + 5
        while b"select with /model" not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"model catalog did not enter the viewport: {output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"model catalog did not enter the viewport: {output!r}"
            output.extend(os.read(master, 4096))

        os.write(master, b"\x1b[5~")
        deadline = time.monotonic() + 5
        while b"history" not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, f"PageUp did not enter transcript history: {output!r}"
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, f"PageUp did not enter transcript history: {output!r}"
            output.extend(os.read(master, 4096))
        os.write(master, b"\x1b[6~")

        os.write(master, b"\x1b[200~a\nb\x1b[201~")
        deadline = time.monotonic() + 5
        while b"a\\nb" not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"multiline paste was not projected into the composer: {output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, (
                f"multiline paste was not projected into the composer: {output!r}"
            )
            output.extend(os.read(master, 4096))

        redraws = output.count(b"\x1b[H")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 50, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        deadline = time.monotonic() + 5
        while output.count(b"\x1b[H") == redraws or b"\x1b[10;" not in output:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (
                f"resize did not redraw the application-owned frame: {output!r}"
            )
            readable, _, _ = select.select([master], [], [], remaining)
            assert readable, (
                f"resize did not redraw the application-owned frame: {output!r}"
            )
            output.extend(os.read(master, 4096))

        # Clear the three pasted code points, then exit normally.
        os.write(master, b"\x7f\x7f\x7f/quit\r")
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
        assert output.count(b"\x1b[?1049l") == 1
        assert output.index(b"\x1b[?1049h") < output.index(b"\x1b[?1049l")
        assert output.index(b"\x1b[?1049l") < output.index(b"\x1b[?2004l")
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)
        temporary.cleanup()


@pytest.mark.skipif(os.name == "nt", reason="POSIX PTY signal test")
def test_posix_terminal_restores_after_interrupt(tmp_path):
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
        while b" > " not in output:
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
