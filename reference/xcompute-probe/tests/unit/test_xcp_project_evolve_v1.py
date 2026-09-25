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

from xcp_agent_lifecycle import _new_ledger
from xcp_creative_project import canonical_json_bytes, sha256_file
from xcp_project_evolve import (
    SCHEMAS,
    _doc_sha,
    _load_json,
    evolve,
    finalize,
    project_tree_sha256,
)
from xcp_source_adapt import adapt


class XcpProjectEvolveV1Tests(unittest.TestCase):
    @staticmethod
    def _write_source(root: pathlib.Path, *, evolved: bool) -> pathlib.Path:
        source = root / ("upstream-next" if evolved else "upstream-base")
        (source / "entities").mkdir(parents=True)
        (source / "levels").mkdir()
        (source / "menu").mkdir()
        (source / "gfx").mkdir()
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
                    "retry=[key(R)]",
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
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "entities" / "player.gd").write_text(
            "\n".join(
                (
                    "extends Node2D",
                    "func next_move():",
                    '    if Input.is_action_pressed("btn_left"):',
                    '        move_in_direction("left")',
                    "",
                )
            ),
            encoding="utf-8",
        )
        tile_x = 20 if evolved else 3
        (source / "levels" / "level_1.tscn").write_text(
            "\n".join(
                (
                    "[gd_scene format=1]",
                    '[node name="level" type="Node2D"]',
                    '[node name="tilemap" type="TileMap" parent="."]',
                    "cell/size = Vector2( 64, 64 )",
                    f"tile_data = IntArray( 196609, 0, {tile_x + 65536}, 2 )",
                    '[node name="start" type="Position2D" parent="."]',
                    "transform/pos = Vector2( 64, 128 )",
                    "",
                )
            ),
            encoding="utf-8",
        )
        (source / "gfx" / "player.png").write_bytes(b"\x89PNG\r\nsame")
        if evolved:
            (source / "entities" / "platform.gd").write_text(
                "extends Node2D\nfunc move_platform():\n    pass\n",
                encoding="utf-8",
            )
        return source

    @staticmethod
    def _adapt_source(
        source: pathlib.Path,
        output: pathlib.Path,
        revision: str,
    ) -> None:
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
            revision=revision,
            authorization_basis="open_source_license",
            authorization_status="verified",
            license_expression="GPL-3.0-or-later",
            attribution="Authorized upstream authors",
        )
        if not result["generation_performed"]:
            raise AssertionError(result)

    @staticmethod
    def _overlay(base_run: pathlib.Path, *, conflict: bool) -> dict:
        inventory = _load_json(base_run / "source-inventory.json")
        ir = _load_json(base_run / "creative-ir.json")
        project_root = base_run / "xcp-project"
        project = _load_json(project_root / "xcp-project.json")
        world = _load_json(project_root / "modules" / "world.json")
        operations = [
            {
                "operation_id": "advance-version",
                "document": "xcp-project.json",
                "pointer": "/version",
                "op": "replace",
                "base_present": True,
                "base_value_sha256": _doc_sha(project["version"]),
                "value": "1.1.0",
                "conflict_policy": "require_human",
            },
            {
                "operation_id": "studio-background",
                "document": "modules/world.json",
                "pointer": "/world/background",
                "op": "replace",
                "base_present": True,
                "base_value_sha256": _doc_sha(world["world"]["background"]),
                "value": "#14213D",
                "conflict_policy": "require_human",
            },
        ]
        if conflict:
            operations.append(
                {
                    "operation_id": "manual-world-width",
                    "document": "modules/world.json",
                    "pointer": "/world/columns",
                    "op": "replace",
                    "base_present": True,
                    "base_value_sha256": _doc_sha(world["world"]["columns"]),
                    "value": 30,
                    "conflict_policy": "require_human",
                }
            )
        return {
            "schema_version": "xcp-creative-evolution-overlay-v1",
            "overlay_id": "foreign-puzzle.manual-overlay",
            "project_id": project["project_id"],
            "base": {
                "source_revision": inventory["source"]["revision"],
                "source_inventory_sha256": sha256_file(
                    base_run / "source-inventory.json"
                ),
                "creative_ir_sha256": sha256_file(
                    base_run / "creative-ir.json"
                ),
                "xcp_project_sha256": sha256_file(
                    project_root / "xcp-project.json"
                ),
                "project_tree_sha256": project_tree_sha256(project_root),
            },
            "purpose": "Preserve the XCP Studio visual treatment across source revisions.",
            "preservation_required": True,
            "operations": operations,
        }

    def test_all_c7_schemas_and_profile_are_valid(self) -> None:
        for key, path in SCHEMAS.items():
            with self.subTest(key=key):
                schema = json.loads(path.read_text(encoding="utf-8"))
                Draft202012Validator.check_schema(schema)
        profile = json.loads(
            (
                ROOT
                / "profiles"
                / "creative"
                / "xcp-project-evolution-v1.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(
            profile["authority"]["c6_backend"],
            "xcp.creative_ir_to_project.v1",
        )
        self.assertEqual(
            profile["authority"]["lifecycle_owner"],
            "C5_AGENT_NATIVE_CREATION_GATE",
        )
        self.assertEqual(profile["status"], "PUBLIC_RECONSTRUCTION_AWAITING_VALIDATION")
        self.assertFalse(profile["invariants"]["parallel_c5_lifecycle_allowed"])
        self.assertTrue(profile["package_strategy"]["pc_side_by_default"])
        self.assertTrue(profile["package_strategy"]["public_worker_package_required_for_live_validation"])
        self.assertEqual(profile["validation"]["status"], "NOT_VALIDATED_FROM_PUBLIC_BUILD")
        self.assertFalse(profile["validation"]["historical_operational_evidence_included"])

    def test_evolution_preserves_overlay_and_reuses_unchanged_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_source = self._write_source(root, evolved=False)
            target_source = self._write_source(root, evolved=True)
            base_run = root / "base-run"
            self._adapt_source(base_source, base_run, "base-revision")
            overlay_path = root / "overlay.json"
            overlay_path.write_bytes(
                canonical_json_bytes(self._overlay(base_run, conflict=False))
            )
            output = root / "evolved-run"
            result = evolve(
                base_run,
                target_source,
                overlay_path,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="target-revision",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            self.assertTrue(result["ok"])
            project = _load_json(output / "xcp-project" / "xcp-project.json")
            world = _load_json(output / "xcp-project" / "modules" / "world.json")
            self.assertEqual(project["version"], "1.1.0")
            self.assertEqual(world["world"]["background"], "#14213D")
            diff = _load_json(output / "source-semantic-diff.json")
            self.assertTrue(diff["has_semantic_changes"])
            self.assertGreater(diff["semantic_summary"]["modified"], 0)
            reuse = _load_json(output / "reuse-manifest.json")
            self.assertGreater(reuse["counts"]["reused_by_content_hash"], 0)
            self.assertGreater(reuse["counts"]["rebuilt"], 0)
            self.assertGreater(reuse["counts"]["overlay_applied"], 0)
            reconciliation = _load_json(output / "reconciliation-plan.json")
            self.assertTrue(reconciliation["overlay_preserved"])
            report = _load_json(output / "evolution-report.json")
            self.assertEqual(report["decision"], "ready_for_c5")
            handoff = _load_json(output / "c6-to-c5-handoff.json")
            self.assertEqual(handoff["lifecycle_gate"], "C5_AGENT_NATIVE_CREATION_GATE")
            self.assertEqual(
                next(
                    item["sha256"]
                    for item in handoff["artifacts"]
                    if item["role"] == "xcp.project"
                ),
                sha256_file(output / "xcp-project" / "xcp-project.json"),
            )

    def test_source_overlay_collision_stops_before_final_project(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_source = self._write_source(root, evolved=False)
            target_source = self._write_source(root, evolved=True)
            base_run = root / "base-run"
            self._adapt_source(base_source, base_run, "base-revision")
            overlay_path = root / "overlay.json"
            overlay_path.write_bytes(
                canonical_json_bytes(self._overlay(base_run, conflict=True))
            )
            output = root / "conflicted-run"
            result = evolve(
                base_run,
                target_source,
                overlay_path,
                output,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="target-revision",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            self.assertFalse(result["ok"])
            self.assertEqual(result["decision"], "blocked_conflict")
            self.assertIn("manual-world-width", result["conflicts"])
            self.assertFalse((output / "xcp-project").exists())
            reconciliation = _load_json(output / "reconciliation-plan.json")
            self.assertFalse(reconciliation["overlay_preserved"])
            self.assertEqual(reconciliation["decision"], "blocked_conflict")

    def test_finalizer_binds_exact_c5_base_update_and_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_source = self._write_source(root, evolved=False)
            target_source = self._write_source(root, evolved=True)
            base_run = root / "base-run"
            self._adapt_source(base_source, base_run, "base-revision")
            overlay_path = root / "overlay.json"
            overlay_path.write_bytes(
                canonical_json_bytes(self._overlay(base_run, conflict=False))
            )
            run_root = root / "evolved-run"
            evolve(
                base_run,
                target_source,
                overlay_path,
                run_root,
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json",
                project_name="Foreign Puzzle",
                origin_kind="git",
                origin_locator="https://example.invalid/foreign-puzzle",
                revision="target-revision",
                authorization_basis="open_source_license",
                authorization_status="verified",
                license_expression="GPL-3.0-or-later",
                attribution="Authorized upstream authors",
            )
            base_project_path = base_run / "xcp-project" / "xcp-project.json"
            target_project_path = run_root / "xcp-project" / "xcp-project.json"
            intent_path = run_root / "c5-intent.json"
            target_project = _load_json(target_project_path)
            ledger = _new_ledger(
                target_project["project_id"],
                sha256_file(intent_path),
            )
            ledger["status"] = "passed"
            ledger_path = run_root / "c5-correction-ledger.json"
            ledger_path.write_bytes(canonical_json_bytes(ledger))
            operations = (
                "discover",
                "validate",
                "build",
                "verify",
                "install",
                "activate",
                "launch",
                "observe",
                "interact",
                "capture",
                "update",
                "rollback",
                "cleanup",
            )
            bundle_a = "a" * 64
            bundle_b = "b" * 64
            receipt = {
                "schema_version": "xcp-agent-lifecycle-receipt-v1",
                "run_id": "foreign-puzzle-c7",
                "project_id": target_project["project_id"],
                "project_version": "1.0.0",
                "intent_sha256": sha256_file(intent_path),
                "project_sha256": sha256_file(base_project_path),
                "bundle_sha256": bundle_a,
                "host_profile_sha256": _load_json(
                    run_root / "evolution-report.json"
                )["identities"]["host_profile_sha256"],
                "version_transition": {
                    "schema_version": "xcp-agent-version-transition-v1",
                    "base": {
                        "project_version": "1.0.0",
                        "project_sha256": sha256_file(base_project_path),
                        "bundle_sha256": bundle_a,
                        "install_id": "base-install",
                    },
                    "update": {
                        "project_version": "1.1.0",
                        "project_sha256": sha256_file(target_project_path),
                        "bundle_sha256": bundle_b,
                        "install_id": "update-install",
                    },
                    "rollback": {
                        "project_version": "1.0.0",
                        "project_sha256": sha256_file(base_project_path),
                        "bundle_sha256": bundle_a,
                        "install_id": "base-install",
                    },
                    "exact_update_activated": True,
                    "exact_base_restored": True,
                },
                "operations": [
                    {
                        "ordinal": index,
                        "name": name,
                        "outcome": "pass",
                        "response_schema_version": "test.v1",
                        "error_code": "",
                        "evidence_refs": [],
                    }
                    for index, name in enumerate(operations, start=1)
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
                "artifacts": [
                    {
                        "role": "creative.capture",
                        "reference": f"capture-{index}.bgra",
                        "bytes": 4,
                        "sha256": str(index) * 64,
                    }
                    for index in (1, 2, 3)
                ],
                "correction_ledger_sha256": sha256_file(ledger_path),
                "decision": "pass",
            }
            receipt_path = run_root / "c5-receipt.json"
            receipt_path.write_bytes(canonical_json_bytes(receipt))
            final_path = run_root / "evolution-final.json"
            result = finalize(run_root, receipt_path, ledger_path, final_path)
            self.assertTrue(result["ok"])
            final = _load_json(final_path)
            self.assertEqual(final["decision"], "pass")
            self.assertTrue(final["c5_lifecycle"]["exact_transition_bound"])
            self.assertEqual(final["c5_lifecycle"]["capture_count"], 3)


if __name__ == "__main__":
    unittest.main()
