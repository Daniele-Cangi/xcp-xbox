"""Source, semantic, plan and target boundaries independent of any adapter."""

from __future__ import annotations

import hashlib
import json
import math
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from jsonschema import Draft202012Validator


def canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def digest(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


class ContractError(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class SourceObservation:
    """Exact source bytes identified without including an absolute checkout path."""

    adapter_id: str
    adapter_version: str
    files: tuple[Mapping[str, Any], ...]

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-source-observation-v1", "adapter_id": self.adapter_id,
                "adapter_version": self.adapter_version, "files": [dict(item) for item in self.files]}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class SourceModel:
    observation_sha256: str
    adapter_id: str
    records: tuple[Mapping[str, Any], ...]
    diagnostics: tuple[str, ...] = ()

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-source-model-v1", "observation_sha256": self.observation_sha256,
                "adapter_id": self.adapter_id, "records": [dict(item) for item in self.records],
                "diagnostics": list(self.diagnostics)}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class ProjectIntent:
    """Author-stated purpose and acceptance coverage, independent of execution."""

    project_id: str
    summary: str
    invariants: tuple[str, ...]
    tolerances: tuple[float, ...] = ()

    def acceptance(self) -> tuple[tuple[str, float], ...]:
        if (not self.project_id or not self.summary or not self.invariants or
                len(set(self.invariants)) != len(self.invariants) or
                any(not isinstance(key, str) or not key for key in self.invariants)):
            raise ContractError("xcp.plan.intent_invalid", "Intent needs an identity, summary and unique invariants")
        tolerances = self.tolerances if self.tolerances else (0.0,) * len(self.invariants)
        if (len(tolerances) != len(self.invariants) or
                any(not isinstance(value, (int, float)) or isinstance(value, bool) or
                    not math.isfinite(value) or value < 0 for value in tolerances)):
            raise ContractError("xcp.plan.acceptance_invalid", "Acceptance tolerances must be finite and nonnegative")
        return tuple(zip(self.invariants, tolerances))

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-project-intent-v2", "project_id": self.project_id,
                "summary": self.summary, "invariants": list(self.invariants),
                "acceptance": [{"key": key, "tolerance": tolerance} for key, tolerance in self.acceptance()]}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class SemanticIR:
    source_model_sha256: str
    records: tuple[Mapping[str, Any], ...]
    required_capabilities: tuple[str, ...]

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-semantic-ir-v1", "source_model_sha256": self.source_model_sha256,
                "records": [dict(item) for item in self.records],
                "required_capabilities": list(self.required_capabilities)}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class TargetProfile:
    target_id: str
    target_version: str
    capabilities: tuple[str, ...]

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-target-profile-v1", "target_id": self.target_id,
                "target_version": self.target_version, "capabilities": list(self.capabilities)}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class AdmissionPlan:
    intent_sha256: str
    source_model_sha256: str
    ir_sha256: str
    target_profile_sha256: str
    capabilities: tuple[str, ...]

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-admission-plan-v1", "intent_sha256": self.intent_sha256,
                "source_model_sha256": self.source_model_sha256,
                "ir_sha256": self.ir_sha256, "target_profile_sha256": self.target_profile_sha256,
                "capabilities": list(self.capabilities)}

    @property
    def sha256(self) -> str:
        return digest(self.document())


class SourceAdapter(Protocol):
    adapter_id: str
    adapter_version: str

    def observe(self, root: Path) -> tuple[SourceObservation, SourceModel, SemanticIR]: ...


class TargetAdapter(Protocol):
    def profile(self) -> TargetProfile: ...
    def lower(self, intent: ProjectIntent, model: SourceModel, ir: SemanticIR,
              plan: AdmissionPlan, output: Path) -> Mapping[str, Any]: ...


def admit(intent: ProjectIntent, model: SourceModel, ir: SemanticIR, target: TargetProfile) -> AdmissionPlan:
    intent.acceptance()
    if ir.source_model_sha256 != model.sha256:
        raise ContractError("xcp.plan.source_model_mismatch", "IR is not bound to this Source Model")
    capabilities = tuple(sorted(set(ir.required_capabilities)))
    missing = set(capabilities) - set(target.capabilities)
    if missing:
        raise ContractError("xcp.plan.capability_unsupported", "Target lacks: " + ", ".join(sorted(missing)))
    return AdmissionPlan(intent.sha256, model.sha256, ir.sha256, target.sha256, capabilities)


def verify_plan(plan: AdmissionPlan, intent: ProjectIntent, model: SourceModel,
                ir: SemanticIR, target: TargetProfile) -> None:
    if plan != admit(intent, model, ir, target):
        raise ContractError("xcp.plan.identity_mismatch", "Plan is not bound to the exact source, IR and target")


def verify_source_chain(observation: SourceObservation, model: SourceModel, ir: SemanticIR) -> None:
    """Check exact bindings before an adapter output can enter planning."""
    if model.observation_sha256 != observation.sha256 or ir.source_model_sha256 != model.sha256:
        raise ContractError("xcp.source.identity_mismatch", "Source artifacts bind different bytes")
    files = {item["path"]: item["sha256"] for item in observation.files}
    if len(files) != len(observation.files):
        raise ContractError("xcp.source.file_duplicate", "Source paths must be unique")
    if tuple(files) != tuple(sorted(files)):
        raise ContractError("xcp.source.file_order_invalid", "Source paths must be canonical")
    ids: set[str] = set()
    for record in (*model.records, *ir.records):
        source_path = record.get("source_path")
        if source_path not in files or record.get("source_sha256") != files[source_path]:
            raise ContractError("xcp.source.reference_invalid", "Semantic record references unbound source bytes")
        record_id = record.get("id")
        if not isinstance(record_id, str) or not record_id:
            raise ContractError("xcp.source.semantic_id_invalid", "Semantic IDs must be nonempty")
    for record in ir.records:
        if record["id"] in ids:
            raise ContractError("xcp.source.semantic_id_duplicate", "Semantic IDs must be unique")
        ids.add(record["id"])
    if tuple(sorted(set(ir.required_capabilities))) != ir.required_capabilities:
        raise ContractError("xcp.source.capability_order_invalid", "Capabilities must be sorted and unique")


def validate_contract(kind: str, document: Mapping[str, Any]) -> None:
    """Validate a public v1 wire document with the packaged JSON Schema."""
    spec_path = Path(__file__).resolve().parent / "spec" / "xcp-platform-contracts-v1.schema.json"
    schema = json.loads(spec_path.read_text(encoding="utf-8"))
    if kind not in schema["$defs"]:
        raise ContractError("xcp.contract.kind_unknown", kind)
    validator = Draft202012Validator({"$ref": f"#/$defs/{kind}", "$defs": schema["$defs"]})
    errors = sorted(validator.iter_errors(document), key=lambda error: tuple(str(part) for part in error.path))
    if errors:
        raise ContractError("xcp.contract.schema_rejected", f"{kind}: {errors[0].message}")
