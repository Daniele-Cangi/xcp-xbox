#include "pch.h"
#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmFullIsaV2Profiles.h"
#include "WorkerGpuXvmMicrotraceProfile.h"
#include "WorkerGpuXvmMicrotracePhaseBProfile.h"
#include "WorkerGpuXvmSpmdPhaseBProfile.h"
#include "WorkerGpuXvmSpmdGeneralizedProfile.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"
#include "WorkerXvmIsa.h"

using namespace winrt;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage::Streams;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ContractSchemaVersion = L"gpu-xvm-profile-contract-v1";
        constexpr wchar_t const* ContractId = L"GPU_XVM_PROFILE_CONTRACT_V1";
        constexpr wchar_t const* ContractSha256 = L"a9d9fc24d095474975d0476e37f50d749aec7e9efbbfb4c0b63e047cc3d3624c";
        constexpr wchar_t const* ProfileId = L"m150_seed_straight_line_v1";
        constexpr wchar_t const* ShaderName = L"XvmIntegerNucleus.cso";
        constexpr wchar_t const* ShaderSha256 = L"47e777ec074fc3dbbf74be907d913c5c558b0171cacd5b23ba37b06a0bf5634d";
        constexpr wchar_t const* ShaderSourceSha256 = L"e63f536281dd0c62a28400376cf8ef9ed448069bc6e971bd47b2fc563e13b6ef";
        constexpr wchar_t const* InstructionWordsSha256 = L"da492215ac064afcbfcb964cc335caef53202fab975b6ad93e795d302c8dd9e1";
        constexpr wchar_t const* GeneralContractSchemaVersion = L"gpu-xvm-general-profile-v1";
        constexpr wchar_t const* GeneralContractId = L"GPU_XVM_GENERAL_V1_PHASE_A";
        constexpr wchar_t const* GeneralContractSha256 = L"f9c0555fd8bbeb9710c1a1505cbd9cad74e54b76c200decfc94a5fe4ea142392";
        constexpr wchar_t const* GeneralProfileId = L"general_v1_straight_line_u32_v1";
        constexpr wchar_t const* GeneralShaderName = L"XvmIntegerGeneralV1.cso";
        constexpr wchar_t const* GeneralShaderSha256 = L"75af411cdf973fd39a3a5cc6d0aa6666536ad597dc27481e98434cdadd7aed39";
        constexpr wchar_t const* GeneralShaderSourceSha256 = L"542b80fd957ca066f94f1aa3ef788676f1f4058ff4f5eb920fb13e3d466a9a01";
        constexpr wchar_t const* ResidentContractSchemaVersion = L"gpu-xvm-resident-execution-core-v1";
        constexpr wchar_t const* ResidentContractId = L"GPU_XVM_RESIDENT_EXECUTION_CORE_V1";
        constexpr wchar_t const* ResidentContractSha256 = L"d8eff2d2769eb176ce7afa2d5242adc12afdc7dcdf20f0d70efbebebd134ef3e";
        constexpr wchar_t const* ResidentProfileId = L"resident_core_v1_straight_line_u32_v1";
        constexpr wchar_t const* ResidentShaderName = L"XvmResidentExecutionCoreV1.cso";
        constexpr wchar_t const* ResidentShaderSha256 = L"4ae1036d6d42e91b72482cb1d6323c0c2b59bd01b51914b63900ba9d1e88987e";
        constexpr wchar_t const* ResidentShaderSourceSha256 = L"2c5c11a59377aa6f5239ff2a6cf982ffdbb047e13200a2e41c78bc14f56c1bd1";

        constexpr WorkerGpuXvmProfileDescriptor M150Profile
        {
            WorkerGpuXvmProfileKind::M150SeedStraightLineV1,
            ContractSchemaVersion,
            ContractId,
            ContractSha256,
            ProfileId,
            ShaderName,
            ShaderSha256,
            ShaderSourceSha256,
            L"xvm_integer_deterministic_cpu_gpu_differential_v1",
            WorkerGpuXvmMaxInstructionsValue,
            WorkerGpuXvmMemoryBytesValue,
            WorkerGpuXvmOutputBytesValue,
            WorkerGpuXvmFuelLimitValue,
            WorkerGpuXvmParameterWordsValue,
            WorkerGpuXvmInputWordsValue,
            0,
            WorkerGpuXvmResultWordsValue,
            WorkerGpuXvmResultOutputWordOffsetValue,
            false,
            false,
            false,
            0,
            1,
            true,
        };

        constexpr WorkerGpuXvmProfileDescriptor GeneralProfile
        {
            WorkerGpuXvmProfileKind::GeneralStraightLineU32V1,
            GeneralContractSchemaVersion,
            GeneralContractId,
            GeneralContractSha256,
            GeneralProfileId,
            GeneralShaderName,
            GeneralShaderSha256,
            GeneralShaderSourceSha256,
            L"xvm_integer_deterministic_cpu_gpu_general_v1_phase_a",
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
            true,
            false,
            1,
            32,
            true,
        };

        constexpr WorkerGpuXvmProfileDescriptor ResidentProfile
        {
            WorkerGpuXvmProfileKind::ResidentCoreStraightLineU32V1,
            ResidentContractSchemaVersion,
            ResidentContractId,
            ResidentContractSha256,
            ResidentProfileId,
            ResidentShaderName,
            ResidentShaderSha256,
            ResidentShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_resident_core_v1",
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
            false,
            true,
            false,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmGeneralResultWordsValue,
            WorkerGpuXvmGeneralInputWordsValue,
            0,
            0,
        };

        constexpr std::array<uint32_t, 60> ExpectedWords =
        {
            2, 0, 0, 0,
            2, 1, 1, 0,
            4, 2, 0, 1,
            1, 3, 2654435769u, 0,
            2, 4, 10, 0,
            10, 4, 13, 0,
            11, 8, 10, 0,
            3, 2, 2, 3,
            6, 2, 2, 5,
            8, 2, 0, 0,
            12, 6, 0, 0,
            7, 5, 0, 0,
            13, 5, 0, 0,
            14, 4, 0, 0,
            0, 0, 0, 0,
        };

        bool HasExactCapabilities(std::vector<std::wstring> const& capabilities)
        {
            static constexpr std::array<wchar_t const*, 3> Required =
            {
                L"artifact_input",
                L"artifact_output",
                L"control_output",
            };
            if (capabilities.size() != Required.size())
            {
                return false;
            }
            return std::all_of(Required.begin(), Required.end(), [&](wchar_t const* required)
            {
                return std::find(capabilities.begin(), capabilities.end(), required) != capabilities.end();
            });
        }

        bool GeneralStraightLineOpcodeIsAdmitted(uint32_t opcode)
        {
            switch (static_cast<WorkerXvmOpcode>(opcode))
            {
            case WorkerXvmOpcode::Halt:
            case WorkerXvmOpcode::MoveImmediate:
            case WorkerXvmOpcode::LoadInputU32:
            case WorkerXvmOpcode::AddU32:
            case WorkerXvmOpcode::XorU32:
            case WorkerXvmOpcode::MultiplyU32:
            case WorkerXvmOpcode::RotateLeftU32:
            case WorkerXvmOpcode::LoadMemoryU32:
            case WorkerXvmOpcode::StoreMemoryU32:
            case WorkerXvmOpcode::EqualU32:
            case WorkerXvmOpcode::OutputU32:
            case WorkerXvmOpcode::SetControl:
                return true;
            default:
                return false;
            }
        }

    }

    wchar_t const* WorkerGpuXvmProfileContractSchemaVersion()
    {
        return ContractSchemaVersion;
    }

    wchar_t const* WorkerGpuXvmProfileContractId()
    {
        return ContractId;
    }

    wchar_t const* WorkerGpuXvmProfileContractSha256()
    {
        return ContractSha256;
    }

    wchar_t const* WorkerGpuXvmProfileId()
    {
        return ProfileId;
    }

    wchar_t const* WorkerGpuXvmShaderName()
    {
        return ShaderName;
    }

    wchar_t const* WorkerGpuXvmShaderSha256()
    {
        return ShaderSha256;
    }

    wchar_t const* WorkerGpuXvmShaderSourceSha256()
    {
        return ShaderSourceSha256;
    }

    wchar_t const* WorkerGpuXvmInstructionWordsSha256()
    {
        return InstructionWordsSha256;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmM150Profile()
    {
        return M150Profile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmGeneralV1Profile()
    {
        return GeneralProfile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmResidentCoreV1Profile()
    {
        return ResidentProfile;
    }

    WorkerGpuXvmProfileDescriptor const* WorkerFindGpuXvmProfile(std::wstring const& profileId)
    {
        if (profileId == M150Profile.profileId)
        {
            return &M150Profile;
        }
        if (profileId == GeneralProfile.profileId)
        {
            return &GeneralProfile;
        }
        if (profileId == ResidentProfile.profileId)
        {
            return &ResidentProfile;
        }
        if (profileId == WorkerGpuXvmMicrotraceProfileId)
        {
            return &WorkerGpuXvmMicrotraceV1Profile();
        }
        if (profileId == WorkerGpuXvmMicrotracePhaseBProfileId)
        {
            return &WorkerGpuXvmMicrotracePhaseBV1Profile();
        }
        if (profileId == WorkerGpuXvmSpmdPhaseBProfileId)
        {
            return &WorkerGpuXvmSpmdPhaseBProfile();
        }
        if (profileId == WorkerGpuXvmSpmdGeneralizedProfileId)
        {
            return &WorkerGpuXvmSpmdGeneralizedProfile();
        }
        if (profileId == WorkerGpuXvmProvableWorldsProfileId)
        {
            return &WorkerGpuXvmProvableWorldsProfile();
        }
        if (auto fullIsaProfile = WorkerFindGpuXvmFullIsaV2Profile(profileId))
        {
            return fullIsaProfile;
        }
        return nullptr;
    }

    bool WorkerGpuXvmProfileMatches(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        return WorkerGpuXvmProfileMatches(M150Profile, program, limits);
    }

    bool WorkerGpuXvmProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        if (profile.kind == WorkerGpuXvmProfileKind::SpmdManyLanePhaseB4x3U32V1)
        {
            return WorkerGpuXvmSpmdPhaseBProfileMatches(profile, program, limits);
        }
        if (profile.kind == WorkerGpuXvmProfileKind::SpmdManyLaneGeneralized8x8U32V1)
        {
            return WorkerGpuXvmSpmdGeneralizedProfileMatches(profile, program, limits);
        }
        if (profile.kind == WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1)
        {
            return WorkerGpuXvmProvableWorldsProfileMatches(profile, program, limits);
        }
        if (profile.kind == WorkerGpuXvmProfileKind::FullIsaV2TypedStructuredU32V1 ||
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2LoopControlU32V1 ||
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2FullCallReturnU32V1 ||
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2RecoveredTrapU32V1 ||
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2LegacyBranchU32V1)
        {
            return WorkerGpuXvmFullIsaV2ProfileMatches(profile, program, limits);
        }

        auto baseContractMatches = program.schemaVersion == L"xvm-program-artifact-v1" &&
            program.isaVersion == L"xvm-v1" &&
            program.artifactKind == L"xvm-program-v1" &&
            program.profile == L"integer-deterministic-v1" &&
            HasExactCapabilities(program.capabilities) &&
            program.memoryBytes == profile.memoryBytes &&
            program.outputBytes == profile.outputBytes &&
            program.maxFuel == profile.fuelLimit &&
            limits.memoryBytes == profile.memoryBytes &&
            limits.outputBytes == profile.outputBytes &&
            limits.fuel == profile.fuelLimit;
        if (!baseContractMatches)
        {
            return false;
        }

        if (profile.kind == WorkerGpuXvmProfileKind::M150SeedStraightLineV1)
        {
            return program.words.size() == ExpectedWords.size() &&
                std::equal(program.words.begin(), program.words.end(), ExpectedWords.begin());
        }

        auto instructionWords = WorkerXvmInstructionWords();
        auto instructionCount = program.words.size() / instructionWords;
        if ((program.words.size() % instructionWords) != 0 || instructionCount < 2 ||
            instructionCount > profile.maxInstructions ||
            program.words[(instructionCount - 1) * instructionWords] != static_cast<uint32_t>(WorkerXvmOpcode::Halt))
        {
            return false;
        }
        for (size_t pc = 0; pc < instructionCount; ++pc)
        {
            if (!GeneralStraightLineOpcodeIsAdmitted(program.words[pc * instructionWords]))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring WorkerGpuXvmHashBytes(std::vector<uint8_t> const& bytes)
    {
        auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
        auto buffer = CryptographicBuffer::CreateFromByteArray(bytes);
        auto value = std::wstring(CryptographicBuffer::EncodeToHexString(provider.HashData(buffer)).c_str());
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool WorkerGpuXvmShaderMatchesContract(
        std::vector<uint8_t> const& bytes,
        std::wstring& actualSha256)
    {
        return WorkerGpuXvmShaderMatchesContract(M150Profile, bytes, actualSha256);
    }

    bool WorkerGpuXvmShaderMatchesContract(
        WorkerGpuXvmProfileDescriptor const& profile,
        std::vector<uint8_t> const& bytes,
        std::wstring& actualSha256)
    {
        actualSha256 = WorkerGpuXvmHashBytes(bytes);
        return actualSha256 == profile.shaderSha256;
    }

    bool WorkerGpuXvmIsDeviceLoss(HRESULT hr)
    {
        return hr == DXGI_ERROR_DEVICE_HUNG ||
            hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET ||
            hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    std::wstring WorkerGpuXvmProfileContractsJson()
    {
        return
            L"[{\"schema_version\":\"gpu-xvm-profile-contract-v1\""
            L",\"contract_id\":\"GPU_XVM_PROFILE_CONTRACT_V1\""
            L",\"contract_sha256\":\"a9d9fc24d095474975d0476e37f50d749aec7e9efbbfb4c0b63e047cc3d3624c\""
            L",\"status\":\"IMPLEMENTED_LOCAL_AND_STRUCTURALLY_VALIDATED\""
            L",\"implementation_scope\":\"bounded_measured_seed_profile_only\""
            L",\"profile_id\":\"m150_seed_straight_line_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"isa_version\":\"xvm-v1\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\"]"
            L",\"exact_instruction_count\":15"
            L",\"resource_caps\":{\"fuel_limit\":256,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"result_uav_words\":16}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"thread_group\":[1,1,1],\"dispatch_groups\":[1,1,1],\"dispatches_per_node\":1,\"epochs_per_node\":1,\"max_thread_invocations\":1}"
            L",\"static_work\":{\"max_unrolled_profile_iterations\":8,\"cpu_static_worst_case_fuel\":43,\"cpu_pass_fuel\":43,\"cpu_fail_fuel\":8,\"gpu_reported_fuel_must_equal_cpu\":true}"
            L",\"instruction_words_sha256\":\"da492215ac064afcbfcb964cc335caef53202fab975b6ad93e795d302c8dd9e1\""
            L",\"shader_id\":\"xvm_integer_nucleus_v1\""
            L",\"shader\":\"XvmIntegerNucleus.cso\""
            L",\"shader_sha256\":\"47e777ec074fc3dbbf74be907d913c5c558b0171cacd5b23ba37b06a0bf5634d\""
            L",\"shader_source_sha256\":\"e63f536281dd0c62a28400376cf8ef9ed448069bc6e971bd47b2fc563e13b6ef\""
            L",\"shader_bytes\":3168"
            L",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true"
            L",\"functional_result_separate_from_gpu_telemetry\":true"
            L",\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\"}"
            L",\"node_snapshot_admitted\":false"
            L",\"node_resume_admitted\":false"
            L",\"graph_checkpoint_after_completed_node_only\":true"
            L",\"async_cancel_idempotent\":true"
            L",\"runtime_shader_compilation\":false"
            L",\"backend_selection\":\"explicit_cpu_gpu_differential\""
            L",\"protocol_compatibility\":{\"worker_protocol_min\":\"0.73\",\"worker_protocol_max\":\"0.73\",\"negotiation_command\":\"negotiate_protocol\",\"sdk_compatibility_schema\":\"worker-sdk-compatibility-0.1\",\"sdk_must_match_contract_id_and_sha256\":true}"
            L",\"generalized_gpu_backend\":false}"
            L",{\"schema_version\":\"gpu-xvm-general-profile-v1\""
            L",\"contract_id\":\"GPU_XVM_GENERAL_V1_PHASE_A\""
            L",\"contract_sha256\":\"f9c0555fd8bbeb9710c1a1505cbd9cad74e54b76c200decfc94a5fe4ea142392\""
            L",\"status\":\"IMPLEMENTED_LOCAL_AND_LIVE_MEASURED\""
            L",\"target\":\"GPU_XVM_GENERAL_V1\""
            L",\"phase\":\"PHASE_A_STRAIGHT_LINE_U32\""
            L",\"implementation_scope\":\"bounded_general_straight_line_xvm_v1\""
            L",\"profile_id\":\"general_v1_straight_line_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"gpu_profile_selection\":\"explicit_gpu_profile_id\""
            L",\"isa_version\":\"xvm-v1\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"output_u32\",\"set_control\"]"
            L",\"instruction_count\":{\"min\":2,\"max\":32,\"final_halt_required\":true,\"branch_or_loop_admitted\":false}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"result_uav_words\":48}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"host_sequential_per_instruction\",\"thread_group\":[1,1,1],\"dispatch_groups\":[1,1,1],\"dispatches_per_node_max\":32,\"epochs_per_node\":1,\"max_thread_invocations\":32}"
            L",\"static_work\":{\"bounded_instruction_slots\":32,\"max_host_dispatches\":32,\"cpu_fuel_equals_executed_instruction_count\":true,\"gpu_reported_fuel_must_equal_cpu\":true}"
            L",\"shader_id\":\"xvm_integer_general_v1_phase_a\",\"shader\":\"XvmIntegerGeneralV1.cso\",\"shader_sha256\":\"75af411cdf973fd39a3a5cc6d0aa6666536ad597dc27481e98434cdadd7aed39\",\"shader_source_sha256\":\"542b80fd957ca066f94f1aa3ef788676f1f4058ff4f5eb920fb13e3d466a9a01\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\"}"
            L",\"node_snapshot_admitted\":false,\"node_resume_admitted\":false,\"graph_checkpoint_after_completed_node_only\":true,\"async_cancel_idempotent\":true"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping\":false,\"xvm_v2_gpu_execution\":false,\"phase_a_implemented\":true,\"live_measured\":true,\"generalized_gpu_backend\":false}"
            L",{\"schema_version\":\"gpu-xvm-resident-execution-core-v1\""
            L",\"contract_id\":\"GPU_XVM_RESIDENT_EXECUTION_CORE_V1\""
            L",\"contract_sha256\":\"d8eff2d2769eb176ce7afa2d5242adc12afdc7dcdf20f0d70efbebebd134ef3e\""
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"target\":\"GPU_XVM_RESIDENT_EXECUTION_CORE_V1\""
            L",\"implementation_scope\":\"bounded_resident_epoch_straight_line_xvm_v1\""
            L",\"profile_id\":\"resident_core_v1_straight_line_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v1\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"output_u32\",\"set_control\"]"
            L",\"instruction_count\":{\"min\":2,\"max\":32,\"final_halt_required\":true,\"branch_or_loop_admitted\":false}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words\":48}"
            L",\"resident_state\":{\"schema_version\":\"gpu-xvm-resident-state-v1\",\"pc_word\":12,\"epoch_word\":13,\"status_word\":14,\"register_word\":16,\"memory_word\":32,\"gpu_resident\":true}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"thread_group\":[1,1,1],\"dispatch_groups\":[1,1,1],\"epoch_instruction_budget\":8,\"max_epochs\":4,\"max_dispatches_per_node\":4,\"host_dispatch_per_instruction\":false}"
            L",\"epoch_boundaries\":{\"state_readback_validated\":true,\"cancel_checked_before_and_after\":true,\"device_loss_checked\":true,\"public_snapshot_admitted\":false,\"public_resume_admitted\":false}"
            L",\"shader_id\":\"xvm_resident_execution_core_v1\",\"shader\":\"XvmResidentExecutionCoreV1.cso\",\"shader_sha256\":\"4ae1036d6d42e91b72482cb1d6323c0c2b59bd01b51914b63900ba9d1e88987e\",\"shader_source_sha256\":\"2c5c11a59377aa6f5239ff2a6cf982ffdbb047e13200a2e41c78bc14f56c1bd1\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"resident_state_invalid\":\"xvm.gpu_resident_state_invalid\",\"epoch_progress_invalid\":\"xvm.gpu_epoch_progress_invalid\",\"epoch_limit_exceeded\":\"xvm.gpu_epoch_limit_exceeded\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\",\"canceled\":\"job.canceled\"}"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping\":false,\"xvm_v2_gpu_execution\":false,\"live_measured\":false,\"production_gpu_authority\":false,\"generalized_gpu_backend\":false}"
            L",{\"schema_version\":\"gpu-xvm-full-isa-v2-typed-structured-slice-v1\""
            L",\"contract_id\":\"GPU_XVM_FULL_ISA_V2_TYPED_STRUCTURED_SLICE_V1\""
            L",\"contract_sha256\":\"580e41cab4530ec0a96cef46f5befd4abd0ede6f467abd99d64d01a9b9947ed6\""
            L",\"status\":\"IMPLEMENTED_LOCAL_AND_LIVE_MEASURED\""
            L",\"target\":\"GPU_XVM_FULL_ISA_V2\""
            L",\"phase\":\"TYPED_STRUCTURED_SLICE_V1\""
            L",\"profile_id\":\"full_isa_v2_typed_structured_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v2\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\",\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"output_u32\",\"set_control\",\"call\",\"return\",\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\"]"
            L",\"excluded_opcodes\":[\"branch_if_zero\",\"loop_begin\",\"loop_end\",\"break_if_zero\",\"continue_if_zero\",\"trap_if_zero\"]"
            L",\"function_subset\":{\"function_count\":1,\"max_call_depth\":1,\"leaf_only\":true}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words_per_lane\":96}"
            L",\"execution_plan\":{\"schema_version\":\"gpu-xvm-execution-plan-v2\",\"bound_fields\":[\"program_sha256\",\"profile_contract\",\"shader_sha256\",\"static_worst_case_fuel\",\"state_layout\",\"grid_shape\",\"workgroup_shape\"],\"epochs_derived_from_static_worst_case_fuel\":true}"
            L",\"lane_execution\":{\"state_schema\":\"gpu-xvm-lane-state-v2\",\"lane_parametric\":true,\"mode\":\"single_lane\",\"logical_lane_count\":1,\"max_logical_lanes\":1,\"grid_shape\":[1,1,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":false}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"epoch_instruction_budget\":8,\"max_epochs\":8,\"required_epochs_formula\":\"ceil(static_worst_case_fuel/8)\"}"
            L",\"shader_id\":\"xvm_full_isa_v2_typed_structured_v1\",\"shader\":\"XvmFullIsaV2TypedStructuredV1.cso\",\"shader_sha256\":\"2d99aaf0d25aaadd79291b03b1518493ba5fecfba2876342824cf30356df8a94\",\"shader_source_sha256\":\"0c852477adfa2c08164495c915caa4b6136cc59e9e514e72a0927f316c35bf46\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"resident_state_invalid\":\"xvm.gpu_resident_state_invalid\",\"epoch_progress_invalid\":\"xvm.gpu_epoch_progress_invalid\",\"epoch_limit_exceeded\":\"xvm.gpu_epoch_limit_exceeded\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\",\"canceled\":\"job.canceled\"}"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping_complete\":false,\"xvm_v2_gpu_execution\":true,\"live_measured\":true,\"production_gpu_authority\":false,\"many_lane_admitted\":false,\"generalized_gpu_backend_complete\":false}"
            L",{\"schema_version\":\"gpu-xvm-full-isa-v2-loop-control-slice-v1\""
            L",\"contract_id\":\"GPU_XVM_FULL_ISA_V2_LOOP_CONTROL_SLICE_V1\""
            L",\"contract_sha256\":\"22da98c9b957dfdcca0146e985d1f71f1ae45d0e4d74e663ce145c3588c28cbe\""
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"target\":\"GPU_XVM_FULL_ISA_V2\""
            L",\"phase\":\"LOOP_CONTROL_SLICE_V1\""
            L",\"profile_id\":\"full_isa_v2_loop_control_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v2\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\",\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"loop_begin\",\"loop_end\",\"output_u32\",\"set_control\",\"call\",\"return\",\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\",\"break_if_zero\",\"continue_if_zero\"]"
            L",\"excluded_opcodes\":[\"branch_if_zero\",\"trap_if_zero\"]"
            L",\"function_subset\":{\"function_count\":1,\"max_call_depth\":1,\"leaf_only\":true}"
            L",\"loop_control\":{\"max_loop_depth\":8,\"owner\":\"innermost_verified_loop_only\",\"condition\":\"register_equals_zero\",\"break_target\":\"loop_end_pc_plus_one\",\"continue_target\":\"loop_end_pc\"}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words_per_lane\":128}"
            L",\"execution_plan\":{\"schema_version\":\"gpu-xvm-execution-plan-v3\",\"state_schema\":\"gpu-xvm-lane-state-v3\",\"max_loop_depth\":8,\"loop_frame_capacity\":8,\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\",\"epochs_derived_from_static_worst_case_fuel\":true}"
            L",\"lane_execution\":{\"state_schema\":\"gpu-xvm-lane-state-v3\",\"lane_parametric\":true,\"mode\":\"single_lane\",\"logical_lane_count\":1,\"max_logical_lanes\":1,\"grid_shape\":[1,1,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":false}"
            L",\"loop_state_offsets\":{\"loop_depth\":76,\"max_loop_depth\":77,\"loop_begin_pc_stack\":86,\"loop_end_pc_stack\":94,\"loop_remaining_stack\":102}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"epoch_instruction_budget\":8,\"max_epochs\":8,\"required_epochs_formula\":\"ceil(static_worst_case_fuel/8)\"}"
            L",\"conformance_programs\":[{\"program_id\":\"gpu_full_isa_v2_loop_control_continue_taken\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-loop-control-continue.json\",\"instruction_count\":28,\"static_worst_case_fuel\":32,\"expected_fuel\":32,\"required_epoch_count\":4,\"expected_executed_epoch_count\":4,\"expected_control\":\"pass\",\"expected_output_hex\":\"62000000010000000000000000000000\",\"max_observed_loop_depth\":1,\"loop_frame_crosses_epoch\":true},{\"program_id\":\"gpu_full_isa_v2_loop_control_nested_break_taken\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-loop-control-nested-break.json\",\"instruction_count\":28,\"static_worst_case_fuel\":43,\"expected_fuel\":28,\"required_epoch_count\":6,\"expected_executed_epoch_count\":4,\"expected_control\":\"pass\",\"expected_output_hex\":\"0000000000000000de000000df000000\",\"max_observed_loop_depth\":2,\"loop_frame_crosses_epoch\":true},{\"program_id\":\"gpu_full_isa_v2_loop_control_depth8\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-loop-control-depth8.json\",\"instruction_count\":27,\"static_worst_case_fuel\":27,\"expected_fuel\":27,\"required_epoch_count\":4,\"expected_executed_epoch_count\":4,\"expected_control\":\"pass\",\"expected_output_hex\":\"05000000000000000000000000000000\",\"max_observed_loop_depth\":8,\"loop_frame_crosses_epoch\":true}]"
            L",\"shader_id\":\"xvm_full_isa_v2_loop_control_v1\",\"shader\":\"XvmFullIsaV2LoopControlV1.cso\",\"shader_sha256\":\"cd804ef8ca4aec340f78fb5562d8543cb77185093dade1c1713b28856cb64495\",\"shader_source_sha256\":\"2890ee49e7fa71c1d1bbfffac7759106d3f2e8dbb0dda5d50424742392f1a891\",\"shader_bytes\":18024,\"shader_identity_status\":\"FINAL_CONTENT_PINNED\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"resident_state_invalid\":\"xvm.gpu_resident_state_invalid\",\"epoch_progress_invalid\":\"xvm.gpu_epoch_progress_invalid\",\"epoch_limit_exceeded\":\"xvm.gpu_epoch_limit_exceeded\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\",\"canceled\":\"job.canceled\"}"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping_complete\":false,\"xvm_v2_gpu_execution\":true,\"live_measured\":false,\"production_gpu_authority\":false,\"many_lane_admitted\":false,\"generalized_gpu_backend_complete\":false}"
            L",{\"schema_version\":\"gpu-xvm-full-isa-v2-full-call-return-slice-v1\""
            L",\"contract_id\":\"GPU_XVM_FULL_ISA_V2_FULL_CALL_RETURN_SLICE_V1\""
            L",\"contract_sha256\":\"711059cfe9eb48d1de79c6a1dc01c73696d22382a297d76a9fd87956e31c6327\""
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"external_live_measured\":false"
            L",\"target\":\"GPU_XVM_FULL_ISA_V2\""
            L",\"phase\":\"FULL_CALL_RETURN_SLICE_V1\""
            L",\"profile_id\":\"full_isa_v2_full_call_return_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v2\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\",\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"loop_begin\",\"loop_end\",\"output_u32\",\"set_control\",\"call\",\"return\",\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\",\"break_if_zero\",\"continue_if_zero\"]"
            L",\"excluded_opcodes\":[\"branch_if_zero\",\"trap_if_zero\"]"
            L",\"function_subset\":{\"function_count_min\":1,\"function_count_max\":32,\"max_call_depth\":8,\"all_functions_reachable\":true,\"call_graph_acyclic\":true,\"recursion_allowed\":false,\"call_inside_active_loop_allowed\":false,\"balanced_loops_inside_callee_allowed\":true}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words_per_lane\":128}"
            L",\"execution_plan\":{\"schema_version\":\"gpu-xvm-execution-plan-v4\",\"state_schema\":\"gpu-xvm-lane-state-v3\",\"max_call_depth\":8,\"call_frame_capacity\":8,\"call_frame_layout\":\"parallel_return_pc_loop_depth_u32_v1\",\"return_pc_stack_word_offset\":60,\"call_loop_depth_stack_word_offset\":68,\"max_loop_depth\":8,\"loop_frame_capacity\":8,\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\",\"verified_function_regions_and_call_graph_hashed\":true,\"epochs_derived_from_static_worst_case_fuel\":true}"
            L",\"lane_execution\":{\"state_schema\":\"gpu-xvm-lane-state-v3\",\"lane_parametric\":true,\"mode\":\"single_lane\",\"logical_lane_count\":1,\"max_logical_lanes\":1,\"grid_shape\":[1,1,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":false}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"epoch_instruction_budget\":8,\"max_epochs\":8,\"required_epochs_formula\":\"ceil(static_worst_case_fuel/8)\"}"
            L",\"conformance_programs\":[{\"program_id\":\"gpu_full_isa_v2_full_call_return_depth8\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-full-call-return-depth8.json\",\"instruction_count\":20,\"static_worst_case_fuel\":20,\"expected_fuel\":20,\"required_epoch_count\":3,\"expected_executed_epoch_count\":3,\"expected_control\":\"pass\",\"expected_output_hex\":\"7f000000000000000000000000000000\",\"max_observed_call_depth\":8,\"max_observed_loop_depth\":0,\"call_frame_crosses_epoch\":true,\"loop_frame_crosses_epoch\":false},{\"program_id\":\"gpu_full_isa_v2_full_call_return_sibling_repeated_lifo\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-full-call-return-sibling-repeated-lifo.json\",\"instruction_count\":17,\"static_worst_case_fuel\":27,\"expected_fuel\":27,\"required_epoch_count\":4,\"expected_executed_epoch_count\":4,\"expected_control\":\"pass\",\"expected_output_hex\":\"10000000000000000000000000000000\",\"max_observed_call_depth\":2,\"max_observed_loop_depth\":0,\"call_frame_crosses_epoch\":false,\"loop_frame_crosses_epoch\":false},{\"program_id\":\"gpu_full_isa_v2_full_call_return_callee_loop\",\"path\":\"profiles/gpu-xvm/conformance/full-isa-v2-full-call-return-callee-loop.json\",\"instruction_count\":9,\"static_worst_case_fuel\":15,\"expected_fuel\":15,\"required_epoch_count\":2,\"expected_executed_epoch_count\":2,\"expected_control\":\"pass\",\"expected_output_hex\":\"03000000000000000000000000000000\",\"max_observed_call_depth\":1,\"max_observed_loop_depth\":1,\"call_frame_crosses_epoch\":true,\"loop_frame_crosses_epoch\":true}]"
            L",\"shader_id\":\"xvm_full_isa_v2_full_call_return_v1\",\"shader\":\"XvmFullIsaV2FullCallReturnV1.cso\",\"shader_sha256\":\"31961ab74b5c3c62cd7adb24332e2a9c84a13b59852ed9f444e80f09255b17b9\",\"shader_source_sha256\":\"a0a8f1bb4a11276264fbbd95bc88a473e84e13083d94c8bc260d745b36fa9a47\",\"shader_bytes\":19012,\"shader_identity_status\":\"FINAL_CONTENT_PINNED\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"call_target_invalid\":\"xvm.call_target_invalid\",\"call_cycle_invalid\":\"xvm.call_cycle_invalid\",\"call_depth_exceeded\":\"xvm.call_depth_exceeded\",\"call_region_invalid\":\"xvm.call_region_invalid\",\"resident_state_invalid\":\"xvm.gpu_resident_state_invalid\",\"epoch_progress_invalid\":\"xvm.gpu_epoch_progress_invalid\",\"epoch_limit_exceeded\":\"xvm.gpu_epoch_limit_exceeded\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\",\"canceled\":\"job.canceled\"}"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping_complete\":false,\"xvm_v2_gpu_execution\":true,\"production_gpu_authority\":false,\"many_lane_admitted\":false,\"generalized_gpu_backend_complete\":false}"
            L",{\"schema_version\":\"gpu-xvm-full-isa-v2-recovered-trap-slice-v1\""
            L",\"contract_id\":\"GPU_XVM_FULL_ISA_V2_RECOVERED_TRAP_SLICE_V1\""
            L",\"contract_sha256\":\"32625831d9404b55a496fcef7eed096b297f0fa7de20a8835c42711a06802257\""
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"external_live_measured\":false"
            L",\"target\":\"GPU_XVM_FULL_ISA_V2\""
            L",\"phase\":\"RECOVERED_TRAP_SLICE_V1\""
            L",\"profile_id\":\"full_isa_v2_recovered_trap_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v2\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\",\"typed_memory_v1\",\"structured_control_v2\",\"structured_control_v2_phase_b\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"loop_begin\",\"loop_end\",\"output_u32\",\"set_control\",\"call\",\"return\",\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\",\"break_if_zero\",\"continue_if_zero\",\"trap_if_zero\"]"
            L",\"excluded_opcodes\":[\"branch_if_zero\"]"
            L",\"function_subset\":{\"function_count_min\":1,\"function_count_max\":32,\"max_call_depth\":8,\"all_functions_reachable\":true,\"call_graph_acyclic\":true,\"recursion_allowed\":false,\"call_inside_active_loop_allowed\":false,\"balanced_loops_inside_callee_allowed\":true}"
            L",\"recovered_trap\":{\"opcode\":25,\"condition\":\"register_equals_zero\",\"trap_code_minimum\":1,\"trap_code_maximum\":65535,\"forward_recovery_only\":true,\"same_verified_code_region\":true,\"deterministic_recovery\":true,\"terminal_fault\":false}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words_per_lane\":128}"
            L",\"execution_plan\":{\"schema_version\":\"gpu-xvm-execution-plan-v5\",\"state_schema\":\"gpu-xvm-lane-state-v4\",\"trap_state_schema\":\"xvm-trap-state-v1\",\"trap_state_layout\":\"status_code_pc_recovery_pc_occurrence_count_u32_v1\",\"trap_status_word_offset\":78,\"trap_code_word_offset\":79,\"trap_pc_word_offset\":80,\"trap_recovery_pc_word_offset\":81,\"trap_occurrence_count_word_offset\":82,\"recovery_same_ownership_verified\":true,\"max_call_depth\":8,\"call_frame_capacity\":8,\"call_frame_layout\":\"parallel_return_pc_loop_depth_u32_v1\",\"return_pc_stack_word_offset\":60,\"call_loop_depth_stack_word_offset\":68,\"max_loop_depth\":8,\"loop_frame_capacity\":8,\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\",\"loop_begin_pc_word_offset\":86,\"loop_end_pc_word_offset\":94,\"loop_remaining_word_offset\":102,\"verified_function_regions_call_graph_and_recovery_sites_hashed\":true,\"epochs_derived_from_static_worst_case_fuel\":true}"
            L",\"lane_execution\":{\"state_schema\":\"gpu-xvm-lane-state-v4\",\"lane_parametric\":true,\"mode\":\"single_lane\",\"logical_lane_count\":1,\"max_logical_lanes\":1,\"grid_shape\":[1,1,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":false}"
            L",\"trap_state_layout\":{\"status_word_offset\":78,\"status_encoding\":{\"none\":0,\"recovered\":1},\"code_word_offset\":79,\"trap_pc_word_offset\":80,\"recovery_pc_word_offset\":81,\"occurrence_count_word_offset\":82,\"occurrence_count_type\":\"u32\"}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"epoch_instruction_budget\":8,\"max_epochs\":8,\"required_epochs_formula\":\"ceil(static_worst_case_fuel/8)\"}"
            L",\"shader_id\":\"xvm_full_isa_v2_recovered_trap_v1\",\"shader\":\"XvmFullIsaV2RecoveredTrapV1.cso\",\"shader_sha256\":\"895ab7a7d75113c744528a3bf1c31eb7d8bb056da69e1dd6ad7ff3b2c15ca0f2\",\"shader_source_sha256\":\"1c7bdbfecddf1a77acf70e39e033109a38579360f15a048cce417db7a185707a\",\"shader_bytes\":21340,\"shader_identity_status\":\"FINAL_CONTENT_PINNED\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"recovered_trap_is_functional_result\":true,\"recovered_trap_is_execution_fault\":false,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\",\"trap_state\"]"
            L",\"stable_errors\":{\"profile_id_required\":\"xvm.gpu_profile_id_required\",\"profile_id_invalid\":\"xvm.gpu_profile_id_invalid\",\"profile_not_admitted\":\"xvm.gpu_profile_not_admitted\",\"contract_mismatch\":\"xvm.gpu_contract_mismatch\",\"call_target_invalid\":\"xvm.call_target_invalid\",\"call_cycle_invalid\":\"xvm.call_cycle_invalid\",\"call_depth_exceeded\":\"xvm.call_depth_exceeded\",\"call_region_invalid\":\"xvm.call_region_invalid\",\"resident_state_invalid\":\"xvm.gpu_resident_state_invalid\",\"epoch_progress_invalid\":\"xvm.gpu_epoch_progress_invalid\",\"epoch_limit_exceeded\":\"xvm.gpu_epoch_limit_exceeded\",\"device_loss\":\"xvm.gpu_device_lost\",\"execution_failed\":\"xvm.gpu_execution_failed\",\"shader_identity_mismatch\":\"xvm.gpu_shader_identity_mismatch\",\"differential_mismatch\":\"xvm.gpu_differential_mismatch\",\"canceled\":\"job.canceled\",\"structured_control_phase_b_dependency_required\":\"xvm.structured_control_phase_b_dependency_required\",\"structured_control_phase_b_capability_required\":\"xvm.structured_control_phase_b_capability_required\",\"structured_control_recovery_invalid\":\"xvm.structured_control_recovery_invalid\"}"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping_complete\":false,\"xvm_v2_gpu_execution\":true,\"recovered_trap_admitted\":true,\"production_gpu_authority\":false,\"many_lane_admitted\":false,\"generalized_gpu_backend_complete\":false}"
            L",{\"schema_version\":\"gpu-xvm-full-isa-v2-legacy-branch-slice-v1\""
            L",\"contract_id\":\"GPU_XVM_FULL_ISA_V2_LEGACY_BRANCH_SLICE_V1\""
            L",\"contract_sha256\":\"8aaf034382afb65d1cdb74269f26493132952cd4c4c0d3ccb11445d4f1441e27\""
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"packaged_status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"external_live_measured\":false"
            L",\"target\":\"GPU_XVM_FULL_ISA_V2\""
            L",\"phase\":\"LEGACY_BRANCH_SLICE_V1\""
            L",\"profile_id\":\"full_isa_v2_legacy_branch_u32_v1\""
            L",\"backend\":\"cpu_gpu_differential\""
            L",\"request_contract_binding\":{\"profile_field\":\"gpu_profile_id\",\"contract_id_field\":\"gpu_contract_id\",\"contract_sha256_field\":\"expected_gpu_contract_sha256\",\"all_fields_required\":true}"
            L",\"isa_version\":\"xvm-v2\""
            L",\"required_capabilities\":[\"artifact_input\",\"artifact_output\",\"control_output\",\"typed_memory_v1\"]"
            L",\"forbidden_capabilities\":[\"structured_control_v2\",\"structured_control_v2_phase_b\"]"
            L",\"opcode_subset\":[\"halt\",\"move_immediate\",\"load_input_u32\",\"add_u32\",\"xor_u32\",\"multiply_u32\",\"rotate_left_u32\",\"load_memory_u32\",\"store_memory_u32\",\"equal_u32\",\"branch_if_zero\",\"output_u32\",\"set_control\",\"call\",\"return\"]"
            L",\"excluded_opcodes\":[\"loop_begin\",\"loop_end\",\"if_zero\",\"else\",\"end_if\",\"less_than_u32\",\"less_than_s32\",\"select_u32\",\"break_if_zero\",\"continue_if_zero\",\"trap_if_zero\"]"
            L",\"legacy_branch\":{\"opcode\":10,\"condition\":\"register_equals_zero\",\"direction\":\"forward_only\",\"same_verified_code_region\":true,\"loop_entry_forbidden\":true,\"structured_control_mixing_forbidden\":true,\"topology_bound_into_plan_hash\":true}"
            L",\"function_subset\":{\"function_count_min\":1,\"function_count_max\":32,\"max_call_depth\":8,\"all_functions_reachable\":true,\"call_graph_acyclic\":true,\"recursion_allowed\":false,\"loop_opcodes_allowed\":false}"
            L",\"resource_caps\":{\"fuel_limit\":64,\"memory_bytes\":64,\"output_bytes\":16,\"parameter_srv_words\":16,\"input_srv_words\":16,\"program_srv_words\":128,\"state_uav_words_per_lane\":128}"
            L",\"execution_plan\":{\"schema_version\":\"gpu-xvm-execution-plan-v6\",\"state_schema\":\"gpu-xvm-lane-state-v5\",\"verified_function_regions_call_graph_and_branch_sites_hashed\":true,\"branch_targets_forward_same_region_verified\":true,\"structured_control_mixing_forbidden\":true,\"reserved_trap_state_canonical_zero\":true,\"reserved_loop_state_canonical_zero\":true,\"max_call_depth\":8,\"call_frame_capacity\":8,\"epochs_derived_from_static_worst_case_fuel\":true}"
            L",\"lane_execution\":{\"state_schema\":\"gpu-xvm-lane-state-v5\",\"lane_parametric\":true,\"mode\":\"single_lane\",\"logical_lane_count\":1,\"max_logical_lanes\":1,\"grid_shape\":[1,1,1],\"workgroup_shape\":[1,1,1],\"many_lane_admitted\":false}"
            L",\"dispatch\":{\"api\":\"D3D11\",\"strategy\":\"gpu_resident_bounded_epoch\",\"epoch_instruction_budget\":8,\"max_epochs\":8,\"required_epochs_formula\":\"ceil(static_worst_case_fuel/8)\"}"
            L",\"shader_id\":\"xvm_full_isa_v2_legacy_branch_v1\",\"shader\":\"XvmFullIsaV2LegacyBranchV1.cso\",\"shader_sha256\":\"34ea6a3197d570b5df0fec6f94c5a748312aafd4f2686657fa7e9f0501d4d2fc\",\"shader_source_sha256\":\"b3d261f40fe2fb147b9d59b2b141393eab87f4d3184063aac09af4d4bca92326\",\"shader_bytes\":18836,\"shader_identity_status\":\"FINAL_CONTENT_PINNED\",\"shader_identity_enforced\":true"
            L",\"cpu_reference_canonical\":true,\"functional_result_separate_from_gpu_telemetry\":true,\"compared_fields\":[\"output\",\"control_token\",\"fuel_consumed\"]"
            L",\"runtime_shader_compilation\":false,\"automatic_backend_selection\":false,\"full_isa_gpu_mapping_complete\":false,\"opcode_union_with_recovered_trap_profile_covers_0_25\":true,\"xvm_v2_gpu_execution\":true,\"legacy_branch_admitted\":true,\"production_gpu_authority\":false,\"many_lane_admitted\":false,\"generalized_gpu_backend_complete\":false}"
            + std::wstring(1, L',') + WorkerGpuXvmMicrotraceV1ProfileContractJson()
            + std::wstring(1, L',') + WorkerGpuXvmMicrotracePhaseBV1ProfileContractJson()
            + std::wstring(1, L',') + WorkerGpuXvmSpmdPhaseBProfileContractJson()
            + std::wstring(1, L',') + WorkerGpuXvmSpmdGeneralizedProfileContractJson()
            + std::wstring(1, L',') + WorkerGpuXvmProvableWorldsProfileContractJson() + L"]";
    }
}
