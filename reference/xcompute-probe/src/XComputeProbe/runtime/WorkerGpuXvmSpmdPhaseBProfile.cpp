#include "pch.h"
#include "WorkerGpuXvmSpmdPhaseBProfile.h"

#include <algorithm>
#include <array>

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion =
            L"gpu-xvm-spmd-many-lane-phase-b-v1";
        constexpr wchar_t const* ContractSha256 =
            L"115da90d099a6930b944e1045dd061232dd45ed5a9e03843f8af4a964064c9b2";
        constexpr wchar_t const* ShaderName = L"XvmSpmdManyLanePhaseB4x3V1.cso";
        constexpr wchar_t const* ShaderSha256 =
            L"95701f34621f42b4573f6443431e7e96cae49fb7c159c9da601266439143445b";
        constexpr wchar_t const* ShaderSourceSha256 =
            L"d94db3837504f55d1c36484024063a78adce21fa2ba81fa300fe889b32cb6eb6";

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
            WorkerGpuXvmProfileKind::SpmdManyLanePhaseB4x3U32V1,
            ContractSchemaVersion,
            WorkerGpuXvmSpmdPhaseBContractId,
            ContractSha256,
            WorkerGpuXvmSpmdPhaseBProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_spmd_many_lane_phase_b_v1",
            9,
            64,
            16,
            16,
            16,
            192,
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
            12,
            { 4, 3, 1 },
            { 1, 1, 1 },
            128,
            16,
            WorkerGpuXvmCapabilitySpmdLaneContextValue,
            1,
            L"gpu-xvm-execution-plan-v7",
            L"gpu-xvm-lane-state-v6",
            0,
            true,
            true,
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

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmSpmdPhaseBProfile()
    {
        return Profile;
    }

    bool WorkerGpuXvmSpmdPhaseBProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        return profile.kind == WorkerGpuXvmProfileKind::SpmdManyLanePhaseB4x3U32V1 &&
            program.schemaVersion == L"xvm-program-artifact-v2" &&
            program.isaVersion == L"xvm-v2" &&
            program.artifactKind == L"xvm-program-v2" &&
            program.profile == L"integer-deterministic-v2" &&
            program.programId == L"spmd_many_lane_context_4x3" &&
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
            limits.fuel == 192 &&
            limits.memoryBytes == 768 &&
            limits.outputBytes == 192;
    }

    std::wstring WorkerGpuXvmSpmdPhaseBProfileContractJson()
    {
        return std::wstring(L"{\"schema_version\":\"gpu-xvm-spmd-many-lane-phase-b-v1\"") +
            L",\"contract_id\":\"GPU_XVM_SPMD_MANY_LANE_V1_PHASE_B_GPU_DIFFERENTIAL\"" +
            L",\"contract_sha256\":" + std::wstring(L"\"") + ContractSha256 + L"\"" +
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\"" +
            L",\"profile_id\":\"spmd_many_lane_v1_phase_b_4x3_u32_v1\"" +
            L",\"backend\":\"cpu_gpu_spmd_differential\"" +
            L",\"cpu_reference_canonical\":true" +
            L",\"gpu_authority\":false" +
            L",\"runtime_shader_compilation\":false" +
            L",\"automatic_backend_selection\":false" +
            L",\"logical_lane_count\":12" +
            L",\"grid_shape\":[4,3,1]" +
            L",\"workgroup_shape\":[1,1,1]" +
            L",\"shared_input\":\"read_only_replicated\"" +
            L",\"private_state_per_lane\":true" +
            L",\"output_ownership\":\"disjoint_contiguous_slice_per_lane\"" +
            L",\"execution_plan_schema\":\"gpu-xvm-execution-plan-v7\"" +
            L",\"state_schema\":\"gpu-xvm-lane-state-v6\"" +
            L",\"spmd_plan_schema\":\"xvm-spmd-execution-plan-v1\"" +
            L",\"shader\":" + std::wstring(L"\"") + ShaderName + L"\"" +
            L",\"shader_sha256\":" + std::wstring(L"\"") + ShaderSha256 + L"\"" +
            L",\"shader_source_sha256\":" + std::wstring(L"\"") + ShaderSourceSha256 + L"\"" +
            L",\"stable_errors\":{\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"state_invalid\":\"xvm.gpu_resident_state_invalid\",\"lockstep_invalid\":\"xvm.gpu_spmd_lockstep_invalid\",\"differential_mismatch\":\"xvm.gpu_spmd_differential_mismatch\"}" +
            L",\"protocol_compatibility\":{\"worker_protocol_min\":\"0.73\",\"worker_protocol_max\":\"0.73\",\"sdk_must_match_contract_id_and_sha256\":true}" +
            L",\"full_isa_mapping_complete\":false}";
    }
}