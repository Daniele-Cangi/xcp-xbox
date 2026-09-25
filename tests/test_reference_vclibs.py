"""Synthetic manifest vectors for the external VCLibs dependency inspector."""

from __future__ import annotations

import sys
from pathlib import Path
from zipfile import ZipFile

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_reference_vclibs import PUBLISHER, inspect  # noqa: E402


def package(path: Path, *, name: str = "Microsoft.VCLibs.140.00",
            version: str = "14.0.33519.0", architecture: str = "x64",
            signature: bool = True) -> Path:
    manifest = (
        '<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">'
        f'<Identity Name="{name}" Publisher="{PUBLISHER}" '
        f'Version="{version}" ProcessorArchitecture="{architecture}"/>'
        '</Package>'
    )
    with ZipFile(path, "w") as archive:
        archive.writestr("AppxManifest.xml", manifest)
        if signature:
            archive.writestr("AppxSignature.p7x", b"synthetic")
    return path


def test_compatible_identity_is_reported_without_claiming_signature_verification(tmp_path: Path) -> None:
    result = inspect(package(tmp_path / "sample.appx"))
    assert result["ok"] is True
    assert result["version"] == "14.0.33519.0"
    assert result["signature_cryptographically_verified"] is False
    assert result["redistributed"] is False


@pytest.mark.parametrize(
    "changes",
    [
        {"name": "Microsoft.VCLibs.140.00.Debug"},
        {"version": "14.0.33518.0"},
        {"architecture": "x86"},
        {"signature": False},
    ],
)
def test_incompatible_or_unsigned_package_is_rejected(tmp_path: Path, changes: dict[str, object]) -> None:
    with pytest.raises(ValueError):
        inspect(package(tmp_path / "sample.appx", **changes))
