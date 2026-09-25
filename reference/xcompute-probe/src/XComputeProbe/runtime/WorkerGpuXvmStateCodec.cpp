#include "pch.h"
#include "WorkerGpuXvmStateCodec.h"
#include "WorkerXvmIsa.h"

#include <algorithm>

namespace XComputeProbe
{
    namespace
    {
        bool IsZeroRange(
            std::vector<uint32_t> const& words,
            uint32_t begin,
            uint32_t end)
        {
            for (auto index = begin; index < end; ++index)
            {
                if (words[index] != 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool ValidateLoopFrames(
            std::vector<uint32_t> const& words,
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            WorkerGpuXvmDecodedState& decoded,
            std::string& error)
        {
            decoded.loopDepth = words[WorkerGpuXvmLaneLoopDepthWordOffsetValue];
            if (plan.loopFrameCapacity != WorkerGpuXvmLaneLoopFrameCapacityValue ||
                plan.loopBeginPcWordOffset != WorkerGpuXvmLaneLoopBeginPcWordOffsetValue ||
                plan.loopEndPcWordOffset != WorkerGpuXvmLaneLoopEndPcWordOffsetValue ||
                plan.loopRemainingWordOffset != WorkerGpuXvmLaneLoopRemainingWordOffsetValue ||
                decoded.loopDepth > plan.maxLoopDepth || plan.maxLoopDepth > plan.loopFrameCapacity)
            {
                error = "GPU XVM loop stack depth or layout is outside the immutable execution plan";
                return false;
            }

            uint32_t previousBegin = 0;
            uint32_t previousEnd = instructionCount;
            for (uint32_t index = 0; index < plan.loopFrameCapacity; ++index)
            {
                auto beginPc = words[plan.loopBeginPcWordOffset + index];
                auto endPc = words[plan.loopEndPcWordOffset + index];
                auto remaining = words[plan.loopRemainingWordOffset + index];
                if (index >= decoded.loopDepth)
                {
                    if (beginPc != 0 || endPc != 0 || remaining != 0)
                    {
                        error = "GPU XVM inactive loop frame is not canonically zero";
                        return false;
                    }
                    continue;
                }

                if (beginPc >= endPc || endPc >= instructionCount ||
                    (index != 0 && (beginPc <= previousBegin || endPc >= previousEnd)))
                {
                    error = "GPU XVM active loop frames are not properly nested";
                    return false;
                }
                auto beginWord = static_cast<size_t>(beginPc) * WorkerXvmInstructionWordsValue;
                auto endWord = static_cast<size_t>(endPc) * WorkerXvmInstructionWordsValue;
                if (endWord + 3 >= program.words.size() ||
                    program.words[beginWord] != static_cast<uint32_t>(WorkerXvmOpcode::LoopBegin) ||
                    program.words[beginWord + 2] != endPc || program.words[beginWord + 3] != 0 ||
                    program.words[endWord] != static_cast<uint32_t>(WorkerXvmOpcode::LoopEnd) ||
                    program.words[endWord + 1] != beginPc ||
                    program.words[endWord + 2] != 0 || program.words[endWord + 3] != 0 ||
                    remaining == 0 || remaining > program.words[beginWord + 1])
                {
                    error = "GPU XVM active loop frame does not match the verified program";
                    return false;
                }
                previousBegin = beginPc;
                previousEnd = endPc;
            }

            if (decoded.loopDepth != 0)
            {
                auto topIndex = decoded.loopDepth - 1;
                auto topBegin = words[plan.loopBeginPcWordOffset + topIndex];
                auto topEnd = words[plan.loopEndPcWordOffset + topIndex];
                if (decoded.pc <= topBegin || decoded.pc > topEnd)
                {
                    error = "GPU XVM resident PC is incompatible with the active loop stack";
                    return false;
                }
            }
            return true;
        }

        bool ValidateActiveCodeRegionV3(
            std::vector<uint32_t> const& words,
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            WorkerGpuXvmDecodedState const& decoded,
            std::string& error)
        {
            if (program.functions.size() != 1)
            {
                error = "GPU XVM v3 state requires exactly one admitted leaf function";
                return false;
            }

            auto const& function = program.functions.front();
            if (function.entryPc == 0 || function.entryPc > function.endPc || function.endPc >= instructionCount)
            {
                error = "GPU XVM function region is outside the verified program";
                return false;
            }

            uint32_t regionBegin = 0;
            uint32_t regionEnd = function.entryPc - 1;
            auto returnPc = words[WorkerGpuXvmLaneReturnPcWordOffsetValue];
            if (decoded.callDepth == 0)
            {
                if (returnPc != 0 || (!decoded.halted && decoded.pc > regionEnd))
                {
                    error = "GPU XVM main-region state is incompatible with the call stack";
                    return false;
                }
            }
            else if (decoded.callDepth == 1)
            {
                if (returnPc == 0 || returnPc >= function.entryPc)
                {
                    error = "GPU XVM return address is outside the admitted main region";
                    return false;
                }
                auto callPc = returnPc - 1;
                auto callWord = static_cast<size_t>(callPc) * WorkerXvmInstructionWordsValue;
                if (callWord + 3 >= program.words.size() ||
                    program.words[callWord] != static_cast<uint32_t>(WorkerXvmOpcode::Call) ||
                    program.words[callWord + 1] != function.entryPc ||
                    program.words[callWord + 2] != 0 || program.words[callWord + 3] != 0 ||
                    decoded.pc < function.entryPc || decoded.pc > function.endPc)
                {
                    error = "GPU XVM active call frame does not match the verified leaf function";
                    return false;
                }
                regionBegin = function.entryPc;
                regionEnd = function.endPc;
            }
            else
            {
                error = "GPU XVM v3 state exceeds the admitted leaf call depth";
                return false;
            }

            for (uint32_t index = 0; index < decoded.loopDepth; ++index)
            {
                auto beginPc = words[plan.loopBeginPcWordOffset + index];
                auto endPc = words[plan.loopEndPcWordOffset + index];
                if (beginPc < regionBegin || endPc > regionEnd)
                {
                    error = "GPU XVM active loop frame crosses the active code region";
                    return false;
                }
            }
            return true;
        }

        bool ValidateV4PlanCallContract(
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            std::string& error)
        {
            if (plan.callFrameCapacity != WorkerGpuXvmLaneCallFrameCapacityValue ||
                plan.returnPcStackWordOffset != WorkerGpuXvmLaneReturnPcWordOffsetValue ||
                plan.callLoopDepthStackWordOffset != WorkerGpuXvmLaneCallLoopDepthWordOffsetValue ||
                plan.maxCallDepth == 0 || plan.maxCallDepth > plan.callFrameCapacity ||
                plan.verifiedActualMaxCallDepth == 0 ||
                plan.verifiedActualMaxCallDepth > plan.maxCallDepth ||
                !plan.callGraphAllFunctionsReachable || !plan.callGraphAcyclic ||
                program.functions.empty() ||
                plan.verifiedCodeRegions.size() != program.functions.size() + 1)
            {
                error = "GPU XVM v4 call contract is outside the immutable execution plan";
                return false;
            }

            auto const& mainRegion = plan.verifiedCodeRegions.front();
            if (mainRegion.regionIndex != 0 || !mainRegion.mainRegion || mainRegion.beginPc != 0 ||
                program.functions.front().entryPc == 0 ||
                mainRegion.endPc != program.functions.front().entryPc - 1)
            {
                error = "GPU XVM v4 main region does not match the verified program";
                return false;
            }
            for (size_t index = 0; index < program.functions.size(); ++index)
            {
                auto const& function = program.functions[index];
                auto const& region = plan.verifiedCodeRegions[index + 1];
                if (region.regionIndex != static_cast<uint32_t>(index + 1) || region.mainRegion ||
                    region.beginPc != function.entryPc || region.endPc != function.endPc ||
                    region.beginPc > region.endPc || region.endPc >= instructionCount)
                {
                    error = "GPU XVM v4 function regions do not match the verified program";
                    return false;
                }
            }

            auto regionForPc = [&](uint32_t pc, uint32_t& regionIndex)
            {
                for (auto const& region : plan.verifiedCodeRegions)
                {
                    if (pc >= region.beginPc && pc <= region.endPc)
                    {
                        regionIndex = region.regionIndex;
                        return true;
                    }
                }
                return false;
            };
            auto regionForEntry = [&](uint32_t entryPc, uint32_t& regionIndex)
            {
                for (size_t index = 1; index < plan.verifiedCodeRegions.size(); ++index)
                {
                    if (plan.verifiedCodeRegions[index].beginPc == entryPc)
                    {
                        regionIndex = plan.verifiedCodeRegions[index].regionIndex;
                        return true;
                    }
                }
                return false;
            };

            size_t expectedCallSiteIndex = 0;
            for (uint32_t pc = 0; pc < instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::Call))
                {
                    continue;
                }
                if (expectedCallSiteIndex >= plan.verifiedCallSites.size())
                {
                    error = "GPU XVM v4 plan omitted a verified call site";
                    return false;
                }
                uint32_t callerRegionIndex = 0;
                uint32_t targetRegionIndex = 0;
                auto targetEntryPc = program.words[wordOffset + 1];
                auto const& call = plan.verifiedCallSites[expectedCallSiteIndex++];
                if (!regionForPc(pc, callerRegionIndex) ||
                    !regionForEntry(targetEntryPc, targetRegionIndex) ||
                    call.callPc != pc || call.callerRegionIndex != callerRegionIndex ||
                    call.targetEntryPc != targetEntryPc ||
                    call.targetRegionIndex != targetRegionIndex ||
                    program.words[wordOffset + 2] != 0 || program.words[wordOffset + 3] != 0)
                {
                    error = "GPU XVM v4 ordered call-site binding does not match the verified program";
                    return false;
                }
            }
            if (expectedCallSiteIndex == 0 || expectedCallSiteIndex != plan.verifiedCallSites.size())
            {
                error = "GPU XVM v4 plan contains an unbound call site";
                return false;
            }
            return true;
        }

        bool ValidateActiveCodeRegionV4(
            std::vector<uint32_t> const& words,
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            WorkerGpuXvmDecodedState const& decoded,
            std::string& error)
        {
            if (!ValidateV4PlanCallContract(program, instructionCount, plan, error) ||
                decoded.callDepth > plan.callFrameCapacity ||
                decoded.callDepth > plan.verifiedActualMaxCallDepth)
            {
                if (error.empty())
                {
                    error = "GPU XVM v4 active call depth exceeds the verified graph";
                }
                return false;
            }

            uint32_t activeRegionIndex = 0;
            for (uint32_t index = 0; index < plan.callFrameCapacity; ++index)
            {
                auto returnPc = words[plan.returnPcStackWordOffset + index];
                auto savedLoopDepth = words[plan.callLoopDepthStackWordOffset + index];
                if (index >= decoded.callDepth)
                {
                    if (returnPc != 0 || savedLoopDepth != 0)
                    {
                        error = "GPU XVM v4 inactive call frame is not canonically zero";
                        return false;
                    }
                    continue;
                }
                if (returnPc == 0 || returnPc >= instructionCount || savedLoopDepth != 0)
                {
                    error = "GPU XVM v4 active call frame violates the no-call-inside-loop policy";
                    return false;
                }

                auto callPc = returnPc - 1;
                auto const& callerRegion = plan.verifiedCodeRegions[activeRegionIndex];
                auto callWord = static_cast<size_t>(callPc) * WorkerXvmInstructionWordsValue;
                if (callPc < callerRegion.beginPc || callPc > callerRegion.endPc ||
                    callWord + 3 >= program.words.size() ||
                    program.words[callWord] != static_cast<uint32_t>(WorkerXvmOpcode::Call) ||
                    program.words[callWord + 2] != 0 || program.words[callWord + 3] != 0)
                {
                    error = "GPU XVM v4 return address is not a call in the active caller region";
                    return false;
                }

                auto targetEntryPc = program.words[callWord + 1];
                auto callSite = std::find_if(
                    plan.verifiedCallSites.begin(),
                    plan.verifiedCallSites.end(),
                    [&](WorkerGpuXvmVerifiedCallSite const& candidate)
                    {
                        return candidate.callPc == callPc &&
                            candidate.callerRegionIndex == activeRegionIndex &&
                            candidate.targetEntryPc == targetEntryPc;
                    });
                if (callSite == plan.verifiedCallSites.end() ||
                    callSite->targetRegionIndex == 0 ||
                    callSite->targetRegionIndex >= plan.verifiedCodeRegions.size() ||
                    plan.verifiedCodeRegions[callSite->targetRegionIndex].beginPc != targetEntryPc)
                {
                    error = "GPU XVM v4 active call frame does not target its verified next callee";
                    return false;
                }
                activeRegionIndex = callSite->targetRegionIndex;
            }

            auto const& activeRegion = plan.verifiedCodeRegions[activeRegionIndex];
            if (!decoded.halted &&
                (decoded.pc < activeRegion.beginPc || decoded.pc > activeRegion.endPc))
            {
                error = "GPU XVM v4 resident PC is outside the active call-chain region";
                return false;
            }
            for (uint32_t index = 0; index < decoded.loopDepth; ++index)
            {
                auto beginPc = words[plan.loopBeginPcWordOffset + index];
                auto endPc = words[plan.loopEndPcWordOffset + index];
                if (beginPc < activeRegion.beginPc || endPc > activeRegion.endPc)
                {
                    error = "GPU XVM v4 active loop frame crosses the final callee region";
                    return false;
                }
            }
            return true;
        }

        bool ValidateTrapStateV4(
            std::vector<uint32_t> const& words,
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            WorkerGpuXvmDecodedState& decoded,
            std::string& error)
        {
            if (plan.trapStateSchemaVersion != WorkerGpuXvmTrapStateSchemaVersion ||
                plan.trapStatusWordOffset != WorkerGpuXvmLaneTrapStatusWordOffsetV4Value ||
                plan.trapCodeWordOffset != WorkerGpuXvmLaneTrapCodeWordOffsetV4Value ||
                plan.trapPcWordOffset != WorkerGpuXvmLaneTrapPcWordOffsetV4Value ||
                plan.trapRecoveryPcWordOffset != WorkerGpuXvmLaneTrapRecoveryPcWordOffsetV4Value ||
                plan.trapOccurrenceCountWordOffset !=
                    WorkerGpuXvmLaneTrapOccurrenceCountWordOffsetV4Value ||
                plan.trapCodeMaximum != WorkerGpuXvmTrapCodeMaximumValue ||
                !plan.recoveredTrapStateAdmitted || !plan.recoverySameOwnershipVerified ||
                plan.verifiedRecoverySites.empty())
            {
                error = "GPU XVM v4 trap state is outside the immutable v5 execution plan";
                return false;
            }

            auto regionForPc = [&](uint32_t pc, uint32_t& regionIndex)
            {
                for (auto const& region : plan.verifiedCodeRegions)
                {
                    if (pc >= region.beginPc && pc <= region.endPc)
                    {
                        regionIndex = region.regionIndex;
                        return true;
                    }
                }
                return false;
            };
            size_t expectedSiteIndex = 0;
            for (uint32_t pc = 0; pc < instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero))
                {
                    continue;
                }
                if (expectedSiteIndex >= plan.verifiedRecoverySites.size())
                {
                    error = "GPU XVM v5 plan omitted a verified recovered trap site";
                    return false;
                }
                auto trapCode = program.words[wordOffset + 2];
                auto recoveryPc = program.words[wordOffset + 3];
                uint32_t sourceRegionIndex = 0;
                uint32_t recoveryRegionIndex = 0;
                auto const& site = plan.verifiedRecoverySites[expectedSiteIndex++];
                if (trapCode == 0 || trapCode > plan.trapCodeMaximum ||
                    recoveryPc <= pc || recoveryPc >= instructionCount ||
                    !regionForPc(pc, sourceRegionIndex) ||
                    !regionForPc(recoveryPc, recoveryRegionIndex) ||
                    sourceRegionIndex != recoveryRegionIndex ||
                    site.trapPc != pc || site.trapCode != trapCode ||
                    site.recoveryPc != recoveryPc || site.regionIndex != sourceRegionIndex)
                {
                    error = "GPU XVM v5 ordered recovery-site binding does not match the verified program";
                    return false;
                }
            }
            if (expectedSiteIndex == 0 || expectedSiteIndex != plan.verifiedRecoverySites.size())
            {
                error = "GPU XVM v5 plan contains an unbound recovered trap site";
                return false;
            }

            auto status = words[plan.trapStatusWordOffset];
            auto code = words[plan.trapCodeWordOffset];
            auto trapPc = words[plan.trapPcWordOffset];
            auto recoveryPc = words[plan.trapRecoveryPcWordOffset];
            auto occurrenceCount = words[plan.trapOccurrenceCountWordOffset];
            if (status == WorkerGpuXvmTrapStatusNoneValue)
            {
                if (code != 0 || trapPc != 0 || recoveryPc != 0 || occurrenceCount != 0)
                {
                    error = "GPU XVM empty recovered trap state is not canonically zero";
                    return false;
                }
                decoded.trap = {};
                decoded.trapCode = 0;
                return true;
            }
            if (status != WorkerGpuXvmTrapStatusRecoveredValue ||
                code == 0 || code > plan.trapCodeMaximum || occurrenceCount == 0 ||
                occurrenceCount > decoded.fuelConsumed || trapPc >= recoveryPc ||
                recoveryPc >= instructionCount)
            {
                error = "GPU XVM recovered trap state is outside the admitted domain";
                return false;
            }

            auto site = std::find_if(
                plan.verifiedRecoverySites.begin(),
                plan.verifiedRecoverySites.end(),
                [&](WorkerGpuXvmVerifiedRecoverySite const& candidate)
                {
                    return candidate.trapPc == trapPc && candidate.trapCode == code &&
                        candidate.recoveryPc == recoveryPc;
                });
            if (site == plan.verifiedRecoverySites.end())
            {
                error = "GPU XVM recovered trap state does not bind to a verified recovery site";
                return false;
            }
            decoded.trap.code = code;
            decoded.trap.trapPc = trapPc;
            decoded.trap.recoveryPc = recoveryPc;
            decoded.trap.occurrenceCount = occurrenceCount;
            decoded.trapCode = code;
            return true;
        }

        bool ValidateLegacyBranchTopologyV5(
            WorkerXvmProgram const& program,
            uint32_t instructionCount,
            WorkerGpuXvmExecutionPlan const& plan,
            std::string& error)
        {
            if (!plan.legacyBranchTopologyAdmitted ||
                !plan.branchTargetsForwardSameRegionVerified ||
                !plan.structuredControlMixingForbidden ||
                plan.verifiedBranchSites.empty() ||
                plan.recoveredTrapStateAdmitted ||
                !plan.verifiedRecoverySites.empty())
            {
                error = "GPU XVM v5 legacy branch state is outside the immutable v6 execution plan";
                return false;
            }
            if (std::find(program.capabilities.begin(), program.capabilities.end(),
                    L"structured_control_v2") != program.capabilities.end() ||
                std::find(program.capabilities.begin(), program.capabilities.end(),
                    L"structured_control_v2_phase_b") != program.capabilities.end())
            {
                error = "GPU XVM v6 legacy branch plan mixed structured-control capabilities";
                return false;
            }

            auto regionForPc = [&](uint32_t pc, uint32_t& regionIndex)
            {
                for (auto const& region : plan.verifiedCodeRegions)
                {
                    if (pc >= region.beginPc && pc <= region.endPc)
                    {
                        regionIndex = region.regionIndex;
                        return true;
                    }
                }
                return false;
            };
            size_t expectedSiteIndex = 0;
            for (uint32_t pc = 0; pc < instructionCount; ++pc)
            {
                auto wordOffset = static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue;
                if (program.words[wordOffset] != static_cast<uint32_t>(WorkerXvmOpcode::BranchIfZero))
                {
                    continue;
                }
                if (expectedSiteIndex >= plan.verifiedBranchSites.size())
                {
                    error = "GPU XVM v6 plan omitted a verified legacy branch site";
                    return false;
                }
                auto registerIndex = program.words[wordOffset + 1];
                auto targetPc = program.words[wordOffset + 2];
                uint32_t sourceRegionIndex = 0;
                uint32_t targetRegionIndex = 0;
                auto const& site = plan.verifiedBranchSites[expectedSiteIndex++];
                if (registerIndex >= 16 || targetPc <= pc || targetPc >= instructionCount ||
                    program.words[wordOffset + 3] != 0 ||
                    !regionForPc(pc, sourceRegionIndex) || !regionForPc(targetPc, targetRegionIndex) ||
                    sourceRegionIndex != targetRegionIndex ||
                    site.branchPc != pc || site.registerIndex != registerIndex ||
                    site.targetPc != targetPc || site.regionIndex != sourceRegionIndex)
                {
                    error = "GPU XVM v6 ordered branch-site binding does not match the verified program";
                    return false;
                }
            }
            if (expectedSiteIndex == 0 || expectedSiteIndex != plan.verifiedBranchSites.size())
            {
                error = "GPU XVM v6 plan contains an unbound legacy branch site";
                return false;
            }
            return true;
        }
    }

    std::vector<uint32_t> WorkerCreateGpuXvmInitialState(
        WorkerGpuXvmProfileDescriptor const& profile,
        uint32_t instructionCount,
        uint32_t memoryWords,
        uint32_t outputWords,
        uint32_t fuelLimit,
        WorkerGpuXvmExecutionPlan const& plan)
    {
        auto wordCount = profile.laneParametric ? plan.totalStateWords : profile.resultWords;
        std::vector<uint32_t> words(wordCount, 0);
        if (!profile.laneParametric && profile.requiresProgramSrv && words.size() >= 8)
        {
            words[3] = outputWords;
            words[5] = instructionCount;
            words[6] = memoryWords;
            words[7] = fuelLimit;
        }
        if (profile.laneParametric)
        {
            if (!plan.admitted || plan.laneCount == 0 || plan.stateWordsPerLane < 96 ||
                words.size() != plan.totalStateWords ||
                words.size() != static_cast<size_t>(plan.laneCount) * plan.stateWordsPerLane)
            {
                throw WorkerXvmError("xvm.gpu_resident_state_invalid", "lane-parametric GPU XVM state does not match its admitted execution plan");
            }
            auto planVersion = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV10
                ? 10u
                : (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV7
                ? 7u
                : (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6
                ? 6u
                : (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5
                ? 5u
                : (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV4
                    ? 4u
                    : (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV3 ? 3u : 2u)))));
            for (uint32_t laneId = 0; laneId < plan.laneCount; ++laneId)
            {
                auto base = static_cast<size_t>(laneId) * plan.stateWordsPerLane;
                words[base + 3] = outputWords;
                words[base + 5] = instructionCount;
                words[base + 6] = memoryWords;
                words[base + 7] = fuelLimit;
                words[base + 15] = plan.capabilityFlags;
                words[base + 48] = laneId;
                words[base + 49] = plan.laneCount;
                words[base + 50] = plan.gridShape[0];
                words[base + 51] = plan.gridShape[1];
                words[base + 52] = plan.gridShape[2];
                words[base + 53] = plan.workgroupShape[0];
                words[base + 54] = plan.workgroupShape[1];
                words[base + 55] = plan.workgroupShape[2];
                words[base + 56] = plan.stateWordsPerLane;
                words[base + 57] = plan.inputWordsPerLane;
                words[base + 58] = 0;
                words[base + 59] = plan.maxCallDepth;
                words[base + 76] = 0;
                words[base + 77] = plan.maxLoopDepth;
                words[base + 83] = planVersion;
                words[base + 84] = plan.staticWorstCaseFuel;
                words[base + 85] = plan.requiredEpochCount;
                if (plan.spmdLaneContextSeed)
                {
                    auto yz = laneId / plan.gridShape[0];
                    words[base + 16 + 12] = laneId;
                    words[base + 16 + 13] = laneId % plan.gridShape[0];
                    words[base + 16 + 14] = yz % plan.gridShape[1];
                    words[base + 16 + 15] = yz / plan.gridShape[1];
                }
            }
        }
        return words;
    }
    bool WorkerDecodeGpuXvmState(
        WorkerGpuXvmProfileDescriptor const& profile,
        std::vector<uint32_t> const& words,
        uint32_t instructionCount,
        uint32_t memoryWords,
        uint32_t outputWords,
        uint32_t fuelLimit,
        WorkerGpuXvmExecutionPlan const& plan,
        WorkerXvmProgram const& program,
        WorkerGpuXvmDecodedState& decoded,
        std::string& error,
        uint32_t laneId)
    {
        if (words.size() != profile.resultWords || words.size() < 8 ||
            profile.resultOutputWordOffset > words.size() ||
            outputWords > words.size() - profile.resultOutputWordOffset)
        {
            error = "GPU XVM state word layout does not match the admitted profile";
            return false;
        }
        if (words[0] > 1 || words[1] > 1)
        {
            error = "GPU XVM terminal or control state is outside the admitted domain";
            return false;
        }
        if (words[3] != outputWords || words[5] != instructionCount ||
            words[6] != memoryWords || words[7] != fuelLimit)
        {
            error = "GPU XVM state header does not match the immutable execution plan";
            return false;
        }
        if (words[2] > fuelLimit)
        {
            error = "GPU XVM fuel exceeds the admitted execution limit";
            return false;
        }

        decoded = {};
        decoded.halted = words[0] == 1;
        decoded.control = words[1];
        decoded.fuelConsumed = words[2];
        decoded.faultCode = words[4];
        decoded.trapCode = profile.laneParametric
            ? (plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5 ||
               plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6
                ? 0
                : words[WorkerGpuXvmLaneTrapCodeWordOffsetValue])
            : words[4];
        decoded.outputWords.assign(
            words.begin() + profile.resultOutputWordOffset,
            words.begin() + profile.resultOutputWordOffset + outputWords);

        if (profile.residentExecution)
        {
            if (words.size() <= WorkerGpuXvmResidentEpochWordOffsetValue ||
                words[WorkerGpuXvmResidentEpochWordOffsetValue] > profile.maxEpochs)
            {
                error = "GPU XVM resident PC or epoch sequence is outside the admitted plan";
                return false;
            }
            decoded.pc = words[WorkerGpuXvmResidentPcWordOffsetValue];
            decoded.epochSequence = words[WorkerGpuXvmResidentEpochWordOffsetValue];

            if (!profile.laneParametric && decoded.pc >= instructionCount)
            {
                error = "GPU XVM resident PC is outside the admitted instruction range";
                return false;
            }
            if (profile.laneParametric)
            {
                auto planV3 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV3;
                auto planV4 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV4;
                auto planV5 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5;
                auto planV6 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6;
                auto planV7 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV7;
                auto planV10 = plan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV10;
                auto stateV3Plan = planV3 || planV4;
                auto stateV4Plan = planV5;
                auto stateV5Plan = planV6;
                auto stateV6Plan = planV7;
                auto stateV7Plan = planV10;
                auto expectedPlanVersion = planV4 ? 4u : (planV3 ? 3u : 2u);
                if (planV5 || planV6 || planV7 || planV10)
                {
                    expectedPlanVersion = planV10 ? 10u : (planV7 ? 7u : (planV6 ? 6u : 5u));
                }
                if (!plan.admitted || laneId >= plan.laneCount || words.size() < 96 ||
                    words.size() != plan.stateWordsPerLane ||
                    words[15] != plan.capabilityFlags || words[48] != laneId || words[49] != plan.laneCount ||
                    words[50] != plan.gridShape[0] || words[51] != plan.gridShape[1] || words[52] != plan.gridShape[2] ||
                    words[53] != plan.workgroupShape[0] || words[54] != plan.workgroupShape[1] || words[55] != plan.workgroupShape[2] ||
                    words[56] != plan.stateWordsPerLane || words[57] != plan.inputWordsPerLane ||
                    words[59] != plan.maxCallDepth || words[77] != plan.maxLoopDepth ||
                    words[83] != expectedPlanVersion ||
                    words[84] != plan.staticWorstCaseFuel || words[85] != plan.requiredEpochCount)
                {
                    error = "GPU XVM lane state topology does not match the immutable execution plan";
                    return false;
                }
                decoded.callDepth = words[WorkerGpuXvmLaneCallDepthWordOffsetValue];
                if (decoded.callDepth > plan.maxCallDepth ||
                    decoded.fuelConsumed > plan.staticWorstCaseFuel ||
                    (!decoded.halted && decoded.pc >= instructionCount) ||
                    (decoded.halted && (decoded.pc != instructionCount || decoded.callDepth != 0)) ||
                    (decoded.callDepth != 0 &&
                        (words[WorkerGpuXvmLaneReturnPcWordOffsetValue] == 0 ||
                         words[WorkerGpuXvmLaneReturnPcWordOffsetValue] >= instructionCount)))
                {
                    error = "GPU XVM lane control or call state is outside the admitted plan";
                    return false;
                }
                if (stateV3Plan)
                {
                    if (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v3" || words.size() != 128 ||
                        !IsZeroRange(words, 78, 83) || !IsZeroRange(words, 110, 128))
                    {
                        error = "GPU XVM versioned loop or reserved state is outside the immutable plan";
                        return false;
                    }
                    if (planV3 &&
                        (!IsZeroRange(words, 68, 76) ||
                         (decoded.callDepth == 0 && words[WorkerGpuXvmLaneReturnPcWordOffsetValue] != 0) ||
                         !IsZeroRange(words, WorkerGpuXvmLaneReturnPcWordOffsetValue + 1, 68)))
                    {
                        error = "GPU XVM versioned loop or reserved state is outside the immutable plan";
                        return false;
                    }
                    if (!ValidateLoopFrames(words, program, instructionCount, plan, decoded, error) ||
                        (planV3
                            ? !ValidateActiveCodeRegionV3(words, program, instructionCount, plan, decoded, error)
                            : !ValidateActiveCodeRegionV4(words, program, instructionCount, plan, decoded, error)) ||
                        (decoded.halted && decoded.loopDepth != 0))
                    {
                        return false;
                    }
                }
                else if (stateV4Plan)
                {
                    if (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v4" || words.size() != 128 ||
                        !IsZeroRange(words, 110, 128))
                    {
                        error = "GPU XVM versioned trap or reserved state is outside the immutable plan";
                        return false;
                    }
                    if (!ValidateLoopFrames(words, program, instructionCount, plan, decoded, error) ||
                        !ValidateActiveCodeRegionV4(words, program, instructionCount, plan, decoded, error) ||
                        !ValidateTrapStateV4(words, program, instructionCount, plan, decoded, error) ||
                        (decoded.halted && decoded.loopDepth != 0))
                    {
                        return false;
                    }
                }
                else if (stateV5Plan)
                {
                    decoded.loopDepth = words[WorkerGpuXvmLaneLoopDepthWordOffsetValue];
                    if (plan.stateSchemaVersion != L"gpu-xvm-lane-state-v5" || words.size() != 128 ||
                        decoded.loopDepth != 0 || plan.maxLoopDepth != 0 ||
                        !IsZeroRange(words, 68, 83) || !IsZeroRange(words, 86, 128))
                    {
                        error = "GPU XVM versioned legacy-branch or reserved state is outside the immutable plan";
                        return false;
                    }
                    if (!ValidateActiveCodeRegionV4(words, program, instructionCount, plan, decoded, error) ||
                        !ValidateLegacyBranchTopologyV5(program, instructionCount, plan, error))
                    {
                        return false;
                    }
                    decoded.trap = {};
                    decoded.trapCode = 0;
                }
                else if (stateV6Plan || stateV7Plan)
                {
                    decoded.loopDepth = words[WorkerGpuXvmLaneLoopDepthWordOffsetValue];
                    auto yz = laneId / plan.gridShape[0];
                    auto expectedX = laneId % plan.gridShape[0];
                    auto expectedY = yz % plan.gridShape[1];
                    auto expectedZ = yz / plan.gridShape[1];
                    auto expectedStateSchema = stateV7Plan
                        ? L"gpu-xvm-lane-state-v7"
                        : L"gpu-xvm-lane-state-v6";
                    auto topologyModeValid = stateV7Plan
                        ? plan.generalizedTopologyAdmitted
                        : !plan.generalizedTopologyAdmitted;
                    if (plan.stateSchemaVersion != expectedStateSchema || words.size() != 128 ||
                        !plan.manyLaneAdmitted || !plan.spmdLaneContextSeed || !topologyModeValid ||
                        plan.spmdExecutionPlanSha256.size() != 64 || decoded.loopDepth != 0 ||
                        plan.maxLoopDepth != 0 || !IsZeroRange(words, 68, 83) ||
                        !IsZeroRange(words, 86, 128) ||
                        words[16 + 12] != laneId || words[16 + 13] != expectedX ||
                        words[16 + 14] != expectedY || words[16 + 15] != expectedZ)
                    {
                        error = "GPU XVM SPMD lane context or reserved state is outside the immutable topology plan";
                        return false;
                    }
                    if (!ValidateActiveCodeRegionV4(words, program, instructionCount, plan, decoded, error))
                    {
                        return false;
                    }
                    decoded.trap = {};
                    decoded.trapCode = 0;
                }
                else
                {
                    decoded.loopDepth = words[WorkerGpuXvmLaneLoopDepthWordOffsetValue];
                }
            }
        }
        return true;
    }
}
