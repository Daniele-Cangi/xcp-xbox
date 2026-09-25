#pragma once
#include <winrt/Windows.Data.Json.h>
#include "WorkerProcessTopologyBrokerPhaseRuntime.h"
namespace winrt::XComputeTopologyBroker::implementation
{
    class WorkerProcessTopologyBrokerStorageRuntime final
    {
    public:
        static Windows::Data::Json::JsonObject Exchange(
            WorkerProcessTopologyBrokerPhaseRuntime const& phaseRuntime,
            Windows::Data::Json::JsonObject const& request);
        static Windows::Data::Json::JsonObject CleanupStatus(
            WorkerProcessTopologyBrokerPhaseRuntime const& phaseRuntime,
            Windows::Data::Json::JsonObject const& request);
    };
}