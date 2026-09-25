#include "pch.h"
#include "ProbeResult.h"

namespace XComputeProbe
{
    std::wstring JsonEscape(std::wstring const& input)
    {
        std::wostringstream out;
        for (wchar_t ch : input)
        {
            switch (ch)
            {
            case L'\\': out << L"\\\\"; break;
            case L'"': out << L"\\\""; break;
            case L'\b': out << L"\\b"; break;
            case L'\f': out << L"\\f"; break;
            case L'\n': out << L"\\n"; break;
            case L'\r': out << L"\\r"; break;
            case L'\t': out << L"\\t"; break;
            default:
                if (ch < 0x20)
                {
                    out << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<int>(ch);
                }
                else
                {
                    out << ch;
                }
            }
        }
        return out.str();
    }

    std::wstring JsonString(std::wstring const& input)
    {
        return L"\"" + JsonEscape(input) + L"\"";
    }

    std::wstring UtcNow()
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        tm utc{};
        gmtime_s(&utc, &time);
        std::wostringstream out;
        out << std::put_time(&utc, L"%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    uint64_t ElapsedMs(std::chrono::steady_clock::time_point started)
    {
        auto elapsed = std::chrono::steady_clock::now() - started;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }

    void ProbeResult::AddString(std::wstring const& name, std::wstring const& value)
    {
        values[name] = JsonString(value);
    }

    void ProbeResult::AddNumber(std::wstring const& name, uint64_t value)
    {
        values[name] = std::to_wstring(value);
    }

    void ProbeResult::AddBool(std::wstring const& name, bool value)
    {
        values[name] = value ? L"true" : L"false";
    }

    void ProbeResult::AddNull(std::wstring const& name, std::wstring const& reason)
    {
        values[name] = L"{\"value\":null,\"reason\":" + JsonString(reason) + L"}";
    }

    static std::wstring JsonArray(std::vector<std::wstring> const& items)
    {
        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0) out << L",";
            out << JsonString(items[i]);
        }
        out << L"]";
        return out.str();
    }

    std::wstring ProbeResult::ToJson() const
    {
        std::wostringstream out;
        out << L"{";
        out << L"\"id\":" << JsonString(id) << L",";
        out << L"\"status\":" << JsonString(status) << L",";
        out << L"\"evidence_class\":" << JsonString(evidenceClass) << L",";
        out << L"\"started_utc\":" << JsonString(startedUtc) << L",";
        out << L"\"ended_utc\":" << JsonString(endedUtc) << L",";
        out << L"\"duration_ms\":" << durationMs << L",";
        out << L"\"values\":{";
        size_t index = 0;
        for (auto const& pair : values)
        {
            if (index++ != 0) out << L",";
            out << JsonString(pair.first) << L":" << pair.second;
        }
        out << L"},";
        out << L"\"api_calls\":" << JsonArray(apiCalls) << L",";
        out << L"\"errors\":" << JsonArray(errors) << L",";
        out << L"\"notes\":" << JsonArray(notes);
        out << L"}";
        return out.str();
    }
}

