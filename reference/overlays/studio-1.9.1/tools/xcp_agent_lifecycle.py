#!/usr/bin/env python3
"""Repository-independent C5 lifecycle tool for ordinary XCP projects."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import sys
import time
from dataclasses import dataclass
from typing import Any, Iterable

from jsonschema import Draft202012Validator

from xcp_creative_project import (
    BUNDLE_FILENAME,
    PROJECT_FILENAME,
    CreativeProjectError,
    build_bundle,
    canonical_json_bytes,
    create_project,
    _bundle_manifest as _expected_bundle_manifest,
    sha256_bytes,
    sha256_file,
    validate_project,
    verify_bundle,
)
from xcp_worker_transport import (
    WorkerEndpoint,
    WorkerJsonClient,
    WorkerSession,
    WorkerTransportError,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_INTENT_SCHEMA = ROOT / "schemas" / "xcp-agent-project-intent-v1.schema.json"
DEFAULT_LEDGER_SCHEMA = (
    ROOT / "schemas" / "xcp-agent-correction-ledger-v1.schema.json"
)
DEFAULT_RECEIPT_SCHEMA = (
    ROOT / "schemas" / "xcp-agent-lifecycle-receipt-v1.schema.json"
)
DEFAULT_HOST_PROFILE_SCHEMA = (
    ROOT / "schemas" / "xcp-creative-host-profile-v1.schema.json"
)
DEFAULT_C5_PROFILE = ROOT / "profiles" / "creative" / "xcp-agent-native-creation-v1.json"
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
IDENTIFIER_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$")
ARTIFACT_CHUNK_BYTES = 64 * 1024
WORKER_ARTIFACT_ID_MAX_CHARS = 64
RUN_ID_MAX_CHARS = WORKER_ARTIFACT_ID_MAX_CHARS - len("-capture-rollback")


class AgentLifecycleError(RuntimeError):
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
            "schema_version": "xcp-agent-error-details-v1",
            "stage": stage,
            "field": field,
            "expected": expected,
            "actual": actual,
            "correction": correction,
            "retryable": retryable,
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


def _load_json(path: pathlib.Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AgentLifecycleError(
            "xcp.agent.file_missing",
            "A required machine document is missing.",
            stage="load",
            field="path",
            expected="existing UTF-8 JSON file",
            actual=str(path),
            correction="provide the exact required file",
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AgentLifecycleError(
            "xcp.agent.json_invalid",
            "A machine document is not valid UTF-8 JSON.",
            stage="load",
            field="path",
            expected="valid UTF-8 JSON",
            actual=str(path),
            correction="correct the JSON document and retry",
        ) from exc


def _schema_validator(path: pathlib.Path) -> Draft202012Validator:
    schema = _load_json(path)
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def _validate_document(
    document: Any,
    schema_path: pathlib.Path,
    *,
    stage: str,
) -> None:
    errors = sorted(
        _schema_validator(schema_path).iter_errors(document),
        key=lambda item: tuple(str(part) for part in item.absolute_path),
    )
    if not errors:
        return
    first = errors[0]
    location = ".".join(str(part) for part in first.absolute_path)
    raise AgentLifecycleError(
        "xcp.agent.schema_rejected",
        "A C5 machine document failed schema validation.",
        stage=stage,
        field=location,
        expected=first.validator,
        actual=first.message,
        correction="correct the structured field and retry",
        retryable=True,
    )


def _write_new_json(path: pathlib.Path, document: Any) -> None:
    path = path.resolve()
    if path.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "A C5 output would overwrite an existing path.",
            stage="write",
            field="path",
            expected="a new output path",
            actual=str(path),
            correction="choose a fresh output path",
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_json_bytes(document))
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _replace_json(path: pathlib.Path, document: Any) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_json_bytes(document))
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _strict_identifier(
    value: str,
    field: str,
    *,
    maximum: int | None = None,
) -> str:
    if (
        not IDENTIFIER_PATTERN.fullmatch(value)
        or (maximum is not None and len(value) > maximum)
    ):
        expected = IDENTIFIER_PATTERN.pattern
        if maximum is not None:
            expected = f"{expected} with length <= {maximum}"
        raise AgentLifecycleError(
            "xcp.agent.identifier_invalid",
            "A C5 identifier is outside the portable grammar.",
            stage="input",
            field=field,
            expected=expected,
            actual=value,
            correction="use a lowercase dot, dash or underscore separated id",
        )
    return value


def validate_intent(
    path: pathlib.Path,
    schema_path: pathlib.Path = DEFAULT_INTENT_SCHEMA,
) -> dict[str, Any]:
    document = _load_json(path)
    _validate_document(document, schema_path, stage="intent")
    assertion_ids = [str(item["id"]) for item in document["acceptance"]]
    if len(assertion_ids) != len(set(assertion_ids)):
        raise AgentLifecycleError(
            "xcp.agent.intent_duplicate_assertion",
            "Intent assertion ids must be unique.",
            stage="intent",
            field="acceptance[].id",
            expected="unique identifiers",
            actual="duplicate",
            correction="rename the duplicate assertion",
        )
    assertion_set = set(assertion_ids)
    for sequence in document.get("interaction_sequences", []):
        unknown = sorted(set(sequence["assertion_ids"]) - assertion_set)
        if unknown:
            raise AgentLifecycleError(
                "xcp.agent.intent_unknown_assertion",
                "An interaction sequence references an unknown assertion.",
                stage="intent",
                field=f"interaction_sequences.{sequence['id']}.assertion_ids",
                expected="ids declared in acceptance",
                actual=",".join(unknown),
                correction="declare or remove the referenced assertion",
            )
    return document


def new_intent(project_id: str, summary: str, experience_kind: str) -> dict[str, Any]:
    project_id = _strict_identifier(project_id, "project_id")
    return {
        "schema_version": "xcp-agent-project-intent-v1",
        "intent_id": f"{project_id}.intent",
        "project_id": project_id,
        "summary": summary,
        "experience_kind": experience_kind,
        "non_goals": [],
        "acceptance": [
            {
                "id": "project-identity",
                "phase": "local",
                "severity": "blocker",
                "observation_path": "project.project_id",
                "operator": "equal",
                "expected": project_id,
                "correction_hint": "make the generated project id match the intent",
            },
            {
                "id": "foreground-active",
                "phase": "live",
                "severity": "blocker",
                "observation_path": "active",
                "operator": "equal",
                "expected": True,
                "correction_hint": "launch the exact active install and observe it",
            },
        ],
        "interaction_sequences": [],
        "source_provenance": {
            "kind": "created_from_prompt",
            "reference": "user_or_agent_authored_intent",
        },
        "completion_policy": {
            "all_blockers_must_pass": True,
            "minimum_live_captures": 1,
            "structured_observation_required": True,
            "rollback_required": True,
            "cleanup_required": True,
        },
    }


def _resolve_observation_path(document: Any, path: str) -> tuple[bool, Any]:
    current = document
    for name, index_text in re.findall(r"([A-Za-z0-9_-]+)|\[([0-9]+)\]", path):
        if name:
            if not isinstance(current, dict) or name not in current:
                return False, None
            current = current[name]
        else:
            index = int(index_text)
            if not isinstance(current, list) or index >= len(current):
                return False, None
            current = current[index]
    return True, current


def _compare(actual: Any, operator: str, expected: Any, exists: bool) -> bool:
    if operator == "exists":
        return exists is bool(expected)
    if not exists:
        return False
    if operator == "equal":
        return actual == expected
    if operator == "not_equal":
        return actual != expected
    if operator == "greater":
        return actual > expected
    if operator == "greater_equal":
        return actual >= expected
    if operator == "less":
        return actual < expected
    if operator == "less_equal":
        return actual <= expected
    if operator == "contains":
        return expected in actual
    if operator == "matches":
        return isinstance(actual, str) and isinstance(expected, str) and bool(
            re.search(expected, actual)
        )
    raise AssertionError(f"unsupported operator {operator}")


def evaluate_acceptance(
    intent: dict[str, Any],
    observation: dict[str, Any],
    *,
    phase: str,
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for assertion in intent["acceptance"]:
        if assertion["phase"] not in (phase, "both"):
            continue
        exists, actual = _resolve_observation_path(
            observation, assertion["observation_path"]
        )
        try:
            passed = _compare(
                actual,
                assertion["operator"],
                assertion["expected"],
                exists,
            )
        except (TypeError, ValueError):
            passed = False
        results.append(
            {
                "assertion_id": assertion["id"],
                "phase": phase,
                "severity": assertion["severity"],
                "outcome": "pass" if passed else "fail",
                "actual": actual if exists else None,
                "expected": assertion["expected"],
            }
        )
    return results


def _blocking_acceptance_passed(results: Iterable[dict[str, Any]]) -> bool:
    return all(
        item["outcome"] == "pass"
        for item in results
        if item["severity"] == "blocker"
    )


@dataclass(frozen=True)
class PreparedProject:
    project: dict[str, Any]
    intent: dict[str, Any]
    local_observation: dict[str, Any]
    acceptance: list[dict[str, Any]]
    bundle_dir: pathlib.Path
    bundle_manifest: dict[str, Any]
    bundle_manifest_sha256: str
    bundle_origin: str
    expected_bundle_manifest_sha256: str


def _is_reparse_path(path: pathlib.Path) -> bool:
    attributes = getattr(os.lstat(path), "st_file_attributes", 0)
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def _ordinary_tree_records_unchecked(
    root: pathlib.Path,
) -> list[dict[str, Any]]:
    if not root.is_dir() or _is_reparse_path(root):
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_tree_invalid",
            "A prebuilt source root must be an ordinary directory.",
            stage="prepare", field="prebuilt_root",
            expected="existing ordinary directory without a reparse point",
            actual=str(root),
            correction="select the exact ordinary promoted project or bundle root",
        )
    records: list[dict[str, Any]] = []
    pending: list[tuple[pathlib.Path, pathlib.PurePosixPath]] = [
        (root, pathlib.PurePosixPath())
    ]
    while pending:
        directory, relative_dir = pending.pop()
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda entry: entry.name)
        for entry in entries:
            path = pathlib.Path(entry.path)
            relative = relative_dir / entry.name
            entry_stat = entry.stat(follow_symlinks=False)
            if entry.is_symlink() or bool(
                getattr(entry_stat, "st_file_attributes", 0)
                & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
            ):
                raise AgentLifecycleError(
                    "xcp.agent.prebuilt_tree_invalid",
                    "Prebuilt project and bundle trees cannot contain links or reparse points.",
                    stage="prepare", field="prebuilt_root",
                    expected="ordinary files and directories only",
                    actual=relative.as_posix(),
                    correction="replace the linked entry with exact ordinary bytes",
                )
            if entry.is_dir(follow_symlinks=False):
                pending.append((path, relative))
                continue
            if not entry.is_file(follow_symlinks=False):
                raise AgentLifecycleError(
                    "xcp.agent.prebuilt_tree_invalid",
                    "Prebuilt trees cannot contain special filesystem entries.",
                    stage="prepare", field="prebuilt_root",
                    expected="ordinary files and directories only",
                    actual=relative.as_posix(),
                    correction="remove the special entry from the promoted tree",
                )
            records.append(
                {
                    "path": relative.as_posix(),
                    "bytes": entry_stat.st_size,
                    "sha256": sha256_file(path),
                }
            )
    records.sort(key=lambda item: item["path"])
    folded = [item["path"].casefold() for item in records]
    if len(folded) != len(set(folded)):
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_tree_invalid",
            "Prebuilt trees cannot contain case-colliding paths.",
            stage="prepare", field="prebuilt_root",
            expected="one canonical spelling per path",
            actual=str(root),
            correction="remove the case-colliding entry",
        )
    return records


def _ordinary_tree_records(root: pathlib.Path) -> list[dict[str, Any]]:
    try:
        return _ordinary_tree_records_unchecked(root)
    except AgentLifecycleError:
        raise
    except (OSError, RuntimeError) as exc:
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_tree_invalid",
            "The prebuilt tree could not be read as stable ordinary bytes.",
            stage="prepare", field="prebuilt_root",
            expected="readable stable ordinary files without path aliasing",
            actual=f"{root}: {type(exc).__name__}",
            correction="repair access or select a fresh exact promoted root",
        ) from exc


def _copy_exact_tree(source: pathlib.Path, destination: pathlib.Path) -> None:
    records = _ordinary_tree_records(source)
    if destination.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The exact prebuilt snapshot destination must be fresh.",
            stage="prepare", field="snapshot_root",
            expected="a new directory",
            actual=str(destination),
            correction="choose a fresh live bundle root",
        )
    destination.mkdir(parents=True)
    for record in records:
        source_file = source / pathlib.PurePosixPath(record["path"])
        destination_file = destination / pathlib.PurePosixPath(record["path"])
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        try:
            shutil.copyfile(source_file, destination_file)
        except OSError as exc:
            raise AgentLifecycleError(
                "xcp.agent.prebuilt_snapshot_failed",
                "An exact prebuilt file could not be copied into the private snapshot.",
                stage="prepare", field="snapshot_root",
                expected=record["sha256"],
                actual=f"{record['path']}: {type(exc).__name__}",
                correction="discard the run root and retry from stable promoted bytes",
            ) from exc
    copied = _ordinary_tree_records(destination)
    source_after = _ordinary_tree_records(source)
    if copied != records or source_after != records:
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_snapshot_mismatch",
            "The private prebuilt snapshot or its source changed during copying.",
            stage="prepare", field="snapshot_root",
            expected=sha256_bytes(canonical_json_bytes(records)),
            actual=sha256_bytes(
                canonical_json_bytes(
                    {"snapshot": copied, "source_after": source_after}
                )
            ),
            correction="discard the run root and create a new exact snapshot",
        )


def _paths_overlap(first: pathlib.Path, second: pathlib.Path) -> bool:
    try:
        left = first.resolve(strict=False)
        right = second.resolve(strict=False)
    except (OSError, RuntimeError) as exc:
        raise AgentLifecycleError(
            "xcp.agent.path_identity_invalid",
            "A live or prebuilt path could not be resolved safely.",
            stage="run_live", field="path",
            expected="stable canonical path without loops or inaccessible components",
            actual=f"{first} / {second}: {type(exc).__name__}",
            correction="select stable ordinary paths and retry before worker contact",
        ) from exc
    return left == right or left in right.parents or right in left.parents


def _snapshot_prebuilt_stage(
    *,
    project_source: pathlib.Path,
    bundle_source: pathlib.Path,
    bundle_root: pathlib.Path,
    role: str,
) -> tuple[pathlib.Path, pathlib.Path]:
    project_snapshot = bundle_root / "prebuilt-projects" / role
    bundle_snapshot = bundle_root / role
    for source, destination in (
        (project_source, project_snapshot),
        (bundle_source, bundle_snapshot),
    ):
        if _paths_overlap(source, destination):
            raise AgentLifecycleError(
                "xcp.agent.prebuilt_snapshot_overlap",
                "A private prebuilt snapshot cannot overlap its source tree.",
                stage="run_live", field=f"{role}_prebuilt_bundle_dir",
                expected="disjoint source and fresh live snapshot roots",
                actual=f"{source} -> {destination}",
                correction="choose a fresh bundle root outside the promoted evidence root",
            )
    if _paths_overlap(project_source, bundle_source):
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_source_overlap",
            "The promoted project and bundle roots must be disjoint.",
            stage="run_live", field=f"{role}_prebuilt_bundle_dir",
            expected="separate ordinary project and bundle trees",
            actual=f"{project_source} / {bundle_source}",
            correction="select the exact separate B5 project and bundle roots",
        )
    _copy_exact_tree(project_source, project_snapshot)
    _copy_exact_tree(bundle_source, bundle_snapshot)
    return project_snapshot, bundle_snapshot


def _prebuilt_requested(
    bundle_dir: pathlib.Path | None,
    expected_sha256: str,
    *,
    field: str,
) -> bool:
    supplied = bundle_dir is not None
    if supplied != bool(expected_sha256):
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_bundle_arguments_incomplete",
            "A prebuilt bundle path and its exact SHA-256 must be supplied together.",
            stage="run_live", field=field,
            expected="both path and 64-character lowercase SHA-256, or neither",
            actual=f"path={supplied}, sha256={bool(expected_sha256)}",
            correction="supply the complete exact prebuilt bundle identity",
        )
    if expected_sha256 and not HASH_PATTERN.fullmatch(expected_sha256):
        raise AgentLifecycleError(
            "xcp.agent.bundle_identity_invalid",
            "The expected prebuilt bundle identity must be a lowercase SHA-256.",
            stage="run_live", field=field,
            expected="64 lowercase hexadecimal characters",
            actual=expected_sha256,
            correction="use the exact verified xcp-bundle.json SHA-256",
        )
    return supplied


def _validate_update_pair(
    base: PreparedProject,
    update: PreparedProject,
) -> None:
    if base.project["project_id"] != update.project["project_id"]:
        raise AgentLifecycleError(
            "xcp.agent.update_project_mismatch",
            "Base and update bundles must belong to the same project.",
            stage="run_live", field="project_id",
            expected=base.project["project_id"],
            actual=update.project["project_id"],
            correction="select a companion update for the same project",
        )
    if (
        base.project["version"] == update.project["version"]
        or base.bundle_manifest_sha256 == update.bundle_manifest_sha256
    ):
        raise AgentLifecycleError(
            "xcp.agent.update_identity_not_distinct",
            "The update must have a distinct version and bundle identity.",
            stage="run_live", field="update_project_dir",
            expected="same project id with distinct version and bundle SHA-256",
            actual=(
                f"base={base.project['version']}/{base.bundle_manifest_sha256}, "
                f"update={update.project['version']}/{update.bundle_manifest_sha256}"
            ),
            correction="prepare one explicit same-project companion update",
        )


def prepare_project(
    project_dir: pathlib.Path,
    bundle_dir: pathlib.Path,
    intent_path: pathlib.Path,
    *,
    host_profile_path: pathlib.Path | None,
    require_existing_bundle: bool = False,
    expected_bundle_manifest_sha256: str = "",
) -> PreparedProject:
    if bool(expected_bundle_manifest_sha256) != require_existing_bundle:
        raise AgentLifecycleError(
            "xcp.agent.prebuilt_bundle_contract_invalid",
            "Prebuilt mode and its pinned bundle identity must be enabled together.",
            stage="prepare", field="require_existing_bundle",
            expected="require_existing_bundle=true with one exact SHA, or neither",
            actual=(
                f"require_existing_bundle={require_existing_bundle}, "
                f"sha256={bool(expected_bundle_manifest_sha256)}"
            ),
            correction="use the explicit prebuilt contract or the legacy build path",
        )
    if require_existing_bundle and not expected_bundle_manifest_sha256:
        raise AgentLifecycleError(
            "xcp.agent.bundle_identity_missing",
            "Required prebuilt bundles must be pinned by exact manifest SHA-256.",
            stage="prepare", field="expected_bundle_manifest_sha256",
            expected="64 lowercase hexadecimal characters",
            actual="",
            correction="supply the promoted bundle manifest SHA-256",
        )
    if expected_bundle_manifest_sha256 and not HASH_PATTERN.fullmatch(
        expected_bundle_manifest_sha256
    ):
        raise AgentLifecycleError(
            "xcp.agent.bundle_identity_invalid",
            "The expected bundle identity must be a lowercase SHA-256.",
            stage="prepare", field="expected_bundle_manifest_sha256",
            expected="64 lowercase hexadecimal characters",
            actual=expected_bundle_manifest_sha256,
            correction="use the exact verified xcp-bundle.json SHA-256",
        )
    intent = validate_intent(intent_path)
    project_path = project_dir / PROJECT_FILENAME
    project = _load_json(project_path)
    if project.get("project_id") != intent["project_id"]:
        raise AgentLifecycleError(
            "xcp.agent.intent_project_mismatch",
            "The project id does not match the intent contract.",
            stage="prepare",
            field="project_id",
            expected=str(intent["project_id"]),
            actual=str(project.get("project_id")),
            correction="correct the project or select its matching intent",
        )
    validation = validate_project(project_dir, host_profile=host_profile_path)
    if bundle_dir.exists():
        verified = verify_bundle(bundle_dir)
        actual_bundle_sha = sha256_file(bundle_dir / BUNDLE_FILENAME)
        if (
            verified.manifest["project"]["id"] != project["project_id"]
            or verified.manifest["project"]["version"] != project["version"]
            or verified.manifest["integrity"]["project_manifest_sha256"]
            != sha256_file(project_path)
        ):
            raise AgentLifecycleError(
                "xcp.agent.bundle_stale",
                "The existing bundle does not bind the current project.",
                stage="prepare",
                field="bundle_dir",
                expected="bundle built from the exact project manifest",
                actual=str(bundle_dir),
                correction="choose a fresh bundle directory",
            )
        projected = _expected_bundle_manifest(validation)
        if verified.manifest != projected:
            raise AgentLifecycleError(
                "xcp.agent.bundle_stale",
                "The existing bundle content does not bind the exact project bytes.",
                stage="prepare", field="bundle_dir",
                expected="bundle manifest projected from every validated project file",
                actual=str(bundle_dir),
                correction="select the exact matching prebuilt bundle",
            )
        if (
            expected_bundle_manifest_sha256
            and actual_bundle_sha != expected_bundle_manifest_sha256
        ):
            raise AgentLifecycleError(
                "xcp.agent.bundle_identity_mismatch",
                "The prebuilt bundle manifest does not match the pinned identity.",
                stage="prepare", field="expected_bundle_manifest_sha256",
                expected=expected_bundle_manifest_sha256,
                actual=actual_bundle_sha,
                correction="select the exact promoted bundle bytes",
            )
        build = verified
        bundle_origin = "prebuilt"
    else:
        if require_existing_bundle:
            raise AgentLifecycleError(
                "xcp.agent.prebuilt_bundle_missing",
                "The required prebuilt bundle directory does not exist.",
                stage="prepare", field="bundle_dir",
                expected="existing verified prebuilt bundle",
                actual=str(bundle_dir),
                correction="select the exact promoted bundle directory",
            )
        build = build_bundle(
            project_dir,
            bundle_dir,
            host_profile=host_profile_path,
        )
        build = verify_bundle(build.output_dir)
        bundle_origin = "built"
    local_observation = {
        "schema_version": "xcp-agent-local-observation-v1",
        "project": {
            "project_id": project["project_id"],
            "version": project["version"],
            "title": project["title"],
            "entry_module": project["entry_module"],
            "module_count": len(project["modules"]),
            "asset_count": len(project["assets"]),
        },
        "validation": validation.metadata(),
        "bundle": build.metadata(),
    }
    acceptance = evaluate_acceptance(intent, local_observation, phase="local")
    if not _blocking_acceptance_passed(acceptance):
        failed = [
            item["assertion_id"]
            for item in acceptance
            if item["severity"] == "blocker" and item["outcome"] != "pass"
        ]
        raise AgentLifecycleError(
            "xcp.agent.acceptance_failed",
            "Local blocking acceptance assertions failed.",
            stage="acceptance",
            field="assertion_ids",
            expected="all blocker assertions pass",
            actual=",".join(failed),
            correction="correct the project from the assertion results",
            retryable=True,
        )
    manifest_path = bundle_dir / BUNDLE_FILENAME
    return PreparedProject(
        project=project,
        intent=intent,
        local_observation=local_observation,
        acceptance=acceptance,
        bundle_dir=bundle_dir,
        bundle_manifest=build.manifest,
        bundle_manifest_sha256=sha256_file(manifest_path),
        bundle_origin=bundle_origin,
        expected_bundle_manifest_sha256=expected_bundle_manifest_sha256,
    )


def _new_ledger(project_id: str, intent_sha256: str) -> dict[str, Any]:
    return {
        "schema_version": "xcp-agent-correction-ledger-v1",
        "ledger_id": f"{project_id}.corrections",
        "project_id": project_id,
        "intent_sha256": intent_sha256,
        "entries": [],
        "head_sha256": "",
        "status": "open",
    }


def _validate_ledger(
    ledger: Any,
    *,
    intent_sha256: str = "",
) -> dict[str, Any]:
    _validate_document(ledger, DEFAULT_LEDGER_SCHEMA, stage="correction")
    assert isinstance(ledger, dict)
    if intent_sha256 and ledger["intent_sha256"] != intent_sha256:
        raise AgentLifecycleError(
            "xcp.agent.ledger_intent_mismatch",
            "The correction ledger belongs to different intent bytes.",
            stage="correction",
            field="intent_sha256",
            expected=intent_sha256,
            actual=str(ledger["intent_sha256"]),
            correction="select the matching ledger or begin a fresh run",
        )
    previous = ""
    for expected_ordinal, entry in enumerate(ledger["entries"], start=1):
        if entry["ordinal"] != expected_ordinal:
            raise AgentLifecycleError(
                "xcp.agent.ledger_ordinal_invalid",
                "The correction ledger ordinal sequence is not contiguous.",
                stage="correction",
                field=f"entries[{expected_ordinal - 1}].ordinal",
                expected=str(expected_ordinal),
                actual=str(entry["ordinal"]),
                correction="reject the modified ledger and restore exact evidence",
            )
        if entry["previous_entry_sha256"] != previous:
            raise AgentLifecycleError(
                "xcp.agent.ledger_chain_invalid",
                "A correction entry does not bind the preceding entry.",
                stage="correction",
                field=f"entries[{expected_ordinal - 1}].previous_entry_sha256",
                expected=previous,
                actual=str(entry["previous_entry_sha256"]),
                correction="reject the modified ledger and restore exact evidence",
            )
        unsigned = dict(entry)
        reported = str(unsigned.pop("entry_sha256"))
        actual = sha256_bytes(canonical_json_bytes(unsigned))
        if reported != actual:
            raise AgentLifecycleError(
                "xcp.agent.ledger_entry_hash_invalid",
                "A correction entry hash does not match its canonical bytes.",
                stage="correction",
                field=f"entries[{expected_ordinal - 1}].entry_sha256",
                expected=actual,
                actual=reported,
                correction="reject the modified ledger and restore exact evidence",
            )
        previous = reported
    if ledger["head_sha256"] != previous:
        raise AgentLifecycleError(
            "xcp.agent.ledger_head_invalid",
            "The correction ledger head does not identify its final entry.",
            stage="correction",
            field="head_sha256",
            expected=previous,
            actual=str(ledger["head_sha256"]),
            correction="reject the modified ledger and restore exact evidence",
        )
    return ledger


def _append_correction(
    ledger: dict[str, Any],
    record: dict[str, Any],
) -> dict[str, Any]:
    _validate_ledger(ledger)
    if ledger["status"] != "open":
        raise AgentLifecycleError(
            "xcp.agent.ledger_closed",
            "A terminal correction ledger cannot be appended.",
            stage="correction",
            field="status",
            expected="open",
            actual=str(ledger["status"]),
            correction="start a new run ledger",
        )
    entry = {
        "ordinal": len(ledger["entries"]) + 1,
        "stage": record["stage"],
        "error": record["error"],
        "input_identity": record["input_identity"],
        "changes": record.get("changes", []),
        "change_summary": record.get(
            "change_summary",
            (
                f"Applied {len(record.get('changes', []))} exact file change(s)."
                if record.get("changes")
                else "Corrected the structured request without changing project bytes."
            ),
        ),
        "retry": record["retry"],
        "evidence_refs": record.get("evidence_refs", []),
        "previous_entry_sha256": ledger["head_sha256"],
    }
    entry["entry_sha256"] = sha256_bytes(canonical_json_bytes(entry))
    ledger = dict(ledger)
    ledger["entries"] = [*ledger["entries"], entry]
    ledger["head_sha256"] = entry["entry_sha256"]
    return _validate_ledger(ledger)


def _validate_pending_correction(
    record: Any,
    ledger: dict[str, Any],
    *,
    project_dir: pathlib.Path,
    update_project_dir: pathlib.Path,
    intent_path: pathlib.Path,
    host_profile_sha256: str,
) -> dict[str, Any]:
    if not isinstance(record, dict):
        raise AgentLifecycleError(
            "xcp.agent.pending_correction_invalid",
            "A pending external correction must be a JSON object.",
            stage="correction",
            field="pending_correction",
            expected="correction record object",
            actual=type(record).__name__,
            correction="provide the exact machine-readable correction record",
        )
    required = {"stage", "error", "input_identity", "changes", "retry"}
    missing = sorted(required.difference(record))
    if missing:
        raise AgentLifecycleError(
            "xcp.agent.pending_correction_invalid",
            "A pending external correction is missing required fields.",
            stage="correction",
            field="pending_correction",
            expected=",".join(sorted(required)),
            actual=",".join(missing),
            correction="complete the correction record from the structured failure",
        )
    if record["stage"] != "live" or record["retry"] != {
        "outcome": "not_run",
        "error_code": "",
    }:
        raise AgentLifecycleError(
            "xcp.agent.pending_correction_state_invalid",
            "Only an unconfirmed live correction may cross a retry boundary.",
            stage="correction",
            field="retry",
            expected='stage=live and retry={"outcome":"not_run","error_code":""}',
            actual=json.dumps(
                {
                    "stage": record.get("stage"),
                    "retry": record.get("retry"),
                },
                separators=(",", ":"),
                sort_keys=True,
            ),
            correction="leave the retry unconfirmed until the corrected launch passes",
        )
    identity = record["input_identity"]
    if (
        not isinstance(identity, dict)
        or identity.get("host_profile_sha256") != host_profile_sha256
    ):
        raise AgentLifecycleError(
            "xcp.agent.pending_correction_host_mismatch",
            "The pending correction was derived from another host profile.",
            stage="correction",
            field="input_identity.host_profile_sha256",
            expected=host_profile_sha256,
            actual=str(identity.get("host_profile_sha256"))
            if isinstance(identity, dict)
            else type(identity).__name__,
            correction="rediscover the host and derive a matching correction",
        )
    changes = record["changes"]
    if not isinstance(changes, list) or not changes:
        raise AgentLifecycleError(
            "xcp.agent.pending_correction_changes_missing",
            "A cross-run content correction must bind at least one changed file.",
            stage="correction",
            field="changes",
            expected="one or more exact before/after file identities",
            actual=type(changes).__name__
            if not isinstance(changes, list)
            else "empty",
            correction="bind every corrected project or intent file",
        )

    roots = {
        "base": project_dir.resolve(),
        "update": update_project_dir.resolve(),
    }
    for change in changes:
        path = str(change.get("path", "")) if isinstance(change, dict) else ""
        parts = pathlib.PurePosixPath(path).parts
        if path == "intent.json":
            target = intent_path.resolve()
        elif len(parts) >= 2 and parts[0] in roots:
            root = roots[parts[0]]
            target = root.joinpath(*parts[1:]).resolve()
            try:
                target.relative_to(root)
            except ValueError as exc:
                raise AgentLifecycleError(
                    "xcp.agent.pending_correction_path_invalid",
                    "A pending correction path escapes its project root.",
                    stage="correction",
                    field="changes.path",
                    expected="base/, update/ or intent.json",
                    actual=path,
                    correction="bind only exact project or intent files",
                ) from exc
        else:
            raise AgentLifecycleError(
                "xcp.agent.pending_correction_path_invalid",
                "A pending correction path is outside the retry inputs.",
                stage="correction",
                field="changes.path",
                expected="base/, update/ or intent.json",
                actual=path,
                correction="bind only exact project or intent files",
            )
        if not target.is_file():
            raise AgentLifecycleError(
                "xcp.agent.pending_correction_file_missing",
                "A file declared by the pending correction is absent.",
                stage="correction",
                field="changes.path",
                expected="corrected file exists",
                actual=path,
                correction="restore the exact corrected file",
            )
        actual_sha = sha256_file(target)
        expected_sha = str(change.get("after_sha256", ""))
        if not expected_sha or actual_sha != expected_sha:
            raise AgentLifecycleError(
                "xcp.agent.pending_correction_change_mismatch",
                "Corrected file bytes do not match the pending correction.",
                stage="correction",
                field="changes.after_sha256",
                expected=expected_sha,
                actual=actual_sha,
                correction="reject drift and retry the exact agent-authored correction",
            )

    # Exercise the authoritative ledger schema and hash-chain rules without
    # appending the unconfirmed record to evidence.
    _append_correction(ledger, record)
    return record


def _confirm_pending_correction(
    record: dict[str, Any],
    *,
    run_id: str,
) -> dict[str, Any]:
    confirmed = dict(record)
    confirmed["retry"] = {"outcome": "pass", "error_code": ""}
    evidence_refs = list(record.get("evidence_refs", []))
    launch_ref = f"live:{run_id}:launch"
    if launch_ref not in evidence_refs:
        evidence_refs.append(launch_ref)
    confirmed["evidence_refs"] = evidence_refs
    return confirmed


def _host_profile_from_response(response: dict[str, Any]) -> dict[str, Any]:
    profile = response.get("creative_host")
    if not isinstance(profile, dict):
        raise AgentLifecycleError(
            "xcp.agent.host_profile_missing",
            "describe_creative_host returned no creative_host object.",
            stage="discover",
            field="creative_host",
            expected="xcp-creative-host-profile-v1 object",
            actual=type(profile).__name__,
            correction="use a worker with the published creative host contract",
        )
    _validate_document(profile, DEFAULT_HOST_PROFILE_SCHEMA, stage="discover")
    return profile


def discover_host(session: WorkerSession) -> tuple[dict[str, Any], dict[str, Any]]:
    runtime = session.request({"command": "describe_runtime"})
    host_response = session.request({"command": "describe_creative_host"})
    profile = _host_profile_from_response(host_response)
    reported_hash = host_response.get("canonical_profile_sha256")
    actual_hash = sha256_bytes(canonical_json_bytes(profile))
    if reported_hash != actual_hash:
        raise AgentLifecycleError(
            "xcp.agent.host_profile_hash_mismatch",
            "The worker host profile hash does not match its canonical bytes.",
            stage="discover",
            field="canonical_profile_sha256",
            expected=actual_hash,
            actual=str(reported_hash),
            correction="fail closed and repair the worker profile projection",
        )
    return runtime, profile


def _publish_artifact(
    session: WorkerSession,
    *,
    artifact_id: str,
    artifact_kind: str,
    path: pathlib.Path,
) -> dict[str, Any]:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    session.request(
        {
            "command": "begin_artifact_upload",
            "artifact_id": artifact_id,
            "artifact_kind": artifact_kind,
            "expected_bytes": len(data),
            "expected_sha256": digest,
        }
    )
    try:
        for offset in range(0, len(data), ARTIFACT_CHUNK_BYTES):
            chunk = data[offset : offset + ARTIFACT_CHUNK_BYTES]
            session.request(
                {
                    "command": "append_artifact_chunk",
                    "artifact_id": artifact_id,
                    "offset": offset,
                    "data_base64": base64.b64encode(chunk).decode("ascii"),
                }
            )
        committed = session.request(
            {
                "command": "commit_artifact_upload",
                "artifact_id": artifact_id,
            }
        )
    except Exception:
        session.request(
            {
                "command": "abort_artifact_upload",
                "artifact_id": artifact_id,
            },
            allow_error=True,
        )
        raise
    if committed.get("sha256") != digest:
        raise AgentLifecycleError(
            "xcp.agent.artifact_hash_mismatch",
            "Committed worker artifact bytes do not match the local file.",
            stage="install",
            field="sha256",
            expected=digest,
            actual=str(committed.get("sha256")),
            correction="delete the staging artifact and retry exact upload",
        )
    return {
        "artifact_id": artifact_id,
        "path": str(path),
        "bytes": len(data),
        "sha256": digest,
    }


def _publish_bundle(
    session: WorkerSession,
    prepared: PreparedProject,
    prefix: str,
) -> dict[str, Any]:
    replayed = verify_bundle(prepared.bundle_dir)
    replayed_sha256 = sha256_file(prepared.bundle_dir / BUNDLE_FILENAME)
    expected_sha256 = (
        prepared.expected_bundle_manifest_sha256
        or prepared.bundle_manifest_sha256
    )
    if (
        replayed.manifest != prepared.bundle_manifest
        or replayed_sha256 != prepared.bundle_manifest_sha256
        or replayed_sha256 != expected_sha256
    ):
        raise AgentLifecycleError(
            "xcp.agent.bundle_changed_after_prepare",
            "Bundle bytes changed after preparation and before publication.",
            stage="install", field="bundle_sha256",
            expected=expected_sha256,
            actual=replayed_sha256,
            correction="abort without installing and use a fresh exact snapshot",
        )
    artifacts: list[dict[str, Any]] = []
    entries = [
        *prepared.bundle_manifest["modules"],
        *prepared.bundle_manifest["assets"],
    ]
    for index, entry in enumerate(entries):
        published = _publish_artifact(
                session,
                artifact_id=f"{prefix}-c{index}",
                artifact_kind="creative-content",
                path=prepared.bundle_dir / entry["path"],
            )
        artifacts.append(published)
        if (
            published["sha256"] != entry["sha256"]
            or published["bytes"] != entry["bytes"]
        ):
            _delete_staging(session, artifacts)
            raise AgentLifecycleError(
                "xcp.agent.bundle_content_changed_during_publish",
                "A bundle content file changed while it was being published.",
                stage="install", field=str(entry["path"]),
                expected=f"{entry['bytes']} bytes/{entry['sha256']}",
                actual=f"{published['bytes']} bytes/{published['sha256']}",
                correction="abort without installing and use a fresh exact snapshot",
            )
    bundle = _publish_artifact(
        session,
        artifact_id=f"{prefix}-bundle",
        artifact_kind="xcp-creative-bundle-v1",
        path=prepared.bundle_dir / BUNDLE_FILENAME,
    )
    artifacts.append(bundle)
    if bundle["sha256"] != expected_sha256:
        _delete_staging(session, artifacts)
        raise AgentLifecycleError(
            "xcp.agent.bundle_changed_during_publish",
            "The bundle manifest changed while it was being published.",
            stage="install", field="bundle_sha256",
            expected=expected_sha256,
            actual=bundle["sha256"],
            correction="abort without installing and use a fresh exact snapshot",
        )
    return {
        "bundle_artifact_id": bundle["artifact_id"],
        "bundle_sha256": bundle["sha256"],
        "artifacts": artifacts,
    }


def _delete_staging(
    session: WorkerSession,
    artifacts: Iterable[dict[str, Any]],
) -> None:
    for artifact in artifacts:
        session.request(
            {
                "command": "delete_artifact",
                "artifact_id": artifact["artifact_id"],
                "delete_unreferenced_blob": True,
            }
        )


def _install_bundle(
    session: WorkerSession,
    published: dict[str, Any],
    profile_sha256: str,
    expected: PreparedProject,
) -> dict[str, Any]:
    binding = {
        "bundle_artifact_id": published["bundle_artifact_id"],
        "expected_bundle_sha256": published["bundle_sha256"],
        "expected_host_profile_sha256": profile_sha256,
    }
    prepared = session.request(
        {
            "command": "prepare_creative_install",
            "schema_version": "xcp-creative-prepare-install-request-v1",
            **binding,
        }
    )
    if prepared.get("ready_to_commit") is not True or prepared.get(
        "missing_count"
    ) != 0:
        raise AgentLifecycleError(
            "xcp.agent.install_not_ready",
            "The worker did not admit all exact bundle content.",
            stage="install",
            field="missing_count",
            expected="0 and ready_to_commit=true",
            actual=str(prepared.get("missing_count")),
            correction="upload the exact missing content and retry prepare",
            retryable=True,
        )
    expected_identity = {
        "project_id": expected.project["project_id"],
        "project_version": expected.project["version"],
        "bundle_sha256": expected.bundle_manifest_sha256,
        "content_sha256": expected.bundle_manifest["integrity"]["content_sha256"],
        "file_count": (
            len(expected.bundle_manifest["modules"])
            + len(expected.bundle_manifest["assets"])
        ),
    }
    for field, value in expected_identity.items():
        if prepared.get(field) != value:
            raise AgentLifecycleError(
                "xcp.agent.install_identity_mismatch",
                "The worker prepare response does not echo the exact bundle identity.",
                stage="install", field=field,
                expected=str(value),
                actual=str(prepared.get(field)),
                correction="abort without commit and repair the worker install contract",
            )
    committed = session.request(
        {
            "command": "commit_creative_install",
            "schema_version": "xcp-creative-commit-install-request-v1",
            **binding,
        }
    )
    for field, value in expected_identity.items():
        if committed.get(field) != value:
            raise AgentLifecycleError(
                "xcp.agent.install_identity_mismatch",
                "The committed install does not bind the exact prepared bundle identity.",
                stage="install", field=field,
                expected=str(value),
                actual=str(committed.get(field)),
                correction="abort activation and repair the worker install contract",
            )
    if committed.get("validated") is not True or not committed.get("install_id"):
        raise AgentLifecycleError(
            "xcp.agent.install_commit_invalid",
            "The worker did not return a validated exact install identity.",
            stage="install", field="validated/install_id",
            expected="validated=true and non-empty install_id",
            actual=f"{committed.get('validated')}/{committed.get('install_id')}",
            correction="abort activation and repair the worker install contract",
        )
    listing = _list_installs(session)
    matches = [
        item
        for item in listing.get("installs", [])
        if item.get("install_id") == committed["install_id"]
    ]
    if len(matches) != 1:
        raise AgentLifecycleError(
            "xcp.agent.install_listing_mismatch",
            "The exact committed install is not uniquely present in worker listing.",
            stage="install", field="install_id",
            expected=str(committed["install_id"]),
            actual=str([item.get("install_id") for item in matches]),
            correction="abort activation and repair the installed-record listing",
        )
    listed = matches[0]
    listing_identity = {
        "project_id": expected_identity["project_id"],
        "project_version": expected_identity["project_version"],
        "bundle_sha256": expected_identity["bundle_sha256"],
        "content_sha256": expected_identity["content_sha256"],
        "file_count": expected_identity["file_count"],
    }
    for field, value in listing_identity.items():
        if listed.get(field) != value:
            raise AgentLifecycleError(
                "xcp.agent.install_listing_mismatch",
                "Worker listing does not bind the committed exact bundle identity.",
                stage="install", field=field,
                expected=str(value),
                actual=str(listed.get(field)),
                correction="abort activation and repair the installed-record listing",
            )
    return committed


def _list_installs(session: WorkerSession) -> dict[str, Any]:
    return session.request(
        {
            "command": "list_creative_installs",
            "schema_version": "xcp-creative-list-installs-request-v1",
        }
    )


def _activation_state(
    listing: dict[str, Any],
    project_id: str,
) -> tuple[str, str]:
    active = ""
    previous = ""
    for install in listing.get("installs", []):
        if install.get("project_id") != project_id:
            continue
        if install.get("active") is True:
            active = str(install["install_id"])
        if install.get("rollback_target") is True:
            previous = str(install["install_id"])
    return active, previous


def _cleanup_rollback_target(
    session: WorkerSession,
    *,
    project_id: str,
    base_install_id: str,
    update_install_id: str,
    profile_sha256: str,
    project_sha256: str,
    bundle_sha256: str,
    run_id: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    listing = _list_installs(session)
    active, previous = _activation_state(listing, project_id)
    expected_ids = {base_install_id, update_install_id}
    if active != base_install_id or previous != update_install_id:
        raise AgentLifecycleError(
            "xcp.agent.cleanup_state_invalid",
            "The post-rollback activation window is not the exact bounded pair.",
            stage="cleanup",
            field="active_install_id/rollback_target_install_id",
            expected=f"{base_install_id}/{update_install_id}",
            actual=f"{active}/{previous}",
            correction="restore the exact base/update rollback window and retry",
        )

    project_installs = [
        install
        for install in listing.get("installs", [])
        if install.get("project_id") == project_id
    ]
    stale_install_ids: list[str] = []
    for install in project_installs:
        install_id = str(install.get("install_id", ""))
        if install_id in expected_ids:
            continue
        if not install_id or install.get("active") is True or (
            install.get("rollback_target") is True
        ):
            raise AgentLifecycleError(
                "xcp.agent.cleanup_stale_install_unsafe",
                "A stale install is missing identity or remains referenced.",
                stage="cleanup",
                field="installs",
                expected="identified inactive non-rollback install",
                actual=json.dumps(install, separators=(",", ":"), sort_keys=True),
                correction="stop and inspect the project activation state",
            )
        stale_install_ids.append(install_id)

    stale_removals: list[dict[str, Any]] = []
    for stale_install_id in sorted(stale_install_ids):
        removed = session.request(
            {
                "command": "remove_creative_install",
                "schema_version": "xcp-creative-remove-install-request-v1",
                "install_id": stale_install_id,
                "delete_unreferenced_blobs": True,
            }
        )
        if (
            removed.get("ok") is not True
            or removed.get("install_id") != stale_install_id
            or removed.get("existed") is not True
            or removed.get("removed") is not True
        ):
            raise AgentLifecycleError(
                "xcp.agent.cleanup_stale_install_failed",
                "The worker did not remove an exact stale project install.",
                stage="cleanup",
                field="remove_creative_install",
                expected=f"removed=true for {stale_install_id}",
                actual=json.dumps(
                    removed,
                    separators=(",", ":"),
                    sort_keys=True,
                ),
                correction="stop and inspect the immutable install records",
            )
        stale_removals.append(
            {
                "install_id": stale_install_id,
                "removed_blob_count": int(
                    removed.get("removed_blob_count", 0)
                ),
                "removed_blob_bytes": int(
                    removed.get("removed_blob_bytes", 0)
                ),
            }
        )

    bounded_listing = _list_installs(session)
    bounded_active, bounded_previous = _activation_state(
        bounded_listing,
        project_id,
    )
    bounded_install_ids = {
        str(install.get("install_id", ""))
        for install in bounded_listing.get("installs", [])
        if install.get("project_id") == project_id
    }
    if (
        bounded_active != base_install_id
        or bounded_previous != update_install_id
        or bounded_install_ids != expected_ids
    ):
        raise AgentLifecycleError(
            "xcp.agent.cleanup_state_invalid",
            "Stale-install cleanup did not produce the exact bounded pair.",
            stage="cleanup",
            field="project_install_ids",
            expected=",".join(sorted(expected_ids)),
            actual=",".join(sorted(bounded_install_ids)),
            correction="stop and restore the exact base/update rollback window",
        )

    removal_probe = session.request(
        {
            "command": "remove_creative_install",
            "schema_version": "xcp-creative-remove-install-request-v1",
            "install_id": update_install_id,
            "delete_unreferenced_blobs": True,
        },
        allow_error=True,
    )
    error = removal_probe.get("error")
    details = error.get("details") if isinstance(error, dict) else None
    code = error.get("code") if isinstance(error, dict) else None
    if (
        removal_probe.get("ok") is not False
        or code != "xcp.creative.install_referenced"
        or not isinstance(details, dict)
        or details.get("field") != "install_id"
        or details.get("actual") != "rollback_target"
    ):
        raise AgentLifecycleError(
            "xcp.agent.cleanup_boundary_changed",
            "The worker did not preserve the exact rollback target boundary.",
            stage="cleanup",
            field="error.code/error.details",
            expected=(
                "xcp.creative.install_referenced with "
                "field=install_id and actual=rollback_target"
            ),
            actual=f"{code}/{details}",
            correction="stop and inspect the worker lifecycle contract",
        )

    verified_listing = _list_installs(session)
    verified_active, verified_previous = _activation_state(
        verified_listing,
        project_id,
    )
    if (
        verified_active != base_install_id
        or verified_previous != update_install_id
    ):
        raise AgentLifecycleError(
            "xcp.agent.cleanup_state_changed",
            "The cleanup probe changed the protected rollback window.",
            stage="cleanup",
            field="active_install_id/rollback_target_install_id",
            expected=f"{base_install_id}/{update_install_id}",
            actual=f"{verified_active}/{verified_previous}",
            correction="stop and restore the exact rollback state",
        )

    cleanup = {
        "schema_version": "xcp-agent-cleanup-v1",
        "decision": "pass",
        "active_install_id": verified_active,
        "rollback_target_install_id": verified_previous,
        "retained_as_rollback_target": True,
        "bounded_install_count": len(bounded_install_ids),
        "stale_install_count": len(stale_removals),
        "stale_removals": stale_removals,
        "staging_artifacts_removed": True,
        "removal_probe": {
            "ok": False,
            "error": {
                "code": code,
                "details": details,
            },
        },
    }
    correction = {
        "stage": "live",
        "change_summary": (
            "Corrected cleanup from deleting the protected rollback target "
            "to verifying the exact bounded base/update rollback window."
        ),
        "error": {
            "code": code,
            "details": details,
        },
        "input_identity": {
            "project_sha256": project_sha256,
            "bundle_sha256": bundle_sha256,
            "host_profile_sha256": profile_sha256,
        },
        "changes": [],
        "retry": {
            "outcome": "pass",
            "error_code": "",
        },
        "evidence_refs": [
            (
                f"live:{run_id}:cleanup"
                "#error=xcp.creative.install_referenced"
            ),
            (
                f"live:{run_id}:cleanup"
                "#state=exact_rollback_window_retained"
            ),
        ],
    }
    return cleanup, correction


def _activate(
    session: WorkerSession,
    project_id: str,
    install_id: str,
    transition_id: str,
) -> dict[str, Any]:
    active, previous = _activation_state(_list_installs(session), project_id)
    return session.request(
        {
            "command": "activate_creative_install",
            "schema_version": "xcp-creative-activate-install-request-v1",
            "project_id": project_id,
            "install_id": install_id,
            "transition_id": transition_id,
            "expected_active_install_id": active,
            "expected_previous_install_id": previous,
        }
    )


def _observe(session: WorkerSession) -> dict[str, Any]:
    return session.request(
        {
            "command": "observe_creative_foreground",
            "schema_version": "xcp-creative-observation-request-v1",
            "project_id": "",
            "expected_install_id": "",
            "expected_plan_sha256": "",
        }
    )


def _launch(
    session: WorkerSession,
    *,
    project_id: str,
    install_id: str,
    launch_id: str,
    profile_sha256: str,
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    foreground = _observe(session)
    expected_head = (
        str(foreground.get("launch_record_sha256", ""))
        if foreground.get("active") is True
        else ""
    )
    request = {
        "command": "launch_creative_project",
        "schema_version": "xcp-creative-launch-request-v1",
        "expected_host_profile_sha256": profile_sha256,
        "project_id": project_id,
        "install_id": install_id,
        "launch_id": launch_id,
        "expected_launch_record_sha256": expected_head,
    }
    result = session.request(request, allow_error=True)
    if result.get("ok") is True:
        return result, None
    error = result.get("error", {})
    details = error.get("details", {}) if isinstance(error, dict) else {}
    reported_head = details.get("expected")
    if (
        error.get("code") == "xcp.creative.foreground_state_conflict"
        and details.get("field") == "expected_launch_record_sha256"
        and details.get("actual") == expected_head
        and isinstance(reported_head, str)
        and HASH_PATTERN.fullmatch(reported_head)
    ):
        request["expected_launch_record_sha256"] = reported_head
        corrected = session.request(request)
        correction = {
            "stage": "live",
            "error": {
                "code": error["code"],
                "details": details,
            },
            "input_identity": {
                "project_sha256": "",
                "bundle_sha256": "",
                "host_profile_sha256": profile_sha256,
            },
            "changes": [],
            "retry": {
                "outcome": "pass",
                "error_code": "",
            },
            "evidence_refs": [f"worker:{launch_id}"],
        }
        return corrected, correction
    raise WorkerTransportError(
        str(error.get("code", "worker.unknown")),
        str(error.get("message", "Worker launch failed.")),
        details=details if isinstance(details, dict) else {},
        response=result,
    )


def _reload(
    session: WorkerSession,
    *,
    project_id: str,
    install_id: str,
    launch_id: str,
    profile_sha256: str,
) -> dict[str, Any]:
    foreground = _observe(session)
    return session.request(
        {
            "command": "reload_creative_project",
            "schema_version": "xcp-creative-reload-request-v1",
            "expected_host_profile_sha256": profile_sha256,
            "project_id": project_id,
            "expected_active_install_id": install_id,
            "launch_id": launch_id,
            "expected_launch_record_sha256": str(
                foreground.get("launch_record_sha256", "")
            ),
        }
    )


def _capture(
    session: WorkerSession,
    *,
    foreground: dict[str, Any],
    capture_id: str,
    profile_sha256: str,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    response = session.request(
        {
            "command": "capture_creative_frame",
            "schema_version": "xcp-creative-frame-capture-request-v1",
            "expected_host_profile_sha256": profile_sha256,
            "project_id": foreground["project_id"],
            "expected_install_id": foreground["install_id"],
            "expected_plan_sha256": foreground["plan_sha256"],
            "capture_id": capture_id,
        }
    )
    data = base64.b64decode(response["data_base64"], validate=True)
    content_bytes = response.get("content_bytes")
    if (
        isinstance(content_bytes, bool)
        or not isinstance(content_bytes, int)
        or content_bytes != len(data)
    ):
        raise AgentLifecycleError(
            "xcp.agent.capture_size_mismatch",
            "The captured frame size does not match the worker receipt.",
            stage="capture",
            field="content_bytes",
            expected=str(len(data)),
            actual=str(content_bytes),
            correction="discard the capture and retry",
        )
    digest = hashlib.sha256(data).hexdigest()
    if digest != response.get("content_sha256"):
        raise AgentLifecycleError(
            "xcp.agent.capture_hash_mismatch",
            "The captured frame bytes do not match worker identity.",
            stage="capture",
            field="content_sha256",
            expected=str(response.get("content_sha256")),
            actual=digest,
            correction="discard the capture and retry",
        )
    if output_path.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "Capture output already exists.",
            stage="capture",
            field="output_path",
            expected="a fresh path",
            actual=str(output_path),
            correction="choose a fresh output path",
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(data)
    result = dict(response)
    del result["data_base64"]
    result["output_path"] = str(output_path.resolve())
    return result


def _dispatch_interactions(
    session: WorkerSession,
    *,
    intent: dict[str, Any],
    foreground: dict[str, Any],
    profile_sha256: str,
) -> dict[str, Any]:
    sequences: list[dict[str, Any]] = []
    current = foreground
    for sequence in intent.get("interaction_sequences", []):
        dispatches: list[dict[str, Any]] = []
        for step in sequence["steps"]:
            remaining = int(step["repeat"])
            while remaining:
                repeat = min(remaining, 64)
                response = session.request(
                    {
                        "command": "dispatch_creative_input",
                        "schema_version": (
                            "xcp-creative-input-dispatch-request-v1"
                        ),
                        "expected_host_profile_sha256": profile_sha256,
                        "project_id": current["project_id"],
                        "expected_install_id": current["install_id"],
                        "expected_plan_sha256": current["plan_sha256"],
                        "action": step["action"],
                        "repeat": repeat,
                    }
                )
                dispatches.append(
                    {
                        "action": step["action"],
                        "repeat": repeat,
                        "changed": bool(response.get("changed")),
                        "state_revision": int(
                            response.get("state_revision", 0)
                        ),
                    }
                )
                remaining -= repeat
            wait_ms = int(step["wait_ms"])
            if wait_ms:
                time.sleep(wait_ms / 1000.0)
        current = _observe(session)
        sequences.append(
            {
                "id": sequence["id"],
                "assertion_ids": sequence["assertion_ids"],
                "dispatches": dispatches,
                "observation": current,
            }
        )
    return {
        "schema_version": "xcp-agent-interaction-result-v1",
        "sequence_count": len(sequences),
        "sequences": sequences,
        "observation": current,
    }


def _prove_structured_live_correction(
    session: WorkerSession,
    *,
    intent: dict[str, Any],
    foreground: dict[str, Any],
    profile_sha256: str,
    project_sha256: str,
    bundle_sha256: str,
) -> dict[str, Any]:
    sequences = intent.get("interaction_sequences", [])
    if not sequences or not sequences[0]["steps"]:
        raise AgentLifecycleError(
            "xcp.agent.live_correction_probe_unavailable",
            "A controlled live correction proof requires a declared interaction.",
            stage="live",
            field="interaction_sequences",
            expected="at least one declared semantic action",
            actual="empty",
            correction="declare a bounded interaction sequence in the intent",
        )
    valid_action = str(sequences[0]["steps"][0]["action"])
    invalid_action = "xcp.agent.probe.undefined"
    request = {
        "command": "dispatch_creative_input",
        "schema_version": "xcp-creative-input-dispatch-request-v1",
        "expected_host_profile_sha256": profile_sha256,
        "project_id": foreground["project_id"],
        "expected_install_id": foreground["install_id"],
        "expected_plan_sha256": foreground["plan_sha256"],
        "action": invalid_action,
        "repeat": 1,
    }
    rejected = session.request(request, allow_error=True)
    error = rejected.get("error", {})
    details = error.get("details", {}) if isinstance(error, dict) else {}
    if (
        rejected.get("ok") is not False
        or error.get("code") != "xcp.creative.input_action_unsupported"
        or not isinstance(details, dict)
        or details.get("field") != "action"
    ):
        raise AgentLifecycleError(
            "xcp.agent.live_correction_probe_invalid",
            "The controlled negative input did not produce the published structured error.",
            stage="live",
            field="error.code/error.details.field",
            expected="xcp.creative.input_action_unsupported/action",
            actual=f"{error.get('code', '')}/{details.get('field', '')}",
            correction="repair the worker structured input contract before proceeding",
        )
    request["action"] = valid_action
    corrected = session.request(request)
    return {
        "stage": "live",
        "error": {
            "code": str(error["code"]),
            "details": details,
        },
        "input_identity": {
            "project_sha256": project_sha256,
            "bundle_sha256": bundle_sha256,
            "host_profile_sha256": profile_sha256,
        },
        "changes": [],
        "change_summary": (
            f"Replaced undeclared action {invalid_action} with the declared "
            f"semantic action {valid_action}."
        ),
        "retry": {
            "outcome": "pass",
            "error_code": "",
        },
        "evidence_refs": [
            f"worker:dispatch_creative_input#error={error['code']}",
            f"worker:dispatch_creative_input#action={valid_action}",
        ],
        "retry_response": corrected,
    }


def _operation(
    operations: list[dict[str, Any]],
    name: str,
    response: dict[str, Any],
) -> None:
    operations.append(
        {
            "ordinal": len(operations) + 1,
            "name": name,
            "outcome": "pass",
            "response_schema_version": str(response.get("schema_version", "")),
            "error_code": "",
            "evidence_refs": [],
        }
    )


def _operation_not_run(
    operations: list[dict[str, Any]],
    name: str,
    evidence_ref: str,
) -> None:
    operations.append(
        {
            "ordinal": len(operations) + 1,
            "name": name,
            "outcome": "not_run",
            "response_schema_version": "",
            "error_code": "",
            "evidence_refs": [evidence_ref],
        }
    )


def run_preview(
    *,
    endpoint: WorkerEndpoint,
    pairing_code: str,
    session_id: str,
    project_dir: pathlib.Path,
    bundle_root: pathlib.Path,
    intent_path: pathlib.Path,
    capture_root: pathlib.Path,
    receipt_path: pathlib.Path,
    ledger_path: pathlib.Path,
    run_id: str,
) -> dict[str, Any]:
    """Run the authoritative single-project path used by interactive preview.

    This deliberately stops after the current project is installed, launched,
    observed, interacted with and captured. Update and rollback remain the
    responsibility of ``run-live`` and are never simulated with a duplicate
    project version.
    """
    _strict_identifier(run_id, "run_id", maximum=RUN_ID_MAX_CHARS)
    if bundle_root.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The preview bundle root must be fresh.",
            stage="run_preview",
            field="bundle_root",
            expected="a new directory",
            actual=str(bundle_root),
            correction="retry; Studio will allocate a fresh preview run",
        )
    if capture_root.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The preview capture root must be fresh.",
            stage="run_preview",
            field="capture_root",
            expected="a new directory",
            actual=str(capture_root),
            correction="retry; Studio will allocate a fresh preview run",
        )

    host_profile_path = bundle_root.parent / f".{run_id}-host-profile.json"
    if receipt_path.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The preview receipt path must be fresh.",
            stage="run_preview",
            field="receipt_path",
            expected="a new file",
            actual=str(receipt_path),
            correction="retry; Studio will allocate a fresh preview run",
        )
    if host_profile_path.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The transient host profile path must be fresh.",
            stage="run_preview",
            field="host_profile_path",
            expected="a new file",
            actual=str(host_profile_path),
            correction="retry with a fresh preview run",
        )
    output_paths = [
        bundle_root,
        capture_root,
        receipt_path,
        ledger_path,
        host_profile_path,
    ]
    for index, first in enumerate(output_paths):
        for second in output_paths[index + 1 :]:
            if _paths_overlap(first, second):
                raise AgentLifecycleError(
                    "xcp.agent.live_output_overlap",
                    "Preview output paths must be pairwise disjoint.",
                    stage="run_preview",
                    field="output_paths",
                    expected="disjoint bundle, capture, receipt, ledger and transient paths",
                    actual=f"{first} / {second}",
                    correction="keep preview outputs in separate paths",
                )

    intent = validate_intent(intent_path)
    intent_sha = sha256_file(intent_path)
    if ledger_path.exists():
        ledger = _load_json(ledger_path)
        ledger = _validate_ledger(ledger, intent_sha256=intent_sha)
        if ledger["project_id"] != intent["project_id"]:
            raise AgentLifecycleError(
                "xcp.agent.ledger_project_mismatch",
                "Correction ledger belongs to another project.",
                stage="correction",
                field="project_id",
                expected=intent["project_id"],
                actual=str(ledger["project_id"]),
                correction="select the matching ledger",
            )
    else:
        ledger = _new_ledger(intent["project_id"], intent_sha)

    operations: list[dict[str, Any]] = []
    artifact_receipts: list[dict[str, Any]] = []
    profile_sha = ""
    final_observation: dict[str, Any] = {}
    prepared: PreparedProject | None = None
    install: dict[str, Any] | None = None

    client = WorkerJsonClient(endpoint)
    with WorkerSession(
        client,
        pairing_code=pairing_code,
        session_id=session_id,
    ) as worker:
        runtime, profile = discover_host(worker)
        profile_sha = sha256_bytes(canonical_json_bytes(profile))
        _operation(operations, "discover", runtime)

        host_profile_path.parent.mkdir(parents=True, exist_ok=True)
        host_profile_path.write_bytes(canonical_json_bytes(profile))
        try:
            prepared = prepare_project(
                project_dir,
                bundle_root / "current",
                intent_path,
                host_profile_path=host_profile_path,
            )
        finally:
            host_profile_path.unlink(missing_ok=True)

        _operation(operations, "validate", prepared.local_observation["validation"])
        _operation(operations, "build", prepared.local_observation["bundle"])
        _operation(operations, "verify", prepared.local_observation["bundle"])

        published = _publish_bundle(worker, prepared, f"{run_id}-current")
        try:
            install = _install_bundle(worker, published, profile_sha, prepared)
        finally:
            _delete_staging(worker, published["artifacts"])
        _operation(operations, "install", install)
        activation = _activate(
            worker,
            prepared.project["project_id"],
            str(install["install_id"]),
            f"{run_id}-activate",
        )
        _operation(operations, "activate", activation)
        launch, correction = _launch(
            worker,
            project_id=prepared.project["project_id"],
            install_id=str(install["install_id"]),
            launch_id=f"{run_id}-launch",
            profile_sha256=profile_sha,
        )
        if correction:
            correction["input_identity"]["project_sha256"] = sha256_file(
                project_dir / PROJECT_FILENAME
            )
            correction["input_identity"]["bundle_sha256"] = (
                prepared.bundle_manifest_sha256
            )
            ledger = _append_correction(ledger, correction)
        _operation(operations, "launch", launch)

        observation = _observe(worker)
        _operation(operations, "observe", observation)
        interaction = _dispatch_interactions(
            worker,
            intent=intent,
            foreground=observation,
            profile_sha256=profile_sha,
        )
        _operation(operations, "interact", interaction)
        final_observation = interaction["observation"]
        capture_path = capture_root / "preview.bgra"
        capture = _capture(
            worker,
            foreground=final_observation,
            capture_id=f"{run_id}-capture",
            profile_sha256=profile_sha,
            output_path=capture_path,
        )
        _operation(operations, "capture", capture)
        artifact_receipts.append(
            {
                "role": "creative.capture",
                "reference": str(capture_path.resolve()),
                "bytes": int(capture["content_bytes"]),
                "sha256": str(capture["content_sha256"]),
            }
        )

    assert prepared is not None
    assert install is not None
    live_acceptance = evaluate_acceptance(
        prepared.intent,
        final_observation,
        phase="live",
    )
    all_acceptance = [*prepared.acceptance, *live_acceptance]
    passed = _blocking_acceptance_passed(all_acceptance)
    ledger["status"] = "passed" if passed else "failed"
    _validate_ledger(ledger, intent_sha256=intent_sha)
    if ledger_path.exists():
        _replace_json(ledger_path, ledger)
    else:
        _write_new_json(ledger_path, ledger)
    ledger_sha = sha256_file(ledger_path)

    receipt = {
        "schema_version": "xcp-agent-lifecycle-receipt-v1",
        "run_id": run_id,
        "project_id": prepared.project["project_id"],
        "project_version": prepared.project["version"],
        "intent_sha256": sha256_file(intent_path),
        "project_sha256": sha256_file(project_dir / PROJECT_FILENAME),
        "bundle_sha256": prepared.bundle_manifest_sha256,
        "host_profile_sha256": profile_sha,
        "install_id": str(install["install_id"]),
        "plan_sha256": str(final_observation.get("plan_sha256", "")),
        "operations": operations,
        "acceptance": all_acceptance,
        "artifacts": artifact_receipts,
        "correction_ledger_sha256": ledger_sha,
        "decision": "pass" if passed else "fail",
    }
    _validate_document(receipt, DEFAULT_RECEIPT_SCHEMA, stage="receipt")
    _write_new_json(receipt_path, receipt)
    if not passed:
        raise AgentLifecycleError(
            "xcp.agent.acceptance_failed",
            "The preview completed but blocking intent acceptance failed.",
            stage="acceptance",
            field="receipt",
            expected="all blocker assertions pass",
            actual=str(receipt_path),
            correction="open Details, correct the project, and retry",
            retryable=True,
        )
    return receipt


def run_live(
    *,
    endpoint: WorkerEndpoint,
    pairing_code: str,
    session_id: str,
    project_dir: pathlib.Path,
    update_project_dir: pathlib.Path,
    bundle_root: pathlib.Path,
    intent_path: pathlib.Path,
    capture_root: pathlib.Path,
    receipt_path: pathlib.Path,
    ledger_path: pathlib.Path,
    run_id: str,
    base_prebuilt_bundle_dir: pathlib.Path | None = None,
    base_expected_bundle_sha256: str = "",
    update_prebuilt_bundle_dir: pathlib.Path | None = None,
    update_expected_bundle_sha256: str = "",
    prove_structured_live_correction: bool = False,
    pending_correction_path: pathlib.Path | None = None,
) -> dict[str, Any]:
    _strict_identifier(run_id, "run_id", maximum=RUN_ID_MAX_CHARS)
    base_uses_prebuilt = _prebuilt_requested(
        base_prebuilt_bundle_dir,
        base_expected_bundle_sha256,
        field="base_prebuilt_bundle_dir/base_expected_bundle_sha256",
    )
    update_uses_prebuilt = _prebuilt_requested(
        update_prebuilt_bundle_dir,
        update_expected_bundle_sha256,
        field="update_prebuilt_bundle_dir/update_expected_bundle_sha256",
    )
    if bundle_root.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The live bundle root must be fresh.",
            stage="run_live",
            field="bundle_root",
            expected="a new directory",
            actual=str(bundle_root),
            correction="choose a fresh run root",
        )
    if capture_root.exists():
        raise AgentLifecycleError(
            "xcp.agent.output_exists",
            "The live capture root must be fresh.",
            stage="run_live",
            field="capture_root",
            expected="a new directory",
            actual=str(capture_root),
            correction="choose a fresh capture root",
        )
    prebuilt_sources: list[pathlib.Path] = []
    if base_uses_prebuilt:
        assert base_prebuilt_bundle_dir is not None
        prebuilt_sources.extend([project_dir, base_prebuilt_bundle_dir])
    if update_uses_prebuilt:
        assert update_prebuilt_bundle_dir is not None
        prebuilt_sources.extend(
            [update_project_dir, update_prebuilt_bundle_dir]
        )
    host_profile_path = bundle_root.parent / f".{run_id}-host-profile.json"
    output_paths = [
        bundle_root,
        capture_root,
        receipt_path,
        ledger_path,
        host_profile_path,
    ]
    for index, first in enumerate(output_paths):
        for second in output_paths[index + 1 :]:
            if _paths_overlap(first, second):
                raise AgentLifecycleError(
                    "xcp.agent.live_output_overlap",
                    "Live output paths must be pairwise disjoint.",
                    stage="run_live", field="output_paths",
                    expected="disjoint bundle, capture, receipt, ledger and transient paths",
                    actual=f"{first} / {second}",
                    correction="choose separate fresh output paths",
                )
    for source in prebuilt_sources:
        for output in output_paths:
            if _paths_overlap(source, output):
                raise AgentLifecycleError(
                    "xcp.agent.prebuilt_output_overlap",
                    "A promoted prebuilt bundle cannot overlap live output paths.",
                    stage="run_live", field="prebuilt_bundle_dir",
                    expected="disjoint promoted input and fresh output paths",
                    actual=f"{source} / {output}",
                    correction="choose output roots outside promoted evidence",
                )
    intent = validate_intent(intent_path)
    intent_sha = sha256_file(intent_path)
    if ledger_path.exists():
        ledger = _load_json(ledger_path)
        ledger = _validate_ledger(ledger, intent_sha256=intent_sha)
        if ledger["project_id"] != intent["project_id"]:
            raise AgentLifecycleError(
                "xcp.agent.ledger_project_mismatch",
                "Correction ledger belongs to another project.",
                stage="correction", field="project_id",
                expected=intent["project_id"],
                actual=str(ledger["project_id"]),
                correction="select the matching ledger",
            )
    else:
        ledger = _new_ledger(intent["project_id"], intent_sha)
    effective_project_dir = project_dir
    effective_update_project_dir = update_project_dir
    base_bundle_dir = bundle_root / "base"
    update_bundle_dir = bundle_root / "update"
    try:
        if base_uses_prebuilt:
            assert base_prebuilt_bundle_dir is not None
            effective_project_dir, base_bundle_dir = _snapshot_prebuilt_stage(
                project_source=project_dir,
                bundle_source=base_prebuilt_bundle_dir,
                bundle_root=bundle_root,
                role="base",
            )
        if update_uses_prebuilt:
            assert update_prebuilt_bundle_dir is not None
            effective_update_project_dir, update_bundle_dir = _snapshot_prebuilt_stage(
                project_source=update_project_dir,
                bundle_source=update_prebuilt_bundle_dir,
                bundle_root=bundle_root,
                role="update",
            )
    except Exception:
        if bundle_root.exists() and (base_uses_prebuilt or update_uses_prebuilt):
            try:
                shutil.rmtree(bundle_root)
            except OSError:
                pass
        raise

    preflight_base: PreparedProject | None = None
    preflight_update: PreparedProject | None = None
    if base_uses_prebuilt:
        preflight_base = prepare_project(
            effective_project_dir,
            base_bundle_dir,
            intent_path,
            host_profile_path=None,
            require_existing_bundle=True,
            expected_bundle_manifest_sha256=base_expected_bundle_sha256,
        )
    if update_uses_prebuilt:
        preflight_update = prepare_project(
            effective_update_project_dir,
            update_bundle_dir,
            intent_path,
            host_profile_path=None,
            require_existing_bundle=True,
            expected_bundle_manifest_sha256=update_expected_bundle_sha256,
        )
    if preflight_base is not None and preflight_update is not None:
        _validate_update_pair(preflight_base, preflight_update)

    operations: list[dict[str, Any]] = []
    artifact_receipts: list[dict[str, Any]] = []
    base_install: dict[str, Any] | None = None
    update_install: dict[str, Any] | None = None
    profile_sha = ""
    final_observation: dict[str, Any] = {}
    base: PreparedProject | None = None
    update: PreparedProject | None = None
    pending_correction: dict[str, Any] | None = None

    client = WorkerJsonClient(endpoint)
    with WorkerSession(
        client,
        pairing_code=pairing_code,
        session_id=session_id,
    ) as worker:
        runtime, profile = discover_host(worker)
        profile_sha = sha256_bytes(canonical_json_bytes(profile))
        _operation(operations, "discover", runtime)

        if host_profile_path.exists():
            raise AgentLifecycleError(
                "xcp.agent.output_exists",
                "The transient host profile path already exists.",
                stage="run_live",
                field="host_profile_path",
                expected="fresh path",
                actual=str(host_profile_path),
                correction="choose another run id",
            )
        host_profile_path.parent.mkdir(parents=True, exist_ok=True)
        host_profile_path.write_bytes(canonical_json_bytes(profile))
        try:
            base = prepare_project(
                effective_project_dir,
                base_bundle_dir,
                intent_path,
                host_profile_path=host_profile_path,
                require_existing_bundle=base_uses_prebuilt,
                expected_bundle_manifest_sha256=base_expected_bundle_sha256,
            )
            update = prepare_project(
                effective_update_project_dir,
                update_bundle_dir,
                intent_path,
                host_profile_path=host_profile_path,
                require_existing_bundle=update_uses_prebuilt,
                expected_bundle_manifest_sha256=update_expected_bundle_sha256,
            )
        finally:
            host_profile_path.unlink(missing_ok=True)
        if pending_correction_path is not None:
            pending_correction = _validate_pending_correction(
                _load_json(pending_correction_path),
                ledger,
                project_dir=effective_project_dir,
                update_project_dir=effective_update_project_dir,
                intent_path=intent_path,
                host_profile_sha256=profile_sha,
            )
        _operation(operations, "validate", base.local_observation["validation"])
        if base.bundle_origin == "built" or update.bundle_origin == "built":
            built = base if base.bundle_origin == "built" else update
            _operation(operations, "build", built.local_observation["bundle"])
        else:
            _operation_not_run(
                operations,
                "build",
                "local:exact-prebuilt-bundles-verified-without-rebuild",
            )
        _operation(operations, "verify", base.local_observation["bundle"])

        _validate_update_pair(base, update)
        base_published = _publish_bundle(worker, base, f"{run_id}-base")
        try:
            base_install = _install_bundle(
                worker, base_published, profile_sha, base
            )
        finally:
            _delete_staging(worker, base_published["artifacts"])
        _operation(operations, "install", base_install)
        base_activation = _activate(
            worker,
            base.project["project_id"],
            str(base_install["install_id"]),
            f"{run_id}-activate-base",
        )
        _operation(operations, "activate", base_activation)
        base_launch, correction = _launch(
            worker,
            project_id=base.project["project_id"],
            install_id=str(base_install["install_id"]),
            launch_id=f"{run_id}-launch-base",
            profile_sha256=profile_sha,
        )
        if pending_correction is not None:
            ledger = _append_correction(
                ledger,
                _confirm_pending_correction(
                    pending_correction,
                    run_id=run_id,
                ),
            )
        if correction:
            correction["input_identity"]["project_sha256"] = sha256_file(
                effective_project_dir / PROJECT_FILENAME
            )
            correction["input_identity"]["bundle_sha256"] = (
                base.bundle_manifest_sha256
            )
            ledger = _append_correction(ledger, correction)
        _operation(operations, "launch", base_launch)
        base_observation = _observe(worker)
        _operation(operations, "observe", base_observation)
        if prove_structured_live_correction:
            live_correction = _prove_structured_live_correction(
                worker,
                intent=intent,
                foreground=base_observation,
                profile_sha256=profile_sha,
                project_sha256=sha256_file(
                    effective_project_dir / PROJECT_FILENAME
                ),
                bundle_sha256=base.bundle_manifest_sha256,
            )
            live_correction.pop("retry_response")
            ledger = _append_correction(ledger, live_correction)
        base_interaction = _dispatch_interactions(
            worker,
            intent=intent,
            foreground=base_observation,
            profile_sha256=profile_sha,
        )
        _operation(operations, "interact", base_interaction)
        base_observation = base_interaction["observation"]
        base_capture = _capture(
            worker,
            foreground=base_observation,
            capture_id=f"{run_id}-capture-base",
            profile_sha256=profile_sha,
            output_path=capture_root / "base.bgra",
        )
        _operation(operations, "capture", base_capture)
        artifact_receipts.append(
            {
                "role": "creative.capture",
                "reference": str((capture_root / "base.bgra").resolve()),
                "bytes": int(base_capture["content_bytes"]),
                "sha256": str(base_capture["content_sha256"]),
            }
        )

        update_published = _publish_bundle(worker, update, f"{run_id}-update")
        try:
            update_install = _install_bundle(
                worker, update_published, profile_sha, update
            )
        finally:
            _delete_staging(worker, update_published["artifacts"])
        update_activation = _activate(
            worker,
            update.project["project_id"],
            str(update_install["install_id"]),
            f"{run_id}-activate-update",
        )
        update_launch, correction = _launch(
            worker,
            project_id=update.project["project_id"],
            install_id=str(update_install["install_id"]),
            launch_id=f"{run_id}-launch-update",
            profile_sha256=profile_sha,
        )
        if correction:
            correction["input_identity"]["project_sha256"] = sha256_file(
                effective_update_project_dir / PROJECT_FILENAME
            )
            correction["input_identity"]["bundle_sha256"] = (
                update.bundle_manifest_sha256
            )
            ledger = _append_correction(ledger, correction)
        update_observation = _observe(worker)
        update_interaction = _dispatch_interactions(
            worker,
            intent=intent,
            foreground=update_observation,
            profile_sha256=profile_sha,
        )
        update_observation = update_interaction["observation"]
        update_capture = _capture(
            worker,
            foreground=update_observation,
            capture_id=f"{run_id}-capture-update",
            profile_sha256=profile_sha,
            output_path=capture_root / "update.bgra",
        )
        _operation(
            operations,
            "update",
            {
                "schema_version": "xcp-agent-update-v1",
                "install": update_install,
                "activation": update_activation,
                "launch": update_launch,
                "observation": update_observation,
                "interaction": update_interaction,
                "capture": update_capture,
            },
        )
        artifact_receipts.append(
            {
                "role": "creative.capture",
                "reference": str((capture_root / "update.bgra").resolve()),
                "bytes": int(update_capture["content_bytes"]),
                "sha256": str(update_capture["content_sha256"]),
            }
        )

        active, previous = _activation_state(
            _list_installs(worker), base.project["project_id"]
        )
        rollback = worker.request(
            {
                "command": "rollback_creative_activation",
                "schema_version": "xcp-creative-rollback-activation-request-v1",
                "project_id": base.project["project_id"],
                "transition_id": f"{run_id}-rollback",
                "expected_active_install_id": active,
                "expected_previous_install_id": previous,
            }
        )
        _reload(
            worker,
            project_id=base.project["project_id"],
            install_id=str(rollback["active_install_id"]),
            launch_id=f"{run_id}-reload-rollback",
            profile_sha256=profile_sha,
        )
        final_observation = _observe(worker)
        rollback_interaction = _dispatch_interactions(
            worker,
            intent=intent,
            foreground=final_observation,
            profile_sha256=profile_sha,
        )
        _operation(operations, "interact", rollback_interaction)
        final_observation = rollback_interaction["observation"]
        rollback_capture = _capture(
            worker,
            foreground=final_observation,
            capture_id=f"{run_id}-capture-rollback",
            profile_sha256=profile_sha,
            output_path=capture_root / "rollback.bgra",
        )
        _operation(operations, "rollback", rollback)
        artifact_receipts.append(
            {
                "role": "creative.capture",
                "reference": str((capture_root / "rollback.bgra").resolve()),
                "bytes": int(rollback_capture["content_bytes"]),
                "sha256": str(rollback_capture["content_sha256"]),
            }
        )

        cleanup, cleanup_correction = _cleanup_rollback_target(
            worker,
            project_id=base.project["project_id"],
            base_install_id=str(base_install["install_id"]),
            update_install_id=str(update_install["install_id"]),
            profile_sha256=profile_sha,
            project_sha256=sha256_file(
                effective_update_project_dir / PROJECT_FILENAME
            ),
            bundle_sha256=update.bundle_manifest_sha256,
            run_id=run_id,
        )
        ledger = _append_correction(ledger, cleanup_correction)
        _operation(operations, "cleanup", cleanup)

    assert base is not None
    live_acceptance = evaluate_acceptance(
        base.intent,
        final_observation,
        phase="live",
    )
    all_acceptance = [*base.acceptance, *live_acceptance]
    passed = _blocking_acceptance_passed(all_acceptance)
    ledger["status"] = "passed" if passed else "failed"
    _validate_ledger(ledger, intent_sha256=intent_sha)
    if ledger_path.exists():
        _replace_json(ledger_path, ledger)
    else:
        _write_new_json(ledger_path, ledger)
    ledger_sha = sha256_file(ledger_path)

    receipt = {
        "schema_version": "xcp-agent-lifecycle-receipt-v1",
        "run_id": run_id,
        "project_id": base.project["project_id"],
        "project_version": base.project["version"],
        "intent_sha256": sha256_file(intent_path),
        "project_sha256": sha256_file(
            effective_project_dir / PROJECT_FILENAME
        ),
        "bundle_sha256": base.bundle_manifest_sha256,
        "host_profile_sha256": profile_sha,
        "install_id": str(base_install["install_id"]) if base_install else "",
        "plan_sha256": str(final_observation.get("plan_sha256", "")),
        "version_transition": {
            "schema_version": "xcp-agent-version-transition-v1",
            "base": {
                "project_version": base.project["version"],
                "project_sha256": sha256_file(
                    effective_project_dir / PROJECT_FILENAME
                ),
                "bundle_sha256": base.bundle_manifest_sha256,
                "install_id": str(base_install["install_id"]),
            },
            "update": {
                "project_version": update.project["version"],
                "project_sha256": sha256_file(
                    effective_update_project_dir / PROJECT_FILENAME
                ),
                "bundle_sha256": update.bundle_manifest_sha256,
                "install_id": str(update_install["install_id"]),
            },
            "rollback": {
                "project_version": base.project["version"],
                "project_sha256": sha256_file(
                    effective_project_dir / PROJECT_FILENAME
                ),
                "bundle_sha256": base.bundle_manifest_sha256,
                "install_id": str(rollback["active_install_id"]),
            },
            "exact_update_activated": (
                str(update_activation["active_install_id"])
                == str(update_install["install_id"])
            ),
            "exact_base_restored": (
                str(rollback["active_install_id"])
                == str(base_install["install_id"])
            ),
        },
        "operations": operations,
        "acceptance": all_acceptance,
        "artifacts": artifact_receipts,
        "correction_ledger_sha256": ledger_sha,
        "decision": "pass" if passed else "fail",
    }
    _validate_document(receipt, DEFAULT_RECEIPT_SCHEMA, stage="receipt")
    _write_new_json(receipt_path, receipt)
    if not passed:
        raise AgentLifecycleError(
            "xcp.agent.acceptance_failed",
            "The live lifecycle completed but blocking intent acceptance failed.",
            stage="acceptance",
            field="receipt",
            expected="all blocker assertions pass",
            actual=str(receipt_path),
            correction="inspect the receipt, correct the project, and rerun",
            retryable=True,
        )
    return receipt


def describe() -> dict[str, Any]:
    profile_bytes = DEFAULT_C5_PROFILE.read_bytes()
    schemas = [
        DEFAULT_INTENT_SCHEMA,
        DEFAULT_LEDGER_SCHEMA,
        DEFAULT_RECEIPT_SCHEMA,
    ]
    return {
        "ok": True,
        "schema_version": "xcp-agent-lifecycle-description-v1",
        "gate_id": "C5_AGENT_NATIVE_CREATION_GATE",
        "profile": {
            "filename": DEFAULT_C5_PROFILE.name,
            "bytes": len(profile_bytes),
            "sha256": sha256_bytes(profile_bytes),
        },
        "schemas": [
            {
                "filename": path.name,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for path in schemas
        ],
        "commands": [
            "describe",
            "init-intent",
            "validate-intent",
            "prepare",
            "evaluate",
            "init-ledger",
            "append-correction",
            "discover",
            "run-preview",
            "run-live",
        ],
        "auth": {
            "pairing_code_environment": "XCP_PAIRING_CODE",
            "session_id_environment": "XCP_SESSION_ID",
            "persisted": False,
        },
    }


def _auth_from_environment() -> tuple[str, str]:
    pairing_code = os.environ.get("XCP_PAIRING_CODE", "")
    session_id = os.environ.get("XCP_SESSION_ID", "")
    if bool(pairing_code) == bool(session_id):
        raise AgentLifecycleError(
            "xcp.agent.auth_context_invalid",
            "Exactly one ephemeral worker authentication value is required.",
            stage="auth",
            field="environment",
            expected="XCP_PAIRING_CODE xor XCP_SESSION_ID",
            actual="both" if pairing_code and session_id else "neither",
            correction="set exactly one value for this process only",
        )
    return pairing_code, session_id


def _endpoint(args: argparse.Namespace) -> WorkerEndpoint:
    return WorkerEndpoint(
        args.device_address,
        args.port,
        args.timeout_seconds,
    )


def _add_endpoint_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device-address", required=True)
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--timeout-seconds", type=float, default=15.0)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the universal C5 lifecycle for ordinary XCP projects."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("describe")

    init_intent = subparsers.add_parser("init-intent")
    init_intent.add_argument("--output", type=pathlib.Path, required=True)
    init_intent.add_argument("--project-id", required=True)
    init_intent.add_argument("--summary", required=True)
    init_intent.add_argument(
        "--experience-kind",
        choices=[
            "game",
            "application",
            "utility",
            "simulation",
            "interactive_scene",
            "educational",
            "experiment",
            "other",
        ],
        required=True,
    )

    validate_intent_parser = subparsers.add_parser("validate-intent")
    validate_intent_parser.add_argument("--intent", type=pathlib.Path, required=True)

    prepare = subparsers.add_parser("prepare")
    prepare.add_argument("--project-dir", type=pathlib.Path, required=True)
    prepare.add_argument("--bundle-dir", type=pathlib.Path, required=True)
    prepare.add_argument("--intent", type=pathlib.Path, required=True)
    prepare.add_argument("--host-profile", type=pathlib.Path)

    evaluate = subparsers.add_parser("evaluate")
    evaluate.add_argument("--intent", type=pathlib.Path, required=True)
    evaluate.add_argument("--observation", type=pathlib.Path, required=True)
    evaluate.add_argument("--phase", choices=["local", "live"], required=True)

    init_ledger = subparsers.add_parser("init-ledger")
    init_ledger.add_argument("--output", type=pathlib.Path, required=True)
    init_ledger.add_argument("--project-id", required=True)
    init_ledger.add_argument("--intent", type=pathlib.Path, required=True)

    append_ledger = subparsers.add_parser("append-correction")
    append_ledger.add_argument("--ledger", type=pathlib.Path, required=True)
    append_ledger.add_argument("--record", type=pathlib.Path, required=True)

    discover = subparsers.add_parser("discover")
    _add_endpoint_arguments(discover)
    discover.add_argument("--output", type=pathlib.Path)

    preview = subparsers.add_parser("run-preview")
    _add_endpoint_arguments(preview)
    preview.add_argument("--project-dir", type=pathlib.Path, required=True)
    preview.add_argument("--bundle-root", type=pathlib.Path, required=True)
    preview.add_argument("--intent", type=pathlib.Path, required=True)
    preview.add_argument("--capture-root", type=pathlib.Path, required=True)
    preview.add_argument("--receipt", type=pathlib.Path, required=True)
    preview.add_argument("--ledger", type=pathlib.Path, required=True)
    preview.add_argument("--run-id", required=True)

    live = subparsers.add_parser("run-live")
    _add_endpoint_arguments(live)
    live.add_argument("--project-dir", type=pathlib.Path, required=True)
    live.add_argument("--update-project-dir", type=pathlib.Path, required=True)
    live.add_argument("--bundle-root", type=pathlib.Path, required=True)
    live.add_argument("--base-prebuilt-bundle-dir", type=pathlib.Path)
    live.add_argument("--base-expected-bundle-sha256", default="")
    live.add_argument("--update-prebuilt-bundle-dir", type=pathlib.Path)
    live.add_argument("--update-expected-bundle-sha256", default="")
    live.add_argument("--intent", type=pathlib.Path, required=True)
    live.add_argument("--capture-root", type=pathlib.Path, required=True)
    live.add_argument("--receipt", type=pathlib.Path, required=True)
    live.add_argument("--ledger", type=pathlib.Path, required=True)
    live.add_argument("--run-id", required=True)
    live.add_argument(
        "--prove-structured-live-correction",
        action="store_true",
        help=(
            "Submit one bounded undeclared action, require its published "
            "structured rejection, then correct it with the first intent action."
        ),
    )
    live.add_argument(
        "--pending-correction",
        type=pathlib.Path,
        help=(
            "Bind one unconfirmed external live correction and append it only "
            "after the corrected base launch passes."
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "describe":
            result = describe()
        elif args.command == "init-intent":
            document = new_intent(
                args.project_id,
                args.summary,
                args.experience_kind,
            )
            _validate_document(document, DEFAULT_INTENT_SCHEMA, stage="intent")
            _write_new_json(args.output, document)
            result = {
                "ok": True,
                "schema_version": "xcp-agent-intent-created-v1",
                "output": str(args.output.resolve()),
                "sha256": sha256_file(args.output),
            }
        elif args.command == "validate-intent":
            document = validate_intent(args.intent)
            result = {
                "ok": True,
                "schema_version": "xcp-agent-intent-validation-v1",
                "project_id": document["project_id"],
                "assertion_count": len(document["acceptance"]),
                "interaction_sequence_count": len(
                    document.get("interaction_sequences", [])
                ),
                "sha256": sha256_file(args.intent),
            }
        elif args.command == "prepare":
            prepared = prepare_project(
                args.project_dir,
                args.bundle_dir,
                args.intent,
                host_profile_path=args.host_profile,
            )
            result = {
                "ok": True,
                "schema_version": "xcp-agent-local-preparation-v1",
                "project": prepared.local_observation["project"],
                "validation": prepared.local_observation["validation"],
                "bundle": prepared.local_observation["bundle"],
                "acceptance": prepared.acceptance,
            }
        elif args.command == "evaluate":
            intent = validate_intent(args.intent)
            observation = _load_json(args.observation)
            acceptance = evaluate_acceptance(
                intent, observation, phase=args.phase
            )
            passed = _blocking_acceptance_passed(acceptance)
            result = {
                "ok": passed,
                "schema_version": "xcp-agent-acceptance-evaluation-v1",
                "phase": args.phase,
                "acceptance": acceptance,
                "decision": "pass" if passed else "fail",
            }
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0 if passed else 2
        elif args.command == "init-ledger":
            intent = validate_intent(args.intent)
            if intent["project_id"] != args.project_id:
                raise AgentLifecycleError(
                    "xcp.agent.intent_project_mismatch",
                    "Ledger project id does not match the intent.",
                    stage="correction",
                    field="project_id",
                    expected=intent["project_id"],
                    actual=args.project_id,
                    correction="use the intent project id",
                )
            ledger = _new_ledger(args.project_id, sha256_file(args.intent))
            _validate_ledger(ledger, intent_sha256=sha256_file(args.intent))
            _write_new_json(args.output, ledger)
            result = {
                "ok": True,
                "schema_version": "xcp-agent-correction-ledger-created-v1",
                "output": str(args.output.resolve()),
                "sha256": sha256_file(args.output),
            }
        elif args.command == "append-correction":
            ledger = _load_json(args.ledger)
            ledger = _validate_ledger(ledger)
            record = _load_json(args.record)
            ledger = _append_correction(ledger, record)
            _replace_json(args.ledger, ledger)
            result = {
                "ok": True,
                "schema_version": "xcp-agent-correction-appended-v1",
                "ordinal": len(ledger["entries"]),
                "head_sha256": ledger["head_sha256"],
            }
        elif args.command == "discover":
            pairing_code, session_id = _auth_from_environment()
            with WorkerSession(
                WorkerJsonClient(_endpoint(args)),
                pairing_code=pairing_code,
                session_id=session_id,
            ) as worker:
                runtime, profile = discover_host(worker)
            if args.output:
                _write_new_json(args.output, profile)
            result = {
                "ok": True,
                "schema_version": "xcp-agent-host-discovery-v1",
                "runtime_schema_version": runtime.get("schema_version", ""),
                "profile_id": profile["profile_id"],
                "host_api_version": profile["host_api_version"],
                "canonical_profile_sha256": sha256_bytes(
                    canonical_json_bytes(profile)
                ),
                "output": str(args.output.resolve()) if args.output else "",
            }
        elif args.command == "run-preview":
            pairing_code, session_id = _auth_from_environment()
            result = {
                "ok": True,
                **run_preview(
                    endpoint=_endpoint(args),
                    pairing_code=pairing_code,
                    session_id=session_id,
                    project_dir=args.project_dir,
                    bundle_root=args.bundle_root,
                    intent_path=args.intent,
                    capture_root=args.capture_root,
                    receipt_path=args.receipt,
                    ledger_path=args.ledger,
                    run_id=args.run_id,
                ),
            }
        elif args.command == "run-live":
            pairing_code, session_id = _auth_from_environment()
            result = {
                "ok": True,
                **run_live(
                    endpoint=_endpoint(args),
                    pairing_code=pairing_code,
                    session_id=session_id,
                    project_dir=args.project_dir,
                    update_project_dir=args.update_project_dir,
                    bundle_root=args.bundle_root,
                    intent_path=args.intent,
                    capture_root=args.capture_root,
                    receipt_path=args.receipt,
                    ledger_path=args.ledger,
                    run_id=args.run_id,
                    base_prebuilt_bundle_dir=args.base_prebuilt_bundle_dir,
                    base_expected_bundle_sha256=(
                        args.base_expected_bundle_sha256
                    ),
                    update_prebuilt_bundle_dir=args.update_prebuilt_bundle_dir,
                    update_expected_bundle_sha256=(
                        args.update_expected_bundle_sha256
                    ),
                    prove_structured_live_correction=(
                        args.prove_structured_live_correction
                    ),
                    pending_correction_path=args.pending_correction,
                ),
            }
        else:
            raise AssertionError(f"unsupported command {args.command}")
    except (AgentLifecycleError, WorkerTransportError) as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    except CreativeProjectError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
