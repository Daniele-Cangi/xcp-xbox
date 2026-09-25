"""Godot Source Model semantic passes used by the G1 compatibility spine."""

from __future__ import annotations

import copy
from dataclasses import dataclass
from typing import Any, Callable, Mapping

from xcp_creative_project import canonical_json_bytes

from ..source_models.godot import (
    GodotSourceModel,
    GodotSourceModelError,
)


class GodotSemanticInventoryPass:
    """Expose the exact parser projection embedded in the source model."""

    pass_id = "godot.semantic-inventory.v1"
    pass_version = "1.0.0"

    def run(self, source_model: GodotSourceModel) -> dict[str, Any]:
        source_model.verify_exact_source_bytes()
        return copy.deepcopy(dict(source_model.semantic_inventory))


@dataclass(frozen=True)
class Godot2CreativeIrPass:
    """Compatibility pass around the frozen Godot 2 IR algorithm.

    The callable is injected by the legacy frontend during G1.  This keeps the
    adapter upstream of Creative IR while preserving every existing byte.
    Godot 4 will replace this compatibility callable with typed semantic
    passes after the source model is validated on a second project.
    """

    legacy_ir_emitter: Callable[
        [GodotSourceModel, Mapping[str, Any]],
        dict[str, Any],
    ]
    pass_id: str = "godot2.creative-ir.compatibility.v1"
    pass_version: str = "1.0.0"

    def emit(
        self,
        source_model: GodotSourceModel,
        semantic_projection: Mapping[str, Any],
        inventory: Mapping[str, Any],
    ) -> dict[str, Any]:
        source_model.verify_exact_source_bytes()
        if canonical_json_bytes(dict(semantic_projection)) != canonical_json_bytes(
            dict(source_model.semantic_inventory)
        ):
            raise GodotSourceModelError(
                "Creative IR emission requires the exact Source Model "
                "semantic projection"
            )
        return self.legacy_ir_emitter(source_model, inventory)
