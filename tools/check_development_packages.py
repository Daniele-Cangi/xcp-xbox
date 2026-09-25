"""Validate the generated development package graph without Xbox hardware."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import xml.etree.ElementTree as ET
import zipfile

WORKER = "XCP.Development.Worker"
CAPSULE = "XCP.Development.CpuCapsule.Framework"
PUBLISHER = "CN=LocalDev"
VCLIBS = "Microsoft.VCLibs.140.00"
HISTORICAL_WORKER_SHA256 = "0b7bf086f2423caefae876239a4ea394afa10346f15b0bf564469aed5d08d06c"


def elements(root: ET.Element, name: str) -> list[ET.Element]:
    return [item for item in root.iter() if item.tag.rsplit("}", 1)[-1] == name]


def inspect(path: pathlib.Path, signed: bool) -> tuple[ET.Element, set[str]]:
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError(f"corrupt package: {path.name}")
        names = set(archive.namelist())
        root = ET.fromstring(archive.read("AppxManifest.xml"))
    if ("AppxSignature.p7x" in names) != signed:
        raise ValueError(f"package signing state differs: {path.name}")
    return root, names


def check(worker: pathlib.Path, capsule: pathlib.Path, signed: bool = True) -> dict[str, object]:
    wroot, wfiles = inspect(worker, signed)
    croot, cfiles = inspect(capsule, signed)
    wid = elements(wroot, "Identity")
    cid = elements(croot, "Identity")
    if len(wid) != 1 or len(cid) != 1:
        raise ValueError("package identity missing or duplicated")
    w, c = wid[0], cid[0]
    if (w.get("Name") != WORKER or c.get("Name") != CAPSULE
            or w.get("Publisher") != PUBLISHER or c.get("Publisher") != PUBLISHER
            or w.get("ProcessorArchitecture") != "x64" or c.get("ProcessorArchitecture") != "x64"):
        raise ValueError("development package identity differs")
    if hashlib.sha256(worker.read_bytes()).hexdigest() == HISTORICAL_WORKER_SHA256:
        raise ValueError("historical signed worker was substituted")
    dependencies = elements(wroot, "PackageDependency")
    capsule_deps = [item for item in dependencies if item.get("Name") == CAPSULE]
    vclibs_deps = [item for item in dependencies if item.get("Name") == VCLIBS]
    if (len(capsule_deps) != 1 or capsule_deps[0].get("Publisher") != PUBLISHER
            or capsule_deps[0].get("MinVersion") != c.get("Version")
            or capsule_deps[0].get("MaxMajorVersionTested") != "1"
            or len(vclibs_deps) != 1 or vclibs_deps[0].get("MinVersion") != "14.0.33519.0"
            or len(dependencies) != 2):
        raise ValueError("development worker dependency graph differs")
    if not any(item.tag.rsplit("}", 1)[-1] == "Framework" and item.text == "true" for item in croot.iter()):
        raise ValueError("CPU Capsule is not a framework package")
    if not {"XComputeProbe.exe", "XComputeNativeModule.dll", "XComputeTopologyBroker.dll",
            "XComputeTopologyBroker.winmd"}.issubset(wfiles):
        raise ValueError("worker package omits native runtime components")
    if "XComputeCpuCapsuleV1.dll" not in cfiles:
        raise ValueError("CPU Capsule package omits its DLL")
    return {"ok": True, "hardware_validation": "NOT_TESTED_ON_XBOX",
            "worker": {"name": WORKER, "version": w.get("Version"), "sha256": hashlib.sha256(worker.read_bytes()).hexdigest()},
            "capsule": {"name": CAPSULE, "version": c.get("Version"), "sha256": hashlib.sha256(capsule.read_bytes()).hexdigest()},
            "vclibs": {"name": VCLIBS, "min_version": "14.0.33519.0", "redistributed": False}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker-package", type=pathlib.Path, required=True)
    parser.add_argument("--capsule-package", type=pathlib.Path, required=True)
    parser.add_argument("--unsigned", action="store_true")
    args = parser.parse_args()
    try:
        result = check(args.worker_package, args.capsule_package, not args.unsigned)
    except (OSError, ValueError, KeyError, ET.ParseError, zipfile.BadZipFile) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
