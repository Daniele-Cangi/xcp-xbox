"""Check the published reconstruction map without reading the private source."""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
REF = re.compile(r"[0-9a-f]{40}\Z")
REQUIRED_COMPONENT_KEYS = {
    "id", "role", "entrypoint", "source", "build_dependencies",
    "runtime_dependencies", "generated", "destination", "provenance",
    "transformation", "original_tests", "transferred_tests", "verification", "status",
    "scope_tier", "disposition", "residual_gap", "pc_checks",
}
SCOPE_TIERS = {"P4_G1_PREVIEW", "PREVIEW_AND_LAB", "LABORATORY_EXTENSION",
               "ADJACENT_PUBLIC_CORE", "HISTORICAL_REFERENCE"}
DISPOSITIONS = {"TRANSFERRED", "DEFERRED", "EXCLUDED"}


def _load(path: str) -> dict:
    return json.loads((ROOT / path).read_text(encoding="utf-8"))


def check(coverage: dict | None = None, lock: dict | None = None) -> list[str]:
    failures: list[str] = []
    coverage = coverage if coverage is not None else _load("provenance/reconstruction-coverage.json")
    lock = lock if lock is not None else _load("provenance/reference-source-lock.json")
    refs = coverage["source_refs"]
    if lock["g1_snapshot_commit"] != refs["g1"]:
        failures.append("G1 snapshot commit differs from coverage source ref")
    for name, commit in refs.items():
        if not REF.fullmatch(commit):
            failures.append(f"source ref {name}: invalid commit")
    ids: set[str] = set()
    by_id: dict[str, dict] = {}
    for component in coverage["components"]:
        ident = component.get("id", "<missing>")
        if ident in ids:
            failures.append(f"component {ident}: duplicate id")
        ids.add(ident)
        by_id[ident] = component
        missing = REQUIRED_COMPONENT_KEYS - component.keys()
        if missing:
            failures.append(f"component {ident}: missing {', '.join(sorted(missing))}")
            continue
        if component["status"] not in coverage["status_values"]:
            failures.append(f"component {ident}: unknown status")
        if component["scope_tier"] not in SCOPE_TIERS or component["disposition"] not in DISPOSITIONS:
            failures.append(f"component {ident}: invalid scope/disposition")
        if not component["residual_gap"]:
            failures.append(f"component {ident}: residual gap is not stated")
        for path in component["transferred_tests"] + component["pc_checks"]:
            if not isinstance(path, str) or not (ROOT / path).exists():
                failures.append(f"component {ident}: claimed PC check absent: {path}")
        source = component["source"]
        if source["ref"] not in refs:
            failures.append(f"component {ident}: unknown source ref")
        if not SHA256.fullmatch(source["sha256"]):
            failures.append(f"component {ident}: missing source hash")
        if not source["anchor"] or not source["scope"]:
            failures.append(f"component {ident}: missing source scope")
        if not component["provenance"] or not component["verification"]:
            failures.append(f"component {ident}: missing provenance/verification")
        if component["status"] == "PENDING_PARITY" and not component["original_tests"]:
            failures.append(f"component {ident}: no original test pointer")

    source_files = lock["g1_kit_source_files"]
    paths = [entry["path"] for entry in source_files]
    if len(paths) != len(set(paths)) or len(paths) != lock["g1_kit_source_file_count"]:
        failures.append("G1 source file list is not unique/complete")
    if len(paths) != 72 or lock["g1_expected_export_file_count_including_requirements"] != 73:
        failures.append("G1 source/export counts do not match the reference")
    if sum(entry["bytes"] for entry in source_files) != lock["g1_kit_source_payload_bytes"]:
        failures.append("G1 source payload byte count differs")
    for entry in source_files + [lock["g1_exporter"]]:
        if not entry["path"].startswith("xcompute-probe/") or not SHA256.fullmatch(entry["sha256"]):
            failures.append(f"invalid G1 file lock: {entry['path']}")
    g1 = by_id.get("g1_frozen_toolchain", {}).get("source", {})
    if g1.get("sha256") != lock["g1_exporter"]["sha256"]:
        failures.append("G1 exporter anchor differs from source lock")
    if lock["g1_expected_export_manifest_sha256"] != coverage["reference_identities"]["g1_1_8_0_manifest_sha256"]:
        failures.append("G1 manifest identity differs from coverage")
    overlay_list = lock["studio_lifecycle_overlays"]
    overlays = {item["version"]: item for item in overlay_list}
    if len(overlay_list) != len(overlays):
        failures.append("Studio overlay version lock contains duplicates")
    if set(overlays) != {"1.9.0", "1.9.1"}:
        failures.append("Studio overlay version lock incomplete")
    else:
        if overlays["1.9.1"]["sha256"] != by_id["c5_lifecycle"]["source"]["sha256"]:
            failures.append("Studio 1.9.1 lifecycle anchor differs")
        if overlays["1.9.0"]["source_commit"] != refs["p4_r10"]:
            failures.append("Studio 1.9.0 overlay commit differs from P4")
        if overlays["1.9.1"]["source_commit"] != refs["draft_p5"]:
            failures.append("Studio 1.9.1 overlay commit differs from draft P5")
        for item in overlays.values():
            if not SHA256.fullmatch(item["sha256"]) or not REF.fullmatch(item["source_commit"]):
                failures.append("Studio overlay hash/ref invalid")
    for name, digest in coverage["reference_identities"].items():
        if not SHA256.fullmatch(digest):
            failures.append(f"reference identity {name}: invalid digest")
    historical = set(coverage["reference_identities"].values())
    public = coverage.get("public_reconstruction_identities", {})
    if len(public) != 3:
        failures.append("public reconstruction identities are incomplete")
    for name, digest in public.items():
        if not SHA256.fullmatch(digest):
            failures.append(f"public reconstruction identity {name}: invalid digest")
        elif digest in historical:
            failures.append(f"public identity {name} reuses a historical digest")
    artifact_states = {"FOUND_AND_HASH_VERIFIED", "DOCUMENTED_ONLY", "MISSING"}
    for artifact in coverage["historical_artifacts"]:
        if artifact["status"] not in artifact_states:
            failures.append(f"artifact {artifact['id']}: invalid state")
        if artifact["status"] == "FOUND_AND_HASH_VERIFIED" and ("sha256" not in artifact or "bytes" not in artifact):
            failures.append(f"artifact {artifact['id']}: verified bytes/hash missing")
        if "sha256" in artifact and not SHA256.fullmatch(artifact["sha256"]):
            failures.append(f"artifact {artifact['id']}: invalid hash")
    decisions = coverage.get("capability_decisions", [])
    if len(decisions) < 9 or len({item.get("id") for item in decisions}) != len(decisions):
        failures.append("capability disposition map is incomplete or duplicated")
    for item in decisions:
        if (item.get("scope_tier") not in SCOPE_TIERS
                or item.get("disposition") not in DISPOSITIONS
                or not item.get("basis") or not item.get("open")):
            failures.append(f"capability {item.get('id')}: missing decision, basis or open gate")
    return failures


if __name__ == "__main__":
    errors = check()
    for error in errors:
        print(error)
    print(f"reconstruction coverage: {'FAIL' if errors else 'PASS'} ({len(errors)} errors)")
    raise SystemExit(bool(errors))
