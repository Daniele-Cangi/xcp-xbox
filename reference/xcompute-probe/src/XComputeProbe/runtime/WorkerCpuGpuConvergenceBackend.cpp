#include "pch.h"
#include "WorkerCpuGpuConvergenceBackend.h"

#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ProgramId = L"provable_worlds_field_v1";
        constexpr wchar_t const* ProgramSha256 =
            L"5c8c06aa8700f0dcec90263c5a4243cd4ecfebbaaec25e870074e1906e8fc836";
        constexpr wchar_t const* SpmdPlanSha256 =
            L"ecb2d61bbff32fd339dc2034091f455faeed39142d377fe5de686761f9227bc1";
        constexpr wchar_t const* GpuPlanSha256 =
            L"399c105dbea185cb0ad6c4c49859ffbaff2cba089eda383063024f059770594a";
        constexpr wchar_t const* GpuContractSha256 =
            L"8587a2fc3a26782861cce2c621cbccaafe78ff249f502da9c4759ebb9d9d89b6";
        constexpr wchar_t const* ShaderSha256 =
            L"835a745b098711b20c5fbf9d6b32f55ff68623e786e2854b052f86de6e08579d";
        constexpr uint64_t CapsuleVersionPacked =
            (1ull << 48) | (3ull << 32);
        constexpr uint32_t CapsuleSelfTestMagic = 0x43504331u;
        std::atomic<bool> CandidateQuarantined{ false };
        std::mutex ExactTileMutex;
        std::unique_ptr<WorkerCpuCapsuleModuleReference> ExactTileModule;
        WorkerCpuCapsuleModuleSelection ExactTileSelection;
        XComputeCpuCapsuleRunProvableWorldsChunkV1Function ExactTileRunChunk = nullptr;

        using AbiFunction = uint32_t(__cdecl*)();
        using BuildFunction = uint64_t(__cdecl*)();
        using SelfTestFunction = uint32_t(__cdecl*)();

        std::wstring AsciiField(char const* value, size_t capacity)
        {
            auto end = std::find(value, value + capacity, '\0');
            if (end == value + capacity)
            {
                throw WorkerXvmError(
                    "xvm.cpu_gpu_convergence_profile_not_admitted",
                    "CPU convergence capsule metadata is not NUL-terminated");
            }
            return std::wstring(to_hstring(std::string(value, end)).c_str());
        }

        template <size_t Size>
        bool ReservedZero(uint32_t const (&values)[Size])
        {
            return std::all_of(
                std::begin(values),
                std::end(values),
                [](uint32_t value) { return value == 0; });
        }

        std::wstring Sha256Text(std::wstring const& value)
        {
            auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
            auto buffer = CryptographicBuffer::ConvertStringToBinary(
                hstring(value),
                BinaryStringEncoding::Utf8);
            auto digest = std::wstring(
                CryptographicBuffer::EncodeToHexString(provider.HashData(buffer)).c_str());
            std::transform(digest.begin(), digest.end(), digest.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return digest;
        }

        std::wstring PlanMaterialJson(WorkerCpuGpuConvergencePlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"admitted\":true,\"immutable\":true" +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"profile_id\":" + JsonString(plan.profileId) +
                L",\"profile_contract_sha256\":" + JsonString(plan.profileContractSha256) +
                L",\"program_sha256\":" + JsonString(plan.programSha256) +
                L",\"spmd_execution_plan_sha256\":" + JsonString(plan.spmdExecutionPlanSha256) +
                L",\"gpu_execution_plan_sha256\":" + JsonString(plan.gpuExecutionPlanSha256) +
                L",\"package_full_name\":" + JsonString(plan.packageFullName) +
                L",\"package_version\":" + JsonString(plan.packageVersion) +
                L",\"package_installed_path\":" + JsonString(plan.packageInstalledPath) +
                L",\"module_path\":" + JsonString(plan.modulePath) +
                L",\"module_sha256\":" + JsonString(plan.moduleSha256) +
                L",\"module_bytes\":" + std::to_wstring(plan.moduleBytes) +
                L",\"abi_version\":" + std::to_wstring(plan.abiVersion) +
                L",\"lane_count\":" + std::to_wstring(plan.laneCount) +
                L",\"field_bytes\":" + std::to_wstring(plan.fieldBytes) +
                L",\"aggregate_fuel\":" + std::to_wstring(plan.aggregateFuel) +
                L",\"maximum_chunk_lanes\":" + std::to_wstring(plan.maximumChunkLanes) + L"}";
        }

        bool SelectionMatchesPlan(
            WorkerCpuCapsuleModuleSelection const& selection,
            WorkerCpuGpuConvergencePlan const& plan)
        {
            return selection.packageFullName == plan.packageFullName &&
                selection.version == plan.packageVersion &&
                selection.installedRoot.wstring() == plan.packageInstalledPath &&
                selection.modulePath.wstring() == plan.modulePath &&
                selection.moduleSha256 == plan.moduleSha256 &&
                selection.moduleBytes == plan.moduleBytes;
        }

        XComputeCpuCapsuleRunProvableWorldsChunkV1Function ValidateLoadedCapsule(
            WorkerCpuCapsuleModuleReference const& module,
            WorkerCpuCapsuleModuleSelection const& selection)
        {
            auto abi = reinterpret_cast<AbiFunction>(module.Export("XComputeCpuCapsuleAbiVersion"));
            auto build = reinterpret_cast<BuildFunction>(
                module.Export("XComputeCpuCapsuleBuildVersionPacked"));
            auto selfTest = reinterpret_cast<SelfTestFunction>(
                module.Export("XComputeCpuCapsuleSelfTest"));
            auto profileAbi = reinterpret_cast<XComputeCpuCapsuleProvableWorldsAbiVersionFunction>(
                module.Export("XComputeCpuCapsuleProvableWorldsAbiVersion"));
            auto query = reinterpret_cast<XComputeCpuCapsuleQueryProvableWorldsV1Function>(
                module.Export("XComputeCpuCapsuleQueryProvableWorldsV1"));
            auto runChunk = reinterpret_cast<XComputeCpuCapsuleRunProvableWorldsChunkV1Function>(
                module.Export("XComputeCpuCapsuleRunProvableWorldsChunkV1"));

            if (abi() != 1u || build() != selection.versionPacked ||
                selfTest() != CapsuleSelfTestMagic ||
                profileAbi() != XComputeCpuProvableWorldsAbiVersionV1)
            {
                CandidateQuarantined.store(true);
                throw WorkerXvmError(
                    "xvm.cpu_gpu_convergence_abi_mismatch",
                    "CPU convergence capsule core, build or profile ABI is incompatible");
            }

            XComputeCpuProvableWorldsProfileV1 profile{};
            profile.structSize = static_cast<uint32_t>(sizeof(profile));
            if (query(&profile) != XComputeCpuProvableWorldsStatusOk ||
                profile.structSize != sizeof(profile) ||
                profile.abiVersion != XComputeCpuProvableWorldsAbiVersionV1 ||
                !ReservedZero(profile.reserved) ||
                AsciiField(profile.profileId, sizeof(profile.profileId)) !=
                    WorkerCpuGpuConvergenceProfileId ||
                AsciiField(profile.profileContractSha256, sizeof(profile.profileContractSha256)) !=
                    WorkerCpuGpuConvergenceProfileContractSha256 ||
                AsciiField(profile.programSha256, sizeof(profile.programSha256)) != ProgramSha256 ||
                AsciiField(profile.spmdExecutionPlanSha256, sizeof(profile.spmdExecutionPlanSha256)) !=
                    SpmdPlanSha256 ||
                AsciiField(profile.gpuExecutionPlanSha256, sizeof(profile.gpuExecutionPlanSha256)) !=
                    GpuPlanSha256 ||
                AsciiField(profile.gpuContractSha256, sizeof(profile.gpuContractSha256)) !=
                    GpuContractSha256 ||
                AsciiField(profile.shaderSha256, sizeof(profile.shaderSha256)) != ShaderSha256 ||
                profile.gridWidth != XComputeCpuProvableWorldsGridWidthV1 ||
                profile.gridHeight != XComputeCpuProvableWorldsGridHeightV1 ||
                profile.laneCount != XComputeCpuProvableWorldsLaneCountV1 ||
                profile.maximumChunkLanes != XComputeCpuProvableWorldsMaximumChunkLanesV1 ||
                profile.fuelPerLane != XComputeCpuProvableWorldsFuelPerLaneV1 ||
                profile.aggregateFuel != XComputeCpuProvableWorldsAggregateFuelV1)
            {
                CandidateQuarantined.store(true);
                throw WorkerXvmError(
                    "xvm.cpu_gpu_convergence_profile_not_admitted",
                    "CPU convergence capsule query does not match the exact content-addressed profile");
            }
            return runChunk;
        }

        struct CandidateExecution
        {
            std::vector<uint32_t> output;
            double elapsedMs = 0.0;
            uint64_t chunkCallCount = 0;
            bool canceled = false;
        };

        CandidateExecution ExecuteCandidate(
            XComputeCpuCapsuleRunProvableWorldsChunkV1Function runChunk,
            uint32_t inputSeed,
            std::function<bool()> const& cancelRequested)
        {
            CandidateExecution candidate;
            candidate.output.resize(XComputeCpuProvableWorldsLaneCountV1);
            auto started = std::chrono::steady_clock::now();
            uint32_t startLane = 0;
            while (startLane < XComputeCpuProvableWorldsLaneCountV1)
            {
                if (cancelRequested && cancelRequested())
                {
                    candidate.canceled = true;
                    break;
                }
                auto count = (std::min)(
                    XComputeCpuProvableWorldsMaximumChunkLanesV1,
                    XComputeCpuProvableWorldsLaneCountV1 - startLane);
                XComputeCpuProvableWorldsRequestV1 request{};
                request.structSize = static_cast<uint32_t>(sizeof(request));
                request.startLane = startLane;
                request.laneCount = count;
                request.inputSeed = inputSeed;
                request.outputWords = candidate.output.data() + startLane;
                request.outputCapacityWords = count;
                XComputeCpuProvableWorldsResultV1 output{};
                output.structSize = static_cast<uint32_t>(sizeof(output));
                auto status = runChunk(&request, &output);
                if (status != XComputeCpuProvableWorldsStatusOk ||
                    output.status != XComputeCpuProvableWorldsStatusOk ||
                    output.structSize != sizeof(output) ||
                    output.lanesCompleted != count ||
                    output.reserved0 != 0 ||
                    !ReservedZero(output.reserved) ||
                    output.sourceInstructions !=
                        static_cast<uint64_t>(count) * XComputeCpuProvableWorldsFuelPerLaneV1)
                {
                    CandidateQuarantined.store(true);
                    throw WorkerXvmError(
                        "xvm.cpu_gpu_convergence_candidate_state_invalid",
                        "CPU convergence capsule returned a non-canonical bounded chunk result");
                }
                startLane += count;
                ++candidate.chunkCallCount;
            }
            candidate.elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return candidate;
        }

        bool CandidateMatchesCanonical(
            CandidateExecution const& candidate,
            std::vector<uint8_t> const& canonicalField)
        {
            auto bytes = candidate.output.size() * sizeof(uint32_t);
            return canonicalField.size() == bytes &&
                std::memcmp(candidate.output.data(), canonicalField.data(), bytes) == 0;
        }

        std::wstring DoubleJson(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << value;
            return out.str();
        }
    }

    WorkerCpuGpuConvergencePlan WorkerBuildCpuGpuConvergencePlan(
        JsonObject const& node,
        WorkerXvmProgram const& program,
        WorkerXvmSpmdExecutionPlan const& spmdPlan,
        WorkerGpuXvmExecutionPlan const& gpuPlan,
        std::wstring const& programSha256)
    {
        WorkerCpuGpuConvergencePlan plan;
        if (!node.HasKey(L"backend_convergence"))
        {
            return plan;
        }
        auto value = node.GetNamedValue(L"backend_convergence");
        if (value.ValueType() != JsonValueType::Object)
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_contract_invalid",
                "backend_convergence must be an exact versioned object");
        }
        auto request = value.GetObject();
        if (request.Size() != 4 ||
            std::wstring(request.GetNamedString(L"schema_version", L"").c_str()) !=
                WorkerCpuGpuConvergenceRequestSchemaVersion ||
            std::wstring(request.GetNamedString(L"contract_id", L"").c_str()) !=
                WorkerCpuGpuConvergenceContractId ||
            std::wstring(request.GetNamedString(L"profile_id", L"").c_str()) !=
                WorkerCpuGpuConvergenceProfileId ||
            std::wstring(request.GetNamedString(L"expected_profile_contract_sha256", L"").c_str()) !=
                WorkerCpuGpuConvergenceProfileContractSha256)
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_contract_invalid",
                "backend_convergence id, profile and content digest must match exactly");
        }
        if (CandidateQuarantined.load())
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_candidate_quarantined",
                "the Provable Worlds CPU capsule candidate is quarantined for this activation");
        }
        if (program.programId != ProgramId ||
            programSha256 != ProgramSha256 ||
            !spmdPlan.admitted ||
            spmdPlan.schemaVersion != WorkerXvmSpmdExecutionPlanSchemaVersionV3 ||
            spmdPlan.planSha256 != SpmdPlanSha256 ||
            spmdPlan.laneCount != XComputeCpuProvableWorldsLaneCountV1 ||
            !gpuPlan.admitted ||
            gpuPlan.schemaVersion != WorkerGpuXvmExecutionPlanSchemaVersionV11 ||
            gpuPlan.planSha256 != GpuPlanSha256 ||
            gpuPlan.spmdExecutionPlanSha256 != SpmdPlanSha256 ||
            gpuPlan.contractSha256 != GpuContractSha256 ||
            gpuPlan.shaderSha256 != ShaderSha256)
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_profile_not_admitted",
                "convergence requires the exact Provable Worlds CPU-SPMD and GPU plans");
        }

        auto selection = WorkerResolveCpuCapsuleModule();
        if (selection.versionPacked != CapsuleVersionPacked ||
            selection.version != L"1.3.0.0")
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_package_version_mismatch",
                "convergence requires the package graph to select CPU capsule 1.3.0.0 exactly");
        }

        plan.admitted = true;
        plan.schemaVersion = WorkerCpuGpuConvergencePlanSchemaVersion;
        plan.contractId = WorkerCpuGpuConvergenceContractId;
        plan.profileId = WorkerCpuGpuConvergenceProfileId;
        plan.profileContractSha256 = WorkerCpuGpuConvergenceProfileContractSha256;
        plan.programSha256 = programSha256;
        plan.spmdExecutionPlanSha256 = spmdPlan.planSha256;
        plan.gpuExecutionPlanSha256 = gpuPlan.planSha256;
        plan.packageFullName = selection.packageFullName;
        plan.packageVersion = selection.version;
        plan.packageInstalledPath = selection.installedRoot.wstring();
        plan.modulePath = selection.modulePath.wstring();
        plan.moduleSha256 = selection.moduleSha256;
        plan.moduleBytes = selection.moduleBytes;
        plan.abiVersion = XComputeCpuProvableWorldsAbiVersionV1;
        plan.laneCount = XComputeCpuProvableWorldsLaneCountV1;
        plan.fieldBytes =
            XComputeCpuProvableWorldsLaneCountV1 * static_cast<uint32_t>(sizeof(uint32_t));
        plan.aggregateFuel = XComputeCpuProvableWorldsAggregateFuelV1;
        plan.maximumChunkLanes = XComputeCpuProvableWorldsMaximumChunkLanesV1;
        plan.planSha256 = Sha256Text(PlanMaterialJson(plan));
        return plan;
    }

    bool WorkerCpuGpuConvergencePlansEqual(
        WorkerCpuGpuConvergencePlan const& expected,
        WorkerCpuGpuConvergencePlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            expected.schemaVersion == actual.schemaVersion &&
            expected.contractId == actual.contractId &&
            expected.profileId == actual.profileId &&
            expected.profileContractSha256 == actual.profileContractSha256 &&
            expected.programSha256 == actual.programSha256 &&
            expected.spmdExecutionPlanSha256 == actual.spmdExecutionPlanSha256 &&
            expected.gpuExecutionPlanSha256 == actual.gpuExecutionPlanSha256 &&
            expected.packageFullName == actual.packageFullName &&
            expected.packageVersion == actual.packageVersion &&
            expected.packageInstalledPath == actual.packageInstalledPath &&
            expected.modulePath == actual.modulePath &&
            expected.moduleSha256 == actual.moduleSha256 &&
            expected.planSha256 == actual.planSha256 &&
            expected.moduleBytes == actual.moduleBytes &&
            expected.aggregateFuel == actual.aggregateFuel &&
            expected.abiVersion == actual.abiVersion &&
            expected.laneCount == actual.laneCount &&
            expected.fieldBytes == actual.fieldBytes &&
            expected.maximumChunkLanes == actual.maximumChunkLanes;
    }

    void WorkerValidateCpuGpuConvergencePlan(
        WorkerCpuGpuConvergencePlan const& admittedPlan,
        bool hasAdmittedPlan,
        WorkerCpuGpuConvergencePlan const& rebuiltPlan)
    {
        if (hasAdmittedPlan != rebuiltPlan.admitted ||
            (hasAdmittedPlan &&
                !WorkerCpuGpuConvergencePlansEqual(admittedPlan, rebuiltPlan)))
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_plan_mismatch",
                "backend convergence plan differs from immutable full-graph pre-admission");
        }
    }

    std::wstring WorkerCpuGpuConvergencePlanJson(
        WorkerCpuGpuConvergencePlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        auto material = PlanMaterialJson(plan);
        material.pop_back();
        return material + L",\"plan_sha256\":" + JsonString(plan.planSha256) +
            L",\"canonical_backend\":\"cpu_reference\"" +
            L",\"cpu_candidate_backend\":\"cpu_packaged_capsule_provable_worlds\"" +
            L",\"gpu_candidate_backend\":\"gpu_xvm_provable_worlds\"" +
            L",\"automatic_backend_selection\":false,\"production_authority\":false}";
    }

    WorkerCpuGpuConvergenceResult WorkerRunCpuGpuConvergenceCpuCandidate(
        WorkerCpuGpuConvergencePlan const& admittedPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::vector<uint8_t> const& canonicalField,
        double referenceElapsedMs,
        std::function<bool()> const& cancelRequested)
    {
        if (CandidateQuarantined.load())
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_candidate_quarantined",
                "the Provable Worlds CPU capsule candidate is quarantined for this activation");
        }
        auto rebuilt = WorkerResolveCpuCapsuleModule();
        if (!SelectionMatchesPlan(rebuilt, admittedPlan))
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_plan_mismatch",
                "resolved CPU convergence capsule differs from full-graph pre-admission");
        }

        WorkerCpuCapsuleModuleReference module(rebuilt);
        auto runChunk = ValidateLoadedCapsule(module, rebuilt);
        auto cold = ExecuteCandidate(runChunk, inputs[0], cancelRequested);
        if (cold.canceled)
        {
            WorkerCpuGpuConvergenceResult canceled;
            canceled.plan = admittedPlan;
            canceled.loadedModulePath = module.LoadedModulePath().wstring();
            canceled.referenceElapsedMs = referenceElapsedMs;
            canceled.moduleLoadElapsedMs = module.LoadElapsedMs();
            canceled.moduleWasLoadedBefore = module.WasLoadedBefore();
            canceled.coldElapsedMs = cold.elapsedMs;
            canceled.chunkCallCount = cold.chunkCallCount;
            canceled.canceled = true;
            return canceled;
        }
        if (!CandidateMatchesCanonical(cold, canonicalField))
        {
            CandidateQuarantined.store(true);
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_cpu_candidate_mismatch",
                "CPU capsule field differs from the canonical interpreter before GPU publication");
        }

        auto warm = ExecuteCandidate(runChunk, inputs[0], cancelRequested);
        if (warm.canceled)
        {
            WorkerCpuGpuConvergenceResult canceled;
            canceled.plan = admittedPlan;
            canceled.loadedModulePath = module.LoadedModulePath().wstring();
            canceled.referenceElapsedMs = referenceElapsedMs;
            canceled.moduleLoadElapsedMs = module.LoadElapsedMs();
            canceled.moduleWasLoadedBefore = module.WasLoadedBefore();
            canceled.coldElapsedMs = cold.elapsedMs;
            canceled.warmElapsedMs = warm.elapsedMs;
            canceled.chunkCallCount = warm.chunkCallCount;
            canceled.canceled = true;
            return canceled;
        }
        if (!CandidateMatchesCanonical(warm, canonicalField) || cold.output != warm.output)
        {
            CandidateQuarantined.store(true);
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_cpu_candidate_mismatch",
                "warm CPU capsule field is not byte-identical to canonical and cold results");
        }

        WorkerCpuGpuConvergenceResult result;
        result.plan = admittedPlan;
        result.loadedModulePath = module.LoadedModulePath().wstring();
        result.referenceElapsedMs = referenceElapsedMs;
        result.moduleLoadElapsedMs = module.LoadElapsedMs();
        result.moduleWasLoadedBefore = module.WasLoadedBefore();
        result.coldElapsedMs = cold.elapsedMs;
        result.warmElapsedMs = warm.elapsedMs;
        result.chunkCallCount = warm.chunkCallCount;
        result.verifiedField.resize(warm.output.size() * sizeof(uint32_t));
        std::memcpy(
            result.verifiedField.data(), warm.output.data(), result.verifiedField.size());
        return result;
    }

    WorkerCpuGpuConvergenceExactTileResult WorkerRunCpuGpuConvergenceExactTile(
        uint32_t startLane, uint32_t laneCount, uint32_t inputSeed,
        std::vector<uint8_t> const& canonicalTile)
    {
        if (CandidateQuarantined.load())
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_candidate_quarantined",
                "the Provable Worlds CPU capsule candidate is quarantined for this activation");
        }
        if (laneCount == 0 ||
            laneCount > XComputeCpuProvableWorldsMaximumChunkLanesV1 ||
            startLane > XComputeCpuProvableWorldsLaneCountV1 ||
            laneCount > XComputeCpuProvableWorldsLaneCountV1 - startLane ||
            canonicalTile.size() != static_cast<size_t>(laneCount) * sizeof(uint32_t))
        {
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_tile_bounds_invalid",
                "the exact CPU convergence tile is outside the admitted profile");
        }
        std::lock_guard<std::mutex> lock(ExactTileMutex);
        if (!ExactTileModule)
        {
            ExactTileSelection = WorkerResolveCpuCapsuleModule();
            if (ExactTileSelection.versionPacked != CapsuleVersionPacked)
            {
                throw WorkerXvmError(
                    "xvm.cpu_gpu_convergence_plan_mismatch",
                    "the resolved CPU convergence capsule version is not admitted");
            }
            ExactTileModule = std::make_unique<WorkerCpuCapsuleModuleReference>(ExactTileSelection);
            ExactTileRunChunk = ValidateLoadedCapsule(*ExactTileModule, ExactTileSelection);
        }
        std::vector<uint32_t> output(laneCount);
        XComputeCpuProvableWorldsRequestV1 request{};
        request.structSize = static_cast<uint32_t>(sizeof(request));
        request.startLane = startLane;
        request.laneCount = laneCount;
        request.inputSeed = inputSeed;
        request.outputWords = output.data();
        request.outputCapacityWords = laneCount;
        XComputeCpuProvableWorldsResultV1 capsuleResult{};
        capsuleResult.structSize = static_cast<uint32_t>(sizeof(capsuleResult));
        auto started = std::chrono::steady_clock::now();
        auto status = ExactTileRunChunk(&request, &capsuleResult);
        auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        auto expectedInstructions = static_cast<uint64_t>(laneCount) *
            XComputeCpuProvableWorldsFuelPerLaneV1;
        if (status != XComputeCpuProvableWorldsStatusOk ||
            capsuleResult.status != XComputeCpuProvableWorldsStatusOk ||
            capsuleResult.structSize != sizeof(capsuleResult) ||
            capsuleResult.lanesCompleted != laneCount ||
            capsuleResult.sourceInstructions != expectedInstructions ||
            capsuleResult.reserved0 != 0 ||
            !ReservedZero(capsuleResult.reserved) ||
            std::memcmp(output.data(), canonicalTile.data(), canonicalTile.size()) != 0)
        {
            CandidateQuarantined.store(true);
            throw WorkerXvmError(
                "xvm.cpu_gpu_convergence_cpu_candidate_mismatch",
                "the bounded CPU capsule tile differs from the canonical reference");
        }
        WorkerCpuGpuConvergenceExactTileResult result;
        result.packageFullName = ExactTileSelection.packageFullName;
        result.packageVersion = ExactTileSelection.version;
        result.modulePath = ExactTileSelection.modulePath.wstring();
        result.moduleSha256 = ExactTileSelection.moduleSha256;
        result.moduleLoadElapsedMs = ExactTileModule->LoadElapsedMs();
        result.executeElapsedMs = elapsedMs;
        result.sourceInstructions = capsuleResult.sourceInstructions;
        result.moduleWasLoadedBefore = ExactTileModule->WasLoadedBefore();
        return result;
    }

    std::wstring WorkerCpuGpuConvergenceResultJson(
        WorkerCpuGpuConvergenceResult const& result,
        bool gpuExactMatch)
    {
        auto warmSpeedup = result.warmElapsedMs > 0.0
            ? result.referenceElapsedMs / result.warmElapsedMs
            : 0.0;
        return std::wstring(L"{\"enabled\":true,\"schema_version\":") +
            JsonString(WorkerCpuGpuConvergenceResultSchemaVersion) +
            L",\"gate_id\":" + JsonString(WorkerCpuGpuConvergenceContractId) +
            L",\"plan\":" + WorkerCpuGpuConvergencePlanJson(result.plan) +
            L",\"loaded_module_path\":" + JsonString(result.loadedModulePath) +
            L",\"loaded_module_path_matches_plan\":true" +
            L",\"module_was_loaded_before\":" +
                (result.moduleWasLoadedBefore ? L"true" : L"false") +
            L",\"module_load_elapsed_ms\":" + DoubleJson(result.moduleLoadElapsedMs) +
            L",\"cpu_reference_elapsed_ms\":" + DoubleJson(result.referenceElapsedMs) +
            L",\"cpu_capsule_cold_elapsed_ms\":" + DoubleJson(result.coldElapsedMs) +
            L",\"cpu_capsule_warm_elapsed_ms\":" + DoubleJson(result.warmElapsedMs) +
            L",\"cpu_capsule_warm_speedup_ratio\":" + DoubleJson(warmSpeedup) +
            L",\"chunk_call_count\":" + std::to_wstring(result.chunkCallCount) +
            L",\"cpu_capsule_cold_exact_match\":true" +
            L",\"cpu_capsule_warm_exact_match\":true" +
            L",\"gpu_exact_match\":" + (gpuExactMatch ? L"true" : L"false") +
            L",\"three_way_exact_match\":" + (gpuExactMatch ? L"true" : L"false") +
            L",\"shared_safepoint\":\"completed_lane_chunk_boundary\"" +
            L",\"cancellation_idempotent\":true" +
            L",\"canonical_backend\":\"cpu_reference\"" +
            L",\"selected_backend\":\"none_measurement_only\"" +
            L",\"fallback_backend\":\"cpu_reference\"" +
            L",\"fallback_available\":true" +
            L",\"candidate_quarantined\":false" +
            L",\"mismatch_policy\":\"quarantine_before_publication\"" +
            L",\"automatic_backend_selection\":false" +
            L",\"production_authority\":false" +
            L",\"runtime_native_codegen\":false" +
            L",\"runtime_shader_compilation\":false}";
    }

    void WorkerQuarantineCpuGpuConvergenceCandidate() noexcept
    {
        CandidateQuarantined.store(true);
    }

    bool WorkerIsCpuGpuConvergenceCandidateQuarantined() noexcept
    {
        return CandidateQuarantined.load();
    }
}
