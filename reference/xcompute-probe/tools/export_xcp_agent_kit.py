#!/usr/bin/env python3
"""Export a hash-bound C5+C6+C7+G1 agent kit without repository state."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import sys
from typing import Any

from xcp_creative_project import canonical_json_bytes, sha256_file


ROOT = pathlib.Path(__file__).resolve().parents[1]
KIT_MANIFEST = "xcp-agent-kit-manifest.json"
ADAPTATION_PACKAGE_ROOT = "tools/xcp_adaptation"
ADAPTATION_PACKAGE_FILES = (
    "tools/xcp_adaptation/__init__.py",
    "tools/xcp_adaptation/adapters/__init__.py",
    "tools/xcp_adaptation/adapters/godot2_legacy.py",
    "tools/xcp_adaptation/adapters/registry.py",
    "tools/xcp_adaptation/core/__init__.py",
    "tools/xcp_adaptation/core/contracts.py",
    "tools/xcp_adaptation/core/runtime.py",
    "tools/xcp_adaptation/fidelity/__init__.py",
    "tools/xcp_adaptation/fidelity/oracles.py",
    "tools/xcp_adaptation/handoff/__init__.py",
    "tools/xcp_adaptation/handoff/c5.py",
    "tools/xcp_adaptation/ir/__init__.py",
    "tools/xcp_adaptation/ir/model.py",
    "tools/xcp_adaptation/lowering/__init__.py",
    "tools/xcp_adaptation/lowering/backend.py",
    "tools/xcp_adaptation/planning/__init__.py",
    "tools/xcp_adaptation/planning/capabilities.py",
    "tools/xcp_adaptation/semantics/__init__.py",
    "tools/xcp_adaptation/semantics/godot.py",
    "tools/xcp_adaptation/semantics/integrity.py",
    "tools/xcp_adaptation/source_models/__init__.py",
    "tools/xcp_adaptation/source_models/engine_assisted.py",
    "tools/xcp_adaptation/source_models/godot-source-model-v1.schema.json",
    "tools/xcp_adaptation/source_models/godot.py",
    "tools/xcp_adaptation/source_models/inventory.py",
)
KIT_FILES = (
    "profiles/creative/godot-adaptation-compatibility-v1.json",
    "profiles/creative/xcp-agent-native-creation-v1.json",
    "profiles/creative/xcp-agent-native-source-adaptation-v1.json",
    "profiles/creative/xcp-project-evolution-v1.json",
    "schemas/worker-sdk-contract-v1.json",
    "schemas/xcp-agent-correction-ledger-v1.schema.json",
    "schemas/xcp-agent-blind-creation-gate-v1.schema.json",
    "schemas/xcp-agent-lifecycle-receipt-v1.schema.json",
    "schemas/xcp-agent-project-intent-v1.schema.json",
    "schemas/xcp-audio-module-v1.schema.json",
    "schemas/xcp-behavior-module-v1.schema.json",
    "schemas/xcp-canvas2d-module-v1.schema.json",
    "schemas/xcp-creative-adaptation-handoff-v1.schema.json",
    "schemas/xcp-creative-adaptation-handoff-v2.schema.json",
    "schemas/xcp-creative-adaptation-plan-v1.schema.json",
    "schemas/xcp-creative-adaptation-readiness-v1.schema.json",
    "schemas/xcp-creative-adaptation-readiness-v2.schema.json",
    "schemas/xcp-creative-bundle-v1.schema.json",
    "schemas/xcp-creative-evolution-overlay-v1.schema.json",
    "schemas/xcp-creative-evolution-report-v1.schema.json",
    "schemas/xcp-creative-evolution-reuse-v1.schema.json",
    "schemas/xcp-creative-fidelity-contract-v1.schema.json",
    "schemas/xcp-creative-fidelity-evidence-v1.schema.json",
    "schemas/xcp-creative-host-profile-v1.schema.json",
    "schemas/xcp-creative-ir-v1.schema.json",
    "schemas/xcp-creative-project-v1.schema.json",
    "schemas/xcp-creative-reconciliation-plan-v1.schema.json",
    "schemas/xcp-creative-semantic-diff-v1.schema.json",
    "schemas/xcp-creative-semantic-inventory-v1.schema.json",
    "schemas/xcp-creative-source-inventory-v1.schema.json",
    "schemas/xcp-creative-source-map-v1.schema.json",
    "schemas/xcp-data-module-v1.schema.json",
    "schemas/xcp-mesh-asset-v1.schema.json",
    "schemas/xcp-scene3d-module-v1.schema.json",
    "schemas/xcp-ui-module-v1.schema.json",
    "schemas/xcp-world2d-campaign-module-v1.schema.json",
    "schemas/xcp-world2d-module-v1.schema.json",
    "schemas/xcp-world2d-module-v2.schema.json",
    "schemas/xcp-xvm-module-v1.schema.json",
    "schemas/xvm-isa-v2.json",
    "tools/xcp_agent_lifecycle.py",
    "tools/xcp_agent_blind_gate.py",
    "tools/xcp_creative_project.py",
    "tools/xcp_project_evolve.py",
    *ADAPTATION_PACKAGE_FILES,
    "tools/xcp_source_adapt.py",
    "tools/xcp_studio_live_adapter.py",
    "tools/xcp_worker_transport.py",
)


class KitExportError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        field: str,
        expected: str,
        actual: str,
        correction: str,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = {
            "schema_version": "xcp-agent-error-details-v1",
            "stage": "export",
            "field": field,
            "expected": expected,
            "actual": actual,
            "correction": correction,
            "retryable": False,
        }

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": False,
            "schema_version": "xcp-agent-error-v1",
            "error": {
                "code": self.code,
                "message": self.message,
                "details": self.details,
            },
        }


def _source(relative: str) -> pathlib.Path:
    path = ROOT / pathlib.PurePosixPath(relative)
    if not path.is_file() or path.is_symlink():
        raise KitExportError(
            "xcp.agent.kit_source_invalid",
            "An authoritative kit source is missing or is a symbolic link.",
            field="source",
            expected="regular repository file",
            actual=relative,
            correction="restore the exact C5 authority before exporting",
        )
    return path


def _validate_adaptation_package_file_set() -> None:
    package_root = ROOT / pathlib.PurePosixPath(ADAPTATION_PACKAGE_ROOT)
    if not package_root.is_dir() or package_root.is_symlink():
        raise KitExportError(
            "xcp.agent.kit_adaptation_package_file_set_mismatch",
            "The G1 adaptation package root is missing or invalid.",
            field=ADAPTATION_PACKAGE_ROOT,
            expected="regular package directory",
            actual="missing_or_symlink",
            correction="restore the exact G1 package before exporting",
        )
    actual = {
        path.relative_to(ROOT).as_posix()
        for path in package_root.rglob("*")
        if path.is_file()
        and "__pycache__" not in path.relative_to(package_root).parts
        and path.suffix in {".py", ".json"}
    }
    expected = set(ADAPTATION_PACKAGE_FILES)
    exported = {
        relative
        for relative in KIT_FILES
        if relative.startswith(f"{ADAPTATION_PACKAGE_ROOT}/")
    }
    if actual != expected or exported != expected:
        raise KitExportError(
            "xcp.agent.kit_adaptation_package_file_set_mismatch",
            "The real G1 package file set differs from the explicit kit contract.",
            field=ADAPTATION_PACKAGE_ROOT,
            expected=",".join(sorted(expected)),
            actual=",".join(sorted(actual)),
            correction=(
                "update the explicit G1 package list for every runtime .py "
                "and schema .json file before exporting"
            ),
        )


def export_kit(output: pathlib.Path) -> dict[str, Any]:
    output = output.resolve()
    if output.exists():
        raise KitExportError(
            "xcp.agent.kit_output_exists",
            "The kit exporter will not merge with or overwrite an existing path.",
            field="output",
            expected="a new directory",
            actual=str(output),
            correction="select a fresh output directory",
        )
    staging = output.with_name(f".{output.name}.tmp-{os.getpid()}")
    if staging.exists():
        raise KitExportError(
            "xcp.agent.kit_staging_exists",
            "The private kit staging path already exists.",
            field="staging",
            expected="a fresh path",
            actual=str(staging),
            correction="remove the stale staging path after inspecting it",
        )
    _validate_adaptation_package_file_set()

    entries: list[dict[str, Any]] = []
    try:
        for relative in KIT_FILES:
            source = _source(relative)
            target = staging / pathlib.PurePosixPath(relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            entries.append(
                {
                    "path": relative,
                    "bytes": target.stat().st_size,
                    "sha256": sha256_file(target),
                }
            )
        requirements = staging / "requirements.txt"
        requirements.write_text(
            "jsonschema>=4.18,<5\n",
            encoding="utf-8",
            newline="\n",
        )
        entries.append(
            {
                "path": "requirements.txt",
                "bytes": requirements.stat().st_size,
                "sha256": sha256_file(requirements),
            }
        )
        entries.sort(key=lambda item: item["path"])
        manifest = {
            "schema_version": "xcp-agent-kit-manifest-v1",
            "kit_id": "xcp.c5-c6.agent-kit",
            "kit_version": "1.8.0",
            "gate_id": "C5_C6_AGENT_NATIVE_CREATIVE_GATE",
            "entrypoints": {
                "lifecycle": "tools/xcp_agent_lifecycle.py",
                "blind_creation_finalizer": "tools/xcp_agent_blind_gate.py",
                "project_builder": "tools/xcp_creative_project.py",
                "source_adaptation": "tools/xcp_source_adapt.py",
                "project_evolution": "tools/xcp_project_evolve.py",
                "studio_live_adapter": "tools/xcp_studio_live_adapter.py",
            },
            "runtime": {
                "python_minimum": "3.11",
                "requirements": "requirements.txt",
            },
            "security": {
                "repository_metadata_included": False,
                "samples_included": False,
                "credentials_included": False,
                "session_material_persisted": False,
            },
            "claim_boundary": {
                "minilens_r11_golden_role": (
                    "structural_refactor_regression_only"
                ),
                "minilens_r11_is_fidelity_target": False,
                "minilens_r11_final_output_frozen": False,
                "versioned_replaceable_stages": [
                    "extraction",
                    "semantic_ir_and_passes",
                    "lowering",
                    "source_fidelity_comparison",
                ],
                "generalization_target": "any_authorized_godot_project",
            },
            "files": entries,
        }
        manifest_path = staging / KIT_MANIFEST
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        os.replace(staging, output)
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise

    manifest_path = output / KIT_MANIFEST
    return {
        "ok": True,
        "schema_version": "xcp-agent-kit-export-v1",
        "output": str(output),
        "manifest": KIT_MANIFEST,
        "manifest_sha256": sha256_file(manifest_path),
        "file_count": len(entries),
        "total_bytes": sum(int(item["bytes"]) for item in entries),
    }


def verify_kit(root: pathlib.Path) -> dict[str, Any]:
    root = root.resolve()
    manifest_path = root / KIT_MANIFEST
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise KitExportError(
            "xcp.agent.kit_manifest_invalid",
            "The kit manifest is missing or invalid.",
            field="manifest",
            expected="valid UTF-8 JSON manifest",
            actual=type(exc).__name__,
            correction="export a fresh exact kit",
        ) from exc
    expected_paths = {str(item["path"]) for item in manifest.get("files", [])}
    actual_paths = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name != KIT_MANIFEST
    }
    if actual_paths != expected_paths:
        raise KitExportError(
            "xcp.agent.kit_file_set_mismatch",
            "The kit file set differs from its manifest.",
            field="files",
            expected=",".join(sorted(expected_paths)),
            actual=",".join(sorted(actual_paths)),
            correction="reject the modified kit and export exact bytes again",
        )
    for entry in manifest["files"]:
        path = root / pathlib.PurePosixPath(str(entry["path"]))
        if path.is_symlink() or path.stat().st_size != int(entry["bytes"]):
            raise KitExportError(
                "xcp.agent.kit_file_identity_mismatch",
                "A kit file has an invalid type or byte length.",
                field=str(entry["path"]),
                expected=str(entry["bytes"]),
                actual="symlink" if path.is_symlink() else str(path.stat().st_size),
                correction="reject the modified kit",
            )
        digest = sha256_file(path)
        if digest != entry["sha256"]:
            raise KitExportError(
                "xcp.agent.kit_file_identity_mismatch",
                "A kit file hash differs from the manifest.",
                field=str(entry["path"]),
                expected=str(entry["sha256"]),
                actual=digest,
                correction="reject the modified kit",
            )
    return {
        "ok": True,
        "schema_version": "xcp-agent-kit-verification-v1",
        "root": str(root),
        "manifest_sha256": sha256_file(manifest_path),
        "file_count": len(manifest["files"]),
        "total_bytes": sum(int(item["bytes"]) for item in manifest["files"]),
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Export or verify a repository-independent C5+C6+C7+G1 agent kit."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    export = subparsers.add_parser("export")
    export.add_argument("--output", type=pathlib.Path, required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("--kit", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = (
            export_kit(args.output)
            if args.command == "export"
            else verify_kit(args.kit)
        )
    except KitExportError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
