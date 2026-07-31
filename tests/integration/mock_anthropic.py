"""In-process Anthropic Messages API mock for liminal integration tests.

Scenario (self-advancing by call count):
  1. 429 with retry-after       -> client must retry
  2. SSE: thinking + text + two tool_use, split at awkward byte boundaries
  3. continuation               -> asserts tool_results + bit-exact thinking replay
Also answers local-compaction summarizer requests and post-compaction turns,
recognized by content rather than call order.

All assertions record into state["errors"]; the runner fails the scenario if
any are present. Auth is a hardcoded fake: no real credentials involved.
"""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "test-key"


def sse(events):
    return "".join(f"event: {name}\ndata: {json.dumps(payload)}\n\n" for name, payload in events).encode()


def make_server(port=0):
    """Returns (server, state). state: calls, log[], errors[]."""
    state = {"calls": 0, "log": [], "errors": []}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                self.handle_post()
            except AssertionError as error:
                state["errors"].append(f"anthropic mock: {error}")
                self.send_error(500, str(error))

        def send_sse(self, events, req_id, chunk_size=None):
            data = sse(events)
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("request-id", req_id)
            self.end_headers()
            if chunk_size:
                for i in range(0, len(data), chunk_size):
                    self.wfile.write(data[i:i + chunk_size])
                    self.wfile.flush()
            else:
                self.wfile.write(data)

        def send_text_turn(self, text, req_id):
            self.send_sse([
                ("message_start", {"type": "message_start", "message": {
                    "id": req_id, "model": "test-model", "usage": {"input_tokens": 10}}}),
                ("content_block_start", {"type": "content_block_start", "index": 0,
                                         "content_block": {"type": "text", "text": ""}}),
                ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                         "delta": {"type": "text_delta", "text": text}}),
                ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn"},
                                   "usage": {"output_tokens": 12}}),
                ("message_stop", {"type": "message_stop"}),
            ], req_id)

        def handle_post(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            state["calls"] += 1

            assert self.headers["x-api-key"] == API_KEY, "missing/wrong x-api-key"
            assert self.headers["anthropic-version"], "missing anthropic-version header"

            last = body["messages"][-1]
            last_texts = [b.get("text", "") for b in last["content"] if b["type"] == "text"]

            # Local-compaction summarizer request: checkpoint prompt, no tools.
            if any("context checkpoint compaction" in t for t in last_texts):
                assert not body.get("tools"), "summarizer request must not offer tools"
                assert last["role"] == "user"
                assert any(m["role"] == "assistant" for m in body["messages"][:-1]), \
                    "summarizer request lacks transcript"
                self.send_text_turn(
                    "SUMMARY: user asked for cwd; ran Get-Location in the repo; read README.",
                    "req_compact")
                state["log"].append("compact-summarizer")
                return

            # Post-compaction turn: [bridge summary, new prompt], old turn gone.
            first = body["messages"][0]
            first_texts = [b.get("text", "") for b in first["content"] if b["type"] == "text"]
            if first["role"] == "user" and any("[Compacted context]" in t for t in first_texts):
                assert any("SUMMARY:" in t for t in first_texts), "summary missing from bridge"
                assert len(body["messages"]) == 2, f"expected [bridge, prompt], got {len(body['messages'])}"
                flattened = json.dumps(body["messages"])
                assert "tool_use" not in flattened and "thinking" not in flattened, \
                    "old turn leaked past compaction"
                self.send_text_turn("Continuing from the compacted context.", "req_post_compact")
                state["log"].append("post-compact")
                return

            if state["calls"] == 1:
                payload = json.dumps({"type": "error",
                                      "error": {"type": "rate_limit_error", "message": "slow down"}}).encode()
                self.send_response(429)
                self.send_header("retry-after", "1")
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                state["log"].append("429")
                return

            if state["calls"] == 2:
                events = [
                    ("message_start", {"type": "message_start", "message": {
                        "id": "msg_1", "model": "test-model", "usage": {"input_tokens": 10}}}),
                    ("ping", {"type": "ping"}),
                    ("content_block_start", {"type": "content_block_start", "index": 0,
                                             "content_block": {"type": "thinking", "thinking": "", "signature": ""}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                             "delta": {"type": "thinking_delta", "thinking": "I should check the cwd."}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                             "delta": {"type": "signature_delta", "signature": "sig-abc123"}}),
                    ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                    ("content_block_start", {"type": "content_block_start", "index": 1,
                                             "content_block": {"type": "text", "text": ""}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 1,
                                             "delta": {"type": "text_delta", "text": "Let me check the "}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 1,
                                             "delta": {"type": "text_delta", "text": "current directory."}}),
                    ("content_block_stop", {"type": "content_block_stop", "index": 1}),
                    ("content_block_start", {"type": "content_block_start", "index": 2,
                                             "content_block": {"type": "tool_use", "id": "toolu_1",
                                                               "name": "run_command", "input": {}}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 2,
                                             "delta": {"type": "input_json_delta", "partial_json": '{"comm'}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 2,
                                             "delta": {"type": "input_json_delta", "partial_json": 'and": "Get-Lo'}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 2,
                                             "delta": {"type": "input_json_delta", "partial_json": 'cation"}'}}),
                    ("content_block_stop", {"type": "content_block_stop", "index": 2}),
                    ("content_block_start", {"type": "content_block_start", "index": 3,
                                             "content_block": {"type": "tool_use", "id": "toolu_2",
                                                               "name": "read_file", "input": {}}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 3,
                                             "delta": {"type": "input_json_delta", "partial_json": '{"path": "README.md"}'}}),
                    ("content_block_stop", {"type": "content_block_stop", "index": 3}),
                    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
                                       "usage": {"output_tokens": 25}}),
                    ("message_stop", {"type": "message_stop"}),
                ]
                # awkward chunk boundaries exercise SSE reassembly
                self.send_sse(events, "req_tools", chunk_size=7)
                state["log"].append("tools-turn")
                return

            # Continuation: both tool_results in one user message, in order,
            # with the thinking block replayed bit-exact first.
            assert last["role"] == "user", f"expected user tool_result message, got {last['role']}"
            assert len(last["content"]) == 2, f"expected 2 tool_results, got {len(last['content'])}"
            cmd_result, file_result = last["content"]
            assert cmd_result["type"] == "tool_result" and cmd_result["tool_use_id"] == "toolu_1"
            assert "exit_code: 0" in cmd_result["content"], "run_command result missing exit code"
            assert not cmd_result["is_error"]
            assert file_result["type"] == "tool_result" and file_result["tool_use_id"] == "toolu_2"
            assert "Liminal" in file_result["content"], "read_file result missing README content"
            assistant = body["messages"][-2]
            assert assistant["role"] == "assistant"
            thinking = assistant["content"][0]
            assert thinking["type"] == "thinking", f"thinking not replayed first, got {thinking['type']}"
            assert thinking["thinking"] == "I should check the cwd."
            assert thinking["signature"] == "sig-abc123", "thinking signature not bit-exact"
            self.send_text_turn("The working directory is the liminal repo.", "req_final")
            state["log"].append("continuation")

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    return server, state
