"""Integration tests: drive the liminal binary against in-process API mocks.

Hermetic: mock servers bind ephemeral localhost ports, credentials are
hardcoded fakes (no real API keys involved), stdin is piped. Run via
`pixi run test-integration` or plain `python -m pytest tests/integration`.

The liminal binary is located from LIMINAL_BIN or the default xmake build
output; tests skip with a clear message if it has not been built.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mock_anthropic
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
pytestmark = pytest.mark.skipif(BINARY is None, reason="liminal binary not built (run `xmake build liminal`)")


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


def run_liminal(stdin, env_extra):
    env = os.environ.copy()
    # Fake credentials only - never real keys.
    env.pop("ANTHROPIC_API_KEY", None)
    env.pop("ANTHROPIC_AUTH_TOKEN", None)
    env.pop("OPENAI_API_KEY", None)
    env["LIMINAL_MODEL"] = "test-model"
    env.update(env_extra)
    result = subprocess.run(
        [str(BINARY)], input=stdin, env=env, cwd=REPO_ROOT,
        capture_output=True, text=True, timeout=TIMEOUT,
    )
    assert result.returncode == 0, f"liminal exited {result.returncode}\nstderr: {result.stderr}\nstdout: {result.stdout}"
    return result.stdout


def check(state, expected_log):
    assert not state["errors"], "\n".join(state["errors"])
    assert state["log"] == expected_log, f"scenario order mismatch: {state['log']}"


def test_anthropic_full_cycle(anthropic_mock):
    """429 retry, SSE reassembly, thinking replay, parallel tools, compaction."""
    url, state = anthropic_mock
    out = run_liminal(
        "what directory are we in?\n/compact\nwhat did we find so far?\n/quit\n",
        {"LIMINAL_PROVIDER": "anthropic", "ANTHROPIC_API_KEY": mock_anthropic.API_KEY,
         "ANTHROPIC_BASE_URL": url},
    )
    check(state, ["429", "tools-turn", "continuation", "compact-summarizer", "post-compact"])
    assert "Let me check the current directory." in out  # streamed text reached stdout
    assert "[history compacted]" in out
    assert "Continuing from the compacted context." in out


def test_openai_full_cycle_remote_compact(openai_mock):
    """429 retry, encrypted reasoning replay, parallel tools, native /responses/compact."""
    url, state = openai_mock
    out = run_liminal(
        "check the working directory and readme\n/compact\nwhat did we find?\n/quit\n",
        {"LIMINAL_PROVIDER": "openai", "OPENAI_API_KEY": mock_openai.API_KEY,
         "OPENAI_BASE_URL": url},
    )
    check(state, ["429", "tools-turn", "continuation", "compact-remote", "post-compact"])
    assert "[history compacted]" in out
    assert "Continuing from the compacted context." in out  # encrypted item replayed


def test_openai_gateway_compact_fallback(openai_mock_no_compact):
    """A gateway without /responses/compact pushes onto local summarization."""
    url, state = openai_mock_no_compact
    out = run_liminal(
        "check the working directory and readme\n/compact\n/quit\n",
        {"LIMINAL_PROVIDER": "openai", "OPENAI_API_KEY": mock_openai.API_KEY,
         "OPENAI_BASE_URL": url},
    )
    check(state, ["429", "tools-turn", "continuation", "compact-404", "compact-summarizer"])
    assert "[history compacted]" in out


def test_provider_switch_carries_history(anthropic_mock, openai_mock):
    """/switch keeps neutral history; foreign private state drops silently."""
    anthropic_url, anthropic_state = anthropic_mock
    openai_url, openai_state = openai_mock
    out = run_liminal(
        "what directory are we in?\n/switch openai\ncheck the working directory and readme\n/quit\n",
        {"LIMINAL_PROVIDER": "anthropic",
         "ANTHROPIC_API_KEY": mock_anthropic.API_KEY, "ANTHROPIC_BASE_URL": anthropic_url,
         "OPENAI_API_KEY": mock_openai.API_KEY, "OPENAI_BASE_URL": openai_url},
    )
    check(anthropic_state, ["429", "tools-turn", "continuation"])
    check(openai_state, ["429", "tools-turn", "continuation"])
    assert "[switched to openai:test-model" in out


def test_unknown_provider_switch_is_recoverable(anthropic_mock):
    url, state = anthropic_mock
    out = run_liminal(
        "/switch nonexistent\nwhat directory are we in?\n/quit\n",
        {"LIMINAL_PROVIDER": "anthropic", "ANTHROPIC_API_KEY": mock_anthropic.API_KEY,
         "ANTHROPIC_BASE_URL": url},
    )
    assert "[switch error:" in out
    check(state, ["429", "tools-turn", "continuation"])  # session stayed usable
