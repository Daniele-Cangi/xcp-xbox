"""Whole-project semantic inventory invariants."""

from __future__ import annotations

import json
from typing import Any

from ..core import SourceAdaptError, _document_sha

SEMANTIC_CATEGORIES = (
    "project_configuration",
    "scene_graph",
    "resource",
    "rendering",
    "animation",
    "collision_physics",
    "behavior",
    "input",
    "ui",
    "audio",
    "state_persistence",
    "lifecycle",
    "asset",
)


def _validate_semantic_inventory_integrity(
    semantic_inventory: dict[str, Any],
    inventory: dict[str, Any],
) -> None:
    expected_inventory_sha = _document_sha(inventory)
    if semantic_inventory["source_inventory_sha256"] != expected_inventory_sha:
        raise SourceAdaptError(
            "xcp.adapt.semantic_inventory_source_mismatch",
            "Semantic inventory does not bind the exact source inventory.",
            stage="semantic_inventory",
            field="source_inventory_sha256",
            expected=expected_inventory_sha,
            actual=str(semantic_inventory["source_inventory_sha256"]),
            correction="re-extract semantics from the exact source inventory",
        )
    source_refs = {str(item["file_id"]) for item in inventory["files"]}
    included = set(semantic_inventory["scope"]["included_source_refs"])
    excluded_list = [
        str(item["source_ref"])
        for item in semantic_inventory["scope"]["excluded_sources"]
    ]
    excluded = set(excluded_list)
    if len(excluded_list) != len(excluded):
        raise SourceAdaptError(
            "xcp.adapt.semantic_inventory_exclusion_duplicate",
            "A source file is excluded more than once.",
            stage="semantic_inventory",
            field="scope.excluded_sources",
            expected="one accounting disposition per source file",
            actual=str(len(excluded_list) - len(excluded)),
            correction="deduplicate source accounting",
        )
    if included & excluded or included | excluded != source_refs:
        raise SourceAdaptError(
            "xcp.adapt.semantic_inventory_scope_invalid",
            "Whole-project semantic scope must account for every source file exactly once.",
            stage="semantic_inventory",
            field="scope",
            expected=f"{len(source_refs)} source file dispositions",
            actual=f"{len(included | excluded)} accounted",
            correction="include or explicitly exclude every inventoried source file",
        )
    failure_refs = {
        str(item["source_ref"])
        for item in semantic_inventory["scope"]["parse_failures"]
    }
    if not failure_refs <= included:
        raise SourceAdaptError(
            "xcp.adapt.semantic_inventory_failure_ref_invalid",
            "Parse failures must refer to included semantic source files.",
            stage="semantic_inventory",
            field="scope.parse_failures",
            expected="included source refs",
            actual=",".join(sorted(failure_refs - included)),
            correction="repair the semantic parser accounting",
        )
    record_ids: set[str] = set()
    for record in semantic_inventory["records"]:
        record_id = str(record["semantic_id"])
        if record_id in record_ids:
            raise SourceAdaptError(
                "xcp.adapt.semantic_id_duplicate",
                "Semantic inventory record ids must be globally unique.",
                stage="semantic_inventory",
                field="records.semantic_id",
                expected="unique id",
                actual=record_id,
                correction="repair deterministic semantic id generation",
            )
        record_ids.add(record_id)
        unknown = set(record["source_refs"]) - included
        if unknown:
            raise SourceAdaptError(
                "xcp.adapt.semantic_source_ref_unknown",
                "A semantic record references a source outside parsed scope.",
                stage="semantic_inventory",
                field=f"records.{record_id}.source_refs",
                expected="included source refs",
                actual=",".join(sorted(unknown)),
                correction="repair semantic source provenance",
            )
    summary = semantic_inventory["summary"]
    actual_category_counts = {
        category: sum(
            record["category"] == category
            for record in semantic_inventory["records"]
        )
        for category in SEMANTIC_CATEGORIES
    }
    expected_summary = {
        "source_file_count": len(source_refs),
        "included_source_count": len(included),
        "excluded_source_count": len(excluded),
        "parse_failure_count": len(
            semantic_inventory["scope"]["parse_failures"]
        ),
        "record_count": len(semantic_inventory["records"]),
        "category_counts": actual_category_counts,
    }
    if summary != expected_summary:
        raise SourceAdaptError(
            "xcp.adapt.semantic_inventory_summary_invalid",
            "Semantic inventory summary does not match exact records and scope.",
            stage="semantic_inventory",
            field="summary",
            expected=json.dumps(expected_summary, sort_keys=True),
            actual=json.dumps(summary, sort_keys=True),
            correction="recompute summary from canonical records",
        )
