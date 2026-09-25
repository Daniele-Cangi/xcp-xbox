"""PC schema and source-wiring checks for the frozen storage recovery path.

These checks do not run the UWP worker or claim native storage parity.
"""

import json
from pathlib import Path

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[1] / "reference/xcompute-probe"
RUNTIME = ROOT / "src/XComputeProbe/runtime"


def test_recovery_request_and_result_schemas_are_closed() -> None:
    schemas = ROOT / "schemas"
    request = json.loads((schemas / "xcp-content-addressed-store-recovery-v1.runtime-request.schema.json").read_text())
    result = json.loads((schemas / "xcp-content-addressed-store-recovery-v1.runtime-result.schema.json").read_text())
    Draft202012Validator.check_schema(request)
    Draft202012Validator.check_schema(result)
    assert request["additionalProperties"] is False
    assert result["additionalProperties"] is False
    assert request["properties"]["operation"]["enum"] == [
        "core_matrix", "seed_restart", "recover_restart", "cleanup_restart"
    ]
    assert result["properties"]["delivery_train"]["const"] == "CONTENT_ADDRESSED_STORE_RECOVERY_V1"


def test_recovery_uses_shared_cas_and_fail_closed_reaping() -> None:
    recovery = (RUNTIME / "WorkerContentAddressedStoreRecovery.cpp").read_text(encoding="utf-8")
    store = (RUNTIME / "WorkerArtifactStore.cpp").read_text(encoding="utf-8")
    header = (RUNTIME / "WorkerArtifactStore.h").read_text(encoding="utf-8")
    for token in ("WorkerCommitContentAddressedBlob", "WorkerContentSha256File",
                  "ArtifactBlobPath", "WorkerRemoveUnreferencedContentAddressedBlob"):
        assert token in recovery
        assert token in store + header
    assert 'entry.path().parent_path().filename() != L"pages"' in store
    assert "Fail-safe: unreadable storage metadata blocks CAS reaping" in store
    assert 'L"second_artifact_system",\n            false' in recovery


def test_recovery_command_and_build_wiring_are_present() -> None:
    server = (RUNTIME / "WorkerCommandServer.cpp").read_text(encoding="utf-8")
    graph = (RUNTIME / "WorkerGraphRuntime.cpp").read_text(encoding="utf-8")
    description = (RUNTIME / "WorkerRuntimeDescription.cpp").read_text(encoding="utf-8")
    project = (ROOT / "src/XComputeProbe/XComputeProbe.vcxproj").read_text(encoding="utf-8")
    assert 'command == L"probe_content_addressed_store_recovery"' in server
    assert 'L"probe_content_addressed_store_recovery"' in graph
    assert "WorkerContentAddressedStoreRecoveryCapabilityJson" in description
    assert r'ClCompile Include="runtime\WorkerContentAddressedStoreRecovery.cpp"' in project


def test_journal_chain_and_root_publication_guards_are_present() -> None:
    recovery = (RUNTIME / "WorkerContentAddressedStoreRecovery.cpp").read_text(encoding="utf-8")
    for field in ("previous_record_sha256", "record_sha256", "content_sha256",
                  "committed_root_sha256", "manifest_sha256"):
        assert field in recovery
    for failure in ("JOURNAL_SEQUENCE_INVALID", "JOURNAL_CHAIN_INVALID",
                    "JOURNAL_RECORD_HASH_INVALID", "JOURNAL_RECORD_AFTER_ROOT",
                    "ROOT_MANIFEST_MISSING", "ROOT_MANIFEST_INCOMPLETE",
                    "ROOT_MANIFEST_HASH_INVALID"):
        assert failure in recovery
    assert recovery.index('record.recordType = L"ROOT_COMMIT"') < recovery.index(
        "snapshot.publicationEligible = true")
    assert "worker_final_publication_veto" in recovery


def test_recovery_fault_matrix_is_bounded_and_explicit() -> None:
    recovery = (RUNTIME / "WorkerContentAddressedStoreRecovery.cpp").read_text(encoding="utf-8")
    for fixture in ("truncated_record", "wrong_record_hash", "missing_commit",
                    "corrupted_page", "missing_page", "wrong_page_hash",
                    "crash_during_commit", "logical_restart", "eviction_during_recovery",
                    "incomplete_manifest", "quota_exhausted",
                    "duplicate_logical_page_identity", "stale_reservation", "orphan_temp",
                    "double_cleanup"):
        assert f'L"{fixture}"' in recovery
    for bound in ("MaximumProbeBytes", "MaximumJournalRecords", "LogicalPageBytes"):
        assert bound in recovery


def test_persistent_coordinator_wraps_recovery_in_a_reservation() -> None:
    coordinator = (RUNTIME / "WorkerPersistentComputeCoordinatorRuntime.cpp").read_text(encoding="utf-8")
    acquire = coordinator.index('L"reservation_acquire"', coordinator.index("storage.RunCoreMatrix") - 12000)
    operation = coordinator.index("storage.RunCoreMatrix")
    release = coordinator.index("releaseLease();", operation)
    assert acquire < operation < release
    for guard in ("ValidateReservationOperation", "release_idempotent",
                  "worker_reauthorization_verified", "broker_semantic"):
        assert guard in coordinator
