"""Versioned ecosystem adapter registry and exact source inspection."""

from __future__ import annotations

import pathlib
from typing import Any

from xcp_creative_project import sha256_file

from ..core import (
    SourceAdaptError,
    SourceAdapter,
    _document_sha,
    _portable_id,
    _validate,
)
from ..source_models import (
    EXECUTABLE_EXTENSIONS,
    Detection,
    _media_type,
    _relative_files,
    _role,
)
from .godot2_legacy import Godot2SourceAdapter

ADAPTERS: tuple[SourceAdapter, ...] = (Godot2SourceAdapter(),)


def detect_source(source: pathlib.Path) -> tuple[SourceAdapter, Detection]:
    detections = [
        (adapter, detection)
        for adapter in ADAPTERS
        if (detection := adapter.detect(source)) is not None
    ]
    if not detections:
        raise SourceAdaptError(
            "xcp.adapt.ecosystem_unsupported",
            "No versioned source adapter recognized the external project.",
            stage="detect",
            field="source",
            expected="a project recognized by the published adapter registry",
            actual=str(source.resolve()),
            correction="install or implement an adapter that emits the common IR",
        )
    detections.sort(key=lambda item: item[1].confidence, reverse=True)
    top = detections[0]
    if len(detections) > 1 and detections[1][1].confidence == top[1].confidence:
        raise SourceAdaptError(
            "xcp.adapt.ecosystem_ambiguous",
            "Multiple adapters reported equal highest confidence.",
            stage="detect",
            field="adapter_id",
            expected="one unambiguous adapter",
            actual=",".join(item[1].adapter_id for item in detections[:2]),
            correction="select an adapter explicitly after inspecting evidence",
        )
    return top


def inspect_source(
    source: pathlib.Path,
    *,
    project_name: str,
    origin_kind: str,
    origin_locator: str,
    revision: str,
    authorization_basis: str,
    authorization_status: str,
    license_expression: str,
    attribution: str,
) -> tuple[dict[str, Any], SourceAdapter]:
    source = source.resolve()
    adapter, detection = detect_source(source)
    files = _relative_files(source)
    license_paths = [
        path.relative_to(source).as_posix()
        for path in files
        if path.name.lower().startswith(("license", "copying"))
    ]
    if authorization_status == "verified" and not license_expression:
        raise SourceAdaptError(
            "xcp.adapt.authorization_incomplete",
            "Verified source authorization requires a license expression.",
            stage="inspect",
            field="license_expression",
            expected="an SPDX-style expression or explicit authorization label",
            actual="empty",
            correction="audit the source rights and provide the expression",
        )
    if (
        authorization_basis == "open_source_license"
        and authorization_status == "verified"
        and not license_paths
    ):
        raise SourceAdaptError(
            "xcp.adapt.license_file_missing",
            "Open-source authorization could not be bound to a license file.",
            stage="inspect",
            field="license_files",
            expected="at least one source license file",
            actual="0",
            correction="select the correct source root or verify authorization",
        )

    entries = []
    root_identity = []
    executable_paths = []
    for path in files:
        relative = path.relative_to(source).as_posix()
        size = path.stat().st_size
        digest = sha256_file(path)
        suffix = path.suffix.lower()
        if suffix in EXECUTABLE_EXTENSIONS:
            executable_paths.append(relative)
        file_id = _portable_id(
            f"{pathlib.PurePosixPath(relative).with_suffix('').as_posix()}."
            f"{digest[:12]}",
            prefix="file",
            maximum=128,
        )
        entries.append(
            {
                "file_id": file_id,
                "path": relative,
                "bytes": size,
                "sha256": digest,
                "media_type": _media_type(path),
                "role": _role(pathlib.PurePosixPath(relative)),
                "provenance": {
                    "origin": origin_locator,
                    "attribution": attribution,
                },
            }
        )
        root_identity.append(
            {"path": relative, "bytes": size, "sha256": digest}
        )
    if executable_paths:
        detection_evidence = [
            *detection.evidence,
            (
                f"{len(executable_paths)} executable-like source files were "
                "inventoried but are never admitted as XCP payloads"
            ),
        ]
    else:
        detection_evidence = list(detection.evidence)
    inventory_id = f"{_portable_id(project_name, maximum=96)}.inventory"
    inventory = {
        "schema_version": "xcp-creative-source-inventory-v1",
        "inventory_id": inventory_id,
        "source": {
            "project_name": project_name,
            "origin_kind": origin_kind,
            "origin_locator": origin_locator,
            "revision": revision,
            "ecosystem": detection.ecosystem,
            "ecosystem_version": detection.ecosystem_version,
            "entrypoints": list(detection.entrypoints),
        },
        "detector": {
            "adapter_id": detection.adapter_id,
            "adapter_version": detection.adapter_version,
            "confidence": detection.confidence,
            "evidence": detection_evidence,
        },
        "authorization": {
            "basis": authorization_basis,
            "status": authorization_status,
            "license_expression": license_expression or "pending",
            "license_files": license_paths,
        },
        "root_sha256": _document_sha(root_identity),
        "files": entries,
        "scan_policy": {
            "relative_paths_only": True,
            "follow_symlinks": False,
            "secrets_in_output": False,
            "executable_payloads_admitted": False,
        },
    }
    _validate(inventory, "inventory")
    return inventory, adapter
