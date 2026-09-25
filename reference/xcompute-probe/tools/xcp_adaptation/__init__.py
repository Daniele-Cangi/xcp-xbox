"""Modular C6 external-source adaptation pipeline."""

from __future__ import annotations

import pathlib
from typing import Any

from xcp_creative_project import sha256_file

from .adapters import (
    ADAPTERS,
    GODOT_NUMBER,
    Godot2SourceAdapter,
    detect_source,
    inspect_source,
)
from .core import (
    CLASSIFICATIONS,
    IDENTIFIER,
    INTENT_SCHEMA,
    LEDGER_SCHEMA,
    PROFILE,
    RECEIPT_SCHEMA,
    ROOT,
    SCHEMAS,
    CapabilityPlanner,
    CreativeIrEmitter,
    EngineSourceModel,
    FidelityOracle,
    SemanticPass,
    SourceAdaptError,
    SourceAdapter,
    XcpLoweringBackend,
    _document_sha,
    _load_json,
    _portable_id,
    _validate,
    _validate_schema,
    _write_new,
)
from .fidelity import (
    _fidelity_disposition,
    _validate_fidelity_contract_integrity,
    fidelity_contract,
    finalize_readiness,
    probe_fidelity,
    readiness_report,
)
from .handoff import (
    _adaptation_handoff,
    _adapted_c5_intent,
    _handoff_artifact,
    _validate_correction_ledger,
    _verify_handoff_artifacts,
)
from .ir import _campaign_scene_records, _ir_records, _validate_ir_integrity
from .lowering import (
    MODULE_SCHEMAS,
    _asset_id,
    _audio_module,
    _module_validate,
    _selected_world_scene,
    _state_scalar,
    _world2d_campaign_module,
    _world2d_hud_module,
    _world2d_module,
    _world2d_solution,
    generate_project,
)
from .planning import (
    _host_sets,
    _plan_item,
    _validate_plan_integrity,
    create_plan,
)
from .semantics import SEMANTIC_CATEGORIES, _validate_semantic_inventory_integrity
from .source_models import (
    ASSET_EXTENSIONS,
    EXECUTABLE_EXTENSIONS,
    TEXT_EXTENSIONS,
    Detection,
    _media_type,
    _relative_files,
    _role,
)

def adapt(
    source: pathlib.Path,
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
    project_version: str = "1.3.2",
) -> dict[str, Any]:
    output = output.resolve()
    if output.exists():
        raise SourceAdaptError(
            "xcp.adapt.output_exists",
            "The adaptation run output must be fresh.",
            stage="adapt",
            field="output",
            expected="new directory",
            actual=str(output),
            correction="select a fresh run directory",
        )
    output.mkdir(parents=True)
    inventory, adapter = inspect_source(
        source,
        project_name=project_name,
        origin_kind=origin_kind,
        origin_locator=origin_locator,
        revision=revision,
        authorization_basis=authorization_basis,
        authorization_status=authorization_status,
        license_expression=license_expression,
        attribution=attribution,
    )
    inventory_path = output / "source-inventory.json"
    _write_new(inventory_path, inventory)
    if inventory["authorization"]["status"] != "verified":
        raise SourceAdaptError(
            "xcp.adapt.authorization_not_verified",
            "Semantic extraction stops until source authorization is verified.",
            stage="extract",
            field="authorization.status",
            expected="verified",
            actual=str(inventory["authorization"]["status"]),
            correction="verify source and asset rights before adapting",
        )
    parser_semantic_inventory = adapter.extract_semantic_inventory(
        source.resolve(),
        inventory,
    )
    source_model = adapter.extract_source_model(
        source.resolve(),
        inventory,
        semantic_inventory=parser_semantic_inventory,
    )
    semantic_inventory = adapter.semantic_pass().run(source_model)
    semantic_inventory_path = output / "semantic-inventory.json"
    _write_new(semantic_inventory_path, semantic_inventory)
    ir = adapter.emit_creative_ir(
        source_model,
        semantic_inventory,
        inventory,
    )
    ir_path = output / "creative-ir.json"
    _write_new(ir_path, ir)
    plan, host_profile = create_plan(inventory, ir, host_profile_path)
    plan_path = output / "adaptation-plan.json"
    _write_new(plan_path, plan)

    project: dict[str, Any] | None = None
    source_map: dict[str, Any] | None = None
    if plan["decision"] in {"ready_to_generate", "ready_with_degradation"}:
        project, source_map = generate_project(
            source.resolve(),
            inventory,
            ir,
            plan,
            output / "xcp-project",
            project_version=project_version,
        )
        _write_new(output / "source-map.json", source_map)
    fidelity = fidelity_contract(
        semantic_inventory,
        ir,
        plan,
        project=project,
        source_map=source_map,
    )
    _write_new(output / "fidelity-contract.json", fidelity)
    readiness = readiness_report(
        inventory,
        semantic_inventory,
        ir,
        plan,
        fidelity,
        host_profile=host_profile,
        project=project,
        source_map=source_map,
    )
    _write_new(output / "readiness-report.json", readiness)
    if project is not None:
        intent = _adapted_c5_intent(
            project,
            output / "xcp-project",
            inventory,
        )
        _write_new(output / "c5-intent.json", intent)
        handoff = _adaptation_handoff(output, project)
        _write_new(output / "c6-to-c5-handoff.json", handoff)
    return {
        "ok": readiness["decision"] == "ready_source_faithful",
        "schema_version": "xcp-source-adaptation-run-v2",
        "adapter_id": adapter.adapter_id,
        "adapter_version": adapter.adapter_version,
        "source_inventory_sha256": _document_sha(inventory),
        "semantic_inventory_sha256": _document_sha(semantic_inventory),
        "creative_ir_sha256": _document_sha(ir),
        "adaptation_plan_sha256": _document_sha(plan),
        "fidelity_contract_sha256": _document_sha(fidelity),
        "generation_decision": plan["decision"],
        "decision": readiness["decision"],
        "achieved_level": fidelity["achieved_level"],
        "generation_performed": project is not None,
        "worker_gap": plan["worker_gap"],
        "c5_handoff_generated": project is not None,
        "c5_handoff_status": (
            "diagnostic_only" if project is not None else "not_ready"
        ),
        "output": str(output),
    }


def describe() -> dict[str, Any]:
    return {
        "ok": True,
        "schema_version": "xcp-source-adaptation-description-v2",
        "gate_id": "C6_AGENT_NATIVE_SOURCE_ADAPTATION_V1",
        "profile_sha256": sha256_file(PROFILE),
        "adapters": [
            {
                "adapter_id": adapter.adapter_id,
                "adapter_version": adapter.adapter_version,
                "emits": "xcp-creative-ir-v1",
                "semantic_inventory": "xcp-creative-semantic-inventory-v1",
                "may_emit_xcp_project": False,
                "may_target_xbox": False,
            }
            for adapter in ADAPTERS
        ],
        "backend": {
            "backend_id": "xcp.creative_ir_to_project.v1",
            "backend_version": "1.4.1",
            "input": "xcp-creative-ir-v1",
            "output": "xcp-creative-project-v1",
        },
        "fidelity": {
            "contract": "xcp-creative-fidelity-contract-v1",
            "readiness": "xcp-creative-adaptation-readiness-v2",
            "target_level": "source_faithful",
            "preliminary_runs_are_ready": False,
            "reference_oracle_required": True,
            "human_xbox_playtest_required": True,
        },
        "commands": [
            "describe",
            "detect",
            "adapt",
            "probe-fidelity",
            "finalize",
        ],
    }


__all__ = [
    'ADAPTERS',
    'ASSET_EXTENSIONS',
    'CLASSIFICATIONS',
    'CapabilityPlanner',
    'CreativeIrEmitter',
    'Detection',
    'EXECUTABLE_EXTENSIONS',
    'EngineSourceModel',
    'FidelityOracle',
    'GODOT_NUMBER',
    'Godot2SourceAdapter',
    'IDENTIFIER',
    'INTENT_SCHEMA',
    'LEDGER_SCHEMA',
    'MODULE_SCHEMAS',
    'PROFILE',
    'RECEIPT_SCHEMA',
    'ROOT',
    'SCHEMAS',
    'SEMANTIC_CATEGORIES',
    'SemanticPass',
    'SourceAdaptError',
    'SourceAdapter',
    'TEXT_EXTENSIONS',
    'XcpLoweringBackend',
    '_adaptation_handoff',
    '_adapted_c5_intent',
    '_asset_id',
    '_audio_module',
    '_campaign_scene_records',
    '_document_sha',
    '_fidelity_disposition',
    '_handoff_artifact',
    '_host_sets',
    '_ir_records',
    '_load_json',
    '_media_type',
    '_module_validate',
    '_plan_item',
    '_portable_id',
    '_relative_files',
    '_role',
    '_selected_world_scene',
    '_state_scalar',
    '_validate',
    '_validate_correction_ledger',
    '_validate_fidelity_contract_integrity',
    '_validate_ir_integrity',
    '_validate_plan_integrity',
    '_validate_schema',
    '_validate_semantic_inventory_integrity',
    '_verify_handoff_artifacts',
    '_world2d_campaign_module',
    '_world2d_hud_module',
    '_world2d_module',
    '_world2d_solution',
    '_write_new',
    'adapt',
    'create_plan',
    'describe',
    'detect_source',
    'fidelity_contract',
    'finalize_readiness',
    'generate_project',
    'inspect_source',
    'probe_fidelity',
    'readiness_report',
]
