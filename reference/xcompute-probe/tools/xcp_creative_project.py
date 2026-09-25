#!/usr/bin/env python3
"""Build and verify deterministic XCP creative project bundles."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
import tempfile
from dataclasses import dataclass, replace
from typing import Any, Iterable

from jsonschema import Draft202012Validator


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_PROJECT_SCHEMA = ROOT / "schemas" / "xcp-creative-project-v1.schema.json"
DEFAULT_BUNDLE_SCHEMA = ROOT / "schemas" / "xcp-creative-bundle-v1.schema.json"
DEFAULT_HOST_PROFILE_SCHEMA = (
    ROOT / "schemas" / "xcp-creative-host-profile-v1.schema.json"
)
PROJECT_FILENAME = "xcp-project.json"
BUNDLE_FILENAME = "xcp-bundle.json"
CANVAS_STARTER_TEMPLATE = "canvas-starter-v1"
MAX_FILE_COUNT = 9216
MAX_FILE_BYTES = 8 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024
WINDOWS_RESERVED_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}


@dataclass(frozen=True)
class CreativeErrorDetails:
    stage: str
    field: str = ""
    path: str = ""
    expected: str = ""
    actual: str = ""
    correction: str = ""
    retryable: bool = False


class CreativeProjectError(ValueError):
    def __init__(
        self,
        code: str,
        message: str,
        details: CreativeErrorDetails,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": False,
            "error": {
                "code": self.code,
                "message": self.message,
                "details": {
                    "schema_version": "xcp-creative-error-details-v1",
                    "stage": self.details.stage,
                    "field": self.details.field,
                    "path": self.details.path,
                    "expected": self.details.expected,
                    "actual": self.details.actual,
                    "correction": self.details.correction,
                    "retryable": self.details.retryable,
                },
            },
        }


@dataclass(frozen=True)
class ProjectValidation:
    root: pathlib.Path
    manifest: dict[str, Any]
    manifest_bytes: bytes
    modules: tuple[dict[str, Any], ...]
    assets: tuple[dict[str, Any], ...]
    host_profile_id: str = ""
    host_profile_canonical_sha256: str = ""
    host_profile: dict[str, Any] | None = None

    def metadata(self) -> dict[str, Any]:
        total_bytes = sum(
            int(entry["bytes"]) for entry in (*self.modules, *self.assets)
        )
        result = {
            "ok": True,
            "schema_version": "xcp-creative-project-validation-v1",
            "project_id": self.manifest["project_id"],
            "project_version": self.manifest["version"],
            "entry_module": self.manifest["entry_module"],
            "module_count": len(self.modules),
            "asset_count": len(self.assets),
            "file_count": len(self.modules) + len(self.assets),
            "total_bytes": total_bytes,
            "project_manifest_sha256": sha256_bytes(self.manifest_bytes),
        }
        if self.host_profile_id:
            result["host_profile_id"] = self.host_profile_id
            result["host_profile_canonical_sha256"] = (
                self.host_profile_canonical_sha256
            )
            result["host_admission"] = "accepted"
        return result


@dataclass(frozen=True)
class BundleBuild:
    output_dir: pathlib.Path
    manifest: dict[str, Any]
    manifest_bytes: bytes
    host_profile_id: str = ""
    host_profile_canonical_sha256: str = ""

    def metadata(self) -> dict[str, Any]:
        result = {
            "ok": True,
            "schema_version": "xcp-creative-bundle-build-v1",
            "project_id": self.manifest["project"]["id"],
            "project_version": self.manifest["project"]["version"],
            "output_dir": str(self.output_dir),
            "bundle_manifest": BUNDLE_FILENAME,
            "bundle_manifest_bytes": len(self.manifest_bytes),
            "bundle_manifest_sha256": sha256_bytes(self.manifest_bytes),
            "content_sha256": self.manifest["integrity"]["content_sha256"],
            "file_count": self.manifest["integrity"]["file_count"],
            "total_bytes": self.manifest["integrity"]["total_bytes"],
        }
        if self.host_profile_id:
            result["host_profile_id"] = self.host_profile_id
            result["host_profile_canonical_sha256"] = (
                self.host_profile_canonical_sha256
            )
            result["host_admission"] = "accepted"
        return result


@dataclass(frozen=True)
class ProjectCreation:
    output_dir: pathlib.Path
    validation: ProjectValidation
    template_id: str

    def metadata(self) -> dict[str, Any]:
        return {
            **self.validation.metadata(),
            "schema_version": "xcp-creative-project-create-v1",
            "output_dir": str(self.output_dir),
            "template_id": self.template_id,
            "created_files": [
                PROJECT_FILENAME,
                "modules/main.json",
            ],
        }


@dataclass(frozen=True)
class HostProfileValidation:
    path: pathlib.Path
    manifest: dict[str, Any]
    manifest_bytes: bytes

    def metadata(self) -> dict[str, Any]:
        return {
            "ok": True,
            "schema_version": "xcp-creative-host-profile-validation-v1",
            "profile_id": self.manifest["profile_id"],
            "host_api_version": self.manifest["host_api_version"],
            "implementation_stage": self.manifest["implementation_stage"],
            "canonical_profile_sha256": sha256_bytes(self.manifest_bytes),
            "module_kind_count": len(self.manifest["module_kinds"]),
            "capability_count": len(self.manifest["capabilities"]),
        }


def canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def _fail(
    code: str,
    message: str,
    *,
    stage: str,
    field: str = "",
    path: str = "",
    expected: str = "",
    actual: str = "",
    correction: str = "",
    retryable: bool = False,
) -> None:
    raise CreativeProjectError(
        code,
        message,
        CreativeErrorDetails(
            stage=stage,
            field=field,
            path=path,
            expected=expected,
            actual=actual,
            correction=correction,
            retryable=retryable,
        ),
    )


def _load_json_bytes(path: pathlib.Path, *, stage: str) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        _fail(
            "xcp.creative.file_unreadable",
            f"cannot read {path.name}: {exc}",
            stage=stage,
            path=str(path),
            expected="readable UTF-8 JSON file",
            correction="restore the file and make it readable",
        )
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _fail(
            "xcp.creative.json_invalid",
            f"{path.name} is not valid UTF-8 JSON: {exc}",
            stage=stage,
            path=str(path),
            expected="valid UTF-8 JSON",
            correction="correct the JSON syntax and encoding",
        )
    if not isinstance(value, dict):
        _fail(
            "xcp.creative.json_root_invalid",
            f"{path.name} must contain a JSON object",
            stage=stage,
            path=str(path),
            expected="object",
            actual=type(value).__name__,
            correction="replace the root value with an object",
        )
    return value, raw


def _schema_validator(schema_path: pathlib.Path, *, stage: str) -> Draft202012Validator:
    schema, _ = _load_json_bytes(schema_path, stage=stage)
    try:
        Draft202012Validator.check_schema(schema)
    except Exception as exc:
        _fail(
            "xcp.creative.schema_invalid",
            f"schema is invalid: {exc}",
            stage=stage,
            path=str(schema_path),
            expected="valid JSON Schema 2020-12",
            correction="repair the authoritative schema",
        )
    return Draft202012Validator(schema)


def _json_path(parts: Iterable[Any]) -> str:
    rendered = "$"
    for part in parts:
        if isinstance(part, int):
            rendered += f"[{part}]"
        else:
            rendered += f".{part}"
    return rendered


def _validate_schema(
    value: dict[str, Any],
    schema_path: pathlib.Path,
    *,
    stage: str,
) -> None:
    validator = _schema_validator(schema_path, stage=stage)
    errors = sorted(
        validator.iter_errors(value),
        key=lambda error: tuple(str(part) for part in error.absolute_path),
    )
    if not errors:
        return
    error = errors[0]
    field = _json_path(error.absolute_path)
    _fail(
        "xcp.creative.schema_rejected",
        f"document does not satisfy the creative contract: {error.message}",
        stage=stage,
        field=field,
        expected="value accepted by the authoritative schema",
        actual=repr(error.instance)[:256],
        correction=f"correct {field} using {schema_path.name}",
    )


def _normalize_relative_path(value: str, *, stage: str, field: str) -> pathlib.PurePosixPath:
    if "\\" in value:
        _fail(
            "xcp.creative.path_not_canonical",
            "runtime paths must use forward slashes",
            stage=stage,
            field=field,
            path=value,
            expected="canonical relative POSIX path",
            actual=value,
            correction="replace backslashes with forward slashes",
        )
    candidate = pathlib.PurePosixPath(value)
    if (
        candidate.is_absolute()
        or not candidate.parts
        or any(part in {"", ".", ".."} for part in candidate.parts)
        or candidate.as_posix() != value
    ):
        _fail(
            "xcp.creative.path_not_canonical",
            "runtime path is not a canonical relative path",
            stage=stage,
            field=field,
            path=value,
            expected="relative path without '.', '..' or repeated separators",
            actual=value,
            correction="use a normalized path inside the project directory",
        )
    if value in {PROJECT_FILENAME, BUNDLE_FILENAME}:
        _fail(
            "xcp.creative.path_reserved",
            "runtime path collides with a creative contract manifest",
            stage=stage,
            field=field,
            path=value,
            expected="runtime content path distinct from contract manifests",
            actual=value,
            correction="move the runtime file under a modules or assets directory",
        )
    for part in candidate.parts:
        stem = part.split(".", 1)[0].upper()
        if stem in WINDOWS_RESERVED_NAMES:
            _fail(
                "xcp.creative.path_reserved",
                "runtime path uses a Windows-reserved name",
                stage=stage,
                field=field,
                path=value,
                expected="portable non-reserved path segment",
                actual=part,
                correction="rename the path segment",
            )
    return candidate


def _resolve_declared_file(
    root: pathlib.Path,
    relative: str,
    *,
    stage: str,
    field: str,
) -> pathlib.Path:
    normalized = _normalize_relative_path(relative, stage=stage, field=field)
    current = root
    for part in normalized.parts:
        current = current / part
        if current.is_symlink():
            _fail(
                "xcp.creative.symlink_rejected",
                "creative project files cannot traverse symlinks",
                stage=stage,
                field=field,
                path=relative,
                expected="regular file under the project root",
                actual="symlink",
                correction="replace the symlink with a regular project file",
            )
    if not current.is_file():
        _fail(
            "xcp.creative.file_missing",
            "declared runtime file is missing",
            stage=stage,
            field=field,
            path=relative,
            expected="existing regular file",
            actual="missing or not a regular file",
            correction="create the file or correct its declared path",
        )
    try:
        resolved = current.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError):
        _fail(
            "xcp.creative.path_escape",
            "declared runtime file escapes the project root",
            stage=stage,
            field=field,
            path=relative,
            expected=str(root),
            actual=str(current),
            correction="move the file inside the project root",
        )
    return resolved


def _require_unique(
    values: Iterable[str],
    *,
    stage: str,
    field: str,
    code: str,
) -> None:
    seen: set[str] = set()
    for value in values:
        if value in seen:
            _fail(
                code,
                f"duplicate value in {field}",
                stage=stage,
                field=field,
                actual=value,
                expected="unique values",
                correction=f"rename or remove the duplicate {value}",
            )
        seen.add(value)


def validate_host_profile(
    profile_path: pathlib.Path | str,
    *,
    host_profile_schema: pathlib.Path = DEFAULT_HOST_PROFILE_SCHEMA,
) -> HostProfileValidation:
    path = pathlib.Path(profile_path).resolve()
    if path.is_symlink():
        _fail(
            "xcp.creative.symlink_rejected",
            "host profile cannot be a symlink",
            stage="host_profile_validation",
            path=str(path),
            expected="regular host-profile JSON file",
            actual="symlink",
            correction="save the discovered profile as a regular file",
        )
    profile, _ = _load_json_bytes(path, stage="host_profile_validation")
    _validate_schema(
        profile,
        host_profile_schema,
        stage="host_profile_validation",
    )
    _require_unique(
        (str(entry["id"]) for entry in profile["module_kinds"]),
        stage="host_profile_validation",
        field="module_kinds[].id",
        code="xcp.creative.host_module_kind_duplicate",
    )
    _require_unique(
        (str(entry["id"]) for entry in profile["asset_formats"]),
        stage="host_profile_validation",
        field="asset_formats[].id",
        code="xcp.creative.host_asset_format_duplicate",
    )
    _require_unique(
        (str(entry["media_type"]) for entry in profile["asset_formats"]),
        stage="host_profile_validation",
        field="asset_formats[].media_type",
        code="xcp.creative.host_asset_media_type_duplicate",
    )
    _require_unique(
        (str(entry["id"]) for entry in profile["capabilities"]),
        stage="host_profile_validation",
        field="capabilities[].id",
        code="xcp.creative.host_capability_duplicate",
    )
    capabilities = {
        str(entry["id"]): bool(entry["admitted"])
        for entry in profile["capabilities"]
    }
    for module in profile["module_kinds"]:
        if not bool(module["admitted"]):
            continue
        for capability in module["required_capabilities"]:
            capability_id = str(capability)
            if not capabilities.get(capability_id, False):
                _fail(
                    "xcp.creative.host_profile_inconsistent",
                    "admitted module kind depends on a capability not admitted by the host",
                    stage="host_profile_validation",
                    field=f"module_kinds[{module['id']}].required_capabilities",
                    expected="capability present with admitted=true",
                    actual=capability_id,
                    correction=(
                        "admit the required capability or mark the module kind "
                        "not admitted"
                    ),
                )
    canonical_bytes = canonical_json_bytes(profile)
    return HostProfileValidation(
        path=path,
        manifest=profile,
        manifest_bytes=canonical_bytes,
    )


def _validate_project_against_host(
    manifest: dict[str, Any],
    modules: tuple[dict[str, Any], ...],
    assets: tuple[dict[str, Any], ...],
    profile: dict[str, Any],
) -> None:
    stage = "host_admission"
    project_schema_version = str(manifest["schema_version"])
    if project_schema_version not in profile["project_schema_versions"]:
        _fail(
            "xcp.creative.host_project_schema_unsupported",
            "host does not admit the project schema version",
            stage=stage,
            field="schema_version",
            expected="one of: " + ", ".join(profile["project_schema_versions"]),
            actual=project_schema_version,
            correction="upgrade the host or export the project in a supported schema",
        )

    module_kinds = {
        str(entry["id"]): entry for entry in profile["module_kinds"]
    }
    requested_capabilities = {
        str(value) for value in manifest["requested_capabilities"]
    }
    for index, module in enumerate(modules):
        kind = str(module["kind"])
        host_kind = module_kinds.get(kind)
        if host_kind is None or not bool(host_kind["admitted"]):
            _fail(
                "xcp.creative.host_module_kind_unsupported",
                "host does not admit a declared module kind",
                stage=stage,
                field=f"modules[{index}].kind",
                expected="an admitted module kind from describe_creative_host",
                actual=kind,
                correction="use an admitted kind or select a host that publishes it",
            )
        for capability in host_kind["required_capabilities"]:
            capability_id = str(capability)
            if capability_id not in requested_capabilities:
                _fail(
                    "xcp.creative.module_capability_not_requested",
                    "module kind requires a capability not requested by the project",
                    stage=stage,
                    field="requested_capabilities",
                    expected=f"project requests {capability_id}",
                    actual="capability absent",
                    correction=(
                        f"add {capability_id} to requested_capabilities or "
                        "remove the dependent module"
                    ),
                )

    capabilities = {
        str(entry["id"]): bool(entry["admitted"])
        for entry in profile["capabilities"]
    }
    for index, capability in enumerate(manifest["requested_capabilities"]):
        capability_id = str(capability)
        if not capabilities.get(capability_id, False):
            _fail(
                "xcp.creative.host_capability_unsupported",
                "host does not admit a requested capability",
                stage=stage,
                field=f"requested_capabilities[{index}]",
                expected="an admitted capability from describe_creative_host",
                actual=capability_id,
                correction="remove the capability or select a host that publishes it",
            )

    asset_formats = {
        str(entry["media_type"]): entry
        for entry in profile["asset_formats"]
    }
    for index, asset in enumerate(assets):
        media_type = str(asset["media_type"])
        asset_format = asset_formats.get(media_type)
        if asset_format is None:
            continue
        if not bool(asset_format["admitted"]):
            _fail(
                "xcp.creative.host_asset_format_unsupported",
                "host does not admit a declared decoded asset format",
                stage=stage,
                field=f"assets[{index}].media_type",
                expected="an admitted decoded format from describe_creative_host",
                actual=media_type,
                correction=(
                    "use an admitted decoded format, keep the asset opaque, "
                    "or select a host that publishes it"
                ),
            )
        max_asset_bytes = int(
            asset_format["limits"]["max_file_bytes"]
        )
        if int(asset["bytes"]) > max_asset_bytes:
            _fail(
                "xcp.creative.host_asset_format_budget_exceeded",
                "decoded asset source exceeds its format-specific host budget",
                stage=stage,
                field=f"assets[{index}].bytes",
                path=str(asset["path"]),
                expected=f"at most {max_asset_bytes} bytes",
                actual=str(asset["bytes"]),
                correction="reduce or split the decoded asset",
            )

    budgets = profile["budgets"]
    if len(modules) > int(budgets["max_module_count"]):
        _fail(
            "xcp.creative.host_module_count_exceeded",
            "project exceeds the host module-count budget",
            stage=stage,
            field="modules",
            expected=f"at most {budgets['max_module_count']} modules",
            actual=str(len(modules)),
            correction="reduce or combine project modules",
        )
    if len(assets) > int(budgets["max_asset_count"]):
        _fail(
            "xcp.creative.host_asset_count_exceeded",
            "project exceeds the host asset-count budget",
            stage=stage,
            field="assets",
            expected=f"at most {budgets['max_asset_count']} assets",
            actual=str(len(assets)),
            correction="reduce or combine project assets",
        )
    entries = (*modules, *assets)
    if len(entries) > int(budgets["max_file_count"]):
        _fail(
            "xcp.creative.host_file_count_exceeded",
            "project exceeds the host file-count budget",
            stage=stage,
            field="modules/assets",
            expected=f"at most {budgets['max_file_count']} files",
            actual=str(len(entries)),
            correction="reduce or combine runtime files",
        )
    max_file_bytes = int(budgets["max_file_bytes"])
    for entry in entries:
        entry_bytes = int(entry["bytes"])
        if entry_bytes > max_file_bytes:
            _fail(
                "xcp.creative.host_file_budget_exceeded",
                "runtime file exceeds the host per-file budget",
                stage=stage,
                field="modules/assets",
                path=str(entry["path"]),
                expected=f"at most {max_file_bytes} bytes",
                actual=str(entry_bytes),
                correction="split or reduce the runtime file",
            )
    if "max_module_document_bytes" in budgets:
        max_module_document_bytes = int(
            budgets["max_module_document_bytes"]
        )
        for module in modules:
            module_bytes = int(module["bytes"])
            if module_bytes > max_module_document_bytes:
                _fail(
                    "xcp.creative.host_module_document_budget_exceeded",
                    "module JSON exceeds the host parse budget",
                    stage=stage,
                    field="modules",
                    path=str(module["path"]),
                    expected=(
                        f"at most {max_module_document_bytes} bytes"
                    ),
                    actual=str(module_bytes),
                    correction="split or reduce the module document",
                )
    total_bytes = sum(int(entry["bytes"]) for entry in entries)
    max_project_bytes = int(budgets["max_project_bytes"])
    if total_bytes > max_project_bytes:
        _fail(
            "xcp.creative.host_project_budget_exceeded",
            "project exceeds the host aggregate project budget",
            stage=stage,
            field="modules/assets",
            expected=f"at most {max_project_bytes} bytes",
            actual=str(total_bytes),
            correction="reduce project content or select a host with a larger budget",
        )


def _validate_module_graph(
    modules: list[dict[str, Any]],
    entry_module: str,
    *,
    stage: str,
) -> None:
    module_ids = [str(module["id"]) for module in modules]
    _require_unique(
        module_ids,
        stage=stage,
        field="modules[].id",
        code="xcp.creative.module_id_duplicate",
    )
    known = set(module_ids)
    if entry_module not in known:
        _fail(
            "xcp.creative.entry_module_missing",
            "entry_module does not name a declared module",
            stage=stage,
            field="entry_module",
            expected="one of: " + ", ".join(sorted(known)),
            actual=entry_module,
            correction="declare the module or select an existing module id",
        )

    dependency_map: dict[str, tuple[str, ...]] = {}
    for module in modules:
        module_id = str(module["id"])
        dependencies = tuple(str(value) for value in module["depends_on"])
        for dependency in dependencies:
            if dependency not in known:
                _fail(
                    "xcp.creative.module_dependency_missing",
                    "module dependency does not name a declared module",
                    stage=stage,
                    field=f"modules[{module_id}].depends_on",
                    expected="one of: " + ", ".join(sorted(known)),
                    actual=dependency,
                    correction="declare the dependency or remove the reference",
                )
            if dependency == module_id:
                _fail(
                    "xcp.creative.module_dependency_self",
                    "module cannot depend on itself",
                    stage=stage,
                    field=f"modules[{module_id}].depends_on",
                    actual=dependency,
                    expected="a different module id",
                    correction="remove the self-dependency",
                )
        dependency_map[module_id] = dependencies

    state: dict[str, int] = {}
    chain: list[str] = []

    def visit(module_id: str) -> None:
        if state.get(module_id) == 2:
            return
        if state.get(module_id) == 1:
            start = chain.index(module_id)
            cycle = chain[start:] + [module_id]
            _fail(
                "xcp.creative.module_dependency_cycle",
                "module dependency graph contains a cycle",
                stage=stage,
                field="modules[].depends_on",
                actual=" -> ".join(cycle),
                expected="acyclic dependency graph",
                correction="remove at least one dependency in the reported cycle",
            )
        state[module_id] = 1
        chain.append(module_id)
        for dependency in dependency_map[module_id]:
            visit(dependency)
        chain.pop()
        state[module_id] = 2

    for module_id in sorted(known):
        visit(module_id)


def _inspect_entries(
    root: pathlib.Path,
    manifest: dict[str, Any],
    *,
    stage: str,
) -> tuple[tuple[dict[str, Any], ...], tuple[dict[str, Any], ...]]:
    _validate_module_graph(
        manifest["modules"],
        str(manifest["entry_module"]),
        stage=stage,
    )
    _require_unique(
        (str(asset["id"]) for asset in manifest["assets"]),
        stage=stage,
        field="assets[].id",
        code="xcp.creative.asset_id_duplicate",
    )

    all_paths = [
        *(str(module["path"]) for module in manifest["modules"]),
        *(str(asset["path"]) for asset in manifest["assets"]),
    ]
    _require_unique(
        all_paths,
        stage=stage,
        field="modules[].path/assets[].path",
        code="xcp.creative.runtime_path_duplicate",
    )
    if len(all_paths) > MAX_FILE_COUNT:
        _fail(
            "xcp.creative.file_count_exceeded",
            "project exceeds the builder file-count ceiling",
            stage=stage,
            field="modules/assets",
            expected=f"at most {MAX_FILE_COUNT} files",
            actual=str(len(all_paths)),
            correction="remove or combine runtime files",
        )

    total_bytes = 0

    def inspect(entry: dict[str, Any], field: str) -> dict[str, Any]:
        nonlocal total_bytes
        relative = str(entry["path"])
        source = _resolve_declared_file(
            root,
            relative,
            stage=stage,
            field=field,
        )
        size = source.stat().st_size
        if size <= 0:
            _fail(
                "xcp.creative.file_empty",
                "declared runtime file is empty",
                stage=stage,
                field=field,
                path=relative,
                expected="at least one byte",
                actual=str(size),
                correction="write the module or asset content",
            )
        if size > MAX_FILE_BYTES:
            _fail(
                "xcp.creative.file_budget_exceeded",
                "declared runtime file exceeds the portable builder ceiling",
                stage=stage,
                field=field,
                path=relative,
                expected=f"at most {MAX_FILE_BYTES} bytes",
                actual=str(size),
                correction="split or reduce the file; the live host may be stricter",
            )
        total_bytes += size
        if total_bytes > MAX_TOTAL_BYTES:
            _fail(
                "xcp.creative.project_budget_exceeded",
                "project exceeds the portable builder aggregate ceiling",
                stage=stage,
                field="modules/assets",
                expected=f"at most {MAX_TOTAL_BYTES} bytes",
                actual=str(total_bytes),
                correction="reduce project content; live host limits may be stricter",
            )
        return {
            **entry,
            "bytes": size,
            "sha256": sha256_file(source),
        }

    modules = tuple(
        inspect(module, f"modules[{index}].path")
        for index, module in enumerate(manifest["modules"])
    )
    assets = tuple(
        inspect(asset, f"assets[{index}].path")
        for index, asset in enumerate(manifest["assets"])
    )
    return modules, assets


def validate_project(
    project_dir: pathlib.Path | str,
    *,
    project_schema: pathlib.Path = DEFAULT_PROJECT_SCHEMA,
    host_profile: pathlib.Path | str | None = None,
    host_profile_schema: pathlib.Path = DEFAULT_HOST_PROFILE_SCHEMA,
) -> ProjectValidation:
    root = pathlib.Path(project_dir).resolve()
    if not root.is_dir():
        _fail(
            "xcp.creative.project_directory_missing",
            "project directory does not exist",
            stage="project_validation",
            path=str(root),
            expected="existing project directory",
            correction="select a directory containing xcp-project.json",
        )
    manifest_path = root / PROJECT_FILENAME
    if manifest_path.is_symlink():
        _fail(
            "xcp.creative.symlink_rejected",
            "project manifest cannot be a symlink",
            stage="project_validation",
            path=str(manifest_path),
            expected="regular xcp-project.json",
            actual="symlink",
            correction="replace the symlink with a regular manifest",
        )
    manifest, manifest_bytes = _load_json_bytes(
        manifest_path,
        stage="project_validation",
    )
    _validate_schema(
        manifest,
        project_schema,
        stage="project_validation",
    )
    modules, assets = _inspect_entries(
        root,
        manifest,
        stage="project_validation",
    )
    validated_profile = (
        validate_host_profile(
            host_profile,
            host_profile_schema=host_profile_schema,
        )
        if host_profile is not None
        else None
    )
    if validated_profile is not None:
        _validate_project_against_host(
            manifest,
            modules,
            assets,
            validated_profile.manifest,
        )
    return ProjectValidation(
        root=root,
        manifest=manifest,
        manifest_bytes=manifest_bytes,
        modules=modules,
        assets=assets,
        host_profile_id=(
            str(validated_profile.manifest["profile_id"])
            if validated_profile is not None
            else ""
        ),
        host_profile_canonical_sha256=(
            sha256_bytes(validated_profile.manifest_bytes)
            if validated_profile is not None
            else ""
        ),
        host_profile=(
            validated_profile.manifest
            if validated_profile is not None
            else None
        ),
    )


def _content_records(
    modules: Iterable[dict[str, Any]],
    assets: Iterable[dict[str, Any]],
) -> list[dict[str, Any]]:
    records = [
        {
            "role": "module",
            "id": str(entry["id"]),
            "path": str(entry["path"]),
            "bytes": int(entry["bytes"]),
            "sha256": str(entry["sha256"]),
        }
        for entry in modules
    ]
    records.extend(
        {
            "role": "asset",
            "id": str(entry["id"]),
            "path": str(entry["path"]),
            "bytes": int(entry["bytes"]),
            "sha256": str(entry["sha256"]),
        }
        for entry in assets
    )
    return sorted(records, key=lambda item: (item["role"], item["id"], item["path"]))


def _bundle_manifest(validation: ProjectValidation) -> dict[str, Any]:
    project = validation.manifest
    modules = sorted(
        (dict(entry) for entry in validation.modules),
        key=lambda entry: str(entry["id"]),
    )
    assets = sorted(
        (dict(entry) for entry in validation.assets),
        key=lambda entry: str(entry["id"]),
    )
    records = _content_records(modules, assets)
    return {
        "schema_version": "xcp-creative-bundle-v1",
        "project": {
            "id": project["project_id"],
            "version": project["version"],
            "title": project["title"],
            "description": project["description"],
            "tags": sorted(project.get("tags", [])),
        },
        "entry_module": project["entry_module"],
        "requested_capabilities": sorted(project["requested_capabilities"]),
        "modules": modules,
        "assets": assets,
        "integrity": {
            "algorithm": "sha256",
            "file_count": len(records),
            "total_bytes": sum(int(record["bytes"]) for record in records),
            "project_manifest_sha256": sha256_bytes(validation.manifest_bytes),
            "content_sha256": sha256_bytes(canonical_json_bytes(records)),
        },
    }


def verify_bundle(
    bundle_dir: pathlib.Path | str,
    *,
    bundle_schema: pathlib.Path = DEFAULT_BUNDLE_SCHEMA,
) -> BundleBuild:
    root = pathlib.Path(bundle_dir).resolve()
    if not root.is_dir():
        _fail(
            "xcp.creative.bundle_directory_missing",
            "bundle directory does not exist",
            stage="bundle_verification",
            path=str(root),
            expected="existing bundle directory",
            correction="build the project or select the correct bundle",
        )
    manifest_path = root / BUNDLE_FILENAME
    if manifest_path.is_symlink():
        _fail(
            "xcp.creative.symlink_rejected",
            "bundle manifest cannot be a symlink",
            stage="bundle_verification",
            path=str(manifest_path),
            expected="regular xcp-bundle.json",
            actual="symlink",
            correction="replace the symlink with the exact generated manifest",
        )
    manifest, manifest_bytes = _load_json_bytes(
        manifest_path,
        stage="bundle_verification",
    )
    _validate_schema(
        manifest,
        bundle_schema,
        stage="bundle_verification",
    )
    _validate_module_graph(
        manifest["modules"],
        str(manifest["entry_module"]),
        stage="bundle_verification",
    )
    _require_unique(
        (str(asset["id"]) for asset in manifest["assets"]),
        stage="bundle_verification",
        field="assets[].id",
        code="xcp.creative.asset_id_duplicate",
    )
    entries = [*manifest["modules"], *manifest["assets"]]
    _require_unique(
        (str(entry["path"]) for entry in entries),
        stage="bundle_verification",
        field="modules[].path/assets[].path",
        code="xcp.creative.runtime_path_duplicate",
    )
    declared_paths = {str(entry["path"]) for entry in entries}
    actual_paths: set[str] = set()
    for candidate in root.rglob("*"):
        relative = candidate.relative_to(root).as_posix()
        if candidate.is_symlink():
            _fail(
                "xcp.creative.symlink_rejected",
                "creative bundles cannot contain symlinks",
                stage="bundle_verification",
                path=relative,
                expected="regular declared file or directory",
                actual="symlink",
                correction="remove the symlink and rebuild the bundle",
            )
        if candidate.is_file() and relative != BUNDLE_FILENAME:
            actual_paths.add(relative)
    unexpected = sorted(actual_paths - declared_paths)
    if unexpected:
        _fail(
            "xcp.creative.bundle_file_undeclared",
            "bundle contains a runtime file not sealed by the manifest",
            stage="bundle_verification",
            field="modules/assets",
            path=unexpected[0],
            expected="every runtime file declared and hash-bound",
            actual=unexpected[0],
            correction="remove the file or declare it in the source project and rebuild",
        )
    for index, entry in enumerate(entries):
        relative = str(entry["path"])
        path = _resolve_declared_file(
            root,
            relative,
            stage="bundle_verification",
            field=f"files[{index}].path",
        )
        actual_size = path.stat().st_size
        expected_size = int(entry["bytes"])
        if actual_size != expected_size:
            _fail(
                "xcp.creative.file_size_mismatch",
                "bundle file size differs from the sealed manifest",
                stage="bundle_verification",
                field=f"files[{index}].bytes",
                path=relative,
                expected=str(expected_size),
                actual=str(actual_size),
                correction="rebuild the bundle from trusted project source",
            )
        actual_sha = sha256_file(path)
        expected_sha = str(entry["sha256"])
        if actual_sha != expected_sha:
            _fail(
                "xcp.creative.file_hash_mismatch",
                "bundle file hash differs from the sealed manifest",
                stage="bundle_verification",
                field=f"files[{index}].sha256",
                path=relative,
                expected=expected_sha,
                actual=actual_sha,
                correction="rebuild the bundle from trusted project source",
            )

    records = _content_records(manifest["modules"], manifest["assets"])
    expected_count = int(manifest["integrity"]["file_count"])
    expected_total = int(manifest["integrity"]["total_bytes"])
    actual_total = sum(int(record["bytes"]) for record in records)
    actual_content_sha = sha256_bytes(canonical_json_bytes(records))
    if len(records) != expected_count:
        _fail(
            "xcp.creative.file_count_mismatch",
            "bundle file count differs from its integrity record",
            stage="bundle_verification",
            field="integrity.file_count",
            expected=str(expected_count),
            actual=str(len(records)),
            correction="rebuild the bundle",
        )
    if actual_total != expected_total:
        _fail(
            "xcp.creative.total_bytes_mismatch",
            "bundle byte total differs from its integrity record",
            stage="bundle_verification",
            field="integrity.total_bytes",
            expected=str(expected_total),
            actual=str(actual_total),
            correction="rebuild the bundle",
        )
    if actual_content_sha != manifest["integrity"]["content_sha256"]:
        _fail(
            "xcp.creative.content_hash_mismatch",
            "bundle aggregate content hash is invalid",
            stage="bundle_verification",
            field="integrity.content_sha256",
            expected=str(manifest["integrity"]["content_sha256"]),
            actual=actual_content_sha,
            correction="rebuild the bundle",
        )
    return BundleBuild(root, manifest, manifest_bytes)


def build_bundle(
    project_dir: pathlib.Path | str,
    output_dir: pathlib.Path | str,
    *,
    project_schema: pathlib.Path = DEFAULT_PROJECT_SCHEMA,
    bundle_schema: pathlib.Path = DEFAULT_BUNDLE_SCHEMA,
    host_profile: pathlib.Path | str | None = None,
    host_profile_schema: pathlib.Path = DEFAULT_HOST_PROFILE_SCHEMA,
) -> BundleBuild:
    validation = validate_project(
        project_dir,
        project_schema=project_schema,
        host_profile=host_profile,
        host_profile_schema=host_profile_schema,
    )
    if (
        validation.host_profile is not None
        and "xcp-creative-bundle-v1"
        not in validation.host_profile["bundle_schema_versions"]
    ):
        _fail(
            "xcp.creative.host_bundle_schema_unsupported",
            "host does not admit the generated bundle schema version",
            stage="host_admission",
            field="bundle_schema_versions",
            expected="xcp-creative-bundle-v1",
            actual=", ".join(validation.host_profile["bundle_schema_versions"]),
            correction="upgrade the host or use a supported bundle builder",
        )
    output = pathlib.Path(output_dir).resolve()
    if output.exists():
        _fail(
            "xcp.creative.output_exists",
            "bundle output directory already exists",
            stage="bundle_build",
            path=str(output),
            expected="nonexistent output path",
            actual="existing path",
            correction="choose a new versioned output directory",
        )
    try:
        output.relative_to(validation.root)
    except ValueError:
        pass
    else:
        _fail(
            "xcp.creative.output_inside_project",
            "bundle output cannot be written inside its source project",
            stage="bundle_build",
            path=str(output),
            expected="output directory outside the source project",
            correction="choose a sibling build directory",
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(
        tempfile.mkdtemp(
            prefix=f".{output.name}.building-",
            dir=output.parent,
        )
    )
    try:
        for entry in (*validation.modules, *validation.assets):
            relative = pathlib.PurePosixPath(str(entry["path"]))
            source = validation.root.joinpath(*relative.parts)
            destination = temporary.joinpath(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)

        manifest = _bundle_manifest(validation)
        manifest_bytes = canonical_json_bytes(manifest)
        (temporary / BUNDLE_FILENAME).write_bytes(manifest_bytes)
        verified = verify_bundle(temporary, bundle_schema=bundle_schema)
        os.replace(temporary, output)
        return BundleBuild(
            output,
            verified.manifest,
            verified.manifest_bytes,
            validation.host_profile_id,
            validation.host_profile_canonical_sha256,
        )
    except Exception:
        if temporary.exists():
            shutil.rmtree(temporary)
        raise


def create_project(
    output_dir: pathlib.Path,
    *,
    project_id: str,
    title: str,
    template_id: str = CANVAS_STARTER_TEMPLATE,
    project_schema: pathlib.Path = DEFAULT_PROJECT_SCHEMA,
    host_profile: pathlib.Path | None = None,
    host_profile_schema: pathlib.Path = DEFAULT_HOST_PROFILE_SCHEMA,
) -> ProjectCreation:
    output = output_dir.resolve()
    if output.exists():
        _fail(
            "xcp.creative.output_exists",
            f"project output already exists: {output}",
            stage="project_create",
            path=str(output),
            expected="a path that does not exist",
            actual="existing path",
            correction="choose a new empty project path",
        )
    if template_id != CANVAS_STARTER_TEMPLATE:
        _fail(
            "xcp.creative.template_unsupported",
            f"unsupported project template: {template_id}",
            stage="project_create",
            field="template_id",
            expected=CANVAS_STARTER_TEMPLATE,
            actual=template_id,
            correction="select a template published by the builder description",
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(
        tempfile.mkdtemp(prefix=".xcp-project-create-", dir=output.parent)
    )
    try:
        (temporary / "modules").mkdir()
        project = {
            "schema_version": "xcp-creative-project-v1",
            "project_id": project_id,
            "version": "1.0.0",
            "title": title,
            "description": (
                f"{title} — created from the public XCP canvas starter."
            ),
            "entry_module": "main",
            "modules": [
                {
                    "id": "main",
                    "kind": "xcp.canvas2d.v1",
                    "path": "modules/main.json",
                    "depends_on": [],
                }
            ],
            "assets": [],
            "requested_capabilities": [
                "input.gamepad",
                "render.canvas2d",
            ],
            "tags": [
                "starter",
                "studio",
            ],
        }
        module = {
            "schema_version": "xcp-canvas2d-module-v1",
            "canvas": {
                "width": 1280,
                "height": 720,
                "background": "#101410",
            },
            "nodes": [
                {
                    "id": "focus",
                    "type": "rectangle",
                    "transform": {
                        "position": [640, 360],
                    },
                    "size": [240, 160],
                    "style": {
                        "fill": "#77E21D",
                        "opacity": 1,
                    },
                    "visible": True,
                }
            ],
            "input_bindings": [
                {
                    "action": "canvas.move-focus",
                    "source": "gamepad.left_stick",
                    "target_node": "focus",
                    "scale": 6,
                }
            ],
        }
        (temporary / PROJECT_FILENAME).write_bytes(canonical_json_bytes(project))
        (temporary / "modules" / "main.json").write_bytes(
            canonical_json_bytes(module)
        )
        validation = validate_project(
            temporary,
            project_schema=project_schema,
            host_profile=host_profile,
            host_profile_schema=host_profile_schema,
        )
        os.replace(temporary, output)
        validation = replace(validation, root=output)
        return ProjectCreation(
            output_dir=output,
            validation=validation,
            template_id=template_id,
        )
    except Exception:
        if temporary.exists():
            shutil.rmtree(temporary)
        raise


def describe_builder() -> dict[str, Any]:
    project_schema_bytes = DEFAULT_PROJECT_SCHEMA.read_bytes()
    bundle_schema_bytes = DEFAULT_BUNDLE_SCHEMA.read_bytes()
    host_profile_schema_bytes = DEFAULT_HOST_PROFILE_SCHEMA.read_bytes()
    return {
        "ok": True,
        "schema_version": "xcp-creative-builder-description-v1",
        "project_schema": {
            "filename": DEFAULT_PROJECT_SCHEMA.name,
            "bytes": len(project_schema_bytes),
            "sha256": sha256_bytes(project_schema_bytes),
        },
        "bundle_schema": {
            "filename": DEFAULT_BUNDLE_SCHEMA.name,
            "bytes": len(bundle_schema_bytes),
            "sha256": sha256_bytes(bundle_schema_bytes),
        },
        "host_profile_schema": {
            "filename": DEFAULT_HOST_PROFILE_SCHEMA.name,
            "bytes": len(host_profile_schema_bytes),
            "sha256": sha256_bytes(host_profile_schema_bytes),
        },
        "project_manifest": PROJECT_FILENAME,
        "bundle_manifest": BUNDLE_FILENAME,
        "commands": ["describe", "create", "validate", "build", "verify"],
        "templates": [
            {
                "id": CANVAS_STARTER_TEMPLATE,
                "project_schema_version": "xcp-creative-project-v1",
                "module_kinds": ["xcp.canvas2d.v1"],
                "requested_capabilities": [
                    "input.gamepad",
                    "render.canvas2d",
                ],
            }
        ],
        "portable_builder_ceilings": {
            "file_count": MAX_FILE_COUNT,
            "file_bytes": MAX_FILE_BYTES,
            "total_bytes": MAX_TOTAL_BYTES,
            "live_host_may_be_stricter": True,
        },
        "host_admission": {
            "optional_profile_argument": "--host-profile",
            "profile_schema": "xcp-creative-host-profile-v1",
            "checks": [
                "project_and_bundle_schema_versions",
                "module_kinds",
                "requested_and_module_required_capabilities",
                "module_asset_file_and_project_budgets",
            ],
        },
        "determinism": {
            "manifest_encoding": "ordered-canonical-json-utf8-lf-v1",
            "object_keys_sorted": True,
            "module_order": "id_ascending",
            "asset_order": "id_ascending",
            "hash": "sha256",
        },
    }


def _path(value: str) -> pathlib.Path:
    return pathlib.Path(value)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate and build deterministic XCP creative bundles."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("describe")

    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--project-dir", required=True, type=_path)
    create_parser.add_argument("--project-id", required=True)
    create_parser.add_argument("--title", required=True)
    create_parser.add_argument(
        "--template",
        default=CANVAS_STARTER_TEMPLATE,
    )
    create_parser.add_argument(
        "--project-schema",
        type=_path,
        default=DEFAULT_PROJECT_SCHEMA,
    )
    create_parser.add_argument("--host-profile", type=_path)
    create_parser.add_argument(
        "--host-profile-schema",
        type=_path,
        default=DEFAULT_HOST_PROFILE_SCHEMA,
    )

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--project-dir", required=True, type=_path)
    validate_parser.add_argument(
        "--project-schema",
        type=_path,
        default=DEFAULT_PROJECT_SCHEMA,
    )
    validate_parser.add_argument("--host-profile", type=_path)
    validate_parser.add_argument(
        "--host-profile-schema",
        type=_path,
        default=DEFAULT_HOST_PROFILE_SCHEMA,
    )

    build_parser = subparsers.add_parser("build")
    build_parser.add_argument("--project-dir", required=True, type=_path)
    build_parser.add_argument("--output-dir", required=True, type=_path)
    build_parser.add_argument(
        "--project-schema",
        type=_path,
        default=DEFAULT_PROJECT_SCHEMA,
    )
    build_parser.add_argument(
        "--bundle-schema",
        type=_path,
        default=DEFAULT_BUNDLE_SCHEMA,
    )
    build_parser.add_argument("--host-profile", type=_path)
    build_parser.add_argument(
        "--host-profile-schema",
        type=_path,
        default=DEFAULT_HOST_PROFILE_SCHEMA,
    )

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--bundle-dir", required=True, type=_path)
    verify_parser.add_argument(
        "--bundle-schema",
        type=_path,
        default=DEFAULT_BUNDLE_SCHEMA,
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "describe":
            result = describe_builder()
        elif args.command == "create":
            result = create_project(
                args.project_dir,
                project_id=args.project_id,
                title=args.title,
                template_id=args.template,
                project_schema=args.project_schema,
                host_profile=args.host_profile,
                host_profile_schema=args.host_profile_schema,
            ).metadata()
        elif args.command == "validate":
            result = validate_project(
                args.project_dir,
                project_schema=args.project_schema,
                host_profile=args.host_profile,
                host_profile_schema=args.host_profile_schema,
            ).metadata()
        elif args.command == "build":
            result = build_bundle(
                args.project_dir,
                args.output_dir,
                project_schema=args.project_schema,
                bundle_schema=args.bundle_schema,
                host_profile=args.host_profile,
                host_profile_schema=args.host_profile_schema,
            ).metadata()
        elif args.command == "verify":
            verified = verify_bundle(
                args.bundle_dir,
                bundle_schema=args.bundle_schema,
            )
            result = {
                **verified.metadata(),
                "schema_version": "xcp-creative-bundle-verification-v1",
            }
        else:
            raise AssertionError(f"unsupported command {args.command}")
    except CreativeProjectError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
