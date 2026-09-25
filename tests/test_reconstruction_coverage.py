"""The source lock must reject contradictory reconstruction authority."""

from __future__ import annotations

from copy import deepcopy

from tools.check_reconstruction_coverage import _load, check


def _inputs() -> tuple[dict, dict]:
    return (
        _load("provenance/reconstruction-coverage.json"),
        _load("provenance/reference-source-lock.json"),
    )


def test_current_reconstruction_lock_is_consistent() -> None:
    coverage, lock = _inputs()
    assert check(coverage, lock) == []


def test_g1_commit_must_match_coverage_ref() -> None:
    coverage, lock = _inputs()
    lock["g1_snapshot_commit"] = "0" * 40
    assert any("G1 snapshot commit differs" in error for error in check(coverage, lock))


def test_duplicate_overlay_version_is_rejected() -> None:
    coverage, lock = _inputs()
    lock["studio_lifecycle_overlays"].append(deepcopy(lock["studio_lifecycle_overlays"][0]))
    assert any("duplicates" in error for error in check(coverage, lock))


def test_overlay_commits_bind_to_p4_and_draft_p5() -> None:
    coverage, lock = _inputs()
    first, second = lock["studio_lifecycle_overlays"]
    first["source_commit"], second["source_commit"] = second["source_commit"], first["source_commit"]
    errors = check(coverage, lock)
    assert any("1.9.0 overlay commit" in error for error in errors)
    assert any("1.9.1 overlay commit" in error for error in errors)


def test_verified_artifact_requires_actual_hash_and_size() -> None:
    coverage, lock = _inputs()
    coverage["historical_artifacts"][0].pop("sha256")
    assert any("verified bytes/hash missing" in error for error in check(coverage, lock))
