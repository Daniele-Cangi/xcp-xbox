from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import source_snapshot  # noqa: E402
import write_distribution_metadata  # noqa: E402


def test_no_git_snapshot_verifies_bytes_tree_and_metadata_fallback(tmp_path: Path,
                                                                   monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "source"
    (root / "provenance").mkdir(parents=True)
    (root / "tools").mkdir()
    files = []
    for name, data in (("README.md", b"synthetic source\n"), ("tools/build.ps1", b"Write-Output 'synthetic'\n")):
        path = root / name
        path.write_bytes(data)
        files.append({"path": name, "mode": "100644", "bytes": len(data),
                      "sha256": source_snapshot.sha256(data),
                      "git_blob": source_snapshot.git_object("blob", data)})
    manifest = {"schema_version": "xcp-source-snapshot-v1", "producer_claimed_commit": "a" * 40,
                "git_tree": source_snapshot.tree_identity(files), "files": files}
    (root / source_snapshot.MANIFEST).write_text(json.dumps(manifest), encoding="utf-8")
    assert source_snapshot.verify(root)["git_tree"] == manifest["git_tree"]
    monkeypatch.setattr(write_distribution_metadata, "ROOT", root)
    inventory, origin = write_distribution_metadata.source_inventory()
    assert len(inventory) == 2
    assert origin["source_origin"] == "verified_source_snapshot"
    assert origin["source_commit_verification"].startswith("producer_assertion")
    (root / "README.md").write_bytes(b"tampered source\n")
    with pytest.raises(ValueError, match="file differs"):
        source_snapshot.verify(root)


def test_no_git_snapshot_rejects_unlisted_file(tmp_path: Path) -> None:
    root = tmp_path / "source"
    (root / "provenance").mkdir(parents=True)
    (root / source_snapshot.MANIFEST).write_text(json.dumps({
        "schema_version": "xcp-source-snapshot-v1", "producer_claimed_commit": "a" * 40,
        "git_tree": source_snapshot.tree_identity([]), "files": []}), encoding="utf-8")
    (root / "extra.txt").write_text("unlisted", encoding="utf-8")
    with pytest.raises(ValueError, match="file set differs"):
        source_snapshot.verify(root)
