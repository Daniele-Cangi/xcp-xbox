#include "pch.h"
#include "WorkerGpuXvmExecutor.h"
#include "WorkerGpuXvmStateCodec.h"
#include "WorkerGpuXvmProvableWorlds.h"
#include "WorkerXvmIsa.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::ApplicationModel;

namespace XComputeProbe
{
    namespace
    {
        std::wstring HResultToString(HRESULT hr)
        {
            std::wostringstream out;
            out << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
            return out.str();
        }

        std::wstring D3DFeatureLevelToString(D3D_FEATURE_LEVEL featureLevel)
        {
            switch (featureLevel)
            {
            case D3D_FEATURE_LEVEL_12_1: return L"12_1";
            case D3D_FEATURE_LEVEL_12_0: return L"12_0";
            case D3D_FEATURE_LEVEL_11_1: return L"11_1";
            case D3D_FEATURE_LEVEL_11_0: return L"11_0";
            case D3D_FEATURE_LEVEL_10_1: return L"10_1";
            case D3D_FEATURE_LEVEL_10_0: return L"10_0";
            default: return L"unknown";
            }
        }

        std::vector<uint8_t> ReadInstalledBinaryFile(std::wstring const& relativeFileName)
        {
            auto installedPath = std::wstring(Package::Current().InstalledLocation().Path().c_str());
            auto path = std::filesystem::path(installedPath) / relativeFileName;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerXvmError("xvm.gpu_execution_failed", "could not open installed XVM GPU shader bytecode");
            }
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        }

        void ThrowGpuHResult(char const* code, char const* operation, HRESULT hr)
        {
            auto stableCode = WorkerGpuXvmIsDeviceLoss(hr) ? "xvm.gpu_device_lost" : code;
            throw WorkerXvmError(
                stableCode,
                std::string(operation) + " failed: " + winrt::to_string(winrt::hstring(HResultToString(hr))));
        }

        void WriteU32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
        {
            bytes[offset] = static_cast<uint8_t>(value & 0xffu);
            bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
            bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
            bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
        }
    }

    WorkerGpuXvmExecutionResult WorkerExecuteGpuXvmProfile(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerGpuXvmExecutionPlan const& executionPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested)
    {
        if (profile.kind == WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1)
        {
            return WorkerExecuteGpuXvmProvableWorlds(
                profile, program, executionPlan, inputs, cancelRequested);
        }
        auto instructionCount = static_cast<uint32_t>(program.words.size() / WorkerXvmInstructionWords());
        auto manyLaneExecution = profile.manyLaneAdmitted;
        auto memoryWords = static_cast<uint32_t>((manyLaneExecution ? profile.memoryBytes : limits.memoryBytes) / sizeof(uint32_t));
        auto outputWords = static_cast<uint32_t>((manyLaneExecution ? profile.outputBytes : limits.outputBytes) / sizeof(uint32_t));
        auto fuelLimit = static_cast<uint32_t>(manyLaneExecution ? profile.fuelLimit : limits.fuel);

        if (profile.microtraceAdmitted)
        {
            if (!executionPlan.admitted || !executionPlan.microtraceAdmitted ||
                (executionPlan.schemaVersion != WorkerGpuXvmExecutionPlanSchemaVersionV8 &&
                 executionPlan.schemaVersion != WorkerGpuXvmExecutionPlanSchemaVersionV9))
            {
                throw WorkerXvmError("xvm.microtrace_plan_invalid", "GPU microtrace execution requires its admitted immutable v8 plan");
            }
            auto revalidated = WorkerBuildGpuXvmExecutionPlan(
                profile, program, executionPlan.programSha256, std::wstring{}, 0, {}, {});
            if (!WorkerGpuXvmExecutionPlansEqual(executionPlan, revalidated))
            {
                throw WorkerXvmError("xvm.microtrace_revalidation_failed", "worker microtrace revalidation did not reproduce the admitted immutable plan");
            }
        }
        if (manyLaneExecution)
        {
            auto revalidated = WorkerBuildGpuXvmExecutionPlan(
                profile,
                program,
                executionPlan.programSha256,
                executionPlan.spmdExecutionPlanSha256,
                executionPlan.laneCount,
                executionPlan.gridShape,
                executionPlan.workgroupShape);
            if (!WorkerGpuXvmExecutionPlansEqual(executionPlan, revalidated))
            {
                throw WorkerXvmError(
                    "xvm.gpu_spmd_revalidation_failed",
                    "worker SPMD revalidation did not reproduce the admitted immutable topology plan");
            }
        }

        uint32_t flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        auto hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            device.put(),
            &featureLevel,
            context.put());
        if (FAILED(hr))
        {
            ThrowGpuHResult("xvm.gpu_execution_failed", "D3D11CreateDevice", hr);
        }

        auto shaderBytes = ReadInstalledBinaryFile(profile.shaderName);
        if (shaderBytes.empty())
        {
            throw WorkerXvmError("xvm.gpu_execution_failed", "installed XVM GPU shader bytecode is empty");
        }
        std::wstring shaderSha256;
        if (!WorkerGpuXvmShaderMatchesContract(profile, shaderBytes, shaderSha256))
        {
            throw WorkerXvmError(
                "xvm.gpu_shader_identity_mismatch",
                "installed XVM GPU shader bytecode does not match the admitted profile contract");
        }

        winrt::com_ptr<ID3D11ComputeShader> shader;
        hr = device->CreateComputeShader(shaderBytes.data(), shaderBytes.size(), nullptr, shader.put());
        if (FAILED(hr))
        {
            ThrowGpuHResult("xvm.gpu_execution_failed", "CreateComputeShader", hr);
        }

        auto createStructuredSrv = [&](std::vector<uint32_t> const& words, char const* label)
        {
            D3D11_BUFFER_DESC bufferDesc{};
            bufferDesc.ByteWidth = static_cast<UINT>(words.size() * sizeof(uint32_t));
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = sizeof(uint32_t);

            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = words.data();

            winrt::com_ptr<ID3D11Buffer> buffer;
            auto bufferHr = device->CreateBuffer(&bufferDesc, &initialData, buffer.put());
            if (FAILED(bufferHr))
            {
                ThrowGpuHResult("xvm.gpu_execution_failed", label, bufferHr);
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(words.size());

            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            auto srvHr = device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put());
            if (FAILED(srvHr))
            {
                ThrowGpuHResult("xvm.gpu_execution_failed", label, srvHr);
            }
            return srv;
        };

        std::vector<uint32_t> parameterWords(profile.parameterWords, 0);
        parameterWords[0] = instructionCount;
        parameterWords[1] = fuelLimit;
        parameterWords[2] = outputWords;
        parameterWords[3] = memoryWords;
        if (profile.residentExecution)
        {
            parameterWords[4] = profile.epochInstructionBudget;
            parameterWords[5] = profile.maxEpochs;
        }
        if (profile.microtraceAdmitted &&
            executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9)
        {
            parameterWords[6] = executionPlan.microtraceOpCount;
            parameterWords[7] = executionPlan.microtraceWordsPerOp;
            parameterWords[8] = executionPlan.instructionCount;
        }
        if (profile.laneParametric)
        {
            if (!executionPlan.admitted)
            {
                throw WorkerXvmError("xvm.gpu_contract_mismatch", "lane-parametric GPU XVM execution requires its admitted immutable plan");
            }
            parameterWords[6] = executionPlan.laneCount;
            parameterWords[7] = executionPlan.gridShape[0];
            parameterWords[8] = executionPlan.gridShape[1];
            parameterWords[9] = executionPlan.gridShape[2];
            parameterWords[10] = executionPlan.workgroupShape[0];
            parameterWords[11] = executionPlan.workgroupShape[1];
            parameterWords[12] = executionPlan.workgroupShape[2];
            parameterWords[13] = executionPlan.stateWordsPerLane;
            parameterWords[14] = executionPlan.inputWordsPerLane;
            parameterWords[15] = executionPlan.capabilityFlags;
        }

        auto inputWordCount = manyLaneExecution
            ? static_cast<size_t>(executionPlan.laneCount) * executionPlan.inputWordsPerLane
            : static_cast<size_t>(profile.inputWords);
        std::vector<uint32_t> inputWords(inputWordCount, 0);
        if (manyLaneExecution)
        {
            if (!executionPlan.admitted || inputWords.size() !=
                static_cast<size_t>(executionPlan.laneCount) * executionPlan.inputWordsPerLane)
            {
                throw WorkerXvmError("xvm.gpu_contract_mismatch", "SPMD input replication does not match the immutable GPU plan");
            }
            for (uint32_t laneId = 0; laneId < executionPlan.laneCount; ++laneId)
            {
                std::copy(inputs.begin(), inputs.end(),
                    inputWords.begin() + static_cast<size_t>(laneId) * executionPlan.inputWordsPerLane);
            }
        }
        else
        {
            std::copy(inputs.begin(), inputs.end(), inputWords.begin());
        }

        auto dispatchLimit = profile.sequentialDispatchPerInstruction
            ? instructionCount
            : (profile.staticFuelDispatchBound
                ? executionPlan.requiredEpochCount
                : (profile.residentExecution
                ? (instructionCount + profile.epochInstructionBudget - 1) / profile.epochInstructionBudget
                : 1u));
        if (dispatchLimit == 0 || (profile.residentExecution && dispatchLimit > profile.maxEpochs))
        {
            throw WorkerXvmError("xvm.gpu_epoch_limit_exceeded", "resident GPU XVM plan exceeds its admitted epoch limit");
        }

        std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> parameterSrvs;
        parameterSrvs.reserve(profile.sequentialDispatchPerInstruction ? dispatchLimit : 1u);
        if (profile.sequentialDispatchPerInstruction)
        {
            for (uint32_t dispatchIndex = 0; dispatchIndex < dispatchLimit; ++dispatchIndex)
            {
                auto dispatchParameterWords = parameterWords;
                dispatchParameterWords[4] = dispatchIndex;
                parameterSrvs.push_back(createStructuredSrv(dispatchParameterWords, "Create XVM parameter SRV"));
            }
        }
        else
        {
            parameterSrvs.push_back(createStructuredSrv(parameterWords, "Create XVM parameter SRV"));
        }

        auto inputSrv = createStructuredSrv(inputWords, "Create XVM input SRV");
        winrt::com_ptr<ID3D11ShaderResourceView> programSrv;
        std::vector<uint32_t> gpuProgramWords;
        if (profile.requiresProgramSrv)
        {
            gpuProgramWords.assign(profile.programWords, 0);
            std::copy(program.words.begin(), program.words.end(), gpuProgramWords.begin());
            programSrv = createStructuredSrv(gpuProgramWords, "Create XVM program SRV");
        }
        winrt::com_ptr<ID3D11ShaderResourceView> microtraceSrv;
        if (profile.microtraceAdmitted)
        {
            microtraceSrv = createStructuredSrv(executionPlan.microtraceWords, "Create XVM microtrace SRV");
        }

        auto initialResultWords = WorkerCreateGpuXvmInitialState(
            profile,
            instructionCount,
            memoryWords,
            outputWords,
            fuelLimit,
            executionPlan);
        auto stateWordCount = profile.laneParametric ? executionPlan.totalStateWords : profile.resultWords;
        D3D11_SUBRESOURCE_DATA resultInitialData{};
        resultInitialData.pSysMem = initialResultWords.data();

        D3D11_BUFFER_DESC resultDesc{};
        resultDesc.ByteWidth = stateWordCount * sizeof(uint32_t);
        resultDesc.Usage = D3D11_USAGE_DEFAULT;
        resultDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        resultDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        resultDesc.StructureByteStride = sizeof(uint32_t);

        winrt::com_ptr<ID3D11Buffer> resultBuffer;
        hr = device->CreateBuffer(&resultDesc, &resultInitialData, resultBuffer.put());
        if (FAILED(hr))
        {
            ThrowGpuHResult("xvm.gpu_execution_failed", "Create XVM result buffer", hr);
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = stateWordCount;

        winrt::com_ptr<ID3D11UnorderedAccessView> resultUav;
        hr = device->CreateUnorderedAccessView(resultBuffer.get(), &uavDesc, resultUav.put());
        if (FAILED(hr))
        {
            ThrowGpuHResult("xvm.gpu_execution_failed", "Create XVM result UAV", hr);
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = stateWordCount * sizeof(uint32_t);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = device->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            ThrowGpuHResult("xvm.gpu_execution_failed", "Create XVM staging buffer", hr);
        }

        auto srvCount = profile.microtraceAdmitted ? 4u : (profile.requiresProgramSrv ? 3u : 2u);
        ID3D11UnorderedAccessView* uavs[] = { resultUav.get() };
        UINT initialCounts[] = { 0 };
        ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr, nullptr, nullptr };
        ID3D11UnorderedAccessView* nullUavs[] = { nullptr };
        context->CSSetShader(shader.get(), nullptr, 0);

        auto readState = [&]()
        {
            context->CopyResource(stagingBuffer.get(), resultBuffer.get());
            context->Flush();

            auto removedHr = device->GetDeviceRemovedReason();
            if (FAILED(removedHr))
            {
                ThrowGpuHResult("xvm.gpu_device_lost", "XVM GPU device removed", removedHr);
            }

            std::vector<uint32_t> words(stateWordCount, 0);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto mapHr = context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(mapHr))
            {
                ThrowGpuHResult("xvm.gpu_execution_failed", "Map XVM staging buffer", mapHr);
            }
            std::memcpy(words.data(), mapped.pData, words.size() * sizeof(uint32_t));
            context->Unmap(stagingBuffer.get(), 0);
            return words;
        };

        std::vector<uint32_t> resultWords;
        WorkerGpuXvmDecodedState decoded;
        std::vector<WorkerGpuXvmDecodedState> decodedLanes;
        bool gpuSpmdLockstepViolation = false;
        auto decodeGpuState = [&](std::vector<uint32_t> const& words, std::string& error)
        {
            gpuSpmdLockstepViolation = false;
            auto laneCount = manyLaneExecution ? executionPlan.laneCount : 1u;
            if (manyLaneExecution && words.size() != executionPlan.totalStateWords)
            {
                gpuSpmdLockstepViolation = true;
                error = "GPU XVM SPMD readback size does not match the immutable plan";
                return false;
            }
            decodedLanes.clear();
            decodedLanes.reserve(laneCount);
            uint32_t expectedStatus = 0;
            for (uint32_t laneId = 0; laneId < laneCount; ++laneId)
            {
                std::vector<uint32_t> laneWords;
                if (manyLaneExecution)
                {
                    auto begin = words.begin() + static_cast<size_t>(laneId) * executionPlan.stateWordsPerLane;
                    laneWords.assign(begin, begin + executionPlan.stateWordsPerLane);
                }
                else
                {
                    laneWords = words;
                }
                WorkerGpuXvmDecodedState laneDecoded;
                if (!WorkerDecodeGpuXvmState(
                        profile,
                        laneWords,
                        instructionCount,
                        memoryWords,
                        outputWords,
                        fuelLimit,
                        executionPlan,
                        program,
                        laneDecoded,
                        error,
                        laneId))
                {
                    return false;
                }
                auto status = laneWords[14];
                if (laneId == 0)
                {
                    expectedStatus = status;
                }
                else
                {
                    auto const& first = decodedLanes.front();
                    auto lockstep = status == expectedStatus &&
                        laneDecoded.halted == first.halted &&
                        laneDecoded.control == first.control &&
                        laneDecoded.fuelConsumed == first.fuelConsumed &&
                        laneDecoded.trapCode == first.trapCode &&
                        laneDecoded.faultCode == first.faultCode &&
                        laneDecoded.pc == first.pc &&
                        laneDecoded.epochSequence == first.epochSequence &&
                        laneDecoded.callDepth == first.callDepth &&
                        laneDecoded.loopDepth == first.loopDepth &&
                        laneDecoded.trap.code == first.trap.code &&
                        laneDecoded.trap.trapPc == first.trap.trapPc &&
                        laneDecoded.trap.recoveryPc == first.trap.recoveryPc &&
                        laneDecoded.trap.occurrenceCount == first.trap.occurrenceCount;
                    if (!lockstep)
                    {
                        gpuSpmdLockstepViolation = true;
                        error = "GPU XVM SPMD lanes diverged at a validated epoch boundary";
                        return false;
                    }
                }
                decodedLanes.push_back(std::move(laneDecoded));
            }
            decoded = decodedLanes.front();
            return true;
        };
        uint32_t submittedDispatchCount = 0;
        uint32_t epochBoundaryReadbacks = 0;
        uint32_t previousFuel = 0;
        uint32_t previousPc = 0;
        uint32_t maxBoundaryLoopDepth = 0;
        uint32_t loopStackNonemptyEpochBoundaries = 0;
        uint32_t maxBoundaryCallDepth = 0;
        uint32_t callStackNonemptyEpochBoundaries = 0;
        uint64_t maxBoundaryTrapOccurrenceCount = 0;
        uint32_t trapStateNonemptyEpochBoundaries = 0;
        WorkerXvmTrapState previousTrap;
        for (uint32_t dispatchIndex = 0; dispatchIndex < dispatchLimit; ++dispatchIndex)
        {
            if (profile.residentExecution && cancelRequested && cancelRequested())
            {
                throw WorkerXvmError("job.canceled", "GPU XVM execution canceled at an admitted epoch boundary");
            }

            auto parameterIndex = profile.sequentialDispatchPerInstruction ? dispatchIndex : 0u;
            ID3D11ShaderResourceView* srvs[] =
            {
                parameterSrvs[parameterIndex].get(),
                inputSrv.get(),
                programSrv.get(),
                microtraceSrv.get(),
            };
            context->CSSetShaderResources(0, srvCount, srvs);
            context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
            // Legacy profile behavior remains equivalent to context->Dispatch(1, 1, 1).
            context->Dispatch(
                profile.laneParametric ? executionPlan.dispatchGroupShape[0] : 1,
                profile.laneParametric ? executionPlan.dispatchGroupShape[1] : 1,
                profile.laneParametric ? executionPlan.dispatchGroupShape[2] : 1);
            context->CSSetShaderResources(0, srvCount, nullSrvs);
            context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ++submittedDispatchCount;

            if (!profile.residentExecution)
            {
                continue;
            }

            resultWords = readState();
            ++epochBoundaryReadbacks;
            std::string decodeError;
            if (!decodeGpuState(resultWords, decodeError))
            {
                throw WorkerXvmError(
                    gpuSpmdLockstepViolation
                        ? "xvm.gpu_spmd_lockstep_invalid"
                        : "xvm.gpu_resident_state_invalid",
                    decodeError);
            }
            auto residentStatus = resultWords[14];
            // Legacy resident validation rejects decoded.pc != instructionCount - 1.
            auto terminalPcValid = profile.terminalPcIsInstructionCount
                ? decoded.pc == instructionCount
                : decoded.pc == instructionCount - 1;
            auto pcProgressValid = !profile.pcMustAdvanceMonotonically || decoded.pc > previousPc;
            auto executionFault = decoded.faultCode != 0;
            auto trapTransitionValid = true;
            if (executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5)
            {
                auto countMonotonic = decoded.trap.occurrenceCount >= previousTrap.occurrenceCount;
                auto tupleStableWithoutNewTrap = decoded.trap.occurrenceCount != previousTrap.occurrenceCount ||
                    (decoded.trap.code == previousTrap.code &&
                     decoded.trap.trapPc == previousTrap.trapPc &&
                     decoded.trap.recoveryPc == previousTrap.recoveryPc);
                auto fuelMonotonic = decoded.fuelConsumed >= previousFuel;
                auto trapCountDelta = countMonotonic
                    ? decoded.trap.occurrenceCount - previousTrap.occurrenceCount
                    : 0;
                auto fuelDelta = fuelMonotonic ? decoded.fuelConsumed - previousFuel : 0;
                trapTransitionValid = countMonotonic && tupleStableWithoutNewTrap &&
                    fuelMonotonic && trapCountDelta <= fuelDelta;
            }
            maxBoundaryLoopDepth = (std::max)(maxBoundaryLoopDepth, decoded.loopDepth);
            maxBoundaryCallDepth = (std::max)(maxBoundaryCallDepth, decoded.callDepth);
            maxBoundaryTrapOccurrenceCount =
                (std::max)(maxBoundaryTrapOccurrenceCount, decoded.trap.occurrenceCount);
            if (decoded.loopDepth != 0)
            {
                ++loopStackNonemptyEpochBoundaries;
            }
            if (decoded.callDepth != 0)
            {
                ++callStackNonemptyEpochBoundaries;
            }
            if (decoded.trap.occurrenceCount != 0)
            {
                ++trapStateNonemptyEpochBoundaries;
            }
            if (!trapTransitionValid || decoded.epochSequence != submittedDispatchCount ||
                (!executionFault && !decoded.halted &&
                    (residentStatus != 2 || decoded.fuelConsumed <= previousFuel || !pcProgressValid)) ||
                (decoded.halted && (residentStatus != 3 || !terminalPcValid ||
                    decoded.callDepth != 0 || decoded.loopDepth != 0)) ||
                (executionFault && residentStatus != 4))
            {
                throw WorkerXvmError("xvm.gpu_epoch_progress_invalid", "resident GPU XVM state did not advance monotonically to a valid epoch boundary");
            }
            previousFuel = decoded.fuelConsumed;
            previousPc = decoded.pc;
            previousTrap = decoded.trap;
            if (cancelRequested && cancelRequested())
            {
                throw WorkerXvmError("job.canceled", "GPU XVM execution canceled at an admitted epoch boundary");
            }
            if (decoded.halted || executionFault)
            {
                break;
            }
        }

        if (!profile.residentExecution)
        {
            resultWords = readState();
            std::string decodeError;
            if (!decodeGpuState(resultWords, decodeError))
            {
                throw WorkerXvmError(
                    gpuSpmdLockstepViolation
                        ? "xvm.gpu_spmd_lockstep_invalid"
                        : "xvm.gpu_execution_failed",
                    decodeError);
            }
        }

        context->CSSetShaderResources(0, srvCount, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);

        if (profile.microtraceAdmitted &&
            executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9 &&
            (resultWords.size() <= 15 || resultWords[15] != executionPlan.microtraceOpCount))
        {
            throw WorkerXvmError("xvm.microtrace_plan_invalid", "GPU resident shader did not consume the exact optimized microtrace record count");
        }

        if (!decoded.halted)
        {
            throw WorkerXvmError(
                "xvm.gpu_execution_failed",
                "XVM GPU execution faulted or exhausted its admitted epoch plan with code " +
                    std::to_string(decoded.faultCode));
        }

        WorkerGpuXvmExecutionResult result;
        auto resultLaneCount = manyLaneExecution ? executionPlan.laneCount : 1u;
        result.run.output.resize(static_cast<size_t>(resultLaneCount) * outputWords * sizeof(uint32_t), 0);
        result.run.controlToken = L"pass";
        for (uint32_t laneId = 0; laneId < resultLaneCount; ++laneId)
        {
            auto const& lane = decodedLanes[laneId];
            for (uint32_t index = 0; index < outputWords; ++index)
            {
                auto byteOffset = (static_cast<size_t>(laneId) * outputWords + index) * sizeof(uint32_t);
                WriteU32(result.run.output, static_cast<uint32_t>(byteOffset), lane.outputWords[index]);
            }
            if (lane.control == 0)
            {
                result.run.controlToken = L"fail";
            }
            result.run.fuelConsumed += lane.fuelConsumed;
            if (result.run.trap.occurrenceCount == 0 && lane.trap.occurrenceCount != 0)
            {
                result.run.trap = lane.trap;
            }
        }
        if (result.run.output.size() != limits.outputBytes)
        {
            throw WorkerXvmError("xvm.gpu_spmd_lockstep_invalid", "GPU XVM aggregate output does not match the admitted node resource plan");
        }

        auto dispatchStrategy = profile.sequentialDispatchPerInstruction
            ? L"host_sequential_per_instruction"
            : (profile.residentExecution ? L"gpu_resident_bounded_epoch" : L"single_dispatch");
        result.telemetryJson =
            std::wstring(L"{\"backend\":") + JsonString(manyLaneExecution
                ? L"cpu_gpu_spmd_differential" : L"cpu_gpu_differential") +
            L",\"shader\":" + JsonString(profile.shaderName) +
            L",\"profile\":" + JsonString(profile.profileId) +
            L",\"profile_contract_schema\":" + JsonString(profile.schemaVersion) +
            L",\"profile_contract_id\":" + JsonString(profile.contractId) +
            L",\"profile_contract_sha256\":" + JsonString(profile.contractSha256) +
            L",\"shader_sha256\":" + JsonString(shaderSha256) +
            L",\"shader_identity_verified\":true" +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(featureLevel)) +
            L",\"shader_bytes\":" + std::to_wstring(shaderBytes.size()) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"input_srv_words\":" + std::to_wstring(inputWords.size()) +
            L",\"parameter_srv_words\":" + std::to_wstring(parameterWords.size()) +
            L",\"program_srv_words\":" + std::to_wstring(gpuProgramWords.size()) +
            L",\"microtrace_srv_words\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceWords.size() : 0) +
            L",\"microtrace_consumed_by_resident_shader\":" + (profile.microtraceAdmitted ? L"true" : L"false") +
            L",\"microtrace_worker_revalidated\":" + (profile.microtraceAdmitted ? L"true" : L"false") +
            L",\"microtrace_sha256\":" + (profile.microtraceAdmitted ? JsonString(executionPlan.microtraceSha256) : L"null") +
            L",\"microtrace_record_schema\":" + (profile.microtraceAdmitted ? JsonString(executionPlan.microtraceSchemaVersion) : L"null") +
            L",\"microtrace_record_count\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceOpCount : 0) +
            L",\"microtrace_executed_record_count\":" + std::to_wstring(
                profile.microtraceAdmitted && executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV9 && resultWords.size() > 15 ? resultWords[15] : 0) +
            L",\"microtrace_fused_record_count\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceFusedRecordCount : 0) +
            L",\"microtrace_constant_fold_record_count\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceConstantFoldRecordCount : 0) +
            L",\"microtrace_alu_fusion_record_count\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceAluFusionRecordCount : 0) +
            L",\"microtrace_superblock_count\":" + std::to_wstring(profile.microtraceAdmitted ? executionPlan.microtraceSuperblockCount : 0) +
            L",\"dispatch_strategy\":" + JsonString(dispatchStrategy) +
            L",\"dispatch_count\":" + std::to_wstring(submittedDispatchCount) +
            L",\"host_instruction_dispatch_count\":" +
                std::to_wstring(profile.sequentialDispatchPerInstruction ? submittedDispatchCount : 0) +
            L",\"epoch_instruction_budget\":" + std::to_wstring(profile.epochInstructionBudget) +
            L",\"required_epoch_count\":" + std::to_wstring(executionPlan.admitted ? executionPlan.requiredEpochCount : 0) +
            L",\"executed_epoch_count\":" + std::to_wstring(profile.residentExecution ? submittedDispatchCount : 0) +
            L",\"epoch_boundary_readbacks\":" + std::to_wstring(epochBoundaryReadbacks) +
            L",\"max_boundary_loop_depth\":" + std::to_wstring(maxBoundaryLoopDepth) +
            L",\"loop_frame_boundary_observed\":" + (loopStackNonemptyEpochBoundaries != 0 ? L"true" : L"false") +
            L",\"loop_stack_nonempty_epoch_boundaries\":" + std::to_wstring(loopStackNonemptyEpochBoundaries) +
            L",\"final_loop_depth\":" + std::to_wstring(decoded.loopDepth) +
            L",\"max_boundary_call_depth\":" + std::to_wstring(maxBoundaryCallDepth) +
            L",\"call_frame_boundary_observed\":" + (callStackNonemptyEpochBoundaries != 0 ? L"true" : L"false") +
            L",\"call_stack_nonempty_epoch_boundaries\":" + std::to_wstring(callStackNonemptyEpochBoundaries) +
            L",\"final_call_depth\":" + std::to_wstring(decoded.callDepth) +
            (executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV5
                ? std::wstring(L",\"trap_state_schema\":") + JsonString(executionPlan.trapStateSchemaVersion) +
                    L",\"recovered_trap_boundary_observed\":" +
                        (trapStateNonemptyEpochBoundaries != 0 ? L"true" : L"false") +
                    L",\"trap_state_nonempty_epoch_boundaries\":" +
                        std::to_wstring(trapStateNonemptyEpochBoundaries) +
                    L",\"max_boundary_trap_occurrence_count\":" +
                        std::to_wstring(maxBoundaryTrapOccurrenceCount) +
                    L",\"final_trap_status\":" +
                        JsonString(decoded.trap.occurrenceCount == 0 ? L"none" : L"recovered") +
                    L",\"final_trap_code\":" + std::to_wstring(decoded.trap.code) +
                    L",\"final_trap_pc\":" + std::to_wstring(decoded.trap.trapPc) +
                    L",\"final_recovery_pc\":" + std::to_wstring(decoded.trap.recoveryPc) +
                    L",\"final_trap_occurrence_count\":" +
                        std::to_wstring(decoded.trap.occurrenceCount) +
                    L",\"verified_recovery_site_count\":" +
                        std::to_wstring(executionPlan.verifiedRecoverySites.size()) +
                    L",\"recovery_same_ownership_verified\":" +
                        (executionPlan.recoverySameOwnershipVerified ? L"true" : L"false") +
                    L",\"recovered_trap_is_execution_fault\":false"
                : L"") +
            (executionPlan.schemaVersion == WorkerGpuXvmExecutionPlanSchemaVersionV6
                ? std::wstring(L",\"verified_branch_site_count\":") +
                    std::to_wstring(executionPlan.verifiedBranchSites.size()) +
                    L",\"legacy_branch_topology_admitted\":" +
                        (executionPlan.legacyBranchTopologyAdmitted ? L"true" : L"false") +
                    L",\"branch_targets_forward_same_region_verified\":" +
                        (executionPlan.branchTargetsForwardSameRegionVerified ? L"true" : L"false") +
                    L",\"structured_control_mixing_forbidden\":" +
                        (executionPlan.structuredControlMixingForbidden ? L"true" : L"false") +
                    L",\"reserved_trap_state_canonical_zero\":true" +
                    L",\"reserved_loop_state_canonical_zero\":true"
                : L"") +
            L",\"final_pc\":" + std::to_wstring(decoded.pc) +
            L",\"static_worst_case_fuel\":" + std::to_wstring(program.staticWorstCaseFuel) +
            L",\"aggregate_static_worst_case_fuel\":" +
                std::to_wstring(manyLaneExecution
                    ? static_cast<uint64_t>(executionPlan.laneCount) * program.staticWorstCaseFuel
                    : program.staticWorstCaseFuel) +
            L",\"epoch_count_derived_from_static_fuel\":" + (profile.staticFuelDispatchBound ? L"true" : L"false") +
            L",\"gpu_execution_plan_schema\":" + (executionPlan.admitted ? JsonString(executionPlan.schemaVersion) : L"null") +
            L",\"gpu_execution_plan_sha256\":" + (executionPlan.admitted ? JsonString(executionPlan.planSha256) : L"null") +
            L",\"gpu_lane_state_schema\":" + (executionPlan.admitted ? JsonString(executionPlan.stateSchemaVersion) : L"null") +
            L",\"max_loop_depth\":" + std::to_wstring(executionPlan.admitted ? executionPlan.maxLoopDepth : 0) +
            L",\"max_call_depth\":" + std::to_wstring(executionPlan.admitted ? executionPlan.maxCallDepth : 0) +
            L",\"verified_actual_max_call_depth\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.verifiedActualMaxCallDepth : 0) +
            L",\"verified_code_region_count\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.verifiedCodeRegions.size() : 0) +
            L",\"verified_call_site_count\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.verifiedCallSites.size() : 0) +
            L",\"call_graph_all_functions_reachable\":" +
                (executionPlan.admitted && executionPlan.callGraphAllFunctionsReachable ? L"true" : L"false") +
            L",\"call_graph_acyclic\":" +
                (executionPlan.admitted && executionPlan.callGraphAcyclic ? L"true" : L"false") +
            L",\"call_frame_capacity\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.callFrameCapacity : 0) +
            L",\"call_frame_layout\":" +
                (executionPlan.admitted && executionPlan.callFrameCapacity != 0
                    ? JsonString(L"parallel_return_pc_loop_depth_u32_v1")
                    : L"null") +
            L",\"return_pc_stack_word_offset\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.returnPcStackWordOffset : 0) +
            L",\"call_loop_depth_stack_word_offset\":" +
                std::to_wstring(executionPlan.admitted ? executionPlan.callLoopDepthStackWordOffset : 0) +
            L",\"lane_parametric_state\":" + (profile.laneParametric ? L"true" : L"false") +
            L",\"logical_lane_count\":" + std::to_wstring(profile.laneParametric ? executionPlan.laneCount : 1) +
            L",\"max_logical_lanes\":" + std::to_wstring(profile.laneParametric ? profile.laneCount : 1) +
            L",\"many_lane_admitted\":" + (manyLaneExecution ? L"true" : L"false") +
            L",\"generalized_topology_admitted\":" +
                (executionPlan.generalizedTopologyAdmitted ? L"true" : L"false") +
            L",\"dispatch_group_shape\":" + (profile.laneParametric
                ? (L"[" + std::to_wstring(executionPlan.dispatchGroupShape[0]) + L"," +
                    std::to_wstring(executionPlan.dispatchGroupShape[1]) + L"," +
                    std::to_wstring(executionPlan.dispatchGroupShape[2]) + L"]")
                : L"[1,1,1]") +
            L",\"workgroup_thread_count\":" +
                std::to_wstring(profile.laneParametric ? executionPlan.workgroupThreadCount : 1) +
            L",\"partial_group_threads_guarded\":" +
                (executionPlan.generalizedTopologyAdmitted ? L"true" : L"false") +
            L",\"spmd_lockstep_epoch_validation\":" + (manyLaneExecution ? L"true" : L"false") +
            L",\"spmd_execution_plan_sha256\":" + (manyLaneExecution
                ? JsonString(executionPlan.spmdExecutionPlanSha256) : L"null") +
            L",\"grid_shape\":" + (profile.laneParametric
                ? (L"[" + std::to_wstring(executionPlan.gridShape[0]) + L"," + std::to_wstring(executionPlan.gridShape[1]) + L"," + std::to_wstring(executionPlan.gridShape[2]) + L"]")
                : L"[1,1,1]") +
            L",\"workgroup_shape\":" + (profile.laneParametric
                ? (L"[" + std::to_wstring(executionPlan.workgroupShape[0]) + L"," + std::to_wstring(executionPlan.workgroupShape[1]) + L"," + std::to_wstring(executionPlan.workgroupShape[2]) + L"]")
                : L"[1,1,1]") +
            L",\"gpu_resident_state\":" + (profile.residentExecution ? L"true" : L"false") +
            L",\"cancel_checks_at_epoch_boundaries\":" + (profile.residentExecution ? L"true" : L"false") +
            L",\"fuel_consumed\":" + std::to_wstring(result.run.fuelConsumed) +
            L",\"memory_bytes\":" + std::to_wstring(limits.memoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(limits.outputBytes) +
            L",\"runtime_shader_compilation_used\":false" +
            L",\"arbitrary_native_or_host_code_executed\":false}";
        return result;
    }
}
