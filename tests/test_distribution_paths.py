from __future__ import annotations

import sys
from pathlib import Path

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from distribution_paths import contained  # noqa: E402


def test_manifest_paths_cannot_escape_distribution(tmp_path: Path) -> None:
    base = tmp_path / "packages"
    base.mkdir()
    outside = tmp_path / "outside.msix"
    outside.write_bytes(b"synthetic")
    assert contained(base, "worker.msix", filename=True) == base / "worker.msix"
    for name in ("../outside.msix", "sub/../../outside.msix", "C:/outside.msix",
                 "/outside.msix", "sub\\worker.msix", "./worker.msix", "", "sub//worker.msix"):
        with pytest.raises(ValueError):
            contained(base, name)
    with pytest.raises(ValueError):
        contained(base, "sub/worker.msix", filename=True)


def test_manifest_paths_cannot_follow_symlink(tmp_path: Path) -> None:
    base = tmp_path / "packages"
    base.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    try:
        (base / "linked").symlink_to(outside, target_is_directory=True)
    except (OSError, NotImplementedError):
        pytest.skip("symlink creation unavailable")
    with pytest.raises(ValueError):
        contained(base, "linked/worker.msix")
