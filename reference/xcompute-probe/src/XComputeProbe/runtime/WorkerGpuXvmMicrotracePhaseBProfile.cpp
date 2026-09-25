#include "pch.h"
#include "WorkerGpuXvmMicrotracePhaseBProfile.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion = L"gpu-xvm-microtrace-phase-b-v1";
        constexpr wchar_t const* ContractSha256 =
            L"3616723d3d9285914137b44f137811144c3c461ea48d3776e48424b0960c911f";
        constexpr wchar_t const* ShaderName = L"XvmGpuMicrotracePhaseBV1.cso";
        constexpr wchar_t const* ShaderSha256 =
            L"da619643dfa38e3aac40fbb0bb0cd5661fd30a3633af9efcabc237e0a11acdfa";
        constexpr wchar_t const* ShaderSourceSha256 =
            L"8824d201fdaa779dc1dd42075b98b4b8c7720c3e78674a81448c6e5a88ccead0";

        constexpr WorkerGpuXvmProfileDescriptor Profile
        {
            WorkerGpuXvmProfileKind::MicrotraceOptimizedStraightLineU32V1,
            ContractSchemaVersion,
            WorkerGpuXvmMicrotracePhaseBContractId,
            ContractSha256,
            WorkerGpuXvmMicrotracePhaseBProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_gpu_microtrace_v1_phase_b_optimized_cpu_gpu_differential",
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
            L"gpu-xvm-execution-plan-v9",
            L"gpu-xvm-resident-state-v1",
            0,
            false,
            false,
            true,
            WorkerGpuXvmMicrotracePhaseBWordsPerOpValue,
            WorkerGpuXvmMicrotracePhaseBMaxOpsValue,
        };
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotracePhaseBV1Profile()
    {
        return Profile;
    }

    std::wstring WorkerGpuXvmMicrotracePhaseBV1ProfileContractJson()
    {
        return std::wstring(L"{\"schema_version\":\"gpu-xvm-microtrace-phase-b-v1\"") +
            L",\"contract_id\":\"XVM_GPU_MICROTRACE_V1_PHASE_B_OPTIMIZATION\"" +
            L",\"contract_sha256\":\"" + ContractSha256 + L"\"" +
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\"" +
            L",\"profile_id\":\"gpu_microtrace_v1_phase_b_optimized_straight_line_u32_v1\"" +
            L",\"backend\":\"cpu_gpu_differential\"" +
            L",\"classification\":\"data_jit_not_shader_jit\"" +
            L",\"runtime_payload\":\"closed_typed_optimized_microtrace_data\"" +
            L",\"execution_plan_schema\":\"gpu-xvm-execution-plan-v9\"" +
            L",\"microtrace_record_schema\":\"xvm-gpu-microtrace-record-v2\"" +
            L",\"microtrace_words_per_op\":16" +
            L",\"max_microtrace_ops\":32" +
            L",\"max_source_span_per_record\":2" +
            L",\"constant_folding\":true" +
            L",\"alu_sequence_fusion\":true" +
            L",\"basic_block_superblock_planning\":true" +
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
