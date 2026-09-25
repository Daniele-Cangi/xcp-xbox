#include "pch.h"
#include "WorkerRuntimeDescription.h"

#include "WorkerCpuCapsuleRuntime.h"
#include "WorkerCpuGpuConvergenceBackend.h"
#include "WorkerGraphAdmission.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphRuntime.h"
#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmSpmdPhaseBProfile.h"
#include "WorkerGpuXvmSpmdGeneralizedProfile.h"
#include "WorkerMacroCatalog.h"
#include "WorkerResultEnvelope.h"
#include "WorkerProtocolBoundary.h"
#include "WorkerProcessTopologyRuntime.h"
#include "WorkerContentAddressedStoreRecovery.h"
#include "WorkerPersistentComputeCoordinatorRuntime.h"
#include "WorkerProvableWorldsStreamingRuntime.h"
#include "WorkerStorageScaleCharacterizationRuntime.h"
#include "WorkerSessionRuntime.h"
#include "WorkerTaskPlanRuntime.h"
#include "WorkerVerifiedProductionMode.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmCpuCapsuleBackend.h"
#include "WorkerXvmRuntime.h"
#include "WorkerXvmSnapshot.h"
#include "WorkerXvmSpmdRuntime.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::ApplicationModel;

namespace XComputeProbe
{
    namespace
    {
        std::wstring JsonStringArray(std::vector<std::wstring> const& values)
        {
            std::wostringstream out;
            out << L"[";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                {
                    out << L",";
                }
                out << JsonString(values[i]);
            }
            out << L"]";
            return out.str();
        }

        std::wstring PackageVersionString()
        {
            try
            {
                auto version = Package::Current().Id().Version();
                std::wostringstream out;
                out << version.Major << L"." << version.Minor << L"." << version.Build << L"." << version.Revision;
                return out.str();
            }
            catch (...)
            {
                return L"unknown";
            }
        }

        std::wstring PackageNameString()
        {
            try
            {
                return std::wstring(Package::Current().Id().Name().c_str());
            }
            catch (...)
            {
                return L"unknown";
            }
        }

        wchar_t const* BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }
    }

    std::wstring WorkerBuildRuntimeDescription(WorkerRuntimeDescriptionInput const& input)
    {
        auto const& limits = input.limits;
        auto const& state = input.state;
        auto supportedCommands = WorkerSupportedCommandIds();
        auto const& gpuGeneralV1 = WorkerGpuXvmGeneralV1Profile();
        auto const& gpuResidentCoreV1 = WorkerGpuXvmResidentCoreV1Profile();
        auto const& gpuFullIsaV2Slice = WorkerGpuXvmFullIsaV2TypedStructuredProfile();
        auto const& gpuFullIsaV2LoopControlSlice = WorkerGpuXvmFullIsaV2LoopControlProfile();
        auto const& gpuFullIsaV2FullCallReturnSlice = WorkerGpuXvmFullIsaV2FullCallReturnProfile();
        auto const& gpuFullIsaV2RecoveredTrapSlice = WorkerGpuXvmFullIsaV2RecoveredTrapProfile();
        auto const& gpuFullIsaV2LegacyBranchSlice = WorkerGpuXvmFullIsaV2LegacyBranchProfile();
        auto const& gpuSpmdPhaseB = WorkerGpuXvmSpmdPhaseBProfile();
        auto const& gpuSpmdGeneralized = WorkerGpuXvmSpmdGeneralizedProfile();
        auto const& gpuMicrotraceV1 = WorkerGpuXvmMicrotraceV1Profile();
        auto const& gpuMicrotracePhaseB = WorkerGpuXvmMicrotracePhaseBV1Profile();

        std::wostringstream out;
        out << L"{\"ok\":true,\"protocol_version\":" << JsonString(input.protocolVersion)
            << L",\"command\":\"describe_runtime\""
            << L",\"schema_version\":\"worker-runtime-description-0.1\""
            << L",\"package\":{\"name\":" << JsonString(PackageNameString())
            << L",\"version\":" << JsonString(PackageVersionString())
            << L",\"manifest_flavor\":\"worker-prototype\""
            << L",\"capability_profile\":\"public_uwp_zero_restricted_capabilities\"}"
            << L",\"transport\":{\"port\":" << input.port
            << L",\"max_request_bytes\":" << limits.maxRequestBytes
            << L",\"binary_payload_read_buffer_bytes\":" << limits.binaryPayloadReadBufferBytes
            << L",\"request_read_buffer_bytes\":" << limits.requestReadBufferBytes << L"}"
            << L",\"process_topology\":" << WorkerProcessTopologyCapabilityJson()
            << L",\"persistent_compute_coordinator\":" << WorkerPersistentComputeCoordinatorCapabilityJson()
            << L",\"content_addressed_store_recovery\":" << WorkerContentAddressedStoreRecoveryCapabilityJson()
            << L",\"provable_worlds_streaming_tiled_v1\":" << WorkerProvableWorldsStreamingCapabilityJson()
            << L",\"storage_scale_characterization_v1\":" << WorkerStorageScaleCharacterizationCapabilityJson()
            << L",\"creative_host\":{\"status\":\"DEVELOPMENT_CANDIDATE_NOT_LIVE_ADMITTED\""
            << L",\"description_command\":\"describe_creative_host\""
            << L",\"profile_schema\":\"xcp-creative-host-profile-v1\""
            << L",\"content_install_admitted\":false"
            << L",\"foreground_launch_admitted\":false}"
            << L",\"trusted_sessions\":{\"supported\":true"
            << L",\"default_ttl_seconds\":" << limits.defaultSessionTtlSeconds
            << L",\"min_ttl_seconds\":" << limits.minSessionTtlSeconds
            << L",\"max_ttl_seconds\":" << limits.maxSessionTtlSeconds
            << L",\"max_trusted_controllers\":" << limits.maxTrustedControllers
            << L",\"auth_material_returned\":false}"
            << L",\"protocol_boundary\":{\"schema_version\":" << JsonString(WorkerProtocolBoundary::SchemaVersion())
            << L",\"session_schema_version\":" << JsonString(WorkerSessionRuntime::SchemaVersion())
            << L",\"negotiation_schema_version\":" << JsonString(WorkerProtocolBoundary::NegotiationSchemaVersion())
            << L",\"sdk_compatibility_schema_version\":" << JsonString(WorkerProtocolBoundary::SdkCompatibilitySchemaVersion())
            << L",\"session_protocol_boundary_v1\":true"
            << L",\"session_state_owned\":true"
            << L",\"authorization_owned\":true"
            << L",\"request_dispatch_owned\":true"
            << L",\"error_translation_owned\":true"
            << L",\"stable_error_envelope\":true"
            << L",\"batch_error_isolation\":true"
            << L",\"binary_authorization_shared\":true"
            << L",\"session_close_idempotent\":true"
            << L",\"worker_protocol_min\":" << JsonString(input.protocolVersion)
            << L",\"worker_protocol_max\":" << JsonString(input.protocolVersion)
            << L",\"sdk_compatibility\":\"declared_range\""
            << L",\"negotiate_command\":\"negotiate_protocol\"}"
            << L",\"artifact_limits\":{\"max_json_chunk_bytes\":" << limits.maxArtifactJsonChunkBytes
            << L",\"max_manifest_json_bytes\":" << limits.maxArtifactManifestJsonBytes
            << L",\"manifest_job_read_buffer_bytes\":" << limits.artifactManifestJobReadBufferBytes
            << L",\"max_stream_read_bytes\":" << limits.maxStreamReadBytes
            << L",\"max_stream_write_bytes\":" << limits.maxStreamWriteBytes
            << L",\"max_manifest_parts\":" << limits.maxArtifactManifestParts
            << L",\"max_expected_bytes\":" << limits.maxArtifactExpectedBytes
            << L",\"default_rollback_headroom_bytes\":" << limits.defaultArtifactRollbackHeadroomBytes
            << L",\"default_workspace_budget_bytes\":" << limits.defaultArtifactWorkspaceBudgetBytes
            << L",\"max_workspace_budget_bytes\":" << limits.maxArtifactWorkspaceBudgetBytes << L"}"
            << L",\"graph_limits\":{\"task_plan_supported\":true"
            << L",\"task_plan_runtime_split_v1\":true"
            << L",\"task_plan_schema\":" << JsonString(WorkerTaskPlanRuntime::SchemaVersion())
            << L",\"task_plan_run_control_schema\":" << JsonString(WorkerTaskPlanRuntime::RunControlSchemaVersion())
            << L",\"task_plan_run_cancel_supported\":true"
            << L",\"task_plan_run_cancel_idempotent\":true"
            << L",\"submit_graph_supported\":true"
            << L",\"full_graph_pre_admission\":true"
            << L",\"execution_plan_schema\":" << JsonString(WorkerGraphExecutionPlanSchemaVersion)
            << L",\"submit_graph_job_supported\":true"
            << L",\"submit_macro_supported\":true"
            << L",\"worker_side_macro_selection\":true"
            << L",\"worker_macro_catalog_schema\":\"worker-on-device-graph-macro-catalog-0.1\""
            << L",\"worker_macro_result_schema\":\"worker-on-device-graph-macro-result-0.1\""
            << L",\"worker_macro_catalog\":" << JsonStringArray(WorkerMacroCatalogIds())
            << L",\"max_task_plan_steps\":" << WorkerMaxTaskPlanSteps()
            << L",\"max_on_device_graph_nodes\":" << WorkerMaxOnDeviceGraphNodes()
            << L",\"max_on_device_graph_edges\":" << WorkerMaxOnDeviceGraphEdges()
            << L",\"on_device_graph_edge_binding\":true"
            << L",\"on_device_graph_dataflow_binding\":true"
            << L",\"on_device_graph_canonical_fanin_binding\":true"
            << L",\"on_device_graph_reduce_node\":true"
            << L",\"on_device_graph_reduce_kernels\":" << JsonStringArray(WorkerGraphReduceKernelIds())
            << L",\"on_device_graph_control_select_node\":true"
            << L",\"on_device_graph_control_kernels\":" << JsonStringArray(WorkerGraphControlKernelIds())
            << L",\"on_device_graph_static_multipass_pingpong\":true"
            << L",\"on_device_graph_static_multipass_profiles\":" << JsonStringArray(WorkerGraphStaticMultipassProfileIds())
            << L",\"on_device_graph_bounded_loop\":true"
            << L",\"on_device_graph_bounded_loop_profiles\":" << JsonStringArray(WorkerGraphBoundedLoopProfileIds())
            << L",\"on_device_graph_bounded_loop_max_iterations\":" << WorkerGraphMaxBoundedLoopIterations()
            << L",\"on_device_graph_static_neighbor_read\":true"
            << L",\"on_device_graph_static_neighbor_read_profiles\":" << JsonStringArray(WorkerGraphStaticNeighborReadProfileIds())
            << L",\"on_device_graph_static_neighbor_read_max_neighbors\":" << WorkerGraphMaxStaticNeighborReadEdges()
            << L",\"on_device_graph_expansion\":true"
            << L",\"on_device_graph_expansion_profiles\":" << JsonStringArray(WorkerGraphExpansionProfileIds())
            << L",\"on_device_graph_expansion_max_nodes\":" << WorkerGraphMaxExpansionNodes()
            << L",\"on_device_graph_input_binding_modes\":" << JsonStringArray(WorkerGraphInputBindingModes())
            << L",\"typed_artifact_edges\":true"
            << L",\"typed_control_edges\":true"
            << L",\"xvm_program_nodes\":true"
            << L",\"xvm_cpu_gpu_differential_backend\":true"
            << L",\"gpu_spmd_precompiled_nodes\":true"
            << L",\"gpu_spmd_precompiled_backend\":\"d3d12_precompiled_shader_shape\""
            << L",\"gpu_spmd_precompiled_kernel\":\"matrix_block_fp32_v1\""
            << L",\"shared_result_envelope_schema\":" << JsonString(WorkerResultEnvelopeSchemaVersion())
            << L",\"graph_resource_ledger_schema\":\"worker-graph-resource-ledger-0.1\""
            << L",\"on_device_graph_result_artifact\":true"
            << L",\"on_device_graph_canonical_result_hashing\":true"
            << L",\"on_device_graph_canonical_result_schema\":\"worker-on-device-graph-canonical-result-0.1\""
            << L",\"on_device_graph_checkpoint_recovery\":true"
            << L",\"on_device_graph_checkpoint_schema\":\"worker-graph-checkpoint-log-0.1\""
            << L",\"on_device_graph_async_cancellation\":true"
            << L",\"xvm_in_node_cancellation_snapshot\":true"
            << L",\"xvm_later_job_state_resume\":true"
            << L",\"on_device_graph_evidence_bundle\":true"
            << L",\"on_device_graph_evidence_bundle_schema\":" << JsonString(WorkerGraphEvidenceBundleSchemaVersion())
            << L",\"on_device_graph_evidence_seal_schema\":" << JsonString(WorkerGraphEvidenceSealSchemaVersion())
            << L",\"max_async_jobs\":" << limits.maxAsyncJobs
            << L",\"async_job_concurrency\":1"
            << L",\"on_device_graph_executor\":\"IMPLEMENTED_M130_GRAPH_EVIDENCE_BUNDLE_PUBLISH\"}"
            << L",\"xvm\":{\"status\":\"XVM_V1_AND_XVM_V2_MEASURED\""
            << L",\"submission_profile_command\":\"describe_submission_profile\""
            << L",\"isa_definition_command\":\"describe_xvm_isa\""
            << L",\"xvm_error_details_schema\":\"xvm-error-details-v1\""
            << L",\"xvm_error_catalog_schema\":\"xvm-error-catalog-v1\""
            << L",\"isa_version\":" << JsonString(WorkerXvmIsaVersion())
            << L",\"latest_isa_version\":" << JsonString(WorkerXvmLatestIsaVersion())
            << L",\"isa_versions\":[\"xvm-v1\",\"xvm-v2\"]"
            << L",\"isa_catalog\":" << WorkerXvmIsaCatalogJson()
            << L",\"program_artifact_schema\":" << JsonString(WorkerXvmProgramArtifactSchemaVersion())
            << L",\"program_artifact_schemas\":[\"xvm-program-artifact-v1\",\"xvm-program-artifact-v2\"]"
            << L",\"profiles\":[\"integer-deterministic-v1\",\"integer-deterministic-v2\"]"
            << L",\"backend\":\"cpu_reference\""
            << L",\"backends\":[\"cpu_reference\",\"cpu_plan_codelet_differential\",\"cpu_packaged_capsule_hot_kernel_differential\",\"cpu_spmd_reference\",\"cpu_gpu_differential\",\"cpu_gpu_spmd_differential\"]"
            << L",\"gpu_differential_shader\":\"XvmIntegerNucleus.cso\""
            << L",\"gpu_differential_compute_kind\":\"xvm_integer_deterministic_cpu_gpu_differential_v1\""
            << L",\"gpu_differential_live_measured\":true"
            << L",\"gpu_profile_selection_field\":\"gpu_profile_id\""
            << L",\"gpu_profile_selection\":\"explicit_for_general_profiles\""
            << L",\"gpu_profile_contracts\":" << WorkerGpuXvmProfileContractsJson()
            << L",\"gpu_profile_contract_status\":\"PROVABLE_WORLDS_V1_IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_profile_contract_required_next_target\":\"PROVABLE_WORLDS_V1_XBOX_LIVE_DIFFERENTIAL\""
            << L",\"gpu_differential_max_instructions\":" << WorkerGpuXvmMaxInstructionsValue
            << L",\"gpu_differential_max_memory_bytes\":" << WorkerGpuXvmMemoryBytesValue
            << L",\"gpu_differential_max_output_bytes\":" << WorkerGpuXvmOutputBytesValue
            << L",\"gpu_differential_max_fuel\":" << WorkerGpuXvmFuelLimitValue
            << L",\"gpu_general_v1_phase_a\":true"
            << L",\"gpu_general_v1_phase_a_status\":\"IMPLEMENTED_LOCAL_AND_LIVE_MEASURED\""
            << L",\"gpu_general_v1_profile_id\":" << JsonString(gpuGeneralV1.profileId)
            << L",\"gpu_general_v1_compute_kind\":" << JsonString(gpuGeneralV1.computeKind)
            << L",\"gpu_general_v1_shader\":" << JsonString(gpuGeneralV1.shaderName)
            << L",\"gpu_general_v1_contract_schema\":" << JsonString(gpuGeneralV1.schemaVersion)
            << L",\"gpu_general_v1_contract_id\":" << JsonString(gpuGeneralV1.contractId)
            << L",\"gpu_general_v1_contract_sha256\":" << JsonString(gpuGeneralV1.contractSha256)
            << L",\"gpu_general_v1_max_instructions\":" << gpuGeneralV1.maxInstructions
            << L",\"gpu_general_v1_max_memory_bytes\":" << gpuGeneralV1.memoryBytes
            << L",\"gpu_general_v1_max_output_bytes\":" << gpuGeneralV1.outputBytes
            << L",\"gpu_general_v1_max_fuel\":" << gpuGeneralV1.fuelLimit
            << L",\"gpu_general_v1_live_measured\":" << (gpuGeneralV1.liveMeasured ? L"true" : L"false")
            << L",\"gpu_resident_execution_core_v1\":true"
            << L",\"gpu_resident_execution_core_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_resident_execution_core_profile_id\":" << JsonString(gpuResidentCoreV1.profileId)
            << L",\"gpu_resident_execution_core_compute_kind\":" << JsonString(gpuResidentCoreV1.computeKind)
            << L",\"gpu_resident_execution_core_shader\":" << JsonString(gpuResidentCoreV1.shaderName)
            << L",\"gpu_resident_execution_core_contract_schema\":" << JsonString(gpuResidentCoreV1.schemaVersion)
            << L",\"gpu_resident_execution_core_contract_id\":" << JsonString(gpuResidentCoreV1.contractId)
            << L",\"gpu_resident_execution_core_contract_sha256\":" << JsonString(gpuResidentCoreV1.contractSha256)
            << L",\"gpu_resident_execution_core_epoch_instruction_budget\":" << gpuResidentCoreV1.epochInstructionBudget
            << L",\"gpu_resident_execution_core_max_epochs\":" << gpuResidentCoreV1.maxEpochs
            << L",\"gpu_resident_execution_core_live_measured\":false"
            << L",\"gpu_resident_execution_core_package_version\":\"0.1.124.0\""
            << L",\"gpu_resident_execution_core_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_resident_execution_core_external_live_measured\":true"
            << L",\"gpu_resident_execution_core_runtime_role\":\"preserved_fallback\""
            << L",\"gpu_full_isa_v2_typed_structured_slice\":true"
            << L",\"gpu_full_isa_v2_typed_structured_slice_status\":\"LIVE_MEASURED_BASELINE\""
            << L",\"gpu_full_isa_v2_typed_structured_slice_profile_id\":" << JsonString(gpuFullIsaV2Slice.profileId)
            << L",\"gpu_full_isa_v2_typed_structured_slice_compute_kind\":" << JsonString(gpuFullIsaV2Slice.computeKind)
            << L",\"gpu_full_isa_v2_typed_structured_slice_shader\":" << JsonString(gpuFullIsaV2Slice.shaderName)
            << L",\"gpu_full_isa_v2_typed_structured_slice_contract_schema\":" << JsonString(gpuFullIsaV2Slice.schemaVersion)
            << L",\"gpu_full_isa_v2_typed_structured_slice_contract_id\":" << JsonString(gpuFullIsaV2Slice.contractId)
            << L",\"gpu_full_isa_v2_typed_structured_slice_contract_sha256\":" << JsonString(gpuFullIsaV2Slice.contractSha256)
            << L",\"gpu_full_isa_v2_typed_structured_slice_lane_count\":1"
            << L",\"gpu_full_isa_v2_typed_structured_slice_many_lane_admitted\":false"
            << L",\"gpu_full_isa_v2_typed_structured_slice_leaf_call_depth\":1"
            << L",\"gpu_full_isa_v2_typed_structured_slice_epochs_from_static_fuel\":true"
            << L",\"gpu_full_isa_v2_typed_structured_slice_live_measured\":" << BoolJson(gpuFullIsaV2Slice.liveMeasured)
            << L",\"gpu_full_isa_v2_typed_structured_slice_package_version\":\"0.1.125.0\""
            << L",\"gpu_full_isa_v2_typed_structured_slice_packaged_status\":\"IMPLEMENTED_LOCAL_AND_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_typed_structured_slice_external_live_measured\":true"
            << L",\"gpu_full_isa_v2_typed_structured_slice_runtime_role\":\"preserved_regression\""
            << L",\"gpu_full_isa_v2_loop_control_slice\":true"
            << L",\"gpu_full_isa_v2_loop_control_slice_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_loop_control_slice_profile_id\":" << JsonString(gpuFullIsaV2LoopControlSlice.profileId)
            << L",\"gpu_full_isa_v2_loop_control_slice_compute_kind\":" << JsonString(gpuFullIsaV2LoopControlSlice.computeKind)
            << L",\"gpu_full_isa_v2_loop_control_slice_shader\":" << JsonString(gpuFullIsaV2LoopControlSlice.shaderName)
            << L",\"gpu_full_isa_v2_loop_control_slice_contract_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.schemaVersion)
            << L",\"gpu_full_isa_v2_loop_control_slice_contract_id\":" << JsonString(gpuFullIsaV2LoopControlSlice.contractId)
            << L",\"gpu_full_isa_v2_loop_control_slice_contract_sha256\":" << JsonString(gpuFullIsaV2LoopControlSlice.contractSha256)
            << L",\"gpu_full_isa_v2_loop_control_slice_lane_count\":1"
            << L",\"gpu_full_isa_v2_loop_control_slice_many_lane_admitted\":false"
            << L",\"gpu_full_isa_v2_loop_control_slice_max_loop_depth\":" << gpuFullIsaV2LoopControlSlice.maxLoopDepth
            << L",\"gpu_full_isa_v2_loop_control_slice_execution_plan_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.executionPlanSchemaVersion)
            << L",\"gpu_full_isa_v2_loop_control_slice_state_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.stateSchemaVersion)
            << L",\"gpu_full_isa_v2_loop_control_slice_epochs_from_static_fuel\":true"
            << L",\"gpu_full_isa_v2_loop_control_slice_live_measured\":false"
            << L",\"gpu_full_isa_v2_loop_control_slice_package_version\":\"0.1.126.0\""
            << L",\"gpu_full_isa_v2_loop_control_slice_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_loop_control_slice_external_live_measured\":true"
            << L",\"gpu_full_isa_v2_loop_control_slice_runtime_role\":\"preserved_regression\""
            << L",\"gpu_full_isa_v2_loop_control_slice_next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2_FULL_CALL_RETURN_SLICE_V1\""
            << L",\"gpu_full_isa_v2_full_call_return_slice\":true"
            << L",\"gpu_full_isa_v2_full_call_return_slice_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_full_call_return_slice_profile_id\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.profileId)
            << L",\"gpu_full_isa_v2_full_call_return_slice_compute_kind\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.computeKind)
            << L",\"gpu_full_isa_v2_full_call_return_slice_shader\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.shaderName)
            << L",\"gpu_full_isa_v2_full_call_return_slice_contract_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.schemaVersion)
            << L",\"gpu_full_isa_v2_full_call_return_slice_contract_id\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.contractId)
            << L",\"gpu_full_isa_v2_full_call_return_slice_contract_sha256\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.contractSha256)
            << L",\"gpu_full_isa_v2_full_call_return_slice_package_version\":\"0.1.127.0\""
            << L",\"gpu_full_isa_v2_full_call_return_slice_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_full_call_return_slice_external_live_measured\":true"
            << L",\"gpu_full_isa_v2_full_call_return_slice_runtime_role\":\"preserved_regression\""
            << L",\"gpu_full_isa_v2_full_call_return_slice_lane_count\":1"
            << L",\"gpu_full_isa_v2_full_call_return_slice_many_lane_admitted\":false"
            << L",\"gpu_full_isa_v2_full_call_return_slice_max_call_depth\":" << gpuFullIsaV2FullCallReturnSlice.maxCallDepth
            << L",\"gpu_full_isa_v2_full_call_return_slice_max_loop_depth\":" << gpuFullIsaV2FullCallReturnSlice.maxLoopDepth
            << L",\"gpu_full_isa_v2_full_call_return_slice_execution_plan_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.executionPlanSchemaVersion)
            << L",\"gpu_full_isa_v2_full_call_return_slice_state_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.stateSchemaVersion)
            << L",\"gpu_full_isa_v2_full_call_return_slice_epochs_from_static_fuel\":true"
            << L",\"gpu_full_isa_v2_full_call_return_slice_runtime_shader_compilation\":false"
            << L",\"gpu_full_isa_v2_full_call_return_slice_full_isa_complete\":false"
            << L",\"gpu_full_isa_v2_full_call_return_slice_production_gpu_authority\":false"
            << L",\"gpu_full_isa_v2_full_call_return_slice_next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2_RECOVERED_TRAP_SLICE_V1\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice\":true"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice_profile_id\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.profileId)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_compute_kind\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.computeKind)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_shader\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.shaderName)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_contract_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.schemaVersion)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_contract_id\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.contractId)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_contract_sha256\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.contractSha256)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_package_version\":\"0.1.129.0\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice_external_live_measured\":true"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_runtime_role\":\"latest_external_live\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice_lane_count\":1"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_many_lane_admitted\":false"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_max_call_depth\":" << gpuFullIsaV2RecoveredTrapSlice.maxCallDepth
            << L",\"gpu_full_isa_v2_recovered_trap_slice_max_loop_depth\":" << gpuFullIsaV2RecoveredTrapSlice.maxLoopDepth
            << L",\"gpu_full_isa_v2_recovered_trap_slice_execution_plan_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.executionPlanSchemaVersion)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_state_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.stateSchemaVersion)
            << L",\"gpu_full_isa_v2_recovered_trap_slice_trap_state_schema\":\"xvm-trap-state-v1\""
            << L",\"gpu_full_isa_v2_recovered_trap_slice_epochs_from_static_fuel\":true"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_runtime_shader_compilation\":false"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_full_isa_complete\":false"
            << L",\"gpu_full_isa_v2_recovered_trap_slice_production_gpu_authority\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice\":true"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_legacy_branch_slice_profile_id\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.profileId)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_compute_kind\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.computeKind)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_shader\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.shaderName)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_contract_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.schemaVersion)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_contract_id\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.contractId)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_contract_sha256\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.contractSha256)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_package_version\":\"0.1.131.0\""
            << L",\"gpu_full_isa_v2_legacy_branch_slice_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"gpu_full_isa_v2_legacy_branch_slice_external_live_measured\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_runtime_role\":\"active_local\""
            << L",\"gpu_full_isa_v2_legacy_branch_slice_lane_count\":1"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_many_lane_admitted\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_max_call_depth\":" << gpuFullIsaV2LegacyBranchSlice.maxCallDepth
            << L",\"gpu_full_isa_v2_legacy_branch_slice_max_loop_depth\":" << gpuFullIsaV2LegacyBranchSlice.maxLoopDepth
            << L",\"gpu_full_isa_v2_legacy_branch_slice_execution_plan_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.executionPlanSchemaVersion)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_state_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.stateSchemaVersion)
            << L",\"gpu_full_isa_v2_legacy_branch_slice_structured_control_mixing_forbidden\":true"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_branch_topology_hashed\":true"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_runtime_shader_compilation\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_full_isa_complete\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_production_gpu_authority\":false"
            << L",\"gpu_full_isa_v2_legacy_branch_slice_next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2_COMPLETE_CPU_GPU_XBOX_MATRIX_V1\""
            << L",\"gpu_runtime\":{\"schema_version\":\"gpu-xvm-runtime-status-v1\""
            << L",\"resident_measured_baseline\":{\"active_target\":\"GPU_XVM_RESIDENT_EXECUTION_CORE_V1\",\"next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2\"}"
            << L",\"active_target\":\"GPU_XVM_FULL_ISA_V2\""
            << L",\"active_slice\":\"GPU_XVM_FULL_ISA_V2_LEGACY_BRANCH_SLICE_V1\""
            << L",\"active_package_version\":\"0.1.131.0\""
            << L",\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"external_live_measured\":false"
            << L",\"latest_external_live_slice\":\"GPU_XVM_FULL_ISA_V2_RECOVERED_TRAP_SLICE_V1\""
            << L",\"latest_external_live_package_version\":\"0.1.129.0\""
            << L",\"latest_external_runtime\":{\"active_slice\":\"GPU_XVM_FULL_ISA_V2_RECOVERED_TRAP_SLICE_V1\",\"package_version\":\"0.1.129.0\",\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\",\"external_live_measured\":true,\"next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2_LEGACY_BRANCH_SLICE_V1\"}"
            << L",\"preserved_regression_slice\":\"GPU_XVM_FULL_ISA_V2_TYPED_STRUCTURED_SLICE_V1\""
            << L",\"preserved_regression_package_version\":\"0.1.125.0\""
            << L",\"preserved_fallback_slice\":\"GPU_XVM_RESIDENT_EXECUTION_CORE_V1\""
            << L",\"preserved_fallback_package_version\":\"0.1.124.0\""
            << L",\"implementation_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"available\":true,\"live_measured\":false,\"production_authorized\":false"
            << L",\"cpu_reference\":{\"available\":true,\"canonical_authority\":true}"
            << L",\"cpu_gpu_differential\":{\"available\":true,\"canonical_authority\":\"cpu_reference\"}"
            << L",\"gpu_verified\":{\"available\":false,\"separate_gate_required\":true}"
            << L",\"next_after_live_gate\":\"GPU_XVM_FULL_ISA_V2_COMPLETE_CPU_GPU_XBOX_MATRIX_V1\"}"
            << L",\"gpu_generalized_backend_complete\":false"
            << L",\"automatic_gpu_backend_selection\":false"
            << L",\"xvm_v2_gpu_execution\":true"
            << L",\"xvm_v2_gpu_execution_scope\":\"typed_structured_loop_control_full_call_return_recovered_trap_plus_legacy_branch_compatibility_single_lane_slices_v1\""
            << L",\"xvm_v2_gpu_full_isa_complete\":false"
            << L",\"xvm_gpu_spmd_many_lane_planned\":true"
            << L",\"xvm_gpu_spmd_many_lane_admitted\":true"
            << L",\"xvm_gpu_spmd_many_lane_generalized\":true"
            << L",\"xvm_spmd_many_lane_phase_a_cpu_canonical_admitted\":true"
            << L",\"xvm_spmd_many_lane_phase_a_package_version\":\"0.1.130.0\""
            << L",\"xvm_spmd_many_lane_phase_a_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"xvm_spmd_many_lane_phase_b_gpu_differential_admitted\":true"
            << L",\"xvm_spmd_many_lane_phase_b_package_version\":\"0.1.133.0\""
            << L",\"xvm_spmd_many_lane_phase_b_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"xvm_spmd_many_lane_phase_b_profile_id\":" << JsonString(gpuSpmdPhaseB.profileId)
            << L",\"xvm_spmd_many_lane_phase_b_contract_id\":" << JsonString(gpuSpmdPhaseB.contractId)
            << L",\"xvm_spmd_many_lane_phase_b_contract_sha256\":" << JsonString(gpuSpmdPhaseB.contractSha256)
            << L",\"xvm_spmd_many_lane_phase_b_shader\":" << JsonString(gpuSpmdPhaseB.shaderName)
            << L",\"xvm_spmd_many_lane_phase_b_shader_sha256\":" << JsonString(gpuSpmdPhaseB.shaderSha256)
            << L",\"xvm_spmd_many_lane_phase_b_execution_plan_schema\":" << JsonString(gpuSpmdPhaseB.executionPlanSchemaVersion)
            << L",\"xvm_spmd_many_lane_phase_b_state_schema\":" << JsonString(gpuSpmdPhaseB.stateSchemaVersion)
            << L",\"xvm_spmd_many_lane_phase_b_logical_lane_count\":" << gpuSpmdPhaseB.laneCount
            << L",\"xvm_spmd_many_lane_phase_b_grid_shape\":[4,3,1]"
            << L",\"xvm_spmd_many_lane_phase_b_workgroup_shape\":[1,1,1]"
            << L",\"xvm_spmd_many_lane_phase_b_runtime_shader_compilation\":false"
            << L",\"xvm_spmd_many_lane_generalized_admitted\":true"
            << L",\"xvm_spmd_many_lane_generalized_package_version\":\"0.1.137.0\""
            << L",\"xvm_spmd_many_lane_generalized_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"xvm_spmd_many_lane_generalized_profile_id\":" << JsonString(gpuSpmdGeneralized.profileId)
            << L",\"xvm_spmd_many_lane_generalized_contract_id\":" << JsonString(gpuSpmdGeneralized.contractId)
            << L",\"xvm_spmd_many_lane_generalized_contract_sha256\":" << JsonString(gpuSpmdGeneralized.contractSha256)
            << L",\"xvm_spmd_many_lane_generalized_shader\":" << JsonString(gpuSpmdGeneralized.shaderName)
            << L",\"xvm_spmd_many_lane_generalized_shader_sha256\":" << JsonString(gpuSpmdGeneralized.shaderSha256)
            << L",\"xvm_spmd_many_lane_generalized_execution_plan_schema\":" << JsonString(gpuSpmdGeneralized.executionPlanSchemaVersion)
            << L",\"xvm_spmd_many_lane_generalized_state_schema\":" << JsonString(gpuSpmdGeneralized.stateSchemaVersion)
            << L",\"xvm_spmd_many_lane_generalized_max_logical_lanes\":" << gpuSpmdGeneralized.laneCount
            << L",\"xvm_spmd_many_lane_generalized_workgroup_shape\":[8,8,1]"
            << L",\"xvm_spmd_many_lane_generalized_partial_group_guard\":true"
            << L",\"xvm_spmd_many_lane_generalized_runtime_shader_compilation\":false"
            << L",\"provable_worlds_v1_admitted\":true"
            << L",\"provable_worlds_v1_package_version\":\"0.1.140.0\""
            << L",\"provable_worlds_v1_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"provable_worlds_v1_profile_id\":\"provable_worlds_v1_1024x1024_u32\""
            << L",\"provable_worlds_v1_contract_id\":\"PROVABLE_WORLDS_V1\""
            << L",\"provable_worlds_v1_contract_sha256\":\"8587a2fc3a26782861cce2c621cbccaafe78ff249f502da9c4759ebb9d9d89b6\""
            << L",\"provable_worlds_v1_shader\":\"XvmProvableWorldsV1.cso\""
            << L",\"provable_worlds_v1_shader_sha256\":\"835a745b098711b20c5fbf9d6b32f55ff68623e786e2854b052f86de6e08579d\""
            << L",\"provable_worlds_v1_shader_source_sha256\":\"e2e63f692a65b1942746a139f2778bb2b2ae40ede6a32f4b07552f9a239e8af2\""
            << L",\"provable_worlds_v1_execution_plan_schema\":\"gpu-xvm-execution-plan-v11\""
            << L",\"provable_worlds_v1_spmd_plan_schema\":\"xvm-spmd-execution-plan-v3\""
            << L",\"provable_worlds_v1_grid_shape\":[1024,1024,1]"
            << L",\"provable_worlds_v1_logical_lane_count\":1048576"
            << L",\"provable_worlds_v1_workgroup_shape\":[8,8,1]"
            << L",\"provable_worlds_v1_dispatch_group_shape\":[128,128,1]"
            << L",\"provable_worlds_v1_single_logical_dispatch\":true"
            << L",\"provable_worlds_v1_field_format\":\"u32_le\""
            << L",\"provable_worlds_v1_field_bytes\":4194304"
            << L",\"provable_worlds_v1_assurance_class\":\"exact_cpu_gpu_differential_v1\""
            << L",\"provable_worlds_v1_artifact_lifecycle\":[\"generated_provisional\",\"verification_pending\",\"verified\",\"quarantined\"]"
            << L",\"provable_worlds_v1_cpu_reference_canonical\":true"
            << L",\"provable_worlds_v1_cpu_only_backend_admitted\":true"
            << L",\"provable_worlds_v1_cpu_only_artifact_publication\":false"
            << L",\"provable_worlds_v1_runtime_shader_compilation\":false"
            << L",\"provable_worlds_v1_gpu_production_authority\":false"
            << L",\"cpu_gpu_evolved_backend_convergence_v1_admitted\":true"
            << L",\"cpu_gpu_evolved_backend_convergence_v1_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"cpu_gpu_evolved_backend_convergence_v1_worker_package_version\":\"0.1.145.0\""
            << L",\"cpu_gpu_evolved_backend_convergence_v1_capsule_version\":\"1.3.0.0\""
            << L",\"cpu_gpu_evolved_backend_convergence_v1_contract_id\":" << JsonString(WorkerCpuGpuConvergenceContractId)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_profile_id\":" << JsonString(WorkerCpuGpuConvergenceProfileId)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_profile_contract_sha256\":" << JsonString(WorkerCpuGpuConvergenceProfileContractSha256)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_request_schema\":" << JsonString(WorkerCpuGpuConvergenceRequestSchemaVersion)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_plan_schema\":" << JsonString(WorkerCpuGpuConvergencePlanSchemaVersion)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_result_schema\":" << JsonString(WorkerCpuGpuConvergenceResultSchemaVersion)
            << L",\"cpu_gpu_evolved_backend_convergence_v1_three_way_exact\":true"
            << L",\"cpu_gpu_evolved_backend_convergence_v1_automatic_selection\":false"
            << L",\"cpu_gpu_evolved_backend_convergence_v1_production_authority\":false"
            << L",\"gpu_xvm_verified_production_mode_v1_admitted\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_status\":\"IMPLEMENTED_LOCAL_PENDING_XBOX\""
            << L",\"gpu_xvm_verified_production_mode_v1_worker_package_version\":\"0.1.146.0\""
            << L",\"gpu_xvm_verified_production_mode_v1_capsule_version\":\"1.3.0.0\""
            << L",\"gpu_xvm_verified_production_mode_v1_contract_id\":" << JsonString(WorkerVerifiedProductionModeContractId)
            << L",\"gpu_xvm_verified_production_mode_v1_policy_id\":" << JsonString(WorkerVerifiedProductionModePolicyId)
            << L",\"gpu_xvm_verified_production_mode_v1_policy_sha256\":" << JsonString(WorkerVerifiedProductionModePolicySha256)
            << L",\"gpu_xvm_verified_production_mode_v1_request_schema\":" << JsonString(WorkerVerifiedProductionModeRequestSchemaVersion)
            << L",\"gpu_xvm_verified_production_mode_v1_plan_schema\":" << JsonString(WorkerVerifiedProductionModePlanSchemaVersion)
            << L",\"gpu_xvm_verified_production_mode_v1_result_schema\":" << JsonString(WorkerVerifiedProductionModeResultSchemaVersion)
            << L",\"gpu_xvm_verified_production_mode_v1_graph_plan_schema\":" << JsonString(WorkerGraphExecutionPlanSchemaVersion)
            << L",\"gpu_xvm_verified_production_mode_v1_selection_policy\":" << JsonString(WorkerVerifiedProductionModeSelectionPolicy)
            << L",\"gpu_xvm_verified_production_mode_v1_automatic_selection\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_per_result_production_authority\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_cpu_reference_oracle\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_gpu_requires_three_way_exact\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_capsule_fallback_requires_cpu_exact\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_gpu_quarantined\":" << BoolJson(WorkerVerifiedProductionModeGpuQuarantined())
            << L",\"gpu_xvm_verified_production_mode_v1_device_loss_error\":\"xvm.gpu_device_lost\""
            << L",\"gpu_xvm_verified_production_mode_v1_controlled_fault_claims_actual_removal\":false"
            << L",\"gpu_xvm_verified_production_mode_v1_checkpoint_boundary\":\"after_completed_node_only\""
            << L",\"gpu_xvm_verified_production_mode_v1_cancellation_idempotent\":true"
            << L",\"gpu_xvm_verified_production_mode_v1_artifact_lifecycle\":[\"generated_provisional\",\"verification_pending\",\"verified_authoritative\",\"quarantined\"]"
            << L",\"gpu_xvm_verified_production_mode_v1_runtime_native_codegen\":false"
            << L",\"gpu_xvm_verified_production_mode_v1_runtime_shader_compilation\":false"
            << L",\"xvm_gpu_microtrace_v1_admitted\":true"
            << L",\"xvm_gpu_microtrace_v1_phase\":\"IDENTITY_SOURCE_INSTRUCTION_V1\""
            << L",\"xvm_gpu_microtrace_v1_package_version\":\"0.1.135.0\""
            << L",\"xvm_gpu_microtrace_v1_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"xvm_gpu_microtrace_v1_profile_id\":" << JsonString(gpuMicrotraceV1.profileId)
            << L",\"xvm_gpu_microtrace_v1_contract_id\":" << JsonString(gpuMicrotraceV1.contractId)
            << L",\"xvm_gpu_microtrace_v1_contract_sha256\":" << JsonString(gpuMicrotraceV1.contractSha256)
            << L",\"xvm_gpu_microtrace_v1_shader\":" << JsonString(gpuMicrotraceV1.shaderName)
            << L",\"xvm_gpu_microtrace_v1_shader_sha256\":" << JsonString(gpuMicrotraceV1.shaderSha256)
            << L",\"xvm_gpu_microtrace_v1_execution_plan_schema\":" << JsonString(gpuMicrotraceV1.executionPlanSchemaVersion)
            << L",\"xvm_gpu_microtrace_v1_record_schema\":\"xvm-gpu-microtrace-record-v1\""
            << L",\"xvm_gpu_microtrace_v1_words_per_op\":" << gpuMicrotraceV1.microtraceWordsPerOp
            << L",\"xvm_gpu_microtrace_v1_max_ops\":" << gpuMicrotraceV1.maxMicrotraceOps
            << L",\"xvm_gpu_microtrace_v1_source_pc_map_required\":true"
            << L",\"xvm_gpu_microtrace_v1_worker_revalidation_required\":true"
            << L",\"xvm_gpu_microtrace_v1_resident_shader_consumes_data\":true"
            << L",\"xvm_gpu_microtrace_v1_runtime_shader_compilation\":false"
            << L",\"xvm_gpu_microtrace_v1_emits_executable_code\":false"
            << L",\"xvm_gpu_microtrace_v1_gpu_authority\":false"
            << L",\"xvm_gpu_microtrace_phase_b_admitted\":true"
            << L",\"xvm_gpu_microtrace_phase_b_phase\":\"OPTIMIZED_SUPERINSTRUCTION_V1\""
            << L",\"xvm_gpu_microtrace_phase_b_package_version\":\"0.1.136.0\""
            << L",\"xvm_gpu_microtrace_phase_b_packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            << L",\"xvm_gpu_microtrace_phase_b_profile_id\":" << JsonString(gpuMicrotracePhaseB.profileId)
            << L",\"xvm_gpu_microtrace_phase_b_contract_id\":" << JsonString(gpuMicrotracePhaseB.contractId)
            << L",\"xvm_gpu_microtrace_phase_b_contract_sha256\":" << JsonString(gpuMicrotracePhaseB.contractSha256)
            << L",\"xvm_gpu_microtrace_phase_b_shader\":" << JsonString(gpuMicrotracePhaseB.shaderName)
            << L",\"xvm_gpu_microtrace_phase_b_shader_sha256\":" << JsonString(gpuMicrotracePhaseB.shaderSha256)
            << L",\"xvm_gpu_microtrace_phase_b_execution_plan_schema\":" << JsonString(gpuMicrotracePhaseB.executionPlanSchemaVersion)
            << L",\"xvm_gpu_microtrace_phase_b_record_schema\":\"xvm-gpu-microtrace-record-v2\""
            << L",\"xvm_gpu_microtrace_phase_b_words_per_record\":" << gpuMicrotracePhaseB.microtraceWordsPerOp
            << L",\"xvm_gpu_microtrace_phase_b_max_records\":" << gpuMicrotracePhaseB.maxMicrotraceOps
            << L",\"xvm_gpu_microtrace_phase_b_max_source_span_per_record\":2"
            << L",\"xvm_gpu_microtrace_phase_b_constant_folding\":true"
            << L",\"xvm_gpu_microtrace_phase_b_alu_sequence_fusion\":true"
            << L",\"xvm_gpu_microtrace_phase_b_basic_block_superblock_planning\":true"
            << L",\"xvm_gpu_microtrace_phase_b_source_pc_map_required\":true"
            << L",\"xvm_gpu_microtrace_phase_b_worker_revalidation_required\":true"
            << L",\"xvm_gpu_microtrace_phase_b_exact_source_instruction_fuel\":true"
            << L",\"xvm_gpu_microtrace_phase_b_resident_shader_consumes_data\":true"
            << L",\"xvm_gpu_microtrace_phase_b_runtime_shader_compilation\":false"
            << L",\"xvm_gpu_microtrace_phase_b_emits_executable_code\":false"
            << L",\"xvm_gpu_microtrace_phase_b_gpu_authority\":false"
            << L",\"xvm_spmd_many_lane_contract_id\":" << JsonString(WorkerXvmSpmdContractId)
            << L",\"xvm_spmd_many_lane_execution_plan_schema\":" << JsonString(WorkerXvmSpmdExecutionPlanSchemaVersion)
            << L",\"xvm_spmd_many_lane_context_schema\":" << JsonString(WorkerXvmSpmdLaneContextSchemaVersion)
            << L",\"xvm_spmd_many_lane_backend\":\"cpu_spmd_reference\""
            << L",\"xvm_spmd_many_lane_backends\":[\"cpu_spmd_reference\",\"cpu_gpu_spmd_differential\"]"
            << L",\"xvm_spmd_many_lane_max_logical_lanes\":" << WorkerXvmSpmdMaxLogicalLanesValue
            << L",\"xvm_spmd_many_lane_shared_input_read_only\":true"
            << L",\"xvm_spmd_many_lane_private_state_per_lane\":true"
            << L",\"xvm_spmd_many_lane_disjoint_output_slices\":true"
            << L",\"xvm_spmd_many_lane_aggregate_static_fuel_required\":true"
            << L",\"xvm_spmd_many_lane_gpu_execution_admitted\":true"
            << L",\"xvm_spmd_many_lane_gpu_authority\":false"
            << L",\"max_program_bytes\":" << WorkerXvmMaxProgramBytes()
            << L",\"max_instructions\":" << WorkerXvmMaxInstructions()
            << L",\"max_loop_iterations\":" << WorkerXvmMaxLoopIterations()
            << L",\"max_loop_depth\":" << WorkerXvmMaxLoopDepth()
            << L",\"max_call_depth\":" << WorkerXvmMaxCallDepth()
            << L",\"max_graph_fuel\":" << WorkerGraphMaxFuel()
            << L",\"static_worst_case_fuel_supported\":true"
            << L",\"static_fuel_proof_schema\":" << JsonString(WorkerXvmStaticFuelProofSchemaVersion)
            << L",\"static_fuel_algorithm\":\"structured_cfg_longest_path_v1\""
            << L",\"static_fuel_node_admission_required\":true"
            << L",\"max_graph_memory_bytes\":" << WorkerGraphMaxXvmMemoryBytes()
            << L",\"max_graph_output_bytes\":" << WorkerGraphMaxXvmOutputBytes()
            << L",\"state_snapshot_schema\":" << JsonString(WorkerXvmStateSnapshotSchemaVersion())
            << L",\"latest_state_snapshot_schema\":" << JsonString(WorkerXvmLatestStateSnapshotSchemaVersion())
            << L",\"state_snapshot_schemas\":[\"xvm-state-snapshot-v2\",\"xvm-state-snapshot-v3\",\"xvm-state-snapshot-v4\",\"xvm-state-snapshot-v5\"]"
            << L",\"state_snapshot_policy_schema\":\"xvm-state-snapshot-policy-v2\""
            << L",\"state_resume_policy_schema\":\"xvm-state-resume-policy-v2\""
            << L",\"state_snapshot_seal_schema\":" << JsonString(WorkerXvmSnapshotSealSchemaVersion())
            << L",\"state_snapshot_provenance_fields\":[\"execution_id\",\"checkpoint_sequence\",\"previous_state_sha256\",\"seal_key_id\",\"worker_seal_sha256\"]"
            << L",\"state_resume_authorization_fields\":[\"snapshot_artifact_id\",\"expected_snapshot_sha256\",\"expected_execution_id\",\"expected_checkpoint_sequence\",\"expected_previous_state_sha256\",\"expected_bound_input_sha256\"]"
            << L",\"state_resume_full_graph_pre_admission_verified\":true"
            << L",\"state_snapshot_authority\":\"worker_local_persistent_hmac_sha256\""
            << L",\"state_snapshot_authority_private_from_workspace\":true"
            << L",\"state_snapshot_v1_resume_admitted\":false"
            << L",\"in_node_cancellation_snapshot\":true"
            << L",\"later_job_state_resume\":true"
            << L",\"typed_memory_v1_implemented_local\":true"
            << L",\"typed_memory_v1_element_types\":[\"u32\"]"
            << L",\"typed_memory_v1_raw_host_pointers\":false"
            << L",\"typed_memory_v1_host_calls\":false"
              << L",\"structured_control_v2_implemented_local\":true"
              << L",\"structured_control_v2_max_depth\":8"
              << L",\"structured_control_v2_snapshot_stack\":false"
              << L",\"structured_control_v2_legacy_branch_mix\":false"
              << L",\"structured_control_v2_phase_b_status\":\"IMPLEMENTED_CPU_REFERENCE\""
              << L",\"structured_control_v2_phase_b_evidence_status\":\"MEASURED_ON_XBOX\""
              << L",\"structured_control_v2_phase_b_capability\":\"structured_control_v2_phase_b\""
              << L",\"structured_control_v2_phase_b_requires\":[\"structured_control_v2\"]"
              << L",\"structured_control_v2_phase_b_opcodes\":[\"less_than_u32\",\"less_than_s32\",\"select_u32\",\"break_if_zero\",\"continue_if_zero\",\"trap_if_zero\"]"
              << L",\"structured_control_v2_phase_b_loop_control\":\"innermost_verified_loop_only\""
              << L",\"structured_control_v2_phase_b_trap_state_schema\":\"xvm-trap-state-v1\""
              << L",\"structured_control_v2_phase_b_snapshot_schema\":\"xvm-state-snapshot-v3\""
              << L",\"structured_control_v2_phase_b_gpu_ordered_compare_select_slice\":true"
              << L",\"structured_control_v2_phase_b_gpu_loop_control_slice\":true"
              << L",\"structured_control_v2_phase_b_gpu_trap\":true"
              << L",\"structured_control_v2_phase_b_gpu_complete\":false"
              << L",\"structured_control_v2_phase_b_full_cpu_reference_only\":false"
            << L",\"runtime_shader_compilation\":false"
            << L",\"host_calls\":false}"
            << L",\"kernel_catalog\":{\"m109\":["
            << L"{\"kernel_id\":\"hash_reduce_v1\",\"surface\":\"run_artifact_manifest_compute_job\",\"compute_kind\":\"logical_sha256_reduce_v1\",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"algorithmic_differentiation\":\"MEASURED_ON_XBOX_M123\"},"
            << L"{\"kernel_id\":\"stream_mix_v1\",\"surface\":\"run_artifact_manifest_compute_job\",\"compute_kind\":\"stream_byte_mix_v1\",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"algorithmic_differentiation\":\"MEASURED_ON_XBOX_M123\"},"
            << L"{\"kernel_id\":\"tiled_scan_u8_v1\",\"surface\":\"run_artifact_manifest_compute_job\",\"compute_kind\":\"tiled_scan_u8_v1\",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"algorithmic_differentiation\":\"MEASURED_ON_XBOX_M123\"},"
            << L"{\"kernel_id\":\"vector_transform_fp32_v1\",\"surface\":\"run_d3d12_fp32_timing_job\",\"enforced_on_worker\":true,\"executable_on_xbox\":true},"
            << L"{\"kernel_id\":\"matrix_block_fp32_v1\",\"surface\":\"run_d3d12_shader_shape_job\",\"enforced_on_worker\":true,\"executable_on_xbox\":true},"
            << L"{\"kernel_id\":\"matrix_block_fp32_v1\",\"surface\":\"gpu_spmd_precompiled\",\"compute_kind\":\"d3d12_precompiled_gpu_spmd_v1\",\"enforced_on_worker\":true,\"executable_on_xbox\":true},"
            << L"{\"kernel_id\":\"xvm_integer_deterministic_v1\",\"surface\":\"xvm_program\",\"compute_kind\":\"xvm_integer_deterministic_cpu_gpu_differential_v1\",\"backend\":\"cpu_gpu_differential\",\"profile_id\":\"m150_seed_straight_line_v1\",\"profile_contract_schema\":\"gpu-xvm-profile-contract-v1\",\"profile_contract_sha256\":\"a9d9fc24d095474975d0476e37f50d749aec7e9efbbfb4c0b63e047cc3d3624c\",\"shader\":\"XvmIntegerNucleus.cso\",\"shader_sha256\":\"47e777ec074fc3dbbf74be907d913c5c558b0171cacd5b23ba37b06a0bf5634d\",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":true},"
            << L"{\"kernel_id\":\"xvm_integer_general_v1_phase_a\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuGeneralV1.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuGeneralV1.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuGeneralV1.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuGeneralV1.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuGeneralV1.contractSha256) << L",\"shader\":" << JsonString(gpuGeneralV1.shaderName) << L",\"shader_sha256\":" << JsonString(gpuGeneralV1.shaderSha256) << L",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":true},"
            << L"{\"kernel_id\":\"xvm_resident_execution_core_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuResidentCoreV1.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuResidentCoreV1.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuResidentCoreV1.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuResidentCoreV1.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuResidentCoreV1.contractSha256) << L",\"shader\":" << JsonString(gpuResidentCoreV1.shaderName) << L",\"shader_sha256\":" << JsonString(gpuResidentCoreV1.shaderSha256) << L",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_full_isa_v2_typed_structured_slice_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuFullIsaV2Slice.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuFullIsaV2Slice.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuFullIsaV2Slice.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuFullIsaV2Slice.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuFullIsaV2Slice.contractSha256) << L",\"shader\":" << JsonString(gpuFullIsaV2Slice.shaderName) << L",\"shader_sha256\":" << JsonString(gpuFullIsaV2Slice.shaderSha256) << L",\"lane_count\":1,\"many_lane_admitted\":false,\"full_isa_complete\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":" << BoolJson(gpuFullIsaV2Slice.liveMeasured) << L"},"
            << L"{\"kernel_id\":\"xvm_full_isa_v2_loop_control_slice_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuFullIsaV2LoopControlSlice.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuFullIsaV2LoopControlSlice.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuFullIsaV2LoopControlSlice.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuFullIsaV2LoopControlSlice.contractSha256) << L",\"shader\":" << JsonString(gpuFullIsaV2LoopControlSlice.shaderName) << L",\"shader_sha256\":" << JsonString(gpuFullIsaV2LoopControlSlice.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuFullIsaV2LoopControlSlice.stateSchemaVersion) << L",\"max_loop_depth\":" << gpuFullIsaV2LoopControlSlice.maxLoopDepth << L",\"lane_count\":1,\"many_lane_admitted\":false,\"full_isa_complete\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_full_isa_v2_full_call_return_slice_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.contractSha256) << L",\"shader\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.shaderName) << L",\"shader_sha256\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuFullIsaV2FullCallReturnSlice.stateSchemaVersion) << L",\"max_call_depth\":" << gpuFullIsaV2FullCallReturnSlice.maxCallDepth << L",\"max_loop_depth\":" << gpuFullIsaV2FullCallReturnSlice.maxLoopDepth << L",\"lane_count\":1,\"many_lane_admitted\":false,\"runtime_shader_compilation\":false,\"full_isa_complete\":false,\"production_gpu_authority\":false,\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\",\"external_live_measured\":true,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":true},"
            << L"{\"kernel_id\":\"xvm_full_isa_v2_recovered_trap_slice_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.contractSha256) << L",\"shader\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.shaderName) << L",\"shader_sha256\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuFullIsaV2RecoveredTrapSlice.stateSchemaVersion) << L",\"trap_state_schema\":\"xvm-trap-state-v1\",\"max_call_depth\":" << gpuFullIsaV2RecoveredTrapSlice.maxCallDepth << L",\"max_loop_depth\":" << gpuFullIsaV2RecoveredTrapSlice.maxLoopDepth << L",\"lane_count\":1,\"many_lane_admitted\":false,\"runtime_shader_compilation\":false,\"full_isa_complete\":false,\"production_gpu_authority\":false,\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\",\"external_live_measured\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_full_isa_v2_legacy_branch_slice_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.contractSha256) << L",\"shader\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.shaderName) << L",\"shader_sha256\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuFullIsaV2LegacyBranchSlice.stateSchemaVersion) << L",\"max_call_depth\":" << gpuFullIsaV2LegacyBranchSlice.maxCallDepth << L",\"max_loop_depth\":0,\"legacy_branch_topology_hashed\":true,\"structured_control_mixing_forbidden\":true,\"lane_count\":1,\"many_lane_admitted\":false,\"runtime_shader_compilation\":false,\"full_isa_complete\":false,\"production_gpu_authority\":false,\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\",\"external_live_measured\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_gpu_microtrace_v1_identity\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuMicrotraceV1.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuMicrotraceV1.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuMicrotraceV1.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuMicrotraceV1.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuMicrotraceV1.contractSha256) << L",\"shader\":" << JsonString(gpuMicrotraceV1.shaderName) << L",\"shader_sha256\":" << JsonString(gpuMicrotraceV1.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuMicrotraceV1.executionPlanSchemaVersion) << L",\"microtrace_record_schema\":\"xvm-gpu-microtrace-record-v1\",\"source_pc_map_required\":true,\"worker_revalidation_required\":true,\"runtime_shader_compilation\":false,\"emits_executable_code\":false,\"cpu_reference_canonical\":true,\"gpu_authority\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_gpu_microtrace_v1_phase_b_optimized\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuMicrotracePhaseB.computeKind) << L",\"backend\":\"cpu_gpu_differential\",\"profile_id\":" << JsonString(gpuMicrotracePhaseB.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuMicrotracePhaseB.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuMicrotracePhaseB.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuMicrotracePhaseB.contractSha256) << L",\"shader\":" << JsonString(gpuMicrotracePhaseB.shaderName) << L",\"shader_sha256\":" << JsonString(gpuMicrotracePhaseB.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuMicrotracePhaseB.executionPlanSchemaVersion) << L",\"microtrace_record_schema\":\"xvm-gpu-microtrace-record-v2\",\"max_source_span_per_record\":2,\"constant_folding\":true,\"alu_sequence_fusion\":true,\"basic_block_superblock_planning\":true,\"source_pc_map_required\":true,\"worker_revalidation_required\":true,\"runtime_shader_compilation\":false,\"emits_executable_code\":false,\"cpu_reference_canonical\":true,\"gpu_authority\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_spmd_many_lane_phase_b_4x3_u32_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuSpmdPhaseB.computeKind) << L",\"backend\":\"cpu_gpu_spmd_differential\",\"profile_id\":" << JsonString(gpuSpmdPhaseB.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuSpmdPhaseB.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuSpmdPhaseB.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuSpmdPhaseB.contractSha256) << L",\"shader\":" << JsonString(gpuSpmdPhaseB.shaderName) << L",\"shader_sha256\":" << JsonString(gpuSpmdPhaseB.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuSpmdPhaseB.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuSpmdPhaseB.stateSchemaVersion) << L",\"lane_count\":12,\"grid_shape\":[4,3,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":true,\"cpu_reference_canonical\":true,\"gpu_authority\":false,\"runtime_shader_compilation\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_spmd_many_lane_generalized_8x8_u32_v1\",\"surface\":\"xvm_program\",\"compute_kind\":" << JsonString(gpuSpmdGeneralized.computeKind) << L",\"backend\":\"cpu_gpu_spmd_differential\",\"profile_id\":" << JsonString(gpuSpmdGeneralized.profileId) << L",\"profile_contract_schema\":" << JsonString(gpuSpmdGeneralized.schemaVersion) << L",\"profile_contract_id\":" << JsonString(gpuSpmdGeneralized.contractId) << L",\"profile_contract_sha256\":" << JsonString(gpuSpmdGeneralized.contractSha256) << L",\"shader\":" << JsonString(gpuSpmdGeneralized.shaderName) << L",\"shader_sha256\":" << JsonString(gpuSpmdGeneralized.shaderSha256) << L",\"execution_plan_schema\":" << JsonString(gpuSpmdGeneralized.executionPlanSchemaVersion) << L",\"state_schema\":" << JsonString(gpuSpmdGeneralized.stateSchemaVersion) << L",\"minimum_lane_count\":2,\"maximum_lane_count\":" << gpuSpmdGeneralized.laneCount << L",\"workgroup_shape\":[8,8,1],\"dispatch_group_shape\":\"ceil_grid_div_workgroup\",\"partial_group_threads_guarded\":true,\"many_lane_admitted\":true,\"generalized_topology_admitted\":true,\"cpu_reference_canonical\":true,\"gpu_authority\":false,\"runtime_shader_compilation\":false,\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":false},"
            << L"{\"kernel_id\":\"xvm_integer_deterministic_v2\",\"surface\":\"xvm_program\",\"compute_kind\":\"xvm_integer_deterministic_v2\",\"backend\":\"cpu_reference\",\"enforced_on_worker\":true,\"executable_on_xbox\":true,\"live_measured\":true},"
            << L"{\"kernel_id\":\"composite_manifest_verdict_v1\",\"surface\":\"pc_runner_verdict\",\"enforced_on_worker\":false,\"executable_on_xbox\":false}"
            << L"],\"m118_physics\":["
            << L"{\"kernel_id\":\"particle_grid_advection_v1\",\"surface\":\"catalog_only\",\"enforced_on_worker\":true,\"executable_on_xbox\":false},"
            << L"{\"kernel_id\":\"bounded_nbody_integrator_v1\",\"surface\":\"catalog_only\",\"enforced_on_worker\":true,\"executable_on_xbox\":false},"
            << L"{\"kernel_id\":\"cellular_material_spread_v1\",\"surface\":\"run_physics_kernel_job\",\"enforced_on_worker\":true,\"executable_on_xbox\":true},"
            << L"{\"kernel_id\":\"collision_broadphase_hash_v1\",\"surface\":\"catalog_only\",\"enforced_on_worker\":true,\"executable_on_xbox\":false},"
            << L"{\"kernel_id\":\"rigid_body_toy_constraints_v1\",\"surface\":\"catalog_only\",\"enforced_on_worker\":true,\"executable_on_xbox\":false}"
            << L"]}"
            << L",\"physics_limits\":{\"max_width\":" << limits.maxPhysicsGridWidth
            << L",\"max_height\":" << limits.maxPhysicsGridHeight
            << L",\"max_cells\":" << limits.maxPhysicsCellCount
            << L",\"max_steps\":" << limits.maxPhysicsSteps
            << L",\"max_cell_updates\":" << limits.maxPhysicsCellUpdates << L"}"
            << L",\"d3d12\":{\"command_surface_supported\":true"
            << L",\"availability_requires_live_probe\":true"
            << L",\"device_probe_command\":\"run_d3d12_device_probe_job\""
            << L",\"runtime_shader_compilation\":false"
            << L",\"graph_gpu_spmd_precompiled_surface\":true"
            << L",\"max_fp32_timing_elements\":" << limits.maxD3D12Fp32TimingElements
            << L",\"max_shader_shape_elements\":" << limits.maxD3D12ShaderShapeElements
            << L",\"commands\":[\"run_d3d12_device_probe_job\",\"run_d3d12_compute_smoke_job\",\"run_d3d12_compute_sweep_job\",\"run_d3d12_compute_timing_job\",\"run_d3d12_fp32_timing_job\",\"run_d3d12_shader_shape_job\",\"submit_d3d12_shader_shape_job\",\"submit_d3d12_shader_shape_soak_job\"]}"
            << L",\"cpu_tiered_runtime\":{\"plan_id\":\"CPU_XVM_TIERED_RUNTIME_PLAN_V1\""
            << L",\"status\":\"GATE_1_AND_GATE_2_MEASURED_ON_XBOX_GATE_3_IMPLEMENTED_LOCAL_PENDING_XBOX\""
            << L",\"active_gate\":\"XVM_CPU_PACKAGED_CAPSULE_HOT_KERNEL_V1\""
            << L",\"package_graph_gate\":{\"gate_id\":" << JsonString(WorkerCpuCapsuleRuntime::GateId())
            << L",\"status\":\"MEASURED_ON_XBOX\""
            << L",\"decision\":\"PACKAGE_GRAPH_UPDATE_NOT_VISIBLE_AFTER_REACTIVATION\""
            << L",\"static_capsule_load_supported\":true"
            << L",\"decision_scope\":\"autonomous_static_rebind_without_main_package_update\""
            << L",\"autonomous_static_rebind_without_main_update_supported\":false"
            << L",\"worker_redeploy_static_rebind_status\":\"UNKNOWN_PENDING_GATE_3\""
            << L",\"dynamic_package_dependency_status\":\"UNKNOWN_NOT_PROBED\""
            << L",\"optional_related_set_status\":\"UNKNOWN_NOT_PROBED\""
            << L",\"command\":\"probe_cpu_capsule_package_graph\""
            << L",\"schema_version\":" << JsonString(WorkerCpuCapsuleRuntime::ProbeSchemaVersion())
            << L",\"dependency_model\":\"static_manifest_framework_package\""
            << L",\"dependency_package_name\":" << JsonString(WorkerCpuCapsuleRuntime::DependencyPackageName())
            << L",\"module_name\":" << JsonString(WorkerCpuCapsuleRuntime::ModuleName())
            << L",\"abi_version\":" << WorkerCpuCapsuleRuntime::AbiVersion()
            << L"}"
            << L",\"plan_codelet_differential\":{\"available\":true"
            << L",\"gate_id\":\"XVM_CPU_PLAN_CODELET_DIFFERENTIAL_V1\""
            << L",\"backend\":\"cpu_plan_codelet_differential\""
            << L",\"execution_plan_schema\":\"xvm-cpu-execution-plan-v1\""
            << L",\"graph_execution_plan_schema\":\"worker-on-device-graph-execution-plan-v5\""
            << L",\"codelet_catalog_id\":\"xvm-cpu-static-codelets-v1\""
            << L",\"snapshot_schema\":\"xvm-state-snapshot-v4\""
            << L",\"snapshot_plan_and_backend_bound\":true"
            << L",\"canonical_backend\":\"cpu_reference\""
            << L",\"exact_differential_required\":true"
            << L",\"quarantine_before_publication\":true"
            << L",\"fallback_backend\":\"cpu_reference\""
            << L",\"production_authority\":false"
            << L",\"runtime_native_codegen\":false"
            << L",\"client_native_payload_accepted\":false"
            << L",\"restricted_capability_required\":false}"
            << L",\"packaged_capsule_hot_kernel\":{\"available\":true"
            << L",\"gate_id\":\"XVM_CPU_PACKAGED_CAPSULE_HOT_KERNEL_V1\""
            << L",\"status\":\"IMPLEMENTED_LOCAL_PENDING_XBOX\""
            << L",\"backend\":" << JsonString(WorkerXvmCpuCapsuleBackendValue)
            << L",\"execution_plan_schema\":" << JsonString(WorkerXvmCpuCapsulePlanSchemaVersion)
            << L",\"graph_execution_plan_schema\":" << JsonString(WorkerGraphExecutionPlanSchemaVersion)
            << L",\"profile_id\":" << JsonString(WorkerXvmCpuCapsuleProfileId)
            << L",\"profile_contract_sha256\":" << JsonString(WorkerXvmCpuCapsuleProfileContractSha256)
            << L",\"minimum_capsule_version\":\"1.3.0.0\""
            << L",\"max_major_version_tested\":1"
            << L",\"worker_package_version\":\"0.1.146.0\""
            << L",\"snapshot_schema\":\"xvm-state-snapshot-v5\""
            << L",\"snapshot_plan_profile_module_bound\":true"
            << L",\"external_resume_admitted\":false"
            << L",\"internal_restore_deoptimization_implemented\":true"
            << L",\"internal_restore_deoptimization_xbox_verified\":false"
            << L",\"resolution_probe_command\":\"probe_cpu_capsule_resolution\""
            << L",\"resolution_probe_schema\":" << JsonString(WorkerCpuCapsuleResolutionProbeSchemaVersion())
            << L",\"resolution_probe_loads_module\":false"
            << L",\"quarantine_diagnostic_command\":\"test_cpu_capsule_quarantine_lifecycle\""
            << L",\"quarantine_diagnostic_development_only\":true"
            << L",\"stable_error_codes_exhaustive\":false"
            << L",\"stable_error_codes\":[\"cpu_capsule.loaded_module_path_unavailable\",\"cpu_capsule.loaded_module_path_mismatch\",\"xvm.cpu_capsule_quarantined\",\"xvm.cpu_capsule_differential_mismatch\"]"
            << L",\"canonical_backend\":\"cpu_reference\""
            << L",\"exact_differential_required\":true"
            << L",\"quarantine_before_publication\":true"
            << L",\"fallback_backend\":\"cpu_reference\""
            << L",\"fallback_available\":true"
            << L",\"minimum_material_warm_speedup_ratio\":1.10"
            << L",\"production_authority\":false"
            << L",\"runtime_native_codegen\":false"
            << L",\"client_native_payload_accepted\":false"
            << L",\"restricted_capability_required\":false}}"
            << L",\"enforcement_status\":{\"m109_kernel_id_enforcement\":\"MEASURED_ON_XBOX_M120\""
            << L",\"m109_manifest_kernel_differentiation\":\"MEASURED_ON_XBOX_M123\""
            << L",\"on_device_graph_executor\":\"IMPLEMENTED_M130_GRAPH_EVIDENCE_BUNDLE_PUBLISH\""
            << L",\"full_graph_pre_admission\":\"MEASURED_ON_XBOX\""
            << L",\"xvm_snapshot_provenance_v2\":\"MEASURED_ON_XBOX\""
            << L",\"xvm_static_worst_case_fuel_v1\":\"MEASURED_ON_XBOX\""
            << L",\"on_device_graph_canonical_result_hashing\":\"IMPLEMENTED_M147\""
            << L",\"on_device_graph_checkpoint_recovery\":\"MEASURED_ON_XBOX_M150\""
            << L",\"graph_checkpoint_journal_v1\":\"MEASURED_ON_XBOX\""
            << L",\"on_device_graph_async_cancellation\":\"IMPLEMENTED_LOCAL_M150_PENDING_LIVE\""
            << L",\"on_device_graph_evidence_bundle\":\"IMPLEMENTED_M130\""
            << L",\"worker_side_macro_selection\":\"IMPLEMENTED_M135\""
            << L",\"physics_kernel_enforcement\":\"MEASURED_ON_XBOX_M121\""
            << L",\"artifact_path_boundary\":\"ENFORCED\""
            << L",\"trusted_session_boundary\":\"ENFORCED\""
            << L",\"expected_physics_digest_mismatch\":\"CONTROLLED_VERDICT\"}"
            << L",\"runtime_state\":{\"running\":" << BoolJson(state.running)
            << L",\"starting\":" << BoolJson(state.starting)
            << L",\"active_session_count\":" << state.activeSessionCount
            << L",\"active_async_job_count\":" << state.activeAsyncJobCount
            << L",\"queued_async_job_count\":" << state.queuedAsyncJobCount
            << L",\"async_job_running\":" << BoolJson(state.asyncJobRunning)
            << L",\"persisted_job_count\":" << state.persistedJobCount
            << L",\"task_plan_count\":" << state.taskPlanCount
            << L",\"active_task_plan_run_count\":" << state.activeTaskPlanRunCount
            << L",\"latest_async_job_id\":" << JsonString(state.latestAsyncJobId)
            << L",\"latest_async_job_status\":" << JsonString(state.latestAsyncJobStatus)
            << L",\"latest_task_plan_id\":" << JsonString(state.latestTaskPlanId)
            << L",\"latest_task_plan_run_status\":" << JsonString(state.latestTaskPlanRunStatus) << L"}"
            << L",\"claim_boundary\":{\"arbitrary_code\":false"
            << L",\"shell\":false"
            << L",\"executable_upload\":false"
            << L",\"runtime_shader_compilation\":false"
            << L",\"xvm_v2_gpu_full_isa_complete\":false"
            << L",\"xvm_gpu_spmd_many_lane_fixed_profile_admitted\":true"
            << L",\"xvm_gpu_spmd_many_lane_generalized\":true"
            << L",\"gpu_production_authority\":false"
            << L",\"codex_or_openai_token_on_xbox\":false"
            << L",\"gdk_or_game_mode\":false"
            << L",\"wired_lan_evidence\":false"
            << L",\"full_console_12_1_tflops\":false"
            << L",\"high_fidelity_physics\":false"
            << L",\"terabyte_scale_generation\":false"
            << L",\"multi_device_cluster\":false}"
            << L",\"supported_commands\":" << JsonStringArray(supportedCommands)
            << L"}";
        return out.str();
    }
}
