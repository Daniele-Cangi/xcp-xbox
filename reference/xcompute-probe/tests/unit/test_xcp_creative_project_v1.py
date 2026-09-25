import contextlib
import hashlib
import io
import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from jsonschema import Draft202012Validator


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from xcp_creative_project import (
    BUNDLE_FILENAME,
    CANVAS_STARTER_TEMPLATE,
    DEFAULT_BUNDLE_SCHEMA,
    DEFAULT_HOST_PROFILE_SCHEMA,
    DEFAULT_PROJECT_SCHEMA,
    MAX_TOTAL_BYTES,
    PROJECT_FILENAME,
    CreativeProjectError,
    build_bundle,
    canonical_json_bytes,
    create_project,
    describe_builder,
    main,
    validate_host_profile,
    validate_project,
    verify_bundle,
)


class XcpCreativeProjectV1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sample = ROOT / "samples" / "creative" / "hello-shapes"

    def copy_sample(self, parent: pathlib.Path) -> pathlib.Path:
        project = parent / "project"
        shutil.copytree(self.sample, project)
        return project

    @staticmethod
    def read_project(project: pathlib.Path) -> dict:
        return json.loads((project / "xcp-project.json").read_text(encoding="utf-8"))

    @staticmethod
    def write_project(project: pathlib.Path, value: dict) -> None:
        (project / "xcp-project.json").write_bytes(canonical_json_bytes(value))

    @staticmethod
    def host_profile() -> dict:
        return {
            "schema_version": "xcp-creative-host-profile-v1",
            "profile_id": "xcp.creative-host.c1-c2-c3-min-target-v1",
            "host_api_version": "0.1.0",
            "implementation_stage": "development_candidate",
            "project_schema_versions": ["xcp-creative-project-v1"],
            "bundle_schema_versions": ["xcp-creative-bundle-v1"],
            "module_kinds": [
                {
                    "id": "xcp.canvas2d.v1",
                    "schema_id": "xcp.canvas2d.schema-v1",
                    "schema_sha256": "0" * 64,
                    "admitted": True,
                    "required_capabilities": [
                        "input.gamepad",
                        "render.canvas2d",
                    ],
                },
                {
                    "id": "xcp.scene3d.v1",
                    "schema_id": "xcp.scene3d.schema-v1",
                    "schema_sha256": "1" * 64,
                    "admitted": True,
                    "required_capabilities": [
                        "input.gamepad",
                        "render.scene3d",
                    ],
                },
            ],
            "asset_formats": [],
            "capabilities": [
                {"id": "input.gamepad", "admitted": True},
                {"id": "render.canvas2d", "admitted": True},
                {"id": "render.scene3d", "admitted": True},
            ],
            "budgets": {
                "max_module_count": 32,
                "max_asset_count": 256,
                "max_file_count": 288,
                "max_file_bytes": 16 * 1024 * 1024,
                "max_project_bytes": 64 * 1024 * 1024,
                "max_installed_workspace_bytes": 8 * 1024 * 1024 * 1024,
            },
            "operations": {
                "install": False,
                "activate": False,
                "list": False,
                "remove": False,
                "rollback": False,
                "launch": False,
                "reload": False,
                "dispatch_input": False,
                "capture": False,
            },
            "observations": [
                "capture.frame",
                "state.inspect",
            ],
            "claim_boundary": {
                "native_payload_upload": False,
                "runtime_shader_compilation": False,
                "process_creation": False,
                "broad_filesystem_access": False,
                "credentials_on_xbox": False,
            },
        }

    @classmethod
    def write_host_profile(
        cls,
        parent: pathlib.Path,
        value: dict | None = None,
    ) -> pathlib.Path:
        path = parent / "host-profile.json"
        path.write_bytes(canonical_json_bytes(value or cls.host_profile()))
        return path

    def test_authoritative_schemas_are_valid_draft_2020_12(self) -> None:
        for path in (
            DEFAULT_PROJECT_SCHEMA,
            DEFAULT_BUNDLE_SCHEMA,
            DEFAULT_HOST_PROFILE_SCHEMA,
            ROOT / "schemas" / "xcp-mesh-asset-v1.schema.json",
        ):
            schema = json.loads(path.read_text(encoding="utf-8"))
            Draft202012Validator.check_schema(schema)

    def test_sample_validates_and_builder_description_binds_schemas(self) -> None:
        result = validate_project(self.sample)
        self.assertEqual(result.manifest["project_id"], "hello-shapes")
        self.assertEqual(result.manifest["entry_module"], "main")
        self.assertEqual(len(result.modules), 1)
        self.assertEqual(len(result.assets), 1)
        self.assertGreater(result.metadata()["total_bytes"], 0)

        description = describe_builder()
        self.assertEqual(
            description["portable_builder_ceilings"]["total_bytes"],
            64 * 1024 * 1024 * 1024,
        )
        self.assertEqual(MAX_TOTAL_BYTES, 64 * 1024 * 1024 * 1024)
        self.assertTrue(
            description["portable_builder_ceilings"]["live_host_may_be_stricter"]
        )
        self.assertEqual(
            description["templates"][0]["id"],
            CANVAS_STARTER_TEMPLATE,
        )
        self.assertIn("create", description["commands"])
        for key, path in (
            ("project_schema", DEFAULT_PROJECT_SCHEMA),
            ("bundle_schema", DEFAULT_BUNDLE_SCHEMA),
            ("host_profile_schema", DEFAULT_HOST_PROFILE_SCHEMA),
        ):
            self.assertEqual(description[key]["bytes"], path.stat().st_size)
            self.assertEqual(
                description[key]["sha256"],
                hashlib.sha256(path.read_bytes()).hexdigest(),
            )

    def test_host_profile_admits_2d_sample_and_binds_validation_and_build(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            profile_path = self.write_host_profile(root)
            profile = validate_host_profile(profile_path)
            self.assertEqual(
                profile.manifest["profile_id"],
                "xcp.creative-host.c1-c2-c3-min-target-v1",
            )
            self.assertEqual(
                {entry["id"] for entry in profile.manifest["module_kinds"]},
                {"xcp.canvas2d.v1", "xcp.scene3d.v1"},
            )

            validation = validate_project(
                self.sample,
                host_profile=profile_path,
            )
            metadata = validation.metadata()
            self.assertEqual(metadata["host_admission"], "accepted")
            self.assertEqual(
                metadata["host_profile_canonical_sha256"],
                hashlib.sha256(profile.manifest_bytes).hexdigest(),
            )

            built = build_bundle(
                self.sample,
                root / "bundle",
                host_profile=profile_path,
            )
            self.assertEqual(built.metadata()["host_admission"], "accepted")
            verify_bundle(root / "bundle")

    def test_host_profile_rejects_unknown_module_and_capability(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            profile_path = self.write_host_profile(root)
            manifest = self.read_project(project)
            manifest["modules"][0]["kind"] = "xcp.audio.v1"
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project, host_profile=profile_path)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_module_kind_unsupported",
            )
            self.assertEqual(raised.exception.details.stage, "host_admission")

            manifest["modules"][0]["kind"] = "xcp.canvas2d.v1"
            manifest["requested_capabilities"].append("audio.playback")
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project, host_profile=profile_path)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_capability_unsupported",
            )

    def test_host_profile_rejects_known_but_unadmitted_asset_format(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            manifest = self.read_project(project)
            manifest["assets"][0]["media_type"] = "image/png"
            self.write_project(project, manifest)
            profile = self.host_profile()
            profile["asset_formats"] = [
                {
                    "id": "xcp.sprite.png.v1",
                    "media_type": "image/png",
                    "decoder": "xcp.decoder.wic-png.v1",
                    "admitted": False,
                    "usages": ["canvas2d.sprite"],
                    "limits": {
                        "max_file_bytes": 16 * 1024 * 1024,
                        "max_width": 4096,
                        "max_height": 4096,
                        "max_decoded_bytes": 64 * 1024 * 1024,
                    },
                }
            ]
            profile_path = self.write_host_profile(root, profile)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project, host_profile=profile_path)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_asset_format_unsupported",
            )

    def test_host_profile_rejects_missing_module_capability(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            profile_path = self.write_host_profile(root)
            manifest = self.read_project(project)
            manifest["requested_capabilities"].remove("input.gamepad")
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project, host_profile=profile_path)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.module_capability_not_requested",
            )
            self.assertIn(
                "input.gamepad",
                raised.exception.details.correction,
            )

    def test_host_profile_rejects_inconsistent_admission_and_tighter_budget(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            profile = self.host_profile()
            profile["capabilities"][0]["admitted"] = False
            inconsistent = self.write_host_profile(root, profile)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_host_profile(inconsistent)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_profile_inconsistent",
            )

            profile = self.host_profile()
            profile["budgets"]["max_project_bytes"] = 1
            constrained = self.write_host_profile(root, profile)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(self.sample, host_profile=constrained)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_project_budget_exceeded",
            )

    def test_build_rejects_host_without_bundle_schema_before_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            profile = self.host_profile()
            profile["bundle_schema_versions"] = [
                "xcp-creative-bundle-v2"
            ]
            profile_path = self.write_host_profile(root, profile)
            output = root / "bundle"
            with self.assertRaises(CreativeProjectError) as raised:
                build_bundle(
                    self.sample,
                    output,
                    host_profile=profile_path,
                )
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.host_bundle_schema_unsupported",
            )
            self.assertFalse(output.exists())

    def test_repeated_builds_emit_identical_manifest_and_content(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            first = build_bundle(self.sample, root / "first")
            second = build_bundle(self.sample, root / "second")
            self.assertEqual(first.manifest_bytes, second.manifest_bytes)
            self.assertEqual(
                first.manifest["integrity"]["content_sha256"],
                second.manifest["integrity"]["content_sha256"],
            )
            self.assertEqual(
                first.manifest["integrity"]["project_manifest_sha256"],
                hashlib.sha256(
                    (self.sample / "xcp-project.json").read_bytes()
                ).hexdigest(),
            )
            self.assertEqual(
                first.manifest["requested_capabilities"],
                sorted(first.manifest["requested_capabilities"]),
            )
            verify_bundle(root / "first")
            verify_bundle(root / "second")

    def test_create_emits_deterministic_valid_public_contract_project(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            first = create_project(
                root / "first",
                project_id="studio-starter",
                title="Studio Starter",
            )
            second = create_project(
                root / "second",
                project_id="studio-starter",
                title="Studio Starter",
            )
            self.assertEqual(
                first.metadata()["schema_version"],
                "xcp-creative-project-create-v1",
            )
            self.assertEqual(first.validation.root, (root / "first").resolve())
            self.assertTrue(first.validation.root.is_dir())
            self.assertEqual(first.template_id, CANVAS_STARTER_TEMPLATE)
            self.assertEqual(
                (root / "first" / PROJECT_FILENAME).read_bytes(),
                (root / "second" / PROJECT_FILENAME).read_bytes(),
            )
            self.assertEqual(
                (root / "first" / "modules" / "main.json").read_bytes(),
                (root / "second" / "modules" / "main.json").read_bytes(),
            )
            validated = validate_project(root / "first")
            self.assertEqual(validated.manifest["project_id"], "studio-starter")
            self.assertEqual(
                validated.manifest["requested_capabilities"],
                ["input.gamepad", "render.canvas2d"],
            )

    def test_create_is_atomic_and_fail_closed_for_invalid_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            invalid = root / "invalid"
            with self.assertRaises(CreativeProjectError) as raised:
                create_project(
                    invalid,
                    project_id="Invalid Project Id",
                    title="Invalid",
                )
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.schema_rejected",
            )
            self.assertFalse(invalid.exists())
            self.assertEqual(
                list(root.glob(".xcp-project-create-*")),
                [],
            )

            existing = root / "existing"
            existing.mkdir()
            marker = existing / "keep.txt"
            marker.write_text("keep\n", encoding="utf-8")
            with self.assertRaises(CreativeProjectError) as raised:
                create_project(
                    existing,
                    project_id="existing-project",
                    title="Existing Project",
                )
            self.assertEqual(raised.exception.code, "xcp.creative.output_exists")
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep\n")

    def test_cli_create_reports_structured_success_and_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = main(
                    [
                        "create",
                        "--project-dir",
                        str(root / "created project"),
                        "--project-id",
                        "created-project",
                        "--title",
                        "Created Project",
                    ]
                )
            self.assertEqual(exit_code, 0)
            result = json.loads(output.getvalue())
            self.assertTrue(result["ok"])
            self.assertEqual(
                result["schema_version"],
                "xcp-creative-project-create-v1",
            )

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = main(
                    [
                        "create",
                        "--project-dir",
                        str(root / "invalid project"),
                        "--project-id",
                        "bad;project",
                        "--title",
                        "Bad Project",
                    ]
                )
            self.assertEqual(exit_code, 2)
            error = json.loads(output.getvalue())["error"]
            self.assertEqual(
                error["code"],
                "xcp.creative.schema_rejected",
            )
            self.assertEqual(
                error["details"]["schema_version"],
                "xcp-creative-error-details-v1",
            )

    def test_cli_reports_machine_readable_success(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_code = main(
                ["validate", "--project-dir", str(self.sample)]
            )
        self.assertEqual(exit_code, 0)
        result = json.loads(output.getvalue())
        self.assertTrue(result["ok"])
        self.assertEqual(
            result["schema_version"],
            "xcp-creative-project-validation-v1",
        )
        with tempfile.TemporaryDirectory() as temporary:
            profile_path = self.write_host_profile(pathlib.Path(temporary))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = main(
                    [
                        "validate",
                        "--project-dir",
                        str(self.sample),
                        "--host-profile",
                        str(profile_path),
                    ]
                )
            self.assertEqual(exit_code, 0)
            profile_bound = json.loads(output.getvalue())
            self.assertEqual(profile_bound["host_admission"], "accepted")
            self.assertEqual(
                profile_bound["host_profile_id"],
                "xcp.creative-host.c1-c2-c3-min-target-v1",
            )

    def test_path_traversal_is_rejected_with_correction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            manifest = self.read_project(project)
            manifest["modules"][0]["path"] = "../outside.json"
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            error = raised.exception.as_dict()["error"]
            self.assertEqual(error["code"], "xcp.creative.path_not_canonical")
            self.assertEqual(
                error["details"]["schema_version"],
                "xcp-creative-error-details-v1",
            )
            self.assertEqual(error["details"]["stage"], "project_validation")
            self.assertTrue(error["details"]["correction"])

    def test_contract_manifest_path_is_reserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.copy_sample(pathlib.Path(temporary))
            manifest = self.read_project(project)
            manifest["modules"][0]["path"] = BUNDLE_FILENAME
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.path_reserved",
            )

    def test_missing_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.copy_sample(pathlib.Path(temporary))
            (project / "modules" / "main.json").unlink()
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.file_missing",
            )

    def test_aggregate_budget_overflow_is_rejected(self) -> None:
        with mock.patch("xcp_creative_project.MAX_TOTAL_BYTES", 1):
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(self.sample)
        self.assertEqual(
            raised.exception.code,
            "xcp.creative.project_budget_exceeded",
        )
        self.assertEqual(
            raised.exception.details.expected,
            "at most 1 bytes",
        )

    def test_duplicate_module_id_and_runtime_path_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            manifest = self.read_project(project)
            manifest["modules"].append(
                {
                    "id": "main",
                    "kind": "xcp.behavior.v1",
                    "path": "assets/palette.json",
                    "depends_on": [],
                }
            )
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.module_id_duplicate",
            )

            manifest["modules"][1]["id"] = "behavior"
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.runtime_path_duplicate",
            )

    def test_dependency_cycle_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            (project / "modules" / "other.json").write_text(
                "{}\n",
                encoding="utf-8",
            )
            manifest = self.read_project(project)
            manifest["modules"][0]["depends_on"] = ["other"]
            manifest["modules"].append(
                {
                    "id": "other",
                    "kind": "xcp.behavior.v1",
                    "path": "modules/other.json",
                    "depends_on": ["main"],
                }
            )
            self.write_project(project, manifest)
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(project)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.module_dependency_cycle",
            )
            self.assertIn("main", raised.exception.details.actual)
            self.assertIn("other", raised.exception.details.actual)

    def test_symlinked_runtime_file_is_rejected(self) -> None:
        module = self.sample / "modules" / "main.json"
        module_key = os.path.normcase(os.path.abspath(module))

        def is_symlink(path: pathlib.Path) -> bool:
            return os.path.normcase(os.path.abspath(path)) == module_key

        with mock.patch.object(
            pathlib.Path,
            "is_symlink",
            autospec=True,
            side_effect=is_symlink,
        ):
            with self.assertRaises(CreativeProjectError) as raised:
                validate_project(self.sample)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.symlink_rejected",
            )

    def test_tampered_bundle_fails_without_rewriting_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "bundle"
            build_bundle(self.sample, output)
            manifest_before = (output / BUNDLE_FILENAME).read_bytes()
            (output / "modules" / "main.json").write_text(
                '{"tampered":true}\n',
                encoding="utf-8",
            )
            with self.assertRaises(CreativeProjectError) as raised:
                verify_bundle(output)
            self.assertIn(
                raised.exception.code,
                {
                    "xcp.creative.file_size_mismatch",
                    "xcp.creative.file_hash_mismatch",
                },
            )
            self.assertEqual(
                (output / BUNDLE_FILENAME).read_bytes(),
                manifest_before,
            )

    def test_undeclared_bundle_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "bundle"
            build_bundle(self.sample, output)
            (output / "undeclared.json").write_text(
                "{}\n",
                encoding="utf-8",
            )
            with self.assertRaises(CreativeProjectError) as raised:
                verify_bundle(output)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.bundle_file_undeclared",
            )
            self.assertEqual(
                raised.exception.details.path,
                "undeclared.json",
            )

    def test_existing_output_and_output_inside_source_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            project = self.copy_sample(root)
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaises(CreativeProjectError) as raised:
                build_bundle(project, existing)
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.output_exists",
            )

            with self.assertRaises(CreativeProjectError) as raised:
                build_bundle(project, project / "build" / "bundle")
            self.assertEqual(
                raised.exception.code,
                "xcp.creative.output_inside_project",
            )


if __name__ == "__main__":
    unittest.main()
