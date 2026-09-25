"""Apply an auditable package-identity substitution to a disposable P4 stage.

The checked-in reference graph stays byte-exact. This script is run only after
prepare_reference_worker.py has verified and copied that graph.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import xml.etree.ElementTree as ET


HISTORICAL_WORKER = "XComputeProbe.WorkerPrototype"
HISTORICAL_CAPSULE = "XComputeProbe.CpuCapsule.Framework"
DEVELOPMENT_WORKER = "XCP.Development.Worker"
DEVELOPMENT_CAPSULE = "XCP.Development.CpuCapsule.Framework"
PUBLISHER = "CN=LocalDev"
DEVELOPMENT_PHONE_ID = "b7d69099-b88b-4da4-a420-a95a050c0a11"
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")


def version(value: str) -> tuple[int, int, int, int]:
    if not VERSION.fullmatch(value):
        raise ValueError(f"invalid four-part package version: {value}")
    parts = tuple(int(part) for part in value.split("."))
    if any(part > 65535 for part in parts):
        raise ValueError("package version component exceeds 65535")
    return parts  # type: ignore[return-value]


def replace(path: pathlib.Path, substitutions: list[tuple[str, str, int]]) -> None:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"missing or linked staged file: {path}")
    original = path.read_bytes()
    value = original.decode("utf-8")
    for before, after, expected in substitutions:
        actual = value.count(before)
        if actual != expected:
            raise ValueError(f"staged identity anchor drift: {path.name}: {before}: {actual} != {expected}")
        value = value.replace(before, after)
    if value != original.decode("utf-8"):
        path.write_bytes(value.encode("utf-8"))


def local(root: ET.Element, name: str) -> list[ET.Element]:
    return [item for item in root.iter() if item.tag.rsplit("}", 1)[-1] == name]


def stage(root: pathlib.Path, worker_version: str, capsule_version: str,
          previous: pathlib.Path | None = None) -> dict[str, object]:
    worker_parts = version(worker_version)
    capsule_parts = version(capsule_version)
    if worker_parts <= version("0.1.181.0") or capsule_parts < version("1.3.0.0") or capsule_parts[0] != 1:
        raise ValueError("development versions must exceed worker .181 and retain capsule ABI major 1")
    if previous:
        old = json.loads(previous.read_text(encoding="utf-8"))
        if old.get("worker", {}).get("name") != DEVELOPMENT_WORKER or old.get("capsule", {}).get("name") != DEVELOPMENT_CAPSULE:
            raise ValueError("previous build is not this development package family")
        if worker_parts <= version(old["worker"]["version"]) or capsule_parts <= version(old["capsule"]["version"]):
            raise ValueError("both package versions must increase for a development update")

    source = root / "src"
    worker_dir = source / "XComputeProbe"
    capsule_dir = source / "XComputeCpuCapsule"
    manifests = (worker_dir / "Package.worker-prototype-0181.appxmanifest", worker_dir / "Package.appxmanifest")
    historical_phone = "d1e0de7c-d7b1-4e6b-89f5-0acb0829f7db"
    for path in manifests:
        replace(path, [(HISTORICAL_WORKER, DEVELOPMENT_WORKER, 1),
                       (HISTORICAL_CAPSULE, DEVELOPMENT_CAPSULE, 1),
                       ("0.1.181.0", worker_version, 1),
                       ("1.3.0.0", capsule_version, 1),
                       (historical_phone, DEVELOPMENT_PHONE_ID, 1),
                       ("<DisplayName>XCP WORKER</DisplayName>", "<DisplayName>XCP DEVELOPMENT WORKER</DisplayName>", 1),
                       ('DisplayName="XCP WORKER"', 'DisplayName="XCP DEVELOPMENT WORKER"', 1)])
    replace(capsule_dir / "Package.appxmanifest", [(HISTORICAL_CAPSULE, DEVELOPMENT_CAPSULE, 1),
                                                     ('Version="1.0.0.0"', f'Version="{capsule_version}"', 1)])
    runtime = worker_dir / "runtime"
    for name in ("WorkerCpuCapsuleRuntime.cpp", "WorkerCpuCapsuleModule.cpp"):
        replace(runtime / name, [(f'L"{HISTORICAL_CAPSULE}"', f'L"{DEVELOPMENT_CAPSULE}"', 1)])
    packed_old = "(1ull << 48) | (3ull << 32)"
    packed_new = " | ".join(f"({part}ull << {shift})" for part, shift in zip(capsule_parts, (48, 32, 16, 0)))
    for name in ("WorkerCpuGpuConvergenceBackend.cpp", "WorkerXvmCpuCapsuleBackend.cpp"):
        path = runtime / name
        replace(path, [(packed_old, packed_new, 1), ("1.3.0.0", capsule_version, 2)])
    replace(runtime / "WorkerRuntimeDescription.cpp", [("1.3.0.0", capsule_version, 3)])
    replace(root / "tools/build.ps1", [('"/m",', '"/m:1",', 2),
                                       ('"/restore",', '"/restore",\n    "/p:MultiProcessorCompilation=false",', 2)])

    worker = ET.parse(manifests[0]).getroot()
    capsule = ET.parse(capsule_dir / "Package.appxmanifest").getroot()
    worker_id, capsule_id = local(worker, "Identity"), local(capsule, "Identity")
    dependency = [item for item in local(worker, "PackageDependency") if item.get("Name") == DEVELOPMENT_CAPSULE]
    if (len(worker_id) != 1 or len(capsule_id) != 1 or len(dependency) != 1
            or worker_id[0].attrib != {"Name": DEVELOPMENT_WORKER, "Publisher": PUBLISHER,
                                       "Version": worker_version, "ProcessorArchitecture": "x64"}
            or capsule_id[0].attrib != {"Name": DEVELOPMENT_CAPSULE, "Publisher": PUBLISHER,
                                        "Version": capsule_version, "ProcessorArchitecture": "x64"}
            or dependency[0].get("Publisher") != PUBLISHER
            or dependency[0].get("MinVersion") != capsule_version):
        raise ValueError("staged package identity/dependency graph differs")
    return {"schema_version": "xcp-development-identity-stage-v1", "hardware_validation": "NOT_TESTED_ON_XBOX",
            "reference_source": "P4/r10", "publisher": PUBLISHER,
            "worker": {"name": DEVELOPMENT_WORKER, "version": worker_version},
            "capsule": {"name": DEVELOPMENT_CAPSULE, "version": capsule_version},
            "staged_transformations": "package identity, exact Capsule version constants and build parallelism only"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    parser.add_argument("--worker-version", default="0.1.182.0")
    parser.add_argument("--capsule-version", default="1.3.0.0")
    parser.add_argument("--previous-manifest", type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = stage(args.stage, args.worker_version, args.capsule_version, args.previous_manifest)
    except (OSError, ValueError, KeyError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps({"ok": True, **result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
