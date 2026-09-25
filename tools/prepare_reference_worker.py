"""Copy the byte-exact P4 worker build graph to a disposable PC workspace."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "provenance/reference-extraction-manifest.json"
PREFIX = pathlib.PurePosixPath("reference/xcompute-probe")
P4 = "4acb86a11396f4e3e4416afe6fc6ae18a22d3a9d"
CATEGORIES = {
    "worker_native_p4",
    "worker_build_tooling_p4",
    "worker_first_party_brand_assets_p4",
}
RELOCATED_WORKSPACE_GUARD = "reference/xcompute-probe/tools/assert-non-onedrive-workspace.ps1"


def prepare(output: pathlib.Path) -> dict[str, object]:
    output = output.resolve()
    if output.exists() or not output.parent.is_dir():
        raise ValueError("output must be a new path with an existing parent")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = [entry for entry in manifest["components"] if entry["category"] in CATEGORIES]
    if len(entries) != 235:
        raise ValueError("P4 worker source set is incomplete")
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".xcp-worker-", dir=output.parent))
    stage = temporary / "workspace"
    stage.mkdir()
    try:
        seen: set[str] = set()
        for entry in entries:
            destination = pathlib.PurePosixPath(entry["destination_path"])
            relative = destination.relative_to(PREFIX)
            exact = entry["transformation"] == "EXACT_EXTRACTION" and entry["source_sha256"] == entry["destination_sha256"]
            relocated = (entry["destination_path"] == RELOCATED_WORKSPACE_GUARD
                         and entry["transformation"] == "MECHANICAL_RELOCATION")
            if (entry["source_commit"] != P4
                    or not (exact or relocated)
                    or not relative.parts
                    or relative.parts[0] not in {"src", "tools"}
                    or any(part in {".", ".."} for part in relative.parts)
                    or "\\" in entry["destination_path"]):
                raise ValueError("invalid frozen worker provenance")
            name = relative.as_posix().casefold()
            if name in seen:
                raise ValueError("duplicate worker build path")
            seen.add(name)
            source = ROOT.joinpath(*destination.parts)
            if not source.is_file() or source.is_symlink():
                raise ValueError("missing or linked worker source")
            data = source.read_bytes()
            if (len(data) != entry["bytes"]
                    or hashlib.sha256(data).hexdigest() != entry["destination_sha256"]):
                raise ValueError("worker source identity mismatch")
            target = stage.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        os.replace(stage, output)
        return {"ok": True, "output": str(output), "source_commit": P4,
                "source_files": len(seen), "exact_files": len(seen) - 1,
                "mechanically_relocated_files": 1}
    finally:
        shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.output)
    except (OSError, ValueError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
