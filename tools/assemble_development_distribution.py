"""Assemble a self-describing PC/Xbox development ZIP from XCP-built inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import zipfile

from check_development_packages import check as check_packages
from export_reference_toolchain import verify_kit
from inspect_reference_vclibs import inspect as inspect_vclibs
from distribution_paths import contained
from write_distribution_metadata import write as write_metadata


ROOT = pathlib.Path(__file__).resolve().parents[1]
DOCS = ("ARCHITECTURE.md", "CLAIM_MODEL.md", "COMMUNITY_XBOX_REPRODUCTION.md",
        "CONFORMANCE.md", "COPILOT_REVIEW_TRIAGE.md", "DEVELOPMENT_DISTRIBUTION.md",
        "EXTRACTION_ANALYSIS.md", "PROVENANCE.md", "RECONSTRUCTION_AUDIT.md",
        "REFERENCE_STUDIO.md", "REFERENCE_TOOLCHAIN.md", "REFERENCE_WORKER.md",
        "REFERENCE_XVM.md", "WINDOWS_SDK_NET_REDISTRIBUTION.md")


def digest(path: pathlib.Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def verified_external_vclibs(vclibs: pathlib.Path) -> dict[str, object]:
    package = vclibs.resolve(strict=True)
    before = digest(package)
    external = inspect_vclibs(package)
    if not external["ok"] or external["redistributed"] or external["sha256"] != before:
        raise ValueError("VCLibs is not an admitted external package with stable bytes")
    signature = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-File",
         str(ROOT / "tools/verify_vclibs_authenticode.ps1"), "-PackagePath", str(package)],
        capture_output=True, text=True, timeout=60, check=False)
    after = digest(package)
    if before != after:
        raise ValueError("VCLibs package changed during Authenticode verification")
    if signature.returncode != 0 or signature.stdout.strip() != "Valid":
        raise ValueError("VCLibs Authenticode signature is not Valid")
    external["signature_cryptographically_verified"] = True
    external["verification"] = "Windows Get-AuthenticodeSignature Status=Valid during assembly; SHA-256 unchanged"
    return external


def assemble(studio: pathlib.Path, signed: pathlib.Path, vclibs: pathlib.Path,
             output: pathlib.Path, archive: pathlib.Path,
             build_tools: dict[str, object] | None = None) -> dict[str, object]:
    if output.exists() or archive.exists() or not output.parent.is_dir() or not archive.parent.is_dir():
        raise ValueError("distribution folder and ZIP must be fresh paths with existing parents")
    studio_manifest = json.loads((studio / "xcp-studio-dev-build-manifest.json").read_text(encoding="utf-8"))
    if studio_manifest.get("hardware_validation") != "NOT_TESTED_ON_XBOX":
        raise ValueError("Studio build validation status differs")
    verify_kit(studio / "toolchain", "public-studio-1.9.1")
    signed_manifest = json.loads((signed / "xcp-worker-development-packages.json").read_text(encoding="utf-8"))
    if signed_manifest.get("schema_version") != "xcp-worker-development-packages-v2":
        raise ValueError("development package manifest missing")
    worker = contained(signed, signed_manifest["worker"]["file"], filename=True)
    capsule = contained(signed, signed_manifest["capsule"]["file"], filename=True)
    package_graph = check_packages(worker, capsule)
    for role, path in (("worker", worker), ("capsule", capsule)):
        if digest(path) != signed_manifest[role]["sha256"]:
            raise ValueError(f"signed {role} package hash differs")
    if digest(signed / "XCP-Development-Public.cer") != signed_manifest["public_certificate_sha256"]:
        raise ValueError("public signing certificate hash differs")
    external = verified_external_vclibs(vclibs)

    shutil.copytree(studio, output)
    packages = output / "packages"
    packages.mkdir()
    for path in (worker, capsule, signed / "XCP-Development-Public.cer",
                 signed / "xcp-worker-development-packages.json"):
        shutil.copy2(path, packages / path.name)
    example = ROOT / "reference/xcompute-probe/samples/creative/hello-shapes"
    shutil.copytree(example, output / "examples/hello-shapes")
    docs = output / "docs"
    docs.mkdir()
    copied_docs = []
    for name in DOCS:
        path = ROOT / "docs" / name
        if not path.is_file():
            raise ValueError(f"required development documentation missing: {name}")
        shutil.copy2(path, docs / name)
        copied_docs.append(name)
    provenance = output / "provenance"
    provenance.mkdir()
    for name in ("reconstruction-coverage.json", "reference-extraction-manifest.json"):
        shutil.copy2(ROOT / "provenance" / name, provenance / name)
    snapshot_manifest = ROOT / "provenance/source-snapshot-manifest.json"
    if snapshot_manifest.is_file():
        shutil.copy2(snapshot_manifest, provenance / snapshot_manifest.name)
    verify = output / "verification"
    verify.mkdir()
    for name in ("StudioDistributionProbe.cs", "check_development_packages.py",
                 "verify_development_distribution.py", "distribution_paths.py"):
        shutil.copy2(ROOT / "tools" / name, verify / name)
    (packages / "external-vclibs.json").write_text(
        json.dumps(external, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")

    metadata = write_metadata(studio, output, external, build_tools or {})

    files = []
    for path in sorted(output.rglob("*")):
        if path.is_symlink():
            raise ValueError("linked file in development distribution")
        if path.is_file():
            files.append({"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size,
                          "sha256": digest(path)})
    manifest = {
        "schema_version": "xcp-development-distribution-v1",
        "hardware_validation": "NOT_TESTED_ON_XBOX",
        "binary_publication_approved": False,
        "historical_reference": "P4/r10; P5/r12 review-hardened, not hardware-closed",
        "studio": studio_manifest,
        "native_packages": package_graph,
        "external_vclibs": external,
        "example": "examples/hello-shapes",
        "documentation": copied_docs,
        "build_provenance": "build-provenance.json",
        "third_party_notices": "THIRD_PARTY_NOTICES.md",
        "first_party_notice": "NOTICE",
        "licensing": metadata,
        "files": files,
    }
    (output / "xcp-development-distribution.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6,
                         allowZip64=True) as zipped:
        for path in sorted(output.rglob("*")):
            if path.is_file():
                item = zipfile.ZipInfo(path.relative_to(output).as_posix(), date_time=(2026, 1, 1, 0, 0, 0))
                item.compress_type = zipfile.ZIP_DEFLATED
                zipped.writestr(item, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    return {"ok": True, "output": str(output), "archive": str(archive), "archive_sha256": digest(archive),
            "file_count": len(files) + 1, "hardware_validation": "NOT_TESTED_ON_XBOX"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--studio", type=pathlib.Path, required=True)
    parser.add_argument("--signed", type=pathlib.Path, required=True)
    parser.add_argument("--vclibs-package", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--build-tools", type=pathlib.Path)
    args = parser.parse_args()
    try:
        build_tools = json.loads(args.build_tools.read_text(encoding="utf-8")) if args.build_tools else {}
        result = assemble(args.studio, args.signed, args.vclibs_package,
                          args.output, args.archive, build_tools)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
