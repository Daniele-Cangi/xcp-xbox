#include "pch.h"
#include "WorkerPrecompiledD3DCommandRuntime.h"
#include "WorkerProtocolBoundary.h"
#include "../native/StaticNativeModule.h"

namespace XComputeProbe
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace winrt::Windows::ApplicationModel;

        constexpr uint32_t D3D11ComputeThreadsPerGroup = 64;
        constexpr uint64_t D3D11FloatOpsPerElement = 32ull * 4ull * 2ull;
        constexpr uint64_t MaxD3D11QueryWaitMs = 10000;
        constexpr double D3D11FloatAbsoluteTolerance = 0.05;
        constexpr double D3D11FloatRelativeTolerance = 0.001;

    struct D3D11ResidentHotLoopResources
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t inputBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t gpuBytesPerDispatch = 0;
        winrt::com_ptr<ID3D11Buffer> inputBuffer;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        winrt::com_ptr<ID3D11Query> disjointQuery;
        winrt::com_ptr<ID3D11Query> startQuery;
        winrt::com_ptr<ID3D11Query> stopQuery;
        bool timestampQuerySupported = false;
        std::wstring timingError;
    };

    static std::wstring Utf8ToWide(std::string const& value)
    {
        return winrt::to_hstring(value).c_str();
    }

    static std::string WideToUtf8(std::wstring const& value)
    {
        return winrt::to_string(winrt::hstring(value));
    }

    static double ElapsedMilliseconds(std::chrono::steady_clock::time_point const& started)
    {
        auto elapsed = std::chrono::steady_clock::now() - started;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

    static std::wstring HResultToString(HRESULT hr)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
        return out.str();
    }

    static uint32_t ExpectedD3D11ComputeValue(uint32_t index)
    {
        uint32_t x = index + 1;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }

    static D3D11Float4 D3D11MatrixInputValue(uint32_t index)
    {
        float seed = static_cast<float>(index + 1u);
        D3D11Float4 value;
        value.x = seed;
        value.y = seed * 0.5f;
        value.z = seed * 0.25f;
        value.w = 1.0f;
        return value;
    }

    static void ApplyD3D11FloatAluLoop(D3D11Float4& value, uint32_t loopCount)
    {
        for (uint32_t i = 0; i < loopCount; ++i)
        {
            value.x = value.x * 1.0001f + 0.01f;
            value.y = value.y * 0.9997f + 0.02f;
            value.z = value.z * 1.0003f + 0.03f;
            value.w = value.w * 0.5f;
        }
    }

    static uint32_t MixD3D11DomainHash32(uint32_t value)
    {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value;
    }

    static float UnitD3D11DomainHash32(uint32_t value)
    {
        return static_cast<float>(value & 0xffffu) / 65535.0f;
    }

    static D3D11Float4 ExpectedD3D11DomainHashMixValue(uint32_t index)
    {
        auto seed = D3D11MatrixInputValue(index);
        uint32_t h = index + 1u;
        for (uint32_t i = 0; i < 8; ++i)
        {
            h = MixD3D11DomainHash32(h + 0x9e3779b9u + (i * 0x85ebca6bu));
        }

        auto h1 = h;
        auto h2 = MixD3D11DomainHash32(h1 + 0x7f4a7c15u);
        auto h3 = MixD3D11DomainHash32(h2 + 0x94d049bbu);
        auto h4 = MixD3D11DomainHash32(h3 + 0x2545f491u);

        D3D11Float4 value;
        value.x = UnitD3D11DomainHash32(h1) + seed.w;
        value.y = UnitD3D11DomainHash32(h2) + seed.w * 0.5f;
        value.z = UnitD3D11DomainHash32(h3) + seed.w * 0.25f;
        value.w = UnitD3D11DomainHash32(h4);
        return value;
    }

    static D3D11Float4 ExpectedD3D11DomainImageKernelValue(uint32_t index)
    {
        auto value = D3D11MatrixInputValue(index);
        for (uint32_t i = 0; i < 12; ++i)
        {
            float luma = value.x * 0.299f + value.y * 0.587f + value.z * 0.114f;
            float blur = (value.x + value.y + value.z + value.w) * 0.25f;
            float edge = (value.x - value.y) * 0.5f + (value.z - value.w) * 0.25f;
            float sharpen = luma * 1.5f - blur * 0.35f + 0.01f;

            D3D11Float4 next;
            next.x = sharpen;
            next.y = blur * 0.92f + edge * 0.08f + 0.02f;
            next.z = luma * 0.85f + edge * 0.15f + 0.03f;
            next.w = value.w * 0.60f + luma * 0.40f;
            value = next;
        }
        return value;
    }

    static D3D11Float4 ExpectedD3D11DomainMatrixTileValue(uint32_t index)
    {
        auto value = D3D11MatrixInputValue(index);
        for (uint32_t i = 0; i < 16; ++i)
        {
            D3D11Float4 next;
            next.x = value.x * 0.92f + value.y * 0.04f + value.z * -0.02f + value.w * 0.01f + 0.010f;
            next.y = value.x * 0.03f + value.y * 0.91f + value.z * 0.05f + value.w * -0.01f + 0.020f;
            next.z = value.x * -0.02f + value.y * 0.06f + value.z * 0.89f + value.w * 0.04f + 0.030f;
            next.w = value.x * 0.01f + value.y * -0.03f + value.z * 0.04f + value.w * 0.94f + 0.040f;
            value = next;
        }
        return value;
    }

    static D3D11Float4 ExpectedD3D11FloatComputeValue(uint32_t index)
    {
        auto value = D3D11MatrixInputValue(index);
        ApplyD3D11FloatAluLoop(value, 32);
        return value;
    }

    static D3D11Float4 ExpectedD3D11ShaderMatrixValue(uint32_t index, D3D11ShaderMatrixVariant const& variant)
    {
        if (variant.id == L"domain_hash_mix")
        {
            return ExpectedD3D11DomainHashMixValue(index);
        }
        if (variant.id == L"domain_image_kernel")
        {
            return ExpectedD3D11DomainImageKernelValue(index);
        }
        if (variant.id == L"domain_matrix_tile")
        {
            return ExpectedD3D11DomainMatrixTileValue(index);
        }

        auto value = D3D11MatrixInputValue(index);
        ApplyD3D11FloatAluLoop(value, variant.loopCount);
        return value;
    }

    static double AbsDouble(double value)
    {
        return value < 0.0 ? -value : value;
    }

    static bool FloatComponentMatches(float actual, float expected, double& absError, double& relativeError)
    {
        if (!std::isfinite(actual))
        {
            absError = 1.0e30;
            relativeError = 1.0e30;
            return false;
        }

        absError = AbsDouble(static_cast<double>(actual) - static_cast<double>(expected));
        auto denominator = AbsDouble(static_cast<double>(expected));
        if (denominator < 1.0)
        {
            denominator = 1.0;
        }
        relativeError = absError / denominator;

        auto tolerance = D3D11FloatAbsoluteTolerance;
        auto relativeTolerance = D3D11FloatRelativeTolerance * denominator;
        if (relativeTolerance > tolerance)
        {
            tolerance = relativeTolerance;
        }
        return absError <= tolerance;
    }

    static D3DDoubleSampleStats ComputeD3DDoubleSampleStats(std::vector<double> values)
    {
        D3DDoubleSampleStats stats;
        stats.count = static_cast<uint64_t>(values.size());
        if (values.empty())
        {
            return stats;
        }

        std::sort(values.begin(), values.end());
        stats.min = values.front();
        stats.max = values.back();
        auto const n = values.size();
        stats.median = (n % 2) == 0
            ? (values[(n / 2) - 1] + values[n / 2]) / 2.0
            : values[n / 2];

        auto p95Index = static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.95));
        if (p95Index == 0)
        {
            p95Index = 1;
        }
        if (p95Index > n)
        {
            p95Index = n;
        }
        stats.p95 = values[p95Index - 1];

        double sum = 0.0;
        for (auto value : values)
        {
            sum += value;
        }
        stats.mean = sum / static_cast<double>(n);

        double variance = 0.0;
        for (auto value : values)
        {
            auto delta = value - stats.mean;
            variance += delta * delta;
        }
        stats.stddev = std::sqrt(variance / static_cast<double>(n));
        return stats;
    }

    static WorkerD3DEnvironmentObservation ObserveD3DEnvironment(
        WorkerD3DEnvironmentObserver const& observeEnvironment)
    {
        return observeEnvironment
            ? observeEnvironment()
            : WorkerD3DEnvironmentObservation{};
    }

    static std::vector<uint8_t> ReadInstalledBinaryFile(std::wstring const& relativeFileName)
    {
        auto installedPath = std::wstring(Package::Current().InstalledLocation().Path().c_str());
        auto path = fs::path(installedPath) / relativeFileName;
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("d3d11_compute.shader_not_found", "could not open installed shader bytecode");
        }
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    }

    D3D12AdapterProbe WorkerReadD3D12AdapterProbe(
        uint32_t index,
        IDXGIAdapter1* adapter)
    {
        D3D12AdapterProbe probe;
        probe.index = index;

        DXGI_ADAPTER_DESC1 desc{};
        if (adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            probe.description = desc.Description;
            probe.vendorId = desc.VendorId;
            probe.deviceId = desc.DeviceId;
            probe.subSysId = desc.SubSysId;
            probe.revision = desc.Revision;
            probe.dedicatedVideoMemory =
                static_cast<uint64_t>(desc.DedicatedVideoMemory);
            probe.dedicatedSystemMemory =
                static_cast<uint64_t>(desc.DedicatedSystemMemory);
            probe.sharedSystemMemory =
                static_cast<uint64_t>(desc.SharedSystemMemory);
            probe.flags = desc.Flags;
            probe.software =
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        }

        return probe;
    }

    D3D12DeviceProbe WorkerRunD3D12DeviceProbe()
    {
        D3D12DeviceProbe probe;

        winrt::com_ptr<IDXGIFactory1> factory;
        probe.factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
        probe.factoryCreated = SUCCEEDED(probe.factoryHr);
        if (!probe.factoryCreated)
        {
            return probe;
        }

        winrt::com_ptr<IDXGIAdapter1> selectedAdapter;
        for (uint32_t index = 0; ; ++index)
        {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            auto enumHr = factory->EnumAdapters1(index, adapter.put());
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(enumHr))
            {
                break;
            }

            auto adapterProbe =
                WorkerReadD3D12AdapterProbe(index, adapter.get());
            if (!selectedAdapter && !adapterProbe.software)
            {
                selectedAdapter = adapter;
                probe.selectedAdapterIndex = index;
                probe.selectedAdapter = adapterProbe;
            }
            probe.adapters.push_back(adapterProbe);
        }

        probe.adapterCount = static_cast<uint32_t>(probe.adapters.size());
        if (!selectedAdapter && !probe.adapters.empty())
        {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(factory->EnumAdapters1(0, adapter.put())))
            {
                selectedAdapter = adapter;
                probe.selectedAdapterIndex = 0;
                probe.selectedAdapter = probe.adapters[0];
            }
        }

        if (!selectedAdapter)
        {
            probe.probeNullHr = DXGI_ERROR_NOT_FOUND;
            probe.createDeviceHr = DXGI_ERROR_NOT_FOUND;
            return probe;
        }

        probe.probeNullHr = D3D12CreateDevice(
            selectedAdapter.get(),
            D3D_FEATURE_LEVEL_11_0,
            __uuidof(ID3D12Device),
            nullptr);
        probe.d3d12ProbeSucceeded = SUCCEEDED(probe.probeNullHr);

        winrt::com_ptr<ID3D12Device> device;
        probe.createDeviceHr = D3D12CreateDevice(
            selectedAdapter.get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.put()));
        probe.deviceCreated = SUCCEEDED(probe.createDeviceHr) && device;
        if (!probe.deviceCreated)
        {
            return probe;
        }

        D3D_FEATURE_LEVEL requestedFeatureLevels[] =
        {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{};
        featureLevels.NumFeatureLevels = ARRAYSIZE(requestedFeatureLevels);
        featureLevels.pFeatureLevelsRequested = requestedFeatureLevels;
        probe.checkFeatureLevelsHr = device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS,
            &featureLevels,
            sizeof(featureLevels));
        probe.featureLevelsChecked = SUCCEEDED(probe.checkFeatureLevelsHr);
        if (probe.featureLevelsChecked)
        {
            probe.maxSupportedFeatureLevel =
                featureLevels.MaxSupportedFeatureLevel;
        }

        probe.optionsHr = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS,
            &probe.options,
            sizeof(probe.options));
        probe.deviceRemovedReason = device->GetDeviceRemovedReason();

        return probe;
    }

    D3D12ComputeSmokeMeasurement WorkerRunD3D12ComputeSmoke(uint64_t requestedElements)
    {
        D3D12ComputeSmokeMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(uint32_t);

        auto shaderBytes = ReadInstalledBinaryFile(L"IntCompute.cso");
        measurement.shaderBytes = static_cast<uint64_t>(shaderBytes.size());
        if (shaderBytes.empty() || measurement.bytes > UINT32_MAX)
        {
            return measurement;
        }

        winrt::com_ptr<IDXGIFactory1> factory;
        measurement.factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
        if (FAILED(measurement.factoryHr))
        {
            return measurement;
        }

        winrt::com_ptr<IDXGIAdapter1> selectedAdapter;
        for (uint32_t index = 0; ; ++index)
        {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            auto enumHr = factory->EnumAdapters1(index, adapter.put());
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(enumHr))
            {
                break;
            }

            auto adapterProbe = WorkerReadD3D12AdapterProbe(index, adapter.get());
            if (!selectedAdapter && !adapterProbe.software)
            {
                selectedAdapter = adapter;
                measurement.selectedAdapterIndex = index;
                measurement.selectedAdapter = adapterProbe;
            }
        }

        if (!selectedAdapter)
        {
            measurement.createDeviceHr = DXGI_ERROR_NOT_FOUND;
            return measurement;
        }

        winrt::com_ptr<ID3D12Device> device;
        measurement.createDeviceHr = D3D12CreateDevice(
            selectedAdapter.get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.put()));
        measurement.deviceCreated = SUCCEEDED(measurement.createDeviceHr) && device;
        if (!measurement.deviceCreated)
        {
            return measurement;
        }

        D3D12_ROOT_PARAMETER rootParameter{};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rootParameter.Descriptor.ShaderRegister = 0;
        rootParameter.Descriptor.RegisterSpace = 0;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = 1;
        rootSignatureDesc.pParameters = &rootParameter;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        winrt::com_ptr<ID3DBlob> rootSignatureBlob;
        winrt::com_ptr<ID3DBlob> rootSignatureError;
        measurement.rootSignatureHr = D3D12SerializeRootSignature(
            &rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            rootSignatureBlob.put(),
            rootSignatureError.put());
        if (FAILED(measurement.rootSignatureHr) || !rootSignatureBlob)
        {
            return measurement;
        }

        winrt::com_ptr<ID3D12RootSignature> rootSignature;
        measurement.rootSignatureHr = device->CreateRootSignature(
            0,
            rootSignatureBlob->GetBufferPointer(),
            rootSignatureBlob->GetBufferSize(),
            IID_PPV_ARGS(rootSignature.put()));
        measurement.rootSignatureCreated = SUCCEEDED(measurement.rootSignatureHr) && rootSignature;
        if (!measurement.rootSignatureCreated)
        {
            return measurement;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = rootSignature.get();
        pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
        pipelineDesc.CS.BytecodeLength = shaderBytes.size();

        winrt::com_ptr<ID3D12PipelineState> pipelineState;
        measurement.pipelineStateHr = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(pipelineState.put()));
        measurement.pipelineStateCreated = SUCCEEDED(measurement.pipelineStateHr) && pipelineState;
        if (!measurement.pipelineStateCreated)
        {
            return measurement;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        winrt::com_ptr<ID3D12CommandQueue> commandQueue;
        measurement.createCommandQueueHr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put()));
        measurement.commandQueueCreated = SUCCEEDED(measurement.createCommandQueueHr) && commandQueue;
        if (!measurement.commandQueueCreated)
        {
            return measurement;
        }

        winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
        measurement.createCommandAllocatorHr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(commandAllocator.put()));
        if (FAILED(measurement.createCommandAllocatorHr) || !commandAllocator)
        {
            return measurement;
        }

        winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
        measurement.createCommandListHr = device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            commandAllocator.get(),
            pipelineState.get(),
            IID_PPV_ARGS(commandList.put()));
        measurement.commandListCreated = SUCCEEDED(measurement.createCommandListHr) && commandList;
        if (!measurement.commandListCreated)
        {
            return measurement;
        }

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = measurement.bytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        winrt::com_ptr<ID3D12Resource> outputBuffer;
        measurement.outputBufferHr = device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outputBuffer.put()));
        measurement.outputBufferCreated = SUCCEEDED(measurement.outputBufferHr) && outputBuffer;
        if (!measurement.outputBufferCreated)
        {
            return measurement;
        }

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readbackDesc = bufferDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> readbackBuffer;
        measurement.readbackBufferHr = device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put()));
        measurement.readbackBufferCreated = SUCCEEDED(measurement.readbackBufferHr) && readbackBuffer;
        if (!measurement.readbackBufferCreated)
        {
            return measurement;
        }

        commandList->SetPipelineState(pipelineState.get());
        commandList->SetComputeRootSignature(rootSignature.get());
        commandList->SetComputeRootUnorderedAccessView(0, outputBuffer->GetGPUVirtualAddress());
        commandList->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = outputBuffer.get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &barrier);
        commandList->CopyBufferRegion(readbackBuffer.get(), 0, outputBuffer.get(), 0, measurement.bytes);

        measurement.closeCommandListHr = commandList->Close();
        if (FAILED(measurement.closeCommandListHr))
        {
            return measurement;
        }

        winrt::com_ptr<ID3D12Fence> fence;
        measurement.createFenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
        if (FAILED(measurement.createFenceHr) || !fence)
        {
            return measurement;
        }

        auto gpuStarted = std::chrono::steady_clock::now();
        ID3D12CommandList* commandLists[] = { commandList.get() };
        commandQueue->ExecuteCommandLists(1, commandLists);
        measurement.signalFenceHr = commandQueue->Signal(fence.get(), measurement.fenceValue);
        if (FAILED(measurement.signalFenceHr))
        {
            return measurement;
        }

        auto waitStarted = std::chrono::steady_clock::now();
        while (fence->GetCompletedValue() < measurement.fenceValue)
        {
            if (ElapsedMilliseconds(waitStarted) > static_cast<double>(MaxD3D11QueryWaitMs))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        measurement.completedFenceValue = fence->GetCompletedValue();
        measurement.fenceCompleted = measurement.completedFenceValue >= measurement.fenceValue;
        measurement.gpuSubmitAndWaitMs = ElapsedMilliseconds(gpuStarted);
        measurement.deviceRemovedReason = device->GetDeviceRemovedReason();
        if (!measurement.fenceCompleted)
        {
            return measurement;
        }

        auto verifyStarted = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(measurement.bytes);
        measurement.mapHr = readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(measurement.mapHr) || mapped == nullptr)
        {
            return measurement;
        }

        auto values = reinterpret_cast<uint32_t const*>(mapped);
        measurement.hash32 = 2166136261u;
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto actual = values[i];
            auto expected = ExpectedD3D11ComputeValue(static_cast<uint32_t>(i));
            if (i == 0)
            {
                measurement.firstValue = actual;
            }
            if (i + 1 == measurement.elements)
            {
                measurement.lastValue = actual;
            }
            measurement.checksum += actual;
            measurement.hash32 ^= actual;
            measurement.hash32 *= 16777619u;
            if (actual != expected)
            {
                ++measurement.mismatches;
            }
        }

        D3D12_RANGE writtenRange{};
        writtenRange.Begin = 0;
        writtenRange.End = 0;
        readbackBuffer->Unmap(0, &writtenRange);
        measurement.readbackVerifyMs = ElapsedMilliseconds(verifyStarted);
        measurement.verified = measurement.mismatches == 0;
        return measurement;
    }

    D3D12ComputeContext WorkerCreateD3D12ComputeContext(
        std::wstring const& shaderFileName,
        D3D12_COMMAND_LIST_TYPE commandListType,
        bool includeInputSrv)
    {
        D3D12ComputeContext context;
        context.shaderName = shaderFileName;
        context.commandListType = commandListType;

        auto shaderBytes = ReadInstalledBinaryFile(shaderFileName);
        context.shaderBytes = static_cast<uint64_t>(shaderBytes.size());
        if (shaderBytes.empty())
        {
            throw WorkerProtocolError("d3d12_compute.shader_empty", "installed shader bytecode is empty");
        }

        winrt::com_ptr<IDXGIFactory1> factory;
        context.factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
        if (FAILED(context.factoryHr))
        {
            throw WorkerProtocolError("d3d12_compute.create_factory_failed", "CreateDXGIFactory1 failed: " + WideToUtf8(HResultToString(context.factoryHr)));
        }

        winrt::com_ptr<IDXGIAdapter1> selectedAdapter;
        for (uint32_t index = 0; ; ++index)
        {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            auto enumHr = factory->EnumAdapters1(index, adapter.put());
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(enumHr))
            {
                break;
            }

            auto adapterProbe = WorkerReadD3D12AdapterProbe(index, adapter.get());
            if (!selectedAdapter && !adapterProbe.software)
            {
                selectedAdapter = adapter;
                context.selectedAdapterIndex = index;
                context.selectedAdapter = adapterProbe;
            }
        }

        if (!selectedAdapter)
        {
            throw WorkerProtocolError("d3d12_compute.no_hardware_adapter", "no hardware adapter was found for D3D12 compute");
        }

        context.createDeviceHr = D3D12CreateDevice(
            selectedAdapter.get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(context.device.put()));
        context.deviceCreated = SUCCEEDED(context.createDeviceHr) && context.device;
        if (!context.deviceCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_device_failed", "D3D12CreateDevice failed: " + WideToUtf8(HResultToString(context.createDeviceHr)));
        }

        std::array<D3D12_ROOT_PARAMETER, 2> rootParameters{};
        rootParameters[0].ParameterType = includeInputSrv
            ? D3D12_ROOT_PARAMETER_TYPE_SRV
            : D3D12_ROOT_PARAMETER_TYPE_UAV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rootParameters[1].Descriptor.ShaderRegister = 0;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = includeInputSrv ? 2 : 1;
        rootSignatureDesc.pParameters = rootParameters.data();
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        winrt::com_ptr<ID3DBlob> rootSignatureBlob;
        winrt::com_ptr<ID3DBlob> rootSignatureError;
        context.rootSignatureHr = D3D12SerializeRootSignature(
            &rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            rootSignatureBlob.put(),
            rootSignatureError.put());
        if (FAILED(context.rootSignatureHr) || !rootSignatureBlob)
        {
            throw WorkerProtocolError("d3d12_compute.serialize_root_signature_failed", "D3D12SerializeRootSignature failed: " + WideToUtf8(HResultToString(context.rootSignatureHr)));
        }

        context.rootSignatureHr = context.device->CreateRootSignature(
            0,
            rootSignatureBlob->GetBufferPointer(),
            rootSignatureBlob->GetBufferSize(),
            IID_PPV_ARGS(context.rootSignature.put()));
        context.rootSignatureCreated = SUCCEEDED(context.rootSignatureHr) && context.rootSignature;
        if (!context.rootSignatureCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_root_signature_failed", "CreateRootSignature failed: " + WideToUtf8(HResultToString(context.rootSignatureHr)));
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = context.rootSignature.get();
        pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
        pipelineDesc.CS.BytecodeLength = shaderBytes.size();

        context.pipelineStateHr = context.device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(context.pipelineState.put()));
        context.pipelineStateCreated = SUCCEEDED(context.pipelineStateHr) && context.pipelineState;
        if (!context.pipelineStateCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_pipeline_failed", "CreateComputePipelineState failed: " + WideToUtf8(HResultToString(context.pipelineStateHr)));
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = context.commandListType;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        context.createCommandQueueHr = context.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(context.commandQueue.put()));
        context.commandQueueCreated = SUCCEEDED(context.createCommandQueueHr) && context.commandQueue;
        if (!context.commandQueueCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_command_queue_failed", "CreateCommandQueue failed: " + WideToUtf8(HResultToString(context.createCommandQueueHr)));
        }

        return context;
    }

    D3D12ComputeSweepMeasurement WorkerRunD3D12ComputeSweepDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D12ComputeSweepMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(uint32_t);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d12_compute.too_large", "D3D12 compute buffer exceeds supported byte range");
        }

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = measurement.bytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        winrt::com_ptr<ID3D12Resource> outputBuffer;
        measurement.outputBufferHr = context.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outputBuffer.put()));
        measurement.outputBufferCreated = SUCCEEDED(measurement.outputBufferHr) && outputBuffer;
        if (!measurement.outputBufferCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_output_failed", "CreateCommittedResource output failed: " + WideToUtf8(HResultToString(measurement.outputBufferHr)));
        }

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readbackDesc = bufferDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> readbackBuffer;
        measurement.readbackBufferHr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put()));
        measurement.readbackBufferCreated = SUCCEEDED(measurement.readbackBufferHr) && readbackBuffer;
        if (!measurement.readbackBufferCreated)
        {
            throw WorkerProtocolError("d3d12_compute.create_readback_failed", "CreateCommittedResource readback failed: " + WideToUtf8(HResultToString(measurement.readbackBufferHr)));
        }

        winrt::com_ptr<ID3D12Fence> fence;
        measurement.createFenceHr = context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
        if (FAILED(measurement.createFenceHr) || !fence)
        {
            throw WorkerProtocolError("d3d12_compute.create_fence_failed", "CreateFence failed: " + WideToUtf8(HResultToString(measurement.createFenceHr)));
        }

        bool outputInCopySourceState = false;
        auto runIteration = [&](bool measuredIteration) -> double
        {
            winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
            auto hr = context.device->CreateCommandAllocator(context.commandListType, IID_PPV_ARGS(commandAllocator.put()));
            if (FAILED(hr) || !commandAllocator)
            {
                throw WorkerProtocolError("d3d12_compute.create_command_allocator_failed", "CreateCommandAllocator failed: " + WideToUtf8(HResultToString(hr)));
            }

            winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
            hr = context.device->CreateCommandList(
                0,
                context.commandListType,
                commandAllocator.get(),
                context.pipelineState.get(),
                IID_PPV_ARGS(commandList.put()));
            if (FAILED(hr) || !commandList)
            {
                throw WorkerProtocolError("d3d12_compute.create_command_list_failed", "CreateCommandList failed: " + WideToUtf8(HResultToString(hr)));
            }

            if (outputInCopySourceState)
            {
                D3D12_RESOURCE_BARRIER preBarrier{};
                preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                preBarrier.Transition.pResource = outputBuffer.get();
                preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                commandList->ResourceBarrier(1, &preBarrier);
            }

            commandList->SetPipelineState(context.pipelineState.get());
            commandList->SetComputeRootSignature(context.rootSignature.get());
            commandList->SetComputeRootUnorderedAccessView(0, outputBuffer->GetGPUVirtualAddress());
            commandList->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);

            D3D12_RESOURCE_BARRIER postBarrier{};
            postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            postBarrier.Transition.pResource = outputBuffer.get();
            postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &postBarrier);
            commandList->CopyBufferRegion(readbackBuffer.get(), 0, outputBuffer.get(), 0, measurement.bytes);

            hr = commandList->Close();
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_compute.close_command_list_failed", "Close command list failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto submitStarted = std::chrono::steady_clock::now();
            ID3D12CommandList* commandLists[] = { commandList.get() };
            context.commandQueue->ExecuteCommandLists(1, commandLists);
            ++measurement.fenceValue;
            hr = context.commandQueue->Signal(fence.get(), measurement.fenceValue);
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_compute.signal_fence_failed", "Signal fence failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto waitStarted = std::chrono::steady_clock::now();
            while (fence->GetCompletedValue() < measurement.fenceValue)
            {
                if (ElapsedMilliseconds(waitStarted) > static_cast<double>(MaxD3D11QueryWaitMs))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            measurement.completedFenceValue = fence->GetCompletedValue();
            measurement.fenceCompleted = measurement.completedFenceValue >= measurement.fenceValue;
            measurement.deviceRemovedReason = context.device->GetDeviceRemovedReason();
            outputInCopySourceState = true;
            if (!measurement.fenceCompleted)
            {
                throw WorkerProtocolError("d3d12_compute.fence_timeout", "D3D12 fence did not complete before timeout");
            }

            auto elapsedMs = ElapsedMilliseconds(submitStarted);
            return measuredIteration ? elapsedMs : 0.0;
        };

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            runIteration(false);
        }
        for (uint64_t i = 0; i < repeats; ++i)
        {
            measurement.totalGpuSubmitAndWaitMs += runIteration(true);
        }

        auto verifyStarted = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(measurement.bytes);
        auto mapHr = readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            throw WorkerProtocolError("d3d12_compute.map_failed", "Map readback failed: " + WideToUtf8(HResultToString(mapHr)));
        }

        auto values = reinterpret_cast<uint32_t const*>(mapped);
        measurement.hash32 = 2166136261u;
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto actual = values[i];
            auto expected = ExpectedD3D11ComputeValue(static_cast<uint32_t>(i));
            if (i == 0)
            {
                measurement.firstValue = actual;
            }
            if (i + 1 == measurement.elements)
            {
                measurement.lastValue = actual;
            }
            measurement.checksum += actual;
            measurement.hash32 ^= actual;
            measurement.hash32 *= 16777619u;
            if (actual != expected)
            {
                ++measurement.mismatches;
            }
        }

        D3D12_RANGE writtenRange{};
        writtenRange.Begin = 0;
        writtenRange.End = 0;
        readbackBuffer->Unmap(0, &writtenRange);

        measurement.readbackVerifyMs = ElapsedMilliseconds(verifyStarted);
        measurement.verified = measurement.mismatches == 0;
        measurement.averageGpuSubmitAndWaitMs = repeats > 0
            ? measurement.totalGpuSubmitAndWaitMs / static_cast<double>(repeats)
            : 0.0;
        auto totalElements = measurement.elements * repeats;
        auto totalBytes = measurement.bytes * repeats;
        measurement.elementsPerSecond = measurement.totalGpuSubmitAndWaitMs > 0.0
            ? static_cast<double>(totalElements) / (measurement.totalGpuSubmitAndWaitMs / 1000.0)
            : 0.0;
        measurement.mibPerSecond = measurement.totalGpuSubmitAndWaitMs > 0.0
            ? ((static_cast<double>(totalBytes) / 1048576.0) / (measurement.totalGpuSubmitAndWaitMs / 1000.0))
            : 0.0;
        return measurement;
    }

    D3D12ComputeTimingMeasurement WorkerRunD3D12ComputeTimingDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D12ComputeTimingMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(uint32_t);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d12_compute_timing.too_large", "D3D12 compute timing buffer exceeds supported byte range");
        }

        measurement.timestampFrequencyHr = context.commandQueue->GetTimestampFrequency(&measurement.timestampFrequency);
        measurement.timestampQuerySupported = SUCCEEDED(measurement.timestampFrequencyHr) && measurement.timestampFrequency > 0;
        if (!measurement.timestampQuerySupported)
        {
            throw WorkerProtocolError("d3d12_compute_timing.timestamp_frequency_failed", "GetTimestampFrequency failed: " + WideToUtf8(HResultToString(measurement.timestampFrequencyHr)));
        }

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = measurement.bytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        winrt::com_ptr<ID3D12Resource> outputBuffer;
        measurement.outputBufferHr = context.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outputBuffer.put()));
        measurement.outputBufferCreated = SUCCEEDED(measurement.outputBufferHr) && outputBuffer;
        if (!measurement.outputBufferCreated)
        {
            throw WorkerProtocolError("d3d12_compute_timing.create_output_failed", "CreateCommittedResource output failed: " + WideToUtf8(HResultToString(measurement.outputBufferHr)));
        }

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readbackDesc = bufferDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> readbackBuffer;
        measurement.readbackBufferHr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put()));
        measurement.readbackBufferCreated = SUCCEEDED(measurement.readbackBufferHr) && readbackBuffer;
        if (!measurement.readbackBufferCreated)
        {
            throw WorkerProtocolError("d3d12_compute_timing.create_readback_failed", "CreateCommittedResource readback failed: " + WideToUtf8(HResultToString(measurement.readbackBufferHr)));
        }

        D3D12_QUERY_HEAP_DESC queryHeapDesc{};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = 2;
        queryHeapDesc.NodeMask = 0;

        winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
        measurement.timestampQueryHeapHr = context.device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(timestampQueryHeap.put()));
        measurement.timestampQueryHeapCreated = SUCCEEDED(measurement.timestampQueryHeapHr) && timestampQueryHeap;
        if (!measurement.timestampQueryHeapCreated)
        {
            throw WorkerProtocolError("d3d12_compute_timing.create_query_heap_failed", "CreateQueryHeap timestamp failed: " + WideToUtf8(HResultToString(measurement.timestampQueryHeapHr)));
        }

        D3D12_RESOURCE_DESC timestampReadbackDesc{};
        timestampReadbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        timestampReadbackDesc.Alignment = 0;
        timestampReadbackDesc.Width = sizeof(uint64_t) * 2;
        timestampReadbackDesc.Height = 1;
        timestampReadbackDesc.DepthOrArraySize = 1;
        timestampReadbackDesc.MipLevels = 1;
        timestampReadbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        timestampReadbackDesc.SampleDesc.Count = 1;
        timestampReadbackDesc.SampleDesc.Quality = 0;
        timestampReadbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        timestampReadbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> timestampReadbackBuffer;
        measurement.timestampReadbackBufferHr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &timestampReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(timestampReadbackBuffer.put()));
        measurement.timestampReadbackBufferCreated = SUCCEEDED(measurement.timestampReadbackBufferHr) && timestampReadbackBuffer;
        if (!measurement.timestampReadbackBufferCreated)
        {
            throw WorkerProtocolError("d3d12_compute_timing.create_timestamp_readback_failed", "CreateCommittedResource timestamp readback failed: " + WideToUtf8(HResultToString(measurement.timestampReadbackBufferHr)));
        }

        winrt::com_ptr<ID3D12Fence> fence;
        measurement.createFenceHr = context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
        if (FAILED(measurement.createFenceHr) || !fence)
        {
            throw WorkerProtocolError("d3d12_compute_timing.create_fence_failed", "CreateFence failed: " + WideToUtf8(HResultToString(measurement.createFenceHr)));
        }

        struct IterationTiming
        {
            double gpuMs = 0.0;
            double cpuSubmitAndWaitMs = 0.0;
        };

        bool outputInCopySourceState = false;
        auto runIteration = [&](bool measuredIteration) -> IterationTiming
        {
            winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
            auto hr = context.device->CreateCommandAllocator(context.commandListType, IID_PPV_ARGS(commandAllocator.put()));
            if (FAILED(hr) || !commandAllocator)
            {
                throw WorkerProtocolError("d3d12_compute_timing.create_command_allocator_failed", "CreateCommandAllocator failed: " + WideToUtf8(HResultToString(hr)));
            }

            winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
            hr = context.device->CreateCommandList(
                0,
                context.commandListType,
                commandAllocator.get(),
                context.pipelineState.get(),
                IID_PPV_ARGS(commandList.put()));
            if (FAILED(hr) || !commandList)
            {
                throw WorkerProtocolError("d3d12_compute_timing.create_command_list_failed", "CreateCommandList failed: " + WideToUtf8(HResultToString(hr)));
            }

            if (outputInCopySourceState)
            {
                D3D12_RESOURCE_BARRIER preBarrier{};
                preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                preBarrier.Transition.pResource = outputBuffer.get();
                preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                commandList->ResourceBarrier(1, &preBarrier);
            }

            commandList->SetPipelineState(context.pipelineState.get());
            commandList->SetComputeRootSignature(context.rootSignature.get());
            commandList->SetComputeRootUnorderedAccessView(0, outputBuffer->GetGPUVirtualAddress());
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            commandList->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            commandList->ResolveQueryData(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampReadbackBuffer.get(), 0);

            D3D12_RESOURCE_BARRIER postBarrier{};
            postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            postBarrier.Transition.pResource = outputBuffer.get();
            postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &postBarrier);
            commandList->CopyBufferRegion(readbackBuffer.get(), 0, outputBuffer.get(), 0, measurement.bytes);

            hr = commandList->Close();
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_compute_timing.close_command_list_failed", "Close command list failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto submitStarted = std::chrono::steady_clock::now();
            ID3D12CommandList* commandLists[] = { commandList.get() };
            context.commandQueue->ExecuteCommandLists(1, commandLists);
            ++measurement.fenceValue;
            hr = context.commandQueue->Signal(fence.get(), measurement.fenceValue);
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_compute_timing.signal_fence_failed", "Signal fence failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto waitStarted = std::chrono::steady_clock::now();
            while (fence->GetCompletedValue() < measurement.fenceValue)
            {
                if (ElapsedMilliseconds(waitStarted) > static_cast<double>(MaxD3D11QueryWaitMs))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            measurement.completedFenceValue = fence->GetCompletedValue();
            measurement.fenceCompleted = measurement.completedFenceValue >= measurement.fenceValue;
            measurement.deviceRemovedReason = context.device->GetDeviceRemovedReason();
            outputInCopySourceState = true;
            if (!measurement.fenceCompleted)
            {
                throw WorkerProtocolError("d3d12_compute_timing.fence_timeout", "D3D12 fence did not complete before timeout");
            }

            auto cpuMs = ElapsedMilliseconds(submitStarted);
            void* timestampMapped = nullptr;
            D3D12_RANGE timestampReadRange{};
            timestampReadRange.Begin = 0;
            timestampReadRange.End = static_cast<SIZE_T>(sizeof(uint64_t) * 2);
            auto timestampMapHr = timestampReadbackBuffer->Map(0, &timestampReadRange, &timestampMapped);
            if (FAILED(timestampMapHr) || timestampMapped == nullptr)
            {
                throw WorkerProtocolError("d3d12_compute_timing.map_timestamp_failed", "Map timestamp readback failed: " + WideToUtf8(HResultToString(timestampMapHr)));
            }

            auto timestamps = reinterpret_cast<uint64_t const*>(timestampMapped);
            auto timestampBegin = timestamps[0];
            auto timestampEnd = timestamps[1];
            D3D12_RANGE timestampWrittenRange{};
            timestampWrittenRange.Begin = 0;
            timestampWrittenRange.End = 0;
            timestampReadbackBuffer->Unmap(0, &timestampWrittenRange);
            if (timestampEnd < timestampBegin)
            {
                throw WorkerProtocolError("d3d12_compute_timing.timestamp_order_invalid", "timestamp end was smaller than timestamp begin");
            }

            auto gpuMs = (static_cast<double>(timestampEnd - timestampBegin) * 1000.0) /
                static_cast<double>(measurement.timestampFrequency);
            return measuredIteration ? IterationTiming{ gpuMs, cpuMs } : IterationTiming{};
        };

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            runIteration(false);
        }
        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto timing = runIteration(true);
            measurement.totalGpuDispatchMs += timing.gpuMs;
            measurement.totalCpuSubmitAndWaitMs += timing.cpuSubmitAndWaitMs;
        }

        auto verifyStarted = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(measurement.bytes);
        auto mapHr = readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            throw WorkerProtocolError("d3d12_compute_timing.map_failed", "Map readback failed: " + WideToUtf8(HResultToString(mapHr)));
        }

        auto values = reinterpret_cast<uint32_t const*>(mapped);
        measurement.hash32 = 2166136261u;
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto actual = values[i];
            auto expected = ExpectedD3D11ComputeValue(static_cast<uint32_t>(i));
            if (i == 0)
            {
                measurement.firstValue = actual;
            }
            if (i + 1 == measurement.elements)
            {
                measurement.lastValue = actual;
            }
            measurement.checksum += actual;
            measurement.hash32 ^= actual;
            measurement.hash32 *= 16777619u;
            if (actual != expected)
            {
                ++measurement.mismatches;
            }
        }

        D3D12_RANGE writtenRange{};
        writtenRange.Begin = 0;
        writtenRange.End = 0;
        readbackBuffer->Unmap(0, &writtenRange);

        measurement.readbackVerifyMs = ElapsedMilliseconds(verifyStarted);
        measurement.verified = measurement.mismatches == 0;
        measurement.gpuTimingAvailable = measurement.timestampQuerySupported && measurement.totalGpuDispatchMs >= 0.0;
        measurement.averageGpuDispatchMs = repeats > 0
            ? measurement.totalGpuDispatchMs / static_cast<double>(repeats)
            : 0.0;
        measurement.averageCpuSubmitAndWaitMs = repeats > 0
            ? measurement.totalCpuSubmitAndWaitMs / static_cast<double>(repeats)
            : 0.0;
        auto totalElements = measurement.elements * repeats;
        auto totalBytes = measurement.bytes * repeats;
        measurement.gpuElementsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalElements) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytes) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;
        return measurement;
    }

    D3D12FloatTimingMeasurement WorkerRunD3D12FloatTimingDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D12FloatTimingMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(D3D11Float4);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.too_large", "D3D12 FP32 timing buffer exceeds supported byte range");
        }

        measurement.timestampFrequencyHr = context.commandQueue->GetTimestampFrequency(&measurement.timestampFrequency);
        measurement.timestampQuerySupported = SUCCEEDED(measurement.timestampFrequencyHr) && measurement.timestampFrequency > 0;
        if (!measurement.timestampQuerySupported)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.timestamp_frequency_failed", "GetTimestampFrequency failed: " + WideToUtf8(HResultToString(measurement.timestampFrequencyHr)));
        }

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = measurement.bytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        winrt::com_ptr<ID3D12Resource> outputBuffer;
        measurement.outputBufferHr = context.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outputBuffer.put()));
        measurement.outputBufferCreated = SUCCEEDED(measurement.outputBufferHr) && outputBuffer;
        if (!measurement.outputBufferCreated)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.create_output_failed", "CreateCommittedResource output failed: " + WideToUtf8(HResultToString(measurement.outputBufferHr)));
        }

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readbackDesc = bufferDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> readbackBuffer;
        measurement.readbackBufferHr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put()));
        measurement.readbackBufferCreated = SUCCEEDED(measurement.readbackBufferHr) && readbackBuffer;
        if (!measurement.readbackBufferCreated)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.create_readback_failed", "CreateCommittedResource readback failed: " + WideToUtf8(HResultToString(measurement.readbackBufferHr)));
        }

        D3D12_QUERY_HEAP_DESC queryHeapDesc{};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = 2;
        queryHeapDesc.NodeMask = 0;

        winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
        measurement.timestampQueryHeapHr = context.device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(timestampQueryHeap.put()));
        measurement.timestampQueryHeapCreated = SUCCEEDED(measurement.timestampQueryHeapHr) && timestampQueryHeap;
        if (!measurement.timestampQueryHeapCreated)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.create_query_heap_failed", "CreateQueryHeap timestamp failed: " + WideToUtf8(HResultToString(measurement.timestampQueryHeapHr)));
        }

        D3D12_RESOURCE_DESC timestampReadbackDesc{};
        timestampReadbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        timestampReadbackDesc.Alignment = 0;
        timestampReadbackDesc.Width = sizeof(uint64_t) * 2;
        timestampReadbackDesc.Height = 1;
        timestampReadbackDesc.DepthOrArraySize = 1;
        timestampReadbackDesc.MipLevels = 1;
        timestampReadbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        timestampReadbackDesc.SampleDesc.Count = 1;
        timestampReadbackDesc.SampleDesc.Quality = 0;
        timestampReadbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        timestampReadbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> timestampReadbackBuffer;
        measurement.timestampReadbackBufferHr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &timestampReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(timestampReadbackBuffer.put()));
        measurement.timestampReadbackBufferCreated = SUCCEEDED(measurement.timestampReadbackBufferHr) && timestampReadbackBuffer;
        if (!measurement.timestampReadbackBufferCreated)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.create_timestamp_readback_failed", "CreateCommittedResource timestamp readback failed: " + WideToUtf8(HResultToString(measurement.timestampReadbackBufferHr)));
        }

        winrt::com_ptr<ID3D12Fence> fence;
        measurement.createFenceHr = context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
        if (FAILED(measurement.createFenceHr) || !fence)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.create_fence_failed", "CreateFence failed: " + WideToUtf8(HResultToString(measurement.createFenceHr)));
        }

        struct IterationTiming
        {
            double gpuMs = 0.0;
            double cpuSubmitAndWaitMs = 0.0;
        };

        bool outputInCopySourceState = false;
        auto runIteration = [&](bool measuredIteration) -> IterationTiming
        {
            winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
            auto hr = context.device->CreateCommandAllocator(context.commandListType, IID_PPV_ARGS(commandAllocator.put()));
            if (FAILED(hr) || !commandAllocator)
            {
                throw WorkerProtocolError("d3d12_fp32_timing.create_command_allocator_failed", "CreateCommandAllocator failed: " + WideToUtf8(HResultToString(hr)));
            }

            winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
            hr = context.device->CreateCommandList(
                0,
                context.commandListType,
                commandAllocator.get(),
                context.pipelineState.get(),
                IID_PPV_ARGS(commandList.put()));
            if (FAILED(hr) || !commandList)
            {
                throw WorkerProtocolError("d3d12_fp32_timing.create_command_list_failed", "CreateCommandList failed: " + WideToUtf8(HResultToString(hr)));
            }

            if (outputInCopySourceState)
            {
                D3D12_RESOURCE_BARRIER preBarrier{};
                preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                preBarrier.Transition.pResource = outputBuffer.get();
                preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                commandList->ResourceBarrier(1, &preBarrier);
            }

            commandList->SetPipelineState(context.pipelineState.get());
            commandList->SetComputeRootSignature(context.rootSignature.get());
            commandList->SetComputeRootUnorderedAccessView(0, outputBuffer->GetGPUVirtualAddress());
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            commandList->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            commandList->ResolveQueryData(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampReadbackBuffer.get(), 0);

            D3D12_RESOURCE_BARRIER postBarrier{};
            postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            postBarrier.Transition.pResource = outputBuffer.get();
            postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &postBarrier);
            commandList->CopyBufferRegion(readbackBuffer.get(), 0, outputBuffer.get(), 0, measurement.bytes);

            hr = commandList->Close();
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_fp32_timing.close_command_list_failed", "Close command list failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto submitStarted = std::chrono::steady_clock::now();
            ID3D12CommandList* commandLists[] = { commandList.get() };
            context.commandQueue->ExecuteCommandLists(1, commandLists);
            ++measurement.fenceValue;
            hr = context.commandQueue->Signal(fence.get(), measurement.fenceValue);
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_fp32_timing.signal_fence_failed", "Signal fence failed: " + WideToUtf8(HResultToString(hr)));
            }

            auto waitStarted = std::chrono::steady_clock::now();
            while (fence->GetCompletedValue() < measurement.fenceValue)
            {
                if (ElapsedMilliseconds(waitStarted) > static_cast<double>(MaxD3D11QueryWaitMs))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            measurement.completedFenceValue = fence->GetCompletedValue();
            measurement.fenceCompleted = measurement.completedFenceValue >= measurement.fenceValue;
            measurement.deviceRemovedReason = context.device->GetDeviceRemovedReason();
            outputInCopySourceState = true;
            if (!measurement.fenceCompleted)
            {
                throw WorkerProtocolError("d3d12_fp32_timing.fence_timeout", "D3D12 fence did not complete before timeout");
            }

            auto cpuMs = ElapsedMilliseconds(submitStarted);
            void* timestampMapped = nullptr;
            D3D12_RANGE timestampReadRange{};
            timestampReadRange.Begin = 0;
            timestampReadRange.End = static_cast<SIZE_T>(sizeof(uint64_t) * 2);
            auto timestampMapHr = timestampReadbackBuffer->Map(0, &timestampReadRange, &timestampMapped);
            if (FAILED(timestampMapHr) || timestampMapped == nullptr)
            {
                throw WorkerProtocolError("d3d12_fp32_timing.map_timestamp_failed", "Map timestamp readback failed: " + WideToUtf8(HResultToString(timestampMapHr)));
            }

            auto timestamps = reinterpret_cast<uint64_t const*>(timestampMapped);
            auto timestampBegin = timestamps[0];
            auto timestampEnd = timestamps[1];
            D3D12_RANGE timestampWrittenRange{};
            timestampWrittenRange.Begin = 0;
            timestampWrittenRange.End = 0;
            timestampReadbackBuffer->Unmap(0, &timestampWrittenRange);
            if (timestampEnd < timestampBegin)
            {
                throw WorkerProtocolError("d3d12_fp32_timing.timestamp_order_invalid", "timestamp end was smaller than timestamp begin");
            }

            auto gpuMs = (static_cast<double>(timestampEnd - timestampBegin) * 1000.0) /
                static_cast<double>(measurement.timestampFrequency);
            return measuredIteration ? IterationTiming{ gpuMs, cpuMs } : IterationTiming{};
        };

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            runIteration(false);
        }
        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto timing = runIteration(true);
            measurement.totalGpuDispatchMs += timing.gpuMs;
            measurement.totalCpuSubmitAndWaitMs += timing.cpuSubmitAndWaitMs;
        }

        auto verifyStarted = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(measurement.bytes);
        auto mapHr = readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            throw WorkerProtocolError("d3d12_fp32_timing.map_failed", "Map readback failed: " + WideToUtf8(HResultToString(mapHr)));
        }

        auto values = reinterpret_cast<D3D11Float4 const*>(mapped);
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values),
            static_cast<size_t>(measurement.bytes));
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto const& actual = values[static_cast<size_t>(i)];
            auto expected = ExpectedD3D11FloatComputeValue(static_cast<uint32_t>(i));
            bool elementMatches = true;
            double absError = 0.0;
            double relativeError = 0.0;

            if (i == 0)
            {
                measurement.firstValue = actual;
            }
            if (i + 1 == measurement.elements)
            {
                measurement.lastValue = actual;
            }

            if (!FloatComponentMatches(actual.x, expected.x, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.y, expected.y, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.z, expected.z, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.w, expected.w, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!elementMatches)
            {
                ++measurement.mismatches;
            }
            measurement.checksum += static_cast<double>(actual.x) +
                static_cast<double>(actual.y) +
                static_cast<double>(actual.z) +
                static_cast<double>(actual.w);
        }

        D3D12_RANGE writtenRange{};
        writtenRange.Begin = 0;
        writtenRange.End = 0;
        readbackBuffer->Unmap(0, &writtenRange);

        measurement.readbackVerifyMs = ElapsedMilliseconds(verifyStarted);
        measurement.verified = measurement.mismatches == 0;
        measurement.gpuTimingAvailable = measurement.timestampQuerySupported && measurement.totalGpuDispatchMs > 0.0;
        measurement.averageGpuDispatchMs = repeats > 0
            ? measurement.totalGpuDispatchMs / static_cast<double>(repeats)
            : 0.0;
        measurement.averageCpuSubmitAndWaitMs = repeats > 0
            ? measurement.totalCpuSubmitAndWaitMs / static_cast<double>(repeats)
            : 0.0;
        measurement.fp32OpsTimed = measurement.elements * repeats * measurement.fp32OpsPerElement;
        auto totalBytes = measurement.bytes * repeats;
        measurement.fp32OpsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(measurement.fp32OpsTimed) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gflops = measurement.fp32OpsPerSecond / 1000000000.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytes) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;
        return measurement;
    }

    D3D11ShaderMatrixMeasurement WorkerRunD3D12ShaderShapeTimingDispatch(
        D3D12ComputeContext const& context,
        D3D11ShaderMatrixVariant const& variant,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D11ShaderMatrixMeasurement measurement;
        measurement.variant = variant;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.shaderBytes = context.shaderBytes;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.inputBytes = measurement.elements * sizeof(D3D11Float4);
        measurement.outputBytes = measurement.elements * sizeof(D3D11Float4);
        measurement.gpuBytesPerDispatch = measurement.elements * measurement.variant.gpuBytesPerElement;
        if (measurement.inputBytes > UINT32_MAX || measurement.outputBytes > UINT32_MAX || measurement.gpuBytesPerDispatch > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d12_shader_shape.too_large", "D3D12 shader shape buffer exceeds supported byte range");
        }

        std::vector<D3D11Float4> inputValues(static_cast<size_t>(measurement.elements));
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            inputValues[static_cast<size_t>(i)] = D3D11MatrixInputValue(static_cast<uint32_t>(i));
        }

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC inputDesc{};
        inputDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        inputDesc.Alignment = 0;
        inputDesc.Width = measurement.inputBytes;
        inputDesc.Height = 1;
        inputDesc.DepthOrArraySize = 1;
        inputDesc.MipLevels = 1;
        inputDesc.Format = DXGI_FORMAT_UNKNOWN;
        inputDesc.SampleDesc.Count = 1;
        inputDesc.SampleDesc.Quality = 0;
        inputDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        inputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> inputUploadBuffer;
        auto hr = context.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &inputDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(inputUploadBuffer.put()));
        if (FAILED(hr) || !inputUploadBuffer)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_input_upload_failed", "CreateCommittedResource input upload failed: " + WideToUtf8(HResultToString(hr)));
        }

        void* inputMapped = nullptr;
        D3D12_RANGE inputReadRange{};
        auto inputMapHr = inputUploadBuffer->Map(0, &inputReadRange, &inputMapped);
        if (FAILED(inputMapHr) || inputMapped == nullptr)
        {
            throw WorkerProtocolError("d3d12_shader_shape.map_input_upload_failed", "Map input upload failed: " + WideToUtf8(HResultToString(inputMapHr)));
        }
        std::memcpy(inputMapped, inputValues.data(), static_cast<size_t>(measurement.inputBytes));
        D3D12_RANGE inputWrittenRange{};
        inputWrittenRange.Begin = 0;
        inputWrittenRange.End = static_cast<SIZE_T>(measurement.inputBytes);
        inputUploadBuffer->Unmap(0, &inputWrittenRange);

        winrt::com_ptr<ID3D12Resource> inputBuffer;
        hr = context.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &inputDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(inputBuffer.put()));
        if (FAILED(hr) || !inputBuffer)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_input_failed", "CreateCommittedResource input failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D12_RESOURCE_DESC outputDesc = inputDesc;
        outputDesc.Width = measurement.outputBytes;
        outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        winrt::com_ptr<ID3D12Resource> outputBuffer;
        hr = context.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &outputDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outputBuffer.put()));
        if (FAILED(hr) || !outputBuffer)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_output_failed", "CreateCommittedResource output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D12_RESOURCE_DESC readbackDesc = outputDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> readbackBuffer;
        hr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put()));
        if (FAILED(hr) || !readbackBuffer)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_readback_failed", "CreateCommittedResource readback failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D12_QUERY_HEAP_DESC queryHeapDesc{};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = 2;
        queryHeapDesc.NodeMask = 0;

        winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
        hr = context.device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(timestampQueryHeap.put()));
        if (FAILED(hr) || !timestampQueryHeap)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_query_heap_failed", "CreateQueryHeap timestamp failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D12_RESOURCE_DESC timestampReadbackDesc{};
        timestampReadbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        timestampReadbackDesc.Alignment = 0;
        timestampReadbackDesc.Width = sizeof(uint64_t) * 2;
        timestampReadbackDesc.Height = 1;
        timestampReadbackDesc.DepthOrArraySize = 1;
        timestampReadbackDesc.MipLevels = 1;
        timestampReadbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        timestampReadbackDesc.SampleDesc.Count = 1;
        timestampReadbackDesc.SampleDesc.Quality = 0;
        timestampReadbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        timestampReadbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        winrt::com_ptr<ID3D12Resource> timestampReadbackBuffer;
        hr = context.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &timestampReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(timestampReadbackBuffer.put()));
        if (FAILED(hr) || !timestampReadbackBuffer)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_timestamp_readback_failed", "CreateCommittedResource timestamp readback failed: " + WideToUtf8(HResultToString(hr)));
        }

        measurement.timestampQuerySupported = SUCCEEDED(context.commandQueue->GetTimestampFrequency(&measurement.gpuTimestampFrequency)) &&
            measurement.gpuTimestampFrequency > 0;
        if (!measurement.timestampQuerySupported)
        {
            throw WorkerProtocolError("d3d12_shader_shape.timestamp_frequency_failed", "GetTimestampFrequency failed");
        }

        winrt::com_ptr<ID3D12Fence> fence;
        hr = context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()));
        if (FAILED(hr) || !fence)
        {
            throw WorkerProtocolError("d3d12_shader_shape.create_fence_failed", "CreateFence failed: " + WideToUtf8(HResultToString(hr)));
        }

        uint64_t fenceValue = 0;
        auto waitForFence = [&]()
        {
            auto waitStarted = std::chrono::steady_clock::now();
            while (fence->GetCompletedValue() < fenceValue)
            {
                if (ElapsedMilliseconds(waitStarted) > static_cast<double>(MaxD3D11QueryWaitMs))
                {
                    throw WorkerProtocolError("d3d12_shader_shape.fence_timeout", "D3D12 fence did not complete before timeout");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };

        auto executeCommandList = [&](ID3D12GraphicsCommandList* commandList)
        {
            ID3D12CommandList* commandLists[] = { commandList };
            context.commandQueue->ExecuteCommandLists(1, commandLists);
            ++fenceValue;
            auto signalHr = context.commandQueue->Signal(fence.get(), fenceValue);
            if (FAILED(signalHr))
            {
                throw WorkerProtocolError("d3d12_shader_shape.signal_fence_failed", "Signal fence failed: " + WideToUtf8(HResultToString(signalHr)));
            }
            waitForFence();
        };

        {
            winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
            hr = context.device->CreateCommandAllocator(context.commandListType, IID_PPV_ARGS(commandAllocator.put()));
            if (FAILED(hr) || !commandAllocator)
            {
                throw WorkerProtocolError("d3d12_shader_shape.create_init_allocator_failed", "CreateCommandAllocator init failed: " + WideToUtf8(HResultToString(hr)));
            }

            winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
            hr = context.device->CreateCommandList(
                0,
                context.commandListType,
                commandAllocator.get(),
                nullptr,
                IID_PPV_ARGS(commandList.put()));
            if (FAILED(hr) || !commandList)
            {
                throw WorkerProtocolError("d3d12_shader_shape.create_init_list_failed", "CreateCommandList init failed: " + WideToUtf8(HResultToString(hr)));
            }

            commandList->CopyBufferRegion(inputBuffer.get(), 0, inputUploadBuffer.get(), 0, measurement.inputBytes);
            D3D12_RESOURCE_BARRIER inputBarrier{};
            inputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            inputBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            inputBarrier.Transition.pResource = inputBuffer.get();
            inputBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            inputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            inputBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            commandList->ResourceBarrier(1, &inputBarrier);

            hr = commandList->Close();
            if (FAILED(hr))
            {
                throw WorkerProtocolError("d3d12_shader_shape.close_init_list_failed", "Close init command list failed: " + WideToUtf8(HResultToString(hr)));
            }
            executeCommandList(commandList.get());
        }

        struct IterationTiming
        {
            double gpuMs = 0.0;
            double cpuSubmitAndWaitMs = 0.0;
        };

        bool outputInCopySourceState = false;
        auto runIteration = [&](bool measuredIteration) -> IterationTiming
        {
            winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
            auto iterHr = context.device->CreateCommandAllocator(context.commandListType, IID_PPV_ARGS(commandAllocator.put()));
            if (FAILED(iterHr) || !commandAllocator)
            {
                throw WorkerProtocolError("d3d12_shader_shape.create_command_allocator_failed", "CreateCommandAllocator failed: " + WideToUtf8(HResultToString(iterHr)));
            }

            winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
            iterHr = context.device->CreateCommandList(
                0,
                context.commandListType,
                commandAllocator.get(),
                context.pipelineState.get(),
                IID_PPV_ARGS(commandList.put()));
            if (FAILED(iterHr) || !commandList)
            {
                throw WorkerProtocolError("d3d12_shader_shape.create_command_list_failed", "CreateCommandList failed: " + WideToUtf8(HResultToString(iterHr)));
            }

            if (outputInCopySourceState)
            {
                D3D12_RESOURCE_BARRIER preBarrier{};
                preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                preBarrier.Transition.pResource = outputBuffer.get();
                preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                commandList->ResourceBarrier(1, &preBarrier);
            }

            commandList->SetPipelineState(context.pipelineState.get());
            commandList->SetComputeRootSignature(context.rootSignature.get());
            commandList->SetComputeRootShaderResourceView(0, inputBuffer->GetGPUVirtualAddress());
            commandList->SetComputeRootUnorderedAccessView(1, outputBuffer->GetGPUVirtualAddress());
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            commandList->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
            commandList->EndQuery(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            commandList->ResolveQueryData(timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampReadbackBuffer.get(), 0);

            D3D12_RESOURCE_BARRIER postBarrier{};
            postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            postBarrier.Transition.pResource = outputBuffer.get();
            postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &postBarrier);
            commandList->CopyBufferRegion(readbackBuffer.get(), 0, outputBuffer.get(), 0, measurement.outputBytes);

            iterHr = commandList->Close();
            if (FAILED(iterHr))
            {
                throw WorkerProtocolError("d3d12_shader_shape.close_command_list_failed", "Close command list failed: " + WideToUtf8(HResultToString(iterHr)));
            }

            auto submitStarted = std::chrono::steady_clock::now();
            executeCommandList(commandList.get());
            outputInCopySourceState = true;
            auto cpuMs = ElapsedMilliseconds(submitStarted);

            void* timestampMapped = nullptr;
            D3D12_RANGE timestampReadRange{};
            timestampReadRange.Begin = 0;
            timestampReadRange.End = static_cast<SIZE_T>(sizeof(uint64_t) * 2);
            auto timestampMapHr = timestampReadbackBuffer->Map(0, &timestampReadRange, &timestampMapped);
            if (FAILED(timestampMapHr) || timestampMapped == nullptr)
            {
                throw WorkerProtocolError("d3d12_shader_shape.map_timestamp_failed", "Map timestamp readback failed: " + WideToUtf8(HResultToString(timestampMapHr)));
            }

            auto timestamps = reinterpret_cast<uint64_t const*>(timestampMapped);
            auto timestampBegin = timestamps[0];
            auto timestampEnd = timestamps[1];
            D3D12_RANGE timestampWrittenRange{};
            timestampWrittenRange.Begin = 0;
            timestampWrittenRange.End = 0;
            timestampReadbackBuffer->Unmap(0, &timestampWrittenRange);
            if (timestampEnd < timestampBegin)
            {
                throw WorkerProtocolError("d3d12_shader_shape.timestamp_order_invalid", "timestamp end was smaller than timestamp begin");
            }

            auto gpuMs = (static_cast<double>(timestampEnd - timestampBegin) * 1000.0) /
                static_cast<double>(measurement.gpuTimestampFrequency);
            return measuredIteration ? IterationTiming{ gpuMs, cpuMs } : IterationTiming{};
        };

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            runIteration(false);
        }
        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto timing = runIteration(true);
            measurement.totalGpuDispatchMs += timing.gpuMs;
            measurement.gpuDispatchSamplesMs.push_back(timing.gpuMs);
            ++measurement.gpuTimingSampleCount;
            measurement.totalCpuSubmitMs += timing.cpuSubmitAndWaitMs;
            measurement.cpuSubmitSamplesMs.push_back(timing.cpuSubmitAndWaitMs);
            ++measurement.cpuSubmitSampleCount;
        }

        measurement.gpuTimingAvailable = measurement.gpuTimingSampleCount > 0;
        measurement.averageGpuDispatchMs = measurement.gpuTimingAvailable
            ? measurement.totalGpuDispatchMs / static_cast<double>(measurement.gpuTimingSampleCount)
            : 0.0;
        measurement.averageCpuSubmitMs = measurement.cpuSubmitSampleCount > 0
            ? measurement.totalCpuSubmitMs / static_cast<double>(measurement.cpuSubmitSampleCount)
            : 0.0;
        measurement.fp32OpsTimed = measurement.elements * measurement.gpuTimingSampleCount * measurement.variant.fp32OpsPerElement;
        measurement.gpuBytesTimed = measurement.gpuBytesPerDispatch * measurement.gpuTimingSampleCount;
        measurement.fp32OpsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(measurement.fp32OpsTimed) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gflops = measurement.fp32OpsPerSecond / 1000000000.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(measurement.gpuBytesTimed) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;

        auto verifyStarted = std::chrono::steady_clock::now();
        void* mapped = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(measurement.outputBytes);
        auto mapHr = readbackBuffer->Map(0, &readRange, &mapped);
        measurement.verificationReadbackMs = ElapsedMilliseconds(verifyStarted);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            throw WorkerProtocolError("d3d12_shader_shape.map_failed", "Map readback failed: " + WideToUtf8(HResultToString(mapHr)));
        }

        auto values = reinterpret_cast<D3D11Float4 const*>(mapped);
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values),
            static_cast<size_t>(measurement.outputBytes));
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto const& actual = values[static_cast<size_t>(i)];
            auto expected = ExpectedD3D11ShaderMatrixValue(static_cast<uint32_t>(i), variant);
            bool elementMatches = true;
            double absError = 0.0;
            double relativeError = 0.0;

            if (i == 0)
            {
                measurement.firstValue = actual;
            }
            if (i + 1 == measurement.elements)
            {
                measurement.lastValue = actual;
            }

            if (!FloatComponentMatches(actual.x, expected.x, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.y, expected.y, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.z, expected.z, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.w, expected.w, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!elementMatches)
            {
                ++measurement.mismatches;
            }
            measurement.checksum += static_cast<double>(actual.x) +
                static_cast<double>(actual.y) +
                static_cast<double>(actual.z) +
                static_cast<double>(actual.w);
        }

        D3D12_RANGE writtenRange{};
        writtenRange.Begin = 0;
        writtenRange.End = 0;
        readbackBuffer->Unmap(0, &writtenRange);
        measurement.verificationReadbackMs = ElapsedMilliseconds(verifyStarted);
        measurement.verified = measurement.mismatches == 0;
        return measurement;
    }

    static bool WaitForD3D11QueryData(
        ID3D11DeviceContext* context,
        ID3D11Asynchronous* query,
        void* data,
        UINT dataSize,
        wchar_t const* label)
    {
        auto started = std::chrono::steady_clock::now();
        while (true)
        {
            auto hr = context->GetData(query, data, dataSize, 0);
            if (hr == S_OK)
            {
                return true;
            }
            if (hr != S_FALSE)
            {
                std::wstring message = L"GetData failed for ";
                message += label;
                message += L": ";
                message += HResultToString(hr);
                throw WorkerProtocolError("d3d11_compute.query_getdata_failed", WideToUtf8(message));
            }
            if (ElapsedMilliseconds(started) > static_cast<double>(MaxD3D11QueryWaitMs))
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    D3D11ComputeContext WorkerCreateD3D11ComputeContext(std::wstring const& shaderFileName)
    {
        uint32_t flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D11ComputeContext compute;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            compute.device.put(),
            &compute.featureLevel,
            compute.context.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_device_failed", "D3D11CreateDevice failed: " + WideToUtf8(HResultToString(hr)));
        }

        auto shaderBytes = ReadInstalledBinaryFile(shaderFileName);
        if (shaderBytes.empty())
        {
            throw WorkerProtocolError("d3d11_compute.shader_empty", "installed shader bytecode is empty");
        }

        compute.shaderName = shaderFileName;
        compute.shaderBytes = static_cast<uint64_t>(shaderBytes.size());
        hr = compute.device->CreateComputeShader(shaderBytes.data(), shaderBytes.size(), nullptr, compute.shader.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_shader_failed", "CreateComputeShader failed: " + WideToUtf8(HResultToString(hr)));
        }
        return compute;
    }

    D3D11ComputeMeasurement WorkerRunD3D11ComputeDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D11ComputeMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(uint32_t);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d11_compute.too_large", "D3D11 compute buffer exceeds supported byte range");
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(uint32_t);

        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        HRESULT hr = compute.device->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_output_failed", "CreateBuffer output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(measurement.elements);

        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        hr = compute.device->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_uav_failed", "CreateUnorderedAccessView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = compute.device->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_staging_failed", "CreateBuffer staging failed: " + WideToUtf8(HResultToString(hr)));
        }

        std::vector<uint32_t> values(static_cast<size_t>(measurement.elements));
        auto runIteration = [&](bool captureValues) -> double
        {
            auto iterationStarted = std::chrono::steady_clock::now();
            ID3D11UnorderedAccessView* views[] = { uav.get() };
            UINT initialCounts[] = { 0 };
            compute.context->CSSetShader(compute.shader.get(), nullptr, 0);
            compute.context->CSSetUnorderedAccessViews(0, 1, views, initialCounts);
            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
            compute.context->CopyResource(stagingBuffer.get(), outputBuffer.get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto mapHr = compute.context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            auto elapsedMs = ElapsedMilliseconds(iterationStarted);
            if (FAILED(mapHr))
            {
                throw WorkerProtocolError("d3d11_compute.map_failed", "Map staging failed: " + WideToUtf8(HResultToString(mapHr)));
            }
            if (captureValues)
            {
                std::memcpy(values.data(), mapped.pData, static_cast<size_t>(measurement.bytes));
            }
            compute.context->Unmap(stagingBuffer.get(), 0);
            return elapsedMs;
        };

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            runIteration(false);
        }
        for (uint64_t i = 0; i < repeats; ++i)
        {
            measurement.totalDispatchAndReadbackMs += runIteration(i + 1 == repeats);
        }

        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        compute.context->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        compute.context->CSSetShader(nullptr, nullptr, 0);

        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto value = values[static_cast<size_t>(i)];
            if (value != ExpectedD3D11ComputeValue(static_cast<uint32_t>(i)))
            {
                ++measurement.mismatches;
            }
            measurement.checksum += value;
        }

        measurement.averageDispatchAndReadbackMs = repeats > 0
            ? measurement.totalDispatchAndReadbackMs / static_cast<double>(repeats)
            : 0.0;
        auto totalElements = measurement.elements * repeats;
        auto totalBytes = measurement.bytes * repeats;
        measurement.elementsPerSecond = measurement.totalDispatchAndReadbackMs > 0.0
            ? static_cast<double>(totalElements) / (measurement.totalDispatchAndReadbackMs / 1000.0)
            : 0.0;
        measurement.mibPerSecond = measurement.totalDispatchAndReadbackMs > 0.0
            ? ((static_cast<double>(totalBytes) / 1048576.0) / (measurement.totalDispatchAndReadbackMs / 1000.0))
            : 0.0;
        measurement.firstValue = values.front();
        measurement.lastValue = values.back();
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values.data()),
            values.size() * sizeof(uint32_t));
        measurement.verified = measurement.mismatches == 0 && !values.empty();
        return measurement;
    }

    D3D11ComputeTimingMeasurement WorkerRunD3D11ComputeTimingDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D11ComputeTimingMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(uint32_t);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d11_compute.too_large", "D3D11 compute buffer exceeds supported byte range");
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(uint32_t);

        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        HRESULT hr = compute.device->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_output_failed", "CreateBuffer output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(measurement.elements);

        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        hr = compute.device->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_uav_failed", "CreateUnorderedAccessView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = compute.device->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_compute.create_staging_failed", "CreateBuffer staging failed: " + WideToUtf8(HResultToString(hr)));
        }

        winrt::com_ptr<ID3D11Query> disjointQuery;
        winrt::com_ptr<ID3D11Query> startQuery;
        winrt::com_ptr<ID3D11Query> stopQuery;
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        auto disjointHr = compute.device->CreateQuery(&queryDesc, disjointQuery.put());
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        auto startHr = compute.device->CreateQuery(&queryDesc, startQuery.put());
        auto stopHr = compute.device->CreateQuery(&queryDesc, stopQuery.put());
        measurement.timestampQuerySupported = SUCCEEDED(disjointHr) && SUCCEEDED(startHr) && SUCCEEDED(stopHr);
        if (!measurement.timestampQuerySupported)
        {
            measurement.timingError = L"timestamp query creation failed: disjoint=" + HResultToString(disjointHr) +
                L" start=" + HResultToString(startHr) +
                L" stop=" + HResultToString(stopHr);
        }

        ID3D11UnorderedAccessView* views[] = { uav.get() };
        UINT initialCounts[] = { 0 };
        compute.context->CSSetShader(compute.shader.get(), nullptr, 0);
        compute.context->CSSetUnorderedAccessViews(0, 1, views, initialCounts);

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
        }

        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto submitStarted = std::chrono::steady_clock::now();
            if (measurement.timestampQuerySupported)
            {
                compute.context->Begin(disjointQuery.get());
                compute.context->End(startQuery.get());
            }

            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);

            if (measurement.timestampQuerySupported)
            {
                compute.context->End(stopQuery.get());
                compute.context->End(disjointQuery.get());
            }
            auto cpuSubmitMs = ElapsedMilliseconds(submitStarted);
            measurement.totalCpuSubmitMs += cpuSubmitMs;
            measurement.cpuSubmitSamplesMs.push_back(cpuSubmitMs);
            ++measurement.cpuSubmitSampleCount;

            if (measurement.timestampQuerySupported)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                UINT64 startTicks = 0;
                UINT64 stopTicks = 0;
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        disjointQuery.get(),
                        &disjointData,
                        sizeof(disjointData),
                        L"timestamp_disjoint"))
                {
                    measurement.timingError = L"timestamp disjoint query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        startQuery.get(),
                        &startTicks,
                        sizeof(startTicks),
                        L"timestamp_start"))
                {
                    measurement.timingError = L"timestamp start query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        stopQuery.get(),
                        &stopTicks,
                        sizeof(stopTicks),
                        L"timestamp_stop"))
                {
                    measurement.timingError = L"timestamp stop query timed out";
                    break;
                }

                measurement.gpuTimestampFrequency = disjointData.Frequency;
                if (disjointData.Disjoint || disjointData.Frequency == 0 || stopTicks < startTicks)
                {
                    ++measurement.gpuDisjointCount;
                }
                else
                {
                    auto gpuDispatchMs =
                        (static_cast<double>(stopTicks - startTicks) / static_cast<double>(disjointData.Frequency)) * 1000.0;
                    measurement.totalGpuDispatchMs += gpuDispatchMs;
                    measurement.gpuDispatchSamplesMs.push_back(gpuDispatchMs);
                    ++measurement.gpuTimingSampleCount;
                }
            }
        }

        measurement.gpuTimingAvailable = measurement.gpuTimingSampleCount > 0;
        measurement.averageGpuDispatchMs = measurement.gpuTimingAvailable
            ? measurement.totalGpuDispatchMs / static_cast<double>(measurement.gpuTimingSampleCount)
            : 0.0;
        measurement.averageCpuSubmitMs = measurement.cpuSubmitSampleCount > 0
            ? measurement.totalCpuSubmitMs / static_cast<double>(measurement.cpuSubmitSampleCount)
            : 0.0;
        auto timedElements = measurement.elements * measurement.gpuTimingSampleCount;
        auto timedBytes = measurement.bytes * measurement.gpuTimingSampleCount;
        measurement.gpuElementsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(timedElements) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(timedBytes) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;

        std::vector<uint32_t> values(static_cast<size_t>(measurement.elements));
        auto readbackStarted = std::chrono::steady_clock::now();
        compute.context->CopyResource(stagingBuffer.get(), outputBuffer.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto mapHr = compute.context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        measurement.verificationReadbackMs = ElapsedMilliseconds(readbackStarted);
        if (FAILED(mapHr))
        {
            throw WorkerProtocolError("d3d11_compute.map_failed", "Map staging failed: " + WideToUtf8(HResultToString(mapHr)));
        }
        std::memcpy(values.data(), mapped.pData, static_cast<size_t>(measurement.bytes));
        compute.context->Unmap(stagingBuffer.get(), 0);

        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        compute.context->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        compute.context->CSSetShader(nullptr, nullptr, 0);

        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto value = values[static_cast<size_t>(i)];
            if (value != ExpectedD3D11ComputeValue(static_cast<uint32_t>(i)))
            {
                ++measurement.mismatches;
            }
            measurement.checksum += value;
        }

        measurement.firstValue = values.front();
        measurement.lastValue = values.back();
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values.data()),
            values.size() * sizeof(uint32_t));
        measurement.verified = measurement.mismatches == 0 && !values.empty();
        return measurement;
    }

    D3D11FloatTimingMeasurement WorkerRunD3D11FloatTimingDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D11FloatTimingMeasurement measurement;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.bytes = measurement.elements * sizeof(D3D11Float4);
        if (measurement.bytes > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d11_fp32.too_large", "D3D11 FP32 compute buffer exceeds supported byte range");
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(D3D11Float4);

        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        HRESULT hr = compute.device->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_fp32.create_output_failed", "CreateBuffer output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(measurement.elements);

        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        hr = compute.device->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_fp32.create_uav_failed", "CreateUnorderedAccessView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(measurement.bytes);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = compute.device->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_fp32.create_staging_failed", "CreateBuffer staging failed: " + WideToUtf8(HResultToString(hr)));
        }

        winrt::com_ptr<ID3D11Query> disjointQuery;
        winrt::com_ptr<ID3D11Query> startQuery;
        winrt::com_ptr<ID3D11Query> stopQuery;
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        auto disjointHr = compute.device->CreateQuery(&queryDesc, disjointQuery.put());
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        auto startHr = compute.device->CreateQuery(&queryDesc, startQuery.put());
        auto stopHr = compute.device->CreateQuery(&queryDesc, stopQuery.put());
        measurement.timestampQuerySupported = SUCCEEDED(disjointHr) && SUCCEEDED(startHr) && SUCCEEDED(stopHr);
        if (!measurement.timestampQuerySupported)
        {
            measurement.timingError = L"timestamp query creation failed: disjoint=" + HResultToString(disjointHr) +
                L" start=" + HResultToString(startHr) +
                L" stop=" + HResultToString(stopHr);
        }

        ID3D11UnorderedAccessView* views[] = { uav.get() };
        UINT initialCounts[] = { 0 };
        compute.context->CSSetShader(compute.shader.get(), nullptr, 0);
        compute.context->CSSetUnorderedAccessViews(0, 1, views, initialCounts);

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
        }

        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto submitStarted = std::chrono::steady_clock::now();
            if (measurement.timestampQuerySupported)
            {
                compute.context->Begin(disjointQuery.get());
                compute.context->End(startQuery.get());
            }

            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);

            if (measurement.timestampQuerySupported)
            {
                compute.context->End(stopQuery.get());
                compute.context->End(disjointQuery.get());
            }
            auto cpuSubmitMs = ElapsedMilliseconds(submitStarted);
            measurement.totalCpuSubmitMs += cpuSubmitMs;
            measurement.cpuSubmitSamplesMs.push_back(cpuSubmitMs);
            ++measurement.cpuSubmitSampleCount;

            if (measurement.timestampQuerySupported)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                UINT64 startTicks = 0;
                UINT64 stopTicks = 0;
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        disjointQuery.get(),
                        &disjointData,
                        sizeof(disjointData),
                        L"fp32_timestamp_disjoint"))
                {
                    measurement.timingError = L"timestamp disjoint query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        startQuery.get(),
                        &startTicks,
                        sizeof(startTicks),
                        L"fp32_timestamp_start"))
                {
                    measurement.timingError = L"timestamp start query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        stopQuery.get(),
                        &stopTicks,
                        sizeof(stopTicks),
                        L"fp32_timestamp_stop"))
                {
                    measurement.timingError = L"timestamp stop query timed out";
                    break;
                }

                measurement.gpuTimestampFrequency = disjointData.Frequency;
                if (disjointData.Disjoint || disjointData.Frequency == 0 || stopTicks < startTicks)
                {
                    ++measurement.gpuDisjointCount;
                }
                else
                {
                    auto gpuDispatchMs =
                        (static_cast<double>(stopTicks - startTicks) / static_cast<double>(disjointData.Frequency)) * 1000.0;
                    measurement.totalGpuDispatchMs += gpuDispatchMs;
                    measurement.gpuDispatchSamplesMs.push_back(gpuDispatchMs);
                    ++measurement.gpuTimingSampleCount;
                }
            }
        }

        measurement.gpuTimingAvailable = measurement.gpuTimingSampleCount > 0;
        measurement.averageGpuDispatchMs = measurement.gpuTimingAvailable
            ? measurement.totalGpuDispatchMs / static_cast<double>(measurement.gpuTimingSampleCount)
            : 0.0;
        measurement.averageCpuSubmitMs = measurement.cpuSubmitSampleCount > 0
            ? measurement.totalCpuSubmitMs / static_cast<double>(measurement.cpuSubmitSampleCount)
            : 0.0;
        measurement.fp32OpsTimed = measurement.elements * measurement.gpuTimingSampleCount * measurement.fp32OpsPerElement;
        auto timedBytes = measurement.bytes * measurement.gpuTimingSampleCount;
        measurement.fp32OpsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(measurement.fp32OpsTimed) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gflops = measurement.fp32OpsPerSecond / 1000000000.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(timedBytes) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;

        std::vector<D3D11Float4> values(static_cast<size_t>(measurement.elements));
        auto readbackStarted = std::chrono::steady_clock::now();
        compute.context->CopyResource(stagingBuffer.get(), outputBuffer.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto mapHr = compute.context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        measurement.verificationReadbackMs = ElapsedMilliseconds(readbackStarted);
        if (FAILED(mapHr))
        {
            throw WorkerProtocolError("d3d11_fp32.map_failed", "Map staging failed: " + WideToUtf8(HResultToString(mapHr)));
        }
        std::memcpy(values.data(), mapped.pData, static_cast<size_t>(measurement.bytes));
        compute.context->Unmap(stagingBuffer.get(), 0);

        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        compute.context->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        compute.context->CSSetShader(nullptr, nullptr, 0);

        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto const& actual = values[static_cast<size_t>(i)];
            auto expected = ExpectedD3D11FloatComputeValue(static_cast<uint32_t>(i));
            bool elementMatches = true;
            double absError = 0.0;
            double relativeError = 0.0;

            if (!FloatComponentMatches(actual.x, expected.x, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.y, expected.y, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.z, expected.z, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.w, expected.w, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!elementMatches)
            {
                ++measurement.mismatches;
            }
            measurement.checksum += static_cast<double>(actual.x) +
                static_cast<double>(actual.y) +
                static_cast<double>(actual.z) +
                static_cast<double>(actual.w);
        }

        measurement.firstValue = values.front();
        measurement.lastValue = values.back();
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values.data()),
            values.size() * sizeof(D3D11Float4));
        measurement.verified = measurement.mismatches == 0 && !values.empty();
        return measurement;
    }

    D3D11ShaderMatrixMeasurement WorkerRunD3D11ShaderMatrixTimingDispatch(
        D3D11ComputeContext const& compute,
        D3D11ShaderMatrixVariant const& variant,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats)
    {
        D3D11ShaderMatrixMeasurement measurement;
        measurement.variant = variant;
        measurement.requestedElements = requestedElements;
        measurement.repeats = repeats;
        measurement.warmupRepeats = warmupRepeats;
        measurement.shaderBytes = compute.shaderBytes;
        measurement.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        measurement.dispatchGroups = measurement.elements / D3D11ComputeThreadsPerGroup;
        measurement.inputBytes = measurement.elements * sizeof(D3D11Float4);
        measurement.outputBytes = measurement.elements * sizeof(D3D11Float4);
        measurement.gpuBytesPerDispatch = measurement.elements * measurement.variant.gpuBytesPerElement;
        if (measurement.inputBytes > UINT32_MAX || measurement.outputBytes > UINT32_MAX || measurement.gpuBytesPerDispatch > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d11_shader_matrix.too_large", "D3D11 shader matrix buffer exceeds supported byte range");
        }

        std::vector<D3D11Float4> inputValues(static_cast<size_t>(measurement.elements));
        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            inputValues[static_cast<size_t>(i)] = D3D11MatrixInputValue(static_cast<uint32_t>(i));
        }

        D3D11_BUFFER_DESC inputDesc{};
        inputDesc.ByteWidth = static_cast<UINT>(measurement.inputBytes);
        inputDesc.Usage = D3D11_USAGE_DEFAULT;
        inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        inputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        inputDesc.StructureByteStride = sizeof(D3D11Float4);

        D3D11_SUBRESOURCE_DATA inputData{};
        inputData.pSysMem = inputValues.data();
        inputData.SysMemPitch = static_cast<UINT>(measurement.inputBytes);

        winrt::com_ptr<ID3D11Buffer> inputBuffer;
        HRESULT hr = compute.device->CreateBuffer(&inputDesc, &inputData, inputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.create_input_failed", "CreateBuffer input failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(measurement.elements);

        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        hr = compute.device->CreateShaderResourceView(inputBuffer.get(), &srvDesc, srv.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.create_srv_failed", "CreateShaderResourceView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = static_cast<UINT>(measurement.outputBytes);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(D3D11Float4);

        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        hr = compute.device->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.create_output_failed", "CreateBuffer output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(measurement.elements);

        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        hr = compute.device->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.create_uav_failed", "CreateUnorderedAccessView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(measurement.outputBytes);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = compute.device->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.create_staging_failed", "CreateBuffer staging failed: " + WideToUtf8(HResultToString(hr)));
        }

        winrt::com_ptr<ID3D11Query> disjointQuery;
        winrt::com_ptr<ID3D11Query> startQuery;
        winrt::com_ptr<ID3D11Query> stopQuery;
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        auto disjointHr = compute.device->CreateQuery(&queryDesc, disjointQuery.put());
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        auto startHr = compute.device->CreateQuery(&queryDesc, startQuery.put());
        auto stopHr = compute.device->CreateQuery(&queryDesc, stopQuery.put());
        measurement.timestampQuerySupported = SUCCEEDED(disjointHr) && SUCCEEDED(startHr) && SUCCEEDED(stopHr);
        if (!measurement.timestampQuerySupported)
        {
            measurement.timingError = L"timestamp query creation failed: disjoint=" + HResultToString(disjointHr) +
                L" start=" + HResultToString(startHr) +
                L" stop=" + HResultToString(stopHr);
        }

        ID3D11ShaderResourceView* srvs[] = { srv.get() };
        ID3D11UnorderedAccessView* views[] = { uav.get() };
        UINT initialCounts[] = { 0 };
        compute.context->CSSetShader(compute.shader.get(), nullptr, 0);
        compute.context->CSSetShaderResources(0, 1, srvs);
        compute.context->CSSetUnorderedAccessViews(0, 1, views, initialCounts);

        for (uint64_t i = 0; i < warmupRepeats; ++i)
        {
            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);
        }

        for (uint64_t i = 0; i < repeats; ++i)
        {
            auto submitStarted = std::chrono::steady_clock::now();
            if (measurement.timestampQuerySupported)
            {
                compute.context->Begin(disjointQuery.get());
                compute.context->End(startQuery.get());
            }

            compute.context->Dispatch(static_cast<UINT>(measurement.dispatchGroups), 1, 1);

            if (measurement.timestampQuerySupported)
            {
                compute.context->End(stopQuery.get());
                compute.context->End(disjointQuery.get());
            }
            auto cpuSubmitMs = ElapsedMilliseconds(submitStarted);
            measurement.totalCpuSubmitMs += cpuSubmitMs;
            measurement.cpuSubmitSamplesMs.push_back(cpuSubmitMs);
            ++measurement.cpuSubmitSampleCount;

            if (measurement.timestampQuerySupported)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                UINT64 startTicks = 0;
                UINT64 stopTicks = 0;
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        disjointQuery.get(),
                        &disjointData,
                        sizeof(disjointData),
                        L"shader_matrix_timestamp_disjoint"))
                {
                    measurement.timingError = L"timestamp disjoint query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        startQuery.get(),
                        &startTicks,
                        sizeof(startTicks),
                        L"shader_matrix_timestamp_start"))
                {
                    measurement.timingError = L"timestamp start query timed out";
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        stopQuery.get(),
                        &stopTicks,
                        sizeof(stopTicks),
                        L"shader_matrix_timestamp_stop"))
                {
                    measurement.timingError = L"timestamp stop query timed out";
                    break;
                }

                measurement.gpuTimestampFrequency = disjointData.Frequency;
                if (disjointData.Disjoint || disjointData.Frequency == 0 || stopTicks < startTicks)
                {
                    ++measurement.gpuDisjointCount;
                }
                else
                {
                    auto gpuDispatchMs =
                        (static_cast<double>(stopTicks - startTicks) / static_cast<double>(disjointData.Frequency)) * 1000.0;
                    measurement.totalGpuDispatchMs += gpuDispatchMs;
                    measurement.gpuDispatchSamplesMs.push_back(gpuDispatchMs);
                    ++measurement.gpuTimingSampleCount;
                }
            }
        }

        measurement.gpuTimingAvailable = measurement.gpuTimingSampleCount > 0;
        measurement.averageGpuDispatchMs = measurement.gpuTimingAvailable
            ? measurement.totalGpuDispatchMs / static_cast<double>(measurement.gpuTimingSampleCount)
            : 0.0;
        measurement.averageCpuSubmitMs = measurement.cpuSubmitSampleCount > 0
            ? measurement.totalCpuSubmitMs / static_cast<double>(measurement.cpuSubmitSampleCount)
            : 0.0;
        measurement.fp32OpsTimed = measurement.elements * measurement.gpuTimingSampleCount * measurement.variant.fp32OpsPerElement;
        measurement.gpuBytesTimed = measurement.gpuBytesPerDispatch * measurement.gpuTimingSampleCount;
        measurement.fp32OpsPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? static_cast<double>(measurement.fp32OpsTimed) / (measurement.totalGpuDispatchMs / 1000.0)
            : 0.0;
        measurement.gflops = measurement.fp32OpsPerSecond / 1000000000.0;
        measurement.gpuMiBPerSecond = measurement.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(measurement.gpuBytesTimed) / 1048576.0) / (measurement.totalGpuDispatchMs / 1000.0))
            : 0.0;

        std::vector<D3D11Float4> values(static_cast<size_t>(measurement.elements));
        auto readbackStarted = std::chrono::steady_clock::now();
        compute.context->CopyResource(stagingBuffer.get(), outputBuffer.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto mapHr = compute.context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        measurement.verificationReadbackMs = ElapsedMilliseconds(readbackStarted);
        if (FAILED(mapHr))
        {
            throw WorkerProtocolError("d3d11_shader_matrix.map_failed", "Map staging failed: " + WideToUtf8(HResultToString(mapHr)));
        }
        std::memcpy(values.data(), mapped.pData, static_cast<size_t>(measurement.outputBytes));
        compute.context->Unmap(stagingBuffer.get(), 0);

        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
        compute.context->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        compute.context->CSSetShaderResources(0, 1, nullSrvs);
        compute.context->CSSetShader(nullptr, nullptr, 0);

        for (uint64_t i = 0; i < measurement.elements; ++i)
        {
            auto const& actual = values[static_cast<size_t>(i)];
            auto expected = ExpectedD3D11ShaderMatrixValue(static_cast<uint32_t>(i), variant);
            bool elementMatches = true;
            double absError = 0.0;
            double relativeError = 0.0;

            if (!FloatComponentMatches(actual.x, expected.x, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.y, expected.y, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.z, expected.z, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!FloatComponentMatches(actual.w, expected.w, absError, relativeError))
            {
                elementMatches = false;
            }
            if (absError > measurement.maxAbsError) { measurement.maxAbsError = absError; }
            if (relativeError > measurement.maxRelativeError) { measurement.maxRelativeError = relativeError; }

            if (!elementMatches)
            {
                ++measurement.mismatches;
            }
            measurement.checksum += static_cast<double>(actual.x) +
                static_cast<double>(actual.y) +
                static_cast<double>(actual.z) +
                static_cast<double>(actual.w);
        }

        measurement.firstValue = values.front();
        measurement.lastValue = values.back();
        measurement.hash32 = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(values.data()),
            values.size() * sizeof(D3D11Float4));
        measurement.verified = measurement.mismatches == 0 && !values.empty();
        return measurement;
    }

    static D3D11ResidentHotLoopResources CreateD3D11ResidentHotLoopResources(
        D3D11ComputeContext const& compute,
        D3D11ShaderMatrixVariant const& variant,
        uint64_t requestedElements)
    {
        D3D11ResidentHotLoopResources resources;
        resources.variant = variant;
        resources.requestedElements = requestedElements;
        resources.elements = ((requestedElements + D3D11ComputeThreadsPerGroup - 1) / D3D11ComputeThreadsPerGroup) * D3D11ComputeThreadsPerGroup;
        resources.dispatchGroups = resources.elements / D3D11ComputeThreadsPerGroup;
        resources.inputBytes = resources.elements * sizeof(D3D11Float4);
        resources.outputBytes = resources.elements * sizeof(D3D11Float4);
        resources.gpuBytesPerDispatch = resources.elements * resources.variant.gpuBytesPerElement;
        if (resources.inputBytes > UINT32_MAX || resources.outputBytes > UINT32_MAX || resources.gpuBytesPerDispatch > UINT32_MAX)
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.too_large", "D3D11 resident hot-loop buffer exceeds supported byte range");
        }

        std::vector<D3D11Float4> inputValues(static_cast<size_t>(resources.elements));
        for (uint64_t i = 0; i < resources.elements; ++i)
        {
            inputValues[static_cast<size_t>(i)] = D3D11MatrixInputValue(static_cast<uint32_t>(i));
        }

        D3D11_BUFFER_DESC inputDesc{};
        inputDesc.ByteWidth = static_cast<UINT>(resources.inputBytes);
        inputDesc.Usage = D3D11_USAGE_DEFAULT;
        inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        inputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        inputDesc.StructureByteStride = sizeof(D3D11Float4);

        D3D11_SUBRESOURCE_DATA inputData{};
        inputData.pSysMem = inputValues.data();
        inputData.SysMemPitch = static_cast<UINT>(resources.inputBytes);

        HRESULT hr = compute.device->CreateBuffer(&inputDesc, &inputData, resources.inputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.create_input_failed", "CreateBuffer input failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(resources.elements);

        hr = compute.device->CreateShaderResourceView(resources.inputBuffer.get(), &srvDesc, resources.srv.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.create_srv_failed", "CreateShaderResourceView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = static_cast<UINT>(resources.outputBytes);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(D3D11Float4);

        hr = compute.device->CreateBuffer(&outputDesc, nullptr, resources.outputBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.create_output_failed", "CreateBuffer output failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(resources.elements);

        hr = compute.device->CreateUnorderedAccessView(resources.outputBuffer.get(), &uavDesc, resources.uav.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.create_uav_failed", "CreateUnorderedAccessView failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(resources.outputBytes);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        hr = compute.device->CreateBuffer(&stagingDesc, nullptr, resources.stagingBuffer.put());
        if (FAILED(hr))
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.create_staging_failed", "CreateBuffer staging failed: " + WideToUtf8(HResultToString(hr)));
        }

        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        auto disjointHr = compute.device->CreateQuery(&queryDesc, resources.disjointQuery.put());
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        auto startHr = compute.device->CreateQuery(&queryDesc, resources.startQuery.put());
        auto stopHr = compute.device->CreateQuery(&queryDesc, resources.stopQuery.put());
        resources.timestampQuerySupported = SUCCEEDED(disjointHr) && SUCCEEDED(startHr) && SUCCEEDED(stopHr);
        if (!resources.timestampQuerySupported)
        {
            resources.timingError = L"timestamp query creation failed: disjoint=" + HResultToString(disjointHr) +
                L" start=" + HResultToString(startHr) +
                L" stop=" + HResultToString(stopHr);
        }

        return resources;
    }

    D3D12ShaderShapeMatrixResult WorkerRunD3D12ShaderShapeMatrix(
        D3D12ShaderShapeMatrixOptions const& options)
    {
        D3D12ShaderShapeMatrixResult result;
        auto started = std::chrono::steady_clock::now();
        result.summaries.reserve(options.variants.size());

        for (auto const& variant : options.variants)
        {
            auto context = WorkerCreateD3D12ComputeContext(
                variant.shaderFileName,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                true);
            if (result.selectedAdapterIndex == UINT32_MAX)
            {
                result.selectedAdapterIndex = context.selectedAdapterIndex;
                result.selectedAdapter = context.selectedAdapter;
            }
            result.deviceCreated = result.deviceCreated && context.deviceCreated;
            result.rootSignatureCreated =
                result.rootSignatureCreated && context.rootSignatureCreated;
            result.pipelineStateCreated =
                result.pipelineStateCreated && context.pipelineStateCreated;
            result.commandQueueCreated =
                result.commandQueueCreated && context.commandQueueCreated;

            D3D11ShaderMatrixVariantSummary summary;
            summary.variant = variant;
            summary.shaderBytes = context.shaderBytes;
            summary.featureLevel = D3D_FEATURE_LEVEL_11_0;
            summary.measurements.reserve(options.elementsList.size());

            for (auto requestedElements : options.elementsList)
            {
                auto measurement = WorkerRunD3D12ShaderShapeTimingDispatch(
                    context,
                    variant,
                    requestedElements,
                    options.repeats,
                    options.warmupRepeats);
                summary.totalElementsTimed +=
                    measurement.elements * measurement.gpuTimingSampleCount;
                summary.totalGpuBytesTimed += measurement.gpuBytesTimed;
                summary.totalFp32OpsTimed += measurement.fp32OpsTimed;
                summary.totalGpuTimingSampleCount +=
                    measurement.gpuTimingSampleCount;
                summary.totalCpuSubmitSampleCount +=
                    measurement.cpuSubmitSampleCount;
                summary.totalGpuDispatchMs += measurement.totalGpuDispatchMs;
                summary.totalCpuSubmitMs += measurement.totalCpuSubmitMs;
                summary.totalVerificationReadbackMs +=
                    measurement.verificationReadbackMs;
                if (measurement.mismatches > summary.maxMismatchCount)
                {
                    summary.maxMismatchCount = measurement.mismatches;
                }
                if (measurement.maxAbsError > summary.maxAbsError)
                {
                    summary.maxAbsError = measurement.maxAbsError;
                }
                if (measurement.maxRelativeError > summary.maxRelativeError)
                {
                    summary.maxRelativeError = measurement.maxRelativeError;
                }
                summary.aggregateHash32 ^= measurement.hash32;
                summary.aggregateHash32 *= 16777619u;
                summary.timestampQuerySupported =
                    summary.timestampQuerySupported &&
                    measurement.timestampQuerySupported;
                summary.gpuTimingAvailable =
                    summary.gpuTimingAvailable && measurement.gpuTimingAvailable;
                summary.verified = summary.verified && measurement.verified;

                result.totalElementsTimed +=
                    measurement.elements * measurement.gpuTimingSampleCount;
                result.totalGpuBytesTimed += measurement.gpuBytesTimed;
                result.totalFp32OpsTimed += measurement.fp32OpsTimed;
                result.totalGpuTimingSampleCount +=
                    measurement.gpuTimingSampleCount;
                result.totalCpuSubmitSampleCount +=
                    measurement.cpuSubmitSampleCount;
                result.totalGpuDispatchMs += measurement.totalGpuDispatchMs;
                result.totalCpuSubmitMs += measurement.totalCpuSubmitMs;
                result.totalVerificationReadbackMs +=
                    measurement.verificationReadbackMs;
                if (measurement.mismatches > result.maxMismatchCount)
                {
                    result.maxMismatchCount = measurement.mismatches;
                }
                if (measurement.maxAbsError > result.maxAbsError)
                {
                    result.maxAbsError = measurement.maxAbsError;
                }
                if (measurement.maxRelativeError > result.maxRelativeError)
                {
                    result.maxRelativeError = measurement.maxRelativeError;
                }
                result.aggregateHash32 ^= measurement.hash32;
                result.aggregateHash32 *= 16777619u;
                result.timestampQuerySupported =
                    result.timestampQuerySupported &&
                    measurement.timestampQuerySupported;
                result.gpuTimingAvailable =
                    result.gpuTimingAvailable && measurement.gpuTimingAvailable;
                result.verified = result.verified && measurement.verified;
                ++result.resultCount;
                summary.measurements.push_back(std::move(measurement));
            }

            result.summaries.push_back(std::move(summary));
        }

        result.elapsedMs = ElapsedMilliseconds(started);
        result.aggregateFp32OpsPerSecond = result.totalGpuDispatchMs > 0.0
            ? static_cast<double>(result.totalFp32OpsTimed) /
                (result.totalGpuDispatchMs / 1000.0)
            : 0.0;
        result.aggregateGflops =
            result.aggregateFp32OpsPerSecond / 1000000000.0;
        result.aggregateGpuMiBPerSecond = result.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(result.totalGpuBytesTimed) / 1048576.0) /
                (result.totalGpuDispatchMs / 1000.0))
            : 0.0;
        result.averageCpuSubmitMs = result.totalCpuSubmitSampleCount > 0
            ? result.totalCpuSubmitMs /
                static_cast<double>(result.totalCpuSubmitSampleCount)
            : 0.0;

        for (auto const& summary : result.summaries)
        {
            auto summaryGflops = summary.totalGpuDispatchMs > 0.0
                ? (static_cast<double>(summary.totalFp32OpsTimed) /
                    (summary.totalGpuDispatchMs / 1000.0)) /
                    1000000000.0
                : 0.0;
            auto summaryMiBPerSecond = summary.totalGpuDispatchMs > 0.0
                ? ((static_cast<double>(summary.totalGpuBytesTimed) /
                    1048576.0) /
                    (summary.totalGpuDispatchMs / 1000.0))
                : 0.0;
            if (summary.variant.fp32OpsPerElement > 0 &&
                summaryGflops > result.bestFp32VariantGflops)
            {
                result.bestFp32VariantId = summary.variant.id;
                result.bestFp32VariantGflops = summaryGflops;
            }
            if (summaryMiBPerSecond > result.bestBandwidthMiBPerSecond)
            {
                result.bestBandwidthVariantId = summary.variant.id;
                result.bestBandwidthMiBPerSecond = summaryMiBPerSecond;
            }
        }

        return result;
    }

    D3D12ShaderShapeSoakResult WorkerRunD3D12ShaderShapeSoak(
        D3D12ShaderShapeSoakOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment,
        std::atomic<bool> const* cancelRequested)
    {
        D3D12ShaderShapeSoakResult result;
        uint64_t minAppMemoryLimitBytes = UINT64_MAX;
        double sumBestFp32Gflops = 0.0;
        double sumAggregateGflops = 0.0;
        double sumGpuDispatchMs = 0.0;
        result.windows.reserve(static_cast<size_t>(options.windowCount));

        for (uint64_t windowIndex = 1;
             windowIndex <= options.windowCount;
             ++windowIndex)
        {
            if (cancelRequested != nullptr && cancelRequested->load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            D3D12ShaderShapeSoakWindow window;
            window.index = windowIndex;
            window.measured =
                windowIndex > options.discardInitialWindows;
            window.result =
                WorkerRunD3D12ShaderShapeMatrix(options.matrix);
            window.environment = ObserveD3DEnvironment(observeEnvironment);

            result.verified = result.verified && window.result.verified;
            if (window.result.maxMismatchCount > result.maxMismatchCount)
            {
                result.maxMismatchCount =
                    window.result.maxMismatchCount;
            }
            if (window.result.maxRelativeError > result.maxRelativeError)
            {
                result.maxRelativeError =
                    window.result.maxRelativeError;
            }
            if (window.environment.appMemoryUsageLimitBytes > 0 &&
                window.environment.appMemoryUsageLimitBytes <
                    minAppMemoryLimitBytes)
            {
                minAppMemoryLimitBytes =
                    window.environment.appMemoryUsageLimitBytes;
            }

            if (window.measured)
            {
                ++result.measuredWindowCount;
                result.totalFp32OpsTimed +=
                    window.result.totalFp32OpsTimed;
                sumBestFp32Gflops +=
                    window.result.bestFp32VariantGflops;
                sumAggregateGflops +=
                    window.result.aggregateGflops;
                sumGpuDispatchMs +=
                    window.result.totalGpuDispatchMs;
                if (result.measuredWindowCount == 1 ||
                    window.result.bestFp32VariantGflops <
                        result.minBestFp32Gflops)
                {
                    result.minBestFp32Gflops =
                        window.result.bestFp32VariantGflops;
                }
                if (result.measuredWindowCount == 1 ||
                    window.result.bestFp32VariantGflops >
                        result.maxBestFp32Gflops)
                {
                    result.maxBestFp32Gflops =
                        window.result.bestFp32VariantGflops;
                }
            }

            result.windows.push_back(std::move(window));
            if (windowIndex < options.windowCount &&
                options.windowPauseMs > 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options.windowPauseMs));
            }
        }

        if (result.windows.empty() || result.measuredWindowCount == 0)
        {
            throw WorkerProtocolError(
                "d3d12_shader_shape_soak.no_measured_windows",
                "D3D12 shader-shape soak produced no measured windows");
        }
        result.minAppMemoryLimitBytes =
            minAppMemoryLimitBytes == UINT64_MAX
            ? 0
            : minAppMemoryLimitBytes;
        result.averageBestFp32Gflops =
            sumBestFp32Gflops /
            static_cast<double>(result.measuredWindowCount);
        result.averageAggregateGflops =
            sumAggregateGflops /
            static_cast<double>(result.measuredWindowCount);
        result.averageTotalGpuDispatchMs =
            sumGpuDispatchMs /
            static_cast<double>(result.measuredWindowCount);
        result.spreadBestFp32Percent =
            result.averageBestFp32Gflops > 0.0
            ? ((result.maxBestFp32Gflops -
                result.minBestFp32Gflops) /
                result.averageBestFp32Gflops) * 100.0
            : 0.0;

        if (cancelRequested != nullptr && cancelRequested->load())
        {
            throw WorkerProtocolError("job.canceled", "job was canceled");
        }
        return result;
    }

    D3D11SustainedSoakResult WorkerRunD3D11SustainedSoak(
        D3D11SustainedSoakOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment)
    {
        auto const& variant = options.variant;
        auto requestedElements = options.requestedElements;
        auto durationSeconds = options.durationSeconds;
        auto windowSeconds = options.windowSeconds;
        auto repeatsPerBatch = options.repeatsPerBatch;
        auto warmupRepeats = options.warmupRepeats;
        auto maxBatches = options.maxBatches;
        auto observe = [&]()
        {
            return ObserveD3DEnvironment(observeEnvironment);
        };
        auto appMemoryUsageBytes = [&]()
        {
            return observe().appMemoryUsageBytes;
        };
        auto appMemoryUsageLimitBytes = [&]()
        {
            return observe().appMemoryUsageLimitBytes;
        };
        auto appMemoryUsageLevelValue = [&]()
        {
            return observe().appMemoryUsageLevel;
        };

        auto compute = WorkerCreateD3D11ComputeContext(variant.shaderFileName);
        auto started = std::chrono::steady_clock::now();
        auto elapsedSinceStartMs = [&started]()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };

        double const requestedDurationMs = static_cast<double>(durationSeconds) * 1000.0;
        double const requestedWindowMs = static_cast<double>(windowSeconds) * 1000.0;
        auto memoryAtStart = appMemoryUsageBytes();
        auto memoryLimit = appMemoryUsageLimitBytes();
        uint64_t peakAppMemoryUsageBytes = memoryAtStart;

        std::vector<D3D11SustainedSoakWindow> windows;
        std::vector<double> allGpuDispatchSamplesMs;
        std::vector<double> allCpuSubmitSamplesMs;
        std::vector<double> allGflopsSamples;
        std::vector<double> allGpuMiBPerSecondSamples;

        uint64_t totalBatchCount = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        bool stoppedEarly = false;
        bool deviceRemoved = false;
        std::wstring stopReason;
        std::wstring firstTimingError;
        std::wstring deviceRemovedReason;

        auto beginWindow = [&](uint64_t index, double startMs)
        {
            D3D11SustainedSoakWindow window;
            window.index = index;
            window.startedMs = startMs;
            window.appMemoryUsageStartBytes = appMemoryUsageBytes();
            window.appMemoryUsageLimitBytes = memoryLimit;
            window.appMemoryUsageLevel = appMemoryUsageLevelValue();
            if (window.appMemoryUsageStartBytes > peakAppMemoryUsageBytes)
            {
                peakAppMemoryUsageBytes = window.appMemoryUsageStartBytes;
            }
            return window;
        };

        auto finalizeWindow = [&](D3D11SustainedSoakWindow& window, double endMs)
        {
            window.endedMs = endMs;
            window.durationMs = endMs - window.startedMs;
            window.appMemoryUsageEndBytes = appMemoryUsageBytes();
            window.appMemoryUsageLevel = appMemoryUsageLevelValue();
            if (window.appMemoryUsageEndBytes > peakAppMemoryUsageBytes)
            {
                peakAppMemoryUsageBytes = window.appMemoryUsageEndBytes;
            }
            window.gpuDispatchStats = ComputeD3DDoubleSampleStats(window.gpuDispatchSamplesMs);
            window.cpuSubmitStats = ComputeD3DDoubleSampleStats(window.cpuSubmitSamplesMs);
            window.gflopsStats = ComputeD3DDoubleSampleStats(window.gflopsSamples);
            window.gpuMiBPerSecondStats = ComputeD3DDoubleSampleStats(window.gpuMiBPerSecondSamples);
            std::vector<double>().swap(window.gpuDispatchSamplesMs);
            std::vector<double>().swap(window.cpuSubmitSamplesMs);
            std::vector<double>().swap(window.gflopsSamples);
            std::vector<double>().swap(window.gpuMiBPerSecondSamples);
            windows.push_back(window);
        };

        auto currentWindow = beginWindow(0, 0.0);

        while (elapsedSinceStartMs() < requestedDurationMs)
        {
            if (totalBatchCount >= maxBatches)
            {
                stoppedEarly = true;
                stopReason = L"max_batches_reached";
                break;
            }

            D3D11ShaderMatrixMeasurement measurement;
            try
            {
                auto batchWarmupRepeats = totalBatchCount == 0 ? warmupRepeats : 0;
                measurement = WorkerRunD3D11ShaderMatrixTimingDispatch(
                    compute,
                    variant,
                    requestedElements,
                    repeatsPerBatch,
                    batchWarmupRepeats);
            }
            catch (WorkerProtocolError const& error)
            {
                stoppedEarly = true;
                stopReason = L"command_error";
                firstTimingError = Utf8ToWide(error.code + ": " + error.message);
                currentWindow.firstTimingError = firstTimingError;
                currentWindow.verified = false;
                verified = false;
                break;
            }
            catch (std::exception const& error)
            {
                stoppedEarly = true;
                stopReason = L"exception";
                firstTimingError = Utf8ToWide(error.what());
                currentWindow.firstTimingError = firstTimingError;
                currentWindow.verified = false;
                verified = false;
                break;
            }

            ++totalBatchCount;
            ++currentWindow.batchCount;
            currentWindow.gpuTimingSampleCount += measurement.gpuTimingSampleCount;
            currentWindow.gpuDisjointCount += measurement.gpuDisjointCount;
            currentWindow.cpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
            currentWindow.elementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
            currentWindow.gpuBytesTimed += measurement.gpuBytesTimed;
            currentWindow.fp32OpsTimed += measurement.fp32OpsTimed;
            currentWindow.totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            currentWindow.totalCpuSubmitMs += measurement.totalCpuSubmitMs;
            currentWindow.totalVerificationReadbackMs += measurement.verificationReadbackMs;
            currentWindow.aggregateHash32 ^= measurement.hash32;
            currentWindow.aggregateHash32 *= 16777619u;
            currentWindow.timestampQuerySupported = currentWindow.timestampQuerySupported && measurement.timestampQuerySupported;
            currentWindow.gpuTimingAvailable = currentWindow.gpuTimingAvailable && measurement.gpuTimingAvailable;
            currentWindow.verified = currentWindow.verified && measurement.verified;
            if (measurement.mismatches > currentWindow.maxMismatchCount)
            {
                currentWindow.maxMismatchCount = measurement.mismatches;
            }
            if (measurement.maxAbsError > currentWindow.maxAbsError)
            {
                currentWindow.maxAbsError = measurement.maxAbsError;
            }
            if (measurement.maxRelativeError > currentWindow.maxRelativeError)
            {
                currentWindow.maxRelativeError = measurement.maxRelativeError;
            }
            if (currentWindow.firstTimingError.empty() && !measurement.timingError.empty())
            {
                currentWindow.firstTimingError = measurement.timingError;
            }

            totalGpuTimingSampleCount += measurement.gpuTimingSampleCount;
            totalGpuDisjointCount += measurement.gpuDisjointCount;
            totalCpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
            totalElementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
            totalGpuBytesTimed += measurement.gpuBytesTimed;
            totalFp32OpsTimed += measurement.fp32OpsTimed;
            totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            totalCpuSubmitMs += measurement.totalCpuSubmitMs;
            totalVerificationReadbackMs += measurement.verificationReadbackMs;
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
            gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
            verified = verified && measurement.verified;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            if (measurement.maxAbsError > maxAbsError)
            {
                maxAbsError = measurement.maxAbsError;
            }
            if (measurement.maxRelativeError > maxRelativeError)
            {
                maxRelativeError = measurement.maxRelativeError;
            }
            if (firstTimingError.empty() && !measurement.timingError.empty())
            {
                firstTimingError = measurement.timingError;
            }

            for (auto value : measurement.cpuSubmitSamplesMs)
            {
                currentWindow.cpuSubmitSamplesMs.push_back(value);
                allCpuSubmitSamplesMs.push_back(value);
            }
            for (auto value : measurement.gpuDispatchSamplesMs)
            {
                auto sampleGflops = (variant.fp32OpsPerElement > 0 && value > 0.0)
                    ? ((static_cast<double>(measurement.elements * variant.fp32OpsPerElement) / (value / 1000.0)) / 1000000000.0)
                    : 0.0;
                auto sampleMiBPerSecond = value > 0.0
                    ? ((static_cast<double>(measurement.gpuBytesPerDispatch) / 1048576.0) / (value / 1000.0))
                    : 0.0;
                currentWindow.gpuDispatchSamplesMs.push_back(value);
                currentWindow.gflopsSamples.push_back(sampleGflops);
                currentWindow.gpuMiBPerSecondSamples.push_back(sampleMiBPerSecond);
                allGpuDispatchSamplesMs.push_back(value);
                allGflopsSamples.push_back(sampleGflops);
                allGpuMiBPerSecondSamples.push_back(sampleMiBPerSecond);
            }

            auto removedHr = compute.device->GetDeviceRemovedReason();
            if (removedHr != S_OK)
            {
                stoppedEarly = true;
                stopReason = L"device_removed";
                deviceRemoved = true;
                deviceRemovedReason = HResultToString(removedHr);
                verified = false;
                currentWindow.verified = false;
                if (firstTimingError.empty())
                {
                    firstTimingError = L"D3D11 device removed: " + deviceRemovedReason;
                }
                if (currentWindow.firstTimingError.empty())
                {
                    currentWindow.firstTimingError = firstTimingError;
                }
                break;
            }

            auto nowMs = elapsedSinceStartMs();
            if (nowMs - currentWindow.startedMs >= requestedWindowMs)
            {
                finalizeWindow(currentWindow, nowMs);
                currentWindow = beginWindow(static_cast<uint64_t>(windows.size()), nowMs);
            }
        }

        auto elapsedMs = elapsedSinceStartMs();
        if (currentWindow.batchCount > 0 || windows.empty())
        {
            finalizeWindow(currentWindow, elapsedMs);
        }
        if (!stoppedEarly && elapsedMs + 1.0 < requestedDurationMs)
        {
            stoppedEarly = true;
            if (stopReason.empty())
            {
                stopReason = L"completed_before_requested_duration";
            }
        }

        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalGpuBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;
        auto aggregateGpuDispatchStats = ComputeD3DDoubleSampleStats(allGpuDispatchSamplesMs);
        auto aggregateCpuSubmitStats = ComputeD3DDoubleSampleStats(allCpuSubmitSamplesMs);
        auto aggregateGflopsStats = ComputeD3DDoubleSampleStats(allGflopsSamples);
        auto aggregateGpuMiBPerSecondStats = ComputeD3DDoubleSampleStats(allGpuMiBPerSecondSamples);

        double initialWindowMedianGflops = 0.0;
        double finalWindowMedianGflops = 0.0;
        double medianGflopsDriftPercent = 0.0;
        double minWindowMedianGflops = 0.0;
        double maxWindowMedianGflops = 0.0;
        if (!windows.empty())
        {
            initialWindowMedianGflops = windows.front().gflopsStats.median;
            finalWindowMedianGflops = windows.back().gflopsStats.median;
            medianGflopsDriftPercent = initialWindowMedianGflops != 0.0
                ? ((finalWindowMedianGflops - initialWindowMedianGflops) / initialWindowMedianGflops) * 100.0
                : 0.0;
            minWindowMedianGflops = windows.front().gflopsStats.median;
            maxWindowMedianGflops = windows.front().gflopsStats.median;
            for (auto const& window : windows)
            {
                if (window.gflopsStats.median < minWindowMedianGflops)
                {
                    minWindowMedianGflops = window.gflopsStats.median;
                }
                if (window.gflopsStats.median > maxWindowMedianGflops)
                {
                    maxWindowMedianGflops = window.gflopsStats.median;
                }
            }
        }


        D3D11SustainedSoakResult result;
        result.featureLevel = compute.featureLevel;
        result.windows = std::move(windows);
        result.totalBatchCount = totalBatchCount;
        result.totalGpuTimingSampleCount = totalGpuTimingSampleCount;
        result.totalGpuDisjointCount = totalGpuDisjointCount;
        result.totalCpuSubmitSampleCount = totalCpuSubmitSampleCount;
        result.totalElementsTimed = totalElementsTimed;
        result.totalGpuBytesTimed = totalGpuBytesTimed;
        result.totalFp32OpsTimed = totalFp32OpsTimed;
        result.totalGpuDispatchMs = totalGpuDispatchMs;
        result.totalCpuSubmitMs = totalCpuSubmitMs;
        result.totalVerificationReadbackMs = totalVerificationReadbackMs;
        result.elapsedMs = elapsedMs;
        result.aggregateFp32OpsPerSecond = aggregateFp32OpsPerSecond;
        result.aggregateGflops = aggregateGflops;
        result.aggregateGpuMiBPerSecond = aggregateGpuMiBPerSecond;
        result.averageCpuSubmitMs = averageCpuSubmitMs;
        result.aggregateGpuDispatchStats = aggregateGpuDispatchStats;
        result.aggregateCpuSubmitStats = aggregateCpuSubmitStats;
        result.aggregateGflopsStats = aggregateGflopsStats;
        result.aggregateGpuMiBPerSecondStats = aggregateGpuMiBPerSecondStats;
        result.initialWindowMedianGflops = initialWindowMedianGflops;
        result.finalWindowMedianGflops = finalWindowMedianGflops;
        result.medianGflopsDriftPercent = medianGflopsDriftPercent;
        result.minWindowMedianGflops = minWindowMedianGflops;
        result.maxWindowMedianGflops = maxWindowMedianGflops;
        result.memoryAtStart = memoryAtStart;
        result.peakAppMemoryUsageBytes = peakAppMemoryUsageBytes;
        result.memoryLimit = memoryLimit;
        result.environmentEnd = observe();
        result.maxMismatchCount = maxMismatchCount;
        result.maxAbsError = maxAbsError;
        result.maxRelativeError = maxRelativeError;
        result.aggregateHash32 = aggregateHash32;
        result.timestampQuerySupported = timestampQuerySupported;
        result.gpuTimingAvailable = gpuTimingAvailable;
        result.verified = verified && maxMismatchCount == 0 && !deviceRemoved;
        result.stoppedEarly = stoppedEarly;
        result.deviceRemoved = deviceRemoved;
        result.stopReason = std::move(stopReason);
        result.firstTimingError = std::move(firstTimingError);
        result.deviceRemovedReason = std::move(deviceRemovedReason);
        return result;
    }

    D3D11ResidentHotLoopResult WorkerRunD3D11ResidentHotLoop(
        D3D11ResidentHotLoopOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment)
    {
        auto const& variant = options.variant;
        auto requestedElements = options.requestedElements;
        auto durationSeconds = options.durationSeconds;
        auto windowSeconds = options.windowSeconds;
        auto dispatchesPerSample = options.dispatchesPerSample;
        auto warmupDispatches = options.warmupDispatches;
        auto maxDispatches = options.maxDispatches;
        auto observe = [&]()
        {
            return ObserveD3DEnvironment(observeEnvironment);
        };
        auto appMemoryUsageBytes = [&]()
        {
            return observe().appMemoryUsageBytes;
        };
        auto appMemoryUsageLimitBytes = [&]()
        {
            return observe().appMemoryUsageLimitBytes;
        };
        auto appMemoryUsageLevelValue = [&]()
        {
            return observe().appMemoryUsageLevel;
        };

        auto compute = WorkerCreateD3D11ComputeContext(variant.shaderFileName);
        auto resources = CreateD3D11ResidentHotLoopResources(compute, variant, requestedElements);

        ID3D11ShaderResourceView* srvs[] = { resources.srv.get() };
        ID3D11UnorderedAccessView* views[] = { resources.uav.get() };
        UINT initialCounts[] = { 0 };
        compute.context->CSSetShader(compute.shader.get(), nullptr, 0);
        compute.context->CSSetShaderResources(0, 1, srvs);
        compute.context->CSSetUnorderedAccessViews(0, 1, views, initialCounts);

        for (uint64_t i = 0; i < warmupDispatches; ++i)
        {
            compute.context->Dispatch(static_cast<UINT>(resources.dispatchGroups), 1, 1);
        }

        auto started = std::chrono::steady_clock::now();
        auto elapsedSinceStartMs = [&started]()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };

        double const requestedDurationMs = static_cast<double>(durationSeconds) * 1000.0;
        double const requestedWindowMs = static_cast<double>(windowSeconds) * 1000.0;
        auto memoryAtStart = appMemoryUsageBytes();
        auto memoryLimit = appMemoryUsageLimitBytes();
        uint64_t peakAppMemoryUsageBytes = memoryAtStart;

        std::vector<D3D11ResidentHotLoopWindow> windows;
        std::vector<double> allGpuDispatchSamplesMs;
        std::vector<double> allCpuSubmitSamplesMs;
        std::vector<double> allGflopsSamples;
        std::vector<double> allGpuMiBPerSecondSamples;

        uint64_t totalDispatchCount = 0;
        uint64_t totalTimedDispatchCount = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double finalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        double checksum = 0.0;
        uint32_t hash32 = 0;
        D3D11Float4 firstValue{};
        D3D11Float4 lastValue{};
        bool timestampQuerySupported = resources.timestampQuerySupported;
        bool gpuTimingAvailable = false;
        bool verified = true;
        bool stoppedEarly = false;
        bool deviceRemoved = false;
        std::wstring stopReason;
        std::wstring firstTimingError = resources.timingError;
        std::wstring deviceRemovedReason;

        auto beginWindow = [&](uint64_t index, double startMs)
        {
            D3D11ResidentHotLoopWindow window;
            window.index = index;
            window.startedMs = startMs;
            window.appMemoryUsageStartBytes = appMemoryUsageBytes();
            window.appMemoryUsageLimitBytes = memoryLimit;
            window.appMemoryUsageLevel = appMemoryUsageLevelValue();
            window.timestampQuerySupported = resources.timestampQuerySupported;
            if (!resources.timingError.empty())
            {
                window.firstTimingError = resources.timingError;
            }
            if (window.appMemoryUsageStartBytes > peakAppMemoryUsageBytes)
            {
                peakAppMemoryUsageBytes = window.appMemoryUsageStartBytes;
            }
            return window;
        };

        auto finalizeWindow = [&](D3D11ResidentHotLoopWindow& window, double endMs)
        {
            window.endedMs = endMs;
            window.durationMs = endMs - window.startedMs;
            window.appMemoryUsageEndBytes = appMemoryUsageBytes();
            window.appMemoryUsageLevel = appMemoryUsageLevelValue();
            if (window.appMemoryUsageEndBytes > peakAppMemoryUsageBytes)
            {
                peakAppMemoryUsageBytes = window.appMemoryUsageEndBytes;
            }
            window.gpuTimingAvailable = window.gpuTimingSampleCount > 0;
            window.gpuDispatchStats = ComputeD3DDoubleSampleStats(window.gpuDispatchSamplesMs);
            window.cpuSubmitStats = ComputeD3DDoubleSampleStats(window.cpuSubmitSamplesMs);
            window.gflopsStats = ComputeD3DDoubleSampleStats(window.gflopsSamples);
            window.gpuMiBPerSecondStats = ComputeD3DDoubleSampleStats(window.gpuMiBPerSecondSamples);
            std::vector<double>().swap(window.gpuDispatchSamplesMs);
            std::vector<double>().swap(window.cpuSubmitSamplesMs);
            std::vector<double>().swap(window.gflopsSamples);
            std::vector<double>().swap(window.gpuMiBPerSecondSamples);
            windows.push_back(window);
        };

        auto currentWindow = beginWindow(0, 0.0);

        while (elapsedSinceStartMs() < requestedDurationMs)
        {
            if (totalDispatchCount >= maxDispatches)
            {
                stoppedEarly = true;
                stopReason = L"max_dispatches_reached";
                break;
            }

            auto dispatchesThisSample = dispatchesPerSample;
            auto remainingDispatches = maxDispatches - totalDispatchCount;
            if (dispatchesThisSample > remainingDispatches)
            {
                dispatchesThisSample = remainingDispatches;
            }
            if (dispatchesThisSample == 0)
            {
                stoppedEarly = true;
                stopReason = L"max_dispatches_reached";
                break;
            }

            auto submitStarted = std::chrono::steady_clock::now();
            if (resources.timestampQuerySupported)
            {
                compute.context->Begin(resources.disjointQuery.get());
                compute.context->End(resources.startQuery.get());
            }

            for (uint64_t i = 0; i < dispatchesThisSample; ++i)
            {
                compute.context->Dispatch(static_cast<UINT>(resources.dispatchGroups), 1, 1);
            }

            if (resources.timestampQuerySupported)
            {
                compute.context->End(resources.stopQuery.get());
                compute.context->End(resources.disjointQuery.get());
            }
            auto cpuSubmitMs = ElapsedMilliseconds(submitStarted);

            totalDispatchCount += dispatchesThisSample;
            currentWindow.dispatchCount += dispatchesThisSample;
            totalCpuSubmitMs += cpuSubmitMs;
            ++totalCpuSubmitSampleCount;
            currentWindow.totalCpuSubmitMs += cpuSubmitMs;
            ++currentWindow.cpuSubmitSampleCount;
            currentWindow.cpuSubmitSamplesMs.push_back(cpuSubmitMs);
            allCpuSubmitSamplesMs.push_back(cpuSubmitMs);

            if (resources.timestampQuerySupported)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                UINT64 startTicks = 0;
                UINT64 stopTicks = 0;
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        resources.disjointQuery.get(),
                        &disjointData,
                        sizeof(disjointData),
                        L"resident_hotloop_timestamp_disjoint"))
                {
                    stoppedEarly = true;
                    stopReason = L"timestamp_disjoint_timeout";
                    firstTimingError = L"timestamp disjoint query timed out";
                    currentWindow.firstTimingError = firstTimingError;
                    verified = false;
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        resources.startQuery.get(),
                        &startTicks,
                        sizeof(startTicks),
                        L"resident_hotloop_timestamp_start"))
                {
                    stoppedEarly = true;
                    stopReason = L"timestamp_start_timeout";
                    firstTimingError = L"timestamp start query timed out";
                    currentWindow.firstTimingError = firstTimingError;
                    verified = false;
                    break;
                }
                if (!WaitForD3D11QueryData(
                        compute.context.get(),
                        resources.stopQuery.get(),
                        &stopTicks,
                        sizeof(stopTicks),
                        L"resident_hotloop_timestamp_stop"))
                {
                    stoppedEarly = true;
                    stopReason = L"timestamp_stop_timeout";
                    firstTimingError = L"timestamp stop query timed out";
                    currentWindow.firstTimingError = firstTimingError;
                    verified = false;
                    break;
                }

                if (disjointData.Disjoint || disjointData.Frequency == 0 || stopTicks < startTicks)
                {
                    ++totalGpuDisjointCount;
                    ++currentWindow.gpuDisjointCount;
                }
                else
                {
                    auto gpuBlockMs =
                        (static_cast<double>(stopTicks - startTicks) / static_cast<double>(disjointData.Frequency)) * 1000.0;
                    auto perDispatchMs = gpuBlockMs / static_cast<double>(dispatchesThisSample);
                    auto sampleFp32Ops = resources.elements * dispatchesThisSample * variant.fp32OpsPerElement;
                    auto sampleGpuBytes = resources.gpuBytesPerDispatch * dispatchesThisSample;
                    auto sampleGflops = (variant.fp32OpsPerElement > 0 && gpuBlockMs > 0.0)
                        ? ((static_cast<double>(sampleFp32Ops) / (gpuBlockMs / 1000.0)) / 1000000000.0)
                        : 0.0;
                    auto sampleMiBPerSecond = gpuBlockMs > 0.0
                        ? ((static_cast<double>(sampleGpuBytes) / 1048576.0) / (gpuBlockMs / 1000.0))
                        : 0.0;

                    totalGpuDispatchMs += gpuBlockMs;
                    totalTimedDispatchCount += dispatchesThisSample;
                    ++totalGpuTimingSampleCount;
                    totalElementsTimed += resources.elements * dispatchesThisSample;
                    totalGpuBytesTimed += sampleGpuBytes;
                    totalFp32OpsTimed += sampleFp32Ops;

                    currentWindow.totalGpuDispatchMs += gpuBlockMs;
                    currentWindow.timedDispatchCount += dispatchesThisSample;
                    ++currentWindow.gpuTimingSampleCount;
                    currentWindow.elementsTimed += resources.elements * dispatchesThisSample;
                    currentWindow.gpuBytesTimed += sampleGpuBytes;
                    currentWindow.fp32OpsTimed += sampleFp32Ops;

                    currentWindow.gpuDispatchSamplesMs.push_back(perDispatchMs);
                    currentWindow.gflopsSamples.push_back(sampleGflops);
                    currentWindow.gpuMiBPerSecondSamples.push_back(sampleMiBPerSecond);
                    allGpuDispatchSamplesMs.push_back(perDispatchMs);
                    allGflopsSamples.push_back(sampleGflops);
                    allGpuMiBPerSecondSamples.push_back(sampleMiBPerSecond);
                }
            }

            auto removedHr = compute.device->GetDeviceRemovedReason();
            if (removedHr != S_OK)
            {
                stoppedEarly = true;
                stopReason = L"device_removed";
                deviceRemoved = true;
                deviceRemovedReason = HResultToString(removedHr);
                verified = false;
                if (firstTimingError.empty())
                {
                    firstTimingError = L"D3D11 device removed: " + deviceRemovedReason;
                }
                if (currentWindow.firstTimingError.empty())
                {
                    currentWindow.firstTimingError = firstTimingError;
                }
                break;
            }

            auto nowMs = elapsedSinceStartMs();
            if (nowMs - currentWindow.startedMs >= requestedWindowMs)
            {
                finalizeWindow(currentWindow, nowMs);
                currentWindow = beginWindow(static_cast<uint64_t>(windows.size()), nowMs);
            }
        }

        auto elapsedMs = elapsedSinceStartMs();
        if (currentWindow.dispatchCount > 0 || windows.empty())
        {
            finalizeWindow(currentWindow, elapsedMs);
        }

        std::vector<D3D11Float4> values(static_cast<size_t>(resources.elements));
        if (!deviceRemoved && totalDispatchCount > 0)
        {
            auto readbackStarted = std::chrono::steady_clock::now();
            compute.context->CopyResource(resources.stagingBuffer.get(), resources.outputBuffer.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto mapHr = compute.context->Map(resources.stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            finalVerificationReadbackMs = ElapsedMilliseconds(readbackStarted);
            if (FAILED(mapHr))
            {
                throw WorkerProtocolError("d3d11_resident_hotloop.map_failed", "Map staging failed: " + WideToUtf8(HResultToString(mapHr)));
            }
            std::memcpy(values.data(), mapped.pData, static_cast<size_t>(resources.outputBytes));
            compute.context->Unmap(resources.stagingBuffer.get(), 0);

            for (uint64_t i = 0; i < resources.elements; ++i)
            {
                auto const& actual = values[static_cast<size_t>(i)];
                auto expected = ExpectedD3D11ShaderMatrixValue(static_cast<uint32_t>(i), variant);
                bool elementMatches = true;
                double absError = 0.0;
                double relativeError = 0.0;

                if (!FloatComponentMatches(actual.x, expected.x, absError, relativeError))
                {
                    elementMatches = false;
                }
                if (absError > maxAbsError) { maxAbsError = absError; }
                if (relativeError > maxRelativeError) { maxRelativeError = relativeError; }

                if (!FloatComponentMatches(actual.y, expected.y, absError, relativeError))
                {
                    elementMatches = false;
                }
                if (absError > maxAbsError) { maxAbsError = absError; }
                if (relativeError > maxRelativeError) { maxRelativeError = relativeError; }

                if (!FloatComponentMatches(actual.z, expected.z, absError, relativeError))
                {
                    elementMatches = false;
                }
                if (absError > maxAbsError) { maxAbsError = absError; }
                if (relativeError > maxRelativeError) { maxRelativeError = relativeError; }

                if (!FloatComponentMatches(actual.w, expected.w, absError, relativeError))
                {
                    elementMatches = false;
                }
                if (absError > maxAbsError) { maxAbsError = absError; }
                if (relativeError > maxRelativeError) { maxRelativeError = relativeError; }

                if (!elementMatches)
                {
                    ++maxMismatchCount;
                }
                checksum += static_cast<double>(actual.x) +
                    static_cast<double>(actual.y) +
                    static_cast<double>(actual.z) +
                    static_cast<double>(actual.w);
            }

            firstValue = values.front();
            lastValue = values.back();
            hash32 = NativeStatic::Hash32(
                reinterpret_cast<uint8_t const*>(values.data()),
                values.size() * sizeof(D3D11Float4));
        }
        else
        {
            verified = false;
        }

        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
        compute.context->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        compute.context->CSSetShaderResources(0, 1, nullSrvs);
        compute.context->CSSetShader(nullptr, nullptr, 0);

        gpuTimingAvailable = totalGpuTimingSampleCount > 0;
        verified = verified && maxMismatchCount == 0 && !deviceRemoved && totalDispatchCount > 0;

        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalGpuBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageGpuDispatchMs = totalTimedDispatchCount > 0
            ? totalGpuDispatchMs / static_cast<double>(totalTimedDispatchCount)
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;
        auto aggregateGpuDispatchStats = ComputeD3DDoubleSampleStats(allGpuDispatchSamplesMs);
        auto aggregateCpuSubmitStats = ComputeD3DDoubleSampleStats(allCpuSubmitSamplesMs);
        auto aggregateGflopsStats = ComputeD3DDoubleSampleStats(allGflopsSamples);
        auto aggregateGpuMiBPerSecondStats = ComputeD3DDoubleSampleStats(allGpuMiBPerSecondSamples);

        double initialWindowMedianGflops = 0.0;
        double finalWindowMedianGflops = 0.0;
        double medianGflopsDriftPercent = 0.0;
        double minWindowMedianGflops = 0.0;
        double maxWindowMedianGflops = 0.0;
        if (!windows.empty())
        {
            initialWindowMedianGflops = windows.front().gflopsStats.median;
            finalWindowMedianGflops = windows.back().gflopsStats.median;
            medianGflopsDriftPercent = initialWindowMedianGflops != 0.0
                ? ((finalWindowMedianGflops - initialWindowMedianGflops) / initialWindowMedianGflops) * 100.0
                : 0.0;
            minWindowMedianGflops = windows.front().gflopsStats.median;
            maxWindowMedianGflops = windows.front().gflopsStats.median;
            for (auto const& window : windows)
            {
                if (window.gflopsStats.median < minWindowMedianGflops)
                {
                    minWindowMedianGflops = window.gflopsStats.median;
                }
                if (window.gflopsStats.median > maxWindowMedianGflops)
                {
                    maxWindowMedianGflops = window.gflopsStats.median;
                }
            }
        }

        auto memoryAtEnd = appMemoryUsageBytes();
        if (memoryAtEnd > peakAppMemoryUsageBytes)
        {
            peakAppMemoryUsageBytes = memoryAtEnd;
        }

        D3D11ResidentHotLoopResult result;
        result.featureLevel = compute.featureLevel;
        result.elements = resources.elements;
        result.dispatchGroups = resources.dispatchGroups;
        result.windows = std::move(windows);
        result.totalDispatchCount = totalDispatchCount;
        result.totalTimedDispatchCount = totalTimedDispatchCount;
        result.totalGpuTimingSampleCount = totalGpuTimingSampleCount;
        result.totalGpuDisjointCount = totalGpuDisjointCount;
        result.totalCpuSubmitSampleCount = totalCpuSubmitSampleCount;
        result.totalElementsTimed = totalElementsTimed;
        result.totalGpuBytesTimed = totalGpuBytesTimed;
        result.totalFp32OpsTimed = totalFp32OpsTimed;
        result.totalGpuDispatchMs = totalGpuDispatchMs;
        result.totalCpuSubmitMs = totalCpuSubmitMs;
        result.finalVerificationReadbackMs = finalVerificationReadbackMs;
        result.elapsedMs = elapsedMs;
        result.aggregateFp32OpsPerSecond = aggregateFp32OpsPerSecond;
        result.aggregateGflops = aggregateGflops;
        result.aggregateGpuMiBPerSecond = aggregateGpuMiBPerSecond;
        result.averageGpuDispatchMs = averageGpuDispatchMs;
        result.averageCpuSubmitMs = averageCpuSubmitMs;
        result.aggregateGpuDispatchStats = aggregateGpuDispatchStats;
        result.aggregateCpuSubmitStats = aggregateCpuSubmitStats;
        result.aggregateGflopsStats = aggregateGflopsStats;
        result.aggregateGpuMiBPerSecondStats = aggregateGpuMiBPerSecondStats;
        result.initialWindowMedianGflops = initialWindowMedianGflops;
        result.finalWindowMedianGflops = finalWindowMedianGflops;
        result.medianGflopsDriftPercent = medianGflopsDriftPercent;
        result.minWindowMedianGflops = minWindowMedianGflops;
        result.maxWindowMedianGflops = maxWindowMedianGflops;
        result.memoryAtStart = memoryAtStart;
        result.peakAppMemoryUsageBytes = peakAppMemoryUsageBytes;
        result.memoryLimit = memoryLimit;
        result.environmentEnd = observe();
        result.environmentEnd.appMemoryUsageBytes = memoryAtEnd;
        result.firstValue = firstValue;
        result.lastValue = lastValue;
        result.checksum = checksum;
        result.hash32 = hash32;
        result.maxMismatchCount = maxMismatchCount;
        result.maxAbsError = maxAbsError;
        result.maxRelativeError = maxRelativeError;
        result.timestampQuerySupported = timestampQuerySupported;
        result.gpuTimingAvailable = gpuTimingAvailable;
        result.verified = verified;
        result.stoppedEarly = stoppedEarly;
        result.deviceRemoved = deviceRemoved;
        result.stopReason = std::move(stopReason);
        result.firstTimingError = std::move(firstTimingError);
        result.deviceRemovedReason = std::move(deviceRemovedReason);
        return result;
    }

}
