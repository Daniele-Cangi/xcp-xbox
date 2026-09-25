"""Source inventory models used by ecosystem adapters."""

from .engine_assisted import (
    GodotEngineAssistedExtractor,
    GodotEngineInvocation,
    canonical_engine_output_bytes,
)
from .godot import (
    GodotSourceModel,
    GodotSourceModelError,
    build_file_parser_source_model,
    build_godot_source_model_document,
    validate_godot_source_model_document,
)
from .inventory import (
    ASSET_EXTENSIONS,
    EXECUTABLE_EXTENSIONS,
    TEXT_EXTENSIONS,
    Detection,
    _media_type,
    _relative_files,
    _role,
)

__all__ = [
    "ASSET_EXTENSIONS",
    "Detection",
    "EXECUTABLE_EXTENSIONS",
    "GodotEngineAssistedExtractor",
    "GodotEngineInvocation",
    "GodotSourceModel",
    "GodotSourceModelError",
    "TEXT_EXTENSIONS",
    "_media_type",
    "_relative_files",
    "_role",
    "build_file_parser_source_model",
    "build_godot_source_model_document",
    "canonical_engine_output_bytes",
    "validate_godot_source_model_document",
]
