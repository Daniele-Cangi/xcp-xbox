"""Canonical, engine-neutral Godot Source Model v1.

The model is intentionally upstream of Creative IR.  It records what the
Godot frontend observed and keeps an exact, hash-bound reference to every
source file, but it contains no XCP module or Xbox-specific decision.
"""

from __future__ import annotations

import copy
import hashlib
import pathlib
from dataclasses import dataclass
from typing import Any, Mapping, Sequence

from xcp_creative_project import canonical_json_bytes


SCHEMA_VERSION = "godot-source-model-v1"
MODEL_SECTIONS = (
    "scene_tree",
    "node_types_and_properties",
    "internal_resources",
    "external_resources",
    "scripts_and_symbol_structure",
    "signals_and_connections",
    "input_map",
    "autoloads",
    "animations",
    "collision_bodies_shapes_layers_and_masks",
    "ui",
    "audio",
    "saved_state_candidates",
    "project_configuration",
    "exact_source_references",
)
FRONTEND_IDS = (
    "godot2.frontend",
    "godot3.frontend",
    "godot4.frontend",
)
EXTRACTION_MODES = (
    "engine_assisted",
    "file_parser",
    "file_parser_fallback",
)


class GodotSourceModelError(ValueError):
    """Fail-closed internal contract violation."""


def _records(
    semantic_inventory: Mapping[str, Any],
    *,
    categories: Sequence[str] = (),
    kinds: Sequence[str] = (),
    kind_prefixes: Sequence[str] = (),
) -> list[dict[str, Any]]:
    category_set = set(categories)
    kind_set = set(kinds)
    selected: list[dict[str, Any]] = []
    for raw in semantic_inventory.get("records", []):
        if not isinstance(raw, Mapping):
            raise GodotSourceModelError(
                "semantic inventory records must be objects"
            )
        category = str(raw.get("category", ""))
        kind = str(raw.get("kind", ""))
        if (
            (category_set and category in category_set)
            or (kind_set and kind in kind_set)
            or any(kind.startswith(prefix) for prefix in kind_prefixes)
        ):
            selected.append(copy.deepcopy(dict(raw)))
    selected.sort(key=lambda item: str(item.get("semantic_id", "")))
    return selected


def _exact_source_references(
    inventory: Mapping[str, Any],
) -> list[dict[str, Any]]:
    references: list[dict[str, Any]] = []
    for raw in inventory.get("files", []):
        if not isinstance(raw, Mapping):
            raise GodotSourceModelError("source inventory files must be objects")
        references.append(
            {
                "source_ref": str(raw.get("file_id", "")),
                "path": str(raw.get("path", "")),
                "bytes": int(raw.get("bytes", -1)),
                "sha256": str(raw.get("sha256", "")),
                "media_type": str(raw.get("media_type", "")),
                "role": str(raw.get("role", "")),
            }
        )
    references.sort(key=lambda item: (item["path"], item["source_ref"]))
    return references


def _diagnostics(
    semantic_inventory: Mapping[str, Any],
) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    scope = semantic_inventory.get("scope", {})
    if not isinstance(scope, Mapping):
        raise GodotSourceModelError("semantic inventory scope must be an object")
    for field in ("excluded_sources", "parse_failures"):
        for raw in scope.get(field, []):
            if not isinstance(raw, Mapping):
                raise GodotSourceModelError(
                    f"semantic inventory {field} entries must be objects"
                )
            entry = copy.deepcopy(dict(raw))
            entry["source_collection"] = field
            diagnostics.append(entry)
    diagnostics.sort(
        key=lambda item: (
            str(item.get("source_collection", "")),
            str(item.get("path", item.get("source_ref", ""))),
            str(item.get("code", item.get("reason", ""))),
        )
    )
    return diagnostics


def build_godot_source_model_document(
    inventory: Mapping[str, Any],
    semantic_inventory: Mapping[str, Any],
    *,
    frontend_id: str,
    frontend_version: str,
    extraction_mode: str,
    engine_version: str = "",
    fallback_reason: str = "",
) -> dict[str, Any]:
    """Project existing exact extraction into Godot Source Model v1."""

    if frontend_id not in FRONTEND_IDS:
        raise GodotSourceModelError(f"unsupported frontend_id: {frontend_id}")
    if extraction_mode not in EXTRACTION_MODES:
        raise GodotSourceModelError(
            f"unsupported extraction_mode: {extraction_mode}"
        )
    inventory_id = str(inventory.get("inventory_id", ""))
    semantic_inventory_id = str(
        semantic_inventory.get("semantic_inventory_id", "")
    )
    semantic_source_sha256 = str(
        semantic_inventory.get("source_inventory_sha256", "")
    )
    inventory_sha256 = hashlib.sha256(
        canonical_json_bytes(dict(inventory))
    ).hexdigest()
    if (
        not inventory_id
        or semantic_source_sha256 != inventory_sha256
    ):
        raise GodotSourceModelError(
            "semantic inventory is not bound to the exact source inventory"
        )

    source = inventory.get("source", {})
    if not isinstance(source, Mapping):
        raise GodotSourceModelError("source inventory source must be an object")
    root_sha256 = str(inventory.get("root_sha256", ""))
    if len(root_sha256) != 64:
        raise GodotSourceModelError("source inventory root_sha256 is invalid")

    document: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "model_id": f"{inventory_id}.godot-source-model",
        "frontend": {
            "frontend_id": frontend_id,
            "frontend_version": frontend_version,
            "engine_family": "Godot",
            "engine_version": engine_version,
            "extraction_mode": extraction_mode,
            "fallback_reason": fallback_reason,
        },
        "source_identity": {
            "source_inventory_id": inventory_id,
            "semantic_inventory_id": semantic_inventory_id,
            "root_sha256": root_sha256,
            "ecosystem": str(source.get("ecosystem", "")),
            "ecosystem_version": str(source.get("ecosystem_version", "")),
            "revision": str(source.get("revision", "")),
            "entrypoints": sorted(
                str(item) for item in source.get("entrypoints", [])
            ),
        },
        "scene_tree": _records(
            semantic_inventory,
            categories=("scene_graph",),
        ),
        "node_types_and_properties": _records(
            semantic_inventory,
            categories=(
                "scene_graph",
                "rendering",
                "collision_physics",
                "ui",
                "audio",
                "animation",
            ),
        ),
        "internal_resources": _records(
            semantic_inventory,
            kinds=(
                "godot.resource.subresource",
                "godot.resource.section",
            ),
            kind_prefixes=("godot.animation.subresource",),
        ),
        "external_resources": _records(
            semantic_inventory,
            kinds=("godot.resource.external",),
        ),
        "scripts_and_symbol_structure": _records(
            semantic_inventory,
            kinds=(
                "godot.resource.script",
                "godot.behavior.function",
                "godot.behavior.member_state",
            ),
        ),
        "signals_and_connections": _records(
            semantic_inventory,
            kinds=(
                "godot.lifecycle.signal",
                "godot.lifecycle.signal_connection",
            ),
        ),
        "input_map": _records(
            semantic_inventory,
            categories=("input",),
        ),
        "autoloads": _records(
            semantic_inventory,
            kinds=("godot.lifecycle.autoload",),
        ),
        "animations": _records(
            semantic_inventory,
            categories=("animation",),
        ),
        "collision_bodies_shapes_layers_and_masks": _records(
            semantic_inventory,
            categories=("collision_physics",),
        ),
        "ui": _records(
            semantic_inventory,
            categories=("ui",),
        ),
        "audio": _records(
            semantic_inventory,
            categories=("audio",),
        ),
        "saved_state_candidates": _records(
            semantic_inventory,
            categories=("state_persistence",),
        ),
        "project_configuration": _records(
            semantic_inventory,
            categories=("project_configuration", "lifecycle"),
        ),
        "exact_source_references": _exact_source_references(inventory),
        "diagnostics": _diagnostics(semantic_inventory),
        "canonicalization": {
            "encoding": "utf-8",
            "json": "xcp-canonical-json",
            "collection_order": "semantic_id_then_source_path",
            "absolute_paths_allowed": False,
        },
    }
    validate_godot_source_model_document(document)
    return document


def validate_godot_source_model_document(
    document: Mapping[str, Any],
) -> None:
    if document.get("schema_version") != SCHEMA_VERSION:
        raise GodotSourceModelError(
            "Godot Source Model schema_version must be godot-source-model-v1"
        )
    frontend = document.get("frontend")
    if not isinstance(frontend, Mapping):
        raise GodotSourceModelError("frontend must be an object")
    if frontend.get("frontend_id") not in FRONTEND_IDS:
        raise GodotSourceModelError("frontend.frontend_id is not versioned")
    if frontend.get("extraction_mode") not in EXTRACTION_MODES:
        raise GodotSourceModelError("frontend.extraction_mode is invalid")
    source_identity = document.get("source_identity")
    if not isinstance(source_identity, Mapping):
        raise GodotSourceModelError("source_identity must be an object")
    root_sha256 = str(source_identity.get("root_sha256", ""))
    if len(root_sha256) != 64:
        raise GodotSourceModelError("source_identity.root_sha256 is invalid")
    for field in MODEL_SECTIONS:
        value = document.get(field)
        if not isinstance(value, list):
            raise GodotSourceModelError(f"{field} must be an array")
    seen_refs: set[str] = set()
    seen_paths: set[str] = set()
    for reference in document["exact_source_references"]:
        if not isinstance(reference, Mapping):
            raise GodotSourceModelError(
                "exact_source_references entries must be objects"
            )
        source_ref = str(reference.get("source_ref", ""))
        path = str(reference.get("path", ""))
        sha256 = str(reference.get("sha256", ""))
        if not source_ref or source_ref in seen_refs:
            raise GodotSourceModelError(
                "exact source references must have unique source_ref values"
            )
        if not path or path in seen_paths:
            raise GodotSourceModelError(
                "exact source references must have unique relative paths"
            )
        candidate = pathlib.PurePosixPath(path)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise GodotSourceModelError(
                "exact source references must use safe relative paths"
            )
        if len(sha256) != 64:
            raise GodotSourceModelError(
                "exact source reference sha256 is invalid"
            )
        seen_refs.add(source_ref)
        seen_paths.add(path)
    canonical_json_bytes(dict(document))


@dataclass(frozen=True)
class GodotSourceModel:
    """Canonical document plus the local exact-byte authority for G1."""

    document: Mapping[str, Any]
    source_root: pathlib.Path
    inventory: Mapping[str, Any]
    semantic_inventory: Mapping[str, Any]

    @property
    def schema_version(self) -> str:
        return str(self.document.get("schema_version", ""))

    def canonical_bytes(self) -> bytes:
        validate_godot_source_model_document(self.document)
        return canonical_json_bytes(dict(self.document))

    def sha256(self) -> str:
        return hashlib.sha256(self.canonical_bytes()).hexdigest()

    def verify_exact_source_bytes(self) -> None:
        root = self.source_root.resolve()
        if not root.is_dir() or root.is_symlink():
            raise GodotSourceModelError(
                "the source model root must be a non-symlink directory"
            )
        for reference in self.document["exact_source_references"]:
            relative = pathlib.PurePosixPath(str(reference["path"]))
            path = root.joinpath(*relative.parts)
            if not path.is_file() or path.is_symlink():
                raise GodotSourceModelError(
                    f"exact source reference is unavailable: {relative}"
                )
            payload = path.read_bytes()
            if len(payload) != int(reference["bytes"]):
                raise GodotSourceModelError(
                    f"exact source byte length changed: {relative}"
                )
            actual = hashlib.sha256(payload).hexdigest()
            if actual != reference["sha256"]:
                raise GodotSourceModelError(
                    f"exact source hash changed: {relative}"
                )


def build_file_parser_source_model(
    source_root: pathlib.Path,
    inventory: Mapping[str, Any],
    semantic_inventory: Mapping[str, Any],
    *,
    frontend_id: str,
    frontend_version: str,
    fallback_reason: str = "",
) -> GodotSourceModel:
    mode = "file_parser_fallback" if fallback_reason else "file_parser"
    document = build_godot_source_model_document(
        inventory,
        semantic_inventory,
        frontend_id=frontend_id,
        frontend_version=frontend_version,
        extraction_mode=mode,
        fallback_reason=fallback_reason,
    )
    model = GodotSourceModel(
        document=document,
        source_root=source_root.resolve(),
        inventory=copy.deepcopy(dict(inventory)),
        semantic_inventory=copy.deepcopy(dict(semantic_inventory)),
    )
    model.verify_exact_source_bytes()
    return model
