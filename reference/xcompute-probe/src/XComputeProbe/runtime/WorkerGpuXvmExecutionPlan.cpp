#include "pch.h"
#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerXvmIsa.h"
#include "../ProbeResult.h"

#include <algorithm>
#include <functional>
#include <limits>

namespace XComputeProbe
{
    namespace
    {
        uint64_t CheckedProduct(std::array<uint32_t, 3> const& value)
        {
            uint64_t product = 1;
            for (auto factor : value)
            {
                if (factor == 0 || product > (std::numeric_limits<uint64_t>::max)() / factor)
                {
                    throw WorkerXvmError("xvm.gpu_profile_not_admitted", "GPU XVM shape is zero or overflows the immutable execution plan");
                }
                product *= factor;
            }
            return product;
        }

        std::wstring ShapeJson(std::array<uint32_t, 3> const& value)
        {
            return L"[" + std::to_wstring(value[0]) + L"," +
                std::to_wstring(value[1]) + L"," + std::to_wstring(value[2]) + L"]";
        }

        std::wstring U32ArrayJson(std::vector<uint32_t> const& values)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                result += std::to_wstring(values[index]);
            }
            return result + L"]";
        }

        std::wstring HashU32Words(std::vector<uint32_t> const& words)
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(words.size() * sizeof(uint32_t));
            for (auto word : words)
            {
                bytes.push_back(static_cast<uint8_t>(word & 0xffu));
                bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xffu));
                bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xffu));
                bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xffu));
            }
            return WorkerGpuXvmHashBytes(bytes);
        }

        enum : uint32_t
        {
            MicrotracePhaseBRecordSingle = 0,
            MicrotracePhaseBRecordPair = 1,
            MicrotracePhaseBRecordConstantFoldPair = 2,
            MicrotracePhaseBFlagConstantPropagated = 1,
        };

        uint32_t ProgramOpcode(WorkerXvmProgram const& program, uint32_t pc)
        {
            return program.words[static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue];
        }

        bool IsPhaseBAluOpcode(uint32_t opcode)
        {
            return opcode == static_cast<uint32_t>(WorkerXvmOpcode::MoveImmediate) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::AddU32) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::XorU32) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::MultiplyU32) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::RotateLeftU32) ||
                opcode == static_cast<uint32_t>(WorkerXvmOpcode::EqualU32);
        }

        bool PhaseBConstantFoldPair(
            WorkerXvmProgram const& program,
            uint32_t pc,
            uint32_t endPcExclusive,
            uint32_t& foldedValue,
            uint32_t& foldedSelector)
        {
            if (pc + 1 >= endPcExclusive)
            {
                return false;
            }
            auto first = static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue;
            auto second = first + WorkerXvmInstructionWordsValue;
            if (program.words[first] != static_cast<uint32_t>(WorkerXvmOpcode::MoveImmediate) ||
                program.words[first + 1] >= WorkerXvmRegisterCountValue || program.words[first + 3] != 0)
            {
                return false;
            }
            auto constantRegister = program.words[first + 1];
            auto constantValue = program.words[first + 2];
            auto opcode = program.words[second];
            if (opcode == static_cast<uint32_t>(WorkerXvmOpcode::RotateLeftU32) &&
                program.words[second + 2] == constantRegister && program.words[second + 3] < 32)
            {
                auto shift = program.words[second + 3];
                foldedValue = shift == 0
                    ? constantValue
                    : ((constantValue << shift) | (constantValue >> (32 - shift)));
                foldedSelector = 3;
                return true;
            }
            if (opcode != static_cast<uint32_t>(WorkerXvmOpcode::AddU32) &&
                opcode != static_cast<uint32_t>(WorkerXvmOpcode::XorU32) &&
                opcode != static_cast<uint32_t>(WorkerXvmOpcode::MultiplyU32) &&
                opcode != static_cast<uint32_t>(WorkerXvmOpcode::EqualU32))
            {
                return false;
            }
            if (program.words[second + 2] == constantRegister)
            {
                foldedValue = constantValue;
                foldedSelector = 1;
                return true;
            }
            if (program.words[second + 3] == constantRegister)
            {
                foldedValue = constantValue;
                foldedSelector = 2;
                return true;
            }
            return false;
        }

        void CopyPhaseBInstruction(
            WorkerXvmProgram const& program,
            uint32_t pc,
            std::vector<uint32_t>& words,
            size_t destination)
        {
            auto source = static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue;
            for (size_t index = 0; index < WorkerXvmInstructionWordsValue; ++index)
            {
                words[destination + index] = program.words[source + index];
            }
        }

        void BuildPhaseBMicrotrace(WorkerGpuXvmExecutionPlan& plan, WorkerXvmProgram const& program)
        {
            constexpr uint32_t wordsPerRecord = 16;
            plan.microtraceSchemaVersion = L"xvm-gpu-microtrace-record-v2";
            plan.microtraceWordsPerOp = wordsPerRecord;
            plan.microtraceFuelDebit = plan.instructionCount;
            plan.microtraceBasicBlockCount = 1;
            plan.microtraceWords.assign(
                static_cast<size_t>(plan.instructionCount) * wordsPerRecord, 0);
            plan.microtraceSourcePcMap.reserve(plan.instructionCount);
            for (uint32_t pc = 0; pc < plan.instructionCount; ++pc)
            {
                plan.microtraceSourcePcMap.push_back(pc);
            }

            for (uint32_t superblockPc = 0; superblockPc < plan.instructionCount;
                 superblockPc += plan.epochInstructionBudget)
            {
                auto endPcExclusive = (std::min)(
                    plan.instructionCount, superblockPc + plan.epochInstructionBudget);
                plan.microtraceSuperblockSourcePcMap.push_back(superblockPc);
                plan.microtraceSuperblockSourceSpanMap.push_back(endPcExclusive - superblockPc);
                ++plan.microtraceSuperblockCount;

                for (uint32_t pc = superblockPc; pc < endPcExclusive;)
                {
                    uint32_t foldedValue = 0;
                    uint32_t foldedSelector = 0;
                    auto foldCurrent = PhaseBConstantFoldPair(
                        program, pc, endPcExclusive, foldedValue, foldedSelector);
                    uint32_t ignoredValue = 0;
                    uint32_t ignoredSelector = 0;
                    auto foldNext = pc + 1 < endPcExclusive && PhaseBConstantFoldPair(
                        program, pc + 1, endPcExclusive, ignoredValue, ignoredSelector);
                    auto firstOpcode = ProgramOpcode(program, pc);
                    auto secondOpcode = pc + 1 < endPcExclusive
                        ? ProgramOpcode(program, pc + 1)
                        : static_cast<uint32_t>(WorkerXvmOpcode::Halt);
                    auto genericPair = !foldCurrent && !foldNext && pc + 1 < endPcExclusive &&
                        firstOpcode != static_cast<uint32_t>(WorkerXvmOpcode::Halt) &&
                        secondOpcode != static_cast<uint32_t>(WorkerXvmOpcode::Halt);
                    auto span = (foldCurrent || genericPair) ? 2u : 1u;
                    auto kind = foldCurrent
                        ? MicrotracePhaseBRecordConstantFoldPair
                        : (genericPair ? MicrotracePhaseBRecordPair : MicrotracePhaseBRecordSingle);
                    auto base = static_cast<size_t>(pc) * wordsPerRecord;
                    plan.microtraceWords[base] = kind;
                    plan.microtraceWords[base + 1] = pc;
                    plan.microtraceWords[base + 2] = span;
                    plan.microtraceWords[base + 3] = span;
                    CopyPhaseBInstruction(program, pc, plan.microtraceWords, base + 4);
                    if (span == 2)
                    {
                        CopyPhaseBInstruction(program, pc + 1, plan.microtraceWords, base + 8);
                        ++plan.microtraceFusedRecordCount;
                        if (IsPhaseBAluOpcode(firstOpcode) && IsPhaseBAluOpcode(secondOpcode))
                        {
                            ++plan.microtraceAluFusionRecordCount;
                        }
                    }
                    if (foldCurrent)
                    {
                        plan.microtraceWords[base + 12] = foldedValue;
                        plan.microtraceWords[base + 13] = foldedSelector;
                        plan.microtraceWords[base + 14] = MicrotracePhaseBFlagConstantPropagated;
                        ++plan.microtraceConstantFoldRecordCount;
                    }
                    plan.microtraceRecordSourcePcMap.push_back(pc);
                    plan.microtraceRecordSourceSpanMap.push_back(span);
                    plan.microtraceRecordKindMap.push_back(kind);
                    ++plan.microtraceOpCount;
                    pc += span;
                }
            }
            if (plan.microtraceOpCount == 0 || plan.microtraceOpCount >= plan.instructionCount ||
                plan.microtraceFusedRecordCount == 0 || plan.microtraceConstantFoldRecordCount == 0 ||
                plan.microtraceRecordSourcePcMap.size() != plan.microtraceOpCount ||
                plan.microtraceRecordSourceSpanMap.size() != plan.microtraceOpCount ||
                plan.microtraceRecordKindMap.size() != plan.microtraceOpCount)
            {
                throw WorkerXvmError(
                    "xvm.microtrace_plan_invalid",
                    "Phase B microtrace did not produce a bounded optimized record plan");
            }
            plan.microtraceExpansionNumerator = plan.microtraceOpCount;
            plan.microtraceExpansionDenominator = plan.instructionCount;
            plan.microtraceSha256 = HashU32Words(plan.microtraceWords);
        }

        std::wstring VerifiedCodeRegionsJson(WorkerGpuXvmExecutionPlan const& plan)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < plan.verifiedCodeRegions.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                auto const& region = plan.verifiedCodeRegions[index];
                result += L"{\"region_index\":" + std::to_wstring(region.regionIndex) +
                    L",\"begin_pc\":" + std::to_wstring(region.beginPc) +
                    L",\"end_pc\":" + std::to_wstring(region.endPc) +
                    L",\"main_region\":" + (region.mainRegion ? L"true" : L"false") + L"}";
            }
            return result + L"]";
        }

        std::wstring VerifiedCallSitesJson(WorkerGpuXvmExecutionPlan const& plan)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < plan.verifiedCallSites.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                auto const& call = plan.verifiedCallSites[index];
                result += L"{\"call_pc\":" + std::to_wstring(call.callPc) +
                    L",\"caller_region_index\":" + std::to_wstring(call.callerRegionIndex) +
                    L",\"target_entry_pc\":" + std::to_wstring(call.targetEntryPc) +
                    L",\"target_region_index\":" + std::to_wstring(call.targetRegionIndex) + L"}";
            }
            return result + L"]";
        }

        std::wstring VerifiedRecoverySitesJson(WorkerGpuXvmExecutionPlan const& plan)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < plan.verifiedRecoverySites.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                auto const& site = plan.verifiedRecoverySites[index];
                result += L"{\"trap_pc\":" + std::to_wstring(site.trapPc) +
                    L",\"trap_code\":" + std::to_wstring(site.trapCode) +
                    L",\"recovery_pc\":" + std::to_wstring(site.recoveryPc) +
                    L",\"region_index\":" + std::to_wstring(site.regionIndex) + L"}";
            }
            return result + L"]";
        }

        std::wstring VerifiedBranchSitesJson(WorkerGpuXvmExecutionPlan const& plan)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < plan.verifiedBranchSites.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                auto const& site = plan.verifiedBranchSites[index];
                result += L"{\"branch_pc\":" + std::to_wstring(site.branchPc) +
                    L",\"register_index\":" + std::to_wstring(site.registerIndex) +
                    L",\"target_pc\":" + std::to_wstring(site.targetPc) +
                    L",\"region_index\":" + std::to_wstring(site.regionIndex) + L"}";
            }
            return result + L"]";
        }

        uint32_t RegionIndexForPc(
            std::vector<WorkerGpuXvmVerifiedCodeRegion> const& regions,
            uint32_t pc)
        {
            for (auto const& region : regions)
            {
                if (pc >= region.beginPc && pc <= region.endPc)
                {
                    return region.regionIndex;
                }
            }
            throw WorkerXvmError(
                "xvm.gpu_profile_not_admitted",
                "GPU XVM v4 plan could not resolve a verified caller region");
        }

        uint32_t RegionIndexForEntry(
            std::vector<WorkerGpuXvmVerifiedCodeRegion> const& regions,
            uint32_t entryPc)
        {
            for (size_t index = 1; index < regions.size(); ++index)
            {
                if (regions[index].beginPc == entryPc)
                {
                    return regions[index].regionIndex;
                }
            }
            throw WorkerXvmError(
                "xvm.gpu_profile_not_admitted",
                "GPU XVM v4 plan call target is not a verified function entry");
        }

        void BuildVerifiedCallGraph(
            WorkerGpuXvmExecutionPlan& plan,
            WorkerXvmProgram const& program)
        {
            if (program.functions.empty() || program.functions.front().entryPc == 0)
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v4 plan requires verified main and function regions");
            }

            plan.verifiedCodeRegions.reserve(program.functions.size() + 1);
            plan.verifiedCodeRegions.push_back({ 0, 0, program.functions.front().entryPc - 1, true });
            for (size_t index = 0; index < program.functions.size(); ++index)
            {
                auto const& function = program.functions[index];
                plan.verifiedCodeRegions.push_back({
                    static_cast<uint32_t>(index + 1),
                    function.entryPc,
                    function.endPc,
                    false,
                });
            }

            std::vector<std::vector<uint32_t>> callEdges(plan.verifiedCodeRegions.size());
            auto instructionWords = WorkerXvmInstructionWords();
            for (uint32_t pc = 0; pc < plan.instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * instructionWords;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::Call))
                {
                    continue;
                }
                auto callerRegionIndex = RegionIndexForPc(plan.verifiedCodeRegions, pc);
                auto targetEntryPc = program.words[wordOffset + 1];
                auto targetRegionIndex = RegionIndexForEntry(plan.verifiedCodeRegions, targetEntryPc);
                plan.verifiedCallSites.push_back({
                    pc,
                    callerRegionIndex,
                    targetEntryPc,
                    targetRegionIndex,
                });
                auto& edges = callEdges[callerRegionIndex];
                if (std::find(edges.begin(), edges.end(), targetRegionIndex) == edges.end())
                {
                    edges.push_back(targetRegionIndex);
                }
            }
            if (plan.verifiedCallSites.empty())
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v4 plan requires at least one verified call site");
            }

            std::vector<bool> visiting(plan.verifiedCodeRegions.size(), false);
            std::vector<bool> reachable(plan.verifiedCodeRegions.size(), false);
            uint32_t actualMaxDepth = 0;
            std::function<void(uint32_t, uint32_t)> visit = [&](uint32_t regionIndex, uint32_t depth)
            {
                if (regionIndex >= callEdges.size() || visiting[regionIndex])
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_not_admitted",
                        "GPU XVM v4 plan requires an acyclic verified call graph");
                }
                visiting[regionIndex] = true;
                reachable[regionIndex] = true;
                actualMaxDepth = (std::max)(actualMaxDepth, depth);
                for (auto targetRegionIndex : callEdges[regionIndex])
                {
                    visit(targetRegionIndex, depth + 1);
                }
                visiting[regionIndex] = false;
            };
            visit(0, 0);

            if (!std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }) ||
                actualMaxDepth == 0 || actualMaxDepth > plan.maxCallDepth ||
                actualMaxDepth > plan.callFrameCapacity)
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v4 call graph reachability or actual depth is outside the immutable plan");
            }
            plan.verifiedActualMaxCallDepth = actualMaxDepth;
            plan.callGraphAllFunctionsReachable = true;
            plan.callGraphAcyclic = true;
        }

        void BuildVerifiedRecoverySites(
            WorkerGpuXvmExecutionPlan& plan,
            WorkerXvmProgram const& program)
        {
            auto instructionWords = WorkerXvmInstructionWords();
            for (uint32_t pc = 0; pc < plan.instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * instructionWords;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero))
                {
                    continue;
                }

                auto trapCode = program.words[wordOffset + 2];
                auto recoveryPc = program.words[wordOffset + 3];
                if (trapCode == 0 || trapCode > WorkerGpuXvmTrapCodeMaximumValue ||
                    recoveryPc <= pc || recoveryPc >= plan.instructionCount)
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_not_admitted",
                        "GPU XVM v5 recovered trap operands are outside the immutable plan");
                }
                auto sourceRegionIndex = RegionIndexForPc(plan.verifiedCodeRegions, pc);
                auto recoveryRegionIndex = RegionIndexForPc(plan.verifiedCodeRegions, recoveryPc);
                if (sourceRegionIndex != recoveryRegionIndex)
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_not_admitted",
                        "GPU XVM v5 recovered trap crosses verified code-region ownership");
                }
                plan.verifiedRecoverySites.push_back({
                    pc,
                    trapCode,
                    recoveryPc,
                    sourceRegionIndex,
                });
            }
            if (plan.verifiedRecoverySites.empty())
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v5 plan requires at least one verified recovered trap site");
            }
            plan.recoveredTrapStateAdmitted = true;
            plan.recoverySameOwnershipVerified = true;
        }

        void BuildVerifiedBranchSites(
            WorkerGpuXvmExecutionPlan& plan,
            WorkerXvmProgram const& program)
        {
            auto hasCapability = [&](wchar_t const* capability)
            {
                return std::find(
                    program.capabilities.begin(),
                    program.capabilities.end(),
                    capability) != program.capabilities.end();
            };
            if (hasCapability(L"structured_control_v2") ||
                hasCapability(L"structured_control_v2_phase_b"))
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v6 legacy branch plan forbids structured-control capability mixing");
            }

            auto instructionWords = WorkerXvmInstructionWords();
            for (uint32_t pc = 0; pc < plan.instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * instructionWords;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::BranchIfZero))
                {
                    continue;
                }

                auto registerIndex = program.words[wordOffset + 1];
                auto targetPc = program.words[wordOffset + 2];
                if (registerIndex >= 16 || targetPc <= pc || targetPc >= plan.instructionCount ||
                    program.words[wordOffset + 3] != 0)
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_not_admitted",
                        "GPU XVM v6 legacy branch operands are outside the immutable plan");
                }
                auto sourceRegionIndex = RegionIndexForPc(plan.verifiedCodeRegions, pc);
                auto targetRegionIndex = RegionIndexForPc(plan.verifiedCodeRegions, targetPc);
                if (sourceRegionIndex != targetRegionIndex)
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_not_admitted",
                        "GPU XVM v6 legacy branch crosses verified code-region ownership");
                }
                plan.verifiedBranchSites.push_back({
                    pc,
                    registerIndex,
                    targetPc,
                    sourceRegionIndex,
                });
            }
            if (plan.verifiedBranchSites.empty())
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v6 plan requires at least one verified legacy branch site");
            }
            plan.legacyBranchTopologyAdmitted = true;
            plan.branchTargetsForwardSameRegionVerified = true;
            plan.structuredControlMixingForbidden = true;
        }

        std::wstring BindingJsonV2(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"capability_flags\":" + std::to_wstring(plan.capabilityFlags) +
                L",\"lane_mode\":\"single_lane\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"state_schema\":\"gpu-xvm-lane-state-v2\"" +
                L",\"state_words_per_lane\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"total_state_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"input_words_per_lane\":" + std::to_wstring(plan.inputWordsPerLane) +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"many_lane_admitted\":false,\"gpu_authority\":false}";
        }

        std::wstring BindingJsonV3(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"capability_flags\":" + std::to_wstring(plan.capabilityFlags) +
                L",\"lane_mode\":\"single_lane\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words_per_lane\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"total_state_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"input_words_per_lane\":" + std::to_wstring(plan.inputWordsPerLane) +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"max_loop_depth\":" + std::to_wstring(plan.maxLoopDepth) +
                L",\"loop_frame_capacity\":" + std::to_wstring(plan.loopFrameCapacity) +
                L",\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\"" +
                L",\"loop_begin_pc_word_offset\":" + std::to_wstring(plan.loopBeginPcWordOffset) +
                L",\"loop_end_pc_word_offset\":" + std::to_wstring(plan.loopEndPcWordOffset) +
                L",\"loop_remaining_word_offset\":" + std::to_wstring(plan.loopRemainingWordOffset) +
                L",\"many_lane_admitted\":false,\"gpu_authority\":false}";
        }

        std::wstring BindingJsonV4(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"capability_flags\":" + std::to_wstring(plan.capabilityFlags) +
                L",\"lane_mode\":\"single_lane\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words_per_lane\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"total_state_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"input_words_per_lane\":" + std::to_wstring(plan.inputWordsPerLane) +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"max_loop_depth\":" + std::to_wstring(plan.maxLoopDepth) +
                L",\"loop_frame_capacity\":" + std::to_wstring(plan.loopFrameCapacity) +
                L",\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\"" +
                L",\"loop_begin_pc_word_offset\":" + std::to_wstring(plan.loopBeginPcWordOffset) +
                L",\"loop_end_pc_word_offset\":" + std::to_wstring(plan.loopEndPcWordOffset) +
                L",\"loop_remaining_word_offset\":" + std::to_wstring(plan.loopRemainingWordOffset) +
                L",\"call_frame_capacity\":" + std::to_wstring(plan.callFrameCapacity) +
                L",\"call_frame_layout\":\"parallel_return_pc_loop_depth_u32_v1\"" +
                L",\"return_pc_stack_word_offset\":" + std::to_wstring(plan.returnPcStackWordOffset) +
                L",\"call_loop_depth_stack_word_offset\":" + std::to_wstring(plan.callLoopDepthStackWordOffset) +
                L",\"verified_actual_max_call_depth\":" + std::to_wstring(plan.verifiedActualMaxCallDepth) +
                L",\"verified_code_regions\":" + VerifiedCodeRegionsJson(plan) +
                L",\"verified_call_sites\":" + VerifiedCallSitesJson(plan) +
                L",\"call_graph_all_functions_reachable\":" +
                    (plan.callGraphAllFunctionsReachable ? L"true" : L"false") +
                L",\"call_graph_acyclic\":" + (plan.callGraphAcyclic ? L"true" : L"false") +
                L",\"many_lane_admitted\":false,\"gpu_authority\":false}";
        }

        std::wstring BindingJsonV5(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"capability_flags\":" + std::to_wstring(plan.capabilityFlags) +
                L",\"lane_mode\":\"single_lane\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words_per_lane\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"total_state_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"input_words_per_lane\":" + std::to_wstring(plan.inputWordsPerLane) +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"max_loop_depth\":" + std::to_wstring(plan.maxLoopDepth) +
                L",\"loop_frame_capacity\":" + std::to_wstring(plan.loopFrameCapacity) +
                L",\"loop_frame_layout\":\"parallel_begin_end_remaining_u32_v1\"" +
                L",\"loop_begin_pc_word_offset\":" + std::to_wstring(plan.loopBeginPcWordOffset) +
                L",\"loop_end_pc_word_offset\":" + std::to_wstring(plan.loopEndPcWordOffset) +
                L",\"loop_remaining_word_offset\":" + std::to_wstring(plan.loopRemainingWordOffset) +
                L",\"call_frame_capacity\":" + std::to_wstring(plan.callFrameCapacity) +
                L",\"call_frame_layout\":\"parallel_return_pc_loop_depth_u32_v1\"" +
                L",\"return_pc_stack_word_offset\":" + std::to_wstring(plan.returnPcStackWordOffset) +
                L",\"call_loop_depth_stack_word_offset\":" + std::to_wstring(plan.callLoopDepthStackWordOffset) +
                L",\"verified_actual_max_call_depth\":" + std::to_wstring(plan.verifiedActualMaxCallDepth) +
                L",\"verified_code_regions\":" + VerifiedCodeRegionsJson(plan) +
                L",\"verified_call_sites\":" + VerifiedCallSitesJson(plan) +
                L",\"call_graph_all_functions_reachable\":" +
                    (plan.callGraphAllFunctionsReachable ? L"true" : L"false") +
                L",\"call_graph_acyclic\":" + (plan.callGraphAcyclic ? L"true" : L"false") +
                L",\"trap_state_schema\":" + JsonString(plan.trapStateSchemaVersion) +
                L",\"trap_state_layout\":\"status_code_pc_recovery_pc_occurrence_count_u32_v1\"" +
                L",\"trap_status_encoding\":{\"none\":0,\"recovered\":1}" +
                L",\"trap_status_word_offset\":" + std::to_wstring(plan.trapStatusWordOffset) +
                L",\"trap_code_word_offset\":" + std::to_wstring(plan.trapCodeWordOffset) +
                L",\"trap_pc_word_offset\":" + std::to_wstring(plan.trapPcWordOffset) +
                L",\"trap_recovery_pc_word_offset\":" + std::to_wstring(plan.trapRecoveryPcWordOffset) +
                L",\"trap_occurrence_count_word_offset\":" +
                    std::to_wstring(plan.trapOccurrenceCountWordOffset) +
                L",\"trap_code_maximum\":" + std::to_wstring(plan.trapCodeMaximum) +
                L",\"recovered_trap_state_admitted\":" +
                    (plan.recoveredTrapStateAdmitted ? L"true" : L"false") +
                L",\"recovery_same_ownership_verified\":" +
                    (plan.recoverySameOwnershipVerified ? L"true" : L"false") +
                L",\"verified_recovery_sites\":" + VerifiedRecoverySitesJson(plan) +
                L",\"many_lane_admitted\":false,\"gpu_authority\":false}";
        }

        std::wstring BindingJsonV6(WorkerGpuXvmExecutionPlan const& plan)
        {
            auto result = BindingJsonV4(plan);
            result.pop_back();
            return result +
                L",\"legacy_branch_semantics\":\"forward_if_register_zero_v1\"" +
                L",\"legacy_branch_topology_admitted\":" +
                    (plan.legacyBranchTopologyAdmitted ? L"true" : L"false") +
                L",\"branch_targets_forward_same_region_verified\":" +
                    (plan.branchTargetsForwardSameRegionVerified ? L"true" : L"false") +
                L",\"structured_control_mixing_forbidden\":" +
                    (plan.structuredControlMixingForbidden ? L"true" : L"false") +
                L",\"verified_branch_sites\":" + VerifiedBranchSitesJson(plan) +
                L",\"reserved_trap_state_canonical_zero\":true" +
                L",\"reserved_loop_state_canonical_zero\":true}";
        }

        std::wstring BindingJsonV7(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"spmd_execution_plan_sha256\":" + JsonString(plan.spmdExecutionPlanSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"capability_flags\":" + std::to_wstring(plan.capabilityFlags) +
                L",\"lane_mode\":\"spmd_many_lane_fixed_4x3_v1\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"lane_context_schema\":\"xvm-spmd-lane-context-v1\"" +
                L",\"lane_context_register_binding\":{\"lane_id\":12,\"x\":13,\"y\":14,\"z\":15}" +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words_per_lane\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"total_state_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"input_words_per_lane\":" + std::to_wstring(plan.inputWordsPerLane) +
                L",\"shared_input_access\":\"read_only_replicated\"" +
                L",\"private_state_per_lane\":true" +
                L",\"output_ownership\":\"disjoint_contiguous_slice_per_lane\"" +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"call_frame_capacity\":" + std::to_wstring(plan.callFrameCapacity) +
                L",\"return_pc_stack_word_offset\":" + std::to_wstring(plan.returnPcStackWordOffset) +
                L",\"call_loop_depth_stack_word_offset\":" + std::to_wstring(plan.callLoopDepthStackWordOffset) +
                L",\"verified_actual_max_call_depth\":" + std::to_wstring(plan.verifiedActualMaxCallDepth) +
                L",\"verified_code_regions\":" + VerifiedCodeRegionsJson(plan) +
                L",\"verified_call_sites\":" + VerifiedCallSitesJson(plan) +
                L",\"call_graph_all_functions_reachable\":" +
                    (plan.callGraphAllFunctionsReachable ? L"true" : L"false") +
                L",\"call_graph_acyclic\":" + (plan.callGraphAcyclic ? L"true" : L"false") +
                L",\"lockstep_epoch_validation\":true" +
                L",\"cpu_reference_canonical\":true" +
                L",\"many_lane_admitted\":true,\"gpu_authority\":false}";
        }
        std::wstring BindingJsonV10(WorkerGpuXvmExecutionPlan const& plan)
        {
            auto binding = BindingJsonV7(plan);
            auto marker = std::wstring(L",\"lane_mode\":\"spmd_many_lane_fixed_4x3_v1\"");
            auto replacement = std::wstring(L",\"lane_mode\":\"spmd_many_lane_generalized_topology_v1\"") +
                L",\"dispatch_group_shape\":" + ShapeJson(plan.dispatchGroupShape) +
                L",\"workgroup_thread_count\":" + std::to_wstring(plan.workgroupThreadCount) +
                L",\"partial_group_threads_guarded\":true" +
                L",\"generalized_topology_admitted\":true";
            auto position = binding.find(marker);
            if (position == std::wstring::npos)
            {
                throw WorkerXvmError("xvm.gpu_profile_not_admitted", "GPU XVM generalized binding marker is missing");
            }
            binding.replace(position, marker.size(), replacement);
            return binding;
        }
        std::wstring BindingJsonV11(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"spmd_execution_plan_sha256\":" + JsonString(plan.spmdExecutionPlanSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"lane_mode\":\"provable_worlds_1024x1024_v1\"" +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"dispatch_group_shape\":" + ShapeJson(plan.dispatchGroupShape) +
                L",\"workgroup_thread_count\":" + std::to_wstring(plan.workgroupThreadCount) +
                L",\"field_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"field_words\":" + std::to_wstring(plan.totalStateWords) +
                L",\"field_format\":\"u32_le\"" +
                L",\"max_call_depth\":" + std::to_wstring(plan.maxCallDepth) +
                L",\"call_frame_capacity\":" + std::to_wstring(plan.callFrameCapacity) +
                L",\"verified_actual_max_call_depth\":" + std::to_wstring(plan.verifiedActualMaxCallDepth) +
                L",\"verified_code_regions\":" + VerifiedCodeRegionsJson(plan) +
                L",\"verified_call_sites\":" + VerifiedCallSitesJson(plan) +
                L",\"call_graph_all_functions_reachable\":" + (plan.callGraphAllFunctionsReachable ? L"true" : L"false") +
                L",\"call_graph_acyclic\":" + (plan.callGraphAcyclic ? L"true" : L"false") +
                L",\"single_logical_dispatch\":true" +
                L",\"worker_revalidation_required\":true" +
                L",\"assurance_class\":\"exact_cpu_gpu_differential_v1\"" +
                L",\"artifact_lifecycle\":[\"generated_provisional\",\"verification_pending\",\"verified\",\"quarantined\"]" +
                L",\"cpu_reference_canonical\":true" +
                L",\"runtime_shader_compilation\":false" +
                L",\"gpu_authority\":false}";
        }
        std::wstring BindingJsonV8(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"microtrace_admitted\":true" +
                L",\"microtrace_schema\":" + JsonString(plan.microtraceSchemaVersion) +
                L",\"microtrace_sha256\":" + JsonString(plan.microtraceSha256) +
                L",\"microtrace_words_per_op\":" + std::to_wstring(plan.microtraceWordsPerOp) +
                L",\"microtrace_op_count\":" + std::to_wstring(plan.microtraceOpCount) +
                L",\"microtrace_fuel_debit\":" + std::to_wstring(plan.microtraceFuelDebit) +
                L",\"microtrace_expansion_numerator\":" + std::to_wstring(plan.microtraceExpansionNumerator) +
                L",\"microtrace_expansion_denominator\":" + std::to_wstring(plan.microtraceExpansionDenominator) +
                L",\"source_pc_map\":" + U32ArrayJson(plan.microtraceSourcePcMap) +
                L",\"program_srv_crosscheck_required\":true" +
                L",\"worker_revalidation_required\":true" +
                L",\"runtime_shader_compilation\":false" +
                L",\"emits_executable_code\":false" +
                L",\"gpu_authority\":false}";
        }

        std::wstring BindingJsonV9(WorkerGpuXvmExecutionPlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"contract_sha256\":" + JsonString(plan.contractSha256) +
                L",\"shader_sha256\":" + JsonString(plan.shaderSha256) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"instruction_count\":" + std::to_wstring(plan.instructionCount) +
                L",\"static_worst_case_fuel\":" + std::to_wstring(plan.staticWorstCaseFuel) +
                L",\"epoch_instruction_budget\":" + std::to_wstring(plan.epochInstructionBudget) +
                L",\"required_epoch_count\":" + std::to_wstring(plan.requiredEpochCount) +
                L",\"max_epochs\":" + std::to_wstring(plan.maxEpochs) +
                L",\"state_schema\":" + JsonString(plan.stateSchemaVersion) +
                L",\"state_words\":" + std::to_wstring(plan.stateWordsPerLane) +
                L",\"microtrace_admitted\":true" +
                L",\"microtrace_schema\":" + JsonString(plan.microtraceSchemaVersion) +
                L",\"microtrace_sha256\":" + JsonString(plan.microtraceSha256) +
                L",\"microtrace_words_per_op\":" + std::to_wstring(plan.microtraceWordsPerOp) +
                L",\"microtrace_srv_words\":" + std::to_wstring(plan.microtraceWords.size()) +
                L",\"microtrace_op_count\":" + std::to_wstring(plan.microtraceOpCount) +
                L",\"microtrace_fuel_debit\":" + std::to_wstring(plan.microtraceFuelDebit) +
                L",\"microtrace_expansion_numerator\":" + std::to_wstring(plan.microtraceExpansionNumerator) +
                L",\"microtrace_expansion_denominator\":" + std::to_wstring(plan.microtraceExpansionDenominator) +
                L",\"basic_block_count\":" + std::to_wstring(plan.microtraceBasicBlockCount) +
                L",\"superblock_count\":" + std::to_wstring(plan.microtraceSuperblockCount) +
                L",\"fused_record_count\":" + std::to_wstring(plan.microtraceFusedRecordCount) +
                L",\"constant_fold_record_count\":" + std::to_wstring(plan.microtraceConstantFoldRecordCount) +
                L",\"alu_fusion_record_count\":" + std::to_wstring(plan.microtraceAluFusionRecordCount) +
                L",\"source_pc_map\":" + U32ArrayJson(plan.microtraceSourcePcMap) +
                L",\"record_source_pc_map\":" + U32ArrayJson(plan.microtraceRecordSourcePcMap) +
                L",\"record_source_span_map\":" + U32ArrayJson(plan.microtraceRecordSourceSpanMap) +
                L",\"record_kind_map\":" + U32ArrayJson(plan.microtraceRecordKindMap) +
                L",\"superblock_source_pc_map\":" + U32ArrayJson(plan.microtraceSuperblockSourcePcMap) +
                L",\"superblock_source_span_map\":" + U32ArrayJson(plan.microtraceSuperblockSourceSpanMap) +
                L",\"program_srv_crosscheck_required\":true" +
                L",\"worker_revalidation_required\":true" +
                L",\"runtime_shader_compilation\":false" +
                L",\"emits_executable_code\":false" +
                L",\"gpu_authority\":false}";
        }

        std::wstring BindingJson(WorkerGpuXvmExecutionPlan const& plan)
        {
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV11)
            {
                return BindingJsonV11(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV10)
            {
                return BindingJsonV10(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9)
            {
                return BindingJsonV9(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV8)
            {
                return BindingJsonV8(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV7)
            {
                return BindingJsonV7(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6)
            {
                return BindingJsonV6(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5)
            {
                return BindingJsonV5(plan);
            }
            if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV4)
            {
                return BindingJsonV4(plan);
            }
            return plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV3
                ? BindingJsonV3(plan)
                : BindingJsonV2(plan);
        }
    }

    WorkerGpuXvmExecutionPlan WorkerBuildGpuXvmExecutionPlan(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        std::wstring const& programSha256,
        std::wstring const& spmdExecutionPlanSha256,
        uint32_t spmdLaneCount,
        std::array<uint32_t, 3> const& spmdGridShape,
        std::array<uint32_t, 3> const& spmdWorkgroupShape)
    {
        WorkerGpuXvmExecutionPlan plan;
        if (profile.kind == WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1)
        {
            if (spmdLaneCount != 1048576 ||
                spmdGridShape != std::array<uint32_t, 3>{ 1024, 1024, 1 } ||
                spmdWorkgroupShape != std::array<uint32_t, 3>{ 8, 8, 1 } ||
                spmdExecutionPlanSha256.size() != 64 ||
                program.staticWorstCaseFuel != 16 ||
                profile.executionPlanSchemaVersion == nullptr ||
                std::wstring(profile.executionPlanSchemaVersion) != WorkerGpuXvmExecutionPlanSchemaVersionV11)
            {
                throw WorkerXvmError("xvm.gpu_profile_not_admitted", "PROVABLE_WORLDS_V1 requires its exact admitted 1024x1024 plan");
            }
            plan.admitted = true;
            plan.schemaVersion = profile.executionPlanSchemaVersion;
            plan.profileId = profile.profileId;
            plan.contractId = profile.contractId;
            plan.contractSha256 = profile.contractSha256;
            plan.shaderSha256 = profile.shaderSha256;
            plan.programId = program.programId;
            plan.programSha256 = programSha256;
            plan.instructionCount = 16;
            plan.staticWorstCaseFuel = 16;
            plan.epochInstructionBudget = 16;
            plan.requiredEpochCount = 1;
            plan.maxEpochs = 1;
            plan.capabilityFlags = profile.capabilityFlags;
            plan.laneCount = spmdLaneCount;
            plan.gridShape = spmdGridShape;
            plan.workgroupShape = spmdWorkgroupShape;
            plan.dispatchGroupShape = { 128, 128, 1 };
            plan.workgroupThreadCount = 64;
            plan.stateWordsPerLane = 1;
            plan.totalStateWords = spmdLaneCount;
            plan.inputWordsPerLane = 0;
            plan.manyLaneAdmitted = true;
            plan.spmdLaneContextSeed = true;
            plan.generalizedTopologyAdmitted = true;
            plan.spmdExecutionPlanSha256 = spmdExecutionPlanSha256;
            plan.maxCallDepth = 1;
            plan.callFrameCapacity = 1;
            plan.stateSchemaVersion = L"gpu-xvm-provable-worlds-field-v1";
            BuildVerifiedCallGraph(plan, program);
            auto binding = winrt::to_string(winrt::hstring(BindingJson(plan)));
            std::vector<uint8_t> bytes(binding.begin(), binding.end());
            plan.planSha256 = WorkerGpuXvmHashBytes(bytes);
            return plan;
        }
        if (!profile.laneParametric && !profile.microtraceAdmitted)
        {
            return plan;
        }
        auto manyLaneProfile = profile.manyLaneAdmitted;
        auto generalizedTopology = profile.generalizedTopologyAdmitted;
        auto laneCount = generalizedTopology ? spmdLaneCount : profile.laneCount;
        auto gridShape = generalizedTopology ? spmdGridShape : profile.gridShape;
        auto workgroupShape = generalizedTopology ? spmdWorkgroupShape : profile.workgroupShape;
        auto gridProduct = CheckedProduct(gridShape);
        auto workgroupProduct = CheckedProduct(workgroupShape);
        auto expectedInputWords = static_cast<uint64_t>(laneCount) * profile.inputWordsPerLane;
        auto fixedTopologyValid = !generalizedTopology &&
            workgroupProduct == 1 && expectedInputWords == profile.inputWords;
        auto generalizedTopologyValid = generalizedTopology && manyLaneProfile &&
            laneCount >= 2 && laneCount <= profile.laneCount &&
            gridShape[0] <= profile.gridShape[0] &&
            gridShape[1] <= profile.gridShape[1] &&
            gridShape[2] <= profile.gridShape[2] &&
            workgroupShape == profile.workgroupShape &&
            workgroupProduct == profile.maxWorkgroupThreads &&
            expectedInputWords <= profile.inputWords;
        if (gridProduct != laneCount || profile.stateWordsPerLane == 0 ||
            profile.inputWordsPerLane == 0 || (!fixedTopologyValid && !generalizedTopologyValid) ||
            program.staticWorstCaseFuel == 0 || program.staticWorstCaseFuel > UINT32_MAX ||
            profile.epochInstructionBudget == 0 ||
            (!manyLaneProfile && (laneCount != 1 || !spmdExecutionPlanSha256.empty())) ||
            (manyLaneProfile && (laneCount < 2 || !profile.spmdLaneContextSeed ||
                spmdExecutionPlanSha256.size() != 64)))
        {
            throw WorkerXvmError("xvm.gpu_profile_not_admitted", "GPU XVM lane plan is outside its bounded single-lane or SPMD profile");
        }

        auto totalStateWords = static_cast<uint64_t>(laneCount) * profile.stateWordsPerLane;
        auto requiredEpochs = (program.staticWorstCaseFuel + profile.epochInstructionBudget - 1) /
            profile.epochInstructionBudget;
        if (totalStateWords > UINT32_MAX || requiredEpochs == 0 || requiredEpochs > profile.maxEpochs)
        {
            throw WorkerXvmError("xvm.gpu_epoch_limit_exceeded", "GPU XVM static fuel proof exceeds the admitted epoch or state plan");
        }

        plan.admitted = true;
        plan.schemaVersion = profile.executionPlanSchemaVersion == nullptr
            ? WorkerGpuXvmExecutionPlanSchemaVersion
            : profile.executionPlanSchemaVersion;
        plan.profileId = profile.profileId;
        plan.contractId = profile.contractId;
        plan.contractSha256 = profile.contractSha256;
        plan.shaderSha256 = profile.shaderSha256;
        plan.programId = program.programId;
        plan.programSha256 = programSha256;
        plan.instructionCount = static_cast<uint32_t>(program.words.size() / WorkerXvmInstructionWords());
        plan.staticWorstCaseFuel = static_cast<uint32_t>(program.staticWorstCaseFuel);
        plan.epochInstructionBudget = profile.epochInstructionBudget;
        plan.requiredEpochCount = static_cast<uint32_t>(requiredEpochs);
        plan.maxEpochs = profile.maxEpochs;
        plan.capabilityFlags = profile.capabilityFlags;
        plan.laneCount = laneCount;
        plan.gridShape = gridShape;
        plan.workgroupShape = workgroupShape;
        plan.dispatchGroupShape =
        {
            (gridShape[0] + workgroupShape[0] - 1) / workgroupShape[0],
            (gridShape[1] + workgroupShape[1] - 1) / workgroupShape[1],
            (gridShape[2] + workgroupShape[2] - 1) / workgroupShape[2],
        };
        plan.workgroupThreadCount = static_cast<uint32_t>(workgroupProduct);
        if (plan.dispatchGroupShape[0] == 0 || plan.dispatchGroupShape[0] > 65535 ||
            plan.dispatchGroupShape[1] == 0 || plan.dispatchGroupShape[1] > 65535 ||
            plan.dispatchGroupShape[2] == 0 || plan.dispatchGroupShape[2] > 65535)
        {
            throw WorkerXvmError("xvm.gpu_profile_not_admitted", "GPU XVM dispatch-group topology exceeds D3D11 bounds");
        }
        plan.stateWordsPerLane = profile.stateWordsPerLane;
        plan.totalStateWords = static_cast<uint32_t>(totalStateWords);
        plan.inputWordsPerLane = profile.inputWordsPerLane;
        plan.manyLaneAdmitted = profile.manyLaneAdmitted;
        plan.spmdLaneContextSeed = profile.spmdLaneContextSeed;
        plan.generalizedTopologyAdmitted = generalizedTopology;
        plan.spmdExecutionPlanSha256 = spmdExecutionPlanSha256;
        plan.maxCallDepth = profile.maxCallDepth;
        plan.stateSchemaVersion = profile.stateSchemaVersion == nullptr
            ? L"gpu-xvm-lane-state-v2"
            : profile.stateSchemaVersion;
        plan.maxLoopDepth = profile.maxLoopDepth;
        auto stateV3Plan = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV3 ||
            plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV4;
        auto stateV4Plan = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5;
        auto stateV5Plan = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6;
        auto stateV6Plan = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV7;
        auto stateV7Plan = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV10;
        if (stateV3Plan || stateV4Plan)
        {
            auto expectedStateSchema = stateV4Plan
                ? L"gpu-xvm-lane-state-v4"
                : L"gpu-xvm-lane-state-v3";
            if (plan.stateSchemaVersion != expectedStateSchema ||
                plan.stateWordsPerLane != 128 || plan.maxLoopDepth == 0 || plan.maxLoopDepth > 8)
            {
                throw WorkerXvmError("xvm.gpu_profile_not_admitted", "GPU XVM loop state is outside the versioned execution-plan contract");
            }
            plan.loopFrameCapacity = 8;
            plan.loopBeginPcWordOffset = 86;
            plan.loopEndPcWordOffset = 94;
            plan.loopRemainingWordOffset = 102;
        }
        if (stateV5Plan &&
            (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v5" ||
             plan.stateWordsPerLane != 128 || plan.maxLoopDepth != 0))
        {
            throw WorkerXvmError(
                "xvm.gpu_profile_not_admitted",
                "GPU XVM legacy branch state is outside the versioned v6 execution-plan contract");
        }
        if (stateV6Plan &&
            (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v6" ||
             plan.stateWordsPerLane != 128 || plan.maxLoopDepth != 0 ||
             !plan.manyLaneAdmitted || !plan.spmdLaneContextSeed || plan.laneCount != 12 ||
             plan.gridShape != std::array<uint32_t, 3>{ 4, 3, 1 } ||
             plan.workgroupShape != std::array<uint32_t, 3>{ 1, 1, 1 }))
        {
            throw WorkerXvmError(
                "xvm.gpu_profile_not_admitted",
                "GPU XVM SPMD state is outside the versioned v7 execution-plan contract");
        }
        if (stateV7Plan &&
            (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v7" ||
             plan.stateWordsPerLane != 128 || plan.maxLoopDepth != 0 ||
             !plan.manyLaneAdmitted || !plan.spmdLaneContextSeed ||
             !plan.generalizedTopologyAdmitted || plan.laneCount < 2 ||
             plan.workgroupShape != std::array<uint32_t, 3>{ 8, 8, 1 } ||
             plan.workgroupThreadCount != 64))
        {
            throw WorkerXvmError(
                "xvm.gpu_profile_not_admitted",
                "GPU XVM generalized SPMD state is outside the versioned v10 execution-plan contract");
        }
        auto spmdStatePlan = stateV6Plan || stateV7Plan;
        if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV4 ||
            stateV4Plan || stateV5Plan || spmdStatePlan)
        {
            if ((!spmdStatePlan && plan.maxCallDepth != WorkerGpuXvmCallFrameCapacityValue) ||
                (spmdStatePlan && (plan.maxCallDepth == 0 ||
                    plan.maxCallDepth > WorkerGpuXvmCallFrameCapacityValue)))
            {
                throw WorkerXvmError(
                    "xvm.gpu_profile_not_admitted",
                    "GPU XVM v4 call depth does not match the fixed state capacity");
            }
            plan.callFrameCapacity = WorkerGpuXvmCallFrameCapacityValue;
            plan.returnPcStackWordOffset = WorkerGpuXvmReturnPcStackWordOffsetValue;
            plan.callLoopDepthStackWordOffset = WorkerGpuXvmCallLoopDepthStackWordOffsetValue;
            BuildVerifiedCallGraph(plan, program);
        }
        if (stateV4Plan)
        {
            plan.trapStateSchemaVersion = WorkerGpuXvmTrapStateSchemaVersion;
            plan.trapStatusWordOffset = WorkerGpuXvmTrapStatusWordOffsetValue;
            plan.trapCodeWordOffset = WorkerGpuXvmTrapCodeWordOffsetValue;
            plan.trapPcWordOffset = WorkerGpuXvmTrapPcWordOffsetValue;
            plan.trapRecoveryPcWordOffset = WorkerGpuXvmTrapRecoveryPcWordOffsetValue;
            plan.trapOccurrenceCountWordOffset = WorkerGpuXvmTrapOccurrenceCountWordOffsetValue;
            plan.trapCodeMaximum = WorkerGpuXvmTrapCodeMaximumValue;
            BuildVerifiedRecoverySites(plan, program);
        }
        if (stateV5Plan)
        {
            BuildVerifiedBranchSites(plan, program);
        }
        if (profile.microtraceAdmitted)
        {
            auto identityPhase = profile.kind == WorkerGpuXvmProfileKind::MicrotraceIdentityStraightLineU32V1;
            auto optimizationPhase = profile.kind == WorkerGpuXvmProfileKind::MicrotraceOptimizedStraightLineU32V1;
            auto schemaAndRecordMatch =
                (identityPhase && plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV8 &&
                    profile.microtraceWordsPerOp == 8) ||
                (optimizationPhase && plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9 &&
                    profile.microtraceWordsPerOp == 16);
            if (!schemaAndRecordMatch || profile.maxMicrotraceOps == 0 ||
                plan.instructionCount > profile.maxMicrotraceOps ||
                plan.staticWorstCaseFuel != plan.instructionCount ||
                !plan.verifiedCodeRegions.empty() || !plan.verifiedCallSites.empty())
            {
                throw WorkerXvmError("xvm.microtrace_plan_invalid", "microtrace plan is outside the admitted bounded straight-line CFG contract");
            }
            plan.microtraceAdmitted = true;
            if (optimizationPhase)
            {
                BuildPhaseBMicrotrace(plan, program);
            }
            else
            {
                plan.microtraceSchemaVersion = L"xvm-gpu-microtrace-record-v1";
                plan.microtraceWordsPerOp = profile.microtraceWordsPerOp;
                plan.microtraceOpCount = plan.instructionCount;
                plan.microtraceFuelDebit = plan.instructionCount;
                plan.microtraceExpansionNumerator = plan.instructionCount;
                plan.microtraceExpansionDenominator = plan.instructionCount;
                plan.microtraceSourcePcMap.reserve(plan.instructionCount);
                plan.microtraceWords.reserve(static_cast<size_t>(plan.instructionCount) * plan.microtraceWordsPerOp);
                for (uint32_t pc = 0; pc < plan.instructionCount; ++pc)
                {
                    auto source = static_cast<size_t>(pc) * WorkerXvmInstructionWords();
                    plan.microtraceSourcePcMap.push_back(pc);
                    plan.microtraceWords.push_back(program.words[source]);
                    plan.microtraceWords.push_back(pc);
                    plan.microtraceWords.push_back(program.words[source + 1]);
                    plan.microtraceWords.push_back(program.words[source + 2]);
                    plan.microtraceWords.push_back(program.words[source + 3]);
                    plan.microtraceWords.push_back(static_cast<uint32_t>(WorkerXvmInstructionWords()));
                    plan.microtraceWords.push_back(1);
                    plan.microtraceWords.push_back(0);
                }
                plan.microtraceSha256 = HashU32Words(plan.microtraceWords);
            }
        }
        else if (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV8 ||
                 plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9)
        {
            throw WorkerXvmError("xvm.microtrace_plan_invalid", "microtrace plan schema requires the admitted microtrace profile");
        }

        auto binding = winrt::to_string(winrt::hstring(BindingJson(plan)));
        std::vector<uint8_t> bytes(binding.begin(), binding.end());
        plan.planSha256 = WorkerGpuXvmHashBytes(bytes);
        return plan;
    }

    std::wstring WorkerGpuXvmExecutionPlanJson(WorkerGpuXvmExecutionPlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        auto binding = BindingJson(plan);
        binding.pop_back();
        return binding + L",\"plan_sha256\":" + JsonString(plan.planSha256) + L"}";
    }

    bool WorkerGpuXvmExecutionPlansEqual(
        WorkerGpuXvmExecutionPlan const& expected,
        WorkerGpuXvmExecutionPlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            (!expected.admitted ||
                (expected.planSha256 == actual.planSha256 &&
                 BindingJson(expected) == BindingJson(actual) &&
                 expected.microtraceWords == actual.microtraceWords));
    }
}
