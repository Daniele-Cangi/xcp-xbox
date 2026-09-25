#pragma once

#include "pch.h"
#include "../ProbeResult.h"

namespace XComputeProbe
{
    class ProbeRunner
    {
    public:
        std::vector<ProbeResult> RunSafeSuite(std::vector<ProbeResult> const& graphicsProbes);
        std::wstring BuildReportJson(std::vector<ProbeResult> const& probes) const;
        bool SaveReport(std::wstring const& reportJson);
        winrt::fire_and_forget SaveReportAsync(std::wstring reportJson);
        winrt::fire_and_forget SendReportToLanReceiverAsync(std::wstring reportJson);

    private:
        ProbeResult RunPlatformProbe();
        ProbeResult RunMemoryProbe();
        ProbeResult RunCpuProbe();
        ProbeResult RunStorageProbe();
        ProbeResult RunNetworkProbe();
        ProbeResult RunInputLifecycleProbe();
        ProbeResult RunGameBucketProbe();
        ProbeResult RunPackagedNativeModulesProbe();
        ProbeResult RunDynamicCodeProbe();
        ProbeResult RunExpandedResourcesProbe();
    };
}
