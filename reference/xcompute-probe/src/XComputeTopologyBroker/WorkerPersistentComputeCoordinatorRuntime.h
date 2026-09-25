#pragma once

#include <string_view>

#include <winrt/Windows.Data.Json.h>

#include "../shared/XcpStorageReservationLedger.h"

namespace winrt::XComputeTopologyBroker::implementation
{
    class WorkerPersistentComputeCoordinatorRuntime final
    {
    public:
        Windows::Data::Json::JsonObject Describe() const;
        Windows::Data::Json::JsonObject Acquire(
            Windows::Data::Json::JsonObject const& request);
        Windows::Data::Json::JsonObject Status(
            Windows::Data::Json::JsonObject const& request) const;
        Windows::Data::Json::JsonObject Release(
            Windows::Data::Json::JsonObject const& request);
        void ReleaseAll(std::wstring_view disposition) noexcept;

    private:
        XComputeShared::XcpReservationBinding ReadBinding(
            Windows::Data::Json::JsonObject const& request) const;
        Windows::Data::Json::JsonObject OperationJson(
            XComputeShared::XcpReservationOperation const& operation) const;
        Windows::Data::Json::JsonObject PolicyJson() const;

        XComputeShared::XcpStorageReservationLedger ledger_;
    };
}
