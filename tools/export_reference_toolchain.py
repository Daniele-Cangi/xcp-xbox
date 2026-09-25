"""Export newly identified public kits from the preserved product source.

This command never invokes Git or consults the private source repository. The
historical G1/P4/P5 identities remain documented but are not reused after
operational evidence is removed from four public profiles.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "reference" / "xcompute-probe"
EXTRACTION_MANIFEST = ROOT / "provenance" / "reference-extraction-manifest.json"
SOURCE_LOCK = ROOT / "provenance" / "reference-source-lock.json"
KIT_MANIFEST = "xcp-agent-kit-manifest.json"
LIFECYCLE = "tools/xcp_agent_lifecycle.py"
EXPECTED = {
    "public-g1": ("1.8.0.1", "63bc6a745ef24f78b06762689b29dea921457d5a270c7c655ffb1b8a8aad94c7"),
    "public-studio-1.9.0": ("1.9.0.1", "3f4db5a9a671db35634c1930c84772e8a5f6b3295d2fdb5363ba6c32c1ab7e57"),
    "public-studio-1.9.1": ("1.9.1.1", "827237cb78a5546c42aa853ff13a7a6847fb8b86a8ba7bfdc1979f466acfc03d"),
}
SANITIZED_PROFILES = {
    "reference/xcompute-probe/profiles/creative/godot-adaptation-compatibility-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-agent-native-creation-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-agent-native-source-adaptation-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-project-evolution-v1.json",
}
PROFILE_TEST_ADAPTATION = "reference/xcompute-probe/tests/unit/test_xcp_project_evolve_v1.py"
PROFILE_TEST_SOURCE_SHA256 = "e620bf81c3f973e8c21189bfcdf015dfa0f30682d21838045542098eae3750fe"


class ToolchainError(RuntimeError):
    pass


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _checked_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    pure = pathlib.PurePosixPath(relative)
    if (pure.is_absolute() or not pure.parts or "\\" in relative or ":" in pure.parts[0]
            or any(part in (".", "..") for part in pure.parts)):
        raise ToolchainError(f"invalid locked path: {relative}")
    return root.joinpath(*pure.parts)


def verify_reference_sources() -> None:
    """Check every transferred source byte and reject unregistered files."""
    manifest = json.loads(EXTRACTION_MANIFEST.read_text(encoding="utf-8"))
    lock = json.loads(SOURCE_LOCK.read_text(encoding="utf-8"))
    locked = {item["path"]: item for item in
              [lock["g1_exporter"], *lock["g1_kit_source_files"]]}
    expected: set[pathlib.Path] = set()
    sanitized_seen: set[str] = set()
    for entry in manifest["components"]:
        path = _checked_path(ROOT, entry["destination_path"])
        if not path.is_file() or path.is_symlink():
            raise ToolchainError(f"missing or linked reference source: {entry['destination_path']}")
        if path.stat().st_size != entry["bytes"] or _sha256(path) != entry["destination_sha256"]:
            raise ToolchainError(f"reference source drift: {entry['destination_path']}")
        original = locked.get(entry["source_path"]) if entry["category"] == "g1_source" else None
        if original and entry["source_sha256"] != original["sha256"]:
            raise ToolchainError(f"historical lock drift: {entry['source_path']}")
        if entry["destination_path"] in SANITIZED_PROFILES:
            sanitized_seen.add(entry["destination_path"])
            if (entry["transformation"] != "PUBLIC_PROFILE_SANITIZATION"
                    or not original or entry["source_bytes"] != original["bytes"]
                    or entry["destination_sha256"] == entry["source_sha256"]):
                raise ToolchainError(f"invalid profile sanitation: {entry['destination_path']}")
            profile = json.loads(path.read_text(encoding="utf-8"))
            if (profile.get("status") != "PUBLIC_RECONSTRUCTION_AWAITING_VALIDATION"
                    or profile.get("validation") != {
                        "status": "NOT_VALIDATED_FROM_PUBLIC_BUILD",
                        "historical_operational_evidence_included": False,
                    }
                    or any(key in profile for key in ("recorded_on", "evidence"))
                    or re.search(r"\b[0-9a-f]{64}\b", path.read_text(encoding="utf-8"))):
                raise ToolchainError(f"operational evidence in public profile: {entry['destination_path']}")
        elif entry["destination_path"] == "reference/xcompute-probe/tools/assert-non-onedrive-workspace.ps1":
            if (entry["transformation"] != "MECHANICAL_RELOCATION"
                    or entry["source_sha256"] != "c15b778d7c4f85ebc6d1cd3a646bdcef8af14c326429e8325d1a6f7d9662b65d"
                    or entry["destination_sha256"] != "d40774272f23edf5ef2260b7b17954471f6369366c455f4dfc672e97882d898d"):
                raise ToolchainError("workspace guard relocation identity changed")
        elif entry["destination_path"] == PROFILE_TEST_ADAPTATION:
            value = path.read_text(encoding="utf-8")
            if (entry["transformation"] != "PUBLIC_PROFILE_TEST_ADAPTATION"
                    or entry["source_sha256"] != PROFILE_TEST_SOURCE_SHA256
                    or 'profile["status"], "PUBLIC_RECONSTRUCTION_AWAITING_VALIDATION"' not in value
                    or 'profile["validation"]["status"], "NOT_VALIDATED_FROM_PUBLIC_BUILD"' not in value
                    or 'profile["evidence"]' in value):
                raise ToolchainError("evolution test does not match the sanitized public profile")
        elif (entry["destination_sha256"] != entry["source_sha256"]
              or entry["transformation"] != "EXACT_EXTRACTION"):
            raise ToolchainError(f"invalid extraction entry: {entry['destination_path']}")
        if path in expected:
            raise ToolchainError(f"duplicate reference entry: {entry['destination_path']}")
        expected.add(path)
    actual = {
        path for path in (ROOT / "reference").rglob("*")
        if path.is_file() and not {"bin", "obj", "__pycache__"}.intersection(path.parts)
        and path.suffix != ".pyc"
    }
    if actual != expected:
        raise ToolchainError("reference tree contains missing or unregistered files")
    if sanitized_seen != SANITIZED_PROFILES:
        raise ToolchainError("public profile sanitation set is incomplete")
    if len(lock["g1_kit_source_files"]) != 72:
        raise ToolchainError("G1 source lock is incomplete")
    for item in [lock["g1_exporter"], *lock["g1_kit_source_files"]]:
        target = _checked_path(ROOT / "reference", item["path"])
        if ("reference/" + item["path"]) not in SANITIZED_PROFILES and _sha256(target) != item["sha256"]:
            raise ToolchainError(f"G1 source lock mismatch: {item['path']}")
    for item in lock["studio_lifecycle_overlays"]:
        target = ROOT / "reference" / "overlays" / f"studio-{item['version']}" / LIFECYCLE
        if _sha256(target) != item["sha256"]:
            raise ToolchainError(f"Studio overlay lock mismatch: {item['version']}")


def _verify_file_entries(root: pathlib.Path, manifest: dict) -> int:
    if len(manifest.get("files", [])) != 73:
        raise ToolchainError("kit file count mismatch")
    expected_paths: set[pathlib.Path] = set()
    total_bytes = 0
    for entry in manifest["files"]:
        path = _checked_path(root, entry["path"])
        if path in expected_paths or not path.is_file() or path.is_symlink():
            raise ToolchainError(f"invalid kit path: {entry['path']}")
        if path.stat().st_size != entry["bytes"] or _sha256(path) != entry["sha256"]:
            raise ToolchainError(f"kit file identity mismatch: {entry['path']}")
        expected_paths.add(path)
        total_bytes += entry["bytes"]
    actual_paths = {path for path in root.rglob("*") if path.is_file() and path != root / KIT_MANIFEST}
    if actual_paths != expected_paths:
        raise ToolchainError("kit contains missing or unregistered files")
    return total_bytes


def verify_kit(root: pathlib.Path, mode: str) -> dict[str, object]:
    expected_version, expected_manifest_hash = EXPECTED[mode]
    manifest_path = root / KIT_MANIFEST
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise ToolchainError("kit manifest missing or linked")
    actual_hash = _sha256(manifest_path)
    if actual_hash != expected_manifest_hash:
        raise ToolchainError(f"{mode} manifest identity mismatch: {actual_hash}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("kit_version") != expected_version
            or manifest.get("public_reconstruction") != {
                "identity": "PUBLIC_PROFILE_SANITIZATION_V1",
                "sanitized_profile_count": len(SANITIZED_PROFILES),
            }):
        raise ToolchainError("public kit identity or version mismatch")
    total_bytes = _verify_file_entries(root, manifest)
    return {"ok": True, "mode": mode, "manifest_sha256": actual_hash, "file_count": 73, "total_bytes": total_bytes}


def export(mode: str, output: pathlib.Path) -> dict[str, object]:
    output = output.resolve()
    if output.exists() or not output.parent.is_dir():
        raise ToolchainError("output must be a new path with an existing parent")
    verify_reference_sources()
    staging_parent = pathlib.Path(tempfile.mkdtemp(prefix=".xcp-toolchain-", dir=output.parent))
    staging = staging_parent / "kit"
    try:
        env = os.environ.copy()
        for name in ("PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONUSERBASE"):
            env.pop(name, None)
        env["PYTHONNOUSERSITE"] = "1"
        exporter = REFERENCE / "tools" / "export_xcp_agent_kit.py"
        bootstrap = (
            "import runpy,sys; "
            "sys.path.insert(0,sys.argv[1]); "
            "script=sys.argv[2]; sys.argv=sys.argv[2:]; "
            "runpy.run_path(script,run_name='__main__')"
        )
        completed = subprocess.run(
            [sys.executable, "-B", "-I", "-c", bootstrap, str(exporter.parent),
             str(exporter), "export", "--output", str(staging)],
            cwd=REFERENCE, env=env, capture_output=True, text=True, timeout=120,
        )
        if completed.returncode:
            raise ToolchainError(f"G1 exporter failed: {completed.returncode}: {completed.stderr.strip()}")
        original_manifest = staging / KIT_MANIFEST
        manifest = json.loads(original_manifest.read_text(encoding="utf-8"))
        if manifest.get("kit_version") != "1.8.0":
            raise ToolchainError("original G1 exporter changed its version")
        _verify_file_entries(staging, manifest)
        if mode != "public-g1":
            version = mode.removeprefix("public-studio-")
            overlay = ROOT / "reference" / "overlays" / f"studio-{version}" / LIFECYCLE
            lifecycle = staging / LIFECYCLE
            shutil.copyfile(overlay, lifecycle)
            entries = [entry for entry in manifest["files"] if entry["path"] == LIFECYCLE]
            if len(entries) != 1:
                raise ToolchainError("G1 lifecycle entry missing or duplicate")
            entries[0]["bytes"] = lifecycle.stat().st_size
            entries[0]["sha256"] = _sha256(lifecycle)
        manifest["kit_version"] = EXPECTED[mode][0]
        manifest["public_reconstruction"] = {
            "identity": "PUBLIC_PROFILE_SANITIZATION_V1",
            "sanitized_profile_count": len(SANITIZED_PROFILES),
        }
        original_manifest.write_bytes(_canonical_json(manifest))
        result = verify_kit(staging, mode)
        os.replace(staging, output)
        return {**result, "output": str(output)}
    finally:
        shutil.rmtree(staging_parent)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    export_parser = subcommands.add_parser("export")
    export_parser.add_argument("--mode", choices=EXPECTED, required=True)
    export_parser.add_argument("--output", type=pathlib.Path, required=True)
    verify_parser = subcommands.add_parser("verify")
    verify_parser.add_argument("--mode", choices=EXPECTED, required=True)
    verify_parser.add_argument("--kit", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = export(args.mode, args.output) if args.command == "export" else verify_kit(args.kit, args.mode)
    except (OSError, ValueError, ToolchainError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
