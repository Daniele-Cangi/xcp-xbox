from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_public_studio_workspace_keeps_exact_kit_and_source(tmp_path: Path) -> None:
    destination = tmp_path / "Studio build workspace"
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "prepare_reference_studio.py"),
         "--mode", "public-studio-1.9.1", "--output", str(destination)],
        cwd=ROOT, capture_output=True, text=True, check=True,
    )
    report = json.loads(result.stdout)
    assert report["studio_source_files"] == 53
    assert report["kit_manifest_sha256"] == (
        "827237cb78a5546c42aa853ff13a7a6847fb8b86a8ba7bfdc1979f466acfc03d")
    assert hashlib.sha256(
        (destination / "xcp-agent-kit-manifest.json").read_bytes()
    ).hexdigest() == report["kit_manifest_sha256"]
    manifest = json.loads(
        (ROOT / "provenance" / "reference-extraction-manifest.json").read_text(encoding="utf-8")
    )
    for item in manifest["components"]:
        if item["category"] != "studio_reference_p4_and_draft_p5":
            continue
        relative = Path(item["destination_path"]).relative_to("reference/xcompute-probe")
        assert hashlib.sha256((destination / relative).read_bytes()).hexdigest() == item["source_sha256"]
