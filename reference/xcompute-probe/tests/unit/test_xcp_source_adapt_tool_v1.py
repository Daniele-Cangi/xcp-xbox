import json
import pathlib
import sys
import tempfile
import unittest

from jsonschema import Draft202012Validator


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from xcp_source_adapt import (
    SCHEMAS,
    SourceAdaptError,
    _world2d_module,
    _world2d_solution,
    adapt,
    describe,
    detect_source,
    finalize_readiness,
    inspect_source,
    probe_fidelity,
)
from xcp_agent_lifecycle import _append_correction, _new_ledger
from xcp_creative_project import canonical_json_bytes, sha256_file


class XcpSourceAdaptToolV1Tests(unittest.TestCase):
    @staticmethod
    def _write_source(root: pathlib.Path) -> pathlib.Path:
        source = root / "upstream"
        (source / "entities").mkdir(parents=True)
        (source / "levels" / "tutorial").mkdir(parents=True)
        (source / "menu").mkdir()
        (source / "audio").mkdir()
        (source / "gfx" / "player").mkdir(parents=True)
        (source / "LICENSE.txt").write_text(
            "GNU GENERAL PUBLIC LICENSE Version 3\n",
            encoding="utf-8",
        )
        (source / "engine.cfg").write_text(
            "\n".join(
                (
                    "[application]",
                    'name="Foreign Puzzle"',
                    'main_scene="res://menu/menu.tscn"',
                    "",
                    "[input]",
                    "btn_left=[key(Left)]",
                    "btn_right=[key(Right)]",
                    "btn_up=[key(Up)]",
                    "btn_down=[key(Down)]",
                    "place_bomb=[key(Space)]",
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "menu" / "menu.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene format=1]",
                    '[node name="menu" type="Control"]',
                    '[node name="play" type="Button" parent="."]',
                    'text = "Play"',
                    '[node name="music" type="StreamPlayer" parent="."]',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "player.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene load_steps=2 format=1]",
                    '[ext_resource path="res://gfx/player/player1.png" '
                    'type="Texture" id=1]',
                    '[node name="player" type="KinematicBody2D"]',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "player.gd").write_text(
            "\n".join(
                (
                    'extends "entity.gd"',
                    "func next_move():",
                    '    if Input.is_action_pressed("btn_left"):',
                    '        move_in_direction("left")',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "entity.gd").write_text(
            "\n".join(
                (
                    "extends KinematicBody2D",
                    "const AUTOMOVE_SOUNDS = {",
                    '    push = "box_hit",',
                    "}",
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "entity.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene format=1]",
                    '[node name="entity" type="KinematicBody2D"]',
                    '[node name="positional_audio" '
                    'type="SamplePlayer2D" parent="."]',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "levels" / "tutorial" / "level_1.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene load_steps=2 format=1]",
                    '[ext_resource path="res://entities/box.tscn" '
                    'type="PackedScene" id=1]',
                    '[node name="level" type="Node2D"]',
                    '[node name="tilemap" type="TileMap" parent="."]',
                    "cell/size = Vector2( 64, 64 )",
                    "tile_data = IntArray( 196609, 0, 196610, 2 )",
                    '[node name="start" type="Position2D" parent="."]',
                    "transform/pos = Vector2( 64, 2.18279e-11 )",
                    '[node name="box" parent="." instance=ExtResource( 1 )]',
                    "transform/pos = Vector2( 128, 128 )",
                    '[node name="hint" type="Label" parent="."]',
                    'text = "Push the radioactive barrel into the acid lake."',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "box.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene load_steps=2 format=1]",
                    '[ext_resource path="res://gfx/radioactive.png" '
                    'type="Texture" id=1]',
                    '[node name="box" type="KinematicBody2D"]',
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "gfx" / "player" / "player1.png").write_bytes(b"png-player")
        (source / "gfx" / "radioactive.png").write_bytes(b"png-barrel")
        (source / "gfx" / "acid_anim.png").write_bytes(b"png-acid")
        (source / "audio" / "music.ogg").write_bytes(b"OggS-test")
        (source / "audio" / "music.wav").write_bytes(b"RIFF-test")
        (source / "audio" / "box_hit.wav").write_bytes(
            bytes.fromhex(
                "524946462400000057415645666d74201000000001000100401f0000"
                "401f0000010008006461746100000000"
            )
        )
        return source

    def test_godot_scientific_notation_preserves_player_start(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = self._write_source(pathlib.Path(temporary))
            adapter, _ = detect_source(source)
            inventory, inspected_adapter = inspect_source(
                source,
                project_name="Foreign Puzzle",
                origin_kind="authorized_local_tree",
                origin_locator="fixture",
                revision="fixture-r1",
                authorization_basis="provided_by_rightsholder",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Fixture author",
            )
            self.assertEqual(inspected_adapter.adapter_id, adapter.adapter_id)
            ir = inspected_adapter.extract(source, inventory)
            level = next(
                scene
                for scene in ir["scenes"]
                if scene["attributes"]["source_path"]
                == "levels/tutorial/level_1.tscn"
            )
            self.assertEqual(
                level["attributes"]["grid_player_start"],
                [1, 0],
            )

    def test_semantic_inventory_counts_member_state_not_function_locals(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            player_path = source / "entities" / "player.gd"
            player_path.write_text(
                "\n".join(
                    (
                        "extends KinematicBody2D",
                        "export(bool) var fall = true",
                        "onready var sprite = get_node(\"sprite\")",
                        "const TILE_SIZE = 64",
                        "func next_move():",
                        "    var local_direction = \"left\"",
                        "    move_in_direction(local_direction)",
                        "",
                    )
                ),
                encoding="utf-8",
            )
            inventory, adapter = inspect_source(
                source,
                project_name="Foreign Puzzle",
                origin_kind="authorized_local_tree",
                origin_locator="fixture",
                revision="fixture-r1",
                authorization_basis="provided_by_rightsholder",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Fixture author",
            )
            semantic = adapter.extract_semantic_inventory(
                source,
                inventory,
            )
            member_names = {
                record["name"]
                for record in semantic["records"]
                if record["locator"]["path"] == "entities/player.gd"
                and record["kind"] == "godot.behavior.member_state"
            }
            self.assertEqual(
                member_names,
                {"fall", "sprite", "TILE_SIZE"},
            )
            self.assertNotIn("local_direction", member_names)

    def test_schemas_are_valid_after_progressive_readiness_change(self) -> None:
        for key, path in SCHEMAS.items():
            with self.subTest(key=key):
                schema = json.loads(path.read_text(encoding="utf-8"))
                Draft202012Validator.check_schema(schema)

    def test_registry_detects_godot2_without_emitting_xcp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = self._write_source(pathlib.Path(temporary))
            adapter, detection = detect_source(source)
            self.assertEqual(adapter.adapter_id, "godot.source-adapter")
            self.assertEqual(detection.ecosystem, "godot_2_x")
            self.assertEqual(detection.confidence, 1.0)
            registry = describe()
            self.assertFalse(registry["adapters"][0]["may_emit_xcp_project"])
            self.assertFalse(registry["adapters"][0]["may_target_xbox"])
            self.assertIn("probe-fidelity", registry["commands"])

    def test_fidelity_probe_executes_all_oracles_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            output = root / "adapted"
            adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            evidence_path = output / "fidelity-evidence.json"
            result = probe_fidelity(output, evidence_path)
            self.assertTrue(result["ok"])
            self.assertEqual(result["decision"], "fail")
            self.assertEqual(
                set(result["oracle_outcomes"]),
                {"structural", "behavioral", "visual", "audio", "state"},
            )
            self.assertEqual(
                result["world_documents"],
                {
                    "exact": 1,
                    "expected": 1,
                    "campaign_router_exact": True,
                },
            )
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            self.assertEqual(
                evidence["human_playtest"]["outcome"],
                "not_run",
            )
            self.assertEqual(
                evidence["human_playtest"]["evidence_refs"],
                [],
            )

    def test_campaign_source_map_and_fidelity_are_scene_specific(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            level_one = (
                source / "levels" / "tutorial" / "level_1.tscn"
            ).read_text(encoding="utf-8")
            (source / "levels" / "tutorial" / "level_2.tscn").write_text(
                level_one.replace(
                    "Push the radioactive barrel into the acid lake.",
                    "Repeat the authored puzzle.",
                ),
                encoding="utf-8",
            )
            output = root / "adapted"
            adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            source_map = json.loads(
                (output / "source-map.json").read_text(encoding="utf-8")
            )
            level_two_map = next(
                entry
                for entry in source_map["entries"]
                if "scene.levels.tutorial.level_2" in entry["ir_refs"]
            )
            self.assertEqual(
                level_two_map["xcp_refs"],
                ["module:world.tutorial.2"],
            )
            semantic = json.loads(
                (output / "semantic-inventory.json").read_text(
                    encoding="utf-8"
                )
            )
            contract = json.loads(
                (output / "fidelity-contract.json").read_text(
                    encoding="utf-8"
                )
            )
            mappings = {
                mapping["semantic_ref"]: mapping
                for mapping in contract["mappings"]
            }
            represented = {
                record["name"]: mappings[record["semantic_id"]][
                    "classification"
                ]
                for record in semantic["records"]
                if record["locator"]["path"]
                == "levels/tutorial/level_2.tscn"
                and record["category"] == "scene_graph"
                and record["name"]
                in {
                    "level:Node2D",
                    "tilemap:TileMap",
                    "start:Position2D",
                    "box:PackedSceneInstance",
                }
            }
            self.assertEqual(
                represented,
                {
                    "level:Node2D": "translated",
                    "tilemap:TileMap": "translated",
                    "start:Position2D": "translated",
                    "box:PackedSceneInstance": "translated",
                },
            )
            level_two_records = [
                record
                for record in semantic["records"]
                if record["locator"]["path"]
                == "levels/tutorial/level_2.tscn"
            ]
            packed_dependency = next(
                record
                for record in level_two_records
                if record["kind"] == "godot.resource.external"
                and record["name"]
                == "PackedScene:res://entities/box.tscn"
            )
            self.assertEqual(
                mappings[packed_dependency["semantic_id"]][
                    "classification"
                ],
                "translated",
            )
            tilemap_collision = next(
                record
                for record in level_two_records
                if record["category"] == "collision_physics"
                and record["name"] == "tilemap:TileMap"
            )
            self.assertEqual(
                mappings[tilemap_collision["semantic_id"]][
                    "classification"
                ],
                "translated",
            )

    def test_real_pipeline_stops_fail_closed_on_general_world2d_gap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            output = root / "adapted"
            profile = json.loads(
                (
                    ROOT
                    / "profiles"
                    / "creative"
                    / "xcp-creative-host-development-v1.json"
                ).read_text(encoding="utf-8")
            )
            profile["module_kinds"] = [
                item
                for item in profile["module_kinds"]
                if item["id"]
                not in {
                    "xcp.world2d.v1",
                    "xcp.world2d.v2",
                    "xcp.world2d.campaign.v1",
                }
            ]
            profile["capabilities"] = [
                item
                for item in profile["capabilities"]
                if item["id"]
                not in {
                    "world2d.deterministic",
                    "world2d.gravity",
                    "world2d.campaign",
                    "render.sprite_atlas",
                }
            ]
            legacy_profile = root / "legacy-host.json"
            legacy_profile.write_text(
                json.dumps(profile),
                encoding="utf-8",
            )
            result = adapt(
                source,
                output,
                legacy_profile,
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            self.assertFalse(result["ok"])
            self.assertEqual(result["decision"], "not_ready")
            self.assertEqual(
                result["generation_decision"],
                "blocked_unsupported",
            )
            self.assertFalse(result["generation_performed"])
            self.assertTrue(
                result["worker_gap"]["general_capability_missing"]
            )
            self.assertFalse((output / "xcp-project").exists())
            self.assertFalse((output / "source-map.json").exists())

            documents = {
                "inventory": json.loads(
                    (output / "source-inventory.json").read_text(
                        encoding="utf-8"
                    )
                ),
                "semantic_inventory": json.loads(
                    (output / "semantic-inventory.json").read_text(
                        encoding="utf-8"
                    )
                ),
                "ir": json.loads(
                    (output / "creative-ir.json").read_text(encoding="utf-8")
                ),
                "plan": json.loads(
                    (output / "adaptation-plan.json").read_text(
                        encoding="utf-8"
                    )
                ),
                "readiness": json.loads(
                    (output / "readiness-report.json").read_text(
                        encoding="utf-8"
                    )
                ),
                "fidelity_contract": json.loads(
                    (output / "fidelity-contract.json").read_text(
                        encoding="utf-8"
                    )
                ),
            }
            for key, document in documents.items():
                with self.subTest(key=key):
                    schema = json.loads(
                        SCHEMAS[key].read_text(encoding="utf-8")
                    )
                    Draft202012Validator(schema).validate(document)

            inventory = documents["inventory"]
            self.assertEqual(
                inventory["source"]["revision"],
                "0123456789abcdef",
            )
            self.assertTrue(
                all(
                    item["provenance"]["origin"]
                    == "https://example.invalid/foreign-puzzle"
                    for item in inventory["files"]
                )
            )
            ir = documents["ir"]
            asset_ids = [item["id"] for item in ir["assets"]]
            self.assertEqual(len(asset_ids), len(set(asset_ids)))
            self.assertIn("asset.audio.music.ogg", asset_ids)
            self.assertIn("asset.audio.music.wav", asset_ids)
            plan = documents["plan"]
            semantic_count = sum(
                len(ir[key])
                for key in (
                    "scenes",
                    "entities",
                    "behaviors",
                    "input_actions",
                    "ui",
                    "assets",
                    "state",
                )
            )
            self.assertEqual(len(plan["items"]), semantic_count)
            self.assertEqual(
                sum(plan["classification_counts"].values()),
                semantic_count,
            )
            self.assertIn(
                "world2d.deterministic",
                {
                    capability
                    for item in plan["items"]
                    for capability in item["required_host_capabilities"]
                },
            )
            readiness = documents["readiness"]
            self.assertEqual(readiness["decision"], "not_ready")
            self.assertEqual(
                readiness["adaptation_gate"]["target_level"],
                "source_faithful",
            )
            self.assertEqual(readiness["corrections"], [])
            self.assertEqual(
                readiness["validation"]["xcp_project"],
                "not_run",
            )

    def test_shared_backend_generates_world2d_for_current_host(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            output = root / "adapted"
            result = adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            self.assertFalse(result["ok"])
            self.assertEqual(result["decision"], "not_ready")
            self.assertEqual(result["achieved_level"], "behavioral_subset")
            self.assertTrue(result["generation_performed"])
            project_root = output / "xcp-project"
            project = json.loads(
                (project_root / "xcp-project.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(project["project_id"], "foreign.puzzle")
            self.assertEqual(project["title"], "Foreign Puzzle")
            self.assertIn(
                "xcp.world2d.v2",
                {item["kind"] for item in project["modules"]},
            )
            self.assertIn(
                "xcp.canvas2d.v1",
                {item["kind"] for item in project["modules"]},
            )
            self.assertIn(
                "xcp.audio.v1",
                {item["kind"] for item in project["modules"]},
            )
            self.assertNotIn(
                "xcp.ui.v1",
                {item["kind"] for item in project["modules"]},
            )
            self.assertEqual(project["version"], "1.3.2")
            world = json.loads(
                (project_root / "modules" / "world.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                world["schema_version"],
                "xcp-world2d-module-v2",
            )
            self.assertTrue(
                any(
                    entity["archetype"] == "barrel"
                    for entity in world["entities"]
                )
            )
            self.assertTrue(
                any(tile["kind"] == "hazard" for tile in world["tiles"])
            )
            player = next(
                entity
                for entity in world["entities"]
                if entity["id"] == "player"
            )
            barrel = next(
                entity
                for entity in world["entities"]
                if entity["archetype"] == "barrel"
            )
            hazard = next(
                tile for tile in world["tiles"] if tile["kind"] == "hazard"
            )
            self.assertEqual(
                player["render"]["asset_id"],
                "asset.gfx.player.player1.png",
            )
            self.assertEqual(
                barrel["render"]["asset_id"],
                "asset.gfx.radioactive.png",
            )
            self.assertEqual(
                hazard["render"]["asset_id"],
                "asset.gfx.acid_anim.png",
            )
            hud = json.loads(
                (project_root / "modules" / "hud.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                next(node for node in hud["nodes"] if node["id"] == "hud.goal")[
                    "text"
                ],
                "Push the radioactive barrel into the acid lake.",
            )
            self.assertEqual(
                next(
                    node
                    for node in hud["nodes"]
                    if node["id"] == "hud.complete"
                )["transform"]["position"],
                [960, 360],
            )
            logic = json.loads(
                (project_root / "modules" / "logic.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertIn(
                "hud.show-completion",
                {rule["id"] for rule in logic["rules"]},
            )
            audio = json.loads(
                (project_root / "modules" / "audio.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                audio["cues"],
                [
                    {
                        "clip_id": "clip.asset.audio.box_hit.wav",
                        "event": "world.entity-pushed",
                        "gain": 1,
                        "id": "cue.asset.audio.box_hit.wav.1",
                    }
                ],
            )
            self.assertIn("audio.playback", project["requested_capabilities"])
            readiness = json.loads(
                (output / "readiness-report.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(readiness["decision"], "not_ready")
            self.assertEqual(readiness["validation"]["xcp_project"], "pass")
            self.assertGreater(
                readiness["adaptation_gate"]["blocking_record_count"],
                0,
            )
            self.assertTrue(result["c5_handoff_generated"])
            handoff = json.loads(
                (output / "c6-to-c5-handoff.json").read_text(
                    encoding="utf-8"
                )
            )
            Draft202012Validator(
                json.loads(SCHEMAS["handoff"].read_text(encoding="utf-8"))
            ).validate(handoff)
            self.assertEqual(handoff["status"], "diagnostic_only")
            self.assertIn(
                "source.semantic_inventory",
                {item["role"] for item in handoff["artifacts"]},
            )
            self.assertIn(
                "adaptation.fidelity_contract",
                {item["role"] for item in handoff["artifacts"]},
            )
            semantics = json.loads(
                (output / "semantic-inventory.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(semantics["scope"]["mode"], "whole_project")
            self.assertEqual(semantics["scope"]["parse_failures"], [])
            self.assertEqual(
                semantics["summary"]["record_count"],
                len(semantics["records"]),
            )
            fidelity_contract = json.loads(
                (output / "fidelity-contract.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                fidelity_contract["achieved_level"],
                "behavioral_subset",
            )
            self.assertEqual(
                len(fidelity_contract["mappings"]),
                semantics["summary"]["record_count"],
            )
            audio_node_classifications = {
                record["locator"]["path"]: next(
                    mapping["classification"]
                    for mapping in fidelity_contract["mappings"]
                    if mapping["semantic_ref"] == record["semantic_id"]
                )
                for record in semantics["records"]
                if record["kind"] == "godot.audio.node_feature"
            }
            self.assertEqual(
                audio_node_classifications["entities/entity.tscn"],
                "translated",
            )
            self.assertEqual(
                audio_node_classifications["menu/menu.tscn"],
                "unsupported",
            )
            intent = json.loads(
                (output / "c5-intent.json").read_text(encoding="utf-8")
            )
            self.assertEqual(intent["project_id"], project["project_id"])
            self.assertIn(
                "world-completed",
                {item["id"] for item in intent["acceptance"]},
            )
            sequence = next(
                item
                for item in intent["interaction_sequences"]
                if item["id"] == "adapted-project-complete-objective"
            )
            self.assertGreaterEqual(
                sum(step["repeat"] for step in sequence["steps"]),
                1,
            )

    def test_shared_backend_generates_campaign_and_c5_advancement(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            level_one = (
                source / "levels" / "tutorial" / "level_1.tscn"
            ).read_text(encoding="utf-8")
            (
                source / "levels" / "tutorial" / "level_2.tscn"
            ).write_text(
                level_one.replace(
                    "Push the radioactive barrel into the acid lake.",
                    "Complete the second authorized fixture level.",
                ),
                encoding="utf-8",
            )
            (source / "levels" / "packs.txt").write_text(
                "tutorial 2\n",
                encoding="utf-8",
            )
            (source / "levels" / "tutorial" / "names.txt").write_text(
                "1: Fixture One\n2: Fixture Two\n",
                encoding="utf-8",
            )
            output = root / "campaign"
            result = adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle Campaign",
                origin_kind="authorized_local_tree",
                origin_locator="fixture",
                revision="fixture-campaign-r1",
                authorization_basis="provided_by_rightsholder",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Fixture author",
            )
            self.assertTrue(result["generation_performed"])
            project_root = output / "xcp-project"
            project = json.loads(
                (project_root / "xcp-project.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                project["project_id"],
                "foreign.puzzle.campaign",
            )
            self.assertEqual(project["title"], "Foreign Puzzle")
            entry = next(
                item
                for item in project["modules"]
                if item["id"] == project["entry_module"]
            )
            self.assertEqual(entry["kind"], "xcp.world2d.campaign.v1")
            self.assertEqual(
                sum(
                    item["kind"] == "xcp.world2d.v2"
                    for item in project["modules"]
                ),
                2,
            )
            campaign = json.loads(
                (project_root / entry["path"]).read_text(encoding="utf-8")
            )
            self.assertEqual(
                [scene["title"] for scene in campaign["scenes"]],
                ["Fixture One", "Fixture Two"],
            )
            self.assertEqual(len(campaign["transitions"]), 1)

            intent = json.loads(
                (output / "c5-intent.json").read_text(encoding="utf-8")
            )
            assertion_ids = {
                item["id"] for item in intent["acceptance"]
            }
            self.assertIn("campaign-entry-completed", assertion_ids)
            self.assertIn("campaign-advanced", assertion_ids)
            self.assertNotIn("world-completed", assertion_ids)
            sequence = next(
                item
                for item in intent["interaction_sequences"]
                if item["id"]
                == "adapted-project-complete-and-advance"
            )
            self.assertEqual(
                sequence["steps"][-1]["action"],
                "input.next-level",
            )

    def test_long_hazard_strip_is_compacted_to_a_playable_viewport(self) -> None:
        ir = {
            "scenes": [
                {
                    "id": "scene.levels.tutorial.level_1",
                    "attributes": {
                        "adaptation_entry": True,
                        "grid_cell_size": 64,
                        "grid_player_start": [4, 7],
                        "grid_instances": [
                            "box|entities/box.tscn|7|7|gfx/radioactive.png"
                        ],
                        "grid_tiles": [
                            *[
                                f"{x},8,floor,0"
                                for x in range(4, 12)
                            ],
                            *[
                                f"{x},11,hazard,2"
                                for x in range(-1, 17)
                            ],
                        ],
                    },
                }
            ],
            "entities": [
                {
                    "attributes": {
                        "source_path": "entities/player.tscn",
                        "visual_asset_id": (
                            "asset.gfx.player.player1.png"
                        ),
                    }
                }
            ],
            "assets": [
                {
                    "id": "asset.gfx.player.player1.png",
                    "media_type": "image/png",
                },
                {
                    "id": "asset.gfx.radioactive.png",
                    "media_type": "image/png",
                },
                {
                    "id": "asset.gfx.acid_anim.png",
                    "media_type": "image/png",
                },
            ],
            "input_actions": [
                {"semantic_action": f"input.btn_{direction}"}
                for direction in ("up", "down", "left", "right")
            ],
        }
        world = _world2d_module(ir)
        self.assertEqual(world["world"]["columns"], 8)
        self.assertEqual(world["world"]["rows"], 6)
        player = next(
            item for item in world["entities"] if item["id"] == "player"
        )
        barrel = next(
            item
            for item in world["entities"]
            if item["archetype"] == "barrel"
        )
        self.assertEqual(player["position"], [0, 1])
        self.assertEqual(barrel["position"], [3, 1])
        hazards = [
            item for item in world["tiles"] if item["kind"] == "hazard"
        ]
        self.assertEqual(len(hazards), 8)
        self.assertEqual(
            {tuple(item["position"]) for item in hazards},
            {(x, 5) for x in range(8)},
        )
        viewport_cell = world["world"]["cell_size"] * min(
            640 / world["world"]["width"],
            360 / world["world"]["height"],
        )
        self.assertGreaterEqual(viewport_cell, 60)
        solution = _world2d_solution(world)
        self.assertIsNotNone(solution)
        self.assertEqual(
            sum(step["repeat"] for step in solution or []),
            8,
        )
        authored = _world2d_module(ir, authored_v2=True)
        self.assertEqual(
            authored["schema_version"],
            "xcp-world2d-module-v2",
        )
        self.assertEqual(authored["world"]["width"], 1024)
        self.assertEqual(authored["world"]["height"], 768)
        self.assertEqual(authored["world"]["content_origin"], [-64.0, 0.0])
        self.assertEqual(
            sum(tile["kind"] == "hazard" for tile in authored["tiles"]),
            18,
        )
        self.assertEqual(
            sum(tile["solid"] for tile in authored["tiles"]),
            8,
        )
        authored_solution = _world2d_solution(authored)
        self.assertEqual(
            authored_solution,
            [
                {
                    "action": "input.btn_right",
                    "repeat": 7,
                    "wait_ms": 40,
                }
            ],
        )

    def test_world_backend_collapses_only_exact_duplicate_solid_instances(
        self,
    ) -> None:
        ir = {
            "scenes": [
                {
                    "id": "scene.levels.fixture.level_1",
                    "attributes": {
                        "adaptation_entry": True,
                        "grid_cell_size": 64,
                        "grid_player_start": [1, 1],
                        "grid_instances": [
                            "box-a|entities/static_box.tscn|4|3|gfx/box.png",
                            "box-b|entities/static_box.tscn|4|3|gfx/box.png",
                        ],
                        "grid_tiles": ["1,2,floor,0", "4,4,floor,0"],
                        "source_viewport": [1024, 768],
                    },
                }
            ],
            "entities": [
                {
                    "attributes": {
                        "source_path": "entities/player.tscn",
                        "visual_asset_id": "asset.gfx.player.player1.png",
                    }
                }
            ],
            "assets": [
                {
                    "id": "asset.gfx.player.player1.png",
                    "media_type": "image/png",
                },
                {
                    "id": "asset.gfx.box.png",
                    "media_type": "image/png",
                },
            ],
            "input_actions": [],
        }
        world = _world2d_module(ir, authored_v2=True)
        barrels = [
            item
            for item in world["entities"]
            if item["archetype"] == "barrel"
        ]
        self.assertEqual(len(barrels), 1)
        self.assertEqual(world["objectives"][0]["count"], 1)

        ir["scenes"][0]["attributes"]["grid_instances"][1] = (
            "box-b|entities/other_box.tscn|4|3|gfx/box.png"
        )
        with self.assertRaises(SourceAdaptError) as raised:
            _world2d_module(ir, authored_v2=True)
        self.assertEqual(
            raised.exception.code,
            "xcp.adapt.world_entity_overlap",
        )

    def test_adaptation_accepts_an_explicit_immutable_project_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            output = root / "adapted-update"
            result = adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
                project_version="1.2.1",
            )
            self.assertFalse(result["ok"])
            self.assertTrue(result["generation_performed"])
            self.assertEqual(result["achieved_level"], "behavioral_subset")
            project = json.loads(
                (output / "xcp-project" / "xcp-project.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(project["version"], "1.2.1")

    def test_finalize_rejoins_exact_c5_receipt_and_correction_chain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = self._write_source(root)
            output = root / "adapted"
            adapt(
                source,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="0123456789abcdef",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            intent_path = output / "c5-intent.json"
            project_path = output / "xcp-project" / "xcp-project.json"
            project = json.loads(project_path.read_text(encoding="utf-8"))
            preliminary = json.loads(
                (output / "readiness-report.json").read_text(
                    encoding="utf-8"
                )
            )
            ledger = _new_ledger(
                project["project_id"],
                sha256_file(intent_path),
            )
            for stage, code in (
                ("conversion", "xcp.adapt.ir_id_duplicate"),
                ("live", "xcp.creative.input_action_unsupported"),
            ):
                ledger = _append_correction(
                    ledger,
                    {
                        "stage": stage,
                        "error": {
                            "code": code,
                            "details": {"field": "test"},
                        },
                        "input_identity": {
                            "project_sha256": sha256_file(project_path),
                            "bundle_sha256": "",
                            "host_profile_sha256": preliminary["identities"][
                                "host_profile_sha256"
                            ],
                        },
                        "changes": [],
                        "change_summary": f"Corrected the {stage} error.",
                        "retry": {"outcome": "pass", "error_code": ""},
                        "evidence_refs": [f"test:{stage}"],
                    },
                )
            ledger["status"] = "passed"
            ledger_path = output / "c5-correction-ledger.json"
            ledger_path.write_bytes(canonical_json_bytes(ledger))
            operation_names = (
                "install",
                "launch",
                "observe",
                "capture",
                "update",
                "rollback",
                "cleanup",
            )
            receipt = {
                "schema_version": "xcp-agent-lifecycle-receipt-v1",
                "run_id": "foreign-puzzle-gate",
                "project_id": project["project_id"],
                "project_version": project["version"],
                "intent_sha256": sha256_file(intent_path),
                "project_sha256": sha256_file(project_path),
                "bundle_sha256": "a" * 64,
                "host_profile_sha256": preliminary["identities"][
                    "host_profile_sha256"
                ],
                "operations": [
                    {
                        "ordinal": ordinal,
                        "name": name,
                        "outcome": "pass",
                        "response_schema_version": "test.v1",
                        "error_code": "",
                        "evidence_refs": [],
                    }
                    for ordinal, name in enumerate(operation_names, start=1)
                ],
                "acceptance": [
                    {
                        "assertion_id": "foreground-active",
                        "phase": "live",
                        "severity": "blocker",
                        "outcome": "pass",
                        "actual": True,
                        "expected": True,
                    }
                ],
                "artifacts": [],
                "correction_ledger_sha256": sha256_file(ledger_path),
                "decision": "pass",
            }
            receipt_path = output / "c5-receipt.json"
            receipt_path.write_bytes(canonical_json_bytes(receipt))
            final_path = output / "readiness-final.json"
            result = finalize_readiness(
                output,
                receipt_path,
                ledger_path,
                final_path,
            )
            self.assertFalse(result["ok"])
            final = json.loads(final_path.read_text(encoding="utf-8"))
            self.assertEqual(
                final["decision"],
                "not_ready",
            )
            self.assertEqual(
                final["adaptation_gate"]["reference_oracle"],
                "not_run",
            )
            self.assertEqual(
                final["adaptation_gate"]["human_playtest"],
                "not_run",
            )
            self.assertGreater(
                result["non_equivalent_semantic_records"],
                0,
            )
            self.assertEqual(
                {item["stage"] for item in final["corrections"]},
                {"conversion", "live"},
            )


if __name__ == "__main__":
    unittest.main()
