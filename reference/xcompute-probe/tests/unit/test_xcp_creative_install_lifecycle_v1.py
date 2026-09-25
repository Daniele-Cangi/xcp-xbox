import json
import pathlib
import unittest

from jsonschema import Draft202012Validator


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "src" / "XComputeProbe" / "runtime"


class XcpCreativeInstallLifecycleV1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.profile = json.loads(
            (
                ROOT
                / "profiles"
                / "creative"
                / "xcp-creative-host-development-v1.json"
            ).read_text(encoding="utf-8")
        )
        cls.profile_schema = json.loads(
            (
                ROOT
                / "schemas"
                / "xcp-creative-host-profile-v1.schema.json"
            ).read_text(encoding="utf-8")
        )
        cls.lifecycle = (
            RUNTIME / "WorkerCreativeInstallRuntime.cpp"
        ).read_text(encoding="utf-8")
        cls.lifecycle_header = (
            RUNTIME / "WorkerCreativeInstallRuntime.h"
        ).read_text(encoding="utf-8")
        cls.foreground = (
            RUNTIME / "WorkerCreativeForegroundRuntime.cpp"
        ).read_text(encoding="utf-8")
        cls.artifact_store = (
            RUNTIME / "WorkerArtifactStore.cpp"
        ).read_text(encoding="utf-8")
        cls.server = (
            RUNTIME / "WorkerCommandServer.cpp"
        ).read_text(encoding="utf-8")
        cls.protocol = (
            RUNTIME / "WorkerProtocolBoundary.cpp"
        ).read_text(encoding="utf-8")
        cls.graph = (
            RUNTIME / "WorkerGraphRuntime.cpp"
        ).read_text(encoding="utf-8")
        cls.project = (
            ROOT / "src" / "XComputeProbe" / "XComputeProbe.vcxproj"
        ).read_text(encoding="utf-8")

    def test_profile_publishes_admitted_bounded_development_lifecycle(
        self,
    ) -> None:
        Draft202012Validator(self.profile_schema).validate(self.profile)
        lifecycle = self.profile["lifecycle"]
        self.assertEqual(
            lifecycle["admission"],
            "development_candidate_not_live_admitted",
        )
        self.assertEqual(
            lifecycle["staging_surface"],
            "existing_worker_artifacts",
        )
        self.assertEqual(
            lifecycle["atomic_activation"],
            "append_only_hash_chained_record",
        )
        self.assertTrue(lifecycle["cas_reference_protection"])
        self.assertTrue(
            all(
                admitted is True
                for admitted in self.profile["operations"].values()
            )
        )
        self.assertTrue(
            all(
                item["admitted"] is True
                for item in self.profile["module_kinds"]
            )
        )

    def test_profile_binds_all_lifecycle_commands_and_request_versions(
        self,
    ) -> None:
        expected = {
            "prepare": (
                "prepare_creative_install",
                "xcp-creative-prepare-install-request-v1",
            ),
            "commit": (
                "commit_creative_install",
                "xcp-creative-commit-install-request-v1",
            ),
            "list": (
                "list_creative_installs",
                "xcp-creative-list-installs-request-v1",
            ),
            "activate": (
                "activate_creative_install",
                "xcp-creative-activate-install-request-v1",
            ),
            "rollback": (
                "rollback_creative_activation",
                "xcp-creative-rollback-activation-request-v1",
            ),
            "remove": (
                "remove_creative_install",
                "xcp-creative-remove-install-request-v1",
            ),
            "launch": (
                "launch_creative_project",
                "xcp-creative-launch-request-v1",
            ),
            "reload": (
                "reload_creative_project",
                "xcp-creative-reload-request-v1",
            ),
            "observe": (
                "observe_creative_foreground",
                "xcp-creative-observation-request-v1",
            ),
            "dispatch_input": (
                "dispatch_creative_input",
                "xcp-creative-input-dispatch-request-v1",
            ),
            "capture": (
                "capture_creative_frame",
                "xcp-creative-frame-capture-request-v1",
            ),
        }
        commands = self.profile["lifecycle"]["commands"]
        self.assertEqual(set(commands), set(expected))
        for key, (command, request_schema) in expected.items():
            self.assertEqual(commands[key]["command"], command)
            self.assertEqual(
                commands[key]["request_schema_version"],
                request_schema,
            )
            self.assertIn(f'command == L"{command}"', self.server)
            self.assertIn(f'L"{command}"', self.graph)
            self.assertIn(
                f'L"{request_schema}"',
                self.lifecycle + self.foreground,
            )

    def test_install_validation_is_exact_bounded_and_fail_closed(self) -> None:
        for token in (
            "xcp-creative-bundle-v1",
            "xcp-creative-installed-bundle-v1",
            "expected_host_profile_sha256",
            "WorkerCreativeHostProfileCanonicalSha256",
            "ResolveArtifactReadTarget",
            'L"xcp-creative-bundle-v1"',
            "CanonicalJsonObject",
            "ContentRecordsCanonical",
            'L",\\"sha256\\":" + JsonString(file.sha256)',
            "WorkerContentSha256File",
            "ValidateModuleDocument",
            "ValidateInstalledContentBlobs",
            "installed_content_missing",
            "installed_content_invalid",
            "MaxBundleManifestBytes",
            "MaxModuleDocumentBytes",
            "MaxProjectBytes",
            "MaxInstalledWorkspaceBytes",
            "ProjectedInstalledWorkspaceBytes",
            "project_version_conflict",
            "hostProfileCanonicalSha256",
            "expectedInstallId",
        ):
            self.assertIn(token, self.lifecycle)

    def test_activation_is_append_only_hash_chained_and_preconditioned(
        self,
    ) -> None:
        for token in (
            "xcp-creative-activation-record-v1",
            "previous_record_sha256",
            "ActivationRecordName",
            "WriteImmutableRecord",
            "expected_active_install_id",
            "expected_previous_install_id",
            "activation_state_conflict",
            "rollback_state_conflict",
            "transition_conflict",
            "FindTransition",
            "ValidateActivationReferences",
            "activation_reference_missing",
            "state.sequence >= MaxActivationRecords",
            "activeInstallId",
            "previousInstallId",
        ):
            self.assertIn(token, self.lifecycle)
        self.assertIn(
            "ActivationRecordName(uint64_t sequence)",
            self.lifecycle,
        )
        self.assertIn(
            "ActivationRecordName(state.sequence + 1)",
            self.lifecycle,
        )
        self.assertNotIn(
            "ActivationRecordName(\n"
            "            uint64_t sequence,\n"
            "            std::wstring const& transitionId)",
            self.lifecycle,
        )
        self.assertNotIn("replace_existing_activation", self.lifecycle)

    def test_creative_install_records_protect_shared_cas_blobs(self) -> None:
        for token in (
            'L"xcp-creative-v1"',
            'L"installs"',
            'L"cas_references"',
            "expectedReferences",
            "declaredReferences",
            "VisitCreativeInstallBlobReferencesLocal",
            "unreadable creative install metadata",
        ):
            self.assertIn(token, self.artifact_store)
        self.assertGreaterEqual(
            self.artifact_store.count(
                "VisitCreativeInstallBlobReferencesLocal"
            ),
            3,
        )
        self.assertIn(
            "WorkerRemoveUnreferencedContentAddressedBlobs",
            self.lifecycle,
        )
        self.assertIn("install_referenced", self.lifecycle)

    def test_errors_are_structured_and_native_files_are_packaged(self) -> None:
        self.assertIn(
            "WorkerCreativeHostErrorDetails",
            self.lifecycle_header,
        )
        self.assertIn(
            "xcp-creative-error-details-v1",
            self.protocol,
        )
        self.assertIn(
            "CreativeCodedErrorResponse",
            self.protocol,
        )
        self.assertIn(
            r'ClInclude Include="runtime\WorkerCreativeInstallRuntime.h"',
            self.project,
        )
        self.assertIn(
            r'ClCompile Include="runtime\WorkerCreativeInstallRuntime.cpp"',
            self.project,
        )


if __name__ == "__main__":
    unittest.main()
