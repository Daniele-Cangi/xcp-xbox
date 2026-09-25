#include "pch.h"
#include "WorkerXvmIsa.h"

#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphRuntime.h"
#include "WorkerXvmIsaV2.generated.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr uint64_t InstructionLimitValue = 4096;
        constexpr uint64_t LoopIterationLimitValue = 65536;
        constexpr uint64_t LoopDepthLimitValue = 8;
        constexpr uint64_t MaxCallDepthValue = 8;
        constexpr uint64_t ConditionalDepthLimitValue = 8;
        constexpr uint64_t TrapCodeLimitValue = 65535;

        constexpr WorkerXvmIsaSpec V1Spec
        {
            L"xvm-v1",
            L"xvm-program-artifact-v1",
            L"xvm-program-v1",
            L"integer-deterministic-v1",
            L"xvm_integer_deterministic_v1",
            static_cast<uint32_t>(WorkerXvmOpcode::SetControl),
            0,
            false,
        };

        constexpr WorkerXvmIsaSpec V2Spec
        {
            L"xvm-v2",
            L"xvm-program-artifact-v2",
            L"xvm-program-v2",
            L"integer-deterministic-v2",
            L"xvm_integer_deterministic_v2",
            static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero),
            MaxCallDepthValue,
            true,
        };
    }

    WorkerXvmIsaSpec const& WorkerXvmV1Spec()
    {
        return V1Spec;
    }

    WorkerXvmIsaSpec const& WorkerXvmV2Spec()
    {
        return V2Spec;
    }

    WorkerXvmIsaSpec const* WorkerFindXvmIsaSpec(std::wstring const& isaVersion)
    {
        if (isaVersion == V1Spec.isaVersion)
        {
            return &V1Spec;
        }
        if (isaVersion == V2Spec.isaVersion)
        {
            return &V2Spec;
        }
        return nullptr;
    }

    bool WorkerXvmOpcodeIsAdmitted(WorkerXvmIsaSpec const& spec, WorkerXvmOpcode opcode)
    {
        return static_cast<uint32_t>(opcode) <= spec.highestOpcode;
    }

    uint32_t WorkerXvmRegisterCount()
    {
        return WorkerXvmRegisterCountValue;
    }

    uint32_t WorkerXvmInstructionWords()
    {
        return WorkerXvmInstructionWordsValue;
    }

    uint32_t WorkerXvmInputWordCount()
    {
        return WorkerXvmInputWordCountValue;
    }

    uint64_t WorkerXvmInstructionLimit()
    {
        return InstructionLimitValue;
    }

    uint64_t WorkerXvmLoopIterationLimit()
    {
        return LoopIterationLimitValue;
    }

    uint64_t WorkerXvmLoopDepthLimit()
    {
        return LoopDepthLimitValue;
    }

    uint64_t WorkerXvmMaxCallDepth()
    {
        return MaxCallDepthValue;
    }

    uint64_t WorkerXvmConditionalDepthLimit()
    {
        return ConditionalDepthLimitValue;
    }

    uint64_t WorkerXvmTrapCodeLimit()
    {
        return TrapCodeLimitValue;
    }

    std::wstring WorkerXvmIsaCatalogJson()
    {
        return
            L"[{\"isa_version\":\"xvm-v1\",\"schema_version\":\"xvm-program-artifact-v1\","
            L"\"artifact_kind\":\"xvm-program-v1\",\"profile\":\"integer-deterministic-v1\","
            L"\"instruction_words\":4,\"register_count\":16,\"structured_calls\":false,\"max_call_depth\":0,"
            L"\"opcodes\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\","
            L"\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\","
            L"\"branch_if_zero\",\"loop_begin\",\"loop_end\",\"output_u32\",\"set_control\"]},"
            L"{\"isa_version\":\"xvm-v2\",\"schema_version\":\"xvm-program-artifact-v2\","
            L"\"artifact_kind\":\"xvm-program-v2\",\"profile\":\"integer-deterministic-v2\","
            L"\"instruction_words\":4,\"register_count\":16,\"structured_calls\":true,\"max_call_depth\":8,"
            L"\"optional_capabilities\":[\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\",\"spmd_lane_context_v1\"],\"typed_memory_element_types\":[\"u32\"],"
            L"\"max_conditional_depth\":8,\"structured_control_snapshot_stack\":false,"
            L"\"opcodes\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\","
            L"\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\","
            L"\"branch_if_zero\",\"loop_begin\",\"loop_end\",\"output_u32\",\"set_control\",\"call\",\"return\","
            L"\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\","
            L"\"break_if_zero\",\"continue_if_zero\",\"trap_if_zero\"]}]";
    }

    std::wstring WorkerXvmIsaDefinitionJson()
    {
        return WorkerXvmIsaV2DefinitionJsonValue;
    }

    std::wstring WorkerXvmIsaDefinitionSha256()
    {
        return WorkerXvmIsaV2DefinitionSha256Value;
    }

    std::wstring WorkerXvmPublishedErrorCatalogJson()
    {
        return
            L"{\"schema_version\":\"xvm-error-catalog-v1\",\"exhaustive\":false,"
            L"\"details_schema\":\"xvm-error-details-v1\",\"stable_identifier\":\"error.code\","
            L"\"message_machine_actionable\":false,\"codes\":["
            L"{\"code\":\"xvm.isa_version_invalid\",\"stage\":\"graph_admission\",\"correction\":\"refresh_isa_and_reassemble\"},"
            L"{\"code\":\"xvm.program_schema_invalid\",\"stage\":\"program_validation\",\"correction\":\"use_published_program_schema\"},"
            L"{\"code\":\"xvm.opcode_not_admitted\",\"stage\":\"program_validation\",\"correction\":\"regenerate_from_published_opcode_table\"},"
            L"{\"code\":\"xvm.capability_not_granted\",\"stage\":\"graph_admission\",\"correction\":\"grant_missing_published_capability\"},"
            L"{\"code\":\"xvm.program_artifact_hash_mismatch\",\"stage\":\"artifact_admission\",\"correction\":\"bind_exact_assembler_sha256\"},"
            L"{\"code\":\"xvm.static_fuel_program_limit_insufficient\",\"stage\":\"program_validation\",\"correction\":\"reassemble_with_verified_fuel\"},"
            L"{\"code\":\"xvm.static_fuel_node_limit_insufficient\",\"stage\":\"graph_admission\",\"correction\":\"raise_node_fuel_to_verified_bound\"},"
            L"{\"code\":\"xvm.resource_contract_invalid\",\"stage\":\"graph_admission\",\"correction\":\"project_resource_limits_from_assembler_metadata\"},"
            L"{\"code\":\"xvm.typed_view_access_invalid\",\"stage\":\"program_validation\",\"correction\":\"repair_typed_view_or_access\"},"
            L"{\"code\":\"xvm.structured_control_target_invalid\",\"stage\":\"program_validation\",\"correction\":\"repair_structured_labels_and_reassemble\"},"
            L"{\"code\":\"xvm.fuel_exhausted\",\"stage\":\"execution\",\"correction\":\"treat_as_terminal_and_reverify_static_contract\"},"
            L"{\"code\":\"xvm.gpu_differential_mismatch\",\"stage\":\"execution\",\"correction\":\"quarantine_gpu_result_and_use_cpu_canonical\"}"
            L"]}";
    }

    std::wstring WorkerXvmSubmissionProfileJson()
    {
        return
            L"{\"schema_version\":\"xvm-submission-profile-v1\","
            L"\"profile_id\":\"xvm-v2-cpu-reference-v1\","
            L"\"command\":\"submit_graph\","
            L"\"graph_schema_version\":\"worker-on-device-graph-v1\","
            L"\"node_command\":\"xvm_program\","
            L"\"isa_version\":\"xvm-v2\","
            L"\"program_schema_version\":\"xvm-program-artifact-v2\","
            L"\"artifact_kind\":\"xvm-program-v2\","
            L"\"profile\":\"integer-deterministic-v2\","
            L"\"backend\":\"cpu_reference\","
            L"\"input_binding\":{\"mode\":\"typed_xvm_inputs_v1\"},"
            L"\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\"],"
            L"\"optional_capabilities\":[\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\"],"
            L"\"resource_limits\":{\"max_fuel\":" +
            std::to_wstring(WorkerGraphMaxFuel()) +
            L",\"max_memory_bytes\":" +
            std::to_wstring(WorkerGraphMaxXvmMemoryBytes()) +
            L",\"max_output_bytes\":" +
            std::to_wstring(WorkerGraphMaxXvmOutputBytes()) +
            L"},\"graph_limits\":{\"max_nodes\":" +
            std::to_wstring(WorkerMaxOnDeviceGraphNodes()) +
            L",\"max_edges\":" +
            std::to_wstring(WorkerMaxOnDeviceGraphEdges()) +
            L"},\"artifact_transfer\":{\"lifecycle\":\"missing_to_staging_to_committed\","
            L"\"begin_command\":\"begin_artifact_upload\","
            L"\"append_command\":\"append_artifact_chunk\","
            L"\"commit_command\":\"commit_artifact_upload\","
            L"\"status_command\":\"get_artifact_status\"},"
            L"\"canonical_result_path\":\"nodes[].execution_details.functional_canonical\","
            L"\"error_contract\":" + WorkerXvmPublishedErrorCatalogJson() +
            L",\"boundaries\":{\"native_or_host_code\":false,"
            L"\"runtime_shader_compilation\":false,\"client_shader_upload\":false,"
            L"\"shell_or_process\":false,\"broad_filesystem\":false,"
            L"\"xvm_sockets\":false,\"credentials_on_xbox\":false}}";
    }
}
