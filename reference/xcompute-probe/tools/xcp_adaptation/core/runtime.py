"""Shared deterministic runtime primitives for C6 source adaptation."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
from typing import Any

from jsonschema import Draft202012Validator

from xcp_creative_project import canonical_json_bytes, sha256_bytes


ROOT = pathlib.Path(__file__).resolve().parents[3]

SCHEMAS = {
    "inventory": ROOT / "schemas" / "xcp-creative-source-inventory-v1.schema.json",
    "semantic_inventory": (
        ROOT / "schemas" / "xcp-creative-semantic-inventory-v1.schema.json"
    ),
    "ir": ROOT / "schemas" / "xcp-creative-ir-v1.schema.json",
    "plan": ROOT / "schemas" / "xcp-creative-adaptation-plan-v1.schema.json",
    "source_map": ROOT / "schemas" / "xcp-creative-source-map-v1.schema.json",
    "fidelity_contract": (
        ROOT / "schemas" / "xcp-creative-fidelity-contract-v1.schema.json"
    ),
    "fidelity_evidence": (
        ROOT / "schemas" / "xcp-creative-fidelity-evidence-v1.schema.json"
    ),
    "readiness": (
        ROOT / "schemas" / "xcp-creative-adaptation-readiness-v2.schema.json"
    ),
    "handoff": (
        ROOT / "schemas" / "xcp-creative-adaptation-handoff-v2.schema.json"
    ),
}

INTENT_SCHEMA = ROOT / "schemas" / "xcp-agent-project-intent-v1.schema.json"

LEDGER_SCHEMA = ROOT / "schemas" / "xcp-agent-correction-ledger-v1.schema.json"

RECEIPT_SCHEMA = ROOT / "schemas" / "xcp-agent-lifecycle-receipt-v1.schema.json"

PROFILE = (
    ROOT / "profiles" / "creative" / "xcp-agent-native-source-adaptation-v1.json"
)

IDENTIFIER = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$")

CLASSIFICATIONS = (
    "preserved_directly",
    "translated",
    "substituted",
    "degraded",
    "unsupported",
    "human_intervention_required",
)


class SourceAdaptError(RuntimeError):
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
        raise SourceAdaptError(
            "xcp.adapt.file_missing",
            "A required C6 machine document is missing.",
            stage="load",
            field="path",
            expected="existing UTF-8 JSON file",
            actual=str(path),
            correction="provide the exact document and retry",
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SourceAdaptError(
            "xcp.adapt.json_invalid",
            "A C6 machine document is not valid UTF-8 JSON.",
            stage="load",
            field="path",
            expected="valid UTF-8 JSON",
            actual=str(path),
            correction="correct the document and retry",
        ) from exc


def _validate(document: Any, schema_key: str) -> None:
    _validate_schema(document, SCHEMAS[schema_key], stage=schema_key)


def _validate_schema(
    document: Any,
    schema_path: pathlib.Path,
    *,
    stage: str,
) -> None:
    schema = _load_json(schema_path)
    Draft202012Validator.check_schema(schema)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(document),
        key=lambda item: tuple(str(part) for part in item.absolute_path),
    )
    if errors:
        first = errors[0]
        location = ".".join(str(part) for part in first.absolute_path)
        raise SourceAdaptError(
            "xcp.adapt.schema_rejected",
            "A C6 machine document failed its authoritative schema.",
            stage=stage,
            field=location,
            expected=str(first.validator),
            actual=first.message,
            correction="correct the structured field and retry",
            retryable=True,
        )


def _write_new(path: pathlib.Path, document: Any) -> None:
    path = path.resolve()
    if path.exists():
        raise SourceAdaptError(
            "xcp.adapt.output_exists",
            "A C6 output would overwrite an existing path.",
            stage="write",
            field="path",
            expected="a new output path",
            actual=str(path),
            correction="select a fresh output path",
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_json_bytes(document))
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _document_sha(document: Any) -> str:
    return sha256_bytes(canonical_json_bytes(document))


def _portable_id(value: str, *, prefix: str = "", maximum: int = 96) -> str:
    lowered = value.lower().replace("\\", "/")
    lowered = re.sub(r"[^a-z0-9._-]+", ".", lowered)
    lowered = re.sub(r"[._-]{2,}", ".", lowered).strip("._-")
    if not lowered or not lowered[0].isalpha():
        lowered = f"x.{lowered}" if lowered else "x"
    candidate = f"{prefix}.{lowered}" if prefix else lowered
    if len(candidate) > maximum:
        suffix = hashlib.sha256(candidate.encode("utf-8")).hexdigest()[:12]
        candidate = f"{candidate[: maximum - 13].rstrip('._-')}.{suffix}"
    if not IDENTIFIER.fullmatch(candidate):
        raise AssertionError(f"internal identifier normalization failed: {candidate}")
    return candidate
