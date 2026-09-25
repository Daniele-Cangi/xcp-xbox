"""Fail-closed PC-side runner for future Godot engine-assisted frontends."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Any, Callable, Mapping, Sequence

from xcp_creative_project import canonical_json_bytes

from .godot import (
    GodotSourceModelError,
    validate_godot_source_model_document,
)


MAX_MODEL_BYTES = 64 * 1024 * 1024
DEFAULT_TIMEOUT_SECONDS = 180


@dataclass(frozen=True)
class GodotEngineInvocation:
    executable: pathlib.Path
    extractor_script: pathlib.Path
    frontend_id: str
    frontend_version: str
    expected_engine_major: int
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS

    def command(
        self,
        source_root: pathlib.Path,
        output_path: pathlib.Path,
    ) -> tuple[str, ...]:
        return (
            str(self.executable.resolve()),
            "--headless",
            "--path",
            str(source_root.resolve()),
            "--script",
            str(self.extractor_script.resolve()),
            "--",
            "--xcp-source-model-output",
            str(output_path),
            "--xcp-frontend-id",
            self.frontend_id,
            "--xcp-frontend-version",
            self.frontend_version,
        )


class GodotEngineAssistedExtractor:
    """Execute a compatible Godot binary without shell interpretation."""

    def __init__(
        self,
        invocation: GodotEngineInvocation,
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self._invocation = invocation
        self._runner = runner

    def extract_document(
        self,
        source_root: pathlib.Path,
    ) -> dict[str, Any]:
        if source_root.is_symlink():
            raise GodotSourceModelError(
                "engine-assisted source root must be a non-symlink directory"
            )
        source_root = source_root.resolve()
        if not source_root.is_dir():
            raise GodotSourceModelError(
                "engine-assisted source root must be a non-symlink directory"
            )
        if self._invocation.executable.is_symlink():
            raise GodotSourceModelError(
                "Godot engine executable must be an exact regular file"
            )
        executable = self._invocation.executable.resolve()
        if not executable.is_file():
            raise GodotSourceModelError(
                "Godot engine executable must be an exact regular file"
            )
        if self._invocation.extractor_script.is_symlink():
            raise GodotSourceModelError(
                "Godot extractor script must be an exact regular file"
            )
        extractor_script = self._invocation.extractor_script.resolve()
        if not extractor_script.is_file():
            raise GodotSourceModelError(
                "Godot extractor script must be an exact regular file"
            )
        if self._invocation.timeout_seconds <= 0:
            raise GodotSourceModelError(
                "engine-assisted timeout must be positive"
            )

        with tempfile.TemporaryDirectory(prefix="xcp-godot-model-") as raw:
            private_root = pathlib.Path(raw).resolve()
            output_path = private_root / "godot-source-model-v1.json"
            command = self._invocation.command(source_root, output_path)
            environment = {
                "PATH": os.environ.get("PATH", ""),
                "SYSTEMROOT": os.environ.get("SYSTEMROOT", ""),
                "TEMP": str(private_root),
                "TMP": str(private_root),
            }
            completed = self._runner(
                list(command),
                cwd=str(source_root),
                env=environment,
                shell=False,
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=self._invocation.timeout_seconds,
            )
            if completed.returncode != 0:
                stderr = str(completed.stderr)[-4096:]
                raise GodotSourceModelError(
                    "Godot headless extractor failed with "
                    f"exit {completed.returncode}: {stderr}"
                )
            if not output_path.is_file() or output_path.is_symlink():
                raise GodotSourceModelError(
                    "Godot headless extractor did not produce its exact output"
                )
            size = output_path.stat().st_size
            if size <= 0 or size > MAX_MODEL_BYTES:
                raise GodotSourceModelError(
                    "Godot Source Model output size is outside the bound"
                )
            try:
                document = json.loads(output_path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise GodotSourceModelError(
                    "Godot headless extractor output is not valid UTF-8 JSON"
                ) from exc
            if not isinstance(document, Mapping):
                raise GodotSourceModelError(
                    "Godot headless extractor output must be an object"
                )
            result = dict(document)
            validate_godot_source_model_document(result)
            frontend = result["frontend"]
            if frontend["frontend_id"] != self._invocation.frontend_id:
                raise GodotSourceModelError(
                    "engine-assisted frontend identity mismatch"
                )
            if frontend["frontend_version"] != self._invocation.frontend_version:
                raise GodotSourceModelError(
                    "engine-assisted frontend version mismatch"
                )
            engine_version = str(frontend.get("engine_version", ""))
            if not engine_version.startswith(
                f"{self._invocation.expected_engine_major}."
            ):
                raise GodotSourceModelError(
                    "engine-assisted Godot major version mismatch"
                )
            canonical_json_bytes(result)
            return result


def canonical_engine_output_bytes(document: Mapping[str, Any]) -> bytes:
    """Return the only accepted serialized form of an engine extraction."""

    validate_godot_source_model_document(document)
    return canonical_json_bytes(dict(document))
