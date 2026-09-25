#!/usr/bin/env python3
"""Build and verify the exact private Python runtime used by XCP Studio."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = (
    ROOT / "profiles" / "studio" / "xcp-studio-python-runtime-v1.json"
)
MANIFEST_NAME = "xcp-studio-python-runtime-manifest.json"


class RuntimeBuildError(RuntimeError):
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
            "stage": "studio_python_runtime",
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


def canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _load_profile(path: pathlib.Path) -> dict[str, Any]:
    try:
        profile = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_profile_invalid",
            "The Python runtime build profile is missing or invalid.",
            field="profile",
            expected="strict UTF-8 JSON profile",
            actual=type(exc).__name__,
            correction="restore the canonical runtime build profile",
        ) from exc
    required = {
        "schema_version": "xcp-studio-python-runtime-build-profile-v1",
        "runtime_id": "xcp.studio.python",
        "architecture": "x64",
    }
    for field, expected in required.items():
        actual = profile.get(field)
        if actual != expected:
            raise RuntimeBuildError(
                "xcp.studio.python_runtime_profile_invalid",
                "The Python runtime build profile has an invalid identity.",
                field=field,
                expected=expected,
                actual=str(actual),
                correction="restore the canonical runtime build profile",
            )
    return profile


def _download_exact(
    entry: dict[str, Any],
    cache: pathlib.Path,
) -> pathlib.Path:
    name = str(entry.get("filename") or entry.get("name") or "")
    url = str(entry.get("url") or "")
    expected = str(entry.get("sha256") or "")
    if not name or not url.startswith("https://") or len(expected) != 64:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_source_invalid",
            "A pinned runtime input is incomplete.",
            field=name or "source",
            expected="HTTPS URL and SHA-256",
            actual="invalid profile entry",
            correction="repair the canonical runtime build profile",
        )
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / name
    if target.exists() and sha256_file(target) == expected:
        return target
    if target.exists():
        target.unlink()
    temporary = target.with_suffix(target.suffix + f".part-{os.getpid()}")
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "XCP-Studio-Runtime-Builder/1"},
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            with temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
        actual = sha256_file(temporary)
        if actual != expected:
            raise RuntimeBuildError(
                "xcp.studio.python_runtime_source_hash_mismatch",
                "A downloaded runtime input failed its pinned hash.",
                field=name,
                expected=expected,
                actual=actual,
                correction="discard the input and verify its upstream source",
            )
        os.replace(temporary, target)
    finally:
        if temporary.exists():
            temporary.unlink()
    return target


def _safe_member_path(root: pathlib.Path, name: str) -> pathlib.Path:
    pure = pathlib.PurePosixPath(name)
    if pure.is_absolute() or not pure.parts or ".." in pure.parts:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_archive_path_invalid",
            "A runtime archive contains an escaping path.",
            field="archive_member",
            expected="relative contained path",
            actual=name,
            correction="reject the archive and restore pinned upstream bytes",
        )
    target = (root / pure).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError as exc:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_archive_path_invalid",
            "A runtime archive contains an escaping path.",
            field="archive_member",
            expected="relative contained path",
            actual=name,
            correction="reject the archive and restore pinned upstream bytes",
        ) from exc
    return target


def _extract_zip(
    archive: pathlib.Path,
    root: pathlib.Path,
    *,
    prefix: pathlib.PurePosixPath | None = None,
    seen: set[str],
) -> None:
    try:
        source = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile) as exc:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_archive_invalid",
            "A pinned runtime archive is not a valid ZIP file.",
            field=archive.name,
            expected="valid ZIP archive",
            actual=type(exc).__name__,
            correction="restore the exact pinned archive",
        ) from exc
    with source:
        for member in source.infolist():
            if member.is_dir():
                continue
            unix_type = (member.external_attr >> 16) & 0o170000
            if unix_type == 0o120000:
                raise RuntimeBuildError(
                    "xcp.studio.python_runtime_archive_type_invalid",
                    "A runtime archive contains a symbolic link.",
                    field=archive.name,
                    expected="regular files only",
                    actual=member.filename,
                    correction="reject the archive",
                )
            relative = pathlib.PurePosixPath(member.filename)
            if prefix is not None:
                relative = prefix / relative
            target = _safe_member_path(root, relative.as_posix())
            identity = relative.as_posix().casefold()
            if identity in seen:
                raise RuntimeBuildError(
                    "xcp.studio.python_runtime_archive_duplicate",
                    "Runtime archives contain a duplicate output path.",
                    field="archive_member",
                    expected="unique case-insensitive paths",
                    actual=relative.as_posix(),
                    correction="repair the pinned runtime inputs",
                )
            seen.add(identity)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(member) as input_stream:
                with target.open("wb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream)


def _configure_embedded_runtime(root: pathlib.Path) -> None:
    candidates = sorted(root.glob("python*._pth"))
    if len(candidates) != 1:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_pth_invalid",
            "The embedded runtime does not contain one exact _pth file.",
            field="python._pth",
            expected="one file",
            actual=str(len(candidates)),
            correction="restore the pinned CPython embedded archive",
        )
    original = [
        line.strip()
        for line in candidates[0].read_text(encoding="utf-8").splitlines()
        if line.strip() and line.strip() not in {"#import site", "import site"}
    ]
    if "Lib/site-packages" not in original:
        original.append("Lib/site-packages")
    original.append("import site")
    candidates[0].write_text(
        "\n".join(original) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _runtime_environment() -> dict[str, str]:
    environment = dict(os.environ)
    for name in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP"):
        environment.pop(name, None)
    environment["PYTHONNOUSERSITE"] = "1"
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["PYTHONUTF8"] = "1"
    return environment


def _smoke_runtime(
    root: pathlib.Path,
    expected_python: str,
    expected_packages: dict[str, str],
) -> dict[str, Any]:
    executable = root / "python.exe"
    code = (
        "import importlib.metadata,json,site,sys;"
        "names=" + repr(sorted(expected_packages)) + ";"
        "print(json.dumps({'python':'.'.join(map(str,sys.version_info[:3])),"
        "'packages':{n:importlib.metadata.version(n) for n in names},"
        "'user_site':bool(site.ENABLE_USER_SITE)}))"
    )
    completed = subprocess.run(
        [str(executable), "-I", "-c", code],
        cwd=root,
        env=_runtime_environment(),
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=60,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_smoke_failed",
            "The private Python runtime failed its isolated import smoke.",
            field="python.exe",
            expected="exit 0 and exact package versions",
            actual=completed.stderr.strip() or str(completed.returncode),
            correction="rebuild the runtime from the pinned inputs",
        )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_smoke_invalid",
            "The runtime smoke returned invalid JSON.",
            field="stdout",
            expected="one JSON document",
            actual=completed.stdout[:200],
            correction="repair the private runtime configuration",
        ) from exc
    if (
        result.get("python") != expected_python
        or result.get("packages") != expected_packages
        or result.get("user_site") is not False
    ):
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_identity_mismatch",
            "The runtime smoke identity differs from the pinned profile.",
            field="runtime_identity",
            expected=json.dumps(
                {
                    "python": expected_python,
                    "packages": expected_packages,
                    "user_site": False,
                },
                sort_keys=True,
            ),
            actual=json.dumps(result, sort_keys=True),
            correction="reject the runtime and rebuild exact bytes",
        )
    return result


def _file_entries(root: pathlib.Path) -> list[dict[str, Any]]:
    entries = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        if path.name == MANIFEST_NAME:
            continue
        entries.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return entries


def build_runtime(
    output: pathlib.Path,
    profile_path: pathlib.Path,
    cache: pathlib.Path,
) -> dict[str, Any]:
    output = output.resolve()
    if output.exists():
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_output_exists",
            "The runtime builder will not merge with an existing directory.",
            field="output",
            expected="new directory",
            actual=str(output),
            correction="select a fresh output path",
        )
    profile_path = profile_path.resolve()
    profile = _load_profile(profile_path)
    staging = output.with_name(f".{output.name}.tmp-{os.getpid()}")
    if staging.exists():
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_staging_exists",
            "The runtime staging directory already exists.",
            field="staging",
            expected="fresh directory",
            actual=str(staging),
            correction="inspect and remove the stale staging directory",
        )
    seen: set[str] = set()
    try:
        archive_entry = dict(profile["source_archive"])
        archive_entry["filename"] = archive_entry["name"]
        archive = _download_exact(archive_entry, cache.resolve())
        staging.mkdir(parents=True)
        _extract_zip(archive, staging, seen=seen)
        site_packages = pathlib.PurePosixPath("Lib/site-packages")
        package_records = []
        expected_packages: dict[str, str] = {}
        for package in profile["packages"]:
            wheel = _download_exact(package, cache.resolve())
            _extract_zip(
                wheel,
                staging,
                prefix=site_packages,
                seen=seen,
            )
            name = str(package["name"])
            version = str(package["version"])
            expected_packages[name] = version
            package_records.append(
                {
                    "name": name,
                    "version": version,
                    "filename": str(package["filename"]),
                    "sha256": str(package["sha256"]),
                    "url": str(package["url"]),
                }
            )
        _configure_embedded_runtime(staging)
        smoke = _smoke_runtime(
            staging,
            str(profile["runtime_version"]),
            expected_packages,
        )
        entries = _file_entries(staging)
        manifest = {
            "schema_version": "xcp-studio-python-runtime-manifest-v1",
            "runtime_id": str(profile["runtime_id"]),
            "runtime_version": str(profile["runtime_version"]),
            "architecture": str(profile["architecture"]),
            "executable": str(profile["executable"]),
            "profile_sha256": sha256_file(profile_path),
            "source_archive": {
                "name": str(profile["source_archive"]["name"]),
                "sha256": str(profile["source_archive"]["sha256"]),
                "url": str(profile["source_archive"]["url"]),
            },
            "packages": package_records,
            "security": dict(profile["security"]),
            "smoke": smoke,
            "files": entries,
        }
        (staging / MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        os.replace(staging, output)
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise
    return verify_runtime(output)


def verify_runtime(root: pathlib.Path) -> dict[str, Any]:
    root = root.resolve()
    manifest_path = root / MANIFEST_NAME
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_manifest_invalid",
            "The runtime manifest is missing or invalid.",
            field="manifest",
            expected="strict UTF-8 JSON manifest",
            actual=type(exc).__name__,
            correction="build a fresh exact runtime",
        ) from exc
    if manifest.get("schema_version") != (
        "xcp-studio-python-runtime-manifest-v1"
    ):
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_manifest_invalid",
            "The runtime manifest schema is invalid.",
            field="schema_version",
            expected="xcp-studio-python-runtime-manifest-v1",
            actual=str(manifest.get("schema_version")),
            correction="build a fresh exact runtime",
        )
    expected_paths = {str(item["path"]) for item in manifest.get("files", [])}
    actual_paths = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name != MANIFEST_NAME
    }
    if actual_paths != expected_paths:
        raise RuntimeBuildError(
            "xcp.studio.python_runtime_file_set_mismatch",
            "The runtime file set differs from its manifest.",
            field="files",
            expected=",".join(sorted(expected_paths)),
            actual=",".join(sorted(actual_paths)),
            correction="reject the runtime and build exact bytes again",
        )
    for entry in manifest["files"]:
        path = root / pathlib.PurePosixPath(str(entry["path"]))
        if path.is_symlink() or path.stat().st_size != int(entry["bytes"]):
            raise RuntimeBuildError(
                "xcp.studio.python_runtime_file_identity_mismatch",
                "A runtime file has an invalid type or byte length.",
                field=str(entry["path"]),
                expected=str(entry["bytes"]),
                actual=(
                    "symlink" if path.is_symlink() else str(path.stat().st_size)
                ),
                correction="reject the modified runtime",
            )
        actual = sha256_file(path)
        if actual != entry["sha256"]:
            raise RuntimeBuildError(
                "xcp.studio.python_runtime_file_identity_mismatch",
                "A runtime file hash differs from the manifest.",
                field=str(entry["path"]),
                expected=str(entry["sha256"]),
                actual=actual,
                correction="reject the modified runtime",
            )
    expected_packages = {
        str(item["name"]): str(item["version"])
        for item in manifest["packages"]
    }
    smoke = _smoke_runtime(
        root,
        str(manifest["runtime_version"]),
        expected_packages,
    )
    return {
        "ok": True,
        "schema_version": "xcp-studio-python-runtime-verification-v1",
        "root": str(root),
        "runtime_version": str(manifest["runtime_version"]),
        "architecture": str(manifest["architecture"]),
        "manifest_sha256": sha256_file(manifest_path),
        "file_count": len(manifest["files"]),
        "total_bytes": sum(int(item["bytes"]) for item in manifest["files"]),
        "smoke": smoke,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build or verify the private XCP Studio Python runtime."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("--output", type=pathlib.Path, required=True)
    build.add_argument("--profile", type=pathlib.Path, default=DEFAULT_PROFILE)
    build.add_argument(
        "--cache",
        type=pathlib.Path,
        default=pathlib.Path.home() / ".cache" / "xcp-studio-runtime",
    )
    verify = subparsers.add_parser("verify")
    verify.add_argument("--runtime", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = (
            build_runtime(args.output, args.profile, args.cache)
            if args.command == "build"
            else verify_runtime(args.runtime)
        )
    except RuntimeBuildError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
