#pragma once

#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerProcessTopologyLifecycleRuntime.h"

namespace XComputeProbe
{
    class WorkerProcessTopologyRuntime
    {
    public:
        WorkerProcessTopologyRuntime() = default;

        std::wstring Probe(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion);
        void RecordLifecycleEvent(std::wstring_view eventName) noexcept
        {
            lifecycleRuntime_.RecordLifecycleEvent(eventName);
        }

    private:
        ProcessTopology::WorkerProcessTopologyLifecycleRuntime lifecycleRuntime_;
    };

    std::wstring WorkerProcessTopologyCapabilityJson();
}