"""Contain untrusted distribution manifest paths before filesystem access."""

from __future__ import annotations

import pathlib


def contained(base: pathlib.Path, name: str, *, filename: bool = False) -> pathlib.Path:
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError("invalid distribution manifest path")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts) or (filename and len(parts) != 1):
        raise ValueError("distribution manifest path escapes its directory")
    path = base.joinpath(*parts)
    if not path.resolve().is_relative_to(base.resolve()):
        raise ValueError("distribution manifest path escapes its directory")
    return path
