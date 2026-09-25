"""Fail-closed source-fidelity accounting, probes, and finalization."""

from __future__ import annotations

import json
import pathlib
from typing import Any

from xcp_creative_project import (
    canonical_json_bytes,
    sha256_bytes,
    sha256_file,
)

from ..core import (
    CLASSIFICATIONS,
    INTENT_SCHEMA,
    RECEIPT_SCHEMA,
    SourceAdaptError,
    _document_sha,
    _load_json,
    _portable_id,
    _validate,
    _validate_schema,
    _write_new,
)
from ..handoff import (
    _validate_correction_ledger,
    _verify_handoff_artifacts,
)
from ..ir import _campaign_scene_records, _ir_records
from ..lowering import (
    _selected_world_scene,
    _world2d_campaign_module,
    _world2d_module,
    _world2d_solution,
)
from ..semantics import (
    SEMANTIC_CATEGORIES,
    _validate_semantic_inventory_integrity,
)


def _fidelity_disposition(
    record: dict[str, Any],
    *,
    represented_scene_paths: set[str],
    represented_instance_names: dict[str, set[str]],
    degraded_instance_names: dict[str, set[str]],
    represented_dependency_paths: dict[str, set[str]],
    authored_world: bool,
    audio_composed: bool,
    composed_audio_node_paths: set[str],
    ir_refs: list[str],
    xcp_refs: list[str],
) -> tuple[str, str, str, list[str]]:
    category = str(record["category"])
    kind = str(record["kind"])
    path = str(record["locator"]["path"])
    name = str(record["name"])
    lowered_name = name.lower()
    represented = path in represented_scene_paths
    node_name = lowered_name.split(":", 1)[0]
    verification = (
        "visual"
        if category in {"rendering", "ui"}
        else "audio"
        if category == "audio"
        else "state"
        if category == "state_persistence"
        else "behavioral"
        if category
        in {
            "scene_graph",
            "animation",
            "collision_physics",
            "behavior",
            "input",
            "lifecycle",
        }
        else "structural"
    )
    if (
        category == "audio"
        and kind == "godot.audio.node_feature"
        and audio_composed
        and path in composed_audio_node_paths
    ):
        return (
            "translated",
            verification,
            "The source effect player is projected to exact admitted PCM bytes and a source-derived deterministic world event cue.",
            [],
        )
    if (
        (category == "asset" or kind == "godot.audio.asset")
        and ir_refs
    ):
        if category == "audio" and path.lower().endswith(".ogg"):
            return (
                "degraded",
                verification,
                "The authorized Ogg bytes are inventoried but not composed by the current PCM-only backend.",
                ["Transcode and compose the exact source audio semantics PC-side."],
            )
        return (
            "preserved_directly",
            verification,
            "The exact admitted source asset bytes are copied into the generated project.",
            [],
        )
    if category == "input":
        action = lowered_name.rsplit(".", 1)[-1]
        if action in {"btn_left", "btn_right", "btn_up", "btn_down"}:
            return (
                "translated",
                verification,
                "The directional source action is mapped to the bounded XCP gamepad surface.",
                [],
            )
        return (
            "unsupported",
            verification,
            "The current vertical slice does not implement this source input action.",
            [f"Implement and differentially verify source input action {name}."],
        )
    if category == "project_configuration":
        if lowered_name.endswith("application.name"):
            return (
                "translated",
                verification,
                "The source project name is projected into XCP project metadata.",
                [],
            )
        return (
            "unsupported",
            verification,
            "This source project setting has no proven equivalent in the generated project.",
            [f"Define and verify an XCP projection for source setting {name}."],
        )
    if category == "lifecycle":
        return (
            "substituted" if "entrypoint" in kind else "unsupported",
            verification,
            (
                "The source menu entrypoint is replaced by a direct diagnostic level launch."
                if "entrypoint" in kind
                else "The source lifecycle/autoload semantics are absent from the current backend."
            ),
            [
                "Preserve the source scene lifecycle, autoload ownership and transitions."
            ],
        )
    if category == "scene_graph":
        if represented and any(
            marker in lowered_name
            for marker in (
                "level:node2d",
                "start:position2d",
            )
        ):
            return (
                "translated",
                verification,
                "This authored campaign scene element has a bounded world2d projection.",
                [],
            )
        if (
            represented
            and node_name
            in represented_instance_names.get(path, set())
        ):
            if node_name in degraded_instance_names.get(path, set()):
                return (
                    "degraded",
                    verification,
                    "The source contains a redundant co-located solid instance that the canonical backend collapses.",
                    [
                        "Prove source-equivalent rendering and behavior for the collapsed duplicate instance."
                    ],
                )
            return (
                "translated",
                verification,
                "The authored packed-scene instance is projected to an exact scene-specific world2d entity.",
                [],
            )
        if represented and "tilemap:tilemap" in lowered_name:
            if authored_world:
                return (
                    "translated",
                    verification,
                    "Tile occupancy, authored atlas regions, offsets, source coordinates and blocking semantics are projected through xcp.world2d.v2.",
                    [],
                )
            return (
                "degraded",
                verification,
                "Tile occupancy is projected, but source tile identities, atlas regions and scene composition are not preserved.",
                [
                    "Preserve tile identity, atlas region, transform, layer and collision semantics."
                ],
            )
        if (
            represented
            and authored_world
            and "background:sprite" in lowered_name
        ):
            return (
                "translated",
                verification,
                "The authored background node is flattened into the scene-specific world background asset binding.",
                [],
            )
        if represented and "label:label" in lowered_name:
            return (
                "degraded",
                verification,
                "Some tutorial text is projected without the exact hierarchy, typography, bounds or activation areas.",
                ["Preserve the complete source UI scene graph and spatial behavior."],
            )
        return (
            "unsupported",
            verification,
            "This source scene node is not represented by the single-scene vertical slice.",
            [f"Represent and verify source scene node {path}#{name}."],
        )
    if category == "resource":
        dependencies = {
            str(value)
            for value in record.get("dependencies", [])
        }
        if (
            represented
            and kind == "godot.resource.external"
            and dependencies
            and dependencies.issubset(
                represented_dependency_paths.get(path, set())
            )
        ):
            return (
                "translated",
                verification,
                "The exact source dependency is projected into the scene-specific world module as an authored asset, tileset semantic or entity instance.",
                [],
            )
        if represented and ir_refs:
            return (
                "degraded",
                verification,
                "The selected scene resource contributes coarse data, but its exact Godot resource semantics are not represented.",
                ["Preserve resource identity, dependency binding and authored properties."],
            )
        return (
            "unsupported",
            verification,
            "This source resource or dependency is not represented in the generated project.",
            [f"Project and verify source resource {path}#{name}."],
        )
    if category == "rendering":
        if represented and authored_world and any(
            marker in lowered_name
            for marker in ("tilemap", "background:sprite")
        ):
            return (
                "translated",
                verification,
                "The authored background or tile atlas region is projected with source-space placement through xcp.world2d.v2.",
                [],
            )
        if represented and ("tilemap" in lowered_name or "label" in lowered_name):
            return (
                "degraded",
                verification,
                "The current output approximates this visual role without source transforms, atlas regions, layers and styling.",
                ["Add reference-frame visual equivalence for this rendering element."],
            )
        return (
            "unsupported",
            verification,
            "This authored rendering element is absent from the current output.",
            [f"Render and reference-compare {path}#{name}."],
        )
    if category == "ui":
        if represented and "label" in lowered_name:
            return (
                "degraded",
                verification,
                "Text content is partially retained, but the authored UI layout and interaction areas are not.",
                ["Preserve source UI hierarchy, layout, focus and activation behavior."],
            )
        return (
            "unsupported",
            verification,
            "This source UI element is absent from the diagnostic level.",
            [f"Implement and visually verify source UI element {path}#{name}."],
        )
    if category == "collision_physics":
        if represented and authored_world and "tilemap" in lowered_name:
            return (
                "translated",
                verification,
                "The source TileMap collision classes are projected to solid, climbable and hazard cells in the deterministic world2d module.",
                [],
            )
        if represented and "tilemap" in lowered_name:
            return (
                "degraded",
                verification,
                "Grid blocking is approximated, but exact collision shapes, layers, masks, areas and triggers are not.",
                [
                    "Preserve and differentially verify authored collision and trigger semantics."
                ],
            )
        return (
            "unsupported",
            verification,
            "This source collision or physics semantic is absent from the current world model.",
            [f"Implement collision/physics semantic {path}#{name}."],
        )
    if category == "behavior":
        translated_member_state: dict[str, set[str]] = {
            "entities/entity.gd": {
                "TILE_SIZE",
                "TileConfig",
                "destroy_after_acid",
                "fall",
                "fall_though_ladders",
                "goal",
                "is_moving",
                "level_holder",
                "level_holder_path",
                "movement",
                "movement_check_collision",
                "movement_original",
                "movement_speed",
                "pause_frames",
                "push_direction",
                "pushable",
                "ray_nodes",
                "ray_status",
                "score_goal_on_destroy",
                "speed_multiplier",
                "tile_directions",
                "tile_types",
                "tilemap",
                "tilemap_path",
                "wait_frames",
            },
            "entities/tile_config.gd": {
                "DEFAULT_CLEAR_TILE",
                "DEFAULT_TILE",
                "TILES",
                "TILESET",
                "TILE_CLIMB",
                "TILE_EMPTY",
                "TILE_SINK",
                "TILE_SOLID",
            },
            "main/level_holder.gd": {
                "acid_animation_pos",
                "acid_animation_time",
                "current_level",
                "current_pack",
                "goal_wait",
                "goals_left",
                "goals_taken",
                "goals_total",
                "level_node",
                "level_scene",
                "level_tileset",
                "player",
                "raw_packs",
                "tile_map_acid_x_end",
                "tile_map_acid_x_start",
                "tile_map_acid_y",
                "turns",
            },
            "pickups/pickup.gd": {
                "Entity",
                "PICK_ALL",
                "PICK_PLAYER",
                "Player",
                "goal",
                "level_holder",
                "level_holder_path",
                "meta",
                "pause_frames",
                "pickable_by",
                "picked",
            },
        }
        if (
            kind == "godot.behavior.member_state"
            and name in translated_member_state.get(path, set())
        ):
            return (
                "translated",
                verification,
                "This source member state is represented by deterministic world entities, tile classes, objectives, inventory or campaign state.",
                [],
            )
        selected_behavior = path in {
            "entities/player.gd",
            "entities/entity.gd",
            "levels/tutorial_level.gd",
        }
        if selected_behavior and name in {
            "next_move",
            "move_in_direction",
            "can_move_in_direction",
            "turn",
            "check_boxes",
        }:
            if authored_world:
                return (
                    "translated",
                    verification,
                    "The selected source grid movement, pushing, gravity, hazard destruction and completion rule are projected into the deterministic world2d state machine.",
                    [],
                )
            return (
                "degraded",
                verification,
                "The vertical slice approximates part of this gameplay rule without executing the source state machine.",
                ["Translate the full rule and compare source/XCP state transitions."],
            )
        return (
            "unsupported",
            verification,
            "This source function or script behavior is not represented by the current backend.",
            [f"Translate and behaviorally verify {path}#{name}."],
        )
    if category == "animation":
        return (
            "unsupported",
            verification,
            "Source animation tracks, timing and state selection are not represented.",
            [f"Preserve and reference-compare animation {path}#{name}."],
        )
    if category == "state_persistence":
        if (
            kind == "godot.state.persisted_member"
            and path == "shared/save_manager.gd"
            and name == "save_path"
            and ir_refs
            and xcp_refs
        ):
            return (
                "translated",
                verification,
                "The source reached-level save slot is projected to bounded project-local campaign state and verified across a fresh worker session.",
                [],
            )
        return (
            "unsupported",
            verification,
            "This source state variable or persistence semantic is not explicitly mapped.",
            [f"Map lifecycle, default and persistence semantics for {path}#{name}."],
        )
    return (
        "unsupported",
        verification,
        "No proven backend mapping exists for this semantic record.",
        [f"Define a measured mapping for {path}#{name}."],
    )


def fidelity_contract(
    semantic_inventory: dict[str, Any],
    ir: dict[str, Any],
    plan: dict[str, Any],
    *,
    project: dict[str, Any] | None,
    source_map: dict[str, Any] | None,
) -> dict[str, Any]:
    ir_refs_by_source: dict[str, set[str]] = {}
    for _, record in _ir_records(ir):
        for source_ref in record["source_refs"]:
            ir_refs_by_source.setdefault(str(source_ref), set()).add(
                str(record["id"])
            )
    xcp_refs_by_source: dict[str, set[str]] = {}
    if source_map is not None:
        for entry in source_map["entries"]:
            for source_ref in entry["source_refs"]:
                xcp_refs_by_source.setdefault(str(source_ref), set()).update(
                    str(reference) for reference in entry["xcp_refs"]
                )
    selected_scene = _selected_world_scene(ir)
    represented_scenes = (
        _campaign_scene_records(ir)
        if project is not None
        and any(
            module["kind"] == "xcp.world2d.campaign.v1"
            for module in project["modules"]
        )
        else [selected_scene]
    )
    represented_scene_paths = {
        str(scene["attributes"]["source_path"])
        for scene in represented_scenes
    }
    represented_instance_names: dict[str, set[str]] = {}
    degraded_instance_names: dict[str, set[str]] = {}
    represented_dependency_paths: dict[str, set[str]] = {}
    for scene in represented_scenes:
        attributes = scene["attributes"]
        path = str(attributes["source_path"])
        represented_instance_names[path] = {
            str(encoded).split("|", 1)[0].lower()
            for encoded in attributes.get("grid_instances", [])
        }
        degraded_instance_names[path] = {
            str(encoded).split("|", 1)[0].lower()
            for encoded in attributes.get(
                "grid_duplicate_instances",
                [],
            )
        }
        dependencies = {
            str(encoded).split("|")[1]
            for encoded in attributes.get("grid_instances", [])
        }
        dependencies.update(
            str(encoded).split("|")[4]
            for encoded in attributes.get("grid_instances", [])
            if len(str(encoded).split("|")) == 5
            and str(encoded).split("|")[4]
        )
        dependencies.update(
            str(encoded).split("|")[2]
            for encoded in attributes.get("grid_tile_visuals", [])
            if len(str(encoded).split("|")) >= 3
        )
        if attributes.get("grid_tile_visuals"):
            dependencies.add("shared/tileset.tres")
        if attributes.get("grid_background_asset"):
            dependencies.add(str(attributes["grid_background_asset"]))
        represented_dependency_paths[path] = dependencies
    authored_world = project is not None and any(
        module["kind"] == "xcp.world2d.v2"
        for module in project["modules"]
    )
    audio_composed = project is not None and any(
        module["kind"] == "xcp.audio.v1"
        for module in project["modules"]
    )
    composed_audio_events = {
        str(event)
        for asset in ir["assets"]
        if asset["media_type"] == "audio/wav"
        and any(
            reference == "module:audio"
            for source_ref in asset["source_refs"]
            for reference in xcp_refs_by_source.get(
                str(source_ref),
                set(),
            )
        )
        for event in asset.get("audio_cues", [])
    }
    composed_audio_node_paths: set[str] = set()
    if composed_audio_events & {
        "world.entity-pushed",
        "world.entity-destroyed",
        "world.explosion",
    }:
        composed_audio_node_paths.add("entities/entity.tscn")
    if "world.item-collected" in composed_audio_events:
        composed_audio_node_paths.add("pickups/pickup.tscn")
    mappings: list[dict[str, Any]] = []
    for record in semantic_inventory["records"]:
        source_refs = [str(value) for value in record["source_refs"]]
        ir_refs = sorted(
            {
                reference
                for source_ref in source_refs
                for reference in ir_refs_by_source.get(source_ref, set())
            }
        )
        xcp_refs = sorted(
            {
                reference
                for source_ref in source_refs
                for reference in xcp_refs_by_source.get(source_ref, set())
            }
        )
        classification, verification, rationale, residual_work = (
            _fidelity_disposition(
                record,
                represented_scene_paths=represented_scene_paths,
                represented_instance_names=represented_instance_names,
                degraded_instance_names=degraded_instance_names,
                represented_dependency_paths=represented_dependency_paths,
                authored_world=authored_world,
                audio_composed=audio_composed,
                composed_audio_node_paths=composed_audio_node_paths,
                ir_refs=ir_refs,
                xcp_refs=xcp_refs,
            )
        )
        mappings.append(
            {
                "semantic_ref": record["semantic_id"],
                "category": record["category"],
                "classification": classification,
                "ir_refs": ir_refs,
                "xcp_refs": xcp_refs,
                "verification": verification,
                "rationale": rationale,
                "residual_work": residual_work,
            }
        )
    coverage = []
    for category in SEMANTIC_CATEGORIES:
        category_mappings = [
            mapping
            for mapping in mappings
            if mapping["category"] == category
        ]
        coverage.append(
            {
                "category": category,
                "total": len(category_mappings),
                **{
                    classification: sum(
                        mapping["classification"] == classification
                        for mapping in category_mappings
                    )
                    for classification in CLASSIFICATIONS
                },
            }
        )
    blocking = sum(
        mapping["classification"]
        not in {"preserved_directly", "translated"}
        for mapping in mappings
    )
    has_vertical_slice = project is not None and any(
        mapping["classification"] in {"translated", "degraded"}
        and mapping["category"] in {"scene_graph", "behavior", "input"}
        for mapping in mappings
    )
    achieved_level = (
        "behavioral_subset"
        if has_vertical_slice and authored_world
        else "vertical_slice"
        if has_vertical_slice
        else "asset_preview"
    )
    reference_oracle = [
        {
            "dimension": dimension,
            "required": (
                dimension != "audio"
                or semantic_inventory["summary"]["category_counts"]["audio"] > 0
            ),
            "outcome": (
                "not_run"
                if dimension != "audio"
                or semantic_inventory["summary"]["category_counts"]["audio"] > 0
                else "not_applicable"
            ),
            "evidence_refs": [],
        }
        for dimension in ("structural", "behavioral", "visual", "audio", "state")
    ]
    contract = {
        "schema_version": "xcp-creative-fidelity-contract-v1",
        "contract_id": f"{ir['project']['id']}.fidelity",
        "identities": {
            "source_inventory_sha256": semantic_inventory[
                "source_inventory_sha256"
            ],
            "semantic_inventory_sha256": _document_sha(semantic_inventory),
            "creative_ir_sha256": _document_sha(ir),
            "adaptation_plan_sha256": _document_sha(plan),
            "xcp_project_sha256": (
                sha256_bytes(canonical_json_bytes(project)) if project else ""
            ),
            "host_profile_sha256": str(plan["host"]["profile_sha256"]),
        },
        "target": {
            "level": "source_faithful",
            "source_scope": "whole_project",
            "policy": "no_unaccounted_semantics_and_reference_equivalence_v1",
        },
        "achieved_level": achieved_level,
        "mappings": mappings,
        "coverage": coverage,
        "reference_oracle": reference_oracle,
        "human_playtest": {
            "required": True,
            "outcome": "not_run",
            "evidence_refs": [],
        },
        "decision": (
            "human_intervention_required"
            if any(
                mapping["classification"] == "human_intervention_required"
                for mapping in mappings
            )
            else "not_ready"
        ),
    }
    _validate(contract, "fidelity_contract")
    _validate_fidelity_contract_integrity(
        contract,
        semantic_inventory,
        ir,
        plan,
        project=project,
    )
    if blocking == 0 and semantic_inventory["scope"]["parse_failures"]:
        raise AssertionError("parse failures cannot produce zero fidelity blockers")
    return contract


def _validate_fidelity_contract_integrity(
    contract: dict[str, Any],
    semantic_inventory: dict[str, Any],
    ir: dict[str, Any],
    plan: dict[str, Any],
    *,
    project: dict[str, Any] | None,
) -> None:
    expected_identities = {
        "source_inventory_sha256": semantic_inventory[
            "source_inventory_sha256"
        ],
        "semantic_inventory_sha256": _document_sha(semantic_inventory),
        "creative_ir_sha256": _document_sha(ir),
        "adaptation_plan_sha256": _document_sha(plan),
        "xcp_project_sha256": (
            sha256_bytes(canonical_json_bytes(project)) if project else ""
        ),
        "host_profile_sha256": str(plan["host"]["profile_sha256"]),
    }
    if contract["identities"] != expected_identities:
        raise SourceAdaptError(
            "xcp.adapt.fidelity_identity_mismatch",
            "Fidelity contract does not bind the exact pipeline documents.",
            stage="fidelity_contract",
            field="identities",
            expected=json.dumps(expected_identities, sort_keys=True),
            actual=json.dumps(contract["identities"], sort_keys=True),
            correction="rebuild the fidelity contract from exact inputs",
        )
    semantic_refs = {
        str(record["semantic_id"])
        for record in semantic_inventory["records"]
    }
    mapped_refs = [
        str(mapping["semantic_ref"])
        for mapping in contract["mappings"]
    ]
    if set(mapped_refs) != semantic_refs or len(mapped_refs) != len(semantic_refs):
        raise SourceAdaptError(
            "xcp.adapt.fidelity_coverage_invalid",
            "Every semantic inventory record must have exactly one fidelity disposition.",
            stage="fidelity_contract",
            field="mappings[].semantic_ref",
            expected=str(len(semantic_refs)),
            actual=str(len(mapped_refs)),
            correction="repair whole-project fidelity accounting",
        )
    coverage_by_category = {
        str(item["category"]): item for item in contract["coverage"]
    }
    if set(coverage_by_category) != set(SEMANTIC_CATEGORIES):
        raise SourceAdaptError(
            "xcp.adapt.fidelity_categories_invalid",
            "Fidelity coverage must report every semantic category exactly once.",
            stage="fidelity_contract",
            field="coverage",
            expected=",".join(SEMANTIC_CATEGORIES),
            actual=",".join(sorted(coverage_by_category)),
            correction="recompute category coverage",
        )
    for category in SEMANTIC_CATEGORIES:
        mappings = [
            mapping
            for mapping in contract["mappings"]
            if mapping["category"] == category
        ]
        expected = {
            "category": category,
            "total": len(mappings),
            **{
                classification: sum(
                    mapping["classification"] == classification
                    for mapping in mappings
                )
                for classification in CLASSIFICATIONS
            },
        }
        if coverage_by_category[category] != expected:
            raise SourceAdaptError(
                "xcp.adapt.fidelity_counts_invalid",
                "Fidelity coverage counts do not match exact mappings.",
                stage="fidelity_contract",
                field=f"coverage.{category}",
                expected=json.dumps(expected, sort_keys=True),
                actual=json.dumps(
                    coverage_by_category[category],
                    sort_keys=True,
                ),
                correction="recompute fidelity coverage",
            )


def readiness_report(
    inventory: dict[str, Any],
    semantic_inventory: dict[str, Any],
    ir: dict[str, Any],
    plan: dict[str, Any],
    fidelity: dict[str, Any],
    *,
    host_profile: dict[str, Any],
    project: dict[str, Any] | None = None,
    source_map: dict[str, Any] | None = None,
) -> dict[str, Any]:
    del host_profile
    _validate_semantic_inventory_integrity(semantic_inventory, inventory)
    _validate_fidelity_contract_integrity(
        fidelity,
        semantic_inventory,
        ir,
        plan,
        project=project,
    )
    dimension_categories = {
        "gameplay": {"scene_graph", "collision_physics"},
        "input": {"input"},
        "ui": {"ui"},
        "assets": {"asset", "resource"},
        "state": {"state_persistence"},
        "audio": {"audio"},
        "rendering_2d": {"rendering", "animation"},
        "behavior": {"behavior"},
        "lifecycle": {"lifecycle", "project_configuration"},
    }
    fidelity_dimensions: list[dict[str, Any]] = []
    differences: list[str] = []
    residual_work: list[str] = []
    for dimension, categories in dimension_categories.items():
        mappings = [
            mapping
            for mapping in fidelity["mappings"]
            if mapping["category"] in categories
        ]
        coverage = {
            "total": len(mappings),
            **{
                classification: sum(
                    mapping["classification"] == classification
                    for mapping in mappings
                )
                for classification in CLASSIFICATIONS
            },
        }
        classifications = {
            str(mapping["classification"]) for mapping in mappings
        }
        status = (
            "not_applicable"
            if not mappings
            else "blocked"
            if classifications & {"unsupported", "human_intervention_required"}
            else "degraded"
            if classifications & {"substituted", "degraded"}
            else "exact"
            if classifications == {"preserved_directly"}
            else "verified"
        )
        item_differences = sorted(
            {
                str(mapping["rationale"])
                for mapping in mappings
                if mapping["classification"]
                not in {"preserved_directly", "translated"}
            }
        )
        item_residual = sorted(
            {
                str(work)
                for mapping in mappings
                for work in mapping["residual_work"]
            }
        )
        differences.extend(item_differences)
        residual_work.extend(item_residual)
        fidelity_dimensions.append(
            {
                "dimension": dimension,
                "status": status,
                "coverage": coverage,
                "differences": item_differences,
                "residual_work": item_residual,
            }
        )
    not_run = {"outcome": "not_run", "evidence_refs": []}
    generated = project is not None and source_map is not None
    blocking_count = sum(
        mapping["classification"]
        not in {"preserved_directly", "translated"}
        for mapping in fidelity["mappings"]
    )
    report = {
        "schema_version": "xcp-creative-adaptation-readiness-v2",
        "report_id": f"{ir['project']['id']}.readiness",
        "identities": {
            "source_inventory_sha256": _document_sha(inventory),
            "semantic_inventory_sha256": _document_sha(semantic_inventory),
            "creative_ir_sha256": _document_sha(ir),
            "adaptation_plan_sha256": _document_sha(plan),
            "fidelity_contract_sha256": _document_sha(fidelity),
            "xcp_project_sha256": (
                sha256_bytes(canonical_json_bytes(project)) if project else ""
            ),
            "source_map_sha256": (
                _document_sha(source_map) if source_map else ""
            ),
            "host_profile_sha256": str(plan["host"]["profile_sha256"]),
        },
        "validation": {
            "inventory": "pass",
            "semantic_inventory": (
                "pass"
                if not semantic_inventory["scope"]["parse_failures"]
                else "fail"
            ),
            "creative_ir": "pass",
            "adaptation_plan": "pass",
            "fidelity_contract": "pass",
            "xcp_project": "pass" if generated else "not_run",
            "source_map": "pass" if generated else "not_run",
        },
        "adaptation_gate": {
            "target_level": "source_faithful",
            "achieved_level": fidelity["achieved_level"],
            "source_scope": "whole_project",
            "semantic_record_count": len(semantic_inventory["records"]),
            "blocking_record_count": blocking_count,
            "reference_oracle": "not_run",
            "human_playtest": "not_run",
        },
        "fidelity": fidelity_dimensions,
        "corrections": [],
        "c5_lifecycle": {
            "authority": "C5_AGENT_NATIVE_CREATION_GATE",
            "install": dict(not_run),
            "launch": dict(not_run),
            "observe": dict(not_run),
            "capture": dict(not_run),
            "update": dict(not_run),
            "rollback": dict(not_run),
            "cleanup": dict(not_run),
        },
        "differences": sorted(set(differences)),
        "residual_work": sorted(
            set(
                [
                    *residual_work,
                    "produce source-vs-XCP structural, behavioral, visual, audio and state oracle evidence",
                    "pass a human Xbox playtest recognizable as the source project",
                ]
            )
        ),
        "worker_gap": {
            "native_change_required": plan["worker_gap"][
                "native_change_required"
            ],
            "general_capability_missing": plan["worker_gap"][
                "general_capability_missing"
            ],
            "evidence_refs": (
                [f"plan:{plan['plan_id']}#worker_gap"]
                if plan["worker_gap"]["evidence"]
                else []
            ),
        },
        "decision": "not_ready",
    }
    _validate(report, "readiness")
    return report


def probe_fidelity(
    run_root: pathlib.Path,
    output_path: pathlib.Path,
    *,
    state_evidence_paths: tuple[pathlib.Path, ...] = (),
    human_playtest_outcome: str = "not_run",
    human_evidence_refs: tuple[str, ...] = (),
) -> dict[str, Any]:
    run_root = run_root.resolve()
    handoff = _load_json(run_root / "c6-to-c5-handoff.json")
    _validate(handoff, "handoff")
    roles = _verify_handoff_artifacts(run_root, handoff)
    inventory = _load_json(roles["source.inventory"])
    semantic_inventory = _load_json(roles["source.semantic_inventory"])
    ir = _load_json(roles["creative.ir"])
    plan = _load_json(roles["adaptation.plan"])
    fidelity = _load_json(roles["adaptation.fidelity_contract"])
    project = _load_json(roles["xcp.project"])
    source_map = _load_json(roles["source.map"])
    _validate(inventory, "inventory")
    _validate(semantic_inventory, "semantic_inventory")
    _validate(ir, "ir")
    _validate(plan, "plan")
    _validate(fidelity, "fidelity_contract")
    _validate(source_map, "source_map")
    _validate_semantic_inventory_integrity(semantic_inventory, inventory)
    _validate_fidelity_contract_integrity(
        fidelity,
        semantic_inventory,
        ir,
        plan,
        project=project,
    )
    if human_playtest_outcome not in {"pass", "fail", "not_run"}:
        raise SourceAdaptError(
            "xcp.adapt.human_playtest_outcome_invalid",
            "Human playtest outcome is outside the fidelity evidence contract.",
            stage="fidelity_probe",
            field="human_playtest_outcome",
            expected="pass, fail or not_run",
            actual=human_playtest_outcome,
            correction="record the operator result without inference",
        )
    if human_playtest_outcome == "not_run" and human_evidence_refs:
        raise SourceAdaptError(
            "xcp.adapt.human_playtest_evidence_invalid",
            "A not-run human playtest cannot claim evidence.",
            stage="fidelity_probe",
            field="human_evidence_refs",
            expected="empty",
            actual=str(len(human_evidence_refs)),
            correction="remove the refs or record the measured outcome",
        )
    if human_playtest_outcome != "not_run" and not human_evidence_refs:
        raise SourceAdaptError(
            "xcp.adapt.human_playtest_evidence_missing",
            "A measured human playtest requires at least one evidence reference.",
            stage="fidelity_probe",
            field="human_evidence_refs",
            expected="one or more exact evidence references",
            actual="empty",
            correction="bind the operator witness before finalization",
        )

    project_root = roles["xcp.project"].parent
    modules_by_id = {
        str(module["id"]): module for module in project["modules"]
    }
    world_modules = [
        module
        for module in project["modules"]
        if module["kind"] == "xcp.world2d.v2"
    ]
    campaign_modules = [
        module
        for module in project["modules"]
        if module["kind"] == "xcp.world2d.campaign.v1"
    ]
    campaign_scenes = _campaign_scene_records(ir)
    expected_scene_modules: list[tuple[dict[str, Any], str]] = []
    exact_world_documents = 0
    entry_world: dict[str, Any] | None = None
    campaign_router_exact = False
    if campaign_modules:
        for scene in campaign_scenes:
            attributes = scene["attributes"]
            module_id = _portable_id(
                (
                    f"world.{attributes['level_pack']}."
                    f"{attributes['level_ordinal']}"
                ),
                maximum=96,
            )
            expected_scene_modules.append((scene, module_id))
            module = modules_by_id.get(module_id)
            if module is None or module["kind"] != "xcp.world2d.v2":
                continue
            actual_world = _load_json(
                project_root / str(module["path"])
            )
            expected_world = _world2d_module(
                ir,
                authored_v2=True,
                scene=scene,
            )
            if canonical_json_bytes(actual_world) == canonical_json_bytes(
                expected_world
            ):
                exact_world_documents += 1
            if entry_world is None:
                entry_world = actual_world
    if campaign_modules and len(expected_scene_modules) >= 2:
        campaign_module = campaign_modules[0]
        actual_campaign = _load_json(
            project_root / str(campaign_module["path"])
        )
        expected_campaign = _world2d_campaign_module(
            ir,
            expected_scene_modules,
        )
        campaign_router_exact = canonical_json_bytes(
            actual_campaign
        ) == canonical_json_bytes(expected_campaign)
    elif not campaign_modules:
        entry = modules_by_id.get(str(project["entry_module"]))
        if entry is not None and entry["kind"] in {
            "xcp.world2d.v1",
            "xcp.world2d.v2",
        }:
            entry_world = _load_json(project_root / str(entry["path"]))
            expected_world = _world2d_module(
                ir,
                authored_v2=entry["kind"] == "xcp.world2d.v2",
            )
            exact_world_documents = int(
                canonical_json_bytes(entry_world)
                == canonical_json_bytes(expected_world)
            )
            expected_scene_modules = [
                (_selected_world_scene(ir), str(entry["id"]))
            ]
            campaign_router_exact = True

    inventory_by_id = {
        str(item["file_id"]): item for item in inventory["files"]
    }
    project_assets = {
        str(asset["id"]): asset for asset in project["assets"]
    }
    plan_by_ir = {
        str(reference): item
        for item in plan["items"]
        for reference in item["ir_refs"]
    }
    exact_asset_copies = 0
    expected_asset_copies = 0
    for asset in ir["assets"]:
        item = plan_by_ir[str(asset["id"])]
        if item["classification"] != "preserved_directly":
            continue
        expected_asset_copies += 1
        asset_id = _portable_id(str(asset["id"]), maximum=96)
        project_asset = project_assets.get(asset_id)
        if project_asset is None:
            continue
        source_item = inventory_by_id[str(asset["source_refs"][0])]
        asset_path = project_root / str(project_asset["path"])
        if (
            project_asset["media_type"] == asset["media_type"]
            and sha256_file(asset_path) == source_item["sha256"]
            and source_item["sha256"] == asset["source_sha256"]
        ):
            exact_asset_copies += 1

    dimensions = ("structural", "behavioral", "visual", "audio", "state")
    mapping_stats: dict[str, tuple[int, int]] = {}
    for dimension in dimensions:
        mappings = [
            mapping
            for mapping in fidelity["mappings"]
            if mapping["verification"] == dimension
        ]
        equivalent = sum(
            mapping["classification"]
            in {"preserved_directly", "translated"}
            for mapping in mappings
        )
        mapping_stats[dimension] = (equivalent, len(mappings))

    expected_world_count = len(expected_scene_modules)
    world_projection_exact = (
        expected_world_count > 0
        and exact_world_documents == expected_world_count
        and campaign_router_exact
        and (
            len(world_modules) == expected_world_count
            if campaign_scenes
            else True
        )
    )
    entry_solution = (
        _world2d_solution(entry_world)
        if entry_world is not None
        else []
    )
    audio_required = (
        semantic_inventory["summary"]["category_counts"]["audio"] > 0
    )
    audio_composed = any(
        module["kind"] == "xcp.audio.v1"
        for module in project["modules"]
    )
    data_modules = [
        module
        for module in project["modules"]
        if module["kind"] == "xcp.data.v1"
    ]
    state_projection_exact = False
    if data_modules:
        data = _load_json(project_root / str(data_modules[0]["path"]))
        expected_state_ids = {
            _portable_id(str(record["id"]), maximum=96)
            for record in ir["state"]
        }
        actual_state_ids = {
            str(record["id"])
            for record in data["local_state"]
        }
        state_projection_exact = expected_state_ids == actual_state_ids

    runtime_state_refs: list[str] = []
    runtime_state_exact = not campaign_modules
    for evidence_path in state_evidence_paths:
        resolved = evidence_path.resolve()
        document = _load_json(resolved)
        digest = sha256_file(resolved)
        runtime_state_refs.append(
            f"runtime-evidence:{digest}#phase={document.get('phase', 'unknown')}"
        )
        checks = document.get("checks", {})
        if (
            document.get("decision") == "pass"
            and document.get("gameplay_input_sent_before_observation") is False
            and isinstance(checks, dict)
            and checks
            and all(value is True for value in checks.values())
        ):
            runtime_state_exact = True

    fidelity_sha = sha256_file(roles["adaptation.fidelity_contract"])
    ir_sha = sha256_file(roles["creative.ir"])
    project_sha = sha256_file(roles["xcp.project"])
    dimension_results: dict[str, tuple[str, list[str], str]] = {}
    structural_equivalent, structural_total = mapping_stats["structural"]
    dimension_results["structural"] = (
        (
            "pass"
            if world_projection_exact
            and structural_equivalent == structural_total
            else "fail"
        ),
        [
            (
                f"creative-ir:{ir_sha}#world_documents="
                f"{exact_world_documents}/{expected_world_count};"
                f"campaign_router={str(campaign_router_exact).lower()}"
            ),
            (
                f"fidelity-contract:{fidelity_sha}#verification=structural;"
                f"equivalent={structural_equivalent}/{structural_total}"
            ),
        ],
        "source-derived-ir-vs-scene-specific-xcp-structure-v1",
    )
    behavioral_equivalent, behavioral_total = mapping_stats["behavioral"]
    dimension_results["behavioral"] = (
        (
            "pass"
            if world_projection_exact
            and bool(entry_solution)
            and behavioral_equivalent == behavioral_total
            else "fail"
        ),
        [
            (
                f"creative-ir:{ir_sha}#entry_solution_steps="
                f"{sum(int(step['repeat']) for step in entry_solution)}"
            ),
            (
                f"fidelity-contract:{fidelity_sha}#verification=behavioral;"
                f"equivalent={behavioral_equivalent}/{behavioral_total}"
            ),
        ],
        "source-derived-world-state-and-solution-differential-v1",
    )
    visual_equivalent, visual_total = mapping_stats["visual"]
    dimension_results["visual"] = (
        (
            "pass"
            if world_projection_exact
            and exact_asset_copies == expected_asset_copies
            and visual_equivalent == visual_total
            else "fail"
        ),
        [
            (
                f"xcp-project:{project_sha}#exact_source_asset_copies="
                f"{exact_asset_copies}/{expected_asset_copies}"
            ),
            (
                f"fidelity-contract:{fidelity_sha}#verification=visual;"
                f"equivalent={visual_equivalent}/{visual_total}"
            ),
        ],
        "source-asset-hash-and-authored-render-binding-v1",
    )
    audio_equivalent, audio_total = mapping_stats["audio"]
    dimension_results["audio"] = (
        (
            "not_applicable"
            if not audio_required
            else "pass"
            if audio_composed and audio_equivalent == audio_total
            else "fail"
        ),
        [
            (
                f"xcp-project:{project_sha}#audio_module="
                f"{str(audio_composed).lower()}"
            ),
            (
                f"fidelity-contract:{fidelity_sha}#verification=audio;"
                f"equivalent={audio_equivalent}/{audio_total}"
            ),
        ],
        "source-audio-semantics-vs-composed-audio-module-v1",
    )
    state_equivalent, state_total = mapping_stats["state"]
    dimension_results["state"] = (
        (
            "pass"
            if state_projection_exact
            and runtime_state_exact
            and state_equivalent == state_total
            else "fail"
        ),
        [
            (
                f"xcp-project:{project_sha}#static_state_projection="
                f"{str(state_projection_exact).lower()};"
                f"runtime_restore={str(runtime_state_exact).lower()}"
            ),
            (
                f"fidelity-contract:{fidelity_sha}#verification=state;"
                f"equivalent={state_equivalent}/{state_total}"
            ),
            *runtime_state_refs,
        ],
        "source-state-mapping-plus-fresh-session-restore-v1",
    )

    oracles = [
        {
            "dimension": dimension,
            "required": dimension != "audio" or audio_required,
            "outcome": dimension_results[dimension][0],
            "evidence_refs": dimension_results[dimension][1],
            "comparison_contract": dimension_results[dimension][2],
        }
        for dimension in dimensions
    ]
    oracle_pass = all(
        oracle["outcome"] in {"pass", "not_applicable"}
        and (not oracle["required"] or oracle["outcome"] == "pass")
        for oracle in oracles
    )
    evidence = {
        "schema_version": "xcp-creative-fidelity-evidence-v1",
        "evidence_id": f"{project['project_id']}.fidelity_evidence",
        "fidelity_contract_sha256": fidelity_sha,
        "xcp_project_sha256": project_sha,
        "oracles": oracles,
        "human_playtest": {
            "outcome": human_playtest_outcome,
            "evidence_refs": list(human_evidence_refs),
            "acceptance": "recognizable_and_playable_as_source_project",
        },
        "decision": (
            "pass"
            if oracle_pass and human_playtest_outcome == "pass"
            else "fail"
        ),
    }
    _validate(evidence, "fidelity_evidence")
    _write_new(output_path.resolve(), evidence)
    return {
        "ok": True,
        "schema_version": "xcp-source-fidelity-probe-v1",
        "decision": evidence["decision"],
        "output": str(output_path.resolve()),
        "sha256": sha256_file(output_path.resolve()),
        "oracle_outcomes": {
            oracle["dimension"]: oracle["outcome"]
            for oracle in oracles
        },
        "human_playtest": human_playtest_outcome,
        "world_documents": {
            "exact": exact_world_documents,
            "expected": expected_world_count,
            "campaign_router_exact": campaign_router_exact,
        },
        "source_asset_copies": {
            "exact": exact_asset_copies,
            "expected": expected_asset_copies,
        },
        "mapping_equivalence": {
            dimension: {
                "equivalent": mapping_stats[dimension][0],
                "total": mapping_stats[dimension][1],
            }
            for dimension in dimensions
        },
    }


def finalize_readiness(
    run_root: pathlib.Path,
    receipt_path: pathlib.Path,
    ledger_path: pathlib.Path,
    output_path: pathlib.Path,
    fidelity_evidence_path: pathlib.Path | None = None,
) -> dict[str, Any]:
    run_root = run_root.resolve()
    handoff = _load_json(run_root / "c6-to-c5-handoff.json")
    _validate(handoff, "handoff")
    roles = _verify_handoff_artifacts(run_root, handoff)
    inventory = _load_json(roles["source.inventory"])
    _validate(inventory, "inventory")
    semantic_inventory = _load_json(roles["source.semantic_inventory"])
    _validate(semantic_inventory, "semantic_inventory")
    ir = _load_json(roles["creative.ir"])
    _validate(ir, "ir")
    plan = _load_json(roles["adaptation.plan"])
    _validate(plan, "plan")
    fidelity = _load_json(roles["adaptation.fidelity_contract"])
    _validate(fidelity, "fidelity_contract")
    project = _load_json(roles["xcp.project"])
    _validate_semantic_inventory_integrity(semantic_inventory, inventory)
    _validate_fidelity_contract_integrity(
        fidelity,
        semantic_inventory,
        ir,
        plan,
        project=project,
    )
    preliminary = _load_json(roles["adaptation.readiness.preliminary"])
    _validate(preliminary, "readiness")
    intent = _load_json(roles["c5.intent"])
    _validate_schema(intent, INTENT_SCHEMA, stage="c5_intent")
    receipt = _load_json(receipt_path.resolve())
    _validate_schema(receipt, RECEIPT_SCHEMA, stage="c5_receipt")
    ledger = _load_json(ledger_path.resolve())
    _validate_correction_ledger(ledger)
    fidelity_evidence: dict[str, Any] | None = None
    fidelity_evidence_sha = ""
    if fidelity_evidence_path is not None:
        fidelity_evidence_path = fidelity_evidence_path.resolve()
        fidelity_evidence = _load_json(fidelity_evidence_path)
        _validate(fidelity_evidence, "fidelity_evidence")
        fidelity_evidence_sha = sha256_file(fidelity_evidence_path)

    ledger_sha = sha256_file(ledger_path.resolve())
    expected_bindings = {
        "handoff.project_id": (
            handoff["project_id"],
            receipt["project_id"],
        ),
        "intent_sha256": (
            sha256_file(roles["c5.intent"]),
            receipt["intent_sha256"],
        ),
        "project_sha256": (
            sha256_file(roles["xcp.project"]),
            receipt["project_sha256"],
        ),
        "host_profile_sha256": (
            preliminary["identities"]["host_profile_sha256"],
            receipt["host_profile_sha256"],
        ),
        "correction_ledger_sha256": (
            ledger_sha,
            receipt["correction_ledger_sha256"],
        ),
        "ledger.intent_sha256": (
            sha256_file(roles["c5.intent"]),
            ledger["intent_sha256"],
        ),
        "ledger.project_id": (
            handoff["project_id"],
            ledger["project_id"],
        ),
        "semantic_inventory_sha256": (
            sha256_file(roles["source.semantic_inventory"]),
            preliminary["identities"]["semantic_inventory_sha256"],
        ),
        "fidelity_contract_sha256": (
            sha256_file(roles["adaptation.fidelity_contract"]),
            preliminary["identities"]["fidelity_contract_sha256"],
        ),
    }
    if fidelity_evidence is not None:
        expected_bindings.update(
            {
                "fidelity_evidence.contract_sha256": (
                    sha256_file(roles["adaptation.fidelity_contract"]),
                    fidelity_evidence["fidelity_contract_sha256"],
                ),
                "fidelity_evidence.project_sha256": (
                    sha256_file(roles["xcp.project"]),
                    fidelity_evidence["xcp_project_sha256"],
                ),
            }
        )
    for field, (expected, actual) in expected_bindings.items():
        if expected != actual:
            raise SourceAdaptError(
                "xcp.adapt.finalization_identity_mismatch",
                "C6 finalization inputs do not bind the same exact lifecycle.",
                stage="finalize",
                field=field,
                expected=str(expected),
                actual=str(actual),
                correction="select the exact receipt and ledger from this handoff",
            )

    operations = {
        str(operation["name"]): operation
        for operation in receipt["operations"]
    }
    lifecycle_names = (
        "install",
        "launch",
        "observe",
        "capture",
        "update",
        "rollback",
        "cleanup",
    )
    receipt_ref = f"receipt:{sha256_file(receipt_path.resolve())}"
    c5_lifecycle = {
        "authority": "C5_AGENT_NATIVE_CREATION_GATE",
        **{
            name: {
                "outcome": (
                    "pass"
                    if operations.get(name, {}).get("outcome") == "pass"
                    else "fail"
                ),
                "evidence_refs": [f"{receipt_ref}#operation={name}"],
            }
            for name in lifecycle_names
        },
    }
    corrections = [
        {
            "stage": entry["stage"],
            "error": entry["error"],
            "change": entry.get(
                "change_summary",
                (
                    f"Applied {len(entry['changes'])} exact file change(s)."
                    if entry["changes"]
                    else "Corrected the structured request."
                ),
            ),
            "retry_outcome": entry["retry"]["outcome"],
            "evidence_refs": entry["evidence_refs"],
        }
        for entry in ledger["entries"]
        if entry["stage"] in {"conversion", "live"}
    ]
    correction_stages = {
        item["stage"]
        for item in corrections
        if item["retry_outcome"] == "pass"
    }
    blockers_pass = all(
        result["outcome"] == "pass"
        for result in receipt["acceptance"]
        if result["severity"] == "blocker"
    )
    lifecycle_pass = all(
        c5_lifecycle[name]["outcome"] == "pass"
        for name in lifecycle_names
    )
    non_equivalent_count = sum(
        mapping["classification"]
        not in {"preserved_directly", "translated"}
        for mapping in fidelity["mappings"]
    )
    parse_complete = not semantic_inventory["scope"]["parse_failures"]
    oracle_outcomes = (
        {
            str(item["dimension"]): str(item["outcome"])
            for item in fidelity_evidence["oracles"]
        }
        if fidelity_evidence is not None
        else {}
    )
    oracle_pass = (
        fidelity_evidence is not None
        and all(
            item["outcome"] in {"pass", "not_applicable"}
            and (not item["required"] or item["outcome"] == "pass")
            for item in fidelity_evidence["oracles"]
        )
    )
    human_playtest_pass = (
        fidelity_evidence is not None
        and fidelity_evidence["human_playtest"]["outcome"] == "pass"
    )
    fidelity_evidence_pass = (
        fidelity_evidence is not None
        and fidelity_evidence["decision"] == "pass"
        and oracle_pass
        and human_playtest_pass
    )
    ready = (
        receipt["decision"] == "pass"
        and ledger["status"] == "passed"
        and blockers_pass
        and lifecycle_pass
        and correction_stages == {"conversion", "live"}
        and not preliminary["worker_gap"]["general_capability_missing"]
        and parse_complete
        and fidelity["achieved_level"] == "source_faithful"
        and non_equivalent_count == 0
        and fidelity_evidence_pass
    )
    final = dict(preliminary)
    final["identities"] = dict(preliminary["identities"])
    if fidelity_evidence is not None:
        final["identities"]["fidelity_evidence_sha256"] = (
            fidelity_evidence_sha
        )
    final["corrections"] = corrections
    final["c5_lifecycle"] = c5_lifecycle
    final["adaptation_gate"] = dict(preliminary["adaptation_gate"])
    final["adaptation_gate"]["achieved_level"] = fidelity["achieved_level"]
    final["adaptation_gate"]["blocking_record_count"] = (
        non_equivalent_count
    )
    final["adaptation_gate"]["reference_oracle"] = (
        "pass"
        if oracle_pass
        else "fail"
        if fidelity_evidence is not None
        else "not_run"
    )
    final["adaptation_gate"]["human_playtest"] = (
        str(fidelity_evidence["human_playtest"]["outcome"])
        if fidelity_evidence is not None
        else "not_run"
    )
    final["decision"] = "ready_source_faithful" if ready else "not_ready"
    if not ready:
        missing = sorted({"conversion", "live"} - correction_stages)
        final["residual_work"] = sorted(
            set(
                [
                    *final["residual_work"],
                    *[
                        f"prove a passing {stage} correction"
                        for stage in missing
                    ],
                    *(
                        ["complete the exact C5 lifecycle"]
                        if not lifecycle_pass
                        else []
                    ),
                    *(
                        ["satisfy all blocking intent assertions"]
                        if not blockers_pass
                        else []
                    ),
                    *(
                        ["complete the whole-project semantic parse"]
                        if not parse_complete
                        else []
                    ),
                    *(
                        [
                            f"resolve {non_equivalent_count} non-equivalent semantic mappings"
                        ]
                        if non_equivalent_count
                        else []
                    ),
                    *(
                        ["produce passing source-reference oracle evidence"]
                        if not oracle_pass
                        else []
                    ),
                    *(
                        ["pass the human Xbox source-recognition playtest"]
                        if not human_playtest_pass
                        else []
                    ),
                ]
            )
        )
    _validate(final, "readiness")
    _write_new(output_path, final)
    return {
        "ok": ready,
        "schema_version": "xcp-creative-adaptation-finalization-v2",
        "decision": final["decision"],
        "output": str(output_path.resolve()),
        "sha256": sha256_file(output_path.resolve()),
        "receipt_sha256": sha256_file(receipt_path.resolve()),
        "correction_ledger_sha256": ledger_sha,
        "correction_stages": sorted(correction_stages),
        "achieved_level": fidelity["achieved_level"],
        "non_equivalent_semantic_records": non_equivalent_count,
        "oracle_outcomes": oracle_outcomes,
        "human_playtest": (
            fidelity_evidence["human_playtest"]["outcome"]
            if fidelity_evidence is not None
            else "not_run"
        ),
    }
