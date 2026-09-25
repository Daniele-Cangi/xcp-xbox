"""Typed, side-effect-free contracts for the C6 adaptation pipeline."""

from __future__ import annotations

import pathlib
from typing import TYPE_CHECKING, Any, Mapping, Protocol

if TYPE_CHECKING:
    from ..source_models import Detection


class SourceAdapter(Protocol):
    """Versioned engine frontend ending at Source Model and Creative IR."""

    adapter_id: str
    adapter_version: str
    frontend_id: str
    frontend_version: str
    source_model_schema_version: str

    def detect(self, source: pathlib.Path) -> Detection | None:
        ...

    def extract_source_model(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
        *,
        semantic_inventory: dict[str, Any] | None = None,
    ) -> EngineSourceModel:
        ...

    def extract_semantic_inventory(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        ...

    def semantic_pass(self) -> SemanticPass:
        ...

    def emit_creative_ir(
        self,
        source_model: EngineSourceModel,
        semantic_projection: Mapping[str, Any],
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        ...

    def extract(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        """Legacy compatibility method; implementations delegate internally."""
        ...


class EngineSourceModel(Protocol):
    """Canonical, exact-byte-bound model emitted by an engine frontend."""

    document: Mapping[str, Any]
    source_root: pathlib.Path
    inventory: Mapping[str, Any]
    semantic_inventory: Mapping[str, Any]

    @property
    def schema_version(self) -> str:
        ...

    def canonical_bytes(self) -> bytes:
        ...

    def sha256(self) -> str:
        ...

    def verify_exact_source_bytes(self) -> None:
        ...


class SemanticPass(Protocol):
    """Project one Source Model without reading unbound source bytes."""

    pass_id: str
    pass_version: str

    def run(
        self,
        source_model: EngineSourceModel,
    ) -> dict[str, Any]:
        ...


class CreativeIrEmitter(Protocol):
    """Transform a Source Model into the unchanged public Creative IR."""

    pass_id: str
    pass_version: str

    def emit(
        self,
        source_model: EngineSourceModel,
        semantic_projection: Mapping[str, Any],
        inventory: Mapping[str, Any],
    ) -> dict[str, Any]:
        ...


class CapabilityPlanner(Protocol):
    """Compare Creative IR with one exact Creative Host profile."""

    def create_plan(
        self,
        inventory: dict[str, Any],
        ir: dict[str, Any],
        host_profile_path: pathlib.Path,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        ...


class XcpLoweringBackend(Protocol):
    """Lower a host-admitted plan to an ordinary XCP project."""

    def generate_project(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
        ir: dict[str, Any],
        plan: dict[str, Any],
        output: pathlib.Path,
        *,
        project_version: str = "1.3.2",
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        ...


class FidelityOracle(Protocol):
    """Produce fail-closed source-fidelity evidence for one C6 run."""

    def probe_fidelity(
        self,
        run_root: pathlib.Path,
        output_path: pathlib.Path,
        *,
        state_evidence_paths: tuple[pathlib.Path, ...] = (),
        human_playtest_outcome: str = "not_run",
        human_evidence_refs: tuple[str, ...] = (),
    ) -> dict[str, Any]:
        ...
