"""Assemble a PC-only Studio development folder from reviewed XCP sources.

The historical P4/P5 archive identities are never used for this output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from export_reference_toolchain import ToolchainError, export, verify_kit


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME_BUILDER = ROOT / "reference/xcompute-probe/tools/build_xcp_studio_python_runtime.py"
MODE = "public-studio-1.9.1"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_runtime(root: pathlib.Path) -> dict[str, object]:
    completed = subprocess.run(
        [sys.executable, str(RUNTIME_BUILDER), "verify", "--runtime", str(root)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    if completed.returncode:
        raise ToolchainError(f"Python runtime verification failed: {completed.stdout.strip() or completed.stderr.strip()}")
    result = json.loads(completed.stdout)
    if result.get("ok") is not True or result.get("runtime_version") != "3.13.15":
        raise ToolchainError("Python runtime is not the pinned Studio runtime")
    return result


def regular_files(root: pathlib.Path) -> list[pathlib.Path]:
    if not root.is_dir() or root.is_symlink():
        raise ToolchainError(f"missing or linked input directory: {root}")
    files: list[pathlib.Path] = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ToolchainError(f"linked input path: {path}")
        if path.is_file():
            files.append(path)
    return files


def assemble(publish: pathlib.Path, runtime: pathlib.Path, output: pathlib.Path) -> dict[str, object]:
    publish = publish.resolve()
    runtime = runtime.resolve()
    output = output.resolve()
    if output.exists() or not output.parent.is_dir():
        raise ToolchainError("output must be a new path with an existing parent")
    publish_files = regular_files(publish)
    if not (publish / "XComputeControlCenter.exe").is_file():
        raise ToolchainError("Studio executable missing from publish")
    if not (publish / "XComputeControlCenter.pri").is_file():
        raise ToolchainError("WinUI PRI missing from publish")
    if sum(path.suffix.lower() == ".xbf" for path in publish_files) < 10:
        raise ToolchainError("WinUI XBF resources missing from publish")
    verify_runtime(runtime)

    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".xcp-studio-dev-", dir=output.parent))
    staging = temporary / "distribution"
    try:
        shutil.copytree(publish, staging)
        kit = export(MODE, staging / "toolchain")
        shutil.copytree(runtime, staging / "runtime" / "python")
        runtime_result = verify_runtime(staging / "runtime" / "python")
        verify_kit(staging / "toolchain", MODE)
        manifest = {
            "schema_version": "xcp-studio-public-development-build-v1",
            "hardware_validation": "NOT_TESTED_ON_XBOX",
            "historical_archive_identity": None,
            "toolchain": {"mode": MODE, "manifest_sha256": kit["manifest_sha256"]},
            "python_runtime": {
                "version": runtime_result["runtime_version"],
                "manifest_sha256": runtime_result["manifest_sha256"],
            },
            "studio": {
                "exe_sha256": sha256(staging / "XComputeControlCenter.exe"),
                "pri_sha256": sha256(staging / "XComputeControlCenter.pri"),
                "xbf_count": sum(path.suffix.lower() == ".xbf" for path in publish_files),
            },
        }
        (staging / "xcp-studio-dev-build-manifest.json").write_text(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        os.replace(staging, output)
        return {"ok": True, "output": str(output), **manifest}
    finally:
        shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publish", type=pathlib.Path, required=True)
    parser.add_argument("--runtime", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = assemble(args.publish, args.runtime, args.output)
    except (OSError, ValueError, ToolchainError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
