"""Fail a public release on tracked private material or provenance drift."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES = {".msix", ".msixbundle", ".pfx", ".p12", ".key", ".pem", ".env", ".zip", ".7z"}
FORBIDDEN_PARTS = {"artifacts", "results", "worldloop", "forgeprotocol", "minilens", "secrets", "credentials"}
PRIVATE_PATTERNS = {
    "private_key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "token": re.compile(rb"\b(?:ghp_|gho_|github_pat_|sk-proj-|AKIA)[A-Za-z0-9_-]{12,}"),
    "windows_absolute_path": re.compile(rb"\b[A-Za-z]:\\[A-Za-z0-9._\\-]+"),
    "personal_unix_path": re.compile(rb"/(?:Users|home)/[A-Za-z0-9._-]+/"),
    "private_ip": re.compile(rb"\b(?:10\.(?:\d{1,3}\.){2}\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b"),
    "internal_url": re.compile(rb"https?://[^\s\"']+\.(?:local|internal)(?:[/:\s\"']|$)", re.I),
    "credential_assignment": re.compile(rb"\b(?:api[_-]?key|access[_-]?token|auth[_-]?token|session[_-]?id|device[_-]?id|password|cookie|pairing[_-]?(?:pin|secret|code)|client[_-]?secret)\b[\"']?\s*[:=]\s*[\"'][A-Za-z0-9_./+-]{8,}[\"']", re.I),
    "authorization_header": re.compile(rb"\bAuthorization\s*[:=]\s*[\"']?Bearer\s+[A-Za-z0-9._-]{12,}", re.I),
}
SCHEMA_NAMESPACE_IDS = {b"https://" + b"xcompute-probe.local/", b"https://" + b"xcp.local/",
                        b"https://" + b"xcompute.local/"}
SANITIZED_PROFILES = {
    "reference/xcompute-probe/profiles/creative/godot-adaptation-compatibility-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-agent-native-creation-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-agent-native-source-adaptation-v1.json",
    "reference/xcompute-probe/profiles/creative/xcp-project-evolution-v1.json",
}
# Exact, non-secret UI examples in the P4/P5 Studio XAML. These are inert
# placeholder text, not a configured project directory or Xbox endpoint.
STUDIO_PLACEHOLDERS = {
    ("reference/xcompute-probe/src/XComputeControlCenter/XComputeControlCenter/Views/CreatePage.xaml",
     "windows_absolute_path", b"C:" + b"\\projects\\my-xcp-project"),
    ("reference/xcompute-probe/src/XComputeControlCenter/XComputeControlCenter/Views/DevicesPage.xaml",
     "private_ip", b"192.168." + b"1.120"),
}
REGISTERED_REFERENCE_PNGS = {
    "reference/xcompute-probe/src/XComputeProbe/Assets/Logo.png",
    "reference/xcompute-probe/src/XComputeProbe/Assets/SmallLogo.png",
    "reference/xcompute-probe/src/XComputeProbe/Assets/SplashScreen.png",
    "reference/xcompute-probe/src/XComputeProbe/Assets/StoreLogo.png",
}
RELOCATED_WORKSPACE_GUARD = "reference/xcompute-probe/tools/assert-non-onedrive-workspace.ps1"
# Two inert strings in the byte-locked original P4 fake-worker test exercise
# credential rejection and ephemeral-session handling. The whole-file digest
# prevents this exception from covering newly added or changed material.
P4_SYNTHETIC_ADAPTER_TEST = "reference/xcompute-probe/tests/unit/test_xcp_studio_live_adapter_v1.py"
P4_SYNTHETIC_ADAPTER_TEST_SHA256 = "04068d984192a3d68d34c91fe3f6c0315e0b26980048fb3a6211bda329b26b77"
P4_SYNTHETIC_CREDENTIAL_FIXTURES = {
    b"session" + b'_id": "must-not-enter"',
    b"session" + b'_id="ephemeral-test-session"',
}


def candidate_files() -> list[Path]:
    output = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT)
    return [ROOT / name.decode("utf-8") for name in output.split(b"\0") if name]


def check() -> list[str]:
    violations: list[str] = []
    workflow = (ROOT / ".github/workflows/tests.yml").read_text(encoding="utf-8")
    if "actions/upload-artifact" in workflow:
        violations.append("CI artifact upload requires an explicit binary publication decision")
    for path in candidate_files():
        relative = path.relative_to(ROOT).as_posix()
        if path.is_symlink():
            violations.append(f"{relative}: symbolic link")
            continue
        parts = {part.lower() for part in path.relative_to(ROOT).parts}
        if parts & FORBIDDEN_PARTS or path.suffix.lower() in FORBIDDEN_SUFFIXES:
            violations.append(f"{relative}: forbidden path or extension")
            continue
        data = path.read_bytes()
        if relative in REGISTERED_REFERENCE_PNGS:
            if not data.startswith(b"\x89PNG\r\n\x1a\n"):
                violations.append(f"{relative}: registered PNG has invalid signature")
            # The reference manifest below must also bind the exact bytes,
            # first-party provenance, and unchanged source identity.
            continue
        if b"\0" in data:
            violations.append(f"{relative}: binary data")
            continue
        for name, pattern in PRIVATE_PATTERNS.items():
            for match in pattern.finditer(data):
                # These exact historical JSON Schema IDs are namespaces, not
                # device endpoints. Every other internal URL is still denied.
                if (name == "internal_url" and relative.startswith("reference/")
                        and relative.endswith(".schema.json")
                        and match.group() in SCHEMA_NAMESPACE_IDS):
                    continue
                if (relative, name, match.group()) in STUDIO_PLACEHOLDERS:
                    continue
                if (name == "credential_assignment"
                        and relative == P4_SYNTHETIC_ADAPTER_TEST
                        and hashlib.sha256(data).hexdigest() == P4_SYNTHETIC_ADAPTER_TEST_SHA256
                        and match.group() in P4_SYNTHETIC_CREDENTIAL_FIXTURES):
                    continue
                violations.append(f"{relative}: {name}")
        if relative.startswith(("src/", "examples/", "tests/")) and re.search(rb"(?i)\b(?:minilens|worldloop|forgeprotocol|g2n\d|g2p\d)\b", data):
            violations.append(f"{relative}: research-only identifier")
    manifest_path = ROOT / "provenance" / "extraction-manifest.json"
    if not manifest_path.is_file():
        violations.append("provenance/extraction-manifest.json: missing")
    else:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        seen: set[str] = set()
        for entry in manifest["components"]:
            destination = entry["destination_path"]
            if destination in seen:
                violations.append(f"{destination}: duplicate provenance entry")
            seen.add(destination)
            file = ROOT / destination
            if not file.is_file():
                violations.append(f"{destination}: missing extracted component")
            elif hashlib.sha256(file.read_bytes()).hexdigest() != entry["destination_sha256"]:
                violations.append(f"{destination}: provenance hash mismatch")
            if entry["license_status"] not in {"first_party_owner_authorized", "first_party_new"}:
                violations.append(f"{destination}: unapproved provenance")
    reference_manifest_path = ROOT / "provenance" / "reference-extraction-manifest.json"
    if (ROOT / "reference").exists():
        if not reference_manifest_path.is_file():
            violations.append("provenance/reference-extraction-manifest.json: missing")
        else:
            reference_manifest = json.loads(reference_manifest_path.read_text(encoding="utf-8"))
            registered: set[str] = set()
            sanitized_seen: set[str] = set()
            for entry in reference_manifest["components"]:
                destination = entry["destination_path"]
                if destination in registered or not destination.startswith("reference/"):
                    violations.append(f"{destination}: duplicate or invalid reference entry")
                registered.add(destination)
                file = ROOT / destination
                if not file.is_file() or file.is_symlink():
                    violations.append(f"{destination}: missing/linked reference component")
                elif (file.stat().st_size != entry["bytes"]
                      or hashlib.sha256(file.read_bytes()).hexdigest() != entry["destination_sha256"]):
                    violations.append(f"{destination}: reference identity mismatch")
                if destination in SANITIZED_PROFILES:
                    sanitized_seen.add(destination)
                    if (entry["transformation"] != "PUBLIC_PROFILE_SANITIZATION"
                            or entry["destination_sha256"] == entry["source_sha256"]):
                        violations.append(f"{destination}: invalid public profile sanitation")
                    if file.is_file():
                        profile_bytes = file.read_bytes()
                        if (re.search(rb"\b[0-9a-f]{64}\b", profile_bytes)
                                or any(key in json.loads(profile_bytes) for key in ("recorded_on", "evidence"))):
                            violations.append(f"{destination}: operational evidence in public profile")
                elif destination == RELOCATED_WORKSPACE_GUARD:
                    if (entry["transformation"] != "MECHANICAL_RELOCATION"
                            or entry["destination_sha256"] == entry["source_sha256"]):
                        violations.append(f"{destination}: invalid mechanical relocation")
                elif destination == "reference/xcompute-probe/tests/unit/test_xcp_project_evolve_v1.py":
                    if (entry["transformation"] != "PUBLIC_PROFILE_TEST_ADAPTATION"
                            or entry["source_sha256"] != "e620bf81c3f973e8c21189bfcdf015dfa0f30682d21838045542098eae3750fe"
                            or b'profile["evidence"]' in file.read_bytes()):
                        violations.append(f"{destination}: invalid public profile test adaptation")
                elif (entry["transformation"] != "EXACT_EXTRACTION"
                      or entry["destination_sha256"] != entry["source_sha256"]):
                    violations.append(f"{destination}: invalid reference provenance")
                if destination in REGISTERED_REFERENCE_PNGS and entry["category"] != "worker_first_party_brand_assets_p4":
                    violations.append(f"{destination}: unregistered binary provenance")
                if entry["license_status"] != "first_party_owner_authorized":
                    violations.append(f"{destination}: invalid reference license status")
            if sanitized_seen != SANITIZED_PROFILES:
                violations.append("reference profiles: sanitation set incomplete")
            candidates = {
                path.relative_to(ROOT).as_posix()
                for path in candidate_files()
                if path.is_relative_to(ROOT / "reference")
            }
            for missing in sorted(candidates - registered):
                violations.append(f"{missing}: unregistered reference file")
    return violations


if __name__ == "__main__":
    failures = check()
    for failure in failures:
        print(failure)
    print(f"public-release hygiene: {'FAIL' if failures else 'PASS'} ({len(failures)} violations)")
    raise SystemExit(bool(failures))
