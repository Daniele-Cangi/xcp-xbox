"""The local CodeQL reporter must accept SARIF results without locations."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from report_codeql_sarif import main  # noqa: E402


@pytest.mark.parametrize("include_empty_locations", [True, False])
def test_valid_sarif_result_without_location_is_reported(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str],
        include_empty_locations: bool) -> None:
    result: dict[str, object] = {"ruleId": "test/no-location", "message": {"text": "synthetic finding"}}
    if include_empty_locations:
        result["locations"] = []
    sarif = {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}}, "results": [result]}]}
    path = tmp_path / "results.sarif"
    path.write_text(json.dumps(sarif), encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["report_codeql_sarif.py", str(path)])
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert main() == 0
    assert "`test/no-location` at `unknown:?`" in capsys.readouterr().out
