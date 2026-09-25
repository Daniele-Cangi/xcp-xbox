#!/usr/bin/env python3
"""Finalize the blind C5 from-zero game-and-utility gate fail-closed."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import sys
from typing import Any

from jsonschema import Draft202012Validator

from xcp_agent_lifecycle import (
    DEFAULT_RECEIPT_SCHEMA,
    _validate_document,
    _validate_ledger,
)
from xcp_creative_project import sha256_file


ROOT = pathlib.Path(__file__).resolve().parents[1]
PROFILE_PATH = (
    ROOT / "profiles" / "creative" / "xcp-agent-native-creation-v1.json"
)
REPORT_SCHEMA_PATH = (
    ROOT / "schemas" / "xcp-agent-blind-creation-gate-v1.schema.json"
)
KIT_MANIFEST_NAME = "xcp-agent-kit-manifest.json"
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
EXPECTED_OPERATIONS = (
    "discover",
    "validate",
    "build",
    "verify",
    "install",
    "activate",
    "launch",
    "observe",
    "interact",
    "capture",
    "update",
    "interact",
    "rollback",
    "cleanup",
)


class BlindGateError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        field: str,
        expected: str,
        actual: str,
        correction: str,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = {
            "schema_version": "xcp-agent-error-details-v1",
            "stage": "blind_gate_finalization",
            "field": field,
            "expected": expected,
            "actual": actual,
            "correction": correction,
            "retryable": False,
        }

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": False,
            "schema_version": "xcp-agent-error-v1",
            "error": {
                "code": self.code,
                "message": self.message,
                "details": self.details,
            },
        }


def _fail(
    code: str,
    message: str,
    *,
    field: str,
    expected: Any,
    actual: Any,
    correction: str,
) -> None:
    raise BlindGateError(
        code,
        message,
        field=field,
        expected=str(expected),
        actual=str(actual),
        correction=correction,
    )


def _load_json(path: pathlib.Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise BlindGateError(
            "xcp.agent.blind_gate_input_missing",
            "A blind-gate input is missing.",
            field="path",
            expected="existing UTF-8 JSON",
            actual=str(path),
            correction="restore the exact machine evidence",
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BlindGateError(
            "xcp.agent.blind_gate_json_invalid",
            "A blind-gate input is not valid UTF-8 JSON.",
            field="path",
            expected="valid UTF-8 JSON",
            actual=str(path),
            correction="restore the exact machine evidence",
        ) from exc


def _identity(path: pathlib.Path) -> dict[str, Any]:
    resolved = path.resolve()
    if not resolved.is_file():
        _fail(
            "xcp.agent.blind_gate_input_missing",
            "A bound evidence file is absent.",
            field="path",
            expected="regular file",
            actual=resolved,
            correction="restore the exact evidence file",
        )
    return {
        "path": str(resolved),
        "bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def _expect_equal(field: str, expected: Any, actual: Any) -> None:
    if expected != actual:
        _fail(
            "xcp.agent.blind_gate_identity_mismatch",
            "Blind-gate inputs do not bind the same exact run.",
            field=field,
            expected=expected,
            actual=actual,
            correction="select the exact agent output and lifecycle evidence",
        )


def _verify_kit(root: pathlib.Path) -> dict[str, Any]:
    resolved = root.resolve()
    manifest_path = resolved / KIT_MANIFEST_NAME
    manifest = _load_json(manifest_path)
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema_version") != "xcp-agent-kit-manifest-v1"
        or manifest.get("kit_id") != "xcp.c5-c6.agent-kit"
    ):
        _fail(
            "xcp.agent.blind_gate_kit_invalid",
            "An input kit has the wrong manifest identity.",
            field="kit_manifest",
            expected="xcp.c5-c6.agent-kit",
            actual=manifest,
            correction="export or select an exact C5+C6 kit",
        )
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        _fail(
            "xcp.agent.blind_gate_kit_invalid",
            "An input kit has no bound file set.",
            field="kit_manifest.files",
            expected="one or more exact files",
            actual=type(entries).__name__,
            correction="export a fresh exact kit",
        )
    declared: set[str] = set()
    total_bytes = 0
    for entry in entries:
        if not isinstance(entry, dict):
            _fail(
                "xcp.agent.blind_gate_kit_invalid",
                "A kit file entry is not an object.",
                field="kit_manifest.files",
                expected="file identity objects",
                actual=type(entry).__name__,
                correction="export a fresh exact kit",
            )
        relative = str(entry.get("path", ""))
        pure = pathlib.PurePosixPath(relative)
        if (
            not relative
            or pure.is_absolute()
            or ".." in pure.parts
            or relative in declared
        ):
            _fail(
                "xcp.agent.blind_gate_kit_invalid",
                "A kit file path is unsafe or duplicated.",
                field="kit_manifest.files.path",
                expected="unique relative paths",
                actual=relative,
                correction="export a fresh exact kit",
            )
        declared.add(relative)
        target = resolved.joinpath(*pure.parts).resolve()
        try:
            target.relative_to(resolved)
        except ValueError as exc:
            raise BlindGateError(
                "xcp.agent.blind_gate_kit_invalid",
                "A kit file path escapes its root.",
                field="kit_manifest.files.path",
                expected=str(resolved),
                actual=str(target),
                correction="export a fresh exact kit",
            ) from exc
        identity = _identity(target)
        _expect_equal(
            f"kit.{relative}.bytes",
            int(entry.get("bytes", -1)),
            identity["bytes"],
        )
        _expect_equal(
            f"kit.{relative}.sha256",
            str(entry.get("sha256", "")),
            identity["sha256"],
        )
        total_bytes += identity["bytes"]
    actual = {
        path.relative_to(resolved).as_posix()
        for path in resolved.rglob("*")
        if path.is_file() and path.name != KIT_MANIFEST_NAME
    }
    _expect_equal("kit.file_set", sorted(declared), sorted(actual))
    security = manifest.get("security", {})
    expected_security = {
        "repository_metadata_included": False,
        "samples_included": False,
        "credentials_included": False,
        "session_material_persisted": False,
    }
    for field, expected in expected_security.items():
        _expect_equal(f"kit.security.{field}", expected, security.get(field))
    return {
        "path": str(resolved),
        "kit_version": str(manifest.get("kit_version", "")),
        "manifest_sha256": sha256_file(manifest_path),
        "file_count": len(entries),
        "total_bytes": total_bytes,
    }


def _manifest_project(
    manifest: dict[str, Any],
    experience_kind: str,
) -> dict[str, Any]:
    matches = [
        project
        for project in manifest.get("projects", [])
        if isinstance(project, dict)
        and project.get("experience_kind") == experience_kind
    ]
    if len(matches) != 1:
        _fail(
            "xcp.agent.blind_gate_authoring_invalid",
            "The authoring result must contain one project of each gate kind.",
            field="projects.experience_kind",
            expected=f"exactly one {experience_kind}",
            actual=len(matches),
            correction="restore the exact blind authoring result",
        )
    return matches[0]


def _validate_authoring(manifest: dict[str, Any]) -> None:
    _expect_equal(
        "authoring.schema_version",
        "xcp-c5-blind-authoring-result-v1",
        manifest.get("schema_version"),
    )
    _expect_equal(
        "authoring.gate_id",
        "C5_AGENT_NATIVE_CREATION_GATE",
        manifest.get("gate_id"),
    )
    _expect_equal(
        "authoring.local_authoring_decision",
        "pass",
        manifest.get("local_authoring_decision"),
    )
    provenance = manifest.get("provenance", {})
    expected = {
        "repository_files_read": 0,
        "repository_metadata_read": 0,
        "internet_used": False,
        "samples_used": False,
        "onedrive_used": False,
        "credentials_used": False,
        "live_operations_used": False,
        "xbox_accessed": False,
        "all_project_files_authored_from_zero": True,
        "python_bytecode_writes_disabled": True,
        "host_profile_used_for_all_prepares": True,
    }
    for field, value in expected.items():
        _expect_equal(f"authoring.provenance.{field}", value, provenance.get(field))


def _validate_declared_project_files(project: dict[str, Any]) -> None:
    for role in ("base", "update"):
        record = project[role]
        project_path = pathlib.Path(str(record["project_file"]))
        _expect_equal(
            f"{project['project_id']}.{role}.project_sha256",
            record["project_sha256"],
            sha256_file(project_path),
        )
        bundle_path = (
            pathlib.Path(str(record["bundle_dir"])) / "xcp-bundle.json"
        )
        _expect_equal(
            f"{project['project_id']}.{role}.bundle_sha256",
            record["bundle_manifest_sha256"],
            sha256_file(bundle_path),
        )
        project_document = _load_json(project_path)
        _expect_equal(
            f"{project['project_id']}.{role}.project_id",
            project["project_id"],
            project_document.get("project_id"),
        )
        _expect_equal(
            f"{project['project_id']}.{role}.version",
            record["version"],
            project_document.get("version"),
        )
    intent = project["intent"]
    _expect_equal(
        f"{project['project_id']}.intent.sha256",
        intent["sha256"],
        sha256_file(pathlib.Path(str(intent["file"]))),
    )


def _validate_receipt_and_ledger(
    project: dict[str, Any],
    receipt_path: pathlib.Path,
    ledger_path: pathlib.Path,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    receipt = _load_json(receipt_path)
    _validate_document(
        receipt,
        DEFAULT_RECEIPT_SCHEMA,
        stage="blind_gate_receipt",
    )
    ledger = _load_json(ledger_path)
    ledger = _validate_ledger(
        ledger,
        intent_sha256=str(project["intent"]["sha256"]),
    )
    _expect_equal("receipt.decision", "pass", receipt["decision"])
    _expect_equal("ledger.status", "passed", ledger["status"])
    _expect_equal("receipt.project_id", project["project_id"], receipt["project_id"])
    _expect_equal(
        "receipt.project_version",
        project["base"]["version"],
        receipt["project_version"],
    )
    _expect_equal(
        "receipt.project_sha256",
        project["base"]["project_sha256"],
        receipt["project_sha256"],
    )
    _expect_equal(
        "receipt.bundle_sha256",
        project["base"]["bundle_manifest_sha256"],
        receipt["bundle_sha256"],
    )
    _expect_equal(
        "receipt.intent_sha256",
        project["intent"]["sha256"],
        receipt["intent_sha256"],
    )
    _expect_equal("ledger.project_id", project["project_id"], ledger["project_id"])
    ledger_identity = _identity(ledger_path)
    _expect_equal(
        "receipt.correction_ledger_sha256",
        ledger_identity["sha256"],
        receipt["correction_ledger_sha256"],
    )

    operation_names = tuple(item["name"] for item in receipt["operations"])
    _expect_equal("receipt.operations", EXPECTED_OPERATIONS, operation_names)
    if any(item["outcome"] != "pass" for item in receipt["operations"]):
        _fail(
            "xcp.agent.blind_gate_operation_failed",
            "A C5 lifecycle operation did not pass.",
            field="receipt.operations",
            expected="all pass",
            actual=receipt["operations"],
            correction="correct the structured failure and rerun C5",
        )
    blockers = [
        item
        for item in receipt["acceptance"]
        if item["severity"] == "blocker"
    ]
    expected_acceptance = int(project["intent"]["local_blocker_count"]) + int(
        project["intent"]["live_blocker_count"]
    )
    _expect_equal("receipt.acceptance_count", expected_acceptance, len(blockers))
    if any(item["outcome"] != "pass" for item in blockers):
        _fail(
            "xcp.agent.blind_gate_acceptance_failed",
            "A blocking semantic assertion did not pass.",
            field="receipt.acceptance",
            expected="all blocker outcomes pass",
            actual=blockers,
            correction="correct the project behavior and rerun C5",
        )
    _expect_equal("receipt.capture_count", 3, len(receipt["artifacts"]))
    captures: list[dict[str, Any]] = []
    for artifact in receipt["artifacts"]:
        _expect_equal("receipt.artifact.role", "creative.capture", artifact["role"])
        identity = _identity(pathlib.Path(str(artifact["reference"])))
        _expect_equal("receipt.artifact.bytes", artifact["bytes"], identity["bytes"])
        _expect_equal(
            "receipt.artifact.sha256",
            artifact["sha256"],
            identity["sha256"],
        )
        captures.append(identity)
    if len(ledger["entries"]) < 1 or any(
        entry["retry"]["outcome"] != "pass" for entry in ledger["entries"]
    ):
        _fail(
            "xcp.agent.blind_gate_correction_missing",
            "The correction ledger is empty or contains an unconfirmed retry.",
            field="ledger.entries",
            expected="one or more confirmed corrections",
            actual=ledger["entries"],
            correction="complete and confirm the structured correction cycle",
        )
    summary = {
        "experience_kind": project["experience_kind"],
        "project_id": project["project_id"],
        "base": {
            "version": project["base"]["version"],
            "project_sha256": project["base"]["project_sha256"],
            "bundle_sha256": project["base"]["bundle_manifest_sha256"],
        },
        "update": {
            "version": project["update"]["version"],
            "project_sha256": project["update"]["project_sha256"],
            "bundle_sha256": project["update"]["bundle_manifest_sha256"],
        },
        "intent_sha256": project["intent"]["sha256"],
        "receipt": _identity(receipt_path),
        "operations": {
            "passed": len(receipt["operations"]),
            "total": len(receipt["operations"]),
        },
        "acceptance": {
            "passed": len(blockers),
            "total": len(blockers),
        },
        "captures": captures,
        "ledger": {
            **ledger_identity,
            "status": ledger["status"],
            "entry_count": len(ledger["entries"]),
            "head_sha256": ledger["head_sha256"],
            "correction_codes": [
                entry["error"]["code"] for entry in ledger["entries"]
            ],
        },
    }
    return receipt, ledger, summary


def _validate_final_state(
    final_state_path: pathlib.Path,
    projects: list[dict[str, Any]],
    expected_package_version: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    state = _load_json(final_state_path)
    _expect_equal(
        "final_state.schema_version",
        "xcp-c5-blind-final-state-v1",
        state.get("schema_version"),
    )
    _expect_equal(
        "final_state.package.version",
        expected_package_version,
        state.get("package", {}).get("version"),
    )
    checks = state.get("checks")
    if not isinstance(checks, dict) or not checks or any(
        value is not True for value in checks.values()
    ):
        _fail(
            "xcp.agent.blind_gate_final_state_failed",
            "The final Xbox state contains a failed check.",
            field="final_state.checks",
            expected="all true",
            actual=checks,
            correction="restore and verify the exact bounded install pairs",
        )
    for project in projects:
        role = (
            "game_installs"
            if project["experience_kind"] == "game"
            else "utility_installs"
        )
        installs = state.get(role)
        if not isinstance(installs, list) or len(installs) != 2:
            _fail(
                "xcp.agent.blind_gate_final_state_failed",
                "A final project install pair is incomplete or contaminated.",
                field=role,
                expected="exactly two installs",
                actual=installs,
                correction="remove only unreferenced stale installs and recheck",
            )
        active = [item for item in installs if item.get("active") is True]
        rollback = [
            item for item in installs if item.get("rollback_target") is True
        ]
        _expect_equal(f"{role}.active_count", 1, len(active))
        _expect_equal(f"{role}.rollback_count", 1, len(rollback))
        _expect_equal(
            f"{role}.active.version",
            project["base"]["version"],
            active[0].get("project_version"),
        )
        _expect_equal(
            f"{role}.active.bundle_sha256",
            project["base"]["bundle_sha256"],
            active[0].get("bundle_sha256"),
        )
        _expect_equal(
            f"{role}.rollback.version",
            project["update"]["version"],
            rollback[0].get("project_version"),
        )
        _expect_equal(
            f"{role}.rollback.bundle_sha256",
            project["update"]["bundle_sha256"],
            rollback[0].get("bundle_sha256"),
        )
        project["final_installs"] = {
            "active_install_id": active[0]["install_id"],
            "rollback_install_id": rollback[0]["install_id"],
        }
    return state, {
        **_identity(final_state_path),
        "checks_passed": len(checks),
        "checks_total": len(checks),
    }


def _error_codes_from_attempts(project: dict[str, Any]) -> list[str]:
    codes: list[str] = []
    for attempt in project.get("live_attempts", []):
        if (
            isinstance(attempt, dict)
            and attempt.get("outcome") == "fail"
            and isinstance(attempt.get("error"), dict)
        ):
            code = str(attempt["error"].get("code", ""))
            if code:
                codes.append(code)
    return codes


def finalize(
    *,
    authoring_manifest_path: pathlib.Path,
    authoring_kit: pathlib.Path,
    live_kit: pathlib.Path,
    finalizer_kit: pathlib.Path,
    game_receipt_path: pathlib.Path,
    game_ledger_path: pathlib.Path,
    utility_receipt_path: pathlib.Path,
    utility_ledger_path: pathlib.Path,
    final_state_path: pathlib.Path,
    game_controller_commit: str,
    utility_controller_commit: str,
    finalizer_controller_commit: str,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    for field, commit in (
        ("game_controller_commit", game_controller_commit),
        ("utility_controller_commit", utility_controller_commit),
        ("finalizer_controller_commit", finalizer_controller_commit),
    ):
        if not COMMIT_PATTERN.fullmatch(commit):
            _fail(
                "xcp.agent.blind_gate_commit_invalid",
                "A controller commit identity is invalid.",
                field=field,
                expected="40 lowercase hexadecimal characters",
                actual=commit,
                correction="bind the exact repository commit",
            )
    if output_path.exists():
        _fail(
            "xcp.agent.blind_gate_output_exists",
            "The blind-gate finalizer will not overwrite evidence.",
            field="output",
            expected="fresh path",
            actual=output_path,
            correction="select a new evidence path",
        )

    profile = _load_json(PROFILE_PATH)
    validation = profile["validation"]
    authoring = _load_json(authoring_manifest_path)
    _validate_authoring(authoring)
    game_project = _manifest_project(authoring, "game")
    utility_project = _manifest_project(authoring, "utility")
    for project in (game_project, utility_project):
        _validate_declared_project_files(project)

    game_receipt, game_ledger, game_summary = _validate_receipt_and_ledger(
        game_project,
        game_receipt_path,
        game_ledger_path,
    )
    (
        utility_receipt,
        utility_ledger,
        utility_summary,
    ) = _validate_receipt_and_ledger(
        utility_project,
        utility_receipt_path,
        utility_ledger_path,
    )
    _expect_equal(
        "host_profile_sha256",
        game_receipt["host_profile_sha256"],
        utility_receipt["host_profile_sha256"],
    )

    project_summaries = [game_summary, utility_summary]
    _, final_state_identity = _validate_final_state(
        final_state_path,
        project_summaries,
        str(validation["candidate_package_version"]),
    )
    local_code = str(
        game_project.get("structured_local_failure", {}).get("error_code", "")
    )
    ledger_local_codes = {
        entry["error"]["code"]
        for entry in game_ledger["entries"]
        if entry["stage"] == "local_validation"
        and entry["retry"]["outcome"] == "pass"
    }
    if not local_code or local_code not in ledger_local_codes:
        _fail(
            "xcp.agent.blind_gate_local_correction_missing",
            "The declared structured local failure was not confirmed.",
            field="game.structured_local_failure",
            expected=local_code,
            actual=sorted(ledger_local_codes),
            correction="preserve and confirm the exact local correction",
        )
    live_attempt_codes = sorted(
        set(
            _error_codes_from_attempts(game_project)
            + _error_codes_from_attempts(utility_project)
        )
    )
    ledger_live_codes = {
        entry["error"]["code"]
        for ledger in (game_ledger, utility_ledger)
        for entry in ledger["entries"]
        if entry["stage"] == "live" and entry["retry"]["outcome"] == "pass"
    }
    natural_live_codes = sorted(set(live_attempt_codes) & ledger_live_codes)
    if not natural_live_codes:
        _fail(
            "xcp.agent.blind_gate_live_correction_missing",
            "No naturally encountered live error was confirmed by the final run.",
            field="live_corrections",
            expected="failed-attempt code present in passed live ledger",
            actual={
                "attempts": live_attempt_codes,
                "ledger": sorted(ledger_live_codes),
            },
            correction="correct a real structured live failure and rerun C5",
        )

    kits = {
        "authoring": _verify_kit(authoring_kit),
        "live_lifecycle": _verify_kit(live_kit),
        "finalizer": _verify_kit(finalizer_kit),
    }
    operations_total = sum(
        project["operations"]["total"] for project in project_summaries
    )
    acceptance_total = sum(
        project["acceptance"]["total"] for project in project_summaries
    )
    captures_total = sum(len(project["captures"]) for project in project_summaries)
    correction_total = sum(
        project["ledger"]["entry_count"] for project in project_summaries
    )
    report = {
        "schema_version": "xcp-agent-blind-creation-gate-report-v1",
        "gate_id": "C5_AGENT_NATIVE_CREATION_GATE",
        "status": "IMPLEMENTED_MEASURED_ON_XBOX",
        "decision": "pass",
        "recorded_on": dt.date.today().isoformat(),
        "runtime": {
            "protocol_version": str(
                _load_json(final_state_path)["protocol_version"]
            ),
            "worker_package_version": str(
                validation["candidate_package_version"]
            ),
            "worker_package_bytes": int(validation["candidate_package_bytes"]),
            "worker_package_sha256": str(
                validation["candidate_package_sha256"]
            ),
            "package_listing": str(validation["xbox_package_listing"]),
            "host_profile_sha256": game_receipt["host_profile_sha256"],
            "worker_package_changed_for_gate": False,
        },
        "controller_commits": {
            "game_live": game_controller_commit,
            "utility_live": utility_controller_commit,
            "finalizer": finalizer_controller_commit,
        },
        "kits": kits,
        "authoring": _identity(authoring_manifest_path),
        "repository_independence": {
            "repository_files_read": 0,
            "repository_metadata_read": 0,
            "internet_used": False,
            "samples_used": False,
            "credentials_used": False,
            "author_live_access_used": False,
            "root_free_form_interpretation_used": False,
        },
        "projects": project_summaries,
        "failed_attempts": {
            "structured_local_error_codes": [local_code],
            "structured_live_error_codes": live_attempt_codes,
            "preserved": True,
        },
        "aggregate": {
            "projects_passed": 2,
            "projects_total": 2,
            "experience_kinds": ["game", "utility"],
            "operations_passed": operations_total,
            "operations_total": operations_total,
            "acceptance_passed": acceptance_total,
            "acceptance_total": acceptance_total,
            "captures_verified": captures_total,
            "captures_total": captures_total,
            "correction_entries_passed": correction_total,
            "correction_entries_total": correction_total,
            "natural_local_correction_codes": [local_code],
            "natural_live_correction_codes": natural_live_codes,
            "final_state_checks_passed": final_state_identity["checks_passed"],
            "final_state_checks_total": final_state_identity["checks_total"],
        },
        "final_state_evidence": final_state_identity,
        "boundaries": {
            "worker_gap_after_gate": False,
            "native_change_required_after_gate": False,
            "restricted_capability_leaks": 0,
            "runtime_shader_compilation": False,
            "native_or_host_code_upload": False,
            "claim_boundary_expanded": False,
            "topology_152_promoted": False,
            "t3_reopened": False,
            "next_target": "C7_PROJECT_EVOLUTION_AND_CONTINUOUS_READAPTATION",
        },
    }
    schema = _load_json(REPORT_SCHEMA_PATH)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(report),
        key=lambda item: tuple(str(part) for part in item.absolute_path),
    )
    if errors:
        error = errors[0]
        _fail(
            "xcp.agent.blind_gate_report_invalid",
            "The final blind-gate report violates its schema.",
            field=".".join(str(part) for part in error.absolute_path) or "$",
            expected=error.message,
            actual=error.instance,
            correction="fix the finalizer projection before promotion",
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return {
        "ok": True,
        "schema_version": "xcp-agent-blind-creation-gate-finalization-v1",
        "output": str(output_path.resolve()),
        "bytes": output_path.stat().st_size,
        "sha256": sha256_file(output_path),
        "decision": "pass",
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Finalize the blind C5 game-and-utility gate fail-closed."
    )
    parser.add_argument("--authoring-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--authoring-kit", type=pathlib.Path, required=True)
    parser.add_argument("--live-kit", type=pathlib.Path, required=True)
    parser.add_argument("--finalizer-kit", type=pathlib.Path, required=True)
    parser.add_argument("--game-receipt", type=pathlib.Path, required=True)
    parser.add_argument("--game-ledger", type=pathlib.Path, required=True)
    parser.add_argument("--utility-receipt", type=pathlib.Path, required=True)
    parser.add_argument("--utility-ledger", type=pathlib.Path, required=True)
    parser.add_argument("--final-state", type=pathlib.Path, required=True)
    parser.add_argument("--game-controller-commit", required=True)
    parser.add_argument("--utility-controller-commit", required=True)
    parser.add_argument("--finalizer-controller-commit", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = finalize(
            authoring_manifest_path=args.authoring_manifest,
            authoring_kit=args.authoring_kit,
            live_kit=args.live_kit,
            finalizer_kit=args.finalizer_kit,
            game_receipt_path=args.game_receipt,
            game_ledger_path=args.game_ledger,
            utility_receipt_path=args.utility_receipt,
            utility_ledger_path=args.utility_ledger,
            final_state_path=args.final_state,
            game_controller_commit=args.game_controller_commit,
            utility_controller_commit=args.utility_controller_commit,
            finalizer_controller_commit=args.finalizer_controller_commit,
            output_path=args.output,
        )
        print(json.dumps(result, indent=2))
        return 0
    except BlindGateError as exc:
        print(json.dumps(exc.as_dict(), indent=2))
        return 2
    except Exception as exc:
        error = BlindGateError(
            "xcp.agent.blind_gate_unexpected_failure",
            "Blind-gate finalization failed unexpectedly.",
            field="exception",
            expected="successful deterministic finalization",
            actual=f"{type(exc).__name__}: {exc}",
            correction="inspect the exact machine inputs and finalizer",
        )
        print(json.dumps(error.as_dict(), indent=2))
        return 2


if __name__ == "__main__":
    sys.exit(main())
