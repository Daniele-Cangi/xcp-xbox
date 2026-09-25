#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace XComputeProbe
{
    struct D3D12AdapterProbe
    {
        uint32_t index = 0;
        std::wstring description;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        uint32_t subSysId = 0;
        uint32_t revision = 0;
        uint64_t dedicatedVideoMemory = 0;
        uint64_t dedicatedSystemMemory = 0;
        uint64_t sharedSystemMemory = 0;
        uint32_t flags = 0;
        bool software = false;
    };

    struct D3D12DeviceProbe
    {
        HRESULT factoryHr = E_FAIL;
        HRESULT probeNullHr = E_FAIL;
        HRESULT createDeviceHr = E_FAIL;
        HRESULT checkFeatureLevelsHr = E_FAIL;
        HRESULT optionsHr = E_FAIL;
        HRESULT deviceRemovedReason = S_OK;
        bool factoryCreated = false;
        bool d3d12ProbeSucceeded = false;
        bool deviceCreated = false;
        bool featureLevelsChecked = false;
        D3D_FEATURE_LEVEL maxSupportedFeatureLevel = D3D_FEATURE_LEVEL_10_0;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        uint32_t adapterCount = 0;
        uint32_t selectedAdapterIndex = UINT32_MAX;
        D3D12AdapterProbe selectedAdapter;
        std::vector<D3D12AdapterProbe> adapters;
    };

    struct D3D12ComputeSmokeMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t shaderBytes = 0;
        D3D12_COMMAND_LIST_TYPE commandListType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        HRESULT factoryHr = E_FAIL;
        HRESULT createDeviceHr = E_FAIL;
        HRESULT rootSignatureHr = E_FAIL;
        HRESULT pipelineStateHr = E_FAIL;
        HRESULT createCommandQueueHr = E_FAIL;
        HRESULT createCommandAllocatorHr = E_FAIL;
        HRESULT createCommandListHr = E_FAIL;
        HRESULT outputBufferHr = E_FAIL;
        HRESULT readbackBufferHr = E_FAIL;
        HRESULT closeCommandListHr = E_FAIL;
        HRESULT createFenceHr = E_FAIL;
        HRESULT signalFenceHr = E_FAIL;
        HRESULT mapHr = E_FAIL;
        HRESULT deviceRemovedReason = S_OK;
        bool deviceCreated = false;
        bool rootSignatureCreated = false;
        bool pipelineStateCreated = false;
        bool commandQueueCreated = false;
        bool commandListCreated = false;
        bool outputBufferCreated = false;
        bool readbackBufferCreated = false;
        bool fenceCompleted = false;
        bool verified = false;
        uint64_t fenceValue = 1;
        uint64_t completedFenceValue = 0;
        uint64_t firstValue = 0;
        uint64_t lastValue = 0;
        uint64_t checksum = 0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double gpuSubmitAndWaitMs = 0.0;
        double readbackVerifyMs = 0.0;
        uint32_t selectedAdapterIndex = UINT32_MAX;
        D3D12AdapterProbe selectedAdapter;
    };

    struct D3D12ComputeContext
    {
        std::wstring shaderName;
        D3D12_COMMAND_LIST_TYPE commandListType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        uint64_t shaderBytes = 0;
        HRESULT factoryHr = E_FAIL;
        HRESULT createDeviceHr = E_FAIL;
        HRESULT rootSignatureHr = E_FAIL;
        HRESULT pipelineStateHr = E_FAIL;
        HRESULT createCommandQueueHr = E_FAIL;
        bool deviceCreated = false;
        bool rootSignatureCreated = false;
        bool pipelineStateCreated = false;
        bool commandQueueCreated = false;
        uint32_t selectedAdapterIndex = UINT32_MAX;
        D3D12AdapterProbe selectedAdapter;
        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12RootSignature> rootSignature;
        winrt::com_ptr<ID3D12PipelineState> pipelineState;
        winrt::com_ptr<ID3D12CommandQueue> commandQueue;
    };

    struct D3D12ComputeSweepMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        HRESULT outputBufferHr = E_FAIL;
        HRESULT readbackBufferHr = E_FAIL;
        HRESULT createFenceHr = E_FAIL;
        HRESULT deviceRemovedReason = S_OK;
        bool outputBufferCreated = false;
        bool readbackBufferCreated = false;
        bool fenceCompleted = false;
        bool verified = false;
        uint64_t fenceValue = 0;
        uint64_t completedFenceValue = 0;
        uint64_t firstValue = 0;
        uint64_t lastValue = 0;
        uint64_t checksum = 0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double totalGpuSubmitAndWaitMs = 0.0;
        double averageGpuSubmitAndWaitMs = 0.0;
        double readbackVerifyMs = 0.0;
        double elementsPerSecond = 0.0;
        double mibPerSecond = 0.0;
    };

    struct D3D12ComputeTimingMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        uint64_t timestampFrequency = 0;
        HRESULT outputBufferHr = E_FAIL;
        HRESULT readbackBufferHr = E_FAIL;
        HRESULT timestampQueryHeapHr = E_FAIL;
        HRESULT timestampReadbackBufferHr = E_FAIL;
        HRESULT timestampFrequencyHr = E_FAIL;
        HRESULT createFenceHr = E_FAIL;
        HRESULT deviceRemovedReason = S_OK;
        bool outputBufferCreated = false;
        bool readbackBufferCreated = false;
        bool timestampQueryHeapCreated = false;
        bool timestampReadbackBufferCreated = false;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        bool fenceCompleted = false;
        bool verified = false;
        uint64_t fenceValue = 0;
        uint64_t completedFenceValue = 0;
        uint64_t firstValue = 0;
        uint64_t lastValue = 0;
        uint64_t checksum = 0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double totalGpuDispatchMs = 0.0;
        double averageGpuDispatchMs = 0.0;
        double totalCpuSubmitAndWaitMs = 0.0;
        double averageCpuSubmitAndWaitMs = 0.0;
        double readbackVerifyMs = 0.0;
        double gpuElementsPerSecond = 0.0;
        double gpuMiBPerSecond = 0.0;
    };

    struct D3D11Float4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };

    struct D3D11ComputeContext
    {
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        winrt::com_ptr<ID3D11ComputeShader> shader;
        std::wstring shaderName;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        uint64_t shaderBytes = 0;
    };

    struct D3D11ComputeMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        double totalDispatchAndReadbackMs = 0.0;
        double averageDispatchAndReadbackMs = 0.0;
        double elementsPerSecond = 0.0;
        double mibPerSecond = 0.0;
        uint32_t firstValue = 0;
        uint32_t lastValue = 0;
        uint64_t checksum = 0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        bool verified = false;
    };

    struct D3D11ComputeTimingMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t gpuTimestampFrequency = 0;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        double totalGpuDispatchMs = 0.0;
        double averageGpuDispatchMs = 0.0;
        double gpuElementsPerSecond = 0.0;
        double gpuMiBPerSecond = 0.0;
        uint64_t cpuSubmitSampleCount = 0;
        double totalCpuSubmitMs = 0.0;
        double averageCpuSubmitMs = 0.0;
        double verificationReadbackMs = 0.0;
        std::wstring timingError;
        uint32_t firstValue = 0;
        uint32_t lastValue = 0;
        uint64_t checksum = 0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        bool verified = false;
    };

    struct D3D11FloatTimingMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        uint64_t fp32OpsPerElement = 32ull * 4ull * 2ull;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t gpuTimestampFrequency = 0;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        uint64_t fp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double averageGpuDispatchMs = 0.0;
        double fp32OpsPerSecond = 0.0;
        double gflops = 0.0;
        double gpuMiBPerSecond = 0.0;
        uint64_t cpuSubmitSampleCount = 0;
        double totalCpuSubmitMs = 0.0;
        double averageCpuSubmitMs = 0.0;
        double verificationReadbackMs = 0.0;
        std::wstring timingError;
        D3D11Float4 firstValue{};
        D3D11Float4 lastValue{};
        double checksum = 0.0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool verified = false;
    };

    struct D3D12FloatTimingMeasurement
    {
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t bytes = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        uint64_t fp32OpsPerElement = 32ull * 4ull * 2ull;
        uint64_t timestampFrequency = 0;
        uint64_t fp32OpsTimed = 0;
        HRESULT outputBufferHr = E_FAIL;
        HRESULT readbackBufferHr = E_FAIL;
        HRESULT timestampQueryHeapHr = E_FAIL;
        HRESULT timestampReadbackBufferHr = E_FAIL;
        HRESULT timestampFrequencyHr = E_FAIL;
        HRESULT createFenceHr = E_FAIL;
        HRESULT deviceRemovedReason = S_OK;
        bool outputBufferCreated = false;
        bool readbackBufferCreated = false;
        bool timestampQueryHeapCreated = false;
        bool timestampReadbackBufferCreated = false;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        bool fenceCompleted = false;
        bool verified = false;
        uint64_t fenceValue = 0;
        uint64_t completedFenceValue = 0;
        D3D11Float4 firstValue{};
        D3D11Float4 lastValue{};
        double checksum = 0.0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        double totalGpuDispatchMs = 0.0;
        double averageGpuDispatchMs = 0.0;
        double fp32OpsPerSecond = 0.0;
        double gflops = 0.0;
        double totalCpuSubmitAndWaitMs = 0.0;
        double averageCpuSubmitAndWaitMs = 0.0;
        double readbackVerifyMs = 0.0;
        double gpuMiBPerSecond = 0.0;
    };

    struct D3D11ShaderMatrixVariant
    {
        std::wstring id;
        std::wstring shaderFileName;
        std::wstring category;
        uint32_t loopCount = 0;
        uint64_t fp32OpsPerElement = 0;
        uint64_t gpuBytesPerElement = sizeof(D3D11Float4) * 2ull;
    };

    struct D3D11ShaderMatrixMeasurement
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t inputBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t gpuBytesPerDispatch = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        uint64_t shaderBytes = 0;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t gpuTimestampFrequency = 0;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        uint64_t fp32OpsTimed = 0;
        uint64_t gpuBytesTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double averageGpuDispatchMs = 0.0;
        double fp32OpsPerSecond = 0.0;
        double gflops = 0.0;
        double gpuMiBPerSecond = 0.0;
        uint64_t cpuSubmitSampleCount = 0;
        double totalCpuSubmitMs = 0.0;
        double averageCpuSubmitMs = 0.0;
        double verificationReadbackMs = 0.0;
        std::wstring timingError;
        D3D11Float4 firstValue{};
        D3D11Float4 lastValue{};
        double checksum = 0.0;
        uint32_t hash32 = 0;
        uint64_t mismatches = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool verified = false;
    };

    struct D3D11ShaderMatrixVariantSummary
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t shaderBytes = 0;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        std::vector<D3D11ShaderMatrixMeasurement> measurements;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;
    };

    struct D3DDoubleSampleStats
    {
        uint64_t count = 0;
        double min = 0.0;
        double median = 0.0;
        double p95 = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double stddev = 0.0;
    };

    struct WorkerD3DEnvironmentObservation
    {
        uint64_t appMemoryUsageBytes = 0;
        uint64_t appMemoryUsageLimitBytes = 0;
        uint64_t appMemoryUsageLevel = 0;
        uint64_t workspaceBytes = 0;
    };

    using WorkerD3DEnvironmentObserver =
        std::function<WorkerD3DEnvironmentObservation()>;

    struct D3D12ShaderShapeMatrixOptions
    {
        std::vector<uint64_t> elementsList;
        std::vector<D3D11ShaderMatrixVariant> variants;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
    };

    struct D3D12ShaderShapeMatrixResult
    {
        std::vector<D3D11ShaderMatrixVariantSummary> summaries;
        uint64_t resultCount = 0;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        double elapsedMs = 0.0;
        double aggregateFp32OpsPerSecond = 0.0;
        double aggregateGflops = 0.0;
        double aggregateGpuMiBPerSecond = 0.0;
        double averageCpuSubmitMs = 0.0;
        std::wstring bestFp32VariantId;
        double bestFp32VariantGflops = 0.0;
        std::wstring bestBandwidthVariantId;
        double bestBandwidthMiBPerSecond = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        bool deviceCreated = true;
        bool rootSignatureCreated = true;
        bool pipelineStateCreated = true;
        bool commandQueueCreated = true;
        uint32_t selectedAdapterIndex = UINT32_MAX;
        D3D12AdapterProbe selectedAdapter;
    };

    struct D3D12ShaderShapeSoakOptions
    {
        D3D12ShaderShapeMatrixOptions matrix;
        uint64_t windowCount = 0;
        uint64_t discardInitialWindows = 0;
        uint64_t windowPauseMs = 0;
    };

    struct D3D12ShaderShapeSoakWindow
    {
        uint64_t index = 0;
        bool measured = false;
        D3D12ShaderShapeMatrixResult result;
        WorkerD3DEnvironmentObservation environment;
    };

    struct D3D12ShaderShapeSoakResult
    {
        std::vector<D3D12ShaderShapeSoakWindow> windows;
        uint64_t measuredWindowCount = 0;
        uint64_t totalFp32OpsTimed = 0;
        uint64_t maxMismatchCount = 0;
        uint64_t minAppMemoryLimitBytes = 0;
        double averageBestFp32Gflops = 0.0;
        double minBestFp32Gflops = 0.0;
        double maxBestFp32Gflops = 0.0;
        double spreadBestFp32Percent = 0.0;
        double averageAggregateGflops = 0.0;
        double averageTotalGpuDispatchMs = 0.0;
        double maxRelativeError = 0.0;
        bool verified = true;
    };

    struct D3D11SustainedSoakOptions
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t requestedElements = 0;
        uint64_t durationSeconds = 0;
        uint64_t windowSeconds = 0;
        uint64_t repeatsPerBatch = 0;
        uint64_t warmupRepeats = 0;
        uint64_t maxBatches = 0;
    };

    struct D3D11SustainedSoakWindow
    {
        uint64_t index = 0;
        double startedMs = 0.0;
        double endedMs = 0.0;
        double durationMs = 0.0;
        uint64_t batchCount = 0;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t cpuSubmitSampleCount = 0;
        uint64_t elementsTimed = 0;
        uint64_t gpuBytesTimed = 0;
        uint64_t fp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint64_t appMemoryUsageStartBytes = 0;
        uint64_t appMemoryUsageEndBytes = 0;
        uint64_t appMemoryUsageLimitBytes = 0;
        uint64_t appMemoryUsageLevel = 0;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        std::vector<double> gflopsSamples;
        std::vector<double> gpuMiBPerSecondSamples;
        D3DDoubleSampleStats gpuDispatchStats;
        D3DDoubleSampleStats cpuSubmitStats;
        D3DDoubleSampleStats gflopsStats;
        D3DDoubleSampleStats gpuMiBPerSecondStats;
    };

    struct D3D11SustainedSoakResult
    {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        std::vector<D3D11SustainedSoakWindow> windows;
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
        double elapsedMs = 0.0;
        double aggregateFp32OpsPerSecond = 0.0;
        double aggregateGflops = 0.0;
        double aggregateGpuMiBPerSecond = 0.0;
        double averageCpuSubmitMs = 0.0;
        D3DDoubleSampleStats aggregateGpuDispatchStats;
        D3DDoubleSampleStats aggregateCpuSubmitStats;
        D3DDoubleSampleStats aggregateGflopsStats;
        D3DDoubleSampleStats aggregateGpuMiBPerSecondStats;
        double initialWindowMedianGflops = 0.0;
        double finalWindowMedianGflops = 0.0;
        double medianGflopsDriftPercent = 0.0;
        double minWindowMedianGflops = 0.0;
        double maxWindowMedianGflops = 0.0;
        uint64_t memoryAtStart = 0;
        uint64_t peakAppMemoryUsageBytes = 0;
        uint64_t memoryLimit = 0;
        WorkerD3DEnvironmentObservation environmentEnd;
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
    };

    struct D3D11ResidentHotLoopOptions
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t requestedElements = 0;
        uint64_t durationSeconds = 0;
        uint64_t windowSeconds = 0;
        uint64_t dispatchesPerSample = 0;
        uint64_t warmupDispatches = 0;
        uint64_t maxDispatches = 0;
    };

    struct D3D11ResidentHotLoopWindow
    {
        uint64_t index = 0;
        double startedMs = 0.0;
        double endedMs = 0.0;
        double durationMs = 0.0;
        uint64_t dispatchCount = 0;
        uint64_t timedDispatchCount = 0;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t cpuSubmitSampleCount = 0;
        uint64_t elementsTimed = 0;
        uint64_t gpuBytesTimed = 0;
        uint64_t fp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        uint64_t appMemoryUsageStartBytes = 0;
        uint64_t appMemoryUsageEndBytes = 0;
        uint64_t appMemoryUsageLimitBytes = 0;
        uint64_t appMemoryUsageLevel = 0;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        std::wstring firstTimingError;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        std::vector<double> gflopsSamples;
        std::vector<double> gpuMiBPerSecondSamples;
        D3DDoubleSampleStats gpuDispatchStats;
        D3DDoubleSampleStats cpuSubmitStats;
        D3DDoubleSampleStats gflopsStats;
        D3DDoubleSampleStats gpuMiBPerSecondStats;
    };

    struct D3D11ResidentHotLoopResult
    {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        std::vector<D3D11ResidentHotLoopWindow> windows;
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
        double elapsedMs = 0.0;
        double aggregateFp32OpsPerSecond = 0.0;
        double aggregateGflops = 0.0;
        double aggregateGpuMiBPerSecond = 0.0;
        double averageGpuDispatchMs = 0.0;
        double averageCpuSubmitMs = 0.0;
        D3DDoubleSampleStats aggregateGpuDispatchStats;
        D3DDoubleSampleStats aggregateCpuSubmitStats;
        D3DDoubleSampleStats aggregateGflopsStats;
        D3DDoubleSampleStats aggregateGpuMiBPerSecondStats;
        double initialWindowMedianGflops = 0.0;
        double finalWindowMedianGflops = 0.0;
        double medianGflopsDriftPercent = 0.0;
        double minWindowMedianGflops = 0.0;
        double maxWindowMedianGflops = 0.0;
        uint64_t memoryAtStart = 0;
        uint64_t peakAppMemoryUsageBytes = 0;
        uint64_t memoryLimit = 0;
        WorkerD3DEnvironmentObservation environmentEnd;
        D3D11Float4 firstValue{};
        D3D11Float4 lastValue{};
        double checksum = 0.0;
        uint32_t hash32 = 0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool timestampQuerySupported = false;
        bool gpuTimingAvailable = false;
        bool verified = false;
        bool stoppedEarly = false;
        bool deviceRemoved = false;
        std::wstring stopReason;
        std::wstring firstTimingError;
        std::wstring deviceRemovedReason;
    };

    D3D12AdapterProbe WorkerReadD3D12AdapterProbe(uint32_t index, IDXGIAdapter1* adapter);
    D3D12DeviceProbe WorkerRunD3D12DeviceProbe();
    D3D12ComputeSmokeMeasurement WorkerRunD3D12ComputeSmoke(uint64_t requestedElements);
    D3D12ComputeContext WorkerCreateD3D12ComputeContext(
        std::wstring const& shaderFileName = L"IntCompute.cso",
        D3D12_COMMAND_LIST_TYPE commandListType = D3D12_COMMAND_LIST_TYPE_COMPUTE,
        bool includeInputSrv = false);
    D3D12ComputeSweepMeasurement WorkerRunD3D12ComputeSweepDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D12ComputeTimingMeasurement WorkerRunD3D12ComputeTimingDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D12FloatTimingMeasurement WorkerRunD3D12FloatTimingDispatch(
        D3D12ComputeContext const& context,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D11ShaderMatrixMeasurement WorkerRunD3D12ShaderShapeTimingDispatch(
        D3D12ComputeContext const& context,
        D3D11ShaderMatrixVariant const& variant,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D11ComputeContext WorkerCreateD3D11ComputeContext(
        std::wstring const& shaderFileName = L"IntCompute.cso");
    D3D11ComputeMeasurement WorkerRunD3D11ComputeDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D11ComputeTimingMeasurement WorkerRunD3D11ComputeTimingDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D11FloatTimingMeasurement WorkerRunD3D11FloatTimingDispatch(
        D3D11ComputeContext const& compute,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D11ShaderMatrixMeasurement WorkerRunD3D11ShaderMatrixTimingDispatch(
        D3D11ComputeContext const& compute,
        D3D11ShaderMatrixVariant const& variant,
        uint64_t requestedElements,
        uint64_t repeats,
        uint64_t warmupRepeats);
    D3D12ShaderShapeMatrixResult WorkerRunD3D12ShaderShapeMatrix(
        D3D12ShaderShapeMatrixOptions const& options);
    D3D12ShaderShapeSoakResult WorkerRunD3D12ShaderShapeSoak(
        D3D12ShaderShapeSoakOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment,
        std::atomic<bool> const* cancelRequested = nullptr);
    D3D11SustainedSoakResult WorkerRunD3D11SustainedSoak(
        D3D11SustainedSoakOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment);
    D3D11ResidentHotLoopResult WorkerRunD3D11ResidentHotLoop(
        D3D11ResidentHotLoopOptions const& options,
        WorkerD3DEnvironmentObserver const& observeEnvironment);
}
