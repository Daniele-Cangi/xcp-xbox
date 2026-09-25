#!/usr/bin/env python3
"""Small protocol-0.73 JSON-line transport for external XCP agents.

Authentication material is accepted only in memory and is never serialized by
this module. Higher-level tools must persist receipts without session ids,
pairing codes, trust signatures, or controller private material.
"""

from __future__ import annotations

import json
import socket
from dataclasses import dataclass
from typing import Any


MAX_RESPONSE_BYTES = 64 * 1024 * 1024


class WorkerTransportError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        details: dict[str, Any] | None = None,
        response: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details or {}
        self.response = response or {}

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": False,
            "schema_version": "xcp-agent-worker-error-v1",
            "error": {
                "code": self.code,
                "message": self.message,
                "details": self.details,
            },
        }


@dataclass(frozen=True)
class WorkerEndpoint:
    host: str
    port: int = 8787
    timeout_seconds: float = 15.0

    def __post_init__(self) -> None:
        if not self.host or len(self.host) > 255:
            raise ValueError("worker host must be a non-empty bounded value")
        if self.port < 1 or self.port > 65535:
            raise ValueError("worker port is outside 1..65535")
        if self.timeout_seconds <= 0 or self.timeout_seconds > 300:
            raise ValueError("worker timeout is outside (0, 300]")


class WorkerJsonClient:
    def __init__(self, endpoint: WorkerEndpoint) -> None:
        self.endpoint = endpoint

    def request(
        self,
        payload: dict[str, Any],
        *,
        session_id: str = "",
        pairing_code: str = "",
        allow_error: bool = False,
    ) -> dict[str, Any]:
        if not isinstance(payload, dict) or not isinstance(
            payload.get("command"), str
        ):
            raise ValueError("worker payload requires a string command")
        if session_id and pairing_code:
            raise ValueError("provide either session_id or pairing_code, not both")

        request = dict(payload)
        if session_id:
            request["session_id"] = session_id
        elif pairing_code:
            request["pairing_code"] = pairing_code

        encoded = (
            json.dumps(
                request,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            + "\n"
        ).encode("utf-8")

        try:
            with socket.create_connection(
                (self.endpoint.host, self.endpoint.port),
                timeout=self.endpoint.timeout_seconds,
            ) as connection:
                connection.settimeout(self.endpoint.timeout_seconds)
                connection.sendall(encoded)
                response_bytes = self._read_line(connection)
        except OSError as exc:
            raise WorkerTransportError(
                "xcp.agent.worker_unreachable",
                "The worker endpoint could not be reached.",
                details={
                    "schema_version": "xcp-agent-error-details-v1",
                    "stage": "transport",
                    "field": "endpoint",
                    "expected": "reachable TCP worker returning one JSON line",
                    "actual": type(exc).__name__,
                    "correction": (
                        "launch XCP WORKER, verify its current address and port, "
                        "then retry outside a socket-restricted sandbox"
                    ),
                    "retryable": True,
                },
            ) from exc

        try:
            response = json.loads(response_bytes)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise WorkerTransportError(
                "xcp.agent.worker_response_invalid",
                "The worker returned a non-JSON response.",
                details={
                    "schema_version": "xcp-agent-error-details-v1",
                    "stage": "transport",
                    "field": "response",
                    "expected": "one UTF-8 JSON object",
                    "actual": type(exc).__name__,
                    "correction": "verify worker protocol compatibility",
                    "retryable": False,
                },
            ) from exc

        if not isinstance(response, dict) or not isinstance(
            response.get("ok"), bool
        ):
            raise WorkerTransportError(
                "xcp.agent.worker_envelope_invalid",
                "The worker response lacks a Boolean ok field.",
                details={
                    "schema_version": "xcp-agent-error-details-v1",
                    "stage": "transport",
                    "field": "ok",
                    "expected": "Boolean",
                    "actual": type(response.get("ok")).__name__
                    if isinstance(response, dict)
                    else type(response).__name__,
                    "correction": "fail closed and negotiate a compatible worker",
                    "retryable": False,
                },
            )

        if not response["ok"] and not allow_error:
            error = response.get("error")
            if not isinstance(error, dict):
                error = {}
            code = error.get("code")
            message = error.get("message")
            details = error.get("details")
            raise WorkerTransportError(
                code if isinstance(code, str) and code else "worker.unknown",
                message
                if isinstance(message, str) and message
                else "Worker command failed.",
                details=details if isinstance(details, dict) else {},
                response=response,
            )
        return response

    @staticmethod
    def _read_line(connection: socket.socket) -> bytes:
        chunks: list[bytes] = []
        received = 0
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            newline = chunk.find(b"\n")
            if newline >= 0:
                chunks.append(chunk[:newline])
                received += newline
                if received > MAX_RESPONSE_BYTES:
                    raise WorkerTransportError(
                        "xcp.agent.worker_response_too_large",
                        "The worker response exceeded the external client ceiling.",
                        details={
                            "schema_version": "xcp-agent-error-details-v1",
                            "stage": "transport",
                            "field": "response_bytes",
                            "expected": f"at most {MAX_RESPONSE_BYTES}",
                            "actual": str(received),
                            "correction": (
                                "use bounded artifact retrieval for large data"
                            ),
                            "retryable": False,
                        },
                    )
                break
            chunks.append(chunk)
            received += len(chunk)
            if received > MAX_RESPONSE_BYTES:
                raise WorkerTransportError(
                    "xcp.agent.worker_response_too_large",
                    "The worker response exceeded the external client ceiling.",
                    details={
                        "schema_version": "xcp-agent-error-details-v1",
                        "stage": "transport",
                        "field": "response_bytes",
                        "expected": f"at most {MAX_RESPONSE_BYTES}",
                        "actual": str(received),
                        "correction": "use bounded artifact retrieval for large data",
                        "retryable": False,
                    },
                )
        if not chunks:
            raise WorkerTransportError(
                "xcp.agent.worker_response_empty",
                "The worker returned an empty response.",
                details={
                    "schema_version": "xcp-agent-error-details-v1",
                    "stage": "transport",
                    "field": "response",
                    "expected": "one JSON line",
                    "actual": "empty",
                    "correction": "verify worker readiness and retry",
                    "retryable": True,
                },
            )
        return b"".join(chunks)


class WorkerSession:
    """Ephemeral session wrapper. Session material never leaves this object."""

    def __init__(
        self,
        client: WorkerJsonClient,
        *,
        pairing_code: str = "",
        session_id: str = "",
        ttl_seconds: int = 1800,
    ) -> None:
        if bool(pairing_code) == bool(session_id):
            raise ValueError("provide exactly one of pairing_code or session_id")
        if ttl_seconds < 30 or ttl_seconds > 3600:
            raise ValueError("session TTL is outside 30..3600")
        self.client = client
        self._pairing_code = pairing_code
        self._session_id = session_id
        self._ttl_seconds = ttl_seconds
        self._owns_session = False

    def __enter__(self) -> "WorkerSession":
        negotiation = self.client.request(
            {
                "command": "negotiate_protocol",
                "sdk_protocol_min": "0.73",
                "sdk_protocol_max": "0.73",
                "sdk_contract_version": "1.0",
            }
        )
        if negotiation.get("compatible") is not True:
            raise WorkerTransportError(
                "xcp.agent.protocol_incompatible",
                "The worker protocol is outside the C5 SDK contract.",
                details={
                    "schema_version": "xcp-agent-error-details-v1",
                    "stage": "negotiate",
                    "field": "compatible",
                    "expected": "true for protocol 0.73",
                    "actual": str(negotiation.get("compatible")),
                    "correction": "use a compatible worker package or agent kit",
                    "retryable": False,
                },
            )
        if not self._session_id:
            opened = self.client.request(
                {
                    "command": "open_session",
                    "ttl_seconds": self._ttl_seconds,
                },
                pairing_code=self._pairing_code,
            )
            session_id = opened.get("session_id")
            if not isinstance(session_id, str) or not session_id:
                raise WorkerTransportError(
                    "xcp.agent.session_invalid",
                    "The worker did not return a usable session id.",
                )
            self._session_id = session_id
            self._owns_session = True
        return self

    def request(
        self,
        payload: dict[str, Any],
        *,
        allow_error: bool = False,
    ) -> dict[str, Any]:
        return self.client.request(
            payload,
            session_id=self._session_id,
            allow_error=allow_error,
        )

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if self._owns_session and self._session_id:
            try:
                try:
                    self.client.request(
                        {"command": "close_session"},
                        session_id=self._session_id,
                        allow_error=True,
                    )
                except WorkerTransportError:
                    # Session close is cleanup. Never replace the primary
                    # lifecycle result with a best-effort close failure.
                    pass
            finally:
                self._session_id = ""
                self._pairing_code = ""
