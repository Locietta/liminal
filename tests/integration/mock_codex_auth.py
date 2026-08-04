"""In-process OpenAI Codex device OAuth mock."""

import base64
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


def encode_segment(value):
    raw = json.dumps(value, separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


ACCESS_TOKEN = ".".join(
    [
        encode_segment({"alg": "none"}),
        encode_segment(
            {"https://api.openai.com/auth": {"chatgpt_account_id": "account-123"}}
        ),
        "signature",
    ]
)


def sse(events):
    return "".join(
        f"event: {event}\ndata: {json.dumps({'type': event, **payload})}\n\n"
        for event, payload in events
    ).encode()


def make_server(port=0):
    state = {"log": [], "errors": []}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                body = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/codex/responses":
                    request = json.loads(body)
                    assert self.headers["authorization"] == f"Bearer {ACCESS_TOKEN}"
                    assert self.headers["chatgpt-account-id"] == "account-123"
                    assert self.headers["originator"] == "liminal"
                    assert "max_output_tokens" not in request
                    assert request["model"] == "gpt-5.6-sol"
                    assert request["stream"] is True
                    assert request["store"] is False
                    text = "CODEX_STREAM_OK"
                    message = {
                        "type": "message",
                        "id": "msg_codex",
                        "role": "assistant",
                        "status": "completed",
                        "content": [
                            {
                                "type": "output_text",
                                "text": text,
                                "annotations": [],
                            }
                        ],
                    }
                    response = {
                        "id": "resp_codex",
                        "model": "gpt-5.6-sol",
                        "status": "completed",
                    }
                    data = sse(
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
                                    "item_id": "msg_codex",
                                    "delta": text,
                                },
                            ),
                            (
                                "response.output_item.done",
                                {"output_index": 0, "item": message},
                            ),
                            ("response.completed", {"response": response}),
                        ]
                    )
                    state["log"].append("responses")
                    self.send_response(200)
                    self.send_header("x-request-id", "req_codex")
                    # The subscription endpoint currently returns valid SSE
                    # without a Content-Type header.
                    self.end_headers()
                    self.wfile.write(data)
                    return
                if self.path == "/api/accounts/deviceauth/usercode":
                    assert json.loads(body) == {
                        "client_id": "app_EMoamEEZ73f0CkXaXp7hrann"
                    }
                    state["log"].append("start")
                    self.send_json(
                        {
                            "device_auth_id": "device-123",
                            "user_code": "TEST-CODE",
                            "interval": "1",
                        }
                    )
                    return
                if self.path == "/api/accounts/deviceauth/token":
                    assert json.loads(body) == {
                        "device_auth_id": "device-123",
                        "user_code": "TEST-CODE",
                    }
                    state["log"].append("poll")
                    self.send_json(
                        {
                            "authorization_code": "authorization-code",
                            "code_verifier": "code-verifier",
                        }
                    )
                    return
                assert self.path == "/oauth/token", f"unexpected path {self.path}"
                form = parse_qs(body.decode())
                assert form["client_id"] == ["app_EMoamEEZ73f0CkXaXp7hrann"]
                if form["grant_type"] == ["refresh_token"]:
                    assert form["refresh_token"] == ["refresh-token"]
                    state["log"].append("refresh")
                else:
                    assert form["grant_type"] == ["authorization_code"]
                    assert form["code"] == ["authorization-code"]
                    assert form["code_verifier"] == ["code-verifier"]
                    state["log"].append("exchange")
                self.send_json(
                    {
                        "access_token": ACCESS_TOKEN,
                        "refresh_token": "refresh-token",
                        "expires_in": 3600,
                    }
                )
            except AssertionError as error:
                state["errors"].append(str(error))
                self.send_error(500, str(error))

        def do_GET(self):
            try:
                parsed = urlparse(self.path)
                assert parsed.path == "/codex/models", f"unexpected path {self.path}"
                assert parse_qs(parsed.query) == {"client_version": ["0.1.0"]}
                assert self.headers["authorization"] == f"Bearer {ACCESS_TOKEN}"
                assert self.headers["chatgpt-account-id"] == "account-123"
                assert self.headers["originator"] == "liminal"
                state["log"].append("models")
                self.send_json(
                    {
                        "models": [
                            {
                                "slug": "discovered-codex-model",
                                "display_name": "Discovered Codex Model",
                            }
                        ]
                    }
                )
            except AssertionError as error:
                state["errors"].append(str(error))
                self.send_error(500, str(error))

        def send_json(self, value):
            data = json.dumps(value).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    return ThreadingHTTPServer(("127.0.0.1", port), Handler), state
