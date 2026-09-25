"""The assembler must verify VCLibs itself before recording signature evidence."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path
from subprocess import CompletedProcess
from zipfile import ZipFile

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from assemble_development_distribution import main as assemble_main, verified_external_vclibs  # noqa: E402
from inspect_reference_vclibs import PUBLISHER  # noqa: E402


def synthetic_package(path: Path) -> Path:
    manifest = (
        '<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">'
        f'<Identity Name="Microsoft.VCLibs.140.00" Publisher="{PUBLISHER}" '
        'Version="14.0.33519.0" ProcessorArchitecture="x64"/>'
        '</Package>'
    )
    with ZipFile(path, "w") as archive:
        archive.writestr("AppxManifest.xml", manifest)
        archive.writestr("AppxSignature.p7x", b"synthetic")
    return path


def test_assembler_binds_mocked_signature_result_to_inspected_bytes(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    path = synthetic_package(tmp_path / "sample.appx")
    calls = []

    def valid(command: list[str], **kwargs: object) -> CompletedProcess[str]:
        calls.append(command)
        assert command[-1] == str(path.resolve())
        assert Path(command[4]).name == "verify_vclibs_authenticode.ps1"
        return CompletedProcess(command, 0, stdout="Valid\n")

    monkeypatch.setattr("assemble_development_distribution.subprocess.run", valid)
    result = verified_external_vclibs(path)
    assert len(calls) == 1
    assert result["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert result["signature_cryptographically_verified"] is True


def test_assembler_rejects_invalid_or_unstable_signature_evidence(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    path = synthetic_package(tmp_path / "sample.appx")

    def invalid(command: list[str], **kwargs: object) -> CompletedProcess[str]:
        return CompletedProcess(command, 1, stdout="")

    monkeypatch.setattr("assemble_development_distribution.subprocess.run", invalid)
    with pytest.raises(ValueError, match="signature is not Valid"):
        verified_external_vclibs(path)

    def changed(command: list[str], **kwargs: object) -> CompletedProcess[str]:
        path.write_bytes(path.read_bytes() + b"changed after verification began")
        return CompletedProcess(command, 0, stdout="Valid\n")

    monkeypatch.setattr("assemble_development_distribution.subprocess.run", changed)
    with pytest.raises(ValueError, match="changed during"):
        verified_external_vclibs(path)


@pytest.mark.skipif(sys.platform != "win32", reason="Windows Authenticode is required")
def test_synthetic_signature_member_is_not_cryptographically_valid(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="signature is not Valid"):
        verified_external_vclibs(synthetic_package(tmp_path / "sample.appx"))


def test_assembler_cli_rejects_old_trust_boolean(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "argv", ["assembler", "--studio", "studio", "--signed", "signed",
                                   "--vclibs-package", "vclibs.appx", "--output", "out",
                                   "--archive", "out.zip", "--vclibs-authenticode-valid"])
    with pytest.raises(SystemExit) as exc:
        assemble_main()
    assert exc.value.code == 2
