#!/usr/bin/env python3
"""Narrow worker-SDK bridge for the XCP Studio WinUI process.

The bridge owns no creative lifecycle rules. It negotiates the published
worker SDK boundary, injects only ephemeral authentication from the existing
C5 environment contract, and forwards the public creative commands selected
by the Studio contract.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any, Callable

from xcp_agent_lifecycle import (
    AgentLifecycleError,
    _auth_from_environment,
    discover_host,
)
from xcp_worker_transport import (
    WorkerEndpoint,
    WorkerJsonClient,
    WorkerSession,
    WorkerTransportError,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAX_REQUEST_BYTES = 2 * 1024 * 1024
REQUEST_SCHEMA = "worker-sdk-request-v1"
RESULT_SCHEMA = "worker-sdk-result-v1"
DESCRIPTION_SCHEMA = "xcp-studio-live-adapter-description-v1"
CREATIVE_COMMANDS = (
    "describe_creative_host",
    "prepare_creative_install",
    "commit_creative_install",
    "activate_creative_install",
    "launch_creative_project",
    "observe_creative_foreground",
    "capture_creative_frame",
    "rollback_creative_activation",
    "remove_creative_install",
)
SECRET_KEY_PATTERN = re.compile(
    r"(?:^|_)(?:pairing(?:_code)?|session(?:_id)?|token|cookie|"
    r"authorization|secret|password|credential|api_key|auth_key|"
    r"private_key|secret_key|key_path|signature|nonce)(?:$|_)",
    re.IGNORECASE,
)


class StudioAdapterError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        stage: str,
        field: str = "",
        expected: str = "",
        actual: str = "",
        correction: str = "",
        retryable: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = {
            "schema_version": "xcp-studio-error-details-v1",
            "stage": stage,
            "field": field,
            "expected": expected,
            "actual": actual,
            "correction": correction,
            "retryable": retryable,
        }


def describe() -> dict[str, Any]:
    return {
        "ok": True,
        "schema_version": DESCRIPTION_SCHEMA,
        "contract_id": "C4B_STUDIO_SHELL_AND_LIVE_CONTRACT_ADAPTER_V1",
        "request_schema": REQUEST_SCHEMA,
        "result_schema": RESULT_SCHEMA,
        "worker_sdk_contract": "schemas/worker-sdk-contract-v1.json",
        "creative_commands": list(CREATIVE_COMMANDS),
        "transport": {
            "framing": "single_json_document_over_stdin_stdout",
            "shell_used": False,
            "bootstrap": "xcp_worker_transport.WorkerSession",
        },
        "authentication": {
            "pairing_code_environment": "XCP_PAIRING_CODE",
            "session_id_environment": "XCP_SESSION_ID",
            "serialized_in_request": False,
            "persisted": False,
        },
        "authority": {
            "portable_lifecycle": "tools/xcp_agent_lifecycle.py",
            "worker_private_rules": False,
            "message_branching": False,
        },
    }


def _adapter_error(error: StudioAdapterError, request: Any = None) -> dict[str, Any]:
    request_id = ""
    command = ""
    if isinstance(request, dict):
        request_id = str(request.get("request_id", ""))
        command = str(request.get("command", ""))
    return {
        "ok": False,
        "schema_version": RESULT_SCHEMA,
        "request_id": request_id,
        "command": command,
        "protocol_version": "0.73",
        "error": {
            "code": error.code,
            "message": error.message,
            "details": error.details,
        },
    }


def _external_error(
    error: AgentLifecycleError | WorkerTransportError,
    request: dict[str, Any],
) -> dict[str, Any]:
    response = error.response if isinstance(error, WorkerTransportError) else {}
    protocol_version = response.get("protocol_version", "0.73")
    return {
        "ok": False,
        "schema_version": RESULT_SCHEMA,
        "request_id": request["request_id"],
        "command": request["command"],
        "protocol_version": (
            protocol_version if isinstance(protocol_version, str) else "0.73"
        ),
        "error": {
            "code": error.code,
            "message": error.message,
            "details": error.details,
        },
    }


def _scan_secret_keys(value: Any, path: tuple[str, ...] = ()) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str):
                raise StudioAdapterError(
                    "xcp.studio.request_invalid",
                    "The Studio SDK request contains a non-string object key.",
                    stage="request",
                    field=".".join(path),
                    expected="string JSON object keys",
                    actual=type(key).__name__,
                    correction="emit a canonical JSON object",
                )
            child_path = (*path, key)
            if SECRET_KEY_PATTERN.search(key):
                raise StudioAdapterError(
                    "xcp.studio.secret_material_forbidden",
                    "Authentication or credential material is forbidden in Studio requests.",
                    stage="request",
                    field=".".join(child_path),
                    expected="ephemeral authentication supplied only by process environment",
                    actual="secret-bearing field name",
                    correction="remove the field and use XCP_PAIRING_CODE xor XCP_SESSION_ID",
                )
            _scan_secret_keys(child, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _scan_secret_keys(child, (*path, str(index)))


def validate_request(document: Any) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise StudioAdapterError(
            "xcp.studio.request_invalid",
            "The Studio SDK request must be a JSON object.",
            stage="request",
            field="$",
            expected="worker-sdk-request-v1 object",
            actual=type(document).__name__,
            correction="emit the published SDK request envelope",
        )
    expected_keys = {"schema_version", "request_id", "command", "payload"}
    if set(document) != expected_keys:
        raise StudioAdapterError(
            "xcp.studio.request_invalid",
            "The Studio SDK request fields do not match the published envelope.",
            stage="request",
            field="$",
            expected="schema_version, request_id, command, payload",
            actual=",".join(sorted(str(key) for key in document)),
            correction="remove private fields and emit the exact SDK envelope",
        )
    if document["schema_version"] != REQUEST_SCHEMA:
        raise StudioAdapterError(
            "xcp.studio.request_schema_unsupported",
            "The Studio SDK request schema is unsupported.",
            stage="request",
            field="schema_version",
            expected=REQUEST_SCHEMA,
            actual=str(document["schema_version"]),
            correction="use the published worker SDK request schema",
        )
    request_id = document["request_id"]
    if not isinstance(request_id, str) or not request_id or len(request_id) > 128:
        raise StudioAdapterError(
            "xcp.studio.request_invalid",
            "The Studio request id is missing or unbounded.",
            stage="request",
            field="request_id",
            expected="1..128 character string",
            actual=type(request_id).__name__,
            correction="generate a bounded opaque request id",
        )
    command = document["command"]
    if command not in CREATIVE_COMMANDS:
        raise StudioAdapterError(
            "xcp.studio.command_not_admitted",
            "The command is outside the narrow Studio creative surface.",
            stage="request",
            field="command",
            expected="one published C4B creative command",
            actual=str(command),
            correction="select a command from the discovered Studio adapter profile",
        )
    if not isinstance(document["payload"], dict):
        raise StudioAdapterError(
            "xcp.studio.request_invalid",
            "The Studio command payload must be a JSON object.",
            stage="request",
            field="payload",
            expected="JSON object",
            actual=type(document["payload"]).__name__,
            correction="emit command fields as a structured object",
        )
    if "command" in document["payload"]:
        raise StudioAdapterError(
            "xcp.studio.request_invalid",
            "The command cannot be overridden inside the Studio payload.",
            stage="request",
            field="payload.command",
            expected="command selected only by the SDK envelope",
            actual=str(document["payload"]["command"]),
            correction="remove payload.command and retry the admitted operation",
        )
    _scan_secret_keys(document["payload"], ("payload",))
    return document


def _supported_commands(runtime: dict[str, Any]) -> set[str]:
    commands = runtime.get("supported_commands")
    if not isinstance(commands, list) or not all(
        isinstance(item, str) for item in commands
    ):
        raise StudioAdapterError(
            "xcp.studio.runtime_description_invalid",
            "describe_runtime did not publish a usable command list.",
            stage="discover",
            field="supported_commands",
            expected="array of command identifiers",
            actual=type(commands).__name__,
            correction="use a worker compatible with WORKER_SDK_CONTRACT_V1",
        )
    return set(commands)


def invoke_document(
    document: Any,
    *,
    endpoint: WorkerEndpoint,
    pairing_code: str,
    session_id: str,
    client_factory: Callable[[WorkerEndpoint], Any] = WorkerJsonClient,
    session_factory: Callable[..., Any] = WorkerSession,
) -> dict[str, Any]:
    request = validate_request(document)
    command = request["command"]
    payload = request["payload"]
    try:
        with session_factory(
            client_factory(endpoint),
            pairing_code=pairing_code,
            session_id=session_id,
        ) as worker:
            if command == "describe_creative_host":
                runtime, profile = discover_host(worker)
                if command not in _supported_commands(runtime):
                    raise StudioAdapterError(
                        "xcp.studio.command_not_supported",
                        "The connected worker does not publish the requested command.",
                        stage="discover",
                        field="supported_commands",
                        expected=command,
                        actual="absent",
                        correction="connect to a compatible Creative Host",
                    )
                raw_response = worker.request({"command": command}, allow_error=True)
                if raw_response.get("ok") is True:
                    raw_response = dict(raw_response)
                    raw_response["creative_host"] = profile
            else:
                runtime = worker.request({"command": "describe_runtime"})
                if command not in _supported_commands(runtime):
                    raise StudioAdapterError(
                        "xcp.studio.command_not_supported",
                        "The connected worker does not publish the requested command.",
                        stage="discover",
                        field="supported_commands",
                        expected=command,
                        actual="absent",
                        correction="connect to a compatible Creative Host",
                    )
                raw_response = worker.request(
                    {"command": command, **payload},
                    allow_error=True,
                )
    except StudioAdapterError:
        raise
    except (AgentLifecycleError, WorkerTransportError) as error:
        return _external_error(error, request)

    protocol_version = raw_response.get(
        "protocol_version",
        runtime.get("protocol_version", "0.73"),
    )
    if raw_response.get("ok") is not True:
        error = raw_response.get("error")
        if not isinstance(error, dict):
            raise StudioAdapterError(
                "xcp.studio.worker_envelope_invalid",
                "The failed worker response contains no structured error.",
                stage="response",
                field="error",
                expected="error.code plus error.details",
                actual=type(error).__name__,
                correction="fail closed and repair the worker SDK projection",
            )
        return {
            "ok": False,
            "schema_version": RESULT_SCHEMA,
            "request_id": request["request_id"],
            "command": command,
            "protocol_version": str(protocol_version),
            "error": error,
        }
    return {
        "ok": True,
        "schema_version": RESULT_SCHEMA,
        "request_id": request["request_id"],
        "command": command,
        "protocol_version": str(protocol_version),
        "payload": raw_response,
    }


def _read_request() -> Any:
    data = sys.stdin.buffer.read(MAX_REQUEST_BYTES + 1)
    if len(data) > MAX_REQUEST_BYTES:
        raise StudioAdapterError(
            "xcp.studio.request_too_large",
            "The Studio SDK request exceeded its PC-side ceiling.",
            stage="request",
            field="bytes",
            expected=f"at most {MAX_REQUEST_BYTES}",
            actual=str(len(data)),
            correction="use the worker artifact lifecycle for large content",
        )
    try:
        return json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise StudioAdapterError(
            "xcp.studio.request_json_invalid",
            "The Studio SDK request is not valid UTF-8 JSON.",
            stage="request",
            field="$",
            expected="one UTF-8 JSON document",
            actual=type(error).__name__,
            correction="serialize the published SDK request envelope",
        ) from error


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Bridge XCP Studio to the published worker SDK contract."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("describe")
    invoke = subparsers.add_parser("invoke")
    invoke.add_argument("--device-address", required=True)
    invoke.add_argument("--port", type=int, default=8787)
    invoke.add_argument("--timeout-seconds", type=float, default=15.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    request: Any = None
    try:
        if args.command == "describe":
            result = describe()
        else:
            request = _read_request()
            pairing_code, session_id = _auth_from_environment()
            result = invoke_document(
                request,
                endpoint=WorkerEndpoint(
                    args.device_address,
                    args.port,
                    args.timeout_seconds,
                ),
                pairing_code=pairing_code,
                session_id=session_id,
            )
    except StudioAdapterError as error:
        result = _adapter_error(error, request)
    except (AgentLifecycleError, WorkerTransportError) as error:
        if isinstance(request, dict):
            result = _external_error(error, request)
        else:
            result = {
                "ok": False,
                "schema_version": RESULT_SCHEMA,
                "request_id": "",
                "command": "",
                "protocol_version": "0.73",
                "error": {
                    "code": error.code,
                    "message": error.message,
                    "details": error.details,
                },
            }
    except (ValueError, OSError) as error:
        result = _adapter_error(
            StudioAdapterError(
                "xcp.studio.adapter_invalid",
                "The Studio adapter could not construct the bounded endpoint.",
                stage="adapter",
                field="endpoint",
                expected="valid host, port and timeout",
                actual=type(error).__name__,
                correction="correct the Studio device settings",
            ),
            request,
        )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
