"""Exact external-source inventory models and file classification."""

from __future__ import annotations

import mimetypes
import pathlib
from dataclasses import dataclass

from ..core import SourceAdaptError

TEXT_EXTENSIONS = {
    ".cfg",
    ".gd",
    ".json",
    ".md",
    ".po",
    ".pot",
    ".tres",
    ".tscn",
    ".txt",
    ".xml",
}

ASSET_EXTENSIONS = {
    ".bmp",
    ".gif",
    ".jpeg",
    ".jpg",
    ".ogg",
    ".otf",
    ".png",
    ".ttf",
    ".wav",
    ".webp",
}

EXECUTABLE_EXTENSIONS = {
    ".bat",
    ".cmd",
    ".com",
    ".dll",
    ".exe",
    ".msi",
    ".ps1",
    ".sh",
}


def _relative_files(source: pathlib.Path) -> list[pathlib.Path]:
    source = source.resolve()
    if not source.is_dir():
        raise SourceAdaptError(
            "xcp.adapt.source_invalid",
            "The external source root is not a directory.",
            stage="inspect",
            field="source",
            expected="existing source directory",
            actual=str(source),
            correction="select the authorized source tree",
        )
    result: list[pathlib.Path] = []
    for path in source.rglob("*"):
        relative = path.relative_to(source)
        if relative.parts and relative.parts[0] == ".git":
            continue
        if path.is_symlink():
            raise SourceAdaptError(
                "xcp.adapt.source_symlink_rejected",
                "Source inventory does not follow or silently omit symbolic links.",
                stage="inspect",
                field="path",
                expected="regular file or directory",
                actual=relative.as_posix(),
                correction="materialize an authorized regular-file source tree",
            )
        if path.is_file():
            result.append(path)
    result.sort(key=lambda item: item.relative_to(source).as_posix())
    if not result:
        raise SourceAdaptError(
            "xcp.adapt.source_empty",
            "The external source tree contains no inventory files.",
            stage="inspect",
            field="source",
            expected="at least one regular file",
            actual=str(source),
            correction="select the project source root",
        )
    return result


def _media_type(path: pathlib.Path) -> str:
    explicit = {
        ".cfg": "text/plain",
        ".gd": "text/x-gdscript",
        ".ogg": "audio/ogg",
        ".tres": "text/x-godot-resource",
        ".tscn": "text/x-godot-scene",
    }
    return explicit.get(
        path.suffix.lower(),
        mimetypes.guess_type(path.name)[0] or "application/octet-stream",
    )


def _role(path: pathlib.PurePosixPath) -> str:
    text = path.as_posix().lower()
    suffix = path.suffix.lower()
    if path.name.lower() in {"engine.cfg", "project.godot"}:
        return "project_metadata"
    if path.name.lower().startswith(("license", "copying")):
        return "license"
    if suffix in {".md", ".txt"}:
        return "documentation"
    if suffix in {".tscn", ".tres"}:
        return "scene"
    if suffix in {".gd", ".cs"}:
        if "input" in text:
            return "input"
        if "save" in text or "state" in text:
            return "state"
        if "gui" in text or "menu" in text or "ui" in path.parts:
            return "ui"
        return "gameplay"
    if suffix in {".ogg", ".wav"}:
        return "audio"
    if suffix in ASSET_EXTENSIONS:
        return "asset"
    return "unknown"


@dataclass(frozen=True)
class Detection:
    adapter_id: str
    adapter_version: str
    ecosystem: str
    ecosystem_version: str
    confidence: float
    entrypoints: tuple[str, ...]
    evidence: tuple[str, ...]
