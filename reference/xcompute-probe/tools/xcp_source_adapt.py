#!/usr/bin/env python3
"""C6 external-source adaptation through a shared Creative IR backend."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

from xcp_adaptation import (
    ADAPTERS,
    ASSET_EXTENSIONS,
    CLASSIFICATIONS,
    EXECUTABLE_EXTENSIONS,
    GODOT_NUMBER,
    IDENTIFIER,
    INTENT_SCHEMA,
    LEDGER_SCHEMA,
    MODULE_SCHEMAS,
    PROFILE,
    RECEIPT_SCHEMA,
    ROOT,
    SCHEMAS,
    SEMANTIC_CATEGORIES,
    TEXT_EXTENSIONS,
    CapabilityPlanner,
    CreativeIrEmitter,
    Detection,
    EngineSourceModel,
    FidelityOracle,
    Godot2SourceAdapter,
    SemanticPass,
    SourceAdaptError,
    SourceAdapter,
    XcpLoweringBackend,
    _adaptation_handoff,
    _adapted_c5_intent,
    _asset_id,
    _audio_module,
    _campaign_scene_records,
    _document_sha,
    _fidelity_disposition,
    _handoff_artifact,
    _host_sets,
    _ir_records,
    _load_json,
    _media_type,
    _module_validate,
    _plan_item,
    _portable_id,
    _relative_files,
    _role,
    _selected_world_scene,
    _state_scalar,
    _validate,
    _validate_correction_ledger,
    _validate_fidelity_contract_integrity,
    _validate_ir_integrity,
    _validate_plan_integrity,
    _validate_schema,
    _validate_semantic_inventory_integrity,
    _verify_handoff_artifacts,
    _world2d_campaign_module,
    _world2d_hud_module,
    _world2d_module,
    _world2d_solution,
    _write_new,
    adapt,
    create_plan,
    describe,
    detect_source,
    fidelity_contract,
    finalize_readiness,
    generate_project,
    inspect_source,
    probe_fidelity,
    readiness_report,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Adapt external projects to XCP through canonical Creative IR."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("describe")
    detect = subparsers.add_parser("detect")
    detect.add_argument("--source", type=pathlib.Path, required=True)
    run = subparsers.add_parser("adapt")
    run.add_argument("--source", type=pathlib.Path, required=True)
    run.add_argument("--output", type=pathlib.Path, required=True)
    run.add_argument("--host-profile", type=pathlib.Path, required=True)
    run.add_argument("--project-name", required=True)
    run.add_argument(
        "--origin-kind",
        choices=["git", "source_archive", "authorized_local_tree"],
        required=True,
    )
    run.add_argument("--origin-locator", required=True)
    run.add_argument("--revision", required=True)
    run.add_argument(
        "--project-version",
        default="1.3.2",
        help=(
            "Immutable semantic version assigned to the generated XCP "
            "project (default: 1.3.2)."
        ),
    )
    run.add_argument(
        "--authorization-basis",
        choices=[
            "open_source_license",
            "provided_by_rightsholder",
            "user_attested_authorization",
        ],
        required=True,
    )
    run.add_argument(
        "--authorization-status",
        choices=["verified", "pending", "rejected"],
        required=True,
    )
    run.add_argument("--license-expression", default="")
    run.add_argument("--attribution", required=True)
    probe = subparsers.add_parser("probe-fidelity")
    probe.add_argument("--run-root", type=pathlib.Path, required=True)
    probe.add_argument("--output", type=pathlib.Path, required=True)
    probe.add_argument(
        "--state-evidence",
        type=pathlib.Path,
        action="append",
        default=[],
        help=(
            "Optional exact campaign restart/restore witness. Repeat for "
            "multiple bound witnesses."
        ),
    )
    probe.add_argument(
        "--human-playtest-outcome",
        choices=["pass", "fail", "not_run"],
        default="not_run",
    )
    probe.add_argument(
        "--human-evidence-ref",
        action="append",
        default=[],
        help="Exact operator evidence reference; repeat as required.",
    )
    finalize = subparsers.add_parser("finalize")
    finalize.add_argument("--run-root", type=pathlib.Path, required=True)
    finalize.add_argument("--receipt", type=pathlib.Path, required=True)
    finalize.add_argument("--ledger", type=pathlib.Path, required=True)
    finalize.add_argument(
        "--fidelity-evidence",
        type=pathlib.Path,
        help=(
            "Exact xcp-creative-fidelity-evidence-v1 document. Without it "
            "finalization remains fail-closed at not_ready."
        ),
    )
    finalize.add_argument("--output", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "describe":
            result = describe()
        elif args.command == "detect":
            _, detection = detect_source(args.source.resolve())
            result = {
                "ok": True,
                "schema_version": "xcp-source-detection-v1",
                "adapter_id": detection.adapter_id,
                "adapter_version": detection.adapter_version,
                "ecosystem": detection.ecosystem,
                "ecosystem_version": detection.ecosystem_version,
                "confidence": detection.confidence,
                "entrypoints": list(detection.entrypoints),
                "evidence": list(detection.evidence),
            }
        elif args.command == "adapt":
            result = adapt(
                args.source,
                args.output,
                args.host_profile,
                project_name=args.project_name,
                origin_kind=args.origin_kind,
                origin_locator=args.origin_locator,
                revision=args.revision,
                authorization_basis=args.authorization_basis,
                authorization_status=args.authorization_status,
                license_expression=args.license_expression,
                attribution=args.attribution,
                project_version=args.project_version,
            )
        elif args.command == "probe-fidelity":
            result = probe_fidelity(
                args.run_root,
                args.output,
                state_evidence_paths=tuple(args.state_evidence),
                human_playtest_outcome=args.human_playtest_outcome,
                human_evidence_refs=tuple(args.human_evidence_ref),
            )
        elif args.command == "finalize":
            result = finalize_readiness(
                args.run_root,
                args.receipt,
                args.ledger,
                args.output,
                args.fidelity_evidence,
            )
        else:
            raise AssertionError(args.command)
    except SourceAdaptError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok") else 2


if __name__ == "__main__":
    raise SystemExit(main())
