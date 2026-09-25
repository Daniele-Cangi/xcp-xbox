"""Device-independent reference conformance checks."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

from ..adapters.godot import GodotAdapter
from ..contracts import ContractError, ProjectIntent, admit, digest, validate_contract, verify_source_chain
from ..evidence import (EvidenceIdentity, ExecutionReceipt, Invariant, Observation,
                        StageEvidence, attribute_stage, compare, compare_replay,
                        verify_comparison_report)
from ..project import verify_bundle
from ..targets.archive import ArchiveTarget, build_archive_bundle
from ..xvm.reference import DEFAULT_SPEC, XvmValidationError, execute_program, load_json, verify_program


ROOT = Path(__file__).resolve().parent
INPUTS = [1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1]
XVM_EXPECTED = {
    "xvm-v2-call-chain.json": ("20010000000000000000000000000000", 13),
    "xvm-v2-structured-control-v2.json": ("08000000000000000000000000000000", 11),
    "xvm-v2-structured-control-phase-b.json": ("64000000030000000100000000000000", 30),
    "xvm-v2-typed-memory-v1.json": ("00000000000000000100000000000000", 8),
    "xvm-v2-cancel-resume-loop.json": ("00003c00000000000000000000000000", 7864449),
}


def xvm() -> dict[str, object]:
    spec = load_json(DEFAULT_SPEC)
    checked = []
    for filename, (output_hex, fuel) in XVM_EXPECTED.items():
        program = load_json(ROOT / "vectors" / filename)
        proof = verify_program(program, spec)["static_fuel_proof"]
        result = execute_program(program, spec, INPUTS)
        if result["output_hex"] != output_hex or result["fuel_consumed"] != fuel:
            raise AssertionError(filename + " diverged from its reference vector")
        if proof["worst_case_fuel"] < fuel:
            raise AssertionError(filename + " exceeded admitted fuel")
        checked.append(filename)
    invalid = load_json(ROOT / "vectors" / "xvm-v2-call-chain.json")
    invalid["max_fuel"] = 12
    try:
        verify_program(invalid, spec)
    except XvmValidationError as exc:
        if exc.code != "xvm.static_fuel_program_limit_insufficient":
            raise
    else:
        raise AssertionError("Insufficient fuel was admitted")
    return {"suite": "xvm-v2", "passed": len(checked) + 1, "vectors": checked}


def adapter() -> dict[str, object]:
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        a, b = base / "a", base / "b"
        shutil.copytree(ROOT / "godot_fixture", a)
        shutil.copytree(a, b)
        source = GodotAdapter()
        first = source.observe(a)
        second = source.observe(b)
        verify_source_chain(*first)
        verify_source_chain(*second)
        for kind, value in zip(("source_observation", "source_model", "semantic_ir"), first):
            validate_contract(kind, value.document())
        if tuple(value.sha256 for value in first) != tuple(value.sha256 for value in second):
            raise AssertionError("Adapter identity depends on checkout location")
        (b / "behavior.gd").write_text("extends Node2D\n", encoding="utf-8")
        _, model, ir = source.observe(b)
        intent = ProjectIntent("synthetic", "Preserve scene structure", ("scene",))
        try:
            admit(intent, model, ir, ArchiveTarget().profile())
        except ContractError as exc:
            if exc.code != "xcp.plan.capability_unsupported":
                raise
        else:
            raise AssertionError("Unmodeled behavior was admitted")
    return {"suite": "godot-structure-v1", "passed": 3}


def target() -> dict[str, object]:
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        source, model, ir = GodotAdapter().observe(ROOT / "godot_fixture")
        verify_source_chain(source, model, ir)
        target = ArchiveTarget()
        intent = ProjectIntent("synthetic", "Preserve scene structure", ("scene",))
        plan = admit(intent, model, ir, target.profile())
        validate_contract("project_intent", intent.document())
        validate_contract("target_profile", target.profile().document())
        validate_contract("admission_plan", plan.document())
        target.lower(intent, model, ir, plan, base / "project")
        built = build_archive_bundle(base / "project", base / "bundle")
        if verify_bundle(base / "bundle").metadata()["content_sha256"] != built["content_sha256"]:
            raise AssertionError("Bundle identity changed")
        changed_intent = ProjectIntent("synthetic", "Revised structural authority", ("scene",))
        changed_plan = admit(changed_intent, model, ir, target.profile())
        target.lower(changed_intent, model, ir, changed_plan, base / "changed-project")
        changed = build_archive_bundle(base / "changed-project", base / "changed-bundle")
        if changed["content_sha256"] == built["content_sha256"]:
            raise AssertionError("A changed plan did not change archive bytes")
    return {"suite": "archive-target-v1", "passed": 3}


def evidence() -> dict[str, object]:
    intent = ProjectIntent("synthetic", "Preserve state", ("state",))
    common = [f"{i:064x}" for i in range(1, 4)] + [intent.sha256] + [f"{i:064x}" for i in range(4, 7)]
    source_identity = EvidenceIdentity(*common, f"{7:064x}", f"{8:064x}")
    target_identity = EvidenceIdentity(*common, f"{9:064x}", f"{10:064x}")
    source = Observation(source_identity, "source", {"state": 1})
    target = Observation(target_identity, "target", {"state": 1})
    invariants = (Invariant("state"),)
    receipt = ExecutionReceipt(target_identity.plan_sha256, target_identity.target_profile_sha256,
                               target_identity.execution_sha256, target_identity.result_sha256,
                               "succeeded", target.sha256)
    report = compare(source, target, intent, invariants, receipt)
    validate_contract("observation", source.document())
    validate_contract("observation", target.document())
    validate_contract("execution_receipt", receipt.document())
    validate_contract("comparison", report)
    verify_comparison_report(report, source, target, intent, invariants, receipt)
    if report["fidelity"] != "pass":
        raise AssertionError("Exact observations did not pass")
    try:
        compare(source, target, intent, (Invariant("state", 1.0),), receipt)
    except ContractError as exc:
        if exc.code != "xcp.evidence.acceptance_mismatch":
            raise
    else:
        raise AssertionError("Post-hoc tolerance substitution was admitted")
    failed = ExecutionReceipt(receipt.plan_sha256, receipt.target_profile_sha256,
                              receipt.execution_sha256, receipt.result_sha256, "failed", target.sha256)
    if compare(source, target, intent, invariants, failed)["fidelity"] != "insufficient":
        raise AssertionError("Execution failure claimed fidelity")
    another = EvidenceIdentity(*target_identity.comparison_context(), f"{11:064x}", f"{12:064x}")
    another_target = Observation(another, "target", dict(target.values))
    another_receipt = ExecutionReceipt(another.plan_sha256, another.target_profile_sha256,
                                       another.execution_sha256, another.result_sha256,
                                       "succeeded", another_target.sha256)
    replay = compare_replay(target, another_target, receipt, another_receipt)
    validate_contract("replay_comparison", replay)
    if replay["status"] != "equal":
        raise AssertionError("Exact replay diverged")
    context_sha = digest(target_identity.comparison_context())
    stages = (StageEvidence(context_sha, "source_observation", "passed", f"{13:064x}"),
              StageEvidence(context_sha, "source_model", "failed", f"{14:064x}"))
    attribution = attribute_stage(report, stages)
    validate_contract("stage_attribution", attribution)
    if attribution["first_failed_stage"] != "source_model":
        raise AssertionError("Stage routing lost its exact binding")
    return {"suite": "evidence-v1", "passed": 5}


SUITES = {"xvm": xvm, "adapter": adapter, "target": target, "evidence": evidence}
