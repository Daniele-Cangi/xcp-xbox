from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from xcp.adapters.godot import GodotAdapter
from xcp.contracts import ContractError, ProjectIntent, admit, digest, validate_contract, verify_source_chain
from xcp.evidence import (EvidenceIdentity, ExecutionReceipt, Invariant, Observation,
                          StageEvidence, attribute_stage, compare, compare_replay,
                          verify_comparison_report)
from xcp.project import verify_bundle
from xcp.targets.archive import ArchiveTarget, build_archive_bundle


ROOT = Path(__file__).resolve().parents[1]


def test_godot_admission_lowering_and_bundle_are_portable(tmp_path: Path) -> None:
    source_a = tmp_path / "a"
    source_b = tmp_path / "b"
    shutil.copytree(ROOT / "examples" / "godot-minimal", source_a)
    shutil.copytree(source_a, source_b)
    adapter = GodotAdapter()
    observed_a, model_a, ir_a = adapter.observe(source_a)
    observed_b, model_b, ir_b = adapter.observe(source_b)
    assert observed_a.sha256 == observed_b.sha256
    assert model_a.sha256 == model_b.sha256
    assert ir_a.sha256 == ir_b.sha256
    assert len(ir_a.records) == 4
    verify_source_chain(observed_a, model_a, ir_a)
    target = ArchiveTarget()
    intent = ProjectIntent("synthetic", "Preserve the scene", ("scene",))
    plan = admit(intent, model_a, ir_a, target.profile())
    lowered = target.lower(intent, model_a, ir_a, plan, tmp_path / "project")
    assert lowered["plan_sha256"] == plan.sha256
    bundle = build_archive_bundle(tmp_path / "project", tmp_path / "bundle")
    assert bundle["ok"]
    assert verify_bundle(tmp_path / "bundle").metadata()["content_sha256"] == bundle["content_sha256"]
    changed_intent = ProjectIntent("synthetic", "Preserve the scene under a revised authority", ("scene",))
    changed_plan = admit(changed_intent, model_a, ir_a, target.profile())
    target.lower(changed_intent, model_a, ir_a, changed_plan, tmp_path / "changed-project")
    changed_bundle = build_archive_bundle(tmp_path / "changed-project", tmp_path / "changed-bundle")
    assert changed_plan.sha256 != plan.sha256
    assert (tmp_path / "project/modules/semantic.json").read_bytes() != (tmp_path / "changed-project/modules/semantic.json").read_bytes()
    assert changed_bundle["content_sha256"] != bundle["content_sha256"]

    # Different checkout locations are harmless; changed source bytes are not.
    with (source_b / "main.tscn").open("a", encoding="utf-8") as stream:
        stream.write("\n# semantic change\n")
    changed, changed_model, changed_ir = adapter.observe(source_b)
    assert changed.sha256 != observed_a.sha256
    assert changed_model.sha256 != model_a.sha256
    assert changed_ir.sha256 != ir_a.sha256


def test_unmodeled_godot_script_fails_target_admission(tmp_path: Path) -> None:
    shutil.copytree(ROOT / "examples" / "godot-minimal", tmp_path / "source")
    (tmp_path / "source" / "actor.gd").write_text("extends Node2D\n", encoding="utf-8")
    _, model, ir = GodotAdapter().observe(tmp_path / "source")
    assert "script_behavior_unmodeled" in model.diagnostics
    with pytest.raises(ContractError) as raised:
        admit(ProjectIntent("synthetic", "Preserve scene", ("scene",)), model, ir, ArchiveTarget().profile())
    assert raised.value.code == "xcp.plan.capability_unsupported"


def test_evidence_requires_exact_identity_coverage_and_execution() -> None:
    intent = ProjectIntent("synthetic", "Preserve scene and position", ("position", "scene"), (0.1, 0.0))
    identity = EvidenceIdentity(*(f"{index:064x}" for index in range(1, 4)), intent.sha256,
                                *(f"{index:064x}" for index in range(4, 9)))
    source = Observation(identity, "source", {"position": 10.0, "scene": "main"})
    target = Observation(identity, "target", {"position": 10.05, "scene": "main"})
    invariants = (Invariant("position", 0.1), Invariant("scene"))
    receipt = ExecutionReceipt(identity.plan_sha256, identity.target_profile_sha256,
                               identity.execution_sha256, identity.result_sha256, "succeeded", target.sha256)
    assert compare(source, target, intent, invariants, receipt)["fidelity"] == "pass"
    valid = compare(source, target, intent, invariants, receipt)
    verify_comparison_report(valid, source, target, intent, invariants, receipt)
    altered = dict(valid)
    altered["fidelity"] = "diverged"
    with pytest.raises(ContractError, match="differs"):
        verify_comparison_report(altered, source, target, intent, invariants, receipt)
    failed = ExecutionReceipt(identity.plan_sha256, identity.target_profile_sha256,
                              identity.execution_sha256, identity.result_sha256, "failed", target.sha256)
    assert compare(source, target, intent, invariants, failed)["fidelity"] == "insufficient"
    with pytest.raises(ContractError, match="pre-execution intent"):
        compare(source, target, intent, (Invariant("position", 1.0), Invariant("scene")), receipt)
    missing_target = Observation(identity, "target", {"position": 10.05})
    missing_receipt = ExecutionReceipt(identity.plan_sha256, identity.target_profile_sha256,
                                       identity.execution_sha256, identity.result_sha256,
                                       "succeeded", missing_target.sha256)
    assert compare(source, missing_target, intent, invariants, missing_receipt)["fidelity"] == "insufficient"
    diverged_target = Observation(identity, "target", {"position": 10.5, "scene": "main"})
    diverged_receipt = ExecutionReceipt(identity.plan_sha256, identity.target_profile_sha256,
                                        identity.execution_sha256, identity.result_sha256,
                                        "succeeded", diverged_target.sha256)
    assert compare(source, diverged_target, intent, invariants, diverged_receipt)["fidelity"] == "diverged"
    with pytest.raises(ContractError, match="Receipt does not bind"):
        compare(source, diverged_target, intent, invariants, receipt)
    other = EvidenceIdentity(*(f"{index:064x}" for index in range(2, 11)))
    with pytest.raises(ContractError, match="different contexts"):
        compare(source, Observation(other, "target", target.values), intent, invariants, receipt)
    bad_receipt = ExecutionReceipt(identity.plan_sha256, identity.target_profile_sha256,
                                   "0" * 64, identity.result_sha256, "succeeded", target.sha256)
    with pytest.raises(ContractError, match="Receipt does not bind"):
        compare(source, target, intent, invariants, bad_receipt)

    context_sha = digest(identity.comparison_context())
    stages = (
        StageEvidence(context_sha, "source_observation", "passed", "a" * 64),
        StageEvidence(context_sha, "source_model", "failed", "b" * 64),
    )
    attribution = attribute_stage(valid, stages)
    assert attribution["first_failed_stage"] == "source_model"
    validate_contract("stage_evidence", stages[0].document())
    validate_contract("stage_attribution", attribution)
    assert attribute_stage(valid, stages[1:])["first_failed_stage"] == "undetermined"

    second_identity = EvidenceIdentity(*identity.comparison_context(), "e" * 64, "f" * 64)
    second_target = Observation(second_identity, "target", dict(target.values))
    second_receipt = ExecutionReceipt(second_identity.plan_sha256, second_identity.target_profile_sha256,
                                      second_identity.execution_sha256, second_identity.result_sha256,
                                      "succeeded", second_target.sha256)
    replay = compare_replay(target, second_target, receipt, second_receipt)
    assert replay["status"] == "equal"
    validate_contract("replay_comparison", replay)
    incomplete_replay = compare_replay(target, second_target, failed, second_receipt)
    assert incomplete_replay["status"] == "insufficient"
    with pytest.raises(ContractError, match="distinct executions"):
        compare_replay(target, target, receipt, receipt)
