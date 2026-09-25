#include "pch.h"
#include "WorkerXvmSpmdRuntime.h"

#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"
#include "WorkerXvmInterpreter.h"
#include "WorkerXvmVerifier.h"

#include <limits>
#include <vector>

namespace XComputeProbe
{
    namespace
    {
        uint64_t CheckedMultiply(uint64_t left, uint64_t right, char const* code, char const* message)
        {
            if (left != 0 && right > (std::numeric_limits<uint64_t>::max)() / left)
            {
                throw WorkerXvmError(code, message);
            }
            return left * right;
        }

        uint64_t ShapeProduct(std::array<uint32_t, 3> const& shape)
        {
            auto xy = CheckedMultiply(shape[0], shape[1], "xvm.spmd_grid_invalid", "SPMD grid product overflows admission");
            return CheckedMultiply(xy, shape[2], "xvm.spmd_grid_invalid", "SPMD grid product overflows admission");
        }

        std::wstring JsonString(std::wstring const& value)
        {
            return L"\"" + value + L"\"";
        }

        std::wstring ShapeJson(std::array<uint32_t, 3> const& shape)
        {
            return L"[" + std::to_wstring(shape[0]) + L"," + std::to_wstring(shape[1]) + L"," +
                std::to_wstring(shape[2]) + L"]";
        }

        std::wstring BindingJson(WorkerXvmSpmdExecutionPlan const& plan)
        {
            auto generalizedTopology =
                plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV2 ||
                plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV3
                ? L",\"workgroup_thread_count\":64,\"generalized_topology_admitted\":true"
                : L"";
            auto provableWorlds = plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV3
                ? L",\"provable_worlds\":true,\"field_shape\":[1024,1024,1],\"field_format\":\"u32_le\",\"assurance_class\":\"exact_cpu_gpu_differential_v1\""
                : L"";
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"program_id\":" + JsonString(plan.programId) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"grid_shape\":" + ShapeJson(plan.gridShape) +
                L",\"workgroup_shape\":" + ShapeJson(plan.workgroupShape) +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"lane_context\":{\"schema_version\":" + JsonString(WorkerXvmSpmdLaneContextSchemaVersion) +
                L",\"register_binding\":{\"lane_id\":12,\"x\":13,\"y\":14,\"z\":15},\"immutable_seed\":true}" +
                L",\"shared_input_access\":\"read_only\"" +
                L",\"private_state_per_lane\":true" +
                L",\"output_ownership\":\"disjoint_contiguous_slice_per_lane\"" +
                L",\"shared_writes_atomics_barriers\":false" +
                L",\"per_lane_worst_case_fuel\":" + std::to_wstring(plan.perLaneWorstCaseFuel) +
                L",\"aggregate_worst_case_fuel\":" + std::to_wstring(plan.aggregateWorstCaseFuel) +
                L",\"per_lane_memory_bytes\":" + std::to_wstring(plan.perLaneMemoryBytes) +
                L",\"total_private_memory_bytes\":" + std::to_wstring(plan.totalPrivateMemoryBytes) +
                L",\"per_lane_output_bytes\":" + std::to_wstring(plan.perLaneOutputBytes) +
                L",\"total_output_bytes\":" + std::to_wstring(plan.totalOutputBytes) +
                generalizedTopology + provableWorlds +
                L",\"cpu_reference_canonical\":true,\"gpu_execution_admitted\":false}";
        }
    }

    WorkerXvmSpmdExecutionPlan WorkerBuildXvmSpmdExecutionPlan(
        std::array<uint32_t, 3> const& gridShape,
        std::array<uint32_t, 3> const& workgroupShape,
        WorkerXvmProgram const& program,
        WorkerXvmStaticFuelProof const& staticFuelProof,
        WorkerGraphNodeResourceLimits const& limits,
        std::wstring const& programSha256)
    {
        WorkerXvmSpmdExecutionPlan plan;
        if (!WorkerXvmHasCapability(program.capabilities, L"spmd_lane_context_v1"))
        {
            throw WorkerXvmError("xvm.spmd_capability_required", "SPMD execution requires spmd_lane_context_v1");
        }
        if (gridShape[0] == 0 || gridShape[1] == 0 || gridShape[2] == 0)
        {
            throw WorkerXvmError("xvm.spmd_grid_invalid", "SPMD execution requires a nonzero declared grid");
        }
        auto canonicalWorkgroup = workgroupShape == std::array<uint32_t, 3>{ 1, 1, 1 };
        auto generalizedWorkgroup = workgroupShape == std::array<uint32_t, 3>{ 8, 8, 1 };
        if (!canonicalWorkgroup && !generalizedWorkgroup)
        {
            throw WorkerXvmError("xvm.spmd_workgroup_invalid", "SPMD execution admits only workgroup [1,1,1] or generalized [8,8,1]");
        }

        auto laneCount = ShapeProduct(gridShape);
        auto provableWorldsPlan =
            gridShape == std::array<uint32_t, 3>{ 1024, 1024, 1 } &&
            generalizedWorkgroup &&
            WorkerGpuXvmProvableWorldsProfileMatches(
                WorkerGpuXvmProvableWorldsProfile(), program, limits);
        if (laneCount < 2 ||
            (laneCount > WorkerXvmSpmdMaxLogicalLanesValue && !provableWorldsPlan))
        {
            throw WorkerXvmError("xvm.spmd_lane_count_invalid", "SPMD admits 2..4096 generic lanes or the exact PROVABLE_WORLDS_V1 1024x1024 profile");
        }
        if (staticFuelProof.worstCaseFuel == 0 || program.maxFuel == 0 ||
            staticFuelProof.worstCaseFuel > program.maxFuel)
        {
            throw WorkerXvmError("xvm.spmd_fuel_invalid", "SPMD per-lane static fuel proof is outside the program contract");
        }

        auto aggregateWorstCaseFuel = CheckedMultiply(
            laneCount,
            staticFuelProof.worstCaseFuel,
            "xvm.spmd_aggregate_fuel_overflow",
            "SPMD aggregate static fuel overflows admission");
        auto aggregateMaximumFuel = CheckedMultiply(
            laneCount,
            program.maxFuel,
            "xvm.spmd_aggregate_fuel_overflow",
            "SPMD aggregate maximum fuel overflows admission");
        auto totalPrivateMemoryBytes = CheckedMultiply(
            laneCount,
            program.memoryBytes,
            "xvm.spmd_resource_overflow",
            "SPMD private-memory total overflows admission");
        auto totalOutputBytes = CheckedMultiply(
            laneCount,
            program.outputBytes,
            "xvm.spmd_resource_overflow",
            "SPMD output total overflows admission");
        if (limits.fuel < aggregateWorstCaseFuel || limits.fuel > aggregateMaximumFuel ||
            limits.memoryBytes != totalPrivateMemoryBytes || limits.outputBytes != totalOutputBytes)
        {
            throw WorkerXvmError(
                "xvm.spmd_resource_contract_invalid",
                "SPMD node resources must bind aggregate fuel and exact private-memory/output totals");
        }

        plan.admitted = true;
        plan.schemaVersion = provableWorldsPlan
            ? WorkerXvmSpmdExecutionPlanSchemaVersionV3
            : (generalizedWorkgroup
                ? WorkerXvmSpmdExecutionPlanSchemaVersionV2
                : WorkerXvmSpmdExecutionPlanSchemaVersion);
        plan.contractId = provableWorldsPlan
            ? WorkerXvmProvableWorldsCpuContractId
            : (generalizedWorkgroup
                ? WorkerXvmSpmdGeneralizedCpuContractId
                : WorkerXvmSpmdContractId);
        plan.programId = program.programId;
        plan.programSha256 = programSha256;
        plan.gridShape = gridShape;
        plan.workgroupShape = workgroupShape;
        plan.laneCount = static_cast<uint32_t>(laneCount);
        plan.perLaneWorstCaseFuel = staticFuelProof.worstCaseFuel;
        plan.aggregateWorstCaseFuel = aggregateWorstCaseFuel;
        plan.perLaneMemoryBytes = program.memoryBytes;
        plan.totalPrivateMemoryBytes = totalPrivateMemoryBytes;
        plan.perLaneOutputBytes = program.outputBytes;
        plan.totalOutputBytes = totalOutputBytes;
        auto binding = winrt::to_string(winrt::hstring(BindingJson(plan)));
        std::vector<uint8_t> bytes(binding.begin(), binding.end());
        plan.planSha256 = WorkerGpuXvmHashBytes(bytes);
        return plan;
    }

    std::wstring WorkerXvmSpmdExecutionPlanJson(WorkerXvmSpmdExecutionPlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        auto binding = BindingJson(plan);
        binding.pop_back();
        return binding + L",\"plan_sha256\":" + JsonString(plan.planSha256) + L"}";
    }

    bool WorkerXvmSpmdExecutionPlansEqual(
        WorkerXvmSpmdExecutionPlan const& expected,
        WorkerXvmSpmdExecutionPlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            (!expected.admitted ||
                (expected.planSha256 == actual.planSha256 && BindingJson(expected) == BindingJson(actual)));
    }

    WorkerXvmSpmdRunResult WorkerRunXvmSpmdCpuReference(
        WorkerXvmSpmdExecutionPlan const& plan,
        WorkerXvmProgram const& program,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& sharedInputs,
        std::function<bool()> const& cancelRequested)
    {
        auto canonicalPlan = plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersion &&
            plan.workgroupShape == std::array<uint32_t, 3>{ 1, 1, 1 };
        auto generalizedPlan =
            (plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV2 ||
             plan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV3) &&
            plan.workgroupShape == std::array<uint32_t, 3>{ 8, 8, 1 };
        if (!plan.admitted || (!canonicalPlan && !generalizedPlan) || plan.laneCount < 2 ||
            ShapeProduct(plan.gridShape) != plan.laneCount)
        {
            throw WorkerXvmError("xvm.spmd_plan_invalid", "SPMD CPU reference requires an admitted immutable topology plan");
        }

        WorkerXvmSpmdRunResult result;
        result.laneCount = plan.laneCount;
        result.aggregate.controlToken = L"pass";
        result.aggregate.output.reserve(static_cast<size_t>(plan.totalOutputBytes));
        WorkerGraphNodeResourceLimits laneLimits;
        laneLimits.fuel = program.maxFuel;
        laneLimits.memoryBytes = program.memoryBytes;
        laneLimits.outputBytes = program.outputBytes;

        for (uint32_t laneId = 0; laneId < plan.laneCount; ++laneId)
        {
            if (cancelRequested && cancelRequested())
            {
                throw WorkerXvmError("job.canceled", "SPMD CPU reference canceled at a lane boundary");
            }
            auto yz = laneId / plan.gridShape[0];
            auto x = laneId % plan.gridShape[0];
            auto y = yz % plan.gridShape[1];
            auto z = yz / plan.gridShape[1];
            auto state = WorkerCreateXvmMachineState(laneLimits);
            state.registers[WorkerXvmSpmdLaneIdRegisterValue] = laneId;
            state.registers[WorkerXvmSpmdLaneXRegisterValue] = x;
            state.registers[WorkerXvmSpmdLaneYRegisterValue] = y;
            state.registers[WorkerXvmSpmdLaneZRegisterValue] = z;
            auto status = WorkerAdvanceXvmCpuReference(
                program,
                laneLimits,
                sharedInputs,
                state,
                0,
                cancelRequested);
            if (status == WorkerXvmAdvanceStatus::Canceled)
            {
                throw WorkerXvmError("job.canceled", "SPMD CPU reference canceled inside a lane");
            }
            auto lane = WorkerXvmMachineResult(state);
            result.aggregate.output.insert(result.aggregate.output.end(), lane.output.begin(), lane.output.end());
            if (lane.fuelConsumed > (std::numeric_limits<uint64_t>::max)() - result.aggregate.fuelConsumed)
            {
                throw WorkerXvmError("xvm.spmd_aggregate_fuel_overflow", "SPMD runtime fuel total overflows u64");
            }
            result.aggregate.fuelConsumed += lane.fuelConsumed;
            if (lane.controlToken != L"pass" && result.firstFailControlLane == UINT32_MAX)
            {
                result.firstFailControlLane = laneId;
                result.aggregate.controlToken = L"fail";
            }
            if (lane.trap.occurrenceCount != 0 && result.aggregateTrapLane == UINT32_MAX)
            {
                result.aggregateTrapLane = laneId;
                result.aggregate.trap = lane.trap;
            }
        }
        if (result.aggregate.output.size() != plan.totalOutputBytes ||
            result.aggregate.fuelConsumed > plan.aggregateWorstCaseFuel)
        {
            throw WorkerXvmError("xvm.spmd_runtime_bound_violation", "SPMD CPU result exceeded the immutable admitted plan");
        }
        return result;
    }
}
