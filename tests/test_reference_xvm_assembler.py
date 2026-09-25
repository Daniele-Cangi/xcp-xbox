"""Run the exact P4 assembler and Python executor on first-party source examples."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest


REFERENCE = Path(__file__).resolve().parents[1] / "reference/xcompute-probe"
ASSEMBLER = REFERENCE / "tools/xvm_assembler.py"
EXECUTOR = REFERENCE / "tools/xvm_reference.py"


def run(*args: Path | str) -> dict[str, object]:
    completed = subprocess.run(
        [sys.executable, *(str(arg) for arg in args)],
        cwd=REFERENCE,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


@pytest.mark.parametrize(
    ("source", "expected_output"),
    [
        ("creative-signal-garden-pulse.xvmasm", "02000000000000000000000000000000"),
        ("agent-native-xvm-toolchain-v1.xvmasm", "00000000000000000000000000000000"),
    ],
)
def test_exact_assembler_produces_valid_executable_artifact(
    tmp_path: Path, source: str, expected_output: str
) -> None:
    artifact = tmp_path / "canonical-artifact.json"
    result = run(ASSEMBLER, "assemble", REFERENCE / "samples/xvm" / source, "--output", artifact)
    assert result["locally_verified"] is True
    assert result["sha256"] == hashlib.sha256(artifact.read_bytes()).hexdigest()
    assert run(ASSEMBLER, "validate", artifact) == result
    execution = run(EXECUTOR, artifact)
    assert execution["program_id"] == result["program_id"]
    assert execution["output_hex"] == expected_output
