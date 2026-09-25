"""Bounded Godot 4 structural observer; script behavior is reported unsupported."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

from ..contracts import ContractError, SemanticIR, SourceModel, SourceObservation, verify_source_chain

_NODE = re.compile(r'^\[node\s+name="([^"]+)"\s+type="([^"]+)"(?:\s+parent="([^"]+)")?[^\]]*\]$')
_MAIN_SCENE = re.compile(r'^run/main_scene="res://([^"\\]+\.tscn)"$', re.MULTILINE)


class GodotAdapter:
    adapter_id = "godot4.structure"
    adapter_version = "0.1.0"

    def observe(self, root: Path) -> tuple[SourceObservation, SourceModel, SemanticIR]:
        root = root.resolve()
        descriptor = root / "project.godot"
        if not descriptor.is_file():
            raise ContractError("xcp.source.godot_descriptor_missing", "Expected a Godot 4 project.godot")
        files: list[dict[str, object]] = []
        paths: list[Path] = []
        for path in root.rglob("*"):
            relative = path.relative_to(root)
            if relative.parts[0] in {".git", ".godot"}:
                continue
            if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
                raise ContractError("xcp.source.symlink_rejected", f"Symlink in source: {relative.as_posix()}")
            if path.is_file():
                paths.append(path)
        if len(paths) > 10_000:
            raise ContractError("xcp.source.file_limit_exceeded", "Source tree exceeds 10,000 files")
        for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix()):
            relative = path.relative_to(root).as_posix()
            size = path.stat().st_size
            if size > 8 * 1024**3:
                raise ContractError("xcp.source.file_limit_exceeded", f"File exceeds 8 GiB: {relative}")
            hasher = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    hasher.update(chunk)
            files.append({"path": relative, "bytes": size, "sha256": hasher.hexdigest()})
        observation = SourceObservation(self.adapter_id, self.adapter_version, tuple(files))
        records: list[dict[str, object]] = []
        diagnostics: list[str] = []
        try:
            descriptor_text = descriptor.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError as exc:
            raise ContractError("xcp.source.godot_descriptor_encoding", "project.godot is not UTF-8") from exc
        entry = _MAIN_SCENE.search(descriptor_text)
        if (entry is None or ".." in Path(entry.group(1)).parts
                or entry.group(1) not in {str(item["path"]) for item in files}):
            raise ContractError("xcp.source.godot_entrypoint_invalid", "main_scene must name an inventoried text scene")
        records.append({"id": "project.root", "kind": "project", "source_path": "project.godot",
                        "source_sha256": next(str(f["sha256"]) for f in files if f["path"] == "project.godot"),
                        "properties": {"main_scene": entry.group(1)}})
        scene_paths = [p for p in paths if p.suffix == ".tscn"]
        for scene_path in sorted(scene_paths, key=lambda p: p.relative_to(root).as_posix()):
            relative = scene_path.relative_to(root).as_posix()
            scene_sha = next(str(f["sha256"]) for f in files if f["path"] == relative)
            scene_id = "scene." + hashlib.sha256(relative.encode()).hexdigest()[:16]
            records.append({"id": scene_id, "kind": "scene", "source_path": relative,
                            "source_sha256": scene_sha, "properties": {"path": relative}})
            try:
                lines = scene_path.read_text(encoding="utf-8-sig").splitlines()
            except UnicodeDecodeError as exc:
                raise ContractError("xcp.source.godot_scene_encoding", relative) from exc
            for line_number, line in enumerate(lines, 1):
                if not line.startswith("[node "):
                    continue
                match = _NODE.match(line)
                if match is None:
                    diagnostics.append(f"unmodeled_node_header:{relative}:{line_number}")
                    continue
                name, node_type, parent = match.groups()
                node_key = f"{relative}:{parent or '.'}/{name}"
                node_id = "node." + hashlib.sha256(node_key.encode()).hexdigest()[:16]
                records.append({"id": node_id, "kind": "node", "source_path": relative,
                                "source_sha256": scene_sha,
                                "properties": {"name": name, "type": node_type, "parent": parent or "."}})
        if not scene_paths:
            diagnostics.append("no_text_scenes")
        if any(p.suffix in {".gd", ".cs"} for p in paths):
            diagnostics.append("script_behavior_unmodeled")
        model = SourceModel(observation.sha256, self.adapter_id, tuple(records), tuple(sorted(diagnostics)))
        required = {"scene.structure.v1"}
        if any(item.startswith("unmodeled_node_header:") for item in diagnostics):
            required.add("scene.unknown_syntax.v1")
        if "script_behavior_unmodeled" in diagnostics:
            required.add("script.gdscript.v1")
        ir = SemanticIR(model.sha256, tuple(records), tuple(sorted(required)))
        verify_source_chain(observation, model, ir)
        return observation, model, ir
