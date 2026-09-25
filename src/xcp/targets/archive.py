"""Local structural target proving lowering and bundle admission without execution claims."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..contracts import AdmissionPlan, ContractError, ProjectIntent, SemanticIR, SourceModel, TargetProfile, canonical_bytes, verify_plan
from ..project import build_bundle


class ArchiveTarget:
    def profile(self) -> TargetProfile:
        return TargetProfile("xcp.reference.archive", "0.1.0", ("scene.structure.v1",))

    def lower(self, intent: ProjectIntent, model: SourceModel, ir: SemanticIR,
              plan: AdmissionPlan, output: Path) -> dict[str, Any]:
        verify_plan(plan, intent, model, ir, self.profile())
        if output.exists():
            raise ContractError("xcp.lower.output_exists", "Output must be a new directory")
        output.mkdir(parents=True)
        (output / "modules").mkdir()
        (output / "modules" / "semantic.json").write_bytes(canonical_bytes({
            "schema_version": "xcp-reference-archive-module-v2", "ir_sha256": ir.sha256,
            "authorized_plan_sha256": plan.sha256,
            "records": list(ir.records),
        }))
        manifest = {
            "schema_version": "xcp-creative-project-v1", "project_id": "xcp.reference.archive",
            "version": "0.1.0", "title": "Structural reference archive",
            "description": "Exact structural projection; no executable behavior or fidelity claim",
            "entry_module": "semantic", "modules": [
                {"id": "semantic", "kind": "xcp.data.v1", "path": "modules/semantic.json", "depends_on": []}
            ], "assets": [], "requested_capabilities": [],
        }
        (output / "xcp-project.json").write_bytes(canonical_bytes(manifest))
        return {"project_dir": str(output), "ir_sha256": ir.sha256, "plan_sha256": plan.sha256}


def build_archive_bundle(project_root: Path, bundle_root: Path) -> dict[str, Any]:
    built = build_bundle(project_root, bundle_root)
    return built.metadata()
