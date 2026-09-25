from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

from tools.public_release_check import ROOT, check_visual_assets


def test_public_visuals_match_pinned_source_blobs() -> None:
    assert check_visual_assets() == []


def test_visual_manifest_cannot_relabel_changed_bytes(tmp_path: Path) -> None:
    manifest_path = Path("provenance/visual-asset-provenance.json")
    manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
    for entry in manifest["assets"]:
        destination = tmp_path / entry["destination_path"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / entry["destination_path"], destination)

    changed = manifest["assets"][0]
    file = tmp_path / changed["destination_path"]
    altered = file.read_bytes() + b"changed"
    file.write_bytes(altered)
    changed["bytes"] = len(altered)
    changed["destination_sha256"] = hashlib.sha256(altered).hexdigest()
    output = tmp_path / manifest_path
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest), encoding="utf-8")

    assert any("differ from source blob" in violation for violation in check_visual_assets(tmp_path))
