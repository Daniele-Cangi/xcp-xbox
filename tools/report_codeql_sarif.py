"""Report local CodeQL findings when GitHub Code Scanning is unavailable."""

from __future__ import annotations

import argparse
import json
import os
import pathlib


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sarif", type=pathlib.Path)
    args = parser.parse_args()
    document = json.loads(args.sarif.read_text(encoding="utf-8"))
    findings = []
    for run in document.get("runs", []):
        for result in run.get("results", []):
            locations = result.get("locations") or []
            location = locations[0].get("physicalLocation", {}) if locations else {}
            artifact = location.get("artifactLocation", {}).get("uri", "unknown")
            line = location.get("region", {}).get("startLine", "?")
            findings.append(f"- `{result.get('ruleId', 'unknown')}` at `{artifact}:{line}`")
    summary = [f"CodeQL analyzed locally; {len(findings)} result(s).",
               "GitHub Code Scanning upload was disabled because this private repository does not have that feature enabled."]
    summary.extend(findings)
    report = "\n".join(summary) + "\n"
    print(report)
    if path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(path, "a", encoding="utf-8") as stream:
            stream.write("## CodeQL local analysis\n\n" + report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
