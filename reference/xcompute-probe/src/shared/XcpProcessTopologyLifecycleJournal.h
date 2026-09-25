#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

namespace XComputeShared
{
    class XcpProcessTopologyLifecycleJournal final
    {
    public:
        static constexpr uint64_t MaximumEvents = 128;
        static constexpr wchar_t const* SchemaVersion = L"xcp-process-topology-lifecycle-journal-v1";

        explicit XcpProcessTopologyLifecycleJournal(std::wstring component)
            : component_(std::move(component)), activationIdentity_(NewActivationIdentity()),
              processIdentity_(L"pid:" + std::to_wstring(GetCurrentProcessId()))
        {
            try
            {
                auto root = std::filesystem::path(std::wstring(
                    winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()));
                path_ = root / (L"xcp-process-topology-lifecycle-" + component_ + L"-v1.json");
                LoadPrevious();
            }
            catch (...)
            {
                continuityValid_ = false;
                activationEpoch_ = 1;
                nextSequence_ = 1;
                events_ = winrt::Windows::Data::Json::JsonArray();
            }
            Record(L"activated");
        }

        XcpProcessTopologyLifecycleJournal(XcpProcessTopologyLifecycleJournal const&) = delete;
        XcpProcessTopologyLifecycleJournal& operator=(XcpProcessTopologyLifecycleJournal const&) = delete;

        std::wstring const& ActivationIdentity() const noexcept { return activationIdentity_; }
        std::wstring const& ProcessIdentity() const noexcept { return processIdentity_; }
        uint64_t ActivationEpoch() const noexcept { return activationEpoch_; }

        void Record(std::wstring_view eventName, uint64_t heartbeatSequence = 0) noexcept
        {
            try
            {
                std::lock_guard guard(mutex_);
                if (!AllowedEvent(eventName))
                {
                    continuityValid_ = false;
                    return;
                }
                if (eventName != L"heartbeat" && events_.Size() != 0)
                {
                    auto previous = events_.GetObjectAt(events_.Size() - 1);
                    if (previous.GetNamedString(L"event", L"") == eventName &&
                        static_cast<uint64_t>(previous.GetNamedNumber(L"activation_epoch", 0)) == activationEpoch_)
                    {
                        return;
                    }
                }

                winrt::Windows::Data::Json::JsonObject item;
                item.Insert(L"sequence", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(static_cast<double>(nextSequence_)));
                item.Insert(L"component", winrt::Windows::Data::Json::JsonValue::CreateStringValue(component_));
                item.Insert(L"event", winrt::Windows::Data::Json::JsonValue::CreateStringValue(std::wstring(eventName)));
                item.Insert(L"activation_epoch", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(static_cast<double>(activationEpoch_)));
                item.Insert(L"activation_identity", winrt::Windows::Data::Json::JsonValue::CreateStringValue(activationIdentity_));
                item.Insert(L"process_identity", winrt::Windows::Data::Json::JsonValue::CreateStringValue(processIdentity_));
                item.Insert(L"unix_ms", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(static_cast<double>(UnixMilliseconds())));
                item.Insert(L"heartbeat_sequence", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(static_cast<double>(heartbeatSequence)));
                events_.Append(item);
                ++nextSequence_;
                while (events_.Size() > MaximumEvents) { events_.RemoveAt(0); }
                PersistLocked();
            }
            catch (...)
            {
                continuityValid_ = false;
            }
        }

        winrt::Windows::Data::Json::JsonObject Snapshot() const
        {
            std::lock_guard guard(mutex_);
            return SnapshotLocked();
        }

    private:
        static uint64_t UnixMilliseconds() noexcept
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        static bool AllowedEvent(std::wstring_view value) noexcept
        {
            return value == L"activated" || value == L"suspending" || value == L"resumed" ||
                value == L"terminated" || value == L"heartbeat";
        }

        static std::wstring NewActivationIdentity()
        {
            auto value = std::wstring(winrt::to_hstring(
                winrt::Windows::Foundation::GuidHelper::CreateNewGuid()).c_str());
            value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch)
            {
                return ch == L'{' || ch == L'}';
            }), value.end());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }

        void LoadPrevious()
        {
            activationEpoch_ = 1;
            nextSequence_ = 1;
            events_ = winrt::Windows::Data::Json::JsonArray();
            if (path_.empty() || !std::filesystem::exists(path_)) { return; }

            std::ifstream input(path_, std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (!input.good() && !input.eof()) { throw std::runtime_error("lifecycle journal read failed"); }
            auto previous = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(raw));
            if (previous.GetNamedString(L"schema_version", L"") != SchemaVersion ||
                previous.GetNamedString(L"component", L"") != component_ ||
                !previous.GetNamedBoolean(L"continuity_valid", false))
            {
                throw std::runtime_error("lifecycle journal continuity invalid");
            }
            auto priorEpoch = static_cast<uint64_t>(previous.GetNamedNumber(L"activation_epoch", 0));
            auto priorSequence = static_cast<uint64_t>(previous.GetNamedNumber(L"last_sequence", 0));
            auto priorEvents = previous.GetNamedArray(L"events");
            if (priorEpoch == 0 || priorSequence == 0 || priorEvents.Size() == 0 || priorEvents.Size() > MaximumEvents)
            {
                throw std::runtime_error("lifecycle journal bounds invalid");
            }
            auto firstSequence = static_cast<uint64_t>(priorEvents.GetObjectAt(0).GetNamedNumber(L"sequence", 0));
            auto lastSequence = static_cast<uint64_t>(priorEvents.GetObjectAt(priorEvents.Size() - 1).GetNamedNumber(L"sequence", 0));
            if (firstSequence == 0 || lastSequence != priorSequence || lastSequence - firstSequence + 1 != priorEvents.Size())
            {
                throw std::runtime_error("lifecycle journal sequence invalid");
            }
            for (uint32_t index = 0; index != priorEvents.Size(); ++index) { events_.Append(priorEvents.GetAt(index)); }
            activationEpoch_ = priorEpoch + 1;
            nextSequence_ = priorSequence + 1;
        }

        winrt::Windows::Data::Json::JsonObject SnapshotLocked() const
        {
            using winrt::Windows::Data::Json::JsonValue;
            winrt::Windows::Data::Json::JsonObject result;
            result.Insert(L"schema_version", JsonValue::CreateStringValue(SchemaVersion));
            result.Insert(L"component", JsonValue::CreateStringValue(component_));
            result.Insert(L"continuity_valid", JsonValue::CreateBooleanValue(continuityValid_));
            result.Insert(L"maximum_events", JsonValue::CreateNumberValue(static_cast<double>(MaximumEvents)));
            result.Insert(L"activation_epoch", JsonValue::CreateNumberValue(static_cast<double>(activationEpoch_)));
            result.Insert(L"activation_identity", JsonValue::CreateStringValue(activationIdentity_));
            result.Insert(L"process_identity", JsonValue::CreateStringValue(processIdentity_));
            result.Insert(L"last_sequence", JsonValue::CreateNumberValue(static_cast<double>(nextSequence_ - 1)));
            winrt::Windows::Data::Json::JsonArray copied;
            for (uint32_t index = 0; index != events_.Size(); ++index) { copied.Append(events_.GetAt(index)); }
            result.Insert(L"events", copied);
            return result;
        }

        void PersistLocked()
        {
            if (path_.empty()) { continuityValid_ = false; return; }
            auto temporary = path_;
            temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
            auto raw = winrt::to_string(SnapshotLocked().Stringify());
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(raw.data(), static_cast<std::streamsize>(raw.size()));
                output.flush();
                if (!output.good()) { continuityValid_ = false; return; }
            }
            std::error_code error;
            std::filesystem::remove(path_, error);
            error.clear();
            std::filesystem::rename(temporary, path_, error);
            if (error)
            {
                continuityValid_ = false;
                std::filesystem::remove(temporary, error);
            }
        }

        std::wstring component_;
        std::wstring activationIdentity_;
        std::wstring processIdentity_;
        std::filesystem::path path_;
        uint64_t activationEpoch_ = 1;
        uint64_t nextSequence_ = 1;
        mutable std::mutex mutex_;
        bool continuityValid_ = true;
        winrt::Windows::Data::Json::JsonArray events_;
    };
}