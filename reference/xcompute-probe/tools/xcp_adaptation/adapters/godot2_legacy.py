"""Legacy Godot 2 ecosystem adapter preserved byte-for-byte for G1."""

from __future__ import annotations

import json
import pathlib
import re
from typing import Any

from xcp_creative_project import sha256_bytes

from ..core import (
    SourceAdaptError,
    _document_sha,
    _portable_id,
    _validate,
)
from ..ir import _validate_ir_integrity
from ..semantics import (
    Godot2CreativeIrPass,
    GodotSemanticInventoryPass,
    SEMANTIC_CATEGORIES,
    _validate_semantic_inventory_integrity,
)
from ..source_models import (
    ASSET_EXTENSIONS,
    Detection,
    GodotSourceModel,
    build_file_parser_source_model,
)

GODOT_NUMBER = r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"


class Godot2SourceAdapter:
    adapter_id = "godot.source-adapter"
    adapter_version = "2.4.0"
    frontend_id = "godot2.frontend"
    frontend_version = "1.0.0"
    source_model_schema_version = "godot-source-model-v1"

    def detect(self, source: pathlib.Path) -> Detection | None:
        config = source / "engine.cfg"
        if not config.is_file():
            return None
        try:
            text = config.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            return None
        evidence = ["engine.cfg is the Godot 2 project descriptor"]
        if "[application]" in text and "main_scene=" in text:
            evidence.append("engine.cfg declares application.main_scene")
        version = "2.x"
        match = re.search(
            r"(?ms)^\[application\.config\].*?^version_major=\"?([0-9]+)\"?",
            text,
        )
        if match:
            evidence.append("application.config contains project version metadata")
        return Detection(
            adapter_id=self.adapter_id,
            adapter_version=self.adapter_version,
            ecosystem="godot_2_x",
            ecosystem_version=version,
            confidence=1.0,
            entrypoints=("engine.cfg",),
            evidence=tuple(evidence),
        )

    @staticmethod
    def _configuration(path: pathlib.Path) -> dict[str, dict[str, str]]:
        sections: dict[str, dict[str, str]] = {}
        current = ""
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith((";", "#")):
                continue
            section = re.fullmatch(r"\[([^\]]+)\]", line)
            if section:
                current = section.group(1)
                sections.setdefault(current, {})
                continue
            if "=" in line and current:
                key, value = line.split("=", 1)
                sections[current][key.strip()] = value.strip()
        return sections

    @staticmethod
    def _unquote(value: str) -> str:
        if len(value) >= 2 and value[0] == value[-1] == '"':
            return value[1:-1]
        return value

    @staticmethod
    def _file_maps(
        inventory: dict[str, Any],
    ) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
        by_path = {str(item["path"]): item for item in inventory["files"]}
        ids = {path: str(item["file_id"]) for path, item in by_path.items()}
        return by_path, ids

    @staticmethod
    def _read_source(source: pathlib.Path, relative: str) -> str:
        path = source / pathlib.PurePosixPath(relative)
        if path.stat().st_size > 16 * 1024 * 1024:
            raise SourceAdaptError(
                "xcp.adapt.semantic_source_too_large",
                "A semantic source document exceeds the bounded parser ceiling.",
                stage="extract",
                field="path",
                expected="at most 16777216 bytes",
                actual=relative,
                correction="split the source document or provide an adapter extension",
            )
        return path.read_text(encoding="utf-8", errors="strict")

    @staticmethod
    def _signed_cell_component(value: int) -> int:
        return value - 65536 if value >= 32768 else value

    @staticmethod
    def _display_texts(text: str) -> list[str]:
        result: list[str] = []
        for match in re.finditer(r'(?m)^text\s*=\s*"((?:[^"\\]|\\.)*)"', text):
            encoded = match.group(1)
            try:
                value = json.loads(f'"{encoded}"')
            except json.JSONDecodeError:
                value = encoded.replace(r"\n", "\n")
            normalized = " ".join(str(value).split())
            if normalized and normalized not in result:
                result.append(normalized[:512])
            if len(result) >= 32:
                break
        return result

    @staticmethod
    def _semantic_property(name: str, value: str) -> dict[str, str]:
        normalized = " ".join(value.strip().split())
        return {
            "name": name[:256],
            "value_sha256": sha256_bytes(value.encode("utf-8")),
            "preview": normalized[:256],
        }

    @classmethod
    def _semantic_properties(cls, block: str) -> list[dict[str, str]]:
        properties: list[dict[str, str]] = []
        for raw in block.splitlines():
            line = raw.strip()
            if not line or line.startswith(("#", ";", "[")) or "=" not in line:
                continue
            name, value = line.split("=", 1)
            name = name.strip()
            if not name:
                continue
            properties.append(cls._semantic_property(name, value))
            if len(properties) >= 4096:
                break
        return properties

    @staticmethod
    def _semantic_dependencies(block: str) -> list[str]:
        return sorted(
            {
                match.group(1).replace("\\", "/").rstrip("/")
                for match in re.finditer(
                    r'["\']res://([^"\']+)["\']',
                    block,
                )
                if match.group(1).replace("\\", "/").rstrip("/")
            }
        )[:4096]

    @staticmethod
    def _header_attribute(header: str, name: str) -> str:
        quoted = re.search(rf'(?:^|\s){re.escape(name)}="([^"]*)"', header)
        if quoted:
            return quoted.group(1)
        bare = re.search(rf"(?:^|\s){re.escape(name)}=([^\s]+)", header)
        return bare.group(1) if bare else ""

    @classmethod
    def _semantic_record(
        cls,
        *,
        category: str,
        kind: str,
        source_ref: str,
        path: str,
        line_start: int,
        line_end: int,
        name: str,
        content: str,
        identity_suffix: str,
        properties: list[dict[str, str]] | None = None,
        dependencies: list[str] | None = None,
    ) -> dict[str, Any]:
        return {
            "semantic_id": _portable_id(
                f"{kind}.{path}.{line_start}.{identity_suffix}",
                prefix="semantic",
                maximum=128,
            ),
            "category": category,
            "kind": kind,
            "source_refs": [source_ref],
            "locator": {
                "path": path,
                "line_start": max(1, line_start),
                "line_end": max(max(1, line_start), line_end),
            },
            "name": (name or kind)[:512],
            "content_sha256": sha256_bytes(content.encode("utf-8")),
            "properties": (
                properties
                if properties is not None
                else cls._semantic_properties(content)
            ),
            "dependencies": (
                dependencies
                if dependencies is not None
                else cls._semantic_dependencies(content)
            ),
        }

    @classmethod
    def _godot_resource_semantics(
        cls,
        text: str,
        *,
        path: str,
        source_ref: str,
    ) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        headings = list(re.finditer(r"(?m)^\[([^\]\r\n]+)\]\s*$", text))
        line_offsets = [0]
        line_offsets.extend(
            match.end()
            for match in re.finditer(r"\n", text)
        )

        def line_number(offset: int) -> int:
            lo, hi = 0, len(line_offsets)
            while lo < hi:
                mid = (lo + hi) // 2
                if line_offsets[mid] <= offset:
                    lo = mid + 1
                else:
                    hi = mid
            return max(1, lo)

        rendering_types = {
            "Sprite",
            "AnimatedSprite",
            "TileMap",
            "CanvasLayer",
            "Camera2D",
            "Light2D",
            "Particles2D",
            "Polygon2D",
        }
        ui_types = {
            "Control",
            "Panel",
            "Button",
            "Label",
            "Container",
            "VBoxContainer",
            "HBoxContainer",
            "TextureButton",
            "TextureRect",
            "LineEdit",
            "CheckBox",
            "OptionButton",
        }
        audio_types = {
            "SamplePlayer",
            "SamplePlayer2D",
            "StreamPlayer",
            "AudioStreamPlayer",
            "AudioStreamPlayer2D",
        }
        for index, heading_match in enumerate(headings):
            start = heading_match.start()
            end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
            block = text[start:end]
            header = heading_match.group(1).strip()
            token = header.split(None, 1)[0]
            line_start = line_number(start)
            line_end = line_number(max(start, end - 1))
            if token in {"gd_scene", "gd_resource"}:
                records.append(
                    cls._semantic_record(
                        category="resource",
                        kind="godot.resource.document",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=line_end,
                        name=header,
                        content=block,
                        identity_suffix=f"document.{index}",
                    )
                )
                continue
            if token == "ext_resource":
                resource_path = cls._header_attribute(header, "path")
                resource_type = cls._header_attribute(header, "type") or "Unknown"
                records.append(
                    cls._semantic_record(
                        category="resource",
                        kind="godot.resource.external",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=line_end,
                        name=f"{resource_type}:{resource_path}",
                        content=block,
                        identity_suffix=f"external.{index}",
                    )
                )
                continue
            if token == "sub_resource":
                resource_type = cls._header_attribute(header, "type") or "Unknown"
                category = (
                    "animation"
                    if "Animation" in resource_type
                    else "collision_physics"
                    if "Shape" in resource_type
                    else "rendering"
                    if resource_type.startswith(("StyleBox", "ImageTexture"))
                    else "resource"
                )
                records.append(
                    cls._semantic_record(
                        category=category,
                        kind=f"godot.{category}.subresource",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=line_end,
                        name=resource_type,
                        content=block,
                        identity_suffix=f"subresource.{index}",
                    )
                )
                continue
            if token == "connection":
                signal = cls._header_attribute(header, "signal") or "connection"
                records.append(
                    cls._semantic_record(
                        category="lifecycle",
                        kind="godot.lifecycle.signal_connection",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=line_end,
                        name=signal,
                        content=block,
                        identity_suffix=f"connection.{index}",
                    )
                )
                continue
            if token != "node":
                records.append(
                    cls._semantic_record(
                        category="resource",
                        kind="godot.resource.section",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=line_end,
                        name=header,
                        content=block,
                        identity_suffix=f"section.{index}",
                    )
                )
                continue

            node_name = cls._header_attribute(header, "name") or f"node-{index}"
            node_type = cls._header_attribute(header, "type")
            if not node_type and "instance=" in header:
                node_type = "PackedSceneInstance"
            node_type = node_type or "InheritedNode"
            node_properties = cls._semantic_properties(block)
            dependencies = cls._semantic_dependencies(block)
            common = {
                "source_ref": source_ref,
                "path": path,
                "line_start": line_start,
                "line_end": line_end,
                "name": f"{node_name}:{node_type}",
                "content": block,
                "properties": node_properties,
                "dependencies": dependencies,
            }
            records.append(
                cls._semantic_record(
                    category="scene_graph",
                    kind="godot.scene.node",
                    identity_suffix=f"node.{index}",
                    **common,
                )
            )
            feature_categories: list[str] = []
            if node_type in rendering_types or node_type == "TileMap":
                feature_categories.append("rendering")
            if node_type in ui_types or any(
                node_type.endswith(suffix)
                for suffix in ("Button", "Label", "Container")
            ):
                feature_categories.append("ui")
            if (
                "Collision" in node_type
                or node_type in {"Area2D", "KinematicBody2D", "StaticBody2D", "RigidBody2D"}
                or node_type == "TileMap"
            ):
                feature_categories.append("collision_physics")
            if node_type in {"AnimationPlayer", "AnimatedSprite"}:
                feature_categories.append("animation")
            if node_type in audio_types or "Audio" in node_type or "SamplePlayer" in node_type:
                feature_categories.append("audio")
            if any(prop["name"] == "script/script" for prop in node_properties):
                feature_categories.append("behavior")
            for category in sorted(set(feature_categories)):
                records.append(
                    cls._semantic_record(
                        category=category,
                        kind=f"godot.{category}.node_feature",
                        identity_suffix=f"node_feature.{category}.{index}",
                        **common,
                    )
                )
        return records

    @classmethod
    def _godot_script_semantics(
        cls,
        text: str,
        *,
        path: str,
        source_ref: str,
    ) -> list[dict[str, Any]]:
        records = [
            cls._semantic_record(
                category="resource",
                kind="godot.resource.script",
                source_ref=source_ref,
                path=path,
                line_start=1,
                line_end=max(1, len(text.splitlines())),
                name=path,
                content=text,
                identity_suffix="script",
            )
        ]
        lines = text.splitlines()
        function_starts = [
            (index + 1, match.group(1))
            for index, line in enumerate(lines)
            if (match := re.match(r"^\s*func\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", line))
        ]
        for ordinal, (line_start, name) in enumerate(function_starts):
            line_end = (
                function_starts[ordinal + 1][0] - 1
                if ordinal + 1 < len(function_starts)
                else len(lines)
            )
            block = "\n".join(lines[line_start - 1 : line_end])
            records.append(
                cls._semantic_record(
                    category="behavior",
                    kind="godot.behavior.function",
                    source_ref=source_ref,
                    path=path,
                    line_start=line_start,
                    line_end=max(line_start, line_end),
                    name=name,
                    content=block,
                    identity_suffix=f"function.{ordinal}.{name}",
                )
            )
            for action_ordinal, action in enumerate(
                sorted(
                    set(
                        re.findall(
                            r'Input\.is_action_[A-Za-z_]+\(\s*"([^"]+)"',
                            block,
                        )
                    )
                )
            ):
                records.append(
                    cls._semantic_record(
                        category="input",
                        kind="godot.input.action_usage",
                        source_ref=source_ref,
                        path=path,
                        line_start=line_start,
                        line_end=max(line_start, line_end),
                        name=action,
                        content=f"{name}:{action}",
                        identity_suffix=f"input.{ordinal}.{action_ordinal}.{action}",
                        properties=[
                            cls._semantic_property("action", action),
                            cls._semantic_property("function", name),
                        ],
                    )
                )
        for index, line in enumerate(lines, start=1):
            signal = re.match(r"^\s*signal\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            if signal:
                records.append(
                    cls._semantic_record(
                        category="lifecycle",
                        kind="godot.lifecycle.signal",
                        source_ref=source_ref,
                        path=path,
                        line_start=index,
                        line_end=index,
                        name=signal.group(1),
                        content=line,
                        identity_suffix=f"signal.{index}.{signal.group(1)}",
                    )
                )
            variable = re.match(
                r"^(?P<prefix>(?:(?:export(?:\([^)]*\))?|onready)\s+)*)"
                r"(?P<declaration>var|const)\s+"
                r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)",
                line,
            )
            if variable:
                variable_name = variable.group("name")
                persisted_member = (
                    path == "shared/save_manager.gd"
                    and variable_name == "save_path"
                ) or (
                    path == "shared/settings_manager.gd"
                    and variable_name
                    in {
                        "default_settings",
                        "options_path",
                        "cache",
                    }
                )
                records.append(
                    cls._semantic_record(
                        category=(
                            "state_persistence"
                            if persisted_member
                            else "behavior"
                        ),
                        kind=(
                            "godot.state.persisted_member"
                            if persisted_member
                            else "godot.behavior.member_state"
                        ),
                        source_ref=source_ref,
                        path=path,
                        line_start=index,
                        line_end=index,
                        name=variable_name,
                        content=line,
                        identity_suffix=(
                            f"{variable.group('declaration')}."
                            f"{index}.{variable_name}"
                        ),
                    )
                )
        return records

    @classmethod
    def _godot_config_semantics(
        cls,
        text: str,
        *,
        path: str,
        source_ref: str,
    ) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        section = ""
        for index, raw in enumerate(text.splitlines(), start=1):
            line = raw.strip()
            section_match = re.fullmatch(r"\[([^\]]+)\]", line)
            if section_match:
                section = section_match.group(1)
                continue
            if not line or line.startswith(("#", ";")) or "=" not in line:
                continue
            key, value = line.split("=", 1)
            category = (
                "input"
                if section == "input"
                else "lifecycle"
                if section in {"application", "autoload"}
                else "rendering"
                if section in {"display", "render", "image_loader"}
                else "project_configuration"
            )
            kind = (
                "godot.input.action"
                if category == "input"
                else "godot.lifecycle.autoload"
                if section == "autoload"
                else "godot.lifecycle.entrypoint"
                if section == "application" and key.strip() == "main_scene"
                else f"godot.{category}.setting"
            )
            records.append(
                cls._semantic_record(
                    category=category,
                    kind=kind,
                    source_ref=source_ref,
                    path=path,
                    line_start=index,
                    line_end=index,
                    name=f"{section}.{key.strip()}",
                    content=raw,
                    identity_suffix=f"{section}.{key.strip()}.{index}",
                    properties=[cls._semantic_property(key.strip(), value)],
                )
            )
        return records

    def extract_semantic_inventory(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        by_path, file_ids = self._file_maps(inventory)
        records: list[dict[str, Any]] = []
        included: list[str] = []
        excluded: list[dict[str, str]] = []
        failures: list[dict[str, str]] = []
        asset_extensions = ASSET_EXTENSIONS | {".fnt"}
        resource_extensions = {".tscn", ".tres"}
        for relative, item in sorted(by_path.items()):
            source_ref = file_ids[relative]
            suffix = pathlib.PurePosixPath(relative).suffix.lower()
            role = str(item["role"])
            if role in {"documentation", "license"}:
                excluded.append(
                    {
                        "source_ref": source_ref,
                        "reason": role,
                    }
                )
                continue
            try:
                file_records: list[dict[str, Any]]
                if relative == "engine.cfg":
                    text = self._read_source(source, relative)
                    file_records = self._godot_config_semantics(
                        text,
                        path=relative,
                        source_ref=source_ref,
                    )
                elif suffix in resource_extensions:
                    text = self._read_source(source, relative)
                    file_records = self._godot_resource_semantics(
                        text,
                        path=relative,
                        source_ref=source_ref,
                    )
                elif suffix == ".gd":
                    text = self._read_source(source, relative)
                    file_records = self._godot_script_semantics(
                        text,
                        path=relative,
                        source_ref=source_ref,
                    )
                elif suffix in asset_extensions:
                    category = (
                        "audio"
                        if str(item["media_type"]).startswith("audio/")
                        else "asset"
                    )
                    file_records = [
                        self._semantic_record(
                            category=category,
                            kind=(
                                "godot.audio.asset"
                                if category == "audio"
                                else "godot.asset.source"
                            ),
                            source_ref=source_ref,
                            path=relative,
                            line_start=1,
                            line_end=1,
                            name=relative,
                            content=str(item["sha256"]),
                            identity_suffix="asset",
                            properties=[
                                self._semantic_property(
                                    "media_type",
                                    str(item["media_type"]),
                                ),
                                self._semantic_property(
                                    "bytes",
                                    str(item["bytes"]),
                                ),
                            ],
                            dependencies=[],
                        )
                    ]
                else:
                    excluded.append(
                        {
                            "source_ref": source_ref,
                            "reason": (
                                "build_metadata"
                                if suffix in {".import", ".flags", ".translation"}
                                else "non_semantic_unknown"
                            ),
                        }
                    )
                    continue
                included.append(source_ref)
                records.extend(file_records)
            except (OSError, UnicodeDecodeError, SourceAdaptError) as exc:
                included.append(source_ref)
                failures.append(
                    {
                        "source_ref": source_ref,
                        "code": "xcp.adapt.semantic_parse_failed",
                        "details": f"{relative}: {exc}"[:2048],
                    }
                )

        semantic_ids = [str(record["semantic_id"]) for record in records]
        if len(semantic_ids) != len(set(semantic_ids)):
            raise SourceAdaptError(
                "xcp.adapt.semantic_id_duplicate",
                "Whole-project semantic inventory ids must be globally unique.",
                stage="semantic_inventory",
                field="records[].semantic_id",
                expected="unique semantic ids",
                actual=str(len(semantic_ids) - len(set(semantic_ids))),
                correction="repair deterministic semantic id generation",
            )
        category_counts = {
            category: sum(record["category"] == category for record in records)
            for category in SEMANTIC_CATEGORIES
        }
        semantic_inventory = {
            "schema_version": "xcp-creative-semantic-inventory-v1",
            "semantic_inventory_id": f"{inventory['inventory_id']}.semantics",
            "source_inventory_sha256": _document_sha(inventory),
            "producer": {
                "adapter_id": self.adapter_id,
                "adapter_version": self.adapter_version,
                "parser_contract": "lossless_source_accounting_bounded_parse_v1",
            },
            "scope": {
                "mode": "whole_project",
                "included_source_refs": sorted(set(included)),
                "excluded_sources": sorted(
                    excluded,
                    key=lambda entry: entry["source_ref"],
                ),
                "parse_failures": sorted(
                    failures,
                    key=lambda entry: entry["source_ref"],
                ),
            },
            "records": records,
            "summary": {
                "source_file_count": len(inventory["files"]),
                "included_source_count": len(set(included)),
                "excluded_source_count": len(excluded),
                "parse_failure_count": len(failures),
                "record_count": len(records),
                "category_counts": category_counts,
            },
        }
        _validate(semantic_inventory, "semantic_inventory")
        _validate_semantic_inventory_integrity(semantic_inventory, inventory)
        return semantic_inventory

    @staticmethod
    def _scene_visual_asset(text: str, scene_path: str) -> str:
        candidates = [
            match.group(1).removeprefix("res://")
            for match in re.finditer(
                r'(?m)^\[ext_resource path="([^"]+\.(?:png|jpg|jpeg))" '
                r'type="Texture" id=[0-9]+\]',
                text,
                flags=re.IGNORECASE,
            )
        ]
        if not candidates:
            return ""
        scene_stem = pathlib.PurePosixPath(scene_path).stem.lower()
        preferred = next(
            (
                candidate
                for candidate in candidates
                if pathlib.PurePosixPath(candidate).stem.lower().startswith(
                    scene_stem
                )
            ),
            "",
        )
        return preferred or candidates[0]

    @classmethod
    def _tileset_visual_blueprint(
        cls,
        text: str,
    ) -> list[str]:
        resources = {
            int(match.group(2)): match.group(1).removeprefix("res://")
            for match in re.finditer(
                r'(?m)^\[ext_resource path="([^"]+)" '
                r'type="Texture" id=([0-9]+)\]',
                text,
            )
        }
        starts = list(
            re.finditer(r'(?m)^([0-9]+)/name\s*=\s*"([^"]+)"', text)
        )
        visuals: list[str] = []
        for index, match in enumerate(starts):
            tile_id = int(match.group(1))
            name = match.group(2)
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            block = text[match.start() : end]
            texture = re.search(
                rf"(?m)^{tile_id}/texture\s*=\s*ExtResource\(\s*([0-9]+)\s*\)",
                block,
            )
            if not texture:
                continue
            asset_path = resources.get(int(texture.group(1)), "")
            if not asset_path:
                continue
            offset = re.search(
                rf"(?m)^{tile_id}/tex_offset\s*=\s*Vector2\(\s*"
                rf"({GODOT_NUMBER})\s*,\s*({GODOT_NUMBER})\s*\)",
                block,
            )
            region = re.search(
                rf"(?m)^{tile_id}/region\s*=\s*Rect2\(\s*"
                rf"({GODOT_NUMBER})\s*,\s*({GODOT_NUMBER})\s*,\s*"
                rf"({GODOT_NUMBER})\s*,\s*({GODOT_NUMBER})\s*\)",
                block,
            )
            offset_x = float(offset.group(1)) if offset else 0.0
            offset_y = float(offset.group(2)) if offset else 0.0
            region_values = (
                [
                    float(region.group(1)),
                    float(region.group(2)),
                    float(region.group(3)),
                    float(region.group(4)),
                ]
                if region
                else [0.0, 0.0, 0.0, 0.0]
            )
            visuals.append(
                "|".join(
                    (
                        str(tile_id),
                        name,
                        asset_path,
                        *[
                            format(value, ".12g")
                            for value in (
                                *region_values,
                                offset_x,
                                offset_y,
                            )
                        ],
                    )
                )
            )
        return visuals

    @classmethod
    def _grid_blueprint(
        cls,
        text: str,
        instance_visual_assets: dict[str, str],
    ) -> dict[str, Any]:
        cell_size_match = re.search(
            r"(?m)^cell/size\s*=\s*Vector2\(\s*"
            rf"({GODOT_NUMBER})\s*,\s*({GODOT_NUMBER})\s*\)",
            text,
        )
        cell_size = (
            max(1, int(float(cell_size_match.group(1))))
            if cell_size_match
            else 64
        )
        tile_cells: list[str] = []
        tile_data = re.search(
            r"(?m)^tile_data\s*=\s*IntArray\(\s*([^)]*)\)",
            text,
        )
        if tile_data:
            values = [
                int(value)
                for value in re.findall(r"-?[0-9]+", tile_data.group(1))
            ]
            for index in range(0, len(values) - 1, 2):
                packed = values[index] & 0xFFFFFFFF
                x = cls._signed_cell_component(packed & 0xFFFF)
                y = cls._signed_cell_component((packed >> 16) & 0xFFFF)
                tile_id = values[index + 1]
                kind = (
                    "hazard"
                    if tile_id == 2
                    else "climbable"
                    if tile_id == 1
                    else "floor"
                )
                tile_cells.append(f"{x},{y},{kind},{tile_id}")

        resources = {
            int(match.group(2)): match.group(1).removeprefix("res://")
            for match in re.finditer(
                r'(?m)^\[ext_resource path="([^"]+)" '
                r'type="[^"]+" id=([0-9]+)\]',
                text,
            )
        }
        nodes = list(
            re.finditer(
                r'(?m)^\[node name="([^"]+)"([^\]]*)\]\s*$',
                text,
            )
        )
        instances: list[str] = []
        first_instance_by_identity: dict[
            tuple[str, int, int, str], str
        ] = {}
        duplicate_instances: list[str] = []
        start_cell: tuple[int, int] | None = None
        for index, node in enumerate(nodes):
            body_start = node.end()
            body_end = (
                nodes[index + 1].start()
                if index + 1 < len(nodes)
                else len(text)
            )
            body = text[body_start:body_end]
            position = re.search(
                r"(?m)^transform/pos\s*=\s*Vector2\(\s*"
                rf"({GODOT_NUMBER})\s*,\s*({GODOT_NUMBER})\s*\)",
                body,
            )
            if not position:
                continue
            x = int(round(float(position.group(1)) / cell_size))
            y = int(round(float(position.group(2)) / cell_size))
            name = node.group(1)
            if name == "start":
                start_cell = (x, y)
            instance = re.search(
                r"instance=ExtResource\(\s*([0-9]+)\s*\)",
                node.group(2),
            )
            if instance:
                source_path = resources.get(int(instance.group(1)), "")
                visual_asset = instance_visual_assets.get(source_path, "")
                identity = (source_path, x, y, visual_asset)
                first_name = first_instance_by_identity.get(identity)
                if first_name is None:
                    first_instance_by_identity[identity] = name
                else:
                    duplicate_instances.append(
                        f"{name}|{first_name}|{source_path}|{x}|{y}|"
                        f"{visual_asset}"
                    )
                instances.append(
                    f"{name}|{source_path}|{x}|{y}|{visual_asset}"
                )
        result: dict[str, Any] = {
            "grid_cell_size": cell_size,
            "grid_tiles": tile_cells,
            "grid_instances": instances,
        }
        if start_cell is not None:
            result["grid_player_start"] = [
                start_cell[0],
                start_cell[1],
            ]
        if duplicate_instances:
            result["grid_duplicate_instances"] = duplicate_instances
        return result

    @classmethod
    def _audio_event_bindings(
        cls,
        source: pathlib.Path,
        by_path: dict[str, dict[str, Any]],
    ) -> dict[str, str]:
        bindings: dict[str, str] = {}
        entity_path = "entities/entity.gd"
        if entity_path in by_path:
            text = cls._read_source(source, entity_path)
            for source_key, event in (
                ("push", "world.entity-pushed"),
                ("sink", "world.entity-destroyed"),
            ):
                match = re.search(
                    rf'(?m)^\s*{source_key}\s*=\s*"([^"]+)"',
                    text,
                )
                if match:
                    bindings[match.group(1).lower()] = event

        for relative in sorted(by_path):
            suffix = pathlib.PurePosixPath(relative).suffix.lower()
            if suffix == ".gd":
                text = cls._read_source(source, relative)
                if (
                    re.search(r"(?m)^func\s+destroy\s*\(", text)
                    and re.search(
                        r'play_sound\(\s*"explode"\s*\)',
                        text,
                    )
                ):
                    bindings["explode"] = "world.explosion"
            elif suffix == ".tscn" and relative.startswith("pickups/"):
                text = cls._read_source(source, relative)
                match = re.search(
                    r'(?m)^play_sound\s*=\s*"([^"]+)"',
                    text,
                )
                if match:
                    bindings[match.group(1).lower()] = (
                        "world.item-collected"
                    )
        return bindings

    def extract_source_model(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
        *,
        semantic_inventory: dict[str, Any] | None = None,
    ) -> GodotSourceModel:
        """Produce the versioned model; never emit an XCP module."""

        exact_semantics = (
            semantic_inventory
            if semantic_inventory is not None
            else self.extract_semantic_inventory(source, inventory)
        )
        return build_file_parser_source_model(
            source,
            inventory,
            exact_semantics,
            frontend_id=self.frontend_id,
            frontend_version=self.frontend_version,
        )

    @staticmethod
    def semantic_pass() -> GodotSemanticInventoryPass:
        return GodotSemanticInventoryPass()

    def creative_ir_emitter(self) -> Godot2CreativeIrPass:
        return Godot2CreativeIrPass(
            lambda model, inventory: self._extract_legacy_ir(
                model.source_root,
                dict(inventory),
            )
        )

    def emit_creative_ir(
        self,
        source_model: GodotSourceModel,
        semantic_projection: dict[str, Any],
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        return self.creative_ir_emitter().emit(
            source_model,
            semantic_projection,
            inventory,
        )

    def extract(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        """Compatibility API projected through the G1 source-model spine."""

        source_model = self.extract_source_model(source, inventory)
        semantic_projection = self.semantic_pass().run(source_model)
        return self.emit_creative_ir(
            source_model,
            semantic_projection,
            inventory,
        )

    def _extract_legacy_ir(
        self,
        source: pathlib.Path,
        inventory: dict[str, Any],
    ) -> dict[str, Any]:
        by_path, file_ids = self._file_maps(inventory)
        config = self._configuration(source / "engine.cfg")
        application = config.get("application", {})
        title = self._unquote(application.get("name", "Godot project"))
        main_scene_path = self._unquote(
            application.get("main_scene", "res://engine.cfg")
        ).removeprefix("res://")
        project_id = _portable_id(
            str(inventory["source"]["project_name"]),
            maximum=96,
        )

        scene_paths = sorted(
            path for path in by_path if pathlib.PurePosixPath(path).suffix == ".tscn"
        )
        scene_text = {
            relative: self._read_source(source, relative)
            for relative in scene_paths
        }
        scene_visual_assets = {
            relative: self._scene_visual_asset(scene_text[relative], relative)
            for relative in scene_paths
        }
        tileset_visuals = (
            self._tileset_visual_blueprint(
                self._read_source(source, "shared/tileset.tres")
            )
            if "shared/tileset.tres" in by_path
            else []
        )
        display = config.get("display", {})
        viewport_width = int(float(display.get("width", "1024")))
        viewport_height = int(float(display.get("height", "768")))
        pack_order: dict[str, int] = {}
        if "levels/packs.txt" in by_path:
            for line in self._read_source(
                source,
                "levels/packs.txt",
            ).splitlines():
                fields = line.strip().split()
                if fields and fields[0] not in pack_order:
                    pack_order[fields[0]] = len(pack_order)
        level_titles: dict[tuple[str, int], str] = {}
        for pack in pack_order:
            names_path = f"levels/{pack}/names.txt"
            if names_path not in by_path:
                continue
            for line in self._read_source(source, names_path).splitlines():
                match = re.match(r"^\s*([0-9]+)\s*:\s*(.+?)\s*$", line)
                if match:
                    level_titles[(pack, int(match.group(1)))] = (
                        match.group(2)[:256]
                    )
        scenes: list[dict[str, Any]] = []
        scene_ids: dict[str, str] = {}
        for relative in scene_paths:
            text = scene_text[relative]
            root_match = re.search(
                r'^\[node name="([^"]+)" type="([^"]+)"(?: parent="[^"]*")?\]',
                text,
                flags=re.MULTILINE,
            )
            root_name = root_match.group(1) if root_match else pathlib.PurePosixPath(
                relative
            ).stem
            root_type = root_match.group(2) if root_match else "Unknown"
            scene_id = _portable_id(
                pathlib.PurePosixPath(relative).with_suffix("").as_posix(),
                prefix="scene",
                maximum=128,
            )
            scene_ids[relative] = scene_id
            semantic_kind = (
                "ui.scene"
                if relative.startswith(("menu/", "gui/"))
                or root_type in {"Control", "Panel", "CanvasLayer"}
                else "gameplay.scene2d"
            )
            level_match = re.fullmatch(
                r"levels/([^/]+)/level_([0-9]+)\.tscn",
                relative,
            )
            level_metadata: dict[str, Any] = {}
            if level_match:
                pack = level_match.group(1)
                ordinal = int(level_match.group(2))
                level_metadata = {
                    "level_pack": pack,
                    "level_ordinal": ordinal,
                    "level_title": level_titles.get(
                        (pack, ordinal),
                        f"{pack.replace('_', ' ').title()} {ordinal}",
                    ),
                    "campaign_order": (
                        pack_order.get(pack, len(pack_order)) * 1024
                        + ordinal
                    ),
                }
            scenes.append(
                {
                    "id": scene_id,
                    "source_refs": [file_ids[relative]],
                    "semantic_kind": semantic_kind,
                    "intent": f"Preserve Godot scene {relative}.",
                    "attributes": {
                        "source_path": relative,
                        "root_name": root_name[:160],
                        "root_type": root_type[:160],
                        "coordinate_space": "two_dimensional",
                        **self._grid_blueprint(
                            text,
                            scene_visual_assets,
                        ),
                        "grid_tile_visuals": tileset_visuals,
                        "grid_background_asset": (
                            self._scene_visual_asset(text, relative)
                        ),
                        "source_viewport": [
                            viewport_width,
                            viewport_height,
                        ],
                        **level_metadata,
                        "adaptation_entry": (
                            relative == "levels/tutorial/level_1.tscn"
                        ),
                    },
                }
            )
        if not scenes:
            raise SourceAdaptError(
                "xcp.adapt.scene_missing",
                "The Godot source contains no text scenes to adapt.",
                stage="extract",
                field="files",
                expected="at least one .tscn scene",
                actual="0",
                correction="select the project root or add a compatible adapter",
            )
        initial_scene = scene_ids.get(main_scene_path, scenes[0]["id"])

        entities: list[dict[str, Any]] = []
        entity_by_stem: dict[str, str] = {}
        for relative in scene_paths:
            if not relative.startswith("entities/"):
                continue
            entity_id = _portable_id(
                pathlib.PurePosixPath(relative).with_suffix("").as_posix(),
                prefix="entity",
                maximum=128,
            )
            entity_by_stem[pathlib.PurePosixPath(relative).stem] = entity_id
            entities.append(
                {
                    "id": entity_id,
                    "source_refs": [file_ids[relative]],
                    "semantic_kind": "gameplay.entity2d",
                    "intent": (
                        "Preserve the source entity, its spatial state and interactions."
                    ),
                    "attributes": {
                        "source_path": relative,
                        "grid_or_collision_participant": True,
                        "visual_asset_id": (
                            _portable_id(
                                scene_visual_assets[relative],
                                prefix="asset",
                                maximum=128,
                            )
                            if scene_visual_assets[relative]
                            else ""
                        ),
                    },
                }
            )
        if not entities:
            entities.append(
                {
                    "id": "entity.player",
                    "source_refs": [scenes[0]["source_refs"][0]],
                    "semantic_kind": "gameplay.entity2d",
                    "intent": "Represent the primary player-controlled entity.",
                    "attributes": {
                        "source_path": str(
                            scenes[0]["attributes"]["source_path"]
                        ),
                        "grid_or_collision_participant": True,
                    },
                }
            )

        behaviors: list[dict[str, Any]] = []
        script_paths = sorted(
            path for path in by_path if pathlib.PurePosixPath(path).suffix == ".gd"
        )
        for relative in script_paths:
            text = self._read_source(source, relative)
            functions = re.findall(r"(?m)^func\s+([A-Za-z0-9_]+)\s*\(", text)
            actions = sorted(set(re.findall(r'Input\.is_action_\w+\("([^"]+)"\)', text)))
            stem = pathlib.PurePosixPath(relative).stem
            owner = entity_by_stem.get(stem, initial_scene)
            effects: list[str] = []
            if functions:
                effects.append("functions: " + ", ".join(functions[:32]))
            if actions:
                effects.append("input actions: " + ", ".join(actions[:32]))
            if "move_in_direction" in text:
                effects.append("grid movement with collision constraints")
            if "destroy()" in text or "queue_free()" in text:
                effects.append("entity destruction and lifecycle mutation")
            if "FileManager" in text or "save_path" in text:
                effects.append("persistent state load and save")
            if not effects:
                effects.append("execute source-defined deterministic script behavior")
            trigger = (
                "input.action"
                if actions
                else "timer.timeout"
                if "timeout" in text
                else "lifecycle.update"
            )
            behaviors.append(
                {
                    "id": _portable_id(
                        pathlib.PurePosixPath(relative).with_suffix("").as_posix(),
                        prefix="behavior",
                        maximum=128,
                    ),
                    "source_refs": [file_ids[relative]],
                    "owner_refs": [owner],
                    "trigger": trigger,
                    "conditions": [],
                    "effects": effects,
                }
            )

        input_actions: list[dict[str, Any]] = []
        for action, encoded in sorted(config.get("input", {}).items()):
            bindings = [
                f"keyboard:{match.group(1).lower()}"
                for match in re.finditer(r"key\(([^)]+)\)", encoded)
            ]
            if not bindings:
                bindings = ["source:unresolved"]
            input_actions.append(
                {
                    "id": _portable_id(action, prefix="action", maximum=128),
                    "source_refs": [file_ids["engine.cfg"]],
                    "semantic_action": _portable_id(
                        action, prefix="input", maximum=128
                    ),
                    "bindings": bindings,
                }
            )

        ui: list[dict[str, Any]] = []
        for scene in scenes:
            relative = str(scene["attributes"]["source_path"])
            text = scene_text[relative]
            control_count = len(
                re.findall(
                    r'type="(?:Control|Button|Label|Panel|Container|'
                    r'VBoxContainer|HBoxContainer|TextureButton)"',
                    text,
                )
            )
            if scene["semantic_kind"] == "ui.scene" or control_count:
                ui.append(
                    {
                        "id": _portable_id(scene["id"], prefix="ui", maximum=128),
                        "source_refs": scene["source_refs"],
                        "semantic_kind": "ui.controller_surface",
                        "intent": f"Preserve controller-usable UI from {relative}.",
                        "attributes": {
                            "source_path": relative,
                            "control_count": control_count,
                            "display_texts": self._display_texts(text),
                            "adaptation_entry": bool(
                                scene["attributes"].get("adaptation_entry")
                            ),
                        },
                    }
                )

        audio_event_bindings = self._audio_event_bindings(source, by_path)
        assets: list[dict[str, Any]] = []
        for relative, item in sorted(by_path.items()):
            suffix = pathlib.PurePosixPath(relative).suffix.lower()
            if suffix not in ASSET_EXTENSIONS:
                continue
            media_type = str(item["media_type"])
            audio_cue = audio_event_bindings.get(
                pathlib.PurePosixPath(relative).stem.lower()
            )
            semantic_role = (
                "audio.effect"
                if media_type.startswith("audio/") and audio_cue
                else "audio.music"
                if media_type.startswith("audio/")
                else "visual.font"
                if suffix in {".ttf", ".otf"}
                else "visual.sprite"
            )
            assets.append(
                {
                    "id": _portable_id(
                        pathlib.PurePosixPath(relative).as_posix(),
                        prefix="asset",
                        maximum=128,
                    ),
                    "source_refs": [str(item["file_id"])],
                    "semantic_role": semantic_role,
                    "media_type": media_type,
                    "source_sha256": str(item["sha256"]),
                    **(
                        {"audio_cues": [audio_cue]}
                        if audio_cue
                        else {}
                    ),
                }
            )

        state: list[dict[str, Any]] = []
        save_path = next(
            (path for path in script_paths if path.endswith("save_manager.gd")),
            "",
        )
        if save_path:
            state.append(
                {
                    "id": "state.reached_level",
                    "source_refs": [file_ids[save_path]],
                    "scope": "project",
                    "value_type": "integer",
                    "default_value": 1,
                    "persistence": "saved",
                }
            )
        state.append(
            {
                "id": "state.current_scene",
                "source_refs": [file_ids["engine.cfg"]],
                "scope": "session",
                "value_type": "string",
                "default_value": initial_scene,
                "persistence": "session",
            }
        )

        diagnostics = []
        ogg_refs = [
            asset["source_refs"][0]
            for asset in assets
            if asset["media_type"] == "audio/ogg"
        ]
        if ogg_refs:
            diagnostics.append(
                {
                    "code": "xcp.adapt.audio_transcode_required",
                    "severity": "warning",
                    "source_refs": ogg_refs[:256],
                    "message": (
                        "The current creative host admits PCM WAV, so Ogg audio "
                        "needs PC-side translation or a declared degradation."
                    ),
                }
            )
        duplicate_instance_refs = [
            scene["source_refs"][0]
            for scene in scenes
            if scene["attributes"].get("grid_duplicate_instances")
        ]
        if duplicate_instance_refs:
            diagnostics.append(
                {
                    "code": "xcp.adapt.duplicate_instance_translation",
                    "severity": "warning",
                    "source_refs": duplicate_instance_refs[:256],
                    "message": (
                        "The source contains identical co-located scene "
                        "instances. The shared backend collapses only redundant "
                        "solid instances and preserves the translation in the "
                        "Creative IR."
                    ),
                }
            )

        ir = {
            "schema_version": "xcp-creative-ir-v1",
            "ir_id": f"{project_id}.ir",
            "source_inventory_sha256": _document_sha(inventory),
            "producer": {
                "adapter_id": self.adapter_id,
                "adapter_version": self.adapter_version,
                "canonicalization": (
                    "utf8_json_sorted_keys_no_insignificant_whitespace_v1"
                ),
            },
            "project": {
                "id": project_id,
                "title": title[:160],
                "description": (
                    f"Semantic adaptation of the authorized {title} source project."
                ),
                "domain": "interactive_2d",
                "initial_scene": initial_scene,
            },
            "scenes": scenes,
            "entities": entities,
            "behaviors": behaviors,
            "input_actions": input_actions,
            "ui": ui,
            "assets": assets,
            "state": state,
            "diagnostics": diagnostics,
        }
        _validate(ir, "ir")
        _validate_ir_integrity(ir, inventory)
        return ir
