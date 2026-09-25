#include "pch.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"

#include <algorithm>
#include <array>

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion = L"gpu-xvm-provable-worlds-v1";
        constexpr wchar_t const* ContractSha256 =
            L"8587a2fc3a26782861cce2c621cbccaafe78ff249f502da9c4759ebb9d9d89b6";
        constexpr wchar_t const* ShaderName = L"XvmProvableWorldsV1.cso";
        constexpr wchar_t const* ShaderSha256 =
            L"835a745b098711b20c5fbf9d6b32f55ff68623e786e2854b052f86de6e08579d";
        constexpr wchar_t const* ShaderSourceSha256 =
            L"e2e63f692a65b1942746a139f2778bb2b2ae40ede6a32f4b07552f9a239e8af2";

        constexpr std::array<uint32_t, 64> ExpectedProgramWords =
        {
            2, 0, 0, 0,
            1, 1, 374761393u, 0,
            5, 2, 13, 1,
            1, 3, 668265263u, 0,
            5, 4, 14, 3,
            4, 5, 2, 4,
            4, 6, 5, 0,
            6, 7, 6, 13,
            3, 8, 7, 13,
            4, 9, 8, 14,
            13, 9, 0, 0,
            15, 13, 0, 0,
            0, 0, 0, 0,
            1, 10, 1, 0,
            14, 10, 0, 0,
            16, 0, 0, 0,
        };

        constexpr WorkerGpuXvmProfileDescriptor Profile
        {
            WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1,
            ContractSchemaVersion,
            WorkerGpuXvmProvableWorldsContractId,
            ContractSha256,
            WorkerGpuXvmProvableWorldsProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_provable_worlds_1024x1024_gpu_differential_v1",
            16,
            64,
            4,
            16,
            8,
            11,
            64,
            1048576,
            0,
            true,
            false,
            false,
            16,
            1,
            false,
            true,
            true,
            false,
            false,
            1048576,
            { 1024, 1024, 1 },
            { 8, 8, 1 },
            1,
            0,
            WorkerGpuXvmCapabilitySpmdLaneContextValue,
            1,
            L"gpu-xvm-execution-plan-v11",
            L"gpu-xvm-provable-worlds-field-v1",
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

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmProvableWorldsProfile()
    {
        return Profile;
    }

    bool WorkerGpuXvmProvableWorldsProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        return profile.kind == WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1 &&
            program.schemaVersion == L"xvm-program-artifact-v2" &&
            program.isaVersion == L"xvm-v2" &&
            program.artifactKind == L"xvm-program-v2" &&
            program.profile == L"integer-deterministic-v2" &&
            program.programId == L"provable_worlds_field_v1" &&
            HasExactCapabilities(program.capabilities) &&
            program.memoryBytes == 64 &&
            program.outputBytes == 4 &&
            program.maxFuel == 16 &&
            program.staticWorstCaseFuel == 16 &&
            program.maxCallDepth == 1 &&
            program.functions.size() == 1 &&
            program.functions[0].entryPc == 13 &&
            program.functions[0].endPc == 15 &&
            program.words.size() == ExpectedProgramWords.size() &&
            std::equal(program.words.begin(), program.words.end(), ExpectedProgramWords.begin()) &&
            limits.fuel == 16777216ull &&
            limits.memoryBytes == 67108864ull &&
            limits.outputBytes == 4194304ull;
    }

    std::wstring WorkerGpuXvmProvableWorldsProfileContractJson()
    {
        return std::wstring(L"{\"schema_version\":\"gpu-xvm-provable-worlds-v1\"") +
            L",\"contract_id\":\"PROVABLE_WORLDS_V1\"" +
            L",\"contract_sha256\":\"" + ContractSha256 + L"\"" +
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\"" +
            L",\"profile_id\":\"provable_worlds_v1_1024x1024_u32\"" +
            L",\"backend\":\"cpu_gpu_spmd_differential\"" +
            L",\"logical_grid_shape\":[1024,1024,1]" +
            L",\"logical_lane_count\":1048576" +
            L",\"workgroup_shape\":[8,8,1]" +
            L",\"dispatch_group_shape\":[128,128,1]" +
            L",\"single_logical_dispatch\":true" +
            L",\"field_format\":\"u32_le\"" +
            L",\"field_bytes\":4194304" +
            L",\"assurance_class\":\"exact_cpu_gpu_differential_v1\"" +
            L",\"artifact_lifecycle\":[\"generated_provisional\",\"verification_pending\",\"verified\",\"quarantined\"]" +
            L",\"cpu_reference_canonical\":true" +
            L",\"gpu_authority\":false" +
            L",\"runtime_shader_compilation\":false" +
            L",\"shader\":\"" + ShaderName + L"\"" +
            L",\"shader_sha256\":\"" + ShaderSha256 + L"\"" +
            L",\"shader_source_sha256\":\"" + ShaderSourceSha256 + L"\"" +
            L",\"execution_plan_schema\":\"gpu-xvm-execution-plan-v11\"" +
            L",\"spmd_plan_schema\":\"xvm-spmd-execution-plan-v3\"" +
            L",\"protocol_compatibility\":{\"worker_protocol_min\":\"0.73\",\"worker_protocol_max\":\"0.73\",\"sdk_must_match_contract_id_and_sha256\":true}" +
            L",\"production_gpu_authority\":false}";
    }
}
