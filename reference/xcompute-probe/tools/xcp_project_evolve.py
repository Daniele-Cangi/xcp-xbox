#!/usr/bin/env python3
"""C7 incremental source evolution over the existing C6 and C5 contracts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
from collections import Counter
from typing import Any, Iterable

from jsonschema import Draft202012Validator

from xcp_creative_project import (
    PROJECT_FILENAME,
    canonical_json_bytes,
    sha256_bytes,
    sha256_file,
    validate_project,
)
from xcp_source_adapt import (
    INTENT_SCHEMA,
    LEDGER_SCHEMA,
    RECEIPT_SCHEMA,
    SCHEMAS as C6_SCHEMAS,
    _adaptation_handoff,
    _adapted_c5_intent,
    _validate as validate_c6,
    adapt as adapt_c6,
    describe as describe_c6,
    fidelity_contract,
    readiness_report,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMAS = {
    "overlay": ROOT / "schemas" / "xcp-creative-evolution-overlay-v1.schema.json",
    "diff": ROOT / "schemas" / "xcp-creative-semantic-diff-v1.schema.json",
    "reconciliation": (
        ROOT / "schemas" / "xcp-creative-reconciliation-plan-v1.schema.json"
    ),
    "reuse": ROOT / "schemas" / "xcp-creative-evolution-reuse-v1.schema.json",
    "report": ROOT / "schemas" / "xcp-creative-evolution-report-v1.schema.json",
    "handoff": C6_SCHEMAS["handoff"],
}
PROFILE = (
    ROOT / "profiles" / "creative" / "xcp-project-evolution-v1.json"
)
IR_COLLECTIONS = (
    "scenes",
    "entities",
    "behaviors",
    "input_actions",
    "ui",
    "assets",
    "state",
)
CHANGE_STATUSES = ("unchanged", "modified", "added", "removed")
MISSING = object()


class EvolutionError(RuntimeError):
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
        raise EvolutionError(
            "xcp.evolve.file_missing",
            "A required C7 input is missing.",
            stage="load",
            field="path",
            expected="existing UTF-8 JSON file",
            actual=str(path),
            correction="restore the exact bound input and retry",
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvolutionError(
            "xcp.evolve.json_invalid",
            "A required C7 input is not valid UTF-8 JSON.",
            stage="load",
            field="path",
            expected="valid UTF-8 JSON",
            actual=str(path),
            correction="correct the document and retry",
        ) from exc


def _validate(document: Any, schema_key: str) -> None:
    schema = _load_json(SCHEMAS[schema_key])
    Draft202012Validator.check_schema(schema)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(document),
        key=lambda item: tuple(str(part) for part in item.absolute_path),
    )
    if errors:
        first = errors[0]
        raise EvolutionError(
            "xcp.evolve.schema_rejected",
            "A C7 machine document failed its authoritative schema.",
            stage=schema_key,
            field=".".join(str(part) for part in first.absolute_path),
            expected=str(first.validator),
            actual=first.message,
            correction="correct the structured field and retry",
            retryable=True,
        )


def _validate_external(document: Any, schema_path: pathlib.Path, stage: str) -> None:
    schema = _load_json(schema_path)
    Draft202012Validator.check_schema(schema)
    errors = list(Draft202012Validator(schema).iter_errors(document))
    if errors:
        first = sorted(
            errors,
            key=lambda item: tuple(str(part) for part in item.absolute_path),
        )[0]
        raise EvolutionError(
            "xcp.evolve.schema_rejected",
            "A lifecycle document failed its authoritative schema.",
            stage=stage,
            field=".".join(str(part) for part in first.absolute_path),
            expected=str(first.validator),
            actual=first.message,
            correction="restore the exact C5 document",
        )


def _write_new(path: pathlib.Path, document: Any) -> None:
    path = path.resolve()
    if path.exists():
        raise EvolutionError(
            "xcp.evolve.output_exists",
            "C7 will not overwrite an existing output.",
            stage="write",
            field="path",
            expected="new path",
            actual=str(path),
            correction="select a fresh output root",
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
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_json_bytes(document))
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _doc_sha(document: Any) -> str:
    return sha256_bytes(canonical_json_bytes(document))


def _value_sha(value: Any) -> str:
    return "" if value is MISSING else _doc_sha(value)


def _tree_entries(root: pathlib.Path) -> list[dict[str, Any]]:
    root = root.resolve()
    if not root.is_dir():
        raise EvolutionError(
            "xcp.evolve.project_root_invalid",
            "A generated project root is missing.",
            stage="identity",
            field="project_root",
            expected="existing directory",
            actual=str(root),
            correction="restore the exact generated project",
        )
    entries: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_symlink():
            raise EvolutionError(
                "xcp.evolve.symlink_rejected",
                "C7 project identities do not follow symbolic links.",
                stage="identity",
                field="path",
                expected="regular file or directory",
                actual=str(path),
                correction="remove the symbolic link",
            )
        if path.is_file():
            entries.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
            )
    return entries


def project_tree_sha256(root: pathlib.Path) -> str:
    return _doc_sha(_tree_entries(root))


def _bounded_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    root = root.resolve()
    path = (root / pathlib.PurePosixPath(relative)).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise EvolutionError(
            "xcp.evolve.path_escape",
            "A bound relative path escapes its root.",
            stage="load",
            field="path",
            expected=str(root),
            actual=str(path),
            correction="reject the modified handoff or overlay",
        ) from exc
    return path


def _verify_handoff(run_root: pathlib.Path) -> dict[str, pathlib.Path]:
    handoff = _load_json(run_root / "c6-to-c5-handoff.json")
    _validate(handoff, "handoff")
    roles: dict[str, pathlib.Path] = {}
    for artifact in handoff["artifacts"]:
        role = str(artifact["role"])
        if role in roles:
            raise EvolutionError(
                "xcp.evolve.handoff_role_duplicate",
                "The prior C6 handoff repeats an artifact role.",
                stage="prior",
                field="artifacts.role",
                expected="one exact artifact per role",
                actual=role,
                correction="restore the generated handoff",
            )
        path = _bounded_path(run_root, str(artifact["path"]))
        actual = sha256_file(path)
        if actual != artifact["sha256"]:
            raise EvolutionError(
                "xcp.evolve.handoff_hash_mismatch",
                "A prior C6 artifact differs from its handoff.",
                stage="prior",
                field=role,
                expected=str(artifact["sha256"]),
                actual=actual,
                correction="restore the exact prior C6 output",
            )
        roles[role] = path
    return roles


def _load_prior(run_root: pathlib.Path) -> dict[str, Any]:
    run_root = run_root.resolve()
    roles = _verify_handoff(run_root)
    inventory = _load_json(roles["source.inventory"])
    ir = _load_json(roles["creative.ir"])
    plan = _load_json(roles["adaptation.plan"])
    source_map = _load_json(roles["source.map"])
    readiness = _load_json(roles["adaptation.readiness.preliminary"])
    project = _load_json(roles["xcp.project"])
    for key, document in (
        ("inventory", inventory),
        ("ir", ir),
        ("plan", plan),
        ("source_map", source_map),
        ("readiness", readiness),
    ):
        validate_c6(document, key)
    project_root = roles["xcp.project"].parent
    validate_project(project_root)
    return {
        "root": run_root,
        "project_root": project_root,
        "inventory": inventory,
        "ir": ir,
        "plan": plan,
        "source_map": source_map,
        "readiness": readiness,
        "project": project,
        "inventory_sha256": sha256_file(roles["source.inventory"]),
        "ir_sha256": sha256_file(roles["creative.ir"]),
        "project_sha256": sha256_file(roles["xcp.project"]),
        "project_tree_sha256": project_tree_sha256(project_root),
    }


def _change_status(base_sha: str, target_sha: str) -> str:
    if base_sha and target_sha:
        return "unchanged" if base_sha == target_sha else "modified"
    return "added" if target_sha else "removed"


def _summary(items: Iterable[dict[str, Any]]) -> dict[str, int]:
    counts = Counter(str(item["status"]) for item in items)
    return {status: counts[status] for status in CHANGE_STATUSES}


def semantic_diff(
    base_inventory: dict[str, Any],
    base_ir: dict[str, Any],
    target_inventory: dict[str, Any],
    target_ir: dict[str, Any],
) -> dict[str, Any]:
    base_files = {str(item["path"]): item for item in base_inventory["files"]}
    target_files = {
        str(item["path"]): item for item in target_inventory["files"]
    }
    file_changes = []
    for path in sorted(set(base_files) | set(target_files)):
        base = base_files.get(path)
        target = target_files.get(path)
        base_sha = str(base["sha256"]) if base else ""
        target_sha = str(target["sha256"]) if target else ""
        role = str((target or base)["role"])
        file_changes.append(
            {
                "path": path,
                "status": _change_status(base_sha, target_sha),
                "base_sha256": base_sha,
                "target_sha256": target_sha,
                "role": role,
            }
        )

    def records(document: dict[str, Any]) -> dict[tuple[str, str], Any]:
        result = {("project", str(document["project"]["id"])): document["project"]}
        for collection in IR_COLLECTIONS:
            result.update(
                {
                    (collection, str(item["id"])): item
                    for item in document[collection]
                }
            )
        return result

    base_records = records(base_ir)
    target_records = records(target_ir)
    semantic_changes = []
    for key in sorted(set(base_records) | set(target_records)):
        base = base_records.get(key, MISSING)
        target = target_records.get(key, MISSING)
        base_sha = _value_sha(base)
        target_sha = _value_sha(target)
        semantic_changes.append(
            {
                "collection": key[0],
                "element_id": key[1],
                "status": _change_status(base_sha, target_sha),
                "base_sha256": base_sha,
                "target_sha256": target_sha,
            }
        )
    result = {
        "schema_version": "xcp-creative-semantic-diff-v1",
        "diff_id": f"{target_ir['project']['id']}.source-evolution",
        "project_id": str(target_ir["project"]["id"]),
        "base": {
            "revision": str(base_inventory["source"]["revision"]),
            "source_inventory_sha256": _doc_sha(base_inventory),
            "creative_ir_sha256": _doc_sha(base_ir),
        },
        "target": {
            "revision": str(target_inventory["source"]["revision"]),
            "source_inventory_sha256": _doc_sha(target_inventory),
            "creative_ir_sha256": _doc_sha(target_ir),
        },
        "file_summary": _summary(file_changes),
        "files": file_changes,
        "semantic_summary": _summary(semantic_changes),
        "semantics": semantic_changes,
        "has_semantic_changes": any(
            item["status"] != "unchanged" for item in semantic_changes
        ),
    }
    _validate(result, "diff")
    return result


def _pointer_tokens(pointer: str) -> list[str]:
    return [
        token.replace("~1", "/").replace("~0", "~")
        for token in pointer[1:].split("/")
    ]


def _pointer_get(document: Any, pointer: str) -> Any:
    current = document
    for token in _pointer_tokens(pointer):
        if isinstance(current, dict):
            if token not in current:
                return MISSING
            current = current[token]
        elif isinstance(current, list):
            if not token.isdigit() or int(token) >= len(current):
                return MISSING
            current = current[int(token)]
        else:
            return MISSING
    return current


def _pointer_parent(document: Any, pointer: str) -> tuple[Any, str]:
    tokens = _pointer_tokens(pointer)
    current = document
    for token in tokens[:-1]:
        if isinstance(current, dict) and token in current:
            current = current[token]
        elif (
            isinstance(current, list)
            and token.isdigit()
            and int(token) < len(current)
        ):
            current = current[int(token)]
        else:
            raise EvolutionError(
                "xcp.evolve.overlay_parent_missing",
                "An overlay JSON Pointer parent does not exist.",
                stage="materialize",
                field="pointer",
                expected="existing object or array parent",
                actual=pointer,
                correction="correct the overlay path",
            )
    return current, tokens[-1]


def _pointer_apply(document: Any, operation: dict[str, Any]) -> None:
    parent, token = _pointer_parent(document, str(operation["pointer"]))
    op = str(operation["op"])
    if isinstance(parent, dict):
        present = token in parent
        if op == "add":
            parent[token] = operation["value"]
        elif op == "replace" and present:
            parent[token] = operation["value"]
        elif op == "remove" and present:
            del parent[token]
        else:
            raise EvolutionError(
                "xcp.evolve.overlay_operation_invalid",
                "An overlay operation is incompatible with the target object.",
                stage="materialize",
                field=str(operation["pointer"]),
                expected=f"valid {op} operation",
                actual="present" if present else "missing",
                correction="correct the overlay operation",
            )
        return
    if isinstance(parent, list):
        if op == "add" and token == "-":
            parent.append(operation["value"])
            return
        if not token.isdigit():
            valid = False
        else:
            index = int(token)
            valid = index < len(parent)
            if op == "add" and index <= len(parent):
                parent.insert(index, operation["value"])
                return
            if op == "replace" and valid:
                parent[index] = operation["value"]
                return
            if op == "remove" and valid:
                del parent[index]
                return
        raise EvolutionError(
            "xcp.evolve.overlay_operation_invalid",
            "An overlay operation is incompatible with the target array.",
            stage="materialize",
            field=str(operation["pointer"]),
            expected=f"valid {op} array index",
            actual=token,
            correction="correct the overlay operation",
        )
    raise EvolutionError(
        "xcp.evolve.overlay_parent_invalid",
        "An overlay parent is not an object or array.",
        stage="materialize",
        field=str(operation["pointer"]),
        expected="object or array",
        actual=type(parent).__name__,
        correction="correct the overlay path",
    )


def _validate_overlay_bindings(overlay: dict[str, Any], prior: dict[str, Any]) -> None:
    expected = {
        "source_revision": str(prior["inventory"]["source"]["revision"]),
        "source_inventory_sha256": prior["inventory_sha256"],
        "creative_ir_sha256": prior["ir_sha256"],
        "xcp_project_sha256": prior["project_sha256"],
        "project_tree_sha256": prior["project_tree_sha256"],
    }
    if overlay["project_id"] != prior["project"]["project_id"]:
        raise EvolutionError(
            "xcp.evolve.overlay_project_mismatch",
            "The overlay targets a different project.",
            stage="overlay",
            field="project_id",
            expected=str(prior["project"]["project_id"]),
            actual=str(overlay["project_id"]),
            correction="bind the overlay to the exact prior project",
        )
    for field, value in expected.items():
        if overlay["base"][field] != value:
            raise EvolutionError(
                "xcp.evolve.overlay_base_mismatch",
                "The overlay base identity does not match the prior C6 output.",
                stage="overlay",
                field=f"base.{field}",
                expected=value,
                actual=str(overlay["base"][field]),
                correction="regenerate the overlay from the exact prior output",
            )
    locations: list[tuple[str, str]] = []
    version_operations = []
    for operation in overlay["operations"]:
        location = (str(operation["document"]), str(operation["pointer"]))
        for existing in locations:
            same_document = existing[0] == location[0]
            overlaps = (
                existing[1] == location[1]
                or existing[1].startswith(location[1] + "/")
                or location[1].startswith(existing[1] + "/")
            )
            if same_document and overlaps:
                raise EvolutionError(
                    "xcp.evolve.overlay_path_overlap",
                    "Overlay operations may not target overlapping pointers.",
                    stage="overlay",
                    field="operations",
                    expected="independent document/pointer locations",
                    actual=f"{existing!r}/{location!r}",
                    correction="combine or separate the overlay operations",
                )
        locations.append(location)
        base_document = _load_json(
            _bounded_path(prior["project_root"], location[0])
        )
        base_value = _pointer_get(base_document, location[1])
        present = base_value is not MISSING
        if bool(operation["base_present"]) != present:
            raise EvolutionError(
                "xcp.evolve.overlay_base_value_mismatch",
                "Overlay base presence differs from the prior project.",
                stage="overlay",
                field=str(operation["operation_id"]),
                expected=str(present),
                actual=str(operation["base_present"]),
                correction="re-author the overlay against the exact prior project",
            )
        if str(operation["base_value_sha256"]) != _value_sha(base_value):
            raise EvolutionError(
                "xcp.evolve.overlay_base_value_mismatch",
                "Overlay base value hash differs from the prior project.",
                stage="overlay",
                field=str(operation["operation_id"]),
                expected=_value_sha(base_value),
                actual=str(operation["base_value_sha256"]),
                correction="re-author the overlay against the exact prior project",
            )
        if operation["op"] in {"replace", "remove"} and not present:
            raise EvolutionError(
                "xcp.evolve.overlay_operation_invalid",
                "Replace and remove require an existing base value.",
                stage="overlay",
                field=str(operation["operation_id"]),
                expected="base_present=true",
                actual="false",
                correction="use add or correct the base pointer",
            )
        if location == ("xcp-project.json", "/version"):
            version_operations.append(operation)
    if len(version_operations) != 1 or version_operations[0]["op"] not in {
        "add",
        "replace",
    }:
        raise EvolutionError(
            "xcp.evolve.version_transition_missing",
            "C7 requires one explicit immutable project-version transition.",
            stage="overlay",
            field="operations",
            expected="one add/replace at xcp-project.json#/version",
            actual=str(len(version_operations)),
            correction="add the exact target semantic version to the overlay",
        )


def reconciliation_plan(
    prior: dict[str, Any],
    raw_project_root: pathlib.Path,
    overlay: dict[str, Any],
    diff_sha256: str,
) -> dict[str, Any]:
    results = []
    conflicts = []
    choices = []
    for operation in overlay["operations"]:
        document_path = str(operation["document"])
        pointer = str(operation["pointer"])
        base_document = _load_json(
            _bounded_path(prior["project_root"], document_path)
        )
        source_document = _load_json(
            _bounded_path(raw_project_root, document_path)
        )
        base_value = _pointer_get(base_document, pointer)
        source_value = _pointer_get(source_document, pointer)
        overlay_value = (
            MISSING if operation["op"] == "remove" else operation["value"]
        )
        source_unchanged = _value_sha(source_value) == _value_sha(base_value)
        already_equivalent = _value_sha(source_value) == _value_sha(overlay_value)
        policy = str(operation["conflict_policy"])
        if already_equivalent:
            result = "already_equivalent"
        elif source_unchanged:
            result = "applied_overlay"
        elif policy == "overlay_wins":
            result = "applied_overlay_over_source_change"
            choices.append(
                {
                    "operation_id": str(operation["operation_id"]),
                    "selection": "overlay_wins",
                    "rationale": str(operation["choice_rationale"]),
                }
            )
        elif policy == "source_wins":
            result = "kept_source_by_explicit_choice"
            choices.append(
                {
                    "operation_id": str(operation["operation_id"]),
                    "selection": "source_wins",
                    "rationale": str(operation["choice_rationale"]),
                }
            )
        else:
            result = "conflict_requires_human"
            conflicts.append(str(operation["operation_id"]))
        results.append(
            {
                "operation_id": str(operation["operation_id"]),
                "document": document_path,
                "pointer": pointer,
                "op": str(operation["op"]),
                "conflict_policy": policy,
                "base_value_sha256": _value_sha(base_value),
                "source_value_sha256": _value_sha(source_value),
                "overlay_value_sha256": _value_sha(overlay_value),
                "result": result,
            }
        )
    plan = {
        "schema_version": "xcp-creative-reconciliation-plan-v1",
        "plan_id": f"{overlay['project_id']}.three-way-reconciliation",
        "project_id": str(overlay["project_id"]),
        "identities": {
            "base_project_tree_sha256": prior["project_tree_sha256"],
            "target_generated_tree_sha256": project_tree_sha256(raw_project_root),
            "overlay_sha256": _doc_sha(overlay),
            "semantic_diff_sha256": diff_sha256,
        },
        "operations": results,
        "conflicts": conflicts,
        "human_choices": choices,
        "overlay_preserved": not conflicts
        and all(
            item["result"] != "kept_source_by_explicit_choice"
            for item in results
        ),
        "decision": "blocked_conflict" if conflicts else "ready_to_materialize",
    }
    _validate(plan, "reconciliation")
    return plan


def _copy_project_by_hash(
    prior_root: pathlib.Path,
    raw_root: pathlib.Path,
    output_root: pathlib.Path,
) -> list[dict[str, Any]]:
    previous_entries = _tree_entries(prior_root)
    raw_entries = _tree_entries(raw_root)
    previous_by_hash: dict[str, list[dict[str, Any]]] = {}
    for entry in previous_entries:
        previous_by_hash.setdefault(str(entry["sha256"]), []).append(entry)
    entries = []
    for entry in raw_entries:
        relative = str(entry["path"])
        target = output_root / pathlib.PurePosixPath(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        matches = previous_by_hash.get(str(entry["sha256"]), [])
        if matches:
            source_relative = str(matches[0]["path"])
            source = prior_root / pathlib.PurePosixPath(source_relative)
            disposition = "reused_by_content_hash"
        else:
            source_relative = relative
            source = raw_root / pathlib.PurePosixPath(relative)
            disposition = "rebuilt"
        shutil.copyfile(source, target)
        if sha256_file(target) != entry["sha256"]:
            raise EvolutionError(
                "xcp.evolve.reuse_copy_mismatch",
                "A content-hash reuse copy changed bytes.",
                stage="reuse",
                field=relative,
                expected=str(entry["sha256"]),
                actual=sha256_file(target),
                correction="discard the generated project",
            )
        entries.append(
            {
                "path": relative,
                "disposition": disposition,
                "source_path": source_relative,
                "sha256": str(entry["sha256"]),
                "bytes": int(entry["bytes"]),
            }
        )
    raw_paths = {str(item["path"]) for item in raw_entries}
    for entry in previous_entries:
        if entry["path"] not in raw_paths:
            entries.append(
                {
                    "path": str(entry["path"]),
                    "disposition": "removed",
                    "source_path": str(entry["path"]),
                    "sha256": "",
                    "bytes": 0,
                }
            )
    return entries


def _apply_overlay(
    project_root: pathlib.Path,
    overlay: dict[str, Any],
    plan: dict[str, Any],
) -> set[str]:
    result_by_id = {
        str(item["operation_id"]): str(item["result"])
        for item in plan["operations"]
    }
    documents: dict[str, Any] = {}
    changed: set[str] = set()
    for operation in overlay["operations"]:
        result = result_by_id[str(operation["operation_id"])]
        if result in {"already_equivalent", "kept_source_by_explicit_choice"}:
            continue
        relative = str(operation["document"])
        document = documents.setdefault(
            relative,
            _load_json(_bounded_path(project_root, relative)),
        )
        _pointer_apply(document, operation)
        changed.add(relative)
    for relative, document in documents.items():
        _replace_json(_bounded_path(project_root, relative), document)
    return changed


def _reuse_manifest(
    project_id: str,
    target_inventory: dict[str, Any],
    host_profile_path: pathlib.Path,
    overlay: dict[str, Any],
    project_root: pathlib.Path,
    entries: list[dict[str, Any]],
    overlay_paths: set[str],
) -> dict[str, Any]:
    final_by_path = {
        str(item["path"]): item for item in _tree_entries(project_root)
    }
    for entry in entries:
        path = str(entry["path"])
        if path in overlay_paths:
            final = final_by_path[path]
            entry.update(
                {
                    "disposition": "overlay_applied",
                    "source_path": path,
                    "sha256": str(final["sha256"]),
                    "bytes": int(final["bytes"]),
                }
            )
    counts = Counter(str(item["disposition"]) for item in entries)
    c6 = describe_c6()
    adapter = c6["adapters"][0]
    backend = c6["backend"]
    manifest = {
        "schema_version": "xcp-creative-evolution-reuse-v1",
        "manifest_id": f"{project_id}.evolution-reuse",
        "project_id": project_id,
        "cache_identity": {
            "source_revision": str(target_inventory["source"]["revision"]),
            "adapter_id": str(adapter["adapter_id"]),
            "adapter_version": str(adapter["adapter_version"]),
            "backend_id": str(backend["backend_id"]),
            "backend_version": str(backend["backend_version"]),
            "host_profile_sha256": sha256_file(host_profile_path.resolve()),
            "overlay_sha256": _doc_sha(overlay),
        },
        "entries": sorted(entries, key=lambda item: str(item["path"])),
        "counts": {
            disposition: counts[disposition]
            for disposition in (
                "reused_by_content_hash",
                "rebuilt",
                "overlay_applied",
                "removed",
            )
        },
        "reused_bytes": sum(
            int(item["bytes"])
            for item in entries
            if item["disposition"] == "reused_by_content_hash"
        ),
        "rebuilt_bytes": sum(
            int(item["bytes"])
            for item in entries
            if item["disposition"] in {"rebuilt", "overlay_applied"}
        ),
        "final_project_tree_sha256": project_tree_sha256(project_root),
    }
    _validate(manifest, "reuse")
    return manifest


def _assertion(
    assertion_id: str,
    actual: Any,
    expected: Any,
    *,
    passed: bool,
) -> dict[str, Any]:
    return {
        "assertion_id": assertion_id,
        "outcome": "pass" if passed else "fail",
        "actual": actual,
        "expected": expected,
    }


def _preliminary_report(
    prior: dict[str, Any],
    output: pathlib.Path,
    target_inventory: dict[str, Any],
    target_ir: dict[str, Any],
    target_plan: dict[str, Any],
    project: dict[str, Any],
    overlay: dict[str, Any],
    diff: dict[str, Any],
    reconciliation: dict[str, Any],
    reuse: dict[str, Any],
    host_profile_path: pathlib.Path,
) -> dict[str, Any]:
    identities = {
        "base_source_inventory_sha256": prior["inventory_sha256"],
        "base_creative_ir_sha256": prior["ir_sha256"],
        "base_xcp_project_sha256": prior["project_sha256"],
        "base_project_tree_sha256": prior["project_tree_sha256"],
        "target_source_inventory_sha256": _doc_sha(target_inventory),
        "target_creative_ir_sha256": _doc_sha(target_ir),
        "target_adaptation_plan_sha256": _doc_sha(target_plan),
        "target_xcp_project_sha256": sha256_file(
            output / "xcp-project" / PROJECT_FILENAME
        ),
        "target_project_tree_sha256": project_tree_sha256(
            output / "xcp-project"
        ),
        "source_map_sha256": sha256_file(output / "source-map.json"),
        "readiness_sha256": sha256_file(output / "readiness-report.json"),
        "overlay_sha256": _doc_sha(overlay),
        "semantic_diff_sha256": _doc_sha(diff),
        "reconciliation_plan_sha256": _doc_sha(reconciliation),
        "reuse_manifest_sha256": _doc_sha(reuse),
        "c5_intent_sha256": sha256_file(output / "c5-intent.json"),
        "c6_handoff_sha256": sha256_file(output / "c6-to-c5-handoff.json"),
        "host_profile_sha256": sha256_file(host_profile_path.resolve()),
    }
    assertions = [
        _assertion(
            "source-revision-advanced",
            str(target_inventory["source"]["revision"]),
            f"different from {prior['inventory']['source']['revision']}",
            passed=target_inventory["source"]["revision"]
            != prior["inventory"]["source"]["revision"],
        ),
        _assertion(
            "semantic-diff-nonempty",
            diff["has_semantic_changes"],
            True,
            passed=bool(diff["has_semantic_changes"]),
        ),
        _assertion(
            "overlay-preserved",
            reconciliation["overlay_preserved"],
            True,
            passed=bool(reconciliation["overlay_preserved"]),
        ),
        _assertion(
            "content-reused",
            reuse["counts"]["reused_by_content_hash"],
            "greater_than_zero",
            passed=reuse["counts"]["reused_by_content_hash"] > 0,
        ),
        _assertion(
            "affected-output-rebuilt",
            reuse["counts"]["rebuilt"],
            "greater_than_zero",
            passed=reuse["counts"]["rebuilt"] > 0,
        ),
        _assertion(
            "immutable-version-advanced",
            project["version"],
            f"different from {prior['project']['version']}",
            passed=project["version"] != prior["project"]["version"],
        ),
        _assertion(
            "same-c6-backend",
            describe_c6()["backend"]["backend_id"],
            "xcp.creative_ir_to_project.v1",
            passed=describe_c6()["backend"]["backend_id"]
            == "xcp.creative_ir_to_project.v1",
        ),
        _assertion(
            "same-c5-lifecycle",
            "C5_AGENT_NATIVE_CREATION_GATE",
            "C5_AGENT_NATIVE_CREATION_GATE",
            passed=True,
        ),
    ]
    report = {
        "schema_version": "xcp-creative-evolution-report-v1",
        "report_id": f"{project['project_id']}.c7-evolution",
        "gate_id": "C7_PROJECT_EVOLUTION_AND_CONTINUOUS_READAPTATION",
        "project_id": str(project["project_id"]),
        "base_version": str(prior["project"]["version"]),
        "target_version": str(project["version"]),
        "identities": identities,
        "assertions": assertions,
        "c5_lifecycle": {
            "authority": "C5_AGENT_NATIVE_CREATION_GATE",
            "receipt_sha256": "",
            "correction_ledger_sha256": "",
            "capture_count": 0,
            "update": "not_run",
            "rollback": "not_run",
            "acceptance": "not_run",
            "exact_transition_bound": False,
        },
        "worker_gap": {
            "native_change_required": False,
            "general_capability_missing": False,
            "evidence": [
                "C7 resolved the revision entirely through C6 IR/backend and C5"
            ],
        },
        "decision": (
            "ready_for_c5"
            if all(item["outcome"] == "pass" for item in assertions)
            else "fail"
        ),
    }
    _validate(report, "report")
    return report


def evolve(
    prior_run: pathlib.Path,
    source: pathlib.Path,
    overlay_path: pathlib.Path,
    output: pathlib.Path,
    host_profile_path: pathlib.Path,
    *,
    project_name: str,
    origin_kind: str,
    origin_locator: str,
    revision: str,
    authorization_basis: str,
    authorization_status: str,
    license_expression: str,
    attribution: str,
) -> dict[str, Any]:
    output = output.resolve()
    if output.exists():
        raise EvolutionError(
            "xcp.evolve.output_exists",
            "The C7 run root must be fresh.",
            stage="evolve",
            field="output",
            expected="new directory",
            actual=str(output),
            correction="select a fresh output directory",
        )
    prior = _load_prior(prior_run.resolve())
    overlay = _load_json(overlay_path.resolve())
    _validate(overlay, "overlay")
    _validate_overlay_bindings(overlay, prior)
    output.mkdir(parents=True)
    _write_new(output / "overlay.json", overlay)

    raw_root = output / "c6-generated"
    c6_result = adapt_c6(
        source.resolve(),
        raw_root,
        host_profile_path.resolve(),
        project_name=project_name,
        origin_kind=origin_kind,
        origin_locator=origin_locator,
        revision=revision,
        authorization_basis=authorization_basis,
        authorization_status=authorization_status,
        license_expression=license_expression,
        attribution=attribution,
    )
    if not c6_result["generation_performed"]:
        raise EvolutionError(
            "xcp.evolve.c6_generation_blocked",
            "The unchanged C6 backend did not admit the target revision.",
            stage="c6",
            field="decision",
            expected="ready_to_generate or ready_with_degradation",
            actual=str(c6_result["decision"]),
            correction="resolve the C6 adaptation plan before C7 merge",
        )
    target_inventory = _load_json(raw_root / "source-inventory.json")
    target_ir = _load_json(raw_root / "creative-ir.json")
    target_plan = _load_json(raw_root / "adaptation-plan.json")
    diff = semantic_diff(
        prior["inventory"],
        prior["ir"],
        target_inventory,
        target_ir,
    )
    _write_new(output / "source-semantic-diff.json", diff)
    reconciliation = reconciliation_plan(
        prior,
        raw_root / "xcp-project",
        overlay,
        _doc_sha(diff),
    )
    _write_new(output / "reconciliation-plan.json", reconciliation)
    if reconciliation["decision"] == "blocked_conflict":
        return {
            "ok": False,
            "schema_version": "xcp-project-evolution-run-v1",
            "decision": "blocked_conflict",
            "conflicts": reconciliation["conflicts"],
            "generation_performed": False,
            "output": str(output),
        }

    project_root = output / "xcp-project"
    project_root.mkdir()
    reuse_entries = _copy_project_by_hash(
        prior["project_root"],
        raw_root / "xcp-project",
        project_root,
    )
    overlay_paths = _apply_overlay(project_root, overlay, reconciliation)
    project = _load_json(project_root / PROJECT_FILENAME)
    if project["version"] == prior["project"]["version"]:
        raise EvolutionError(
            "xcp.evolve.version_not_advanced",
            "The materialized project reuses the immutable prior version.",
            stage="materialize",
            field="xcp-project.json.version",
            expected=f"different from {prior['project']['version']}",
            actual=str(project["version"]),
            correction="set an explicit new semantic version in the overlay",
        )
    validate_project(project_root, host_profile=host_profile_path.resolve())

    for name in (
        "source-inventory.json",
        "semantic-inventory.json",
        "creative-ir.json",
        "adaptation-plan.json",
    ):
        _write_new(output / name, _load_json(raw_root / name))
    source_map = _load_json(raw_root / "source-map.json")
    source_map["xcp_project_sha256"] = sha256_file(
        project_root / PROJECT_FILENAME
    )
    validate_c6(source_map, "source_map")
    _write_new(output / "source-map.json", source_map)
    semantic_inventory = _load_json(output / "semantic-inventory.json")
    fidelity = fidelity_contract(
        semantic_inventory,
        target_ir,
        target_plan,
        project=project,
        source_map=source_map,
    )
    _write_new(output / "fidelity-contract.json", fidelity)
    host_profile = _load_json(host_profile_path.resolve())
    readiness = readiness_report(
        target_inventory,
        semantic_inventory,
        target_ir,
        target_plan,
        fidelity,
        host_profile=host_profile,
        project=project,
        source_map=source_map,
    )
    _write_new(output / "readiness-report.json", readiness)
    intent = _adapted_c5_intent(project, project_root, target_inventory)
    _write_new(output / "c5-intent.json", intent)
    handoff = _adaptation_handoff(output, project)
    _write_new(output / "c6-to-c5-handoff.json", handoff)

    reuse = _reuse_manifest(
        str(project["project_id"]),
        target_inventory,
        host_profile_path,
        overlay,
        project_root,
        reuse_entries,
        overlay_paths,
    )
    _write_new(output / "reuse-manifest.json", reuse)
    report = _preliminary_report(
        prior,
        output,
        target_inventory,
        target_ir,
        target_plan,
        project,
        overlay,
        diff,
        reconciliation,
        reuse,
        host_profile_path,
    )
    _write_new(output / "evolution-report.json", report)
    return {
        "ok": report["decision"] == "ready_for_c5",
        "schema_version": "xcp-project-evolution-run-v1",
        "decision": report["decision"],
        "base_revision": str(prior["inventory"]["source"]["revision"]),
        "target_revision": str(target_inventory["source"]["revision"]),
        "base_version": str(prior["project"]["version"]),
        "target_version": str(project["version"]),
        "semantic_diff_sha256": _doc_sha(diff),
        "overlay_sha256": _doc_sha(overlay),
        "reconciliation_plan_sha256": _doc_sha(reconciliation),
        "reuse_manifest_sha256": _doc_sha(reuse),
        "project_sha256": sha256_file(project_root / PROJECT_FILENAME),
        "project_tree_sha256": project_tree_sha256(project_root),
        "c5_handoff_generated": True,
        "worker_package_required": False,
        "output": str(output),
    }


def finalize(
    run_root: pathlib.Path,
    receipt_path: pathlib.Path,
    ledger_path: pathlib.Path,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    run_root = run_root.resolve()
    report = _load_json(run_root / "evolution-report.json")
    _validate(report, "report")
    if report["decision"] != "ready_for_c5":
        raise EvolutionError(
            "xcp.evolve.not_ready_for_c5",
            "The C7 local gate is not ready for lifecycle finalization.",
            stage="finalize",
            field="decision",
            expected="ready_for_c5",
            actual=str(report["decision"]),
            correction="resolve the local C7 assertions",
        )
    receipt = _load_json(receipt_path.resolve())
    ledger = _load_json(ledger_path.resolve())
    _validate_external(receipt, RECEIPT_SCHEMA, "receipt")
    _validate_external(ledger, LEDGER_SCHEMA, "ledger")
    ledger_sha = sha256_file(ledger_path.resolve())
    if receipt["correction_ledger_sha256"] != ledger_sha:
        raise EvolutionError(
            "xcp.evolve.ledger_binding_mismatch",
            "The C5 receipt does not bind the supplied correction ledger.",
            stage="finalize",
            field="correction_ledger_sha256",
            expected=ledger_sha,
            actual=str(receipt["correction_ledger_sha256"]),
            correction="supply the exact C5 ledger",
        )
    transition = receipt.get("version_transition")
    if not isinstance(transition, dict):
        raise EvolutionError(
            "xcp.evolve.transition_binding_missing",
            "The C5 receipt lacks the exact base/update/rollback binding.",
            stage="finalize",
            field="version_transition",
            expected="xcp-agent-version-transition-v1",
            actual=type(transition).__name__,
            correction="run C7 through the current unchanged C5 lifecycle",
        )
    identities = report["identities"]
    checks = {
        "base.project_sha256": (
            transition["base"]["project_sha256"],
            identities["base_xcp_project_sha256"],
        ),
        "update.project_sha256": (
            transition["update"]["project_sha256"],
            identities["target_xcp_project_sha256"],
        ),
        "rollback.project_sha256": (
            transition["rollback"]["project_sha256"],
            identities["base_xcp_project_sha256"],
        ),
        "receipt.intent_sha256": (
            receipt["intent_sha256"],
            identities["c5_intent_sha256"],
        ),
    }
    for field, (actual, expected) in checks.items():
        if actual != expected:
            raise EvolutionError(
                "xcp.evolve.transition_binding_mismatch",
                "C5 lifecycle evidence is not bound to the C7 bytes.",
                stage="finalize",
                field=field,
                expected=str(expected),
                actual=str(actual),
                correction="rerun C5 with the exact C7 base/update inputs",
            )
    operations = {
        str(item["name"]): str(item["outcome"])
        for item in receipt["operations"]
    }
    blockers_pass = all(
        item["outcome"] == "pass"
        for item in receipt["acceptance"]
        if item["severity"] == "blocker"
    )
    captures = [
        item for item in receipt["artifacts"] if item["role"] == "creative.capture"
    ]
    exact_transition = bool(
        transition["exact_update_activated"]
        and transition["exact_base_restored"]
    )
    passed = bool(
        receipt["decision"] == "pass"
        and operations.get("update") == "pass"
        and operations.get("rollback") == "pass"
        and blockers_pass
        and len(captures) >= 3
        and exact_transition
    )
    final = json.loads(json.dumps(report))
    final["identities"]["c5_receipt_sha256"] = sha256_file(
        receipt_path.resolve()
    )
    final["identities"]["c5_correction_ledger_sha256"] = ledger_sha
    final["c5_lifecycle"] = {
        "authority": "C5_AGENT_NATIVE_CREATION_GATE",
        "receipt_sha256": sha256_file(receipt_path.resolve()),
        "correction_ledger_sha256": ledger_sha,
        "capture_count": len(captures),
        "update": operations.get("update", "not_run"),
        "rollback": operations.get("rollback", "not_run"),
        "acceptance": "pass" if blockers_pass else "fail",
        "exact_transition_bound": exact_transition,
    }
    final["assertions"].extend(
        [
            _assertion(
                "c5-update",
                operations.get("update", "not_run"),
                "pass",
                passed=operations.get("update") == "pass",
            ),
            _assertion(
                "c5-rollback",
                operations.get("rollback", "not_run"),
                "pass",
                passed=operations.get("rollback") == "pass",
            ),
            _assertion(
                "c5-exact-transition",
                exact_transition,
                True,
                passed=exact_transition,
            ),
            _assertion(
                "c5-captures",
                len(captures),
                "greater_equal_3",
                passed=len(captures) >= 3,
            ),
        ]
    )
    final["decision"] = "pass" if passed else "fail"
    _validate(final, "report")
    _write_new(output_path.resolve(), final)
    return {
        "ok": passed,
        "schema_version": "xcp-project-evolution-finalization-v1",
        "decision": final["decision"],
        "output": str(output_path.resolve()),
        "sha256": sha256_file(output_path.resolve()),
        "receipt_sha256": sha256_file(receipt_path.resolve()),
        "correction_ledger_sha256": ledger_sha,
        "capture_count": len(captures),
        "exact_transition_bound": exact_transition,
    }


def describe() -> dict[str, Any]:
    return {
        "ok": True,
        "schema_version": "xcp-project-evolution-description-v1",
        "gate_id": "C7_PROJECT_EVOLUTION_AND_CONTINUOUS_READAPTATION",
        "profile_sha256": sha256_file(PROFILE),
        "inputs": [
            "prior_c6_outputs",
            "authorized_later_source_revision",
            "explicit_hash_bound_overlay",
            "exact_creative_host_profile",
        ],
        "outputs": [
            "source_semantic_diff",
            "three_way_reconciliation_plan",
            "content_hash_reuse_manifest",
            "new_c6_outputs",
            "ordinary_c5_handoff",
            "evolution_report",
        ],
        "backend": describe_c6()["backend"],
        "lifecycle_owner": "C5_AGENT_NATIVE_CREATION_GATE",
        "commands": ["describe", "evolve", "finalize"],
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Evolve a C6 adaptation across an authorized source revision."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("describe")
    run = subparsers.add_parser("evolve")
    run.add_argument("--prior-run", type=pathlib.Path, required=True)
    run.add_argument("--source", type=pathlib.Path, required=True)
    run.add_argument("--overlay", type=pathlib.Path, required=True)
    run.add_argument("--output", type=pathlib.Path, required=True)
    run.add_argument("--host-profile", type=pathlib.Path, required=True)
    run.add_argument("--project-name", required=True)
    run.add_argument(
        "--origin-kind",
        choices=["git", "source_archive", "authorized_local_tree"],
        required=True,
    )
    run.add_argument("--origin-locator", required=True)
    run.add_argument("--revision", required=True)
    run.add_argument(
        "--authorization-basis",
        choices=[
            "open_source_license",
            "provided_by_rightsholder",
            "user_attested_authorization",
        ],
        required=True,
    )
    run.add_argument(
        "--authorization-status",
        choices=["verified", "pending", "rejected"],
        required=True,
    )
    run.add_argument("--license-expression", default="")
    run.add_argument("--attribution", required=True)
    final = subparsers.add_parser("finalize")
    final.add_argument("--run-root", type=pathlib.Path, required=True)
    final.add_argument("--receipt", type=pathlib.Path, required=True)
    final.add_argument("--ledger", type=pathlib.Path, required=True)
    final.add_argument("--output", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "describe":
            result = describe()
        elif args.command == "evolve":
            result = evolve(
                args.prior_run,
                args.source,
                args.overlay,
                args.output,
                args.host_profile,
                project_name=args.project_name,
                origin_kind=args.origin_kind,
                origin_locator=args.origin_locator,
                revision=args.revision,
                authorization_basis=args.authorization_basis,
                authorization_status=args.authorization_status,
                license_expression=args.license_expression,
                attribution=args.attribution,
            )
        else:
            result = finalize(
                args.run_root,
                args.receipt,
                args.ledger,
                args.output,
            )
    except EvolutionError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    except Exception as exc:
        error = EvolutionError(
            "xcp.evolve.unhandled_failure",
            "C7 failed closed on an unexpected error.",
            stage="internal",
            field="exception",
            expected="successful bounded evolution",
            actual=f"{type(exc).__name__}: {exc}",
            correction="inspect the structured failure and preserve the run root",
        )
        print(json.dumps(error.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok") else 2


if __name__ == "__main__":
    raise SystemExit(main())
