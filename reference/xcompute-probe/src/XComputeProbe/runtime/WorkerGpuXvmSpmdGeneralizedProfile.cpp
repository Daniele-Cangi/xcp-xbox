#include "pch.h"
#include "WorkerGpuXvmSpmdGeneralizedProfile.h"

#include <algorithm>
#include <array>

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion =
            L"gpu-xvm-spmd-many-lane-generalized-v1";
        constexpr wchar_t const* ContractSha256 =
            L"1944cc27bd458a4652ceb655102bd670b31782d14edf16c83d266f19ccc26e07";
        constexpr wchar_t const* ShaderName = L"XvmSpmdManyLaneGeneralized8x8V1.cso";
        constexpr wchar_t const* ShaderSha256 =
            L"071eeaa3588a90c2b17dee06399f95dd6b5ea4ebc12895f4882ecda518471ea5";
        constexpr wchar_t const* ShaderSourceSha256 =
            L"b6f212ea82daebd1a068a80e9d228cdf730610910283b63c28880e0bd869d3cc";

        constexpr std::array<uint32_t, 36> ExpectedProgramWords =
        {
            13, 12, 0, 0,
            13, 13, 4, 0,
            13, 14, 8, 0,
            13, 15, 12, 0,
            15, 7, 0, 0,
            14, 0, 0, 0,
            0, 0, 0, 0,
            1, 0, 1, 0,
            16, 0, 0, 0,
        };

        constexpr WorkerGpuXvmProfileDescriptor Profile
        {
            WorkerGpuXvmProfileKind::SpmdManyLaneGeneralized8x8U32V1,
            ContractSchemaVersion,
            WorkerGpuXvmSpmdGeneralizedContractId,
            ContractSha256,
            WorkerGpuXvmSpmdGeneralizedProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_spmd_many_lane_generalized_v1",
            9,
            64,
            16,
            16,
            16,
            65536,
            128,
            128,
            8,
            true,
            false,
            true,
            8,
            2,
            false,
            true,
            true,
            false,
            true,
            4096,
            { 4096, 4096, 4096 },
            { 8, 8, 1 },
            128,
            16,
            WorkerGpuXvmCapabilitySpmdLaneContextValue,
            1,
            L"gpu-xvm-execution-plan-v10",
            L"gpu-xvm-lane-state-v7",
            0,
            true,
            true,
            false,
            0,
            0,
            true,
            64,
        };

        bool HasExactCapabilities(std::vector<std::wstring> const& capabilities)
        {
            static constexpr std::array<wchar_t const*, 4> Required =
            {
                L"artifact_input",
                L"artifact_output",
                L"control_output",
                L"spmd_lane_context_v1",
            };
            return capabilities.size() == Required.size() &&
                std::all_of(Required.begin(), Required.end(), [&](wchar_t const* required)
                {
                    return std::find(capabilities.begin(), capabilities.end(), required) != capabilities.end();
                });
        }
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmSpmdGeneralizedProfile()
    {
        return Profile;
    }

    bool WorkerGpuXvmSpmdGeneralizedProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        if (profile.kind != WorkerGpuXvmProfileKind::SpmdManyLaneGeneralized8x8U32V1 ||
            profile.memoryBytes == 0 || limits.memoryBytes % profile.memoryBytes != 0)
        {
            return false;
        }
        auto laneCount = limits.memoryBytes / profile.memoryBytes;
        return program.schemaVersion == L"xvm-program-artifact-v2" &&
            program.isaVersion == L"xvm-v2" &&
            program.artifactKind == L"xvm-program-v2" &&
            program.profile == L"integer-deterministic-v2" &&
            program.programId == L"spmd_many_lane_context_generalized" &&
            HasExactCapabilities(program.capabilities) &&
            program.memoryBytes == profile.memoryBytes &&
            program.outputBytes == profile.outputBytes &&
            program.maxFuel == profile.fuelLimit &&
            program.staticWorstCaseFuel == 9 &&
            program.maxCallDepth == 1 &&
            program.functions.size() == 1 &&
            program.functions[0].entryPc == 7 &&
            program.functions[0].endPc == 8 &&
            program.words.size() == ExpectedProgramWords.size() &&
            std::equal(program.words.begin(), program.words.end(), ExpectedProgramWords.begin()) &&
            laneCount >= 2 && laneCount <= profile.laneCount &&
            limits.outputBytes == laneCount * profile.outputBytes &&
            limits.fuel >= laneCount * program.staticWorstCaseFuel &&
            limits.fuel <= laneCount * profile.fuelLimit;
    }

    std::wstring WorkerGpuXvmSpmdGeneralizedProfileContractJson()
    {
        return std::wstring(L"{\"schema_version\":\"gpu-xvm-spmd-many-lane-generalized-v1\"") +
            L",\"contract_id\":\"GPU_XVM_SPMD_MANY_LANE_V1_GENERALIZED_TOPOLOGY\"" +
            L",\"contract_sha256\":" + std::wstring(L"\"") + ContractSha256 + L"\"" +
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\"" +
            L",\"profile_id\":\"spmd_many_lane_v1_generalized_8x8_u32_v1\"" +
            L",\"backend\":\"cpu_gpu_spmd_differential\"" +
            L",\"cpu_reference_canonical\":true" +
            L",\"gpu_authority\":false" +
            L",\"runtime_shader_compilation\":false" +
            L",\"automatic_backend_selection\":false" +
            L",\"minimum_logical_lanes\":2" +
            L",\"maximum_logical_lanes\":4096" +
            L",\"grid_shape\":\"declared_nonzero_product_equals_lane_count\"" +
            L",\"workgroup_shape\":[8,8,1]" +
            L",\"dispatch_group_shape\":\"ceil_grid_div_workgroup\"" +
            L",\"partial_group_threads_guarded\":true" +
            L",\"shared_input\":\"read_only_replicated\"" +
            L",\"private_state_per_lane\":true" +
            L",\"output_ownership\":\"disjoint_contiguous_slice_per_lane\"" +
            L",\"execution_plan_schema\":\"gpu-xvm-execution-plan-v10\"" +
            L",\"state_schema\":\"gpu-xvm-lane-state-v7\"" +
            L",\"spmd_plan_schema\":\"xvm-spmd-execution-plan-v2\"" +
            L",\"shader\":" + std::wstring(L"\"") + ShaderName + L"\"" +
            L",\"shader_sha256\":" + std::wstring(L"\"") + ShaderSha256 + L"\"" +
            L",\"shader_source_sha256\":" + std::wstring(L"\"") + ShaderSourceSha256 + L"\"" +
            L",\"stable_errors\":{\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"revalidation_failed\":\"xvm.gpu_spmd_revalidation_failed\",\"state_invalid\":\"xvm.gpu_resident_state_invalid\",\"lockstep_invalid\":\"xvm.gpu_spmd_lockstep_invalid\",\"differential_mismatch\":\"xvm.gpu_spmd_differential_mismatch\"}" +
            L",\"protocol_compatibility\":{\"worker_protocol_min\":\"0.73\",\"worker_protocol_max\":\"0.73\",\"sdk_must_match_contract_id_and_sha256\":true}" +
            L",\"provable_worlds_1024x1024_admitted\":false}";
    }
}