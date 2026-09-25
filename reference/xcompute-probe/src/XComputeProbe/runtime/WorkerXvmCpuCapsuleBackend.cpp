#include "pch.h"
#include "WorkerXvmCpuCapsuleBackend.h"

#include "../ProbeResult.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ProgramId = L"xvm-cpu-plan-codelet-differential-v1";
        constexpr wchar_t const* ProgramSha256 =
            L"6e4fc53399a76ccb17fc98f280c186fce735582554630a2ebe3e2f316a870370";
        constexpr wchar_t const* ExecutionPlanSha256 =
            L"80887336b9f965f7d0f058b16b5387cc1f6937a3fad9c8c6a42ec885e4de1867";
        constexpr uint64_t CapsuleVersionPacked =
            (1ull << 48) | (3ull << 32);
        constexpr uint32_t CapsuleSelfTestMagic = 0x43504331u;
        constexpr uint64_t CheckpointFuel = 12345ull;
        constexpr uint32_t PrologueFuel = 5u;
        constexpr uint32_t HotLoopBeginPc = 4u;
        constexpr uint32_t HotBodyPc = 5u;
        constexpr uint32_t HotLoopEndPc = 9u;
        constexpr uint32_t PostLoopPc = 10u;
        constexpr uint32_t HotRegister = 1u;
        constexpr uint32_t AbiOverheadSamples = 256u;
        std::atomic<bool> CapsuleQuarantined{ false };

        using AbiFunction = uint32_t(__cdecl*)();
        using BuildFunction = uint64_t(__cdecl*)();
        using SelfTestFunction = uint32_t(__cdecl*)();

        std::wstring AsciiField(char const* value, size_t capacity)
        {
            auto end = std::find(value, value + capacity, '\0');
            if (end == value + capacity)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_profile_not_admitted",
                    "CPU capsule profile metadata is not NUL-terminated");
            }
            return std::wstring(winrt::to_hstring(std::string(value, end)).c_str());
        }

        bool ReservedZero(XComputeCpuHotKernelProfileV1 const& profile)
        {
            return std::all_of(
                std::begin(profile.reserved),
                std::end(profile.reserved),
                [](uint32_t value) { return value == 0; });
        }

        WorkerXvmCpuCapsulePlan BuildReadOnlyPlan(
            WorkerXvmProgram const& program,
            WorkerXvmCpuExecutionPlan const& executionPlan,
            std::wstring const& programSha256)
        {
            if (CapsuleQuarantined.load())
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_quarantined",
                    "the CPU capsule hot-kernel profile is quarantined for this worker activation");
            }
            if (program.programId != ProgramId ||
                programSha256 != ProgramSha256 ||
                executionPlan.planSha256 != ExecutionPlanSha256 ||
                executionPlan.programSha256 != programSha256 ||
                program.staticWorstCaseFuel != 327695ull)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_profile_not_admitted",
                    "program and immutable execution plan do not match the admitted CPU capsule profile");
            }

            auto selection = WorkerResolveCpuCapsuleModule();
            if (selection.versionPacked != CapsuleVersionPacked ||
                selection.version != L"1.3.0.0")
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_package_version_mismatch",
                    "the active worker requires the additive CPU capsule 1.3.0.0 ABI exactly");
            }

            WorkerXvmCpuCapsulePlan plan;
            plan.admitted = true;
            plan.schemaVersion = WorkerXvmCpuCapsulePlanSchemaVersion;
            plan.profileId = WorkerXvmCpuCapsuleProfileId;
            plan.profileContractSha256 = WorkerXvmCpuCapsuleProfileContractSha256;
            plan.programSha256 = programSha256;
            plan.executionPlanSha256 = executionPlan.planSha256;
            plan.packageFullName = selection.packageFullName;
            plan.packageVersion = selection.version;
            plan.packageInstalledPath = selection.installedRoot.wstring();
            plan.modulePath = selection.modulePath.wstring();
            plan.moduleSha256 = selection.moduleSha256;
            plan.moduleBytes = selection.moduleBytes;
            plan.abiVersion = XComputeCpuHotKernelAbiVersionV1;
            plan.loopIterations = XComputeCpuHotKernelLoopIterationsV1;
            plan.maximumChunkIterations = XComputeCpuHotKernelMaximumChunkIterationsV1;
            return plan;
        }

        bool SelectionMatchesPlan(
            WorkerCpuCapsuleModuleSelection const& selection,
            WorkerXvmCpuCapsulePlan const& plan)
        {
            return selection.packageFullName == plan.packageFullName &&
                selection.version == plan.packageVersion &&
                selection.installedRoot.wstring() == plan.packageInstalledPath &&
                selection.modulePath.wstring() == plan.modulePath &&
                selection.moduleSha256 == plan.moduleSha256 &&
                selection.moduleBytes == plan.moduleBytes;
        }

        XComputeCpuCapsuleRunHotKernelChunkV1Function ValidateLoadedCapsule(
            WorkerCpuCapsuleModuleReference const& module,
            WorkerCpuCapsuleModuleSelection const& selection)
        {
            auto abi = reinterpret_cast<AbiFunction>(module.Export("XComputeCpuCapsuleAbiVersion"));
            auto build = reinterpret_cast<BuildFunction>(module.Export("XComputeCpuCapsuleBuildVersionPacked"));
            auto selfTest = reinterpret_cast<SelfTestFunction>(module.Export("XComputeCpuCapsuleSelfTest"));
            auto hotAbi = reinterpret_cast<XComputeCpuCapsuleHotKernelAbiVersionFunction>(
                module.Export("XComputeCpuCapsuleHotKernelAbiVersion"));
            auto query = reinterpret_cast<XComputeCpuCapsuleQueryHotKernelV1Function>(
                module.Export("XComputeCpuCapsuleQueryHotKernelV1"));
            auto runChunk = reinterpret_cast<XComputeCpuCapsuleRunHotKernelChunkV1Function>(
                module.Export("XComputeCpuCapsuleRunHotKernelChunkV1"));

            if (abi() != 1u || build() != selection.versionPacked ||
                selfTest() != CapsuleSelfTestMagic ||
                hotAbi() != XComputeCpuHotKernelAbiVersionV1)
            {
                CapsuleQuarantined.store(true);
                throw WorkerXvmError(
                    "xvm.cpu_capsule_abi_mismatch",
                    "CPU capsule core, build or hot-kernel ABI does not match the admitted contract");
            }

            XComputeCpuHotKernelProfileV1 profile{};
            profile.structSize = static_cast<uint32_t>(sizeof(profile));
            if (query(&profile) != XComputeCpuHotKernelStatusOk ||
                profile.structSize != sizeof(profile) ||
                profile.abiVersion != XComputeCpuHotKernelAbiVersionV1 ||
                !ReservedZero(profile) ||
                AsciiField(profile.profileId, sizeof(profile.profileId)) != WorkerXvmCpuCapsuleProfileId ||
                AsciiField(profile.profileContractSha256, sizeof(profile.profileContractSha256)) !=
                    WorkerXvmCpuCapsuleProfileContractSha256 ||
                AsciiField(profile.programSha256, sizeof(profile.programSha256)) != ProgramSha256 ||
                AsciiField(profile.executionPlanSha256, sizeof(profile.executionPlanSha256)) != ExecutionPlanSha256 ||
                profile.loopIterations != XComputeCpuHotKernelLoopIterationsV1 ||
                profile.maximumChunkIterations != XComputeCpuHotKernelMaximumChunkIterationsV1 ||
                profile.addConstant != XComputeCpuHotKernelAddConstantV1 ||
                profile.xorConstant != XComputeCpuHotKernelXorConstantV1 ||
                profile.multiplyConstant != XComputeCpuHotKernelMultiplyConstantV1 ||
                profile.rotateBits != XComputeCpuHotKernelRotateBitsV1 ||
                profile.actualFuel != XComputeCpuHotKernelActualFuelV1)
            {
                CapsuleQuarantined.store(true);
                throw WorkerXvmError(
                    "xvm.cpu_capsule_profile_not_admitted",
                    "CPU capsule query metadata does not match the exact admitted hot-kernel profile");
            }
            return runChunk;
        }

        struct CandidateExecution
        {
            WorkerXvmRunResult result;
            WorkerXvmMachineState state;
            uint64_t chunkCallCount = 0;
            double elapsedMs = 0.0;
            bool canceled = false;
        };

        CandidateExecution ExecuteCandidate(
            WorkerXvmProgram const& program,
            WorkerGraphNodeResourceLimits const& limits,
            std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
            WorkerXvmMachineState const& initialState,
            XComputeCpuCapsuleRunHotKernelChunkV1Function runChunk,
            uint32_t targetIterations,
            bool finishProgram,
            std::function<bool()> const& cancelRequested)
        {
            CandidateExecution candidate;
            candidate.state = initialState;
            auto started = std::chrono::steady_clock::now();
            auto prologue = WorkerAdvanceXvmCpuReference(
                program, limits, inputs, candidate.state, PrologueFuel, cancelRequested);
            if (prologue == WorkerXvmAdvanceStatus::Canceled)
            {
                candidate.canceled = true;
                candidate.elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                return candidate;
            }
            if (prologue != WorkerXvmAdvanceStatus::Checkpoint ||
                candidate.state.pc != HotBodyPc ||
                candidate.state.fuelConsumed != PrologueFuel ||
                candidate.state.loopStack.size() != 1 ||
                candidate.state.loopStack.back().beginPc != HotLoopBeginPc ||
                candidate.state.loopStack.back().endPc != HotLoopEndPc ||
                candidate.state.loopStack.back().remaining != XComputeCpuHotKernelLoopIterationsV1)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_state_invalid",
                    "canonical prologue did not reach the admitted hot-kernel safepoint");
            }

            uint32_t completed = 0;
            while (completed < targetIterations)
            {
                if (cancelRequested && cancelRequested())
                {
                    candidate.canceled = true;
                    candidate.elapsedMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();
                    return candidate;
                }
                auto count = (std::min)(
                    XComputeCpuHotKernelMaximumChunkIterationsV1,
                    targetIterations - completed);
                XComputeCpuHotKernelRequestV1 request{};
                request.structSize = static_cast<uint32_t>(sizeof(request));
                request.iterations = count;
                request.initialValue = candidate.state.registers[HotRegister];
                XComputeCpuHotKernelResultV1 output{};
                output.structSize = static_cast<uint32_t>(sizeof(output));
                auto status = runChunk(&request, &output);
                if (status != XComputeCpuHotKernelStatusOk ||
                    output.status != XComputeCpuHotKernelStatusOk ||
                    output.structSize != static_cast<uint32_t>(sizeof(output)) ||
                    output.iterationsCompleted != count ||
                    output.bodySourceInstructions != static_cast<uint64_t>(count) * 4ull ||
                    !std::all_of(std::begin(output.reserved), std::end(output.reserved),
                        [](uint32_t value) { return value == 0; }))
                {
                    CapsuleQuarantined.store(true);
                    throw WorkerXvmError(
                        "xvm.cpu_capsule_execution_failed",
                        "CPU capsule hot-kernel chunk violated its admitted ABI result contract");
                }
                candidate.state.registers[HotRegister] = output.finalValue;
                candidate.state.fuelConsumed += static_cast<uint64_t>(count) * 5ull;
                completed += count;
                auto& frame = candidate.state.loopStack.back();
                if (count > frame.remaining)
                {
                    CapsuleQuarantined.store(true);
                    throw WorkerXvmError(
                        "xvm.cpu_capsule_state_invalid",
                        "CPU capsule chunk exceeded the admitted loop frame");
                }
                frame.remaining -= count;
                if (frame.remaining == 0)
                {
                    candidate.state.loopStack.pop_back();
                    candidate.state.pc = PostLoopPc;
                }
                else
                {
                    candidate.state.pc = HotBodyPc;
                }
                ++candidate.chunkCallCount;
                WorkerValidateXvmMachineState(program, limits, candidate.state);
            }

            if (finishProgram)
            {
                if (targetIterations != XComputeCpuHotKernelLoopIterationsV1 ||
                    !candidate.state.loopStack.empty() ||
                    candidate.state.pc != PostLoopPc)
                {
                    throw WorkerXvmError(
                        "xvm.cpu_capsule_state_invalid",
                        "CPU capsule full run did not close the admitted hot loop");
                }
                auto status = WorkerAdvanceXvmCpuReference(
                    program, limits, inputs, candidate.state, 0, cancelRequested);
                if (status == WorkerXvmAdvanceStatus::Canceled)
                {
                    candidate.canceled = true;
                }
                else
                {
                    candidate.result = WorkerXvmMachineResult(candidate.state);
                }
            }
            candidate.elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return candidate;
        }

        double DoubleJsonValue(double value)
        {
            return value;
        }

        std::wstring DoubleJson(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(6) << DoubleJsonValue(value);
            return out.str();
        }
    }

    void WorkerQuarantineXvmCpuCapsule() noexcept
    {
        CapsuleQuarantined.store(true);
    }

    bool WorkerIsXvmCpuCapsuleQuarantined() noexcept
    {
        return CapsuleQuarantined.load();
    }

    std::wstring WorkerXvmCpuCapsuleQuarantineDiagnosticJson(std::wstring const& protocolVersion)
    {
        auto before = WorkerIsXvmCpuCapsuleQuarantined();
        WorkerQuarantineXvmCpuCapsule();
        auto afterFirst = WorkerIsXvmCpuCapsuleQuarantined();
        WorkerQuarantineXvmCpuCapsule();
        auto afterSecond = WorkerIsXvmCpuCapsuleQuarantined();
        std::wostringstream out;
        out << L"{\"ok\":true,\"protocol_version\":" << JsonString(protocolVersion)
            << L",\"command\":\"test_cpu_capsule_quarantine_lifecycle\""
            << L",\"schema_version\":\"cpu-capsule-quarantine-diagnostic-v1\""
            << L",\"quarantined_before\":" << (before ? L"true" : L"false")
            << L",\"quarantined_after_first\":" << (afterFirst ? L"true" : L"false")
            << L",\"quarantined_after_second\":" << (afterSecond ? L"true" : L"false")
            << L",\"idempotent\":" << ((!before && afterFirst && afterSecond) ? L"true" : L"false")
            << L",\"scope\":\"current_worker_activation_candidate_only\""
            << L",\"development_diagnostic_only\":true,\"production_authority\":false}";
        return out.str();
    }

    WorkerXvmCpuCapsulePlan WorkerBuildXvmCpuCapsulePlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& executionPlan,
        std::wstring const& programSha256)
    {
        return BuildReadOnlyPlan(program, executionPlan, programSha256);
    }

    bool WorkerXvmCpuCapsulePlansEqual(
        WorkerXvmCpuCapsulePlan const& expected,
        WorkerXvmCpuCapsulePlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            expected.schemaVersion == actual.schemaVersion &&
            expected.profileId == actual.profileId &&
            expected.profileContractSha256 == actual.profileContractSha256 &&
            expected.programSha256 == actual.programSha256 &&
            expected.executionPlanSha256 == actual.executionPlanSha256 &&
            expected.packageFullName == actual.packageFullName &&
            expected.packageVersion == actual.packageVersion &&
            expected.packageInstalledPath == actual.packageInstalledPath &&
            expected.modulePath == actual.modulePath &&
            expected.moduleSha256 == actual.moduleSha256 &&
            expected.moduleBytes == actual.moduleBytes &&
            expected.abiVersion == actual.abiVersion &&
            expected.loopIterations == actual.loopIterations &&
            expected.maximumChunkIterations == actual.maximumChunkIterations;
    }

    std::wstring WorkerXvmCpuCapsulePlanJson(WorkerXvmCpuCapsulePlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        std::wostringstream out;
        out << LR"json({"schema_version":)json" << JsonString(plan.schemaVersion)
            << LR"json(,"admitted":true,"profile_id":)json" << JsonString(plan.profileId)
            << LR"json(,"profile_contract_sha256":)json" << JsonString(plan.profileContractSha256)
            << LR"json(,"program_sha256":)json" << JsonString(plan.programSha256)
            << LR"json(,"execution_plan_sha256":)json" << JsonString(plan.executionPlanSha256)
            << LR"json(,"package_full_name":)json" << JsonString(plan.packageFullName)
            << LR"json(,"package_version":)json" << JsonString(plan.packageVersion)
            << LR"json(,"package_installed_path":)json" << JsonString(plan.packageInstalledPath)
            << LR"json(,"module_path":)json" << JsonString(plan.modulePath)
            << LR"json(,"module_sha256":)json" << JsonString(plan.moduleSha256)
            << LR"json(,"module_bytes":)json" << plan.moduleBytes
            << LR"json(,"abi_version":)json" << plan.abiVersion
            << LR"json(,"loop_iterations":)json" << plan.loopIterations
            << LR"json(,"maximum_chunk_iterations":)json" << plan.maximumChunkIterations
            << LR"json(,"immutable":true})json";
        return out.str();
    }

    WorkerXvmCpuCapsuleDifferentialResult WorkerRunXvmCpuCapsuleDifferential(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& executionPlan,
        WorkerXvmCpuCapsulePlan const& admittedPlan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested)
    {
        auto currentPlan = BuildReadOnlyPlan(program, executionPlan, admittedPlan.programSha256);
        if (!WorkerXvmCpuCapsulePlansEqual(admittedPlan, currentPlan))
        {
            throw WorkerXvmError(
                "xvm.cpu_capsule_plan_mismatch",
                "CPU capsule plan differs from immutable full-graph pre-admission");
        }

        auto selection = WorkerResolveCpuCapsuleModule();
        if (!SelectionMatchesPlan(selection, currentPlan))
        {
            throw WorkerXvmError(
                "xvm.cpu_capsule_plan_mismatch",
                "selected CPU capsule identity, path or hash changed after immutable admission");
        }
        std::unique_ptr<WorkerCpuCapsuleModuleReference> module;
        try
        {
            module = std::make_unique<WorkerCpuCapsuleModuleReference>(selection);
        }
        catch (WorkerXvmError const&)
        {
            WorkerQuarantineXvmCpuCapsule();
            throw;
        }
        XComputeCpuCapsuleRunHotKernelChunkV1Function runChunk = nullptr;
        try
        {
            runChunk = ValidateLoadedCapsule(*module, selection);
        }
        catch (WorkerXvmError const&)
        {
            WorkerQuarantineXvmCpuCapsule();
            throw;
        }

        auto cold = ExecuteCandidate(
            program, limits, inputs, initialState, runChunk,
            XComputeCpuHotKernelLoopIterationsV1, true, cancelRequested);
        if (cold.canceled)
        {
            WorkerXvmCpuCapsuleDifferentialResult canceled;
            canceled.canceled = true;
            canceled.canceledState = std::move(cold.state);
            canceled.moduleLoadElapsedMs = module->LoadElapsedMs();
            canceled.moduleWasLoadedBefore = module->WasLoadedBefore();
            canceled.loadedModulePath = module->LoadedModulePath().wstring();
            return canceled;
        }

        auto referenceState = initialState;
        auto referenceStarted = std::chrono::steady_clock::now();
        auto referenceStatus = WorkerAdvanceXvmCpuReference(
            program, limits, inputs, referenceState, 0, cancelRequested);
        auto referenceElapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - referenceStarted).count();
        if (referenceStatus == WorkerXvmAdvanceStatus::Canceled)
        {
            WorkerXvmCpuCapsuleDifferentialResult canceled;
            canceled.canceled = true;
            canceled.canceledState = std::move(referenceState);
            canceled.referenceElapsedMs = referenceElapsed;
            canceled.moduleLoadElapsedMs = module->LoadElapsedMs();
            canceled.moduleWasLoadedBefore = module->WasLoadedBefore();
            canceled.loadedModulePath = module->LoadedModulePath().wstring();
            return canceled;
        }
        auto referenceResult = WorkerXvmMachineResult(referenceState);

        auto overheadStarted = std::chrono::steady_clock::now();
        for (uint32_t sample = 0; sample < AbiOverheadSamples; ++sample)
        {
            XComputeCpuHotKernelRequestV1 request{};
            request.structSize = static_cast<uint32_t>(sizeof(request));
            XComputeCpuHotKernelResultV1 output{};
            output.structSize = static_cast<uint32_t>(sizeof(output));
            if (runChunk(&request, &output) != XComputeCpuHotKernelStatusOk ||
                output.status != XComputeCpuHotKernelStatusOk ||
                output.finalValue != 0 ||
                output.iterationsCompleted != 0)
            {
                WorkerQuarantineXvmCpuCapsule();
                throw WorkerXvmError(
                    "xvm.cpu_capsule_execution_failed",
                    "CPU capsule ABI-overhead probe violated the zero-iteration contract");
            }
        }
        auto overheadNs = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - overheadStarted).count() /
            static_cast<double>(AbiOverheadSamples);

        auto warm = ExecuteCandidate(
            program, limits, inputs, initialState, runChunk,
            XComputeCpuHotKernelLoopIterationsV1, true, cancelRequested);
        if (warm.canceled)
        {
            WorkerXvmCpuCapsuleDifferentialResult canceled;
            canceled.canceled = true;
            canceled.canceledState = std::move(warm.state);
            canceled.referenceElapsedMs = referenceElapsed;
            canceled.moduleLoadElapsedMs = module->LoadElapsedMs();
            canceled.moduleWasLoadedBefore = module->WasLoadedBefore();
            canceled.loadedModulePath = module->LoadedModulePath().wstring();
            return canceled;
        }
        if (!WorkerXvmRunResultsEqual(referenceResult, cold.result) ||
            !WorkerXvmRunResultsEqual(referenceResult, warm.result))
        {
            CapsuleQuarantined.store(true);
            throw WorkerXvmError(
                "xvm.cpu_capsule_differential_mismatch",
                "CPU capsule result diverged from the canonical interpreter; candidate quarantined before publication");
        }

        WorkerXvmCpuCapsuleDifferentialResult result;
        result.result = std::move(warm.result);
        result.plan = std::move(currentPlan);
        result.loadedModulePath = module->LoadedModulePath().wstring();
        result.referenceElapsedMs = referenceElapsed;
        result.moduleLoadElapsedMs = module->LoadElapsedMs();
        result.moduleWasLoadedBefore = module->WasLoadedBefore();
        result.coldElapsedMs = cold.elapsedMs;
        result.warmElapsedMs = warm.elapsedMs;
        result.abiCallOverheadNs = overheadNs;
        result.chunkCallCount = warm.chunkCallCount;
        if (checkpointFuel != 0)
        {
            if (checkpointFuel != CheckpointFuel)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_checkpoint_mismatch",
                    "CPU capsule profile admits only its exact verified checkpoint fuel");
            }
            auto referenceCheckpoint = initialState;
            auto referenceCheckpointStatus = WorkerAdvanceXvmCpuReference(
                program, limits, inputs, referenceCheckpoint, checkpointFuel, cancelRequested);
            if (referenceCheckpointStatus == WorkerXvmAdvanceStatus::Canceled)
            {
                result.canceled = true;
                result.canceledState = std::move(referenceCheckpoint);
                return result;
            }
            auto checkpointStarted = std::chrono::steady_clock::now();
            auto candidateCheckpoint = ExecuteCandidate(
                program, limits, inputs, initialState, runChunk,
                static_cast<uint32_t>((checkpointFuel - PrologueFuel) / 5ull),
                false, cancelRequested);
            result.checkpointElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - checkpointStarted).count();
            if (candidateCheckpoint.canceled)
            {
                result.canceled = true;
                result.canceledState = std::move(candidateCheckpoint.state);
                return result;
            }
            if (referenceCheckpointStatus != WorkerXvmAdvanceStatus::Checkpoint ||
                !WorkerXvmMachineStatesEqual(referenceCheckpoint, candidateCheckpoint.state))
            {
                CapsuleQuarantined.store(true);
                throw WorkerXvmError(
                    "xvm.cpu_capsule_checkpoint_mismatch",
                    "CPU capsule checkpoint state diverged from the canonical interpreter");
            }
            result.checkpointState = std::move(candidateCheckpoint.state);
            result.checkpointVerified = true;
        }
        return result;
    }

    std::wstring WorkerXvmCpuCapsuleExecutionJson(
        WorkerXvmCpuCapsuleDifferentialResult const& result,
        bool deoptimizedToInterpreter)
    {
        auto warmSpeedup = result.warmElapsedMs > 0.0
            ? result.referenceElapsedMs / result.warmElapsedMs
            : 0.0;
        std::wostringstream out;
        out << LR"json({"enabled":true,"schema_version":"xvm-cpu-packaged-capsule-execution-result-v1")json"
            << LR"json(,"gate_id":"XVM_CPU_PACKAGED_CAPSULE_HOT_KERNEL_V1","plan":)json"
            << WorkerXvmCpuCapsulePlanJson(result.plan)
            << LR"json(,"reference_elapsed_ms":)json" << DoubleJson(result.referenceElapsedMs)
            << LR"json(,"loaded_module_path":)json" << JsonString(result.loadedModulePath)
            << LR"json(,"loaded_module_path_matches_plan":true)json"
            << LR"json(,"module_was_loaded_before":)json" << (result.moduleWasLoadedBefore ? L"true" : L"false")
            << LR"json(,"module_load_elapsed_ms":)json" << DoubleJson(result.moduleLoadElapsedMs)
            << LR"json(,"module_load_measurement_scope":"first_capsule_load_in_graph_execution")json"
            << LR"json(,"cold_elapsed_ms":)json" << DoubleJson(result.coldElapsedMs)
            << LR"json(,"cold_execution_scope":"first_full_kernel_after_contract_validation_before_abi_probe")json"
            << LR"json(,"warm_elapsed_ms":)json" << DoubleJson(result.warmElapsedMs)
            << LR"json(,"warm_execution_scope":"second_full_kernel_after_abi_probe")json"
            << LR"json(,"warm_speedup_ratio":)json" << DoubleJson(warmSpeedup)
            << LR"json(,"abi_call_overhead_ns":)json" << DoubleJson(result.abiCallOverheadNs)
            << LR"json(,"chunk_call_count":)json" << result.chunkCallCount
            << LR"json(,"cold_exact_match":true,"warm_exact_match":true)json"
            << LR"json(,"fuel_exact_match":true,"trap_exact_match":true,"checkpoint_exact_match":)json"
            << (result.checkpointVerified ? L"true" : L"false")
            << LR"json(,"checkpoint_elapsed_ms":)json" << DoubleJson(result.checkpointElapsedMs)
            << LR"json(,"snapshot_schema":"xvm-state-snapshot-v5","deoptimized_to_interpreter":)json"
            << (deoptimizedToInterpreter ? L"true" : L"false")
            << LR"json(,"canonical_backend":"cpu_reference")json"
            << LR"json(,"selected_backend":"cpu_packaged_capsule_hot_kernel")json"
            << LR"json(,"fallback_backend":"cpu_reference","fallback_available":true)json"
            << LR"json(,"candidate_quarantined":false,"mismatch_policy":"quarantine_before_publication")json"
            << LR"json(,"production_authority":false,"runtime_native_codegen":false)json"
            << LR"json(,"client_native_payload_accepted":false})json";
        return out.str();
    }
}
