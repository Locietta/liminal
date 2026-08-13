"""In-process OpenAI Responses API mock for liminal integration tests.

Scenario (self-advancing by call count):
  1. 429                        -> client must retry
  2. SSE: reasoning + text + two function calls, awkward chunk boundaries
  3. continuation               -> asserts function outputs + bit-exact
                                   encrypted-reasoning replay
Answers /responses/compact natively, or with 404 when compact_404=True
(simulating an OpenAI-compatible gateway without the proprietary endpoint,
which must push liminal onto the local-summarization fallback). Summarizer
requests are recognized by content.

All assertions record into state["errors"]; auth is a hardcoded fake.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "test-key"


def sse(events):
    return "".join(
        f"event: {name}\ndata: {json.dumps({'type': name, **payload})}\n\n"
        for name, payload in events
    ).encode()


def make_server(port=0, compact_404=False, gate_tools_turn=False):
    """Returns (server, state). state: calls, log[], errors[]."""
    state = {
        "calls": 0,
        "log": [],
        "errors": [],
        "request_bodies": [],
        "stream_paused": threading.Event(),
        "stream_release": threading.Event(),
    }

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                self.handle_post()
            except AssertionError as error:
                state["errors"].append(f"openai mock: {error}")
                self.send_error(500, str(error))
            except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                # Cancellation tests deliberately close an active response.
                pass

        def send_json(self, status, payload, req_id):
            data = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("x-request-id", req_id)
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def send_sse(self, events, req_id, chunk_size=None, pause_after=None):
            data = sse(events)
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("x-request-id", req_id)
            self.end_headers()
            if chunk_size:
                pause_offset = None
                if pause_after is not None:
                    marker_offset = data.index(pause_after)
                    pause_offset = data.index(b"\n\n", marker_offset) + 2
                for i in range(0, len(data), chunk_size):
                    end = i + chunk_size
                    self.wfile.write(data[i:end])
                    self.wfile.flush()
                    if pause_offset is not None and end >= pause_offset:
                        state["stream_paused"].set()
                        state["stream_release"].wait()
                        pause_offset = None
            else:
                self.wfile.write(data)

        def send_text_turn(self, text, req_id, msg_id):
            message = {
                "type": "message",
                "id": msg_id,
                "role": "assistant",
                "status": "completed",
                "content": [{"type": "output_text", "text": text, "annotations": []}],
            }
            response = {
                "id": req_id,
                "model": "test-model",
                "status": "completed",
                "usage": {"input_tokens": 30, "output_tokens": 12},
            }
            self.send_sse(
                [
                    (
                        "response.created",
                        {"response": {**response, "status": "in_progress"}},
                    ),
                    (
                        "response.output_text.delta",
                        {
                            "output_index": 0,
                            "content_index": 0,
                            "item_id": msg_id,
                            "delta": text,
                        },
                    ),
                    ("response.output_item.done", {"output_index": 0, "item": message}),
                    ("response.completed", {"response": response}),
                ],
                req_id,
            )

        def handle_post(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            state["request_bodies"].append(body)
            assert self.headers["Authorization"] == f"Bearer {API_KEY}", (
                "missing/wrong bearer token"
            )

            if self.path == "/v1/responses/compact":
                if compact_404:
                    self.send_json(
                        404,
                        {
                            "error": {
                                "message": "Unknown request URL",
                                "type": "invalid_request_error",
                            }
                        },
                        "req_compact_404",
                    )
                    state["log"].append("compact-404")
                    return
                assert body["model"] == "test-model"
                assert body["instructions"], "compact request missing instructions"
                assert any(item["type"] == "message" for item in body["input"]), (
                    "compact request missing transcript"
                )
                self.send_json(
                    200,
                    {
                        "output": [
                            {
                                "type": "compaction",
                                "id": "cmp_1",
                                "encrypted_content": "encrypted-compaction",
                                "created_by": "test",
                            }
                        ]
                    },
                    "req_compact",
                )
                state["log"].append("compact-remote")
                return

            assert self.path == "/v1/responses", f"unexpected path {self.path}"
            state["calls"] += 1
            assert body["store"] is False, "store must be false (stateless/ZDR)"
            assert "reasoning.encrypted_content" in body["include"]
            assert body.get("instructions") == "Test system policy.", (
                "system instructions must be lifted into the top-level field"
            )
            assert not any(
                item.get("role") == "system"
                for item in body["input"]
                if item["type"] == "message"
            ), "the Codex backend rejects system-role input items"
            developer = body["input"][0]
            assert developer == {
                "type": "message",
                "role": "developer",
                "content": [{"type": "input_text", "text": "Test developer policy."}],
            }

            # Local-compaction summarizer request: checkpoint prompt, no tools.
            if "context checkpoint compaction" in json.dumps(body["input"]):
                assert not body.get("tools"), "summarizer request must not offer tools"
                self.send_text_turn(
                    "SUMMARY: user asked for cwd; ran pwd; read README.",
                    "req_sum",
                    "msg_sum",
                )
                state["log"].append("compact-summarizer")
                return

            # Post-compaction turn after a remote compact: the encrypted
            # compaction item must be replayed verbatim.
            if any(item["type"] == "compaction" for item in body["input"]):
                compaction = next(
                    item for item in body["input"] if item["type"] == "compaction"
                )
                assert compaction["encrypted_content"] == "encrypted-compaction", (
                    "compaction item not replayed bit-exact"
                )
                self.send_text_turn(
                    "Continuing from the compacted context.",
                    "req_post_compact",
                    "msg_post",
                )
                state["log"].append("post-compact")
                return

            assert all(
                tool["type"] in {"function", "web_search"}
                for tool in body.get("tools", [])
            )
            functions = [
                tool for tool in body.get("tools", []) if tool["type"] == "function"
            ]
            assert all(tool["strict"] is False for tool in functions), (
                "function tools must stay non-strict so optional parameters remain valid"
            )
            assert all(
                tool["parameters"]["additionalProperties"] is False
                for tool in functions
            )

            if state["calls"] == 1:
                payload = {
                    "error": {
                        "type": "rate_limit_error",
                        "code": "rate_limit_exceeded",
                        "message": "slow down",
                    }
                }
                data = json.dumps(payload).encode()
                self.send_response(429)
                self.send_header("retry-after", "0")
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                state["log"].append("429")
                return

            if state["calls"] == 2:
                reasoning = {
                    "type": "reasoning",
                    "id": "rs_1",
                    "summary": [],
                    "content": [],
                    "encrypted_content": "encrypted-reasoning",
                    "status": "completed",
                }
                message = {
                    "type": "message",
                    "id": "msg_1",
                    "role": "assistant",
                    "status": "completed",
                    "content": [
                        {
                            "type": "output_text",
                            "text": "Let me inspect the repository.",
                            "annotations": [],
                        }
                    ],
                }
                command = {
                    "type": "function_call",
                    "id": "fc_1",
                    "call_id": "call_1",
                    "name": "exec_command",
                    "arguments": '{"cmd":"pwd"}',
                    "status": "completed",
                }
                readme = {
                    "type": "function_call",
                    "id": "fc_2",
                    "call_id": "call_2",
                    "name": "read_file",
                    "arguments": '{"path":"README.md"}',
                    "status": "completed",
                }
                response = {
                    "id": "resp_1",
                    "model": "test-model",
                    "status": "completed",
                    "usage": {
                        "input_tokens": 20,
                        "output_tokens": 15,
                        "input_tokens_details": {"cached_tokens": 0},
                        "output_tokens_details": {"reasoning_tokens": 5},
                    },
                }
                events = [
                    (
                        "response.created",
                        {"response": {**response, "status": "in_progress"}},
                    ),
                    (
                        "response.output_item.done",
                        {"output_index": 0, "item": reasoning},
                    ),
                    (
                        "response.output_text.delta",
                        {
                            "output_index": 1,
                            "content_index": 0,
                            "item_id": "msg_1",
                            "delta": "Let me inspect the repository.",
                        },
                    ),
                    ("response.output_item.done", {"output_index": 1, "item": message}),
                    ("response.output_item.done", {"output_index": 2, "item": command}),
                    ("response.output_item.done", {"output_index": 3, "item": readme}),
                    ("response.completed", {"response": response}),
                ]
                self.send_sse(
                    events,
                    "req_tools",
                    chunk_size=7,
                    pause_after=(
                        b"Let me inspect the repository." if gate_tools_turn else None
                    ),
                )
                state["log"].append("tools-turn")
                return

            # Continuation: encrypted reasoning + calls + outputs replayed.
            items = body["input"]
            reasoning = next(item for item in items if item["type"] == "reasoning")
            assert reasoning["encrypted_content"] == "encrypted-reasoning", (
                "encrypted reasoning not replayed bit-exact"
            )
            calls = [item for item in items if item["type"] == "function_call"]
            outputs = [item for item in items if item["type"] == "function_call_output"]
            # Tail-match: model changes may have carried earlier neutral rounds in.
            assert [item["call_id"] for item in calls][-2:] == ["call_1", "call_2"]
            assert [item["call_id"] for item in outputs][-2:] == ["call_1", "call_2"]
            outputs = outputs[-2:]
            assert "exit_code: 0" in outputs[0]["output"], (
                "exec_command output missing exit code"
            )
            assert "Liminal" in outputs[1]["output"], (
                "read_file output missing README content"
            )
            self.send_text_turn(
                "The working directory is the liminal repository.", "req_final", "msg_2"
            )
            state["log"].append("continuation")

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    return server, state
