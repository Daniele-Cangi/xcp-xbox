#include "pch.h"
#include "WorkerXvmFuelAnalysis.h"

#include "WorkerXvmIsa.h"

#include <algorithm>
#include <map>
#include <vector>

namespace XComputeProbe
{
    namespace
    {
        struct FuelRegion
        {
            uint32_t beginPc = 0;
            uint32_t endPc = 0;
        };

        class StaticFuelAnalyzer
        {
        public:
            explicit StaticFuelAnalyzer(WorkerXvmProgram const& program) :
                m_program(program),
                m_instructionCount(static_cast<uint32_t>(
                    program.words.size() / WorkerXvmInstructionWords())),
                m_limit(program.maxFuel),
                m_exceeded(program.maxFuel + 1)
            {
                if (m_instructionCount == 0 || m_limit == UINT64_MAX)
                {
                    throw WorkerXvmError(
                        "xvm.static_fuel_analysis_invalid",
                        "static fuel analysis requires bounded non-empty bytecode");
                }

                if (program.functions.empty())
                {
                    m_regions.push_back({ 0, m_instructionCount - 1 });
                }
                else
                {
                    m_regions.push_back({ 0, program.functions.front().entryPc - 1 });
                    for (auto const& function : program.functions)
                    {
                        m_regions.push_back({ function.entryPc, function.endPc });
                    }
                }

                m_regionState.resize(m_regions.size(), 0);
                m_regionCost.resize(m_regions.size(), 0);
                m_loopEndByBegin.assign(m_instructionCount, UINT32_MAX);
                m_loopIterationsByBegin.assign(m_instructionCount, 0);

                for (size_t regionIndex = 1; regionIndex < m_regions.size(); ++regionIndex)
                {
                    m_functionRegionByEntry.emplace(
                        m_regions[regionIndex].beginPc,
                        regionIndex);
                }
                for (uint32_t pc = 0; pc < m_instructionCount; ++pc)
                {
                    if (Opcode(pc) == WorkerXvmOpcode::LoopBegin)
                    {
                        m_loopIterationsByBegin[pc] = Word(pc, 1);
                        m_loopEndByBegin[pc] = Word(pc, 2);
                    }
                }
            }

            uint64_t AnalyzeMain()
            {
                return AnalyzeRegion(0);
            }

            uint64_t RegionCount() const
            {
                return static_cast<uint64_t>(m_regions.size());
            }

        private:
            uint32_t Word(uint32_t pc, uint32_t word) const
            {
                return m_program.words[
                    (static_cast<size_t>(pc) * WorkerXvmInstructionWords()) + word];
            }

            WorkerXvmOpcode Opcode(uint32_t pc) const
            {
                return static_cast<WorkerXvmOpcode>(Word(pc, 0));
            }

            uint64_t Add(uint64_t left, uint64_t right) const
            {
                if (left > m_limit || right > m_limit || left > m_limit - right)
                {
                    return m_exceeded;
                }
                return left + right;
            }

            uint64_t Multiply(uint64_t left, uint64_t right) const
            {
                if (left == 0 || right == 0)
                {
                    return 0;
                }
                if (left > m_limit || right > m_limit || left > m_limit / right)
                {
                    return m_exceeded;
                }
                return left * right;
            }

            uint64_t AnalyzeRegion(size_t regionIndex)
            {
                if (regionIndex >= m_regions.size())
                {
                    throw WorkerXvmError(
                        "xvm.static_fuel_analysis_invalid",
                        "static fuel analysis resolved an invalid code region");
                }
                if (m_regionState[regionIndex] == 2)
                {
                    return m_regionCost[regionIndex];
                }
                if (m_regionState[regionIndex] == 1)
                {
                    throw WorkerXvmError(
                        "xvm.call_cycle_invalid",
                        "static fuel analysis requires an acyclic call graph");
                }

                m_regionState[regionIndex] = 1;
                auto const& region = m_regions[regionIndex];
                auto cost = AnalyzeSegment(
                    region.beginPc,
                    region.endPc + 1,
                    regionIndex);
                m_regionCost[regionIndex] = cost;
                m_regionState[regionIndex] = 2;
                return cost;
            }

            uint64_t SegmentSuccessor(
                std::vector<uint64_t> const& costs,
                uint32_t target,
                uint32_t beginPc,
                uint32_t endExclusive) const
            {
                if (target < beginPc || target > endExclusive)
                {
                    throw WorkerXvmError(
                        "xvm.static_fuel_analysis_invalid",
                        "verified control flow escaped its static analysis segment");
                }
                return costs[target];
            }

            uint64_t AnalyzeSegment(
                uint32_t beginPc,
                uint32_t endExclusive,
                size_t regionIndex)
            {
                if (beginPc >= endExclusive || endExclusive > m_instructionCount)
                {
                    throw WorkerXvmError(
                        "xvm.static_fuel_analysis_invalid",
                        "static fuel analysis received an invalid code segment");
                }

                std::vector<uint64_t> costs(static_cast<size_t>(endExclusive) + 1, 0);
                for (uint32_t pc = endExclusive; pc-- > beginPc;)
                {
                    auto opcode = Opcode(pc);
                    auto successor = costs[pc + 1];
                    switch (opcode)
                    {
                    case WorkerXvmOpcode::Halt:
                    case WorkerXvmOpcode::Return:
                        costs[pc] = 1;
                        break;
                    case WorkerXvmOpcode::BranchIfZero:
                    {
                        auto branchCost = SegmentSuccessor(
                            costs,
                            Word(pc, 2),
                            beginPc,
                            endExclusive);
                        costs[pc] = Add(1, (std::max)(successor, branchCost));
                        break;
                    }
                    case WorkerXvmOpcode::BreakIfZero:
                    case WorkerXvmOpcode::ContinueIfZero:
                    case WorkerXvmOpcode::TrapIfZero:
                    {
                        auto target = Word(pc, 3);
                        if (opcode == WorkerXvmOpcode::BreakIfZero)
                        {
                            ++target;
                        }
                        auto branchCost = SegmentSuccessor(
                            costs,
                            target,
                            beginPc,
                            endExclusive);
                        costs[pc] = Add(1, (std::max)(successor, branchCost));
                        break;
                    }
                    case WorkerXvmOpcode::LoopBegin:
                    {
                        auto loopEndPc = m_loopEndByBegin[pc];
                        auto iterations = m_loopIterationsByBegin[pc];
                        if (loopEndPc == UINT32_MAX || iterations == 0 ||
                            loopEndPc < pc + 1 || loopEndPc >= endExclusive)
                        {
                            throw WorkerXvmError(
                                "xvm.static_fuel_analysis_invalid",
                                "verified loop ownership is invalid during static fuel analysis");
                        }
                        auto bodyCost = AnalyzeSegment(
                            pc + 1,
                            loopEndPc + 1,
                            regionIndex);
                        auto loopCost = Add(1, Multiply(iterations, bodyCost));
                        costs[pc] = Add(loopCost, costs[loopEndPc + 1]);
                        break;
                    }
                    case WorkerXvmOpcode::Call:
                    {
                        auto target = m_functionRegionByEntry.find(Word(pc, 1));
                        if (target == m_functionRegionByEntry.end())
                        {
                            throw WorkerXvmError(
                                "xvm.static_fuel_analysis_invalid",
                                "verified call target is unavailable during static fuel analysis");
                        }
                        auto functionCost = AnalyzeRegion(target->second);
                        costs[pc] = Add(Add(1, functionCost), successor);
                        break;
                    }
                    case WorkerXvmOpcode::IfZero:
                    {
                        auto elsePc = Word(pc, 2);
                        auto endIfPc = Word(pc, 3);
                        auto branchPc = elsePc < endIfPc ? elsePc + 1 : endIfPc + 1;
                        auto branchCost = SegmentSuccessor(
                            costs,
                            branchPc,
                            beginPc,
                            endExclusive);
                        costs[pc] = Add(1, (std::max)(successor, branchCost));
                        break;
                    }
                    case WorkerXvmOpcode::Else:
                    {
                        auto branchCost = SegmentSuccessor(
                            costs,
                            Word(pc, 1) + 1,
                            beginPc,
                            endExclusive);
                        costs[pc] = Add(1, branchCost);
                        break;
                    }
                    default:
                        costs[pc] = Add(1, successor);
                        break;
                    }
                }
                return costs[beginPc];
            }

            WorkerXvmProgram const& m_program;
            uint32_t m_instructionCount = 0;
            uint64_t m_limit = 0;
            uint64_t m_exceeded = 0;
            std::vector<FuelRegion> m_regions;
            std::map<uint32_t, size_t> m_functionRegionByEntry;
            std::vector<uint8_t> m_regionState;
            std::vector<uint64_t> m_regionCost;
            std::vector<uint32_t> m_loopEndByBegin;
            std::vector<uint64_t> m_loopIterationsByBegin;
        };
    }

    WorkerXvmStaticFuelProof WorkerAnalyzeXvmStaticFuel(
        WorkerXvmProgram const& program)
    {
        StaticFuelAnalyzer analyzer(program);
        auto worstCaseFuel = analyzer.AnalyzeMain();
        if (worstCaseFuel > program.maxFuel)
        {
            throw WorkerXvmError(
                "xvm.static_fuel_program_limit_insufficient",
                "program max_fuel is smaller than its verified worst-case instruction bound");
        }

        WorkerXvmStaticFuelProof proof;
        proof.worstCaseFuel = worstCaseFuel;
        proof.declaredProgramMaxFuel = program.maxFuel;
        proof.instructionCount =
            program.words.size() / WorkerXvmInstructionWords();
        proof.regionCount = analyzer.RegionCount();
        return proof;
    }

    std::wstring WorkerXvmStaticFuelProofJson(
        WorkerXvmStaticFuelProof const& proof,
        uint64_t admittedNodeFuel)
    {
        return std::wstring(L"{\"schema_version\":\"") +
            WorkerXvmStaticFuelProofSchemaVersion +
            L"\",\"algorithm\":\"structured_cfg_longest_path_v1\"" +
            L",\"instruction_cost_model\":\"one_per_executed_instruction\"" +
            L",\"worst_case_fuel\":" + std::to_wstring(proof.worstCaseFuel) +
            L",\"declared_program_max_fuel\":" + std::to_wstring(proof.declaredProgramMaxFuel) +
            L",\"admitted_node_fuel\":" + std::to_wstring(admittedNodeFuel) +
            L",\"instruction_count\":" + std::to_wstring(proof.instructionCount) +
            L",\"region_count\":" + std::to_wstring(proof.regionCount) +
            L",\"exact\":true" +
            L",\"bounded_loops_accounted\":true" +
            L",\"acyclic_calls_accounted\":true" +
            L",\"branch_maxima_accounted\":true" +
            L",\"deterministic_trap_paths_bounded\":true" +
            L",\"runtime_fuel_defense_in_depth\":true}";
    }
}
