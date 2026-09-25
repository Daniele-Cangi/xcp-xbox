"""Sanitized public product kits must reproduce from public source alone."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "export_reference_toolchain.py"
EXPECTED = {
    "public-g1": "63bc6a745ef24f78b06762689b29dea921457d5a270c7c655ffb1b8a8aad94c7",
    "public-studio-1.9.0": "3f4db5a9a671db35634c1930c84772e8a5f6b3295d2fdb5363ba6c32c1ab7e57",
    "public-studio-1.9.1": "827237cb78a5546c42aa853ff13a7a6847fb8b86a8ba7bfdc1979f466acfc03d",
}


@pytest.mark.parametrize("mode", EXPECTED)
def test_public_reference_export_and_tamper_refusal(tmp_path: Path, mode: str) -> None:
    output = tmp_path / "path with spaces" / mode
    output.parent.mkdir()
    command = [sys.executable, str(TOOL)]
    exported = subprocess.run(command + ["export", "--mode", mode, "--output", str(output)],
                              cwd=tmp_path, capture_output=True, text=True)
    assert exported.returncode == 0, exported.stderr + exported.stdout
    result = json.loads(exported.stdout)
    assert result["manifest_sha256"] == EXPECTED[mode]
    assert result["manifest_sha256"] not in {
        "86d2cefebd9e01d69e3eb7bd15c17b70a7a501decdcb6f0fbef109515ed46346",
        "39d4db52184b239f75c90115f12e5e3743c5c818a89df966ff66f3cf6236ba69",
        "2d53d979a96ec8fce3413fb27cee3a94a2f22fdaa6a0891fe6257b6f6e558130",
    }
    assert result["file_count"] == 73
    verified = subprocess.run(command + ["verify", "--mode", mode, "--kit", str(output)],
                              cwd=tmp_path, capture_output=True, text=True)
    assert verified.returncode == 0, verified.stderr + verified.stdout
    lifecycle = output / "tools" / "xcp_agent_lifecycle.py"
    lifecycle.write_bytes(lifecycle.read_bytes() + b"\n# tamper\n")
    rejected = subprocess.run(command + ["verify", "--mode", mode, "--kit", str(output)],
                              cwd=tmp_path, capture_output=True, text=True)
    assert rejected.returncode == 2
    assert json.loads(rejected.stdout)["ok"] is False
