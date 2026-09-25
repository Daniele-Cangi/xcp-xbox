"""Shared Creative IR to XCP project lowering backend."""

from __future__ import annotations

import os
import pathlib
import shutil
from collections import deque
from typing import Any

from jsonschema import Draft202012Validator

from xcp_creative_project import (
    PROJECT_FILENAME,
    canonical_json_bytes,
    sha256_file,
    validate_project,
)

from ..core import (
    ROOT,
    SourceAdaptError,
    _document_sha,
    _load_json,
    _portable_id,
    _validate,
)
from ..ir import _campaign_scene_records
from ..planning import _validate_plan_integrity

MODULE_SCHEMAS = {
    "xcp.canvas2d.v1": ROOT / "schemas" / "xcp-canvas2d-module-v1.schema.json",
    "xcp.ui.v1": ROOT / "schemas" / "xcp-ui-module-v1.schema.json",
    "xcp.behavior.v1": ROOT / "schemas" / "xcp-behavior-module-v1.schema.json",
    "xcp.data.v1": ROOT / "schemas" / "xcp-data-module-v1.schema.json",
    "xcp.audio.v1": ROOT / "schemas" / "xcp-audio-module-v1.schema.json",
    "xcp.world2d.v1": ROOT / "schemas" / "xcp-world2d-module-v1.schema.json",
    "xcp.world2d.v2": ROOT / "schemas" / "xcp-world2d-module-v2.schema.json",
    "xcp.world2d.campaign.v1": (
        ROOT / "schemas" / "xcp-world2d-campaign-module-v1.schema.json"
    ),
}


def _module_validate(kind: str, document: dict[str, Any]) -> None:
    schema = _load_json(MODULE_SCHEMAS[kind])
    errors = list(Draft202012Validator(schema).iter_errors(document))
    if errors:
        first = errors[0]
        raise SourceAdaptError(
            "xcp.adapt.backend_module_invalid",
            "The shared backend emitted an invalid XCP module.",
            stage="generate",
            field=kind,
            expected=str(first.validator),
            actual=first.message,
            correction="repair the single IR backend",
        )


def _state_scalar(record: dict[str, Any]) -> tuple[str, Any]:
    value_type = str(record["value_type"])
    value = record["default_value"]
    if value_type == "string":
        return value_type, str(value)[:4096]
    if value_type == "boolean":
        return value_type, bool(value)
    if value_type == "integer":
        return value_type, int(value)
    return value_type, float(value)


def _asset_id(
    ir: dict[str, Any],
    *preferred_suffixes: str,
) -> str:
    available = {
        str(asset["id"])
        for asset in ir["assets"]
        if str(asset["media_type"]) == "image/png"
    }
    for suffix in preferred_suffixes:
        normalized = suffix.lower()
        found = next(
            (
                asset_id
                for asset_id in sorted(available)
                if asset_id.lower().endswith(normalized)
            ),
            "",
        )
        if found:
            return _portable_id(found, maximum=96)
    return ""


def _selected_world_scene(ir: dict[str, Any]) -> dict[str, Any]:
    candidates = [
        scene
        for scene in ir["scenes"]
        if scene["attributes"].get("grid_tiles")
    ]
    if not candidates:
        raise SourceAdaptError(
            "xcp.adapt.world_blueprint_missing",
            "Creative IR has no bounded grid blueprint for world2d generation.",
            stage="generate",
            field="scenes[].attributes.grid_tiles",
            expected="at least one extracted grid scene",
            actual="none",
            correction="extend the ecosystem adapter semantic extraction",
        )
    return next(
        (
            item
            for item in candidates
            if item["attributes"].get("adaptation_entry") is True
        ),
        candidates[0],
    )


def _world2d_module(
    ir: dict[str, Any],
    *,
    authored_v2: bool = False,
    scene: dict[str, Any] | None = None,
) -> dict[str, Any]:
    scene = scene or _selected_world_scene(ir)
    attributes = scene["attributes"]
    cell_size = int(attributes.get("grid_cell_size", 64))
    raw_tiles: list[tuple[int, int, str, int]] = []
    for encoded in attributes["grid_tiles"]:
        x_text, y_text, kind, source_id = str(encoded).split(",", 3)
        raw_tiles.append((int(x_text), int(y_text), kind, int(source_id)))
    raw_instances: list[tuple[str, str, int, int, str]] = []
    for encoded in attributes.get("grid_instances", []):
        parts = str(encoded).split("|")
        if len(parts) not in {4, 5}:
            raise SourceAdaptError(
                "xcp.adapt.world_instance_invalid",
                "Creative IR contains an invalid grid instance.",
                stage="generate",
                field=f"scenes.{scene['id']}.attributes.grid_instances",
                expected="name|source_path|x|y|optional_visual_asset",
                actual=str(encoded),
                correction="repair the ecosystem adapter semantic extraction",
            )
        name, source_path, x_text, y_text = parts[:4]
        visual_asset = parts[4] if len(parts) == 5 else ""
        raw_instances.append(
            (name, source_path, int(x_text), int(y_text), visual_asset)
        )
    start = attributes.get("grid_player_start")
    if not isinstance(start, list) or len(start) != 2:
        raise SourceAdaptError(
            "xcp.adapt.world_player_start_missing",
            "The selected source grid does not declare a player start.",
            stage="generate",
            field=f"scenes.{scene['id']}.attributes.grid_player_start",
            expected="two source cell coordinates",
            actual=str(start),
            correction="repair the adapter player-start extraction",
        )
    # The legacy v1 viewport compacts long decorative hazard strips because
    # it can only fit the complete normalized grid. Authored v2 preserves
    # source coordinates and relies on an explicit clipped viewport.
    # playable puzzle. Rendering the complete strip forces the fixed
    # whole-world viewport to shrink the actual actors into debug-sized
    # sprites. Keep the exact hazard cells beneath the extracted gameplay
    # span and classify the layout as an IR-side presentation translation;
    # gameplay entities, their relative cells, and the objective remain
    # unchanged.
    gameplay_x = [
        x for x, _, kind, _ in raw_tiles if kind != "hazard"
    ]
    gameplay_x.extend(x for _, _, x, _, _ in raw_instances)
    gameplay_x.append(int(start[0]))
    hazard_x = [x for x, _, kind, _ in raw_tiles if kind == "hazard"]
    compact_hazard_view = False
    if not authored_v2 and gameplay_x and hazard_x:
        gameplay_min_x = min(gameplay_x)
        gameplay_max_x = max(gameplay_x)
        hazard_min_x = min(hazard_x)
        hazard_max_x = max(hazard_x)
        gameplay_span = gameplay_max_x - gameplay_min_x + 1
        hazard_span = hazard_max_x - hazard_min_x + 1
        hazard_coverage = set(hazard_x)
        if (
            gameplay_span >= 3
            and hazard_span >= gameplay_span + 4
            and all(
                x in hazard_coverage
                for x in range(gameplay_min_x, gameplay_max_x + 1)
            )
        ):
            raw_tiles = [
                item
                for item in raw_tiles
                if item[2] != "hazard"
                or gameplay_min_x <= item[0] <= gameplay_max_x
            ]
            compact_hazard_view = True
    points = [(x, y) for x, y, _, _ in raw_tiles]
    points.extend((x, y) for _, _, x, y, _ in raw_instances)
    points.append((int(start[0]), int(start[1])))
    viewport = attributes.get("source_viewport", [1024, 768])
    viewport_columns = max(
        1,
        int((int(viewport[0]) + cell_size - 1) // cell_size),
    )
    viewport_rows = max(
        1,
        int((int(viewport[1]) + cell_size - 1) // cell_size),
    )
    horizontal_margin = 0 if compact_hazard_view else 1
    bottom_margin = 0 if compact_hazard_view else 1
    minimum_x = (
        min(0, min(x for x, _ in points))
        if authored_v2
        else min(x for x, _ in points) - horizontal_margin
    )
    minimum_y = (
        min(0, min(y for _, y in points))
        if authored_v2
        else min(y for _, y in points) - 1
    )
    maximum_x = (
        max(
            viewport_columns - 1,
            max(x for x, _ in points),
        )
        if authored_v2
        else max(x for x, _ in points) + horizontal_margin
    )
    maximum_y = (
        max(
            viewport_rows - 1,
            max(y for _, y in points),
        )
        if authored_v2
        else max(y for _, y in points) + bottom_margin
    )
    columns = maximum_x - minimum_x + 1
    rows = maximum_y - minimum_y + 1
    if columns > 512 or rows > 512:
        raise SourceAdaptError(
            "xcp.adapt.world_bounds_exceeded",
            "Extracted source grid exceeds the native world2d bounds.",
            stage="generate",
            field=f"scenes.{scene['id']}",
            expected="at most 512 columns and rows",
            actual=f"{columns}x{rows}",
            correction="select or partition a bounded source scene",
        )

    def cell(x: int, y: int) -> list[int]:
        return [x - minimum_x, y - minimum_y]

    palette = {
        "floor": "#537A45",
        "hazard": "#89C83A",
        "climbable": "#C7A75B",
    }
    tile_assets = {
        "hazard": _asset_id(ir, "asset.gfx.acid_anim.png"),
        "climbable": _asset_id(ir, "asset.gfx.ladder.png"),
    }
    tile_visuals: dict[int, dict[str, Any]] = {}
    for encoded in attributes.get("grid_tile_visuals", []):
        parts = str(encoded).split("|")
        if len(parts) != 9:
            continue
        (
            source_id_text,
            _,
            asset_path,
            source_x,
            source_y,
            source_width,
            source_height,
            offset_x,
            offset_y,
        ) = parts
        source_width_value = float(source_width)
        source_height_value = float(source_height)
        render: dict[str, Any] = {
            "asset_id": _portable_id(
                asset_path,
                prefix="asset",
                maximum=96,
            ),
            "offset": [float(offset_x), float(offset_y)],
            "size": (
                [source_width_value, source_height_value]
                if source_width_value > 0 and source_height_value > 0
                else [float(cell_size), float(cell_size)]
            ),
            "opacity": 1.0,
        }
        if source_width_value > 0 and source_height_value > 0:
            render["source_rect"] = [
                float(source_x),
                float(source_y),
                source_width_value,
                source_height_value,
            ]
        tile_visuals[int(source_id_text)] = render
    tiles = []
    for index, (x, y, kind, source_id) in enumerate(raw_tiles):
        tile = {
            "id": f"tile.{index}",
            "position": cell(x, y),
            "kind": kind,
            "solid": authored_v2 and kind in {"floor", "solid"},
            "color": (
                "#FFFFFF00"
                if authored_v2 and source_id in tile_visuals
                else palette.get(kind, "#537A45")
            ),
            "visible": True,
        }
        if authored_v2 and source_id in tile_visuals:
            tile["render"] = tile_visuals[source_id]
        elif authored_v2 and tile_assets.get(kind):
            tile["render"] = {
                "asset_id": tile_assets[kind],
                "offset": [0.0, 0.0],
                "size": [float(cell_size), float(cell_size)],
                "opacity": 1.0,
            }
        elif not authored_v2 and tile_assets.get(kind):
            tile["asset_id"] = tile_assets[kind]
        tiles.append(tile)
    player_asset = next(
        (
            _portable_id(
                str(entity["attributes"]["visual_asset_id"]),
                maximum=96,
            )
            for entity in ir["entities"]
            if str(entity["attributes"].get("source_path", "")).endswith(
                "/player.tscn"
            )
            and entity["attributes"].get("visual_asset_id")
        ),
        "",
    ) or _asset_id(ir, "asset.gfx.player.player1.png")
    entities = [
        {
            "id": "player",
            "archetype": "player",
            "position": cell(int(start[0]), int(start[1])),
            "color": "#DDE8EF",
            "solid": True,
            "pushable": False,
            "destructible": False,
            "collectible": False,
            "controllable": True,
            **(
                {
                    "gravity": True,
                    "movement_mode": "platform_grid",
                    "color": "#FFFFFF00",
                    **(
                        {
                            "render": {
                                "asset_id": player_asset,
                                "offset": [0.0, 0.0],
                                "size": [
                                    float(cell_size),
                                    float(cell_size),
                                ],
                                "opacity": 1.0,
                            }
                        }
                        if player_asset
                        else {}
                    ),
                }
                if authored_v2
                else {}
            ),
            "initially_visible": True,
            "z_index": 10,
            **(
                {"asset_id": player_asset}
                if player_asset and not authored_v2
                else {}
            ),
        }
    ]
    archetype_counts: dict[str, int] = {}
    occupied_solid_cells: dict[
        tuple[int, int], tuple[str, str]
    ] = {
        tuple(entities[0]["position"]): ("player", player_asset)
    }
    for name, source_path, x, y, visual_asset_path in raw_instances:
        lowered = f"{name} {source_path}".lower()
        if "box" in lowered:
            archetype = "barrel"
            properties = {
                "color": "#E7D447",
                "solid": True,
                "pushable": True,
                "destructible": True,
                "collectible": False,
                "controllable": False,
            }
            fallback_asset = _asset_id(
                ir,
                "asset.gfx.radioactive.png",
                "asset.gfx.box.png",
            )
        elif "flower" in lowered or "pickup" in lowered:
            archetype = "pickup"
            properties = {
                "color": "#E68FB3",
                "solid": False,
                "pushable": False,
                "destructible": False,
                "collectible": True,
                "controllable": False,
                "inventory_key": "pickup",
                "inventory_amount": 1,
            }
            fallback_asset = _asset_id(
                ir,
                "asset.gfx.flower.png",
                "asset.gfx.pickup_bomb.png",
            )
        else:
            continue
        visual_asset = (
            _portable_id(
                visual_asset_path,
                prefix="asset",
                maximum=96,
            )
            if visual_asset_path
            else fallback_asset
        )
        position = cell(x, y)
        if bool(properties["solid"]):
            position_key = (position[0], position[1])
            signature = (source_path, visual_asset)
            existing = occupied_solid_cells.get(position_key)
            if existing == signature:
                # Identical static source instances at one cell add no
                # distinct behavior to the bounded world model. Keep the
                # duplication visible in Creative IR diagnostics, but emit
                # one canonical solid entity so the native host can retain
                # its strict no-overlap invariant.
                continue
            if existing is not None:
                raise SourceAdaptError(
                    "xcp.adapt.world_entity_overlap",
                    "Conflicting visible solid source instances share one cell.",
                    stage="generate",
                    field=(
                        f"scenes.{scene['id']}.attributes.grid_instances"
                    ),
                    expected=(
                        "unique solid cells or an exact redundant source "
                        "instance"
                    ),
                    actual=(
                        f"{position_key[0]},{position_key[1]}:"
                        f"{existing[0]}|{source_path}"
                    ),
                    correction=(
                        "resolve the conflicting source semantics in Creative "
                        "IR before generating the XCP module"
                    ),
                )
            occupied_solid_cells[position_key] = signature
        ordinal = archetype_counts.get(archetype, 0)
        archetype_counts[archetype] = ordinal + 1
        entities.append(
            {
                "id": f"{archetype}.{ordinal}",
                "archetype": archetype,
                "position": position,
                **properties,
                **(
                    {
                        "gravity": archetype != "pickup",
                        "movement_mode": "four_way",
                        "color": "#FFFFFF00",
                        **(
                            {
                                "render": {
                                    "asset_id": visual_asset,
                                    "offset": [0.0, 0.0],
                                    "size": [
                                        float(cell_size),
                                        float(cell_size),
                                    ],
                                    "opacity": 1.0,
                                }
                            }
                            if visual_asset
                            else {}
                        ),
                    }
                    if authored_v2
                    else {}
                ),
                "initially_visible": True,
                "z_index": 5,
                **(
                    {"asset_id": visual_asset}
                    if visual_asset and not authored_v2
                    else {}
                ),
            }
        )
    input_bindings: list[dict[str, Any]] = []
    directions = {
        "up": ("gamepad.dpad_up", "move_up"),
        "down": ("gamepad.dpad_down", "move_down"),
        "left": ("gamepad.dpad_left", "move_left"),
        "right": ("gamepad.dpad_right", "move_right"),
    }
    for action in ir["input_actions"]:
        semantic = str(action["semantic_action"])
        lowered = semantic.lower()
        direction = next(
            (key for key in directions if key in lowered),
            "",
        )
        if direction:
            source_name, command = directions[direction]
        elif "retry" in lowered or "reset" in lowered:
            source_name, command = "gamepad.x", "reset"
        else:
            continue
        input_bindings.append(
            {
                "action": semantic,
                "source": source_name,
                "command": command,
                "target_entity": "player",
            }
        )
    if not input_bindings:
        input_bindings = [
            {
                "action": f"input.move-{direction}",
                "source": source,
                "command": command,
                "target_entity": "player",
            }
            for direction, (source, command) in directions.items()
        ]
    objectives = []
    if archetype_counts.get("barrel", 0):
        objectives.append(
            {
                "id": "objective.clear-barrels",
                "type": "destroy_all",
                "target": "barrel",
                "count": archetype_counts["barrel"],
                "completion_event": "world.level-completed",
            }
        )
    result = {
        "schema_version": (
            "xcp-world2d-module-v2"
            if authored_v2
            else "xcp-world2d-module-v1"
        ),
        "world": {
            "width": (
                int(viewport[0])
                if authored_v2
                else max(1, columns * cell_size)
            ),
            "height": (
                int(viewport[1])
                if authored_v2
                else max(1, rows * cell_size)
            ),
            "background": "#F5FFD4" if authored_v2 else "#1A2430",
            "columns": columns,
            "rows": rows,
            "cell_size": cell_size,
            **(
                {
                    "content_origin": [
                        float(minimum_x * cell_size),
                        float(minimum_y * cell_size),
                    ],
                    **(
                        {
                            "background_asset_id": _portable_id(
                                str(
                                    attributes[
                                        "grid_background_asset"
                                    ]
                                ),
                                prefix="asset",
                                maximum=96,
                            )
                        }
                        if attributes.get("grid_background_asset")
                        else {}
                    ),
                }
                if authored_v2
                else {}
            ),
        },
        "tiles": tiles,
        "entities": entities,
        "input_bindings": input_bindings,
        "objectives": objectives,
    }
    _module_validate(
        "xcp.world2d.v2" if authored_v2 else "xcp.world2d.v1",
        result,
    )
    return result


def _world2d_campaign_module(
    ir: dict[str, Any],
    scene_modules: list[tuple[dict[str, Any], str]],
) -> dict[str, Any]:
    if len(scene_modules) < 2:
        raise SourceAdaptError(
            "xcp.adapt.campaign_scene_count_invalid",
            "A world2d campaign requires at least two extracted scenes.",
            stage="generate",
            field="scenes",
            expected="2..128 campaign scenes",
            actual=str(len(scene_modules)),
            correction="emit a single world2d module or extract the campaign",
        )
    scenes: list[dict[str, Any]] = []
    for scene, module_id in scene_modules:
        attributes = scene["attributes"]
        pack_id = _portable_id(
            str(attributes["level_pack"]),
            prefix="pack",
            maximum=96,
        )
        scenes.append(
            {
                "id": _portable_id(
                    str(scene["id"]),
                    prefix="level",
                    maximum=96,
                ),
                "module_id": module_id,
                "pack_id": pack_id,
                "ordinal": int(attributes["level_ordinal"]),
                "title": str(attributes["level_title"])[:256],
            }
        )
    transitions = [
        {
            "id": f"transition.{index + 1}",
            "from_scene": scenes[index]["id"],
            "event": "world.level-completed",
            "to_scene": scenes[index + 1]["id"],
        }
        for index in range(len(scenes) - 1)
    ]
    result = {
        "schema_version": "xcp-world2d-campaign-module-v1",
        "entry_scene": scenes[0]["id"],
        "scenes": scenes,
        "transitions": transitions,
        "input_bindings": [
            {
                "action": "input.next-level",
                "source": "gamepad.a",
                "command": "advance_scene",
            }
        ],
    }
    _module_validate("xcp.world2d.campaign.v1", result)
    return result


def _world2d_hud_module(
    ir: dict[str, Any],
    world: dict[str, Any],
    *,
    campaign: bool = False,
) -> dict[str, Any]:
    scene = _selected_world_scene(ir)
    scene_path = str(scene["attributes"]["source_path"])
    source_texts = next(
        (
            [
                str(text)
                for text in item["attributes"].get("display_texts", [])
                if str(text).strip()
            ]
            for item in ir["ui"]
            if str(item["attributes"].get("source_path", "")) == scene_path
        ),
        [],
    )
    goal = next(
        (
            text
            for text in source_texts
            if any(
                marker in text.lower()
                for marker in ("push", "collect", "reach", "destroy", "acid")
            )
        ),
        f"Complete the {ir['project']['title']} objective.",
    )
    commands = {
        str(binding["command"]): str(binding["source"])
        for binding in world["input_bindings"]
    }
    controls = ["D-pad: move"]
    if "activate" in commands:
        controls.append("A: action")
    if "reset" in commands:
        controls.append("X: retry")
    if campaign:
        controls.append("A: next level after completion")
    result = {
        "schema_version": "xcp-canvas2d-module-v1",
        "canvas": {
            "width": 1280,
            "height": 720,
            "background": "#00000000",
        },
        "nodes": [
            {
                "id": "hud.backdrop",
                "type": "rectangle",
                "transform": {"position": [640, 58]},
                "size": [1180, 92],
                "style": {
                    "fill": "#07120C",
                    "opacity": 0.82,
                },
                "z_index": 100,
                "visible": True,
            },
            {
                "id": "hud.goal",
                "type": "text",
                "transform": {"position": [640, 38]},
                "text": goal[:256],
                "font_size": 24,
                "style": {
                    "fill": "#F2F7E9",
                    "opacity": 1.0,
                },
                "z_index": 101,
                "visible": True,
            },
            {
                "id": "hud.controls",
                "type": "text",
                "transform": {"position": [640, 72]},
                "text": "  |  ".join(controls),
                "font_size": 20,
                "style": {
                    "fill": "#9CDD58",
                    "opacity": 1.0,
                },
                "z_index": 101,
                "visible": True,
            },
            {
                "id": "hud.complete-backdrop",
                "type": "rectangle",
                "transform": {"position": [640, 360]},
                "size": [620, 150],
                "style": {
                    "fill": "#102814",
                    "stroke": "#9CDD58",
                    "stroke_width": 4,
                    "opacity": 0.95,
                },
                "z_index": 200,
                "visible": False,
            },
            {
                "id": "hud.complete",
                "type": "text",
                # Canvas text is leading-aligned inside a fixed-width text
                # rectangle. Offset this short completion label so its
                # measured glyph run is centered over the backdrop.
                "transform": {"position": [960, 360]},
                "text": "LEVEL COMPLETE",
                "font_size": 46,
                "style": {
                    "fill": "#F5FFE9",
                    "opacity": 1.0,
                },
                "z_index": 201,
                "visible": False,
            },
        ],
        "input_bindings": [],
    }
    _module_validate("xcp.canvas2d.v1", result)
    return result


def _audio_module(
    ir: dict[str, Any],
    plan_by_ir: dict[str, dict[str, Any]],
) -> tuple[dict[str, Any] | None, list[str]]:
    composed_assets = [
        asset
        for asset in ir["assets"]
        if asset["media_type"] == "audio/wav"
        and asset.get("audio_cues")
        and plan_by_ir[str(asset["id"])]["classification"]
        == "preserved_directly"
    ]
    if not composed_assets:
        return None, []

    clips: list[dict[str, Any]] = []
    cues: list[dict[str, Any]] = []
    for asset in composed_assets:
        asset_id = _portable_id(str(asset["id"]), maximum=96)
        clip_id = _portable_id(
            f"clip.{asset['id']}",
            maximum=96,
        )
        clips.append(
            {
                "id": clip_id,
                "asset_id": asset_id,
                "bus_id": "effects",
                "gain": 1,
                "loop": False,
            }
        )
        for index, event in enumerate(asset["audio_cues"], start=1):
            cues.append(
                {
                    "id": _portable_id(
                        f"cue.{asset['id']}.{index}",
                        maximum=96,
                    ),
                    "event": str(event),
                    "clip_id": clip_id,
                    "gain": 1,
                }
            )

    result = {
        "schema_version": "xcp-audio-module-v1",
        "buses": [{"id": "effects", "gain": 1}],
        "clips": clips,
        "cues": cues,
    }
    _module_validate("xcp.audio.v1", result)
    return result, [str(asset["id"]) for asset in composed_assets]


def generate_project(
    source: pathlib.Path,
    inventory: dict[str, Any],
    ir: dict[str, Any],
    plan: dict[str, Any],
    output: pathlib.Path,
    *,
    project_version: str = "1.3.2",
) -> tuple[dict[str, Any], dict[str, Any]]:
    _validate_plan_integrity(plan, ir)
    if plan["decision"] not in {"ready_to_generate", "ready_with_degradation"}:
        raise SourceAdaptError(
            "xcp.adapt.generation_blocked",
            "The shared backend cannot emit a project from a blocked plan.",
            stage="generate",
            field="decision",
            expected="ready_to_generate or ready_with_degradation",
            actual=str(plan["decision"]),
            correction="resolve unsupported or human-required items first",
        )
    output = output.resolve()
    if output.exists():
        raise SourceAdaptError(
            "xcp.adapt.output_exists",
            "The generated project output must be fresh.",
            stage="generate",
            field="output",
            expected="new directory",
            actual=str(output),
            correction="select a fresh output directory",
        )

    source_by_id = {
        str(item["file_id"]): item for item in inventory["files"]
    }
    plan_by_ir = {
        str(reference): item
        for item in plan["items"]
        for reference in item["ir_refs"]
    }
    project_id = str(ir["project"]["id"])
    staging = output.with_name(f".{output.name}.tmp-{os.getpid()}")
    if staging.exists():
        raise SourceAdaptError(
            "xcp.adapt.staging_exists",
            "The private backend staging path already exists.",
            stage="generate",
            field="staging",
            expected="fresh path",
            actual=str(staging),
            correction="inspect and remove the stale staging path",
        )

    modules: list[dict[str, Any]] = []
    assets: list[dict[str, Any]] = []
    source_map_entries: list[dict[str, Any]] = []
    xcp_refs: set[str] = {f"project:{project_id}"}
    module_refs_by_ir: dict[str, list[str]] = {}
    authored_world = any(
        "render.sprite_atlas" in item["required_host_capabilities"]
        for item in plan["items"]
    )
    campaign_world = any(
        "world2d.campaign" in item["required_host_capabilities"]
        for item in plan["items"]
    )
    world_kind = (
        "xcp.world2d.campaign.v1"
        if campaign_world
        else "xcp.world2d.v2"
        if authored_world
        else "xcp.world2d.v1"
    )
    try:
        (staging / "modules").mkdir(parents=True)
        if campaign_world:
            scene_modules: list[tuple[dict[str, Any], str]] = []
            world_documents: list[dict[str, Any]] = []
            for scene in _campaign_scene_records(ir):
                attributes = scene["attributes"]
                module_id = _portable_id(
                    (
                        f"world.{attributes['level_pack']}."
                        f"{attributes['level_ordinal']}"
                    ),
                    maximum=96,
                )
                world_document = _world2d_module(
                    ir,
                    authored_v2=True,
                    scene=scene,
                )
                world_documents.append(world_document)
                relative_path = f"modules/{module_id}.json"
                (staging / pathlib.PurePosixPath(relative_path)).write_bytes(
                    canonical_json_bytes(world_document)
                )
                modules.append(
                    {
                        "id": module_id,
                        "kind": "xcp.world2d.v2",
                        "path": relative_path,
                        "depends_on": [],
                    }
                )
                scene_modules.append((scene, module_id))
                xcp_refs.add(f"module:{module_id}")
                module_refs_by_ir[str(scene["id"])] = [
                    f"module:{module_id}"
                ]
            campaign_document = _world2d_campaign_module(
                ir,
                scene_modules,
            )
            world_path = staging / "modules" / "world.json"
            world_path.write_bytes(
                canonical_json_bytes(campaign_document)
            )
            modules.append(
                {
                    "id": "world",
                    "kind": world_kind,
                    "path": "modules/world.json",
                    "depends_on": [
                        module_id
                        for _, module_id in scene_modules
                    ],
                }
            )
            world_document = world_documents[0]
            xcp_refs.add("module:world")
        else:
            world_document = _world2d_module(
                ir,
                authored_v2=authored_world,
            )
            world_path = staging / "modules" / "world.json"
            world_path.write_bytes(canonical_json_bytes(world_document))
            modules.append(
                {
                    "id": "world",
                    "kind": world_kind,
                    "path": "modules/world.json",
                    "depends_on": [],
                }
            )
            xcp_refs.add("module:world")
            module_refs_by_ir[str(_selected_world_scene(ir)["id"])] = [
                "module:world"
            ]

        if ir["ui"]:
            hud = _world2d_hud_module(
                ir,
                world_document,
                campaign=campaign_world,
            )
            (staging / "modules" / "hud.json").write_bytes(
                canonical_json_bytes(hud)
            )
            modules.append(
                {
                    "id": "hud",
                    "kind": "xcp.canvas2d.v1",
                    "path": "modules/hud.json",
                    "depends_on": ["world"],
                }
            )
            xcp_refs.add("module:hud")

        if ir["state"]:
            local_state = []
            for record in ir["state"][:1024]:
                value_type, value = _state_scalar(record)
                local_state.append(
                    {
                        "id": _portable_id(
                            str(record["id"]), maximum=96
                        ),
                        "type": value_type,
                        "default": value,
                        "max_bytes": 4096,
                        "persistence": "project_local",
                    }
                )
            data = {
                "schema_version": "xcp-data-module-v1",
                "constants": [],
                "local_state": local_state,
            }
            _module_validate("xcp.data.v1", data)
            (staging / "modules" / "data.json").write_bytes(
                canonical_json_bytes(data)
            )
            modules.append(
                {
                    "id": "data",
                    "kind": "xcp.data.v1",
                    "path": "modules/data.json",
                    "depends_on": [],
                }
            )
            xcp_refs.add("module:data")
            xcp_refs.update(
                f"state:{record['id']}" for record in ir["state"]
            )

        translated_behaviors = [
            record
            for record in ir["behaviors"]
            if plan_by_ir[str(record["id"])]["classification"] == "translated"
        ]
        if translated_behaviors or ir["ui"]:
            rules = []
            for record in translated_behaviors[:4096]:
                rules.append(
                    {
                        "id": _portable_id(str(record["id"]), maximum=96),
                        "event": str(record["trigger"]),
                        "conditions": [],
                        "actions": [
                            {
                                "type": "log.write",
                                "message": str(record["effects"][0])[:1024],
                            }
                        ],
                    }
                )
            if ir["ui"]:
                rules.append(
                    {
                        "id": "hud.show-completion",
                        "event": "world.completed",
                        "conditions": [],
                        "actions": [
                            {
                                "type": "node.visibility",
                                "target": "hud.complete-backdrop",
                                "value": True,
                            },
                            {
                                "type": "node.visibility",
                                "target": "hud.complete",
                                "value": True,
                            },
                            {
                                "type": "log.write",
                                "message": "The adapted world objective completed.",
                            },
                        ],
                    }
                )
            logic = {
                "schema_version": "xcp-behavior-module-v1",
                "state": [],
                "timers": [],
                "rules": rules,
            }
            _module_validate("xcp.behavior.v1", logic)
            (staging / "modules" / "logic.json").write_bytes(
                canonical_json_bytes(logic)
            )
            modules.append(
                {
                    "id": "logic",
                    "kind": "xcp.behavior.v1",
                    "path": "modules/logic.json",
                    "depends_on": ["world"],
                }
            )
            xcp_refs.add("module:logic")

        (staging / "assets").mkdir()
        for asset in ir["assets"]:
            item = plan_by_ir[str(asset["id"])]
            if item["classification"] != "preserved_directly":
                continue
            source_ref = str(asset["source_refs"][0])
            source_item = source_by_id[source_ref]
            original = source / pathlib.PurePosixPath(source_item["path"])
            extension = pathlib.PurePosixPath(source_item["path"]).suffix.lower()
            target_relative = f"assets/{asset['id']}{extension}"
            target = staging / pathlib.PurePosixPath(target_relative)
            shutil.copyfile(original, target)
            if sha256_file(target) != asset["source_sha256"]:
                raise SourceAdaptError(
                    "xcp.adapt.asset_copy_mismatch",
                    "A preserved asset changed during backend copy.",
                    stage="generate",
                    field=str(source_item["path"]),
                    expected=str(asset["source_sha256"]),
                    actual=sha256_file(target),
                    correction="discard the generated project",
                )
            assets.append(
                {
                    "id": _portable_id(str(asset["id"]), maximum=96),
                    "path": target_relative,
                    "media_type": str(asset["media_type"]),
                }
            )
            xcp_refs.add(
                f"asset:{_portable_id(str(asset['id']), maximum=96)}"
            )

        audio, composed_audio_assets = _audio_module(ir, plan_by_ir)
        if audio is not None:
            (staging / "modules" / "audio.json").write_bytes(
                canonical_json_bytes(audio)
            )
            modules.append(
                {
                    "id": "audio",
                    "kind": "xcp.audio.v1",
                    "path": "modules/audio.json",
                    "depends_on": ["world"],
                }
            )
            xcp_refs.add("module:audio")
            for ir_ref in composed_audio_assets:
                module_refs_by_ir[ir_ref] = [
                    f"asset:{_portable_id(ir_ref, maximum=96)}",
                    "module:audio",
                ]

        kinds = {module["kind"] for module in modules}
        required_by_kind = {
            "xcp.canvas2d.v1": {"input.gamepad", "render.canvas2d"},
            "xcp.world2d.v1": {
                "input.gamepad",
                "render.canvas2d",
                "world2d.deterministic",
            },
            "xcp.world2d.v2": {
                "input.gamepad",
                "render.canvas2d",
                "render.sprite_atlas",
                "world2d.deterministic",
                "world2d.gravity",
            },
            "xcp.world2d.campaign.v1": {
                "input.gamepad",
                "render.canvas2d",
                "render.sprite_atlas",
                "state.local",
                "world2d.campaign",
                "world2d.deterministic",
                "world2d.gravity",
            },
            "xcp.ui.v1": {"input.gamepad", "ui.controller"},
            "xcp.behavior.v1": {"behavior.deterministic"},
            "xcp.data.v1": {"state.local"},
            "xcp.audio.v1": {"audio.playback"},
        }
        requested = sorted(
            {
                capability
                for kind in kinds
                for capability in required_by_kind[kind]
            }
        )
        project = {
            "schema_version": "xcp-creative-project-v1",
            "project_id": project_id,
            "version": project_version,
            "title": str(ir["project"]["title"])[:96],
            "description": str(ir["project"]["description"])[:1024],
            "entry_module": "world",
            "modules": modules,
            "assets": assets,
            "requested_capabilities": requested,
            "tags": ["adapted", "c6", "external-source"],
        }
        project_path = staging / PROJECT_FILENAME
        project_path.write_bytes(canonical_json_bytes(project))
        validate_project(staging)

        for plan_item in plan["items"]:
            targets = sorted(
                {
                    target
                    for ir_ref in plan_item["ir_refs"]
                    for target in module_refs_by_ir.get(
                        str(ir_ref),
                        [],
                    )
                }
            )
            if not targets:
                targets = [
                    target
                    for target in plan_item["xcp_targets"]
                    if target in xcp_refs
                ]
            if not targets:
                targets = [f"project:{project_id}"]
            source_map_entries.append(
                {
                    "mapping_type": (
                        "one_to_many" if len(targets) > 1 else "many_to_one"
                    ),
                    "source_refs": plan_item["source_refs"],
                    "ir_refs": plan_item["ir_refs"],
                    "xcp_refs": targets,
                    "classification": plan_item["classification"],
                }
            )
        mapped_sources = {
            reference
            for entry in source_map_entries
            for reference in entry["source_refs"]
        }
        all_sources = {str(item["file_id"]) for item in inventory["files"]}
        source_map = {
            "schema_version": "xcp-creative-source-map-v1",
            "mapping_id": f"{project_id}.source_map",
            "source_inventory_sha256": _document_sha(inventory),
            "creative_ir_sha256": _document_sha(ir),
            "adaptation_plan_sha256": _document_sha(plan),
            "xcp_project_sha256": sha256_file(project_path),
            "entries": source_map_entries,
            "unmapped_source_refs": sorted(all_sources - mapped_sources),
            "untraced_xcp_refs": [],
        }
        _validate(source_map, "source_map")
        os.replace(staging, output)
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise
    return project, source_map


def _world2d_solution(
    world: dict[str, Any],
) -> list[dict[str, Any]]:
    controllers = [
        entity
        for entity in world["entities"]
        if entity["controllable"] and entity["initially_visible"]
    ]
    objectives = [
        objective
        for objective in world["objectives"]
        if objective["type"] == "destroy_all"
    ]
    if len(controllers) != 1 or len(objectives) != 1:
        return []
    target_archetype = str(objectives[0]["target"])
    targets = [
        entity
        for entity in world["entities"]
        if entity["archetype"] == target_archetype
        and entity["pushable"]
        and entity["destructible"]
        and entity["initially_visible"]
    ]
    if len(targets) != 1:
        return []
    action_by_command = {
        str(binding["command"]): str(binding["action"])
        for binding in world["input_bindings"]
    }
    directions = [
        ("move_up", (0, -1)),
        ("move_down", (0, 1)),
        ("move_left", (-1, 0)),
        ("move_right", (1, 0)),
    ]
    if any(command not in action_by_command for command, _ in directions):
        return []

    columns = int(world["world"]["columns"])
    rows = int(world["world"]["rows"])
    solid_tiles = {
        tuple(int(value) for value in tile["position"])
        for tile in world["tiles"]
        if tile["visible"] and tile["solid"]
    }
    hazard_tiles = {
        tuple(int(value) for value in tile["position"])
        for tile in world["tiles"]
        if tile["visible"] and tile["kind"] == "hazard"
    }
    climbable_tiles = {
        tuple(int(value) for value in tile["position"])
        for tile in world["tiles"]
        if tile["visible"] and tile["kind"] == "climbable"
    }
    if not hazard_tiles:
        return []
    protected_solids = {
        tuple(int(value) for value in entity["position"])
        for entity in world["entities"]
        if entity["solid"]
        and entity["initially_visible"]
        and entity["id"] not in {controllers[0]["id"], targets[0]["id"]}
    }
    start_player = tuple(int(value) for value in controllers[0]["position"])
    start_target = tuple(int(value) for value in targets[0]["position"])
    authored_v2 = (
        world.get("schema_version") == "xcp-world2d-module-v2"
    )
    player_gravity = bool(controllers[0].get("gravity", False))
    target_gravity = bool(targets[0].get("gravity", False))
    player_movement = str(
        controllers[0].get("movement_mode", "four_way")
    )
    initial = (start_player, start_target)
    queue: deque[
        tuple[
            tuple[tuple[int, int], tuple[int, int] | None],
            tuple[str, ...],
        ]
    ] = deque([(initial, ())])
    visited = {initial}
    maximum_states = min(262144, max(4096, columns * rows * columns * rows))

    def open_cell(
        position: tuple[int, int],
        target: tuple[int, int] | None,
    ) -> bool:
        return (
            0 <= position[0] < columns
            and 0 <= position[1] < rows
            and position not in solid_tiles
            and position not in protected_solids
            and position != target
        )

    def settle_gravity(
        player: tuple[int, int],
        target: tuple[int, int] | None,
    ) -> tuple[tuple[int, int], tuple[int, int] | None]:
        if not authored_v2:
            return player, target
        maximum_passes = rows * 2
        for _ in range(maximum_passes):
            changed = False
            if player_gravity:
                below = (player[0], player[1] + 1)
                if below[1] >= rows:
                    player = start_player
                    changed = True
                elif (
                    below not in solid_tiles
                    and below not in protected_solids
                    and below != target
                ):
                    player = below
                    changed = True
                    if player in hazard_tiles:
                        player = start_player
            if target is not None and target_gravity:
                below = (target[0], target[1] + 1)
                if below[1] >= rows:
                    target = None
                    changed = True
                elif (
                    below not in solid_tiles
                    and below not in protected_solids
                    and below != player
                ):
                    target = below
                    changed = True
                    if target in hazard_tiles:
                        target = None
            if not changed:
                break
        return player, target

    while queue and len(visited) <= maximum_states:
        (player, target), path = queue.popleft()
        for command, (dx, dy) in directions:
            next_player = (player[0] + dx, player[1] + dy)
            next_target = target
            if (
                authored_v2
                and player_movement == "platform_grid"
                and dy != 0
                and player not in climbable_tiles
                and next_player not in climbable_tiles
            ):
                continue
            if target is not None and next_player == target:
                if (
                    authored_v2
                    and player_movement == "platform_grid"
                    and dy != 0
                ):
                    continue
                pushed = (target[0] + dx, target[1] + dy)
                if not open_cell(pushed, None):
                    continue
                next_target = None if pushed in hazard_tiles else pushed
            elif not open_cell(next_player, target):
                continue
            if next_player in hazard_tiles:
                next_player = start_player
            next_player, next_target = settle_gravity(
                next_player,
                next_target,
            )
            next_path = (*path, action_by_command[command])
            if next_target is None:
                compressed: list[dict[str, Any]] = []
                for action in next_path:
                    if compressed and compressed[-1]["action"] == action:
                        compressed[-1]["repeat"] += 1
                    else:
                        compressed.append(
                            {
                                "action": action,
                                "repeat": 1,
                                "wait_ms": 40,
                            }
                        )
                return compressed
            state = (next_player, next_target)
            if state not in visited:
                visited.add(state)
                queue.append((state, next_path))
    return []
