"""Check development updates without changing any frozen P4 source file."""

import json
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from prepare_reference_worker import prepare  # noqa: E402
from stage_development_package_identities import stage  # noqa: E402


def test_new_families_and_update_versions_are_staged_only(tmp_path: Path) -> None:
    frozen = ROOT / "reference/xcompute-probe/src/XComputeProbe/runtime/WorkerCpuCapsuleModule.cpp"
    frozen_bytes = frozen.read_bytes()
    first = tmp_path / "first"
    prepare(first)
    first_identity = stage(first, "0.1.182.0", "1.3.0.0")
    assert first_identity["worker"]["name"] == "XCP.Development.Worker"
    assert "XCP.Development.CpuCapsule.Framework" in (
        first / "src/XComputeProbe/runtime/WorkerCpuCapsuleModule.cpp"
    ).read_text(encoding="utf-8")
    assert frozen.read_bytes() == frozen_bytes

    previous = tmp_path / "previous.json"
    previous.write_text(json.dumps(first_identity), encoding="utf-8")
    next_stage = tmp_path / "next"
    prepare(next_stage)
    with pytest.raises(ValueError, match="both package versions must increase"):
        stage(next_stage, "0.1.182.0", "1.3.0.1", previous)
    updated = stage(next_stage, "0.1.183.0", "1.3.0.1", previous)
    assert updated["capsule"]["version"] == "1.3.0.1"
    worker_runtime = next_stage / "src/XComputeProbe/runtime/WorkerXvmCpuCapsuleBackend.cpp"
    assert 'selection.version != L"1.3.0.1"' in worker_runtime.read_text(encoding="utf-8")
    assert frozen.read_bytes() == frozen_bytes
