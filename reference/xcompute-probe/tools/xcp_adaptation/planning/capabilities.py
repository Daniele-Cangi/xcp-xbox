"""Host-bound capability planning for canonical Creative IR."""

from __future__ import annotations

import json
import pathlib
from typing import Any

from xcp_creative_project import sha256_bytes, validate_host_profile

from ..core import (
    CLASSIFICATIONS,
    SourceAdaptError,
    _document_sha,
    _portable_id,
    _validate,
)
from ..ir import _campaign_scene_records, _ir_records, _validate_ir_integrity


def _host_sets(profile: dict[str, Any]) -> tuple[set[str], set[str], set[str]]:
    modules = {
        str(item["id"])
        for item in profile["module_kinds"]
        if item["admitted"] is True
    }
    capabilities = {
        str(item["id"])
        for item in profile["capabilities"]
        if item["admitted"] is True
    }
    media = {
        str(item["media_type"])
        for item in profile["asset_formats"]
        if item["admitted"] is True
    }
    return modules, capabilities, media


def _plan_item(
    collection: str,
    record: dict[str, Any],
    *,
    modules: set[str],
    capabilities: set[str],
    media: set[str],
    project_id: str,
    campaign_required: bool,
) -> dict[str, Any]:
    record_id = str(record["id"])
    classification = "translated"
    required: list[str] = []
    targets: list[str] = []
    rationale = ""
    fidelity = "low"
    residual: list[str] = []

    if collection in {"scenes", "entities"}:
        semantic_kind = str(record.get("semantic_kind", ""))
        if semantic_kind.startswith("ui.") and "xcp.ui.v1" in modules:
            required = ["ui.controller"]
            targets = ["module:interface"]
            rationale = "Translate controller-safe UI structure through xcp.ui.v1."
        else:
            required = (
                [
                    "input.gamepad",
                    "render.canvas2d",
                    "render.sprite_atlas",
                    "world2d.deterministic",
                    "world2d.gravity",
                    *(
                        ["world2d.campaign"]
                        if campaign_required
                        else []
                    ),
                ]
                if "xcp.world2d.v2" in modules
                else ["world2d.deterministic"]
            )
            targets = ["module:world"]
            missing_world_capabilities = sorted(
                set(required) - capabilities
            )
            if missing_world_capabilities:
                classification = "unsupported"
                fidelity = "blocking"
                residual = [
                    "Provide the missing general authored world2d capabilities: "
                    + ", ".join(missing_world_capabilities)
                    + "."
                ]
                rationale = (
                    "The current canvas module draws nodes but cannot execute a "
                    "stateful tile world with collision and entity interactions."
                )
            else:
                rationale = (
                    "Translate authored spatial, atlas, gravity and scene "
                    "progression semantics through the campaign-aware "
                    "world2d host."
                    if campaign_required
                    else
                    "Translate authored spatial, atlas and gravity semantics "
                    "through xcp.world2d.v2."
                    if "xcp.world2d.v2" in modules
                    else "Translate spatial semantics through the general 2D world."
                )
    elif collection == "behaviors":
        effects = " ".join(str(item) for item in record["effects"]).lower()
        requires_world = any(
            marker in effects
            for marker in (
                "collision",
                "destruction",
                "grid movement",
                "entity",
            )
        )
        if requires_world and "world2d.deterministic" not in capabilities:
            classification = "unsupported"
            required = ["world2d.deterministic"]
            targets = ["module:world"]
            fidelity = "blocking"
            residual = [
                "Express the behavior in the general deterministic 2D world rules."
            ]
            rationale = (
                "Existing scalar behavior rules cannot model spatial queries, "
                "collisions and entity spawning/destruction."
            )
        elif "behavior.deterministic" in capabilities:
            required = ["behavior.deterministic"]
            targets = ["module:logic"]
            rationale = "Translate bounded event/state behavior to xcp.behavior.v1."
        else:
            classification = "unsupported"
            required = ["behavior.deterministic"]
            targets = ["module:logic"]
            fidelity = "blocking"
            residual = ["Provide deterministic behavior execution."]
            rationale = "The host does not admit deterministic behavior modules."
    elif collection == "input_actions":
        required = ["input.gamepad"]
        targets = ["module:world"]
        if "input.gamepad" not in capabilities:
            classification = "unsupported"
            fidelity = "blocking"
            residual = ["Provide bounded controller input."]
        rationale = "Map semantic actions to the controller-safe XCP input surface."
    elif collection == "ui":
        required = ["render.canvas2d"]
        if record["attributes"].get("adaptation_entry") is True:
            targets = ["module:hud"]
            fidelity = "low"
            residual = [
                "Advanced source menus and focus graphs remain outside the in-level HUD."
            ]
            rationale = (
                "Translate the selected source scene guidance into a bounded "
                "non-modal canvas HUD."
            )
        else:
            classification = "degraded"
            targets = [f"project:{project_id}"]
            fidelity = "medium"
            residual = [
                "This non-entry source UI surface is inventoried but is not "
                "present in the first playable XCP scene."
            ]
            rationale = (
                "Preserve provenance while avoiding a permanent diagnostic "
                "panel over the playable scene."
            )
        if "render.canvas2d" not in capabilities:
            classification = "unsupported"
            fidelity = "blocking"
            residual = ["Provide bounded canvas rendering for the in-level HUD."]
    elif collection == "assets":
        media_type = str(record["media_type"])
        targets = [f"asset:{_portable_id(record_id, maximum=96)}"]
        if media_type in media:
            classification = "preserved_directly"
            fidelity = "none"
            rationale = "Copy exact admitted asset bytes into the XCP project."
        elif media_type == "audio/ogg" and "audio/wav" in media:
            classification = "degraded"
            fidelity = "medium"
            residual = [
                "Transcode authorized Ogg audio to bounded PCM WAV PC-side."
            ]
            rationale = (
                "Audio semantics are preservable, but the shared backend does not "
                "yet include the deterministic Ogg-to-WAV converter."
            )
        else:
            classification = "degraded"
            fidelity = "medium"
            targets = [f"project:{project_id}"]
            residual = [
                f"Replace or convert unsupported media type {media_type} PC-side."
            ]
            rationale = "The source asset format is not admitted by this host."
    elif collection == "state":
        required = ["state.local"]
        targets = [f"state:{_portable_id(record_id, maximum=96)}"]
        if "state.local" not in capabilities:
            classification = "unsupported"
            fidelity = "blocking"
            residual = ["Provide bounded project-local state."]
        rationale = "Translate source state to bounded XCP project-local state."
    else:
        raise AssertionError(collection)

    return {
        "item_id": _portable_id(
            f"{collection}.{record_id}", prefix="adapt", maximum=128
        ),
        "source_refs": list(record["source_refs"]),
        "ir_refs": [record_id],
        "classification": classification,
        "required_host_capabilities": sorted(required),
        "xcp_targets": sorted(targets),
        "rationale": rationale,
        "fidelity_impact": fidelity,
        "residual_work": residual,
    }


def create_plan(
    inventory: dict[str, Any],
    ir: dict[str, Any],
    host_profile_path: pathlib.Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    _validate(inventory, "inventory")
    _validate(ir, "ir")
    _validate_ir_integrity(ir, inventory)
    host = validate_host_profile(host_profile_path)
    profile = host.manifest
    modules, capabilities, media = _host_sets(profile)
    campaign_required = len(_campaign_scene_records(ir)) > 1
    items = [
        _plan_item(
            collection,
            record,
            modules=modules,
            capabilities=capabilities,
            media=media,
            project_id=str(ir["project"]["id"]),
            campaign_required=campaign_required,
        )
        for collection, record in _ir_records(ir)
    ]
    counts = {
        classification: sum(
            item["classification"] == classification for item in items
        )
        for classification in CLASSIFICATIONS
    }
    if counts["human_intervention_required"]:
        decision = "blocked_human_intervention"
    elif counts["unsupported"]:
        decision = "blocked_unsupported"
    elif counts["degraded"] or counts["substituted"]:
        decision = "ready_with_degradation"
    else:
        decision = "ready_to_generate"
    world_gap_items = [
        item
        for item in items
        if item["classification"] == "unsupported"
        and any(
            capability in item["required_host_capabilities"]
            for capability in (
                "world2d.deterministic",
                "world2d.campaign",
            )
        )
    ]
    plan = {
        "schema_version": "xcp-creative-adaptation-plan-v1",
        "plan_id": f"{ir['project']['id']}.plan",
        "source_inventory_sha256": _document_sha(inventory),
        "creative_ir_sha256": _document_sha(ir),
        "host": {
            "profile_id": profile["profile_id"],
            "profile_sha256": sha256_bytes(host.manifest_bytes),
            "discovered_through": "describe_creative_host",
        },
        "backend": {
            "backend_id": "xcp.creative_ir_to_project.v1",
            "backend_version": "1.4.1",
            "input_schema_version": "xcp-creative-ir-v1",
            "output_schema_version": "xcp-creative-project-v1",
        },
        "items": items,
        "classification_counts": counts,
        "decision": decision,
        "worker_gap": {
            "native_change_required": bool(world_gap_items),
            "general_capability_missing": bool(world_gap_items),
            "evidence": (
                [
                    (
                        f"{len(world_gap_items)} semantic records require a "
                        "general world2d.deterministic capability absent from "
                        f"{profile['profile_id']}."
                    ),
                    (
                        "The gap is shared by tile games, simulations and spatial "
                        "interactive applications; it is not Minilens-specific."
                    ),
                ]
                if world_gap_items
                else []
            ),
        },
    }
    _validate(plan, "plan")
    _validate_plan_integrity(plan, ir)
    return plan, profile


def _validate_plan_integrity(
    plan: dict[str, Any],
    ir: dict[str, Any],
) -> None:
    if plan["creative_ir_sha256"] != _document_sha(ir):
        raise SourceAdaptError(
            "xcp.adapt.plan_ir_mismatch",
            "The adaptation plan does not bind exact Creative IR bytes.",
            stage="plan",
            field="creative_ir_sha256",
            expected=_document_sha(ir),
            actual=str(plan["creative_ir_sha256"]),
            correction="re-plan from the exact IR",
        )
    expected_ids = {str(record["id"]) for _, record in _ir_records(ir)}
    planned: list[str] = [
        str(reference)
        for item in plan["items"]
        for reference in item["ir_refs"]
    ]
    if set(planned) != expected_ids or len(planned) != len(expected_ids):
        raise SourceAdaptError(
            "xcp.adapt.plan_coverage_invalid",
            "Every semantic IR element must be classified exactly once.",
            stage="plan",
            field="items[].ir_refs",
            expected=str(len(expected_ids)),
            actual=str(len(planned)),
            correction="repair the shared planner coverage",
        )
    actual_counts = {
        classification: sum(
            item["classification"] == classification
            for item in plan["items"]
        )
        for classification in CLASSIFICATIONS
    }
    if actual_counts != plan["classification_counts"]:
        raise SourceAdaptError(
            "xcp.adapt.plan_counts_invalid",
            "Classification counts do not match plan items.",
            stage="plan",
            field="classification_counts",
            expected=json.dumps(actual_counts, sort_keys=True),
            actual=json.dumps(plan["classification_counts"], sort_keys=True),
            correction="recompute the plan from canonical items",
        )
