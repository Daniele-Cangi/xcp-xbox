#include "pch.h"
#include "WorkerGpuXvmFullIsaV2Profiles.h"

#include "WorkerXvmIsa.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* TypedStructuredContractSchemaVersion =
            L"gpu-xvm-full-isa-v2-typed-structured-slice-v1";
        constexpr wchar_t const* TypedStructuredContractId =
            L"GPU_XVM_FULL_ISA_V2_TYPED_STRUCTURED_SLICE_V1";
        constexpr wchar_t const* TypedStructuredContractSha256 =
            L"580e41cab4530ec0a96cef46f5befd4abd0ede6f467abd99d64d01a9b9947ed6";
        constexpr wchar_t const* TypedStructuredProfileId =
            L"full_isa_v2_typed_structured_u32_v1";
        constexpr wchar_t const* TypedStructuredShaderName =
            L"XvmFullIsaV2TypedStructuredV1.cso";
        constexpr wchar_t const* TypedStructuredShaderSha256 =
            L"2d99aaf0d25aaadd79291b03b1518493ba5fecfba2876342824cf30356df8a94";
        constexpr wchar_t const* TypedStructuredShaderSourceSha256 =
            L"0c852477adfa2c08164495c915caa4b6136cc59e9e514e72a0927f316c35bf46";

        constexpr wchar_t const* LoopControlContractSchemaVersion =
            L"gpu-xvm-full-isa-v2-loop-control-slice-v1";
        constexpr wchar_t const* LoopControlContractId =
            L"GPU_XVM_FULL_ISA_V2_LOOP_CONTROL_SLICE_V1";
        constexpr wchar_t const* LoopControlContractSha256 =
            L"22da98c9b957dfdcca0146e985d1f71f1ae45d0e4d74e663ce145c3588c28cbe";
        constexpr wchar_t const* LoopControlProfileId =
            L"full_isa_v2_loop_control_u32_v1";
        constexpr wchar_t const* LoopControlShaderName =
            L"XvmFullIsaV2LoopControlV1.cso";
        constexpr wchar_t const* LoopControlShaderSha256 =
            L"cd804ef8ca4aec340f78fb5562d8543cb77185093dade1c1713b28856cb64495";
        constexpr wchar_t const* LoopControlShaderSourceSha256 =
            L"2890ee49e7fa71c1d1bbfffac7759106d3f2e8dbb0dda5d50424742392f1a891";

        constexpr wchar_t const* FullCallReturnContractSchemaVersion =
            L"gpu-xvm-full-isa-v2-full-call-return-slice-v1";
        constexpr wchar_t const* FullCallReturnContractId =
            L"GPU_XVM_FULL_ISA_V2_FULL_CALL_RETURN_SLICE_V1";
        constexpr wchar_t const* FullCallReturnContractSha256 =
            L"711059cfe9eb48d1de79c6a1dc01c73696d22382a297d76a9fd87956e31c6327";
        constexpr wchar_t const* FullCallReturnProfileId =
            L"full_isa_v2_full_call_return_u32_v1";
        constexpr wchar_t const* FullCallReturnShaderName =
            L"XvmFullIsaV2FullCallReturnV1.cso";
        constexpr wchar_t const* FullCallReturnShaderSha256 =
            L"31961ab74b5c3c62cd7adb24332e2a9c84a13b59852ed9f444e80f09255b17b9";
        constexpr wchar_t const* FullCallReturnShaderSourceSha256 =
            L"a0a8f1bb4a11276264fbbd95bc88a473e84e13083d94c8bc260d745b36fa9a47";

        constexpr wchar_t const* RecoveredTrapContractSchemaVersion =
            L"gpu-xvm-full-isa-v2-recovered-trap-slice-v1";
        constexpr wchar_t const* RecoveredTrapContractId =
            L"GPU_XVM_FULL_ISA_V2_RECOVERED_TRAP_SLICE_V1";
        constexpr wchar_t const* RecoveredTrapContractSha256 =
            L"32625831d9404b55a496fcef7eed096b297f0fa7de20a8835c42711a06802257";
        constexpr wchar_t const* RecoveredTrapProfileId =
            L"full_isa_v2_recovered_trap_u32_v1";
        constexpr wchar_t const* RecoveredTrapShaderName =
            L"XvmFullIsaV2RecoveredTrapV1.cso";
        constexpr wchar_t const* RecoveredTrapShaderSha256 =
            L"895ab7a7d75113c744528a3bf1c31eb7d8bb056da69e1dd6ad7ff3b2c15ca0f2";
        constexpr wchar_t const* RecoveredTrapShaderSourceSha256 =
            L"1c7bdbfecddf1a77acf70e39e033109a38579360f15a048cce417db7a185707a";

        constexpr wchar_t const* LegacyBranchContractSchemaVersion =
            L"gpu-xvm-full-isa-v2-legacy-branch-slice-v1";
        constexpr wchar_t const* LegacyBranchContractId =
            L"GPU_XVM_FULL_ISA_V2_LEGACY_BRANCH_SLICE_V1";
        constexpr wchar_t const* LegacyBranchContractSha256 =
            L"8aaf034382afb65d1cdb74269f26493132952cd4c4c0d3ccb11445d4f1441e27";
        constexpr wchar_t const* LegacyBranchProfileId =
            L"full_isa_v2_legacy_branch_u32_v1";
        constexpr wchar_t const* LegacyBranchShaderName =
            L"XvmFullIsaV2LegacyBranchV1.cso";
        constexpr wchar_t const* LegacyBranchShaderSha256 =
            L"34ea6a3197d570b5df0fec6f94c5a748312aafd4f2686657fa7e9f0501d4d2fc";
        constexpr wchar_t const* LegacyBranchShaderSourceSha256 =
            L"b3d261f40fe2fb147b9d59b2b141393eab87f4d3184063aac09af4d4bca92326";

        constexpr WorkerGpuXvmProfileDescriptor TypedStructuredProfile
        {
            WorkerGpuXvmProfileKind::FullIsaV2TypedStructuredU32V1,
            TypedStructuredContractSchemaVersion,
            TypedStructuredContractId,
            TypedStructuredContractSha256,
            TypedStructuredProfileId,
            TypedStructuredShaderName,
            TypedStructuredShaderSha256,
            TypedStructuredShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_full_isa_v2_typed_structured_slice_v1",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmFullIsaV2ParameterWordsValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmFullIsaV2ProgramWordsValue,
            WorkerGpuXvmFullIsaV2StateWordsPerLaneValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue,
            WorkerGpuXvmFullIsaV2MaxEpochsValue,
            true,
            true,
            true,
            false,
            true,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmFullIsaV2StateWordsPerLaneValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmCapabilityTypedMemoryValue |
                WorkerGpuXvmCapabilityStructuredControlValue |
                WorkerGpuXvmCapabilityStructuredControlPhaseBValue,
            1,
            L"gpu-xvm-execution-plan-v2",
            L"gpu-xvm-lane-state-v2",
            0,
        };

        constexpr WorkerGpuXvmProfileDescriptor LoopControlProfile
        {
            WorkerGpuXvmProfileKind::FullIsaV2LoopControlU32V1,
            LoopControlContractSchemaVersion,
            LoopControlContractId,
            LoopControlContractSha256,
            LoopControlProfileId,
            LoopControlShaderName,
            LoopControlShaderSha256,
            LoopControlShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_full_isa_v2_loop_control_slice_v1",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmFullIsaV2ParameterWordsValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmFullIsaV2ProgramWordsValue,
            WorkerGpuXvmFullIsaV2LoopControlStateWordsPerLaneValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue,
            WorkerGpuXvmFullIsaV2MaxEpochsValue,
            false,
            true,
            true,
            false,
            true,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmFullIsaV2LoopControlStateWordsPerLaneValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmCapabilityTypedMemoryValue |
                WorkerGpuXvmCapabilityStructuredControlValue |
                WorkerGpuXvmCapabilityStructuredControlPhaseBValue,
            1,
            L"gpu-xvm-execution-plan-v3",
            L"gpu-xvm-lane-state-v3",
            WorkerGpuXvmFullIsaV2LoopControlMaxDepthValue,
        };

        constexpr WorkerGpuXvmProfileDescriptor FullCallReturnProfile
        {
            WorkerGpuXvmProfileKind::FullIsaV2FullCallReturnU32V1,
            FullCallReturnContractSchemaVersion,
            FullCallReturnContractId,
            FullCallReturnContractSha256,
            FullCallReturnProfileId,
            FullCallReturnShaderName,
            FullCallReturnShaderSha256,
            FullCallReturnShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_full_isa_v2_full_call_return_slice_v1",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmFullIsaV2ParameterWordsValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmFullIsaV2ProgramWordsValue,
            WorkerGpuXvmFullIsaV2FullCallReturnStateWordsPerLaneValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue,
            WorkerGpuXvmFullIsaV2MaxEpochsValue,
            false,
            true,
            true,
            false,
            true,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmFullIsaV2FullCallReturnStateWordsPerLaneValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmCapabilityTypedMemoryValue |
                WorkerGpuXvmCapabilityStructuredControlValue |
                WorkerGpuXvmCapabilityStructuredControlPhaseBValue,
            WorkerGpuXvmFullIsaV2FullCallReturnMaxDepthValue,
            L"gpu-xvm-execution-plan-v4",
            L"gpu-xvm-lane-state-v3",
            WorkerGpuXvmFullIsaV2FullCallReturnMaxDepthValue,
        };

        constexpr WorkerGpuXvmProfileDescriptor RecoveredTrapProfile
        {
            WorkerGpuXvmProfileKind::FullIsaV2RecoveredTrapU32V1,
            RecoveredTrapContractSchemaVersion,
            RecoveredTrapContractId,
            RecoveredTrapContractSha256,
            RecoveredTrapProfileId,
            RecoveredTrapShaderName,
            RecoveredTrapShaderSha256,
            RecoveredTrapShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_full_isa_v2_recovered_trap_slice_v1",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmFullIsaV2ParameterWordsValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmFullIsaV2ProgramWordsValue,
            WorkerGpuXvmFullIsaV2RecoveredTrapStateWordsPerLaneValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue,
            WorkerGpuXvmFullIsaV2MaxEpochsValue,
            false,
            true,
            true,
            false,
            true,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmFullIsaV2RecoveredTrapStateWordsPerLaneValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmCapabilityTypedMemoryValue |
                WorkerGpuXvmCapabilityStructuredControlValue |
                WorkerGpuXvmCapabilityStructuredControlPhaseBValue,
            WorkerGpuXvmFullIsaV2RecoveredTrapMaxDepthValue,
            L"gpu-xvm-execution-plan-v5",
            L"gpu-xvm-lane-state-v4",
            WorkerGpuXvmFullIsaV2RecoveredTrapMaxDepthValue,
        };

        constexpr WorkerGpuXvmProfileDescriptor LegacyBranchProfile
        {
            WorkerGpuXvmProfileKind::FullIsaV2LegacyBranchU32V1,
            LegacyBranchContractSchemaVersion,
            LegacyBranchContractId,
            LegacyBranchContractSha256,
            LegacyBranchProfileId,
            LegacyBranchShaderName,
            LegacyBranchShaderSha256,
            LegacyBranchShaderSourceSha256,
            L"xvm_integer_deterministic_gpu_full_isa_v2_legacy_branch_slice_v1",
            WorkerGpuXvmGeneralMaxInstructionsValue,
            WorkerGpuXvmGeneralMemoryBytesValue,
            WorkerGpuXvmGeneralOutputBytesValue,
            WorkerGpuXvmGeneralFuelLimitValue,
            WorkerGpuXvmFullIsaV2ParameterWordsValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmFullIsaV2ProgramWordsValue,
            WorkerGpuXvmFullIsaV2LegacyBranchStateWordsPerLaneValue,
            WorkerGpuXvmGeneralResultOutputWordOffsetValue,
            true,
            false,
            true,
            WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue,
            WorkerGpuXvmFullIsaV2MaxEpochsValue,
            false,
            true,
            true,
            false,
            true,
            1,
            { 1, 1, 1 },
            { 1, 1, 1 },
            WorkerGpuXvmFullIsaV2LegacyBranchStateWordsPerLaneValue,
            WorkerGpuXvmFullIsaV2InputWordsValue,
            WorkerGpuXvmCapabilityTypedMemoryValue,
            WorkerGpuXvmFullIsaV2LegacyBranchMaxDepthValue,
            L"gpu-xvm-execution-plan-v6",
            L"gpu-xvm-lane-state-v5",
            0,
        };

        bool HasExactLegacyBranchCapabilities(std::vector<std::wstring> const& capabilities)
        {
            static constexpr std::array<wchar_t const*, 4> Required =
            {
                L"artifact_input",
                L"artifact_output",
                L"control_output",
                L"typed_memory_v1",
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

        bool HasExactFullIsaV2Capabilities(std::vector<std::wstring> const& capabilities)
        {
            static constexpr std::array<wchar_t const*, 6> Required =
            {
                L"artifact_input",
                L"artifact_output",
                L"control_output",
                L"typed_memory_v1",
                L"structured_control_v2",
                L"structured_control_v2_phase_b",
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

        bool TypedStructuredOpcodeIsAdmitted(uint32_t opcode)
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
            case WorkerXvmOpcode::Call:
            case WorkerXvmOpcode::Return:
            case WorkerXvmOpcode::IfZero:
            case WorkerXvmOpcode::Else:
            case WorkerXvmOpcode::EndIf:
            case WorkerXvmOpcode::LessThanU32:
            case WorkerXvmOpcode::LessThanS32:
            case WorkerXvmOpcode::SelectU32:
                return true;
            default:
                return false;
            }
        }

        bool LoopControlOpcodeIsAdmitted(uint32_t opcode)
        {
            if (TypedStructuredOpcodeIsAdmitted(opcode))
            {
                return true;
            }
            switch (static_cast<WorkerXvmOpcode>(opcode))
            {
            case WorkerXvmOpcode::LoopBegin:
            case WorkerXvmOpcode::LoopEnd:
            case WorkerXvmOpcode::BreakIfZero:
            case WorkerXvmOpcode::ContinueIfZero:
                return true;
            default:
                return false;
            }
        }

        bool RecoveredTrapOpcodeIsAdmitted(uint32_t opcode)
        {
            return LoopControlOpcodeIsAdmitted(opcode) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero);
        }

        bool LegacyBranchOpcodeIsAdmitted(uint32_t opcode)
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
            case WorkerXvmOpcode::BranchIfZero:
            case WorkerXvmOpcode::OutputU32:
            case WorkerXvmOpcode::SetControl:
            case WorkerXvmOpcode::Call:
            case WorkerXvmOpcode::Return:
                return true;
            default:
                return false;
            }
        }
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2TypedStructuredProfile()
    {
        return TypedStructuredProfile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2LoopControlProfile()
    {
        return LoopControlProfile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2FullCallReturnProfile()
    {
        return FullCallReturnProfile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2RecoveredTrapProfile()
    {
        return RecoveredTrapProfile;
    }

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2LegacyBranchProfile()
    {
        return LegacyBranchProfile;
    }

    WorkerGpuXvmProfileDescriptor const* WorkerFindGpuXvmFullIsaV2Profile(
        std::wstring const& profileId)
    {
        if (profileId == TypedStructuredProfile.profileId)
        {
            return &TypedStructuredProfile;
        }
        if (profileId == LoopControlProfile.profileId)
        {
            return &LoopControlProfile;
        }
        if (profileId == FullCallReturnProfile.profileId)
        {
            return &FullCallReturnProfile;
        }
        if (profileId == RecoveredTrapProfile.profileId)
        {
            return &RecoveredTrapProfile;
        }
        if (profileId == LegacyBranchProfile.profileId)
        {
            return &LegacyBranchProfile;
        }
        return nullptr;
    }

    bool WorkerGpuXvmFullIsaV2ProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits)
    {
        auto recoveredTrapProfile =
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2RecoveredTrapU32V1;
        auto legacyBranchProfile =
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2LegacyBranchU32V1;
        auto fullCallReturnProfile =
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2FullCallReturnU32V1 ||
            recoveredTrapProfile || legacyBranchProfile;
        auto fullIsaProfile = profile.kind == WorkerGpuXvmProfileKind::FullIsaV2TypedStructuredU32V1 ||
            profile.kind == WorkerGpuXvmProfileKind::FullIsaV2LoopControlU32V1 ||
            fullCallReturnProfile;
        auto baseContractMatches = fullIsaProfile &&
            program.schemaVersion == L"xvm-program-artifact-v2" &&
            program.isaVersion == L"xvm-v2" &&
            program.artifactKind == L"xvm-program-v2" &&
            program.profile == L"integer-deterministic-v2" &&
            (legacyBranchProfile
                ? HasExactLegacyBranchCapabilities(program.capabilities)
                : HasExactFullIsaV2Capabilities(program.capabilities)) &&
            program.memoryBytes == profile.memoryBytes &&
            program.outputBytes == profile.outputBytes &&
            program.maxFuel == profile.fuelLimit &&
            program.staticWorstCaseFuel != 0 &&
            program.staticWorstCaseFuel <= profile.fuelLimit &&
            program.maxCallDepth == profile.maxCallDepth &&
            !program.inputViews.empty() && !program.memoryViews.empty() && !program.outputViews.empty() &&
            limits.memoryBytes == profile.memoryBytes &&
            limits.outputBytes == profile.outputBytes &&
            limits.fuel == profile.fuelLimit;
        if (!baseContractMatches)
        {
            return false;
        }

        if (fullCallReturnProfile)
        {
            if (program.functions.empty() || program.functions.size() > 32)
            {
                return false;
            }
        }
        else if (!(program.functions.size() == 1))
        {
            return false;
        }

        auto instructionWords = WorkerXvmInstructionWords();
        auto instructionCount = program.words.size() / instructionWords;
        if (fullCallReturnProfile)
        {
            if ((program.words.size() % instructionWords) != 0 || instructionCount < 4 ||
                instructionCount > profile.maxInstructions || program.functions.front().entryPc == 0 ||
                program.words[(program.functions.front().entryPc - 1) * instructionWords] !=
                    static_cast<uint32_t>(WorkerXvmOpcode::Halt))
            {
                return false;
            }

            uint32_t expectedEntryPc = program.functions.front().entryPc;
            for (auto const& function : program.functions)
            {
                if (function.entryPc != expectedEntryPc || function.entryPc > function.endPc ||
                    function.endPc >= instructionCount ||
                    program.words[function.endPc * instructionWords] !=
                        static_cast<uint32_t>(WorkerXvmOpcode::Return))
                {
                    return false;
                }
                expectedEntryPc = function.endPc + 1;
            }
            if (expectedEntryPc != instructionCount)
            {
                return false;
            }

            uint32_t callCount = 0;
            uint32_t trapCount = 0;
            uint32_t branchCount = 0;
            for (size_t pc = 0; pc < instructionCount; ++pc)
            {
                auto opcode = program.words[pc * instructionWords];
                if (!(legacyBranchProfile
                        ? LegacyBranchOpcodeIsAdmitted(opcode)
                        : (recoveredTrapProfile
                        ? RecoveredTrapOpcodeIsAdmitted(opcode)
                        : LoopControlOpcodeIsAdmitted(opcode))))
                {
                    return false;
                }
                if (opcode == static_cast<uint32_t>(WorkerXvmOpcode::Call))
                {
                    ++callCount;
                }
                if (opcode == static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero))
                {
                    ++trapCount;
                }
                if (opcode == static_cast<uint32_t>(WorkerXvmOpcode::BranchIfZero))
                {
                    ++branchCount;
                }
            }
            return callCount != 0 && (!recoveredTrapProfile || trapCount != 0) &&
                (!legacyBranchProfile || branchCount != 0);
        }

        auto const& function = program.functions.front();
        if ((program.words.size() % instructionWords) != 0 || instructionCount < 4 ||
            instructionCount > profile.maxInstructions || function.entryPc == 0 ||
            function.endPc + 1 != instructionCount ||
            program.words[(function.entryPc - 1) * instructionWords] != static_cast<uint32_t>(WorkerXvmOpcode::Halt) ||
            program.words[function.endPc * instructionWords] != static_cast<uint32_t>(WorkerXvmOpcode::Return))
        {
            return false;
        }

        auto opcodeIsAdmitted = profile.kind == WorkerGpuXvmProfileKind::FullIsaV2LoopControlU32V1
            ? LoopControlOpcodeIsAdmitted
            : TypedStructuredOpcodeIsAdmitted;
        uint32_t callCount = 0;
        for (size_t pc = 0; pc < instructionCount; ++pc)
        {
            auto opcode = program.words[pc * instructionWords];
            if (!opcodeIsAdmitted(opcode))
            {
                return false;
            }
            if (opcode == static_cast<uint32_t>(WorkerXvmOpcode::Call))
            {
                if (pc >= function.entryPc || program.words[pc * instructionWords + 1] != function.entryPc)
                {
                    return false;
                }
                ++callCount;
            }
            if (pc >= function.entryPc && opcode == static_cast<uint32_t>(WorkerXvmOpcode::Call))
            {
                return false;
            }
        }
        return callCount != 0;
    }
}
