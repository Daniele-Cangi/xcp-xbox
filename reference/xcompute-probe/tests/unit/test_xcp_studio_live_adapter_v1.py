from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from xcp_studio_live_adapter import (  # noqa: E402
    CREATIVE_COMMANDS,
    StudioAdapterError,
    describe,
    invoke_document,
    validate_request,
)
from xcp_worker_transport import WorkerEndpoint  # noqa: E402


class _FakeWorker:
    def __init__(self, responses: dict[str, dict[str, object]]) -> None:
        self.responses = responses
        self.requests: list[tuple[dict[str, object], bool]] = []

    def request(
        self,
        payload: dict[str, object],
        *,
        allow_error: bool = False,
    ) -> dict[str, object]:
        self.requests.append((payload, allow_error))
        return dict(self.responses[str(payload["command"])])


class _FakeSession:
    def __init__(self, worker: _FakeWorker) -> None:
        self.worker = worker
        self.auth: tuple[str, str] | None = None

    def factory(
        self,
        _client: object,
        *,
        pairing_code: str,
        session_id: str,
    ) -> "_FakeSession":
        self.auth = pairing_code, session_id
        return self

    def __enter__(self) -> _FakeWorker:
        return self.worker

    def __exit__(self, *_args: object) -> None:
        return None


def _request(command: str, payload: dict[str, object] | None = None) -> dict[str, object]:
    return {
        "schema_version": "worker-sdk-request-v1",
        "request_id": "studio-test-1",
        "command": command,
        "payload": payload or {},
    }


class XcpStudioLiveAdapterTests(unittest.TestCase):
    def test_description_is_narrow_and_ephemeral(self) -> None:
        result = describe()
        self.assertTrue(result["ok"])
        self.assertEqual(list(CREATIVE_COMMANDS), result["creative_commands"])
        self.assertFalse(result["authentication"]["serialized_in_request"])
        self.assertFalse(result["authentication"]["persisted"])
        self.assertFalse(result["transport"]["shell_used"])
        self.assertFalse(result["authority"]["worker_private_rules"])
        self.assertFalse(result["authority"]["message_branching"])

    def test_secret_bearing_payload_is_rejected_before_transport(self) -> None:
        with self.assertRaises(StudioAdapterError) as caught:
            validate_request(
                _request(
                    "launch_creative_project",
                    {"project_id": "demo", "session_id": "must-not-enter"},
                )
            )
        self.assertEqual(
            "xcp.studio.secret_material_forbidden",
            caught.exception.code,
        )

    def test_unknown_command_fails_closed(self) -> None:
        with self.assertRaises(StudioAdapterError) as caught:
            validate_request(_request("describe_runtime"))
        self.assertEqual("xcp.studio.command_not_admitted", caught.exception.code)

    def test_payload_cannot_override_the_admitted_command(self) -> None:
        with self.assertRaises(StudioAdapterError) as caught:
            validate_request(
                _request(
                    "observe_creative_foreground",
                    {"command": "remove_creative_install"},
                )
            )
        self.assertEqual("xcp.studio.request_invalid", caught.exception.code)
        self.assertEqual("payload.command", caught.exception.details["field"])

    def test_public_command_is_projected_through_sdk_bootstrap(self) -> None:
        worker = _FakeWorker(
            {
                "describe_runtime": {
                    "ok": True,
                    "protocol_version": "0.73",
                    "supported_commands": list(CREATIVE_COMMANDS),
                },
                "observe_creative_foreground": {
                    "ok": True,
                    "protocol_version": "0.73",
                    "schema_version": "xcp-creative-foreground-observation-v1",
                    "foreground": {"project_id": "demo"},
                },
            }
        )
        session = _FakeSession(worker)
        result = invoke_document(
            _request("observe_creative_foreground"),
            endpoint=WorkerEndpoint("127.0.0.1"),
            pairing_code="",
            session_id="ephemeral-test-session",
            client_factory=lambda endpoint: endpoint,
            session_factory=session.factory,
        )
        self.assertTrue(result["ok"])
        self.assertEqual("worker-sdk-result-v1", result["schema_version"])
        self.assertEqual((("", "ephemeral-test-session")), session.auth)
        self.assertEqual(
            [
                ({"command": "describe_runtime"}, False),
                ({"command": "observe_creative_foreground"}, True),
            ],
            worker.requests,
        )
        serialized = json.dumps(result, sort_keys=True)
        self.assertNotIn("ephemeral-test-session", serialized)

    def test_worker_error_code_and_details_are_preserved(self) -> None:
        worker = _FakeWorker(
            {
                "describe_runtime": {
                    "ok": True,
                    "protocol_version": "0.73",
                    "supported_commands": list(CREATIVE_COMMANDS),
                },
                "activate_creative_install": {
                    "ok": False,
                    "protocol_version": "0.73",
                    "error": {
                        "code": "xcp.creative.install_missing",
                        "message": "diagnostic only",
                        "details": {
                            "schema_version": "xcp-creative-error-details-v1",
                            "stage": "activate",
                            "field": "install_id",
                            "correction": "select an installed immutable bundle",
                        },
                    },
                },
            }
        )
        session = _FakeSession(worker)
        result = invoke_document(
            _request(
                "activate_creative_install",
                {"project_id": "demo", "install_id": "missing"},
            ),
            endpoint=WorkerEndpoint("127.0.0.1"),
            pairing_code="pairing",
            session_id="",
            client_factory=lambda endpoint: endpoint,
            session_factory=session.factory,
        )
        self.assertFalse(result["ok"])
        self.assertEqual(
            "xcp.creative.install_missing",
            result["error"]["code"],
        )
        self.assertEqual(
            "install_id",
            result["error"]["details"]["field"],
        )

    def test_cli_describe_emits_one_structured_document(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(TOOLS / "xcp_studio_live_adapter.py"), "describe"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, completed.returncode, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(
            "xcp-studio-live-adapter-description-v1",
            result["schema_version"],
        )


if __name__ == "__main__":
    unittest.main()
