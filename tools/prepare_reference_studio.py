"""Assemble the exact Studio sources with a versioned kit for PC builds/tests.

The output is a composite build workspace, not a historical kit artifact. The
kit is verified before Studio source and tests are added to that workspace.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import tempfile

from export_reference_toolchain import ToolchainError, export

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "provenance" / "reference-extraction-manifest.json"
REFERENCE_PREFIX = pathlib.PurePosixPath("reference/xcompute-probe")
STUDIO_COMMIT = "4acb86a11396f4e3e4416afe6fc6ae18a22d3a9d"


def prepare(mode: str, output: pathlib.Path) -> dict[str, object]:
    if mode not in {"public-studio-1.9.0", "public-studio-1.9.1"}:
        raise ToolchainError("Studio build requires a versioned Studio kit")
    output = output.resolve()
    if output.exists() or not output.parent.is_dir():
        raise ToolchainError("output must be a new path with an existing parent")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    studio = [item for item in manifest["components"]
              if item["category"] == "studio_reference_p4_and_draft_p5"]
    if len(studio) != 53:
        raise ToolchainError("Studio reference source set is incomplete")

    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".xcp-studio-", dir=output.parent))
    workspace = temporary / "workspace"
    try:
        kit = export(mode, workspace)
        copied: set[str] = set()
        for item in studio:
            destination = pathlib.PurePosixPath(item["destination_path"])
            if (item["source_commit"] != STUDIO_COMMIT
                    or item["transformation"] != "EXACT_EXTRACTION"
                    or item["source_sha256"] != item["destination_sha256"]):
                raise ToolchainError("Studio source provenance is not exact P4")
            try:
                relative = destination.relative_to(REFERENCE_PREFIX)
            except ValueError as exc:
                raise ToolchainError("Studio source escaped reference root") from exc
            if (not relative.parts or relative.parts[0] not in {"src", "tests"}
                    or any(part in {".", ".."} for part in relative.parts)
                    or "\\" in item["destination_path"]):
                raise ToolchainError("invalid Studio source path")
            target = workspace.joinpath(*relative.parts)
            if target.exists() or relative.as_posix() in copied:
                raise ToolchainError("duplicate Studio build path")
            copied.add(relative.as_posix())
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT.joinpath(*destination.parts), target)
            if hashlib.sha256(target.read_bytes()).hexdigest() != item["source_sha256"]:
                raise ToolchainError("Studio build copy changed source bytes")
        os.replace(workspace, output)
        return {"ok": True, "mode": mode, "output": str(output),
                "kit_manifest_sha256": kit["manifest_sha256"],
                "studio_source_files": len(copied)}
    finally:
        shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("public-studio-1.9.0", "public-studio-1.9.1"), required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.mode, args.output)
    except (OSError, ValueError, ToolchainError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
