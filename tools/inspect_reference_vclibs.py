"""Inspect a locally installed Windows SDK VCLibs package for the P4 worker.

This tool reads the package in place. It neither copies nor redistributes the
Microsoft binary; Windows CI separately checks its Authenticode signature.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import BadZipFile, ZipFile


NAME = "Microsoft.VCLibs.140.00"
PUBLISHER = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
MIN_VERSION = (14, 0, 33519, 0)


def inspect(path: Path) -> dict[str, object]:
    with ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError("VCLibs package contains a corrupt member")
        names = set(archive.namelist())
        if "AppxSignature.p7x" not in names:
            raise ValueError("VCLibs package lacks its signature member")
        root = ET.fromstring(archive.read("AppxManifest.xml"))
    identities = [item for item in root.iter() if item.tag.rsplit("}", 1)[-1] == "Identity"]
    if len(identities) != 1:
        raise ValueError("VCLibs package has no unique identity")
    identity = identities[0]
    try:
        version = tuple(int(part) for part in identity.attrib["Version"].split("."))
    except (KeyError, ValueError) as exc:
        raise ValueError("VCLibs version is invalid") from exc
    if (identity.get("Name") != NAME or identity.get("Publisher") != PUBLISHER
            or identity.get("ProcessorArchitecture") != "x64"
            or len(version) != 4 or version < MIN_VERSION):
        raise ValueError("VCLibs identity does not satisfy the P4 worker dependency")
    return {
        "ok": True,
        "name": NAME,
        "version": identity.attrib["Version"],
        "architecture": "x64",
        "publisher": PUBLISHER,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "signature_member_present": True,
        "signature_cryptographically_verified": False,
        "redistributed": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.package)
    except (OSError, BadZipFile, ET.ParseError, ValueError, KeyError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
