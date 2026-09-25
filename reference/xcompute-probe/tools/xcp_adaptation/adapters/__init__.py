"""Versioned ecosystem adapter registry."""

from .godot2_legacy import GODOT_NUMBER, Godot2SourceAdapter
from .registry import ADAPTERS, detect_source, inspect_source

__all__ = [
    "ADAPTERS",
    "GODOT_NUMBER",
    "Godot2SourceAdapter",
    "detect_source",
    "inspect_source",
]
