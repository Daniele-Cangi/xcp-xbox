#include "pch.h"
#include "WorkerGraphRuntime.h"
#include "../ProbeResult.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr uint32_t MaxTaskPlanStepsValue = 64;
        constexpr uint32_t MaxOnDeviceGraphNodesValue = 8;
        constexpr uint32_t MaxOnDeviceGraphEdgesValue = 16;
        constexpr uint32_t MaxBoundedLoopIterationsValue = 4;
        constexpr uint32_t MaxStaticNeighborReadEdgesValue = 4;
        constexpr uint32_t MaxExpansionNodesValue = 2;
        constexpr wchar_t const* GraphEvidenceBundleSchemaVersionValue = L"worker-on-device-graph-evidence-bundle-0.1";
        constexpr wchar_t const* GraphEvidenceSealSchemaVersionValue = L"worker-on-device-graph-evidence-seal-0.1";
    }

    uint32_t WorkerMaxTaskPlanSteps()
    {
        return MaxTaskPlanStepsValue;
    }

    uint32_t WorkerMaxOnDeviceGraphNodes()
    {
        return MaxOnDeviceGraphNodesValue;
    }

    uint32_t WorkerMaxOnDeviceGraphEdges()
    {
        return MaxOnDeviceGraphEdgesValue;
    }

    uint32_t WorkerGraphMaxBoundedLoopIterations()
    {
        return MaxBoundedLoopIterationsValue;
    }

    uint32_t WorkerGraphMaxStaticNeighborReadEdges()
    {
        return MaxStaticNeighborReadEdgesValue;
    }

    uint32_t WorkerGraphMaxExpansionNodes()
    {
        return MaxExpansionNodesValue;
    }

    wchar_t const* WorkerGraphEvidenceBundleSchemaVersion()
    {
        return GraphEvidenceBundleSchemaVersionValue;
    }

    wchar_t const* WorkerGraphEvidenceSealSchemaVersion()
    {
        return GraphEvidenceSealSchemaVersionValue;
    }

    std::wstring WorkerGraphJsonArray(std::vector<std::wstring> const& jsonItems)
    {
        std::wstring json = L"[";
        for (size_t i = 0; i < jsonItems.size(); ++i)
        {
            if (i != 0)
            {
                json += L",";
            }
            json += jsonItems[i];
        }
        json += L"]";
        return json;
    }

    std::wstring WorkerGraphInputEdgeJson(
        WorkerGraphNodeRecord const& source,
        std::wstring const& role,
        std::wstring const& edgeType,
        std::wstring const& valueType)
    {
        return L"{\"from_node_id\":" + JsonString(source.nodeId) +
            L",\"role\":" + JsonString(role) +
            L",\"edge_type\":" + JsonString(edgeType) +
            L",\"value_type\":" + JsonString(valueType) +
            L",\"result_artifact_id\":" + JsonString(source.resultArtifactId) +
            L",\"result_sha256\":" + JsonString(source.resultSha256) +
            L",\"logical_sha256\":" + JsonString(source.logicalSha256) +
            L",\"compute_mix64\":" + JsonString(source.computeMix64) +
            L",\"control_token\":" + JsonString(source.controlToken) +
            L",\"source_ok\":" + std::wstring(source.ok ? L"true" : L"false") +
            L"}";
    }

    std::wstring WorkerGraphDeterministicInputEdgeJson(
        WorkerGraphNodeRecord const& source,
        std::wstring const& role,
        std::wstring const& edgeType,
        std::wstring const& valueType)
    {
        return L"{\"from_node_id\":" + JsonString(source.nodeId) +
            L",\"role\":" + JsonString(role) +
            L",\"edge_type\":" + JsonString(edgeType) +
            L",\"value_type\":" + JsonString(valueType) +
            L",\"source_kernel_id\":" + JsonString(source.kernelId) +
            L",\"logical_sha256\":" + JsonString(source.logicalSha256) +
            L",\"compute_mix64\":" + JsonString(source.computeMix64) +
            L",\"control_token\":" + JsonString(source.controlToken) +
            L",\"source_ok\":" + std::wstring(source.ok ? L"true" : L"false") +
            L"}";
    }

    std::vector<std::wstring> WorkerGraphReduceKernelIds()
    {
        return { L"graph_reduce_verdict_v1" };
    }

    std::vector<std::wstring> WorkerGraphControlKernelIds()
    {
        return { L"control_select_v1" };
    }

    std::vector<std::wstring> WorkerGraphStaticMultipassProfileIds()
    {
        return { L"static_ping_pong_v1" };
    }

    std::vector<std::wstring> WorkerGraphBoundedLoopProfileIds()
    {
        return { L"bounded_static_loop_v1" };
    }

    std::vector<std::wstring> WorkerGraphStaticNeighborReadProfileIds()
    {
        return { L"static_neighbor_read_v1" };
    }

    std::vector<std::wstring> WorkerGraphExpansionProfileIds()
    {
        return { L"bounded_static_fanin_expansion_v1" };
    }

    std::vector<std::wstring> WorkerGraphInputBindingModes()
    {
        return { L"mix_bound_input_edges_v1", L"typed_xvm_inputs_v1", L"control_select_v1", L"static_neighbor_read_v1" };
    }

    std::vector<std::wstring> WorkerSupportedCommandIds()
    {
        return {
            L"ping", L"negotiate_protocol", L"open_session", L"open_session_with_trust", L"session_status", L"close_session",
            L"register_trusted_controller", L"list_trusted_controllers", L"remove_trusted_controller",
            L"describe_runtime", L"describe_creative_host", L"prepare_creative_install", L"commit_creative_install",
            L"list_creative_installs", L"activate_creative_install", L"rollback_creative_activation", L"remove_creative_install",
            L"launch_creative_project", L"reload_creative_project", L"observe_creative_foreground", L"dispatch_creative_input", L"capture_creative_frame",
            L"describe_submission_profile", L"describe_xvm_isa",
            L"probe_process_topology", L"probe_persistent_compute_coordinator", L"probe_content_addressed_store_recovery", L"run_provable_worlds_streaming_tiled_v1", L"run_storage_scale_characterization_v1",
            L"probe_cpu_capsule_resolution", L"probe_cpu_capsule_package_graph", L"test_cpu_capsule_quarantine_lifecycle",
            L"stat", L"export_diagnostics", L"read_diagnostics", L"list_tree", L"hash_file",
            L"mkdir", L"delete", L"artifact_quota_preflight", L"begin_artifact_upload", L"append_artifact_chunk",
            L"commit_artifact_upload", L"generate_artifact_dataset", L"abort_artifact_upload", L"delete_artifact",
            L"reap_artifacts", L"get_artifact_status", L"run_artifact_manifest_job",
            L"run_artifact_manifest_compute_job", L"submit_graph", L"submit_graph_job", L"submit_macro", L"run_physics_kernel_job", L"publish_job_result_artifact",
            L"search", L"run_native_bench", L"run_hash_job", L"run_vector_job", L"run_d3d11_compute_job",
            L"run_d3d11_compute_sweep_job", L"run_d3d11_compute_timing_job", L"run_d3d11_fp32_timing_job",
            L"run_d3d11_shader_matrix_job", L"run_d3d11_shader_matrix_stats_job", L"run_d3d11_sustained_soak_job",
            L"run_d3d11_resident_hotloop_job", L"submit_d3d11_resident_hotloop_job", L"run_d3d12_device_probe_job",
            L"run_d3d12_compute_smoke_job", L"run_d3d12_compute_sweep_job", L"run_d3d12_compute_timing_job",
            L"run_d3d12_fp32_timing_job", L"run_d3d12_shader_shape_job", L"submit_d3d12_shader_shape_job",
            L"submit_d3d12_shader_shape_soak_job", L"run_interpreter_job", L"store_interpreter_program",
            L"run_stored_interpreter_job", L"list_interpreter_programs", L"delete_interpreter_program",
            L"run_memory_interpreter_job", L"run_memory_file_job", L"store_memory_program", L"list_memory_programs",
            L"run_stored_memory_file_job", L"delete_memory_program", L"submit_stored_memory_file_job",
            L"get_job_status", L"get_job_result", L"cancel_job", L"purge_job", L"list_jobs", L"create_task_plan",
            L"get_task_plan", L"resume_task_plan", L"advance_task_plan", L"submit_task_plan_run", L"cancel_task_plan_run",
            L"get_task_plan_status", L"get_task_plan_result", L"update_task_plan_step", L"delete_task_plan",
            L"list_task_plans", L"read", L"write", L"patch", L"read_chunk", L"write_chunk",
        };
    }
}
