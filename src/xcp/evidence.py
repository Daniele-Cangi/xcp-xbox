"""Exact-bound differential evidence and conservative claim decisions."""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Any, Mapping

from .contracts import ContractError, ProjectIntent, digest


@dataclass(frozen=True)
class EvidenceIdentity:
    source_sha256: str
    source_model_sha256: str
    ir_sha256: str
    intent_sha256: str
    project_sha256: str
    plan_sha256: str
    target_profile_sha256: str
    execution_sha256: str
    result_sha256: str

    def document(self) -> dict[str, str]:
        return {key: getattr(self, key) for key in self.__dataclass_fields__}

    def validate(self) -> None:
        for field, value in self.document().items():
            if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
                raise ContractError("xcp.evidence.identity_invalid", f"Invalid {field}")

    def comparison_context(self) -> tuple[str, ...]:
        return (self.source_sha256, self.source_model_sha256, self.ir_sha256, self.intent_sha256,
                self.project_sha256, self.plan_sha256, self.target_profile_sha256)


@dataclass(frozen=True)
class Observation:
    identity: EvidenceIdentity
    role: str
    values: Mapping[str, Any]

    def document(self) -> dict[str, Any]:
        return {"schema_version": "xcp-observation-v1", "identity": self.identity.document(),
                "role": self.role, "values": dict(self.values)}

    @property
    def sha256(self) -> str:
        return digest(self.document())


@dataclass(frozen=True)
class Invariant:
    key: str
    tolerance: float = 0.0


@dataclass(frozen=True)
class ExecutionReceipt:
    """Target-authored outcome; authenticity is the target's responsibility."""

    plan_sha256: str
    target_profile_sha256: str
    execution_sha256: str
    result_sha256: str
    status: str
    target_observation_sha256: str

    def document(self) -> dict[str, str]:
        return {"schema_version": "xcp-execution-receipt-v2", "plan_sha256": self.plan_sha256,
                "target_profile_sha256": self.target_profile_sha256,
                "execution_sha256": self.execution_sha256, "result_sha256": self.result_sha256,
                "status": self.status, "target_observation_sha256": self.target_observation_sha256}

    @property
    def sha256(self) -> str:
        return digest(self.document())


def _validate_receipt(receipt: ExecutionReceipt, target: Observation) -> None:
    if receipt.status not in {"succeeded", "failed", "canceled"}:
        raise ContractError("xcp.evidence.receipt_status_invalid", "Unknown execution status")
    if (receipt.plan_sha256 != target.identity.plan_sha256 or
            receipt.target_profile_sha256 != target.identity.target_profile_sha256 or
            receipt.execution_sha256 != target.identity.execution_sha256 or
            receipt.result_sha256 != target.identity.result_sha256 or
            receipt.target_observation_sha256 != target.sha256):
        raise ContractError("xcp.evidence.receipt_mismatch", "Receipt does not bind target observation")


def compare(source: Observation, target: Observation, intent: ProjectIntent,
            invariants: tuple[Invariant, ...], receipt: ExecutionReceipt) -> dict[str, Any]:
    source.identity.validate()
    target.identity.validate()
    if source.identity.comparison_context() != target.identity.comparison_context():
        raise ContractError("xcp.evidence.identity_mismatch", "Source and target observations bind different contexts")
    if source.identity.intent_sha256 != intent.sha256:
        raise ContractError("xcp.evidence.intent_mismatch", "Observations do not bind this intent")
    if source.role != "source" or target.role != "target":
        raise ContractError("xcp.evidence.role_invalid", "Expected source and target observations")
    _validate_receipt(receipt, target)
    if not invariants or len({item.key for item in invariants}) != len(invariants):
        raise ContractError("xcp.evidence.invariants_invalid", "A nonempty unique invariant set is required")
    if tuple((item.key, item.tolerance) for item in invariants) != intent.acceptance():
        raise ContractError("xcp.evidence.acceptance_mismatch", "Comparison criteria differ from pre-execution intent")
    comparisons: list[dict[str, Any]] = []
    for invariant in invariants:
        if not math.isfinite(invariant.tolerance) or invariant.tolerance < 0:
            raise ContractError("xcp.evidence.tolerance_invalid", invariant.key)
        if invariant.key not in source.values or invariant.key not in target.values:
            comparisons.append({"key": invariant.key, "status": "missing",
                                "source_present": invariant.key in source.values,
                                "target_present": invariant.key in target.values})
            continue
        a, b = source.values[invariant.key], target.values[invariant.key]
        numeric = (isinstance(a, (int, float)) and not isinstance(a, bool) and
                   isinstance(b, (int, float)) and not isinstance(b, bool))
        distance = abs(a - b) if numeric else None
        if numeric:
            passed = distance <= invariant.tolerance
        else:
            passed = a == b and invariant.tolerance == 0
        item = {"key": invariant.key, "status": "pass" if passed else "diverged",
                "tolerance": invariant.tolerance, "source_value_sha256": digest(a),
                "target_value_sha256": digest(b)}
        if distance is not None:
            item["distance"] = distance
        comparisons.append(item)
    execution_succeeded = receipt.status == "succeeded"
    fidelity = "insufficient" if not execution_succeeded or any(c["status"] == "missing" for c in comparisons) else (
        "diverged" if any(c["status"] == "diverged" for c in comparisons) else "pass")
    report = {"schema_version": "xcp-comparison-v1", "source_identity": source.identity.document(),
              "target_identity": target.identity.document(),
              "source_observation_sha256": source.sha256, "target_observation_sha256": target.sha256,
              "execution_receipt_sha256": receipt.sha256, "execution_succeeded": execution_succeeded,
              "fidelity": fidelity, "comparisons": comparisons}
    report["sha256"] = digest(report)
    return report


def verify_comparison_report(report: Mapping[str, Any], source: Observation, target: Observation,
                             intent: ProjectIntent, invariants: tuple[Invariant, ...],
                             receipt: ExecutionReceipt) -> None:
    expected = compare(source, target, intent, invariants, receipt)
    if dict(report) != expected:
        raise ContractError("xcp.evidence.report_mismatch", "Comparison report differs from exact observations")


def compare_replay(first: Observation, second: Observation,
                   first_receipt: ExecutionReceipt, second_receipt: ExecutionReceipt) -> dict[str, Any]:
    """Compare two independently executed target observations for logical replay."""
    first.identity.validate()
    second.identity.validate()
    if first.role != "target" or second.role != "target":
        raise ContractError("xcp.evidence.replay_role_invalid", "Replay requires target observations")
    if first.identity.comparison_context() != second.identity.comparison_context():
        raise ContractError("xcp.evidence.replay_context_mismatch", "Replay contexts differ")
    _validate_receipt(first_receipt, first)
    _validate_receipt(second_receipt, second)
    if first.identity.execution_sha256 == second.identity.execution_sha256:
        raise ContractError("xcp.evidence.replay_execution_reused", "Replay requires distinct executions")
    status = ("insufficient" if first_receipt.status != "succeeded" or second_receipt.status != "succeeded"
              else "equal" if first.values == second.values else "diverged")
    report = {"schema_version": "xcp-replay-comparison-v1",
              "context_sha256": digest(first.identity.comparison_context()),
              "first_observation_sha256": first.sha256, "second_observation_sha256": second.sha256,
              "first_receipt_sha256": first_receipt.sha256, "second_receipt_sha256": second_receipt.sha256,
              "status": status}
    report["sha256"] = digest(report)
    return report


STAGE_ORDER = ("source_observation", "source_model", "semantic_ir", "planning",
               "lowering", "target_runtime", "verification")


@dataclass(frozen=True)
class StageEvidence:
    context_sha256: str
    stage: str
    outcome: str
    artifact_sha256: str

    def document(self) -> dict[str, str]:
        return {"schema_version": "xcp-stage-evidence-v1", "context_sha256": self.context_sha256,
                "stage": self.stage, "outcome": self.outcome, "artifact_sha256": self.artifact_sha256}


def attribute_stage(report: Mapping[str, Any], stages: tuple[StageEvidence, ...]) -> dict[str, Any]:
    """Route the first independently reported failed stage; no causal proof implied."""
    report_body = {key: value for key, value in report.items() if key != "sha256"}
    if report.get("sha256") != digest(report_body):
        raise ContractError("xcp.evidence.report_hash_invalid", "Comparison report hash differs")
    source_identity = report["source_identity"]
    target_identity = report["target_identity"]
    keys = ("source_sha256", "source_model_sha256", "ir_sha256", "intent_sha256",
            "project_sha256", "plan_sha256", "target_profile_sha256")
    context = tuple(source_identity[key] for key in keys)
    if context != tuple(target_identity[key] for key in keys):
        raise ContractError("xcp.evidence.identity_mismatch", "Comparison contexts differ")
    context_sha = digest(context)
    seen: dict[str, StageEvidence] = {}
    for stage in stages:
        if stage.stage not in STAGE_ORDER or stage.outcome not in {"passed", "failed"}:
            raise ContractError("xcp.evidence.stage_invalid", "Unknown stage or outcome")
        if stage.stage in seen or stage.context_sha256 != context_sha:
            raise ContractError("xcp.evidence.stage_binding_invalid", "Stage evidence is duplicate or unbound")
        if len(stage.artifact_sha256) != 64 or any(ch not in "0123456789abcdef" for ch in stage.artifact_sha256):
            raise ContractError("xcp.evidence.stage_artifact_invalid", "Stage artifact identity is invalid")
        seen[stage.stage] = stage
    first_failed = "undetermined"
    for name in STAGE_ORDER:
        item = seen.get(name)
        if item is None:
            break
        if item.outcome == "failed":
            first_failed = name
            break
    result = {"schema_version": "xcp-stage-attribution-v1", "comparison_sha256": report["sha256"],
              "context_sha256": context_sha, "first_failed_stage": first_failed,
              "stage_evidence_sha256": [digest(seen[name].document()) for name in STAGE_ORDER if name in seen]}
    result["sha256"] = digest(result)
    return result
