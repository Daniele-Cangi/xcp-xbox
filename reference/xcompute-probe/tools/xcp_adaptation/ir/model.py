"""Canonical Creative IR traversal and integrity rules."""

from __future__ import annotations

from typing import Any, Iterable

from ..core import SourceAdaptError, _document_sha


def _ir_records(ir: dict[str, Any]) -> Iterable[tuple[str, dict[str, Any]]]:
    for collection in (
        "scenes",
        "entities",
        "behaviors",
        "input_actions",
        "ui",
        "assets",
        "state",
    ):
        for record in ir[collection]:
            yield collection, record


def _validate_ir_integrity(
    ir: dict[str, Any],
    inventory: dict[str, Any],
) -> None:
    if ir["source_inventory_sha256"] != _document_sha(inventory):
        raise SourceAdaptError(
            "xcp.adapt.ir_inventory_mismatch",
            "Creative IR does not bind the exact inventory bytes.",
            stage="ir",
            field="source_inventory_sha256",
            expected=_document_sha(inventory),
            actual=str(ir["source_inventory_sha256"]),
            correction="re-extract from the exact inventory",
        )
    source_ids = {str(item["file_id"]) for item in inventory["files"]}
    semantic_ids: set[str] = set()
    for collection, record in _ir_records(ir):
        record_id = str(record["id"])
        if record_id in semantic_ids:
            raise SourceAdaptError(
                "xcp.adapt.ir_id_duplicate",
                "Creative IR semantic ids must be globally unique.",
                stage="ir",
                field=f"{collection}.{record_id}",
                expected="unique semantic id",
                actual=record_id,
                correction="make adapter ids stable and globally unique",
            )
        semantic_ids.add(record_id)
        unknown = sorted(set(record["source_refs"]) - source_ids)
        if unknown:
            raise SourceAdaptError(
                "xcp.adapt.ir_source_ref_unknown",
                "Creative IR references a file outside the exact inventory.",
                stage="ir",
                field=f"{collection}.{record_id}.source_refs",
                expected="inventory file ids",
                actual=",".join(unknown),
                correction="repair the adapter source mapping",
            )
    if ir["project"]["initial_scene"] not in {
        item["id"] for item in ir["scenes"]
    }:
        raise SourceAdaptError(
            "xcp.adapt.ir_initial_scene_unknown",
            "Creative IR initial_scene is not a declared scene.",
            stage="ir",
            field="project.initial_scene",
            expected="declared scene id",
            actual=str(ir["project"]["initial_scene"]),
            correction="repair entrypoint extraction",
        )
    for behavior in ir["behaviors"]:
        unknown = sorted(set(behavior["owner_refs"]) - semantic_ids)
        if unknown:
            raise SourceAdaptError(
                "xcp.adapt.ir_owner_ref_unknown",
                "A behavior owner does not name a semantic IR element.",
                stage="ir",
                field=f"behaviors.{behavior['id']}.owner_refs",
                expected="declared semantic ids",
                actual=",".join(unknown),
                correction="repair adapter ownership extraction",
            )


def _campaign_scene_records(ir: dict[str, Any]) -> list[dict[str, Any]]:
    scenes = [
        scene
        for scene in ir["scenes"]
        if scene["attributes"].get("grid_tiles")
        and str(scene["attributes"].get("source_path", "")).startswith(
            "levels/"
        )
        and "level_ordinal" in scene["attributes"]
    ]
    return sorted(
        scenes,
        key=lambda scene: (
            int(scene["attributes"].get("campaign_order", 1000000)),
            str(scene["attributes"]["source_path"]),
        ),
    )
