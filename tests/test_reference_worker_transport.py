"""PC-only wire and session vectors for the unchanged P4 transport source."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest


REFERENCE_TOOLS = Path(__file__).resolve().parents[1] / "reference/xcompute-probe/tools"
sys.path.insert(0, str(REFERENCE_TOOLS))
from xcp_worker_transport import (  # noqa: E402
    WorkerEndpoint,
    WorkerJsonClient,
    WorkerSession,
    WorkerTransportError,
)


class FakeSocket:
    def __init__(self, chunks: list[bytes]) -> None:
        self.chunks = iter(chunks)
        self.sent = b""
        self.timeout = None

    def __enter__(self) -> "FakeSocket":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def settimeout(self, value: float) -> None:
        self.timeout = value

    def sendall(self, data: bytes) -> None:
        self.sent += data

    def recv(self, _size: int) -> bytes:
        return next(self.chunks, b"")


def client_with_reply(monkeypatch: pytest.MonkeyPatch, *chunks: bytes) -> tuple[WorkerJsonClient, FakeSocket]:
    connection = FakeSocket(list(chunks))
    monkeypatch.setattr("xcp_worker_transport.socket.create_connection", lambda *_args, **_kw: connection)
    return WorkerJsonClient(WorkerEndpoint("127.0.0.1", 8787, 2)), connection


def test_json_line_framing_and_ephemeral_auth(monkeypatch: pytest.MonkeyPatch) -> None:
    client, connection = client_with_reply(monkeypatch, b'{"ok":true,"result":1}\nextra')
    payload = {"command": "ping", "marker": "sample"}
    assert client.request(payload, session_id="s1") == {"ok": True, "result": 1}
    assert payload == {"command": "ping", "marker": "sample"}
    assert connection.sent.endswith(b"\n")
    assert json.loads(connection.sent) == {"command": "ping", "marker": "sample", "session_id": "s1"}
    assert connection.timeout == 2


@pytest.mark.parametrize(
    ("reply", "code"),
    [
        (b"not-json\n", "xcp.agent.worker_response_invalid"),
        (b'{"ok":"yes"}\n', "xcp.agent.worker_envelope_invalid"),
        (b"", "xcp.agent.worker_response_empty"),
    ],
)
def test_transport_rejects_invalid_responses(
    monkeypatch: pytest.MonkeyPatch, reply: bytes, code: str
) -> None:
    client, _ = client_with_reply(monkeypatch, reply)
    with pytest.raises(WorkerTransportError) as caught:
        client.request({"command": "ping"})
    assert caught.value.code == code
    assert caught.value.details["stage"] == "transport"


def test_worker_error_preserved_or_returned(monkeypatch: pytest.MonkeyPatch) -> None:
    response = {"ok": False, "error": {"code": "worker.sample", "message": "sample", "details": {"field": "item"}}}
    client, _ = client_with_reply(monkeypatch, json.dumps(response).encode() + b"\n")
    with pytest.raises(WorkerTransportError) as caught:
        client.request({"command": "ping"})
    assert caught.value.code == "worker.sample"
    assert caught.value.details == {"field": "item"}
    assert caught.value.response == response

    client, _ = client_with_reply(monkeypatch, json.dumps(response).encode() + b"\n")
    assert client.request({"command": "ping"}, allow_error=True) == response


class FakeClient:
    def __init__(self, compatible: bool = True) -> None:
        self.calls: list[tuple[dict[str, object], dict[str, object]]] = []
        self.compatible = compatible

    def request(self, payload: dict[str, object], **kwargs: object) -> dict[str, object]:
        self.calls.append((payload, kwargs))
        if payload["command"] == "negotiate_protocol":
            return {"ok": True, "compatible": self.compatible}
        if payload["command"] == "open_session":
            return {"ok": True, "session_id": "s1"}
        return {"ok": True}


def test_session_negotiates_opens_and_closes() -> None:
    client = FakeClient()
    session = WorkerSession(client, pairing_code="p1")
    with session as worker:
        assert worker.request({"command": "ping"}) == {"ok": True}
    assert [payload["command"] for payload, _ in client.calls] == [
        "negotiate_protocol", "open_session", "ping", "close_session"
    ]
    assert client.calls[1][1] == {"pairing_code": "p1"}
    assert client.calls[2][1] == {"session_id": "s1", "allow_error": False}
    assert session._session_id == ""
    assert session._pairing_code == ""


def test_incompatible_protocol_fails_before_pairing() -> None:
    client = FakeClient(compatible=False)
    with pytest.raises(WorkerTransportError) as caught:
        with WorkerSession(client, pairing_code="p1"):
            pass
    assert caught.value.code == "xcp.agent.protocol_incompatible"
    assert [payload["command"] for payload, _ in client.calls] == ["negotiate_protocol"]
