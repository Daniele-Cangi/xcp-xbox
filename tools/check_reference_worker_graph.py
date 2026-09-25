"""Check the frozen P4 native source and CPU Capsule package dependency graph."""

from __future__ import annotations

import argparse
import json
import pathlib
import xml.etree.ElementTree as ET
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "reference/xcompute-probe"
PROJECTS = (
    "XComputeProbe/XComputeProbe.vcxproj",
    "XComputeCpuCapsule/XComputeCpuCapsule.vcxproj",
    "XComputeProbeNativeModule/XComputeProbeNativeModule.vcxproj",
    "XComputeTopologyBroker/XComputeTopologyBroker.vcxproj",
)
FILE_ITEMS = {
    "ClCompile", "ClInclude", "FXCompile", "AppxManifest", "Image",
    "Content", "None", "ResourceCompile", "Midl", "Page", "ProjectReference",
}


def local_name(element: ET.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def children(root: ET.Element, name: str) -> list[ET.Element]:
    return [item for item in root.iter() if local_name(item) == name]


def check() -> list[str]:
    failures: list[str] = []
    source = SOURCE / "src"
    included = 0
    for relative in PROJECTS:
        project = source / relative
        if not project.is_file():
            failures.append(f"missing project: {relative}")
            continue
        tree = ET.parse(project)
        for item in tree.iter():
            include = item.get("Include")
            if local_name(item) not in FILE_ITEMS or not include:
                continue
            if any(token in include for token in ("$(", "@(", "%(", "*")):
                continue
            included += 1
            target = project.parent / include.replace("\\", "/")
            if not target.is_file():
                failures.append(f"{relative}: missing {include}")
    worker = ET.parse(source / "XComputeProbe/Package.worker-prototype-0181.appxmanifest")
    capsule = ET.parse(source / "XComputeCpuCapsule/Package.appxmanifest")
    worker_identity = children(worker.getroot(), "Identity")
    capsule_identity = children(capsule.getroot(), "Identity")
    if (len(worker_identity) != 1 or worker_identity[0].get("Name") != "XComputeProbe.WorkerPrototype"
            or worker_identity[0].get("Version") != "0.1.181.0"):
        failures.append("frozen .181 worker identity differs")
    if (len(capsule_identity) != 1
            or capsule_identity[0].get("Name") != "XComputeProbe.CpuCapsule.Framework"
            or capsule_identity[0].get("Publisher") != "CN=LocalDev"):
        failures.append("CPU Capsule framework identity differs")
    dependencies = [item for item in children(worker.getroot(), "PackageDependency")
                    if item.get("Name") == "XComputeProbe.CpuCapsule.Framework"]
    if (len(dependencies) != 1 or dependencies[0].get("Publisher") != "CN=LocalDev"
            or dependencies[0].get("MinVersion") != "1.3.0.0"):
        failures.append("worker CPU Capsule dependency is not closed at 1.3.0.0")
    build = (SOURCE / "tools/build.ps1").read_text(encoding="utf-8")
    if (build.find("$nativeExit = Invoke-MSBuildCleanEnvironment") < 0
            or build.find("$exit = Invoke-MSBuildCleanEnvironment") < 0
            or build.find("$nativeExit = Invoke-MSBuildCleanEnvironment")
            > build.find("$exit = Invoke-MSBuildCleanEnvironment")):
        failures.append("native module is not built before the worker")
    package = (SOURCE / "tools/package-cpu-capsule-package-graph-v1.ps1").read_text(encoding="utf-8")
    if "$manifest.Package.Identity.Version = $CapsuleVersion" not in package:
        failures.append("CPU Capsule build does not set its requested version")
    if included < 220:
        failures.append(f"native project include count fell to {included}")
    return failures


def package_graph(worker_path: pathlib.Path, capsule_path: pathlib.Path) -> list[str]:
    failures: list[str] = []

    def inspect(path: pathlib.Path) -> tuple[ET.Element, set[str]]:
        with zipfile.ZipFile(path) as archive:
            names = set(archive.namelist())
            manifest = ET.fromstring(archive.read("AppxManifest.xml"))
        return manifest, names

    worker, worker_files = inspect(worker_path)
    capsule, capsule_files = inspect(capsule_path)
    worker_id = children(worker, "Identity")
    capsule_id = children(capsule, "Identity")
    if (len(worker_id) != 1 or worker_id[0].get("Name") != "XComputeProbe.WorkerPrototype"
            or worker_id[0].get("Version") != "0.1.181.0"
            or worker_id[0].get("Publisher") != "CN=LocalDev"):
        failures.append("built worker package identity differs from frozen .181")
    if (len(capsule_id) != 1 or capsule_id[0].get("Name") != "XComputeProbe.CpuCapsule.Framework"
            or capsule_id[0].get("Version") != "1.3.0.0"
            or capsule_id[0].get("Publisher") != "CN=LocalDev"):
        failures.append("built CPU Capsule package identity differs from required dependency")
    deps = children(worker, "PackageDependency")
    capsule_deps = [item for item in deps if item.get("Name") == "XComputeProbe.CpuCapsule.Framework"]
    vclibs = [item for item in deps if item.get("Name") == "Microsoft.VCLibs.140.00"]
    if (len(capsule_deps) != 1 or capsule_deps[0].get("Publisher") != "CN=LocalDev"
            or capsule_deps[0].get("MinVersion") != "1.3.0.0"):
        failures.append("built worker does not bind the CPU Capsule package")
    if len(vclibs) != 1 or vclibs[0].get("MinVersion") != "14.0.33519.0":
        failures.append("built worker VCLibs dependency differs")
    for filename in ("XComputeProbe.exe", "XComputeNativeModule.dll", "XComputeTopologyBroker.dll", "XComputeTopologyBroker.winmd"):
        if filename not in worker_files:
            failures.append(f"built worker omits {filename}")
    if "XComputeCpuCapsuleV1.dll" not in capsule_files:
        failures.append("CPU Capsule package omits its module")
    if "AppxSignature.p7x" in capsule_files:
        failures.append("unsigned development capsule unexpectedly contains a signature")
    return failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker-package", type=pathlib.Path)
    parser.add_argument("--capsule-package", type=pathlib.Path)
    arguments = parser.parse_args()
    errors = check()
    if bool(arguments.worker_package) != bool(arguments.capsule_package):
        errors.append("both package paths are required for package graph validation")
    elif arguments.worker_package:
        errors.extend(package_graph(arguments.worker_package, arguments.capsule_package))
    for error in errors:
        print(error)
    print(json.dumps({"ok": not errors, "errors": len(errors)}))
    raise SystemExit(bool(errors))
