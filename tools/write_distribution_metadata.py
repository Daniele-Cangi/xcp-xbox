"""Record source/build inputs and exact third-party terms in a development folder."""

from __future__ import annotations

import hashlib
import json
import pathlib
import shutil
import subprocess
import xml.etree.ElementTree as ET

from source_snapshot import MANIFEST as SNAPSHOT_MANIFEST, verify as verify_snapshot


ROOT = pathlib.Path(__file__).resolve().parents[1]


def sha(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True,
                          text=True, encoding="utf-8").stdout.strip()


def source_inventory() -> tuple[list[dict[str, str]], dict[str, object]]:
    if (ROOT / ".git").exists():
        files = []
        for name in sorted(set(git("ls-files", "--cached", "--others", "--exclude-standard").splitlines())):
            path = ROOT / name
            if not path.is_file() or path.is_symlink():
                raise ValueError(f"Tracked build source missing or linked: {name}")
            files.append({"path": pathlib.PurePosixPath(name.replace("\\", "/")).as_posix(), "sha256": sha(path)})
        origin = {"source_origin": "git_checkout", "source_commit": git("rev-parse", "HEAD"),
                  "source_git_tree": git("rev-parse", "HEAD^{tree}"),
                  "source_commit_verification": "local_git", "source_dirty": bool(
                      git("status", "--porcelain", "--untracked-files=all"))}
        return files, origin
    snapshot = verify_snapshot(ROOT)
    files = [{"path": item["path"], "sha256": item["sha256"]} for item in snapshot["files"]]
    origin = {"source_origin": "verified_source_snapshot",
              "source_commit": snapshot["producer_claimed_commit"],
              "source_git_tree": snapshot["git_tree"],
              "source_commit_verification": "producer_assertion; Git tree recomputed from source bytes",
              "source_snapshot_manifest_sha256": sha(ROOT / SNAPSHOT_MANIFEST),
              "source_dirty": False}
    return files, origin


def write(studio: pathlib.Path, output: pathlib.Path, external: dict[str, object],
          build_tools: dict[str, object]) -> dict[str, object]:
    licenses = output / "licenses"
    licenses.mkdir()
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    shutil.copy2(ROOT / "NOTICE", output / "NOTICE")
    shutil.copy2(studio / "runtime/python/LICENSE.txt", licenses / "CPython-LICENSE.txt")
    deps = json.loads((studio / "XComputeControlCenter.deps.json").read_text(encoding="utf-8"))
    cache = pathlib.Path.home() / ".nuget/packages"
    dependencies: list[dict[str, object]] = []
    notices = ["# Third-party notices", "", "Exact package versions come from the distributed Studio .deps.json.",
               "License and notice files are copied from those exact NuGet packages.",
               "VCLibs remains an external prerequisite and is not redistributed.", ""]
    for coordinate, item in sorted(deps["libraries"].items()):
        if item["type"] not in {"package", "runtimepack"}:
            continue
        name, version = coordinate.rsplit("/", 1)
        package_id = name.removeprefix("runtimepack.")
        directory = cache / package_id.lower() / version
        if not directory.is_dir():
            raise ValueError(f"NuGet license source missing for {coordinate}")
        record: dict[str, object] = {"name": name, "version": version, "type": item["type"], "terms": []}
        candidates = [p for p in directory.rglob("*") if p.is_file() and
                      p.name.lower() in {"license.txt", "license.md", "notice.txt", "thirdpartynotices.txt",
                                         "third-party-notices.txt", "copying"} and
                      ".nupkg" not in p.parts]
        for path in sorted(candidates):
            relative = path.relative_to(directory)
            destination = licenses / "nuget" / name / version / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            record["terms"].append({"path": destination.relative_to(output).as_posix(), "sha256": sha(destination)})
        if not record["terms"]:
            if name != "runtimepack.Microsoft.Windows.SDK.NET.Ref":
                raise ValueError(f"No NuGet license text for {coordinate}")
            nuspec = directory / "microsoft.windows.sdk.net.ref.nuspec"
            document = ET.parse(nuspec).getroot()
            license_url = next((node.text for node in document.iter() if node.tag.endswith("licenseUrl")), None)
            if not license_url:
                raise ValueError("Windows SDK targeting pack has no license reference")
            destination = licenses / "nuget" / name / version / nuspec.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(nuspec, destination)
            record["terms_url"] = license_url
            record["terms"] = [{"path": destination.relative_to(output).as_posix(), "sha256": sha(destination)}]
            if version != "10.0.19041.57" or license_url != "https://aka.ms/WinSDKLicenseURL":
                raise ValueError("Unreviewed Windows SDK .NET targeting pack version or terms URL")
            listed = {}
            for filename in ("Microsoft.Windows.SDK.NET.dll", "WinRT.Runtime.dll"):
                source_dll = directory / "lib/net8.0" / filename
                published_dll = studio / filename
                if not source_dll.is_file() or not published_dll.is_file() or sha(source_dll) != sha(published_dll):
                    raise ValueError(f"Windows SDK .NET output is not the exact NuGet binary: {filename}")
                listed[filename] = sha(published_dll)
            record["redistribution_evidence"] = {
                "official_redist_url": "https://learn.microsoft.com/en-us/legal/windows-sdk/redist",
                "official_terms_url": "https://aka.ms/WinSDKLicenseURL",
                "listed_paths": ["lib/net8.0/Microsoft.Windows.SDK.NET.dll", "lib/net8.0/WinRT.Runtime.dll"],
                "unmodified_output_sha256": listed,
                "finding": "Microsoft REDIST list explicitly includes these NuGet package files subject to SDK terms"}
        dependencies.append(record)
        notices.append(f"- `{name}/{version}`: " + ", ".join(t["path"] for t in record["terms"]))
    python_root = studio / "runtime/python"
    python_terms: list[dict[str, str]] = [{"name": "CPython", "version": "3.13.15",
                                          "path": "licenses/CPython-LICENSE.txt", "sha256": sha(licenses / "CPython-LICENSE.txt")}]
    for dist in sorted((python_root / "Lib/site-packages").glob("*.dist-info")):
        terms = [p for p in dist.rglob("*") if p.is_file() and
                 p.name.lower() in {"license", "copying", "notice", "license.txt"}]
        if not terms:
            raise ValueError(f"Python package terms missing: {dist.name}")
        for path in sorted(terms):
            relative = path.relative_to(python_root).as_posix()
            python_terms.append({"name": dist.name, "path": f"runtime/python/{relative}", "sha256": sha(path)})
    notices.extend(["", "Python runtime and wheel terms:"])
    notices.extend(f"- `{item['name']}`: {item['path']}" for item in python_terms)
    notices.extend(["", "The Windows SDK .NET targeting pack identifies its official terms in the included nuspec:",
                    "https://aka.ms/WinSDKLicenseURL", "Microsoft's REDIST list expressly includes its net8.0 DLLs:",
                    "https://learn.microsoft.com/en-us/legal/windows-sdk/redist", ""])
    (output / "THIRD_PARTY_NOTICES.md").write_text("\n".join(notices), encoding="utf-8")
    references = [name for name, item in sorted(deps["libraries"].items()) if item["type"] == "reference"]
    projects = [name for name, item in sorted(deps["libraries"].items()) if item["type"] == "project"]
    if references != ["Microsoft.Web.WebView2.Core.Projection/1.0.3179.45"]:
        raise ValueError("Unreviewed .NET reference dependency in Studio output")
    terms_manifest = {"schema_version": "xcp-distribution-terms-v1", "dotnet": dependencies,
                      "dotnet_references_covered_by_parent_package": references,
                      "first_party_projects": projects, "python": python_terms, "external_vclibs": external["name"],
                      "first_party_notice_sha256": sha(output / "NOTICE"),
                      "publication_review_required": any(p.get("publication_review_required") for p in dependencies)}
    (licenses / "manifest.json").write_text(canonical(terms_manifest), encoding="utf-8")

    tracked, source_origin = source_inventory()
    tree_hash = hashlib.sha256(canonical(tracked).encode("utf-8")).hexdigest()
    inputs = {}
    for name in ("xcp-studio-dev-build-manifest.json", "XComputeControlCenter.deps.json",
                 "XComputeControlCenter.exe", "XComputeControlCenter.pri",
                 "runtime/python/xcp-studio-python-runtime-manifest.json",
                 "toolchain/xcp-agent-kit-manifest.json"):
        inputs[name] = sha(studio / name)
    for path in sorted((output / "packages").iterdir()):
        if path.is_file():
            inputs[f"packages/{path.name}"] = sha(path)
    inputs["source/python-runtime-build-profile"] = sha(
        ROOT / "reference/xcompute-probe/profiles/studio/xcp-studio-python-runtime-v1.json")
    provenance = {"schema_version": "xcp-development-build-provenance-v1",
                  **source_origin,
                  "source_files": tracked, "source_files_sha256": tree_hash,
                  "build_recipe": {"path": "tools/build_development_distribution.ps1",
                                   "sha256": sha(ROOT / "tools/build_development_distribution.ps1")},
                  "tool_versions": build_tools, "built_inputs": inputs,
                  "external_vclibs": {"name": external["name"], "sha256": external["sha256"]},
                  "terms_manifest_sha256": sha(licenses / "manifest.json"),
                  "hardware_validation": "NOT_TESTED_ON_XBOX"}
    (output / "build-provenance.json").write_text(canonical(provenance), encoding="utf-8")
    return {"source_files": len(tracked), "dotnet_dependencies": len(dependencies),
            "python_terms": len(python_terms), "publication_review_required": terms_manifest["publication_review_required"]}
