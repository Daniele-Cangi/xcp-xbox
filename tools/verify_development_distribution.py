"""Extract a development ZIP afresh and exercise its PC Studio workflow."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile
import xml.sax.saxutils
import zipfile

from check_development_packages import check as check_packages
from distribution_paths import contained

REQUIRED_DOCS = {"ARCHITECTURE.md", "CLAIM_MODEL.md", "COMMUNITY_XBOX_REPRODUCTION.md",
                 "CONFORMANCE.md", "COPILOT_REVIEW_TRIAGE.md", "DEVELOPMENT_DISTRIBUTION.md",
                 "EXTRACTION_ANALYSIS.md", "PROVENANCE.md", "RECONSTRUCTION_AUDIT.md",
                 "REFERENCE_STUDIO.md", "REFERENCE_TOOLCHAIN.md", "REFERENCE_WORKER.md",
                 "REFERENCE_XVM.md", "WINDOWS_SDK_NET_REDISTRIBUTION.md"}


def digest(path: pathlib.Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def checked_members(archive: zipfile.ZipFile) -> None:
    seen: set[str] = set()
    for item in archive.infolist():
        path = pathlib.PurePosixPath(item.filename)
        folded = item.filename.casefold()
        if (path.is_absolute() or "\\" in item.filename or ":" in item.filename
                or any(part in ("", ".", "..") for part in path.parts)
                or folded in seen or item.is_dir()):
            raise ValueError(f"unsafe or duplicate ZIP member: {item.filename}")
        seen.add(folded)


def verify_files(root: pathlib.Path) -> dict[str, object]:
    manifest = json.loads((root / "xcp-development-distribution.json").read_text(encoding="utf-8"))
    if (manifest.get("schema_version") != "xcp-development-distribution-v1"
            or manifest.get("hardware_validation") != "NOT_TESTED_ON_XBOX"
            or manifest.get("binary_publication_approved") is not False):
        raise ValueError("development distribution manifest differs")
    expected = {item["path"]: item for item in manifest["files"]}
    if len(expected) != len(manifest["files"]):
        raise ValueError("duplicate file in distribution manifest")
    for name in expected:
        contained(root, name)
    actual = {path.relative_to(root).as_posix(): path for path in root.rglob("*") if path.is_file()}
    if set(actual) != set(expected) | {"xcp-development-distribution.json"}:
        raise ValueError("distribution file set differs from manifest")
    for name, item in expected.items():
        path = actual[name]
        if path.is_symlink() or path.stat().st_size != item["bytes"] or digest(path) != item["sha256"]:
            raise ValueError(f"distribution file differs: {name}")
    for folder, filename in (("toolchain", "xcp-agent-kit-manifest.json"),
                             ("runtime/python", "xcp-studio-python-runtime-manifest.json")):
        base = root / folder
        embedded = json.loads((base / filename).read_text(encoding="utf-8"))
        for item in embedded["files"]:
            path = contained(base, item["path"])
            if not path.is_file() or path.stat().st_size != item["bytes"] or digest(path) != item["sha256"]:
                raise ValueError(f"{folder} locked file differs: {item['path']}")
    packages = root / "packages"
    signed = json.loads((packages / "xcp-worker-development-packages.json").read_text(encoding="utf-8"))
    graph = check_packages(contained(packages, signed["worker"]["file"], filename=True),
                           contained(packages, signed["capsule"]["file"], filename=True))
    for role in ("worker", "capsule"):
        if graph[role]["sha256"] != signed[role]["sha256"]:
            raise ValueError(f"signed {role} package digest differs")
    external = json.loads((packages / "external-vclibs.json").read_text(encoding="utf-8"))
    if (external.get("name") != "Microsoft.VCLibs.140.00"
            or external.get("redistributed") is not False
            or external.get("signature_cryptographically_verified") is not True):
        raise ValueError("external VCLibs verification record differs")
    if not (root / "XComputeControlCenter.exe").is_file():
        raise ValueError("Studio executable missing")
    studio = manifest["studio"]["studio"]
    if (digest(root / "XComputeControlCenter.exe") != studio["exe_sha256"]
            or digest(root / "XComputeControlCenter.pri") != studio["pri_sha256"]
            or len(list(root.rglob("*.xbf"))) != studio["xbf_count"]
            or studio["xbf_count"] < 10):
        raise ValueError("extracted WinUI executable or compiled resources differ")
    if set(manifest["documentation"]) != REQUIRED_DOCS:
        raise ValueError("development documentation set differs")
    if (manifest.get("build_provenance") != "build-provenance.json" or
            manifest.get("third_party_notices") != "THIRD_PARTY_NOTICES.md" or
            manifest.get("first_party_notice") != "NOTICE"):
        raise ValueError("distribution provenance or notices missing")
    provenance = json.loads((root / "build-provenance.json").read_text(encoding="utf-8"))
    terms = json.loads((root / "licenses/manifest.json").read_text(encoding="utf-8"))
    if (provenance.get("schema_version") != "xcp-development-build-provenance-v1" or
            provenance.get("hardware_validation") != "NOT_TESTED_ON_XBOX" or
            provenance.get("terms_manifest_sha256") != digest(root / "licenses/manifest.json") or
            terms.get("schema_version") != "xcp-distribution-terms-v1" or
            terms.get("first_party_notice_sha256") != digest(root / "NOTICE")):
        raise ValueError("build provenance or licensing manifest differs")
    if provenance.get("source_origin") == "verified_source_snapshot":
        snapshot = root / "provenance/source-snapshot-manifest.json"
        if not snapshot.is_file() or digest(snapshot) != provenance.get("source_snapshot_manifest_sha256"):
            raise ValueError("source snapshot provenance missing")
        source_manifest = json.loads(snapshot.read_text(encoding="utf-8"))
        listed = [{"path": item["path"], "sha256": item["sha256"]} for item in source_manifest["files"]]
        if (listed != provenance["source_files"] or
                source_manifest["git_tree"] != provenance["source_git_tree"] or
                source_manifest["producer_claimed_commit"] != provenance["source_commit"]):
            raise ValueError("source snapshot provenance differs")
    elif provenance.get("source_origin") != "git_checkout":
        raise ValueError("unknown source origin")
    source_files = provenance["source_files"]
    source_serialized = json.dumps(source_files, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"
    if (hashlib.sha256(source_serialized.encode("utf-8")).hexdigest() != provenance["source_files_sha256"]
            or not source_files or len({item["path"] for item in source_files}) != len(source_files)):
        raise ValueError("build source inventory differs")
    for item in source_files:
        contained(pathlib.Path("source"), item["path"])
    if (provenance["built_inputs"]["XComputeControlCenter.exe"] != digest(root / "XComputeControlCenter.exe") or
            provenance["built_inputs"]["XComputeControlCenter.pri"] != digest(root / "XComputeControlCenter.pri") or
            provenance["built_inputs"]["XComputeControlCenter.deps.json"] != digest(root / "XComputeControlCenter.deps.json") or
            provenance["external_vclibs"]["sha256"] != external["sha256"]):
        raise ValueError("build input provenance differs from package")
    for name, expected_sha in provenance["built_inputs"].items():
        if name.startswith("packages/") and digest(contained(root, name)) != expected_sha:
            raise ValueError("signed build input provenance differs")
    if (provenance["built_inputs"]["runtime/python/xcp-studio-python-runtime-manifest.json"] !=
            digest(root / "runtime/python/xcp-studio-python-runtime-manifest.json") or
            provenance["built_inputs"]["toolchain/xcp-agent-kit-manifest.json"] !=
            digest(root / "toolchain/xcp-agent-kit-manifest.json")):
        raise ValueError("runtime or toolchain input provenance differs")
    for item in terms["python"]:
        if digest(contained(root, item["path"])) != item["sha256"]:
            raise ValueError("Python license inventory differs")
    for package in terms["dotnet"]:
        for item in package["terms"]:
            if digest(contained(root, item["path"])) != item["sha256"]:
                raise ValueError("NuGet license inventory differs")
        if package["name"] == "runtimepack.Microsoft.Windows.SDK.NET.Ref":
            evidence = package["redistribution_evidence"]
            if (package["version"] != "10.0.19041.57" or
                    evidence["official_redist_url"] != "https://learn.microsoft.com/en-us/legal/windows-sdk/redist" or
                    any(digest(root / name) != expected for name, expected in
                        evidence["unmodified_output_sha256"].items())):
                raise ValueError("Windows SDK .NET redistribution evidence differs")
    return {"file_count": len(actual), "worker": graph["worker"], "capsule": graph["capsule"],
            "vclibs_external": True, "publication_review_required": terms["publication_review_required"]}


def pc_workflow(root: pathlib.Path) -> dict[str, object]:
    python = root / "runtime/python/python.exe"
    completed = subprocess.run([str(python), "-c", "import jsonschema,sys; print(sys.version.split()[0])"],
                               capture_output=True, text=True, timeout=60, check=True)
    if completed.stdout.strip() != "3.13.15":
        raise ValueError("extracted application-local Python version differs")
    with tempfile.TemporaryDirectory(prefix="xcp-dist-probe-") as temp:
        work = pathlib.Path(temp)
        source = root / "verification/StudioDistributionProbe.cs"
        shutil.copy2(source, work / source.name)
        core = root / "XComputeControlCenter.Core.dll"
        hint = xml.sax.saxutils.escape(str(core))
        project = work / "StudioDistributionProbe.csproj"
        project.write_text(
            '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
            '<TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>'
            '<Nullable>enable</Nullable></PropertyGroup><ItemGroup><Reference Include="XComputeControlCenter.Core">'
            f'<HintPath>{hint}</HintPath></Reference></ItemGroup></Project>', encoding="utf-8")
        completed = subprocess.run(["dotnet", "run", "--project", str(project), "-c", "Release", "--",
                                    str(root), str(work / "project workspace")],
                                   capture_output=True, text=True, timeout=240, check=False)
        if completed.returncode:
            raise ValueError(f"extracted Studio Core workflow failed: {completed.stdout[-3000:]} {completed.stderr[-3000:]}")
        lines = [line for line in completed.stdout.splitlines() if line.startswith("{")]
        if not lines:
            raise ValueError("Studio Core workflow emitted no result")
        result = json.loads(lines[-1])
        if result.get("ok") is not True or len(result.get("operations", [])) != 12:
            raise ValueError("Studio Core workflow result differs")
        return result


def verify(archive: pathlib.Path, extracted: pathlib.Path) -> dict[str, object]:
    archive = archive.resolve()
    extracted = extracted.resolve()
    if extracted.exists() or not extracted.parent.is_dir():
        raise ValueError("fresh extraction directory required")
    with zipfile.ZipFile(archive) as zipped:
        checked_members(zipped)
        if zipped.testzip() is not None:
            raise ValueError("corrupt development ZIP")
        zipped.extractall(extracted)
    files = verify_files(extracted)
    workflow = pc_workflow(extracted)
    return {"ok": True, "hardware_validation": "NOT_TESTED_ON_XBOX", "archive_sha256": digest(archive),
            "extraction_verified": files, "pc_workflow": workflow}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--extract", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.archive, args.extract)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
