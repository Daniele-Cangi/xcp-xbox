#include "pch.h"
#include "WorkerGpuXvmMicrotraceProfile.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion = L"gpu-xvm-microtrace-v1";
        constexpr wchar_t const* ContractSha256 =
            L"bf1a4287e380de3cb4d6c3cd9ef5252231723b786df129a3ff820a4a49c61cd7";
        constexpr wchar_t const* ShaderName = L"XvmGpuMicrotraceV1.cso";
        constexpr wchar_t const* ShaderSha256 =
            L"394a769bab928e81d8b1f1e9aeec11d616266bbd3faefed2e2e91617945703d7";
        constexpr wchar_t const* ShaderSourceSha256 =
            L"49ecc0bdf241ef0e8ac22dbd6196038f8a0650cc4377cee42d7fc9ccb954a50c";

        constexpr WorkerGpuXvmProfileDescriptor Profile
        {
            WorkerGpuXvmProfileKind::MicrotraceIdentityStraightLineU32V1,
            ContractSchemaVersion,
            WorkerGpuXvmMicrotraceContractId,
            ContractSha256,
            WorkerGpuXvmMicrotraceProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_gpu_microtrace_v1_identity_cpu_gpu_differential",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmGeneralParameterWordsValue,
            WorkerGpuXvmGeneralInputWordsValue,
            WorkerGpuXvmGeneralProgramWordsValue,
            WorkerGpuXvmGeneralResultWordsValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmResidentEpochInstructionBudgetValue,
            WorkerGpuXvmResidentMaxEpochsValue,
            false,
            false,
            true,
            true,
            false,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmGeneralResultWordsValue,
            WorkerGpuXvmGeneralInputWordsValue,
            0,
            0,
            L"gpu-xvm-execution-plan-v8",
            L"gpu-xvm-resident-state-v1",
            0,
            false,
            false,
            true,
            WorkerGpuXvmMicrotraceWordsPerOpValue,
            WorkerGpuXvmMicrotraceMaxOpsValue,
        };
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotraceV1Profile()
    {
        return Profile;
    }

    std::wstring WorkerGpuXvmMicrotraceV1ProfileContractJson()
    {
        return std::wstring(L"{\"schema_version\":\"gpu-xvm-microtrace-v1\"") +
            L",\"contract_id\":\"XVM_GPU_MICROTRACE_V1\"" +
            L",\"contract_sha256\":\"" + ContractSha256 + L"\"" +
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\"" +
            L",\"profile_id\":\"gpu_microtrace_v1_identity_straight_line_u32_v1\"" +
            L",\"backend\":\"cpu_gpu_differential\"" +
            L",\"classification\":\"data_jit_not_shader_jit\"" +
            L",\"runtime_payload\":\"closed_typed_microtrace_data\"" +
            L",\"execution_plan_schema\":\"gpu-xvm-execution-plan-v8\"" +
            L",\"microtrace_record_schema\":\"xvm-gpu-microtrace-record-v1\"" +
            L",\"microtrace_words_per_op\":8" +
            L",\"max_microtrace_ops\":32" +
            L",\"source_pc_map_required\":true" +
            L",\"worker_revalidation_required\":true" +
            L",\"exact_source_instruction_fuel\":true" +
            L",\"resident_shader_consumes_microtrace_srv\":true" +
            L",\"program_srv_crosscheck_required\":true" +
            L",\"fallback_profile_id\":\"resident_core_v1_straight_line_u32_v1\"" +
            L",\"cpu_reference_canonical\":true" +
            L",\"gpu_authority\":false" +
            L",\"runtime_shader_compilation\":false" +
            L",\"emits_executable_code\":false" +
            L",\"shader\":\"" + ShaderName + L"\"" +
            L",\"shader_sha256\":\"" + ShaderSha256 + L"\"" +
            L",\"shader_source_sha256\":\"" + ShaderSourceSha256 + L"\"" +
            L",\"stable_errors\":{\"plan_invalid\":\"xvm.microtrace_plan_invalid\",\"revalidation_failed\":\"xvm.microtrace_revalidation_failed\",\"differential_mismatch\":\"xvm.gpu_microtrace_differential_mismatch\"}}";
    }
}