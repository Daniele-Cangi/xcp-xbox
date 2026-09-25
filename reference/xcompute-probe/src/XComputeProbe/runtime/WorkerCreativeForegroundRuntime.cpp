#include "pch.h"
#include "WorkerCreativeForegroundRuntime.h"

#include <array>
#include <set>
#include <xaudio2.h>

#include "WorkerArtifactStore.h"
#include "WorkerCreativeHostRuntime.h"
#include "WorkerCreativeInstallRuntime.h"
#include "WorkerXvmInterpreter.h"
#include "WorkerXvmVerifier.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Gaming::Input;
using namespace Windows::System;
namespace fs = std::filesystem;

namespace XComputeProbe
{
    namespace
    {
        constexpr uint64_t MaxModuleDocumentBytes =
            16ull * 1024ull * 1024ull;
        constexpr uint64_t MaxLaunchRecords = 4096;
        constexpr uint64_t MaxLocalStateBytes = 8ull * 1024ull * 1024ull;
        constexpr uint64_t MaxSceneTriangles = 65536;
        constexpr uint64_t MaxDecodedAssetFileBytes =
            16ull * 1024ull * 1024ull;
        constexpr uint32_t MaxSpriteDimension = 4096;
        constexpr uint64_t MaxSpriteDecodedBytes =
            64ull * 1024ull * 1024ull;
        constexpr uint32_t MaxMeshVertices = 65536;
        constexpr uint32_t MaxMeshIndices = 196608;
        constexpr uint32_t MaxSceneAnimations = 256;
        constexpr uint32_t MaxAudioClips = 128;
        constexpr uint32_t MaxAudioVoices = 32;
        constexpr uint64_t MaxAudioPcmBytes =
            16ull * 1024ull * 1024ull;
        constexpr uint32_t MaxXvmServices = 32;
        constexpr uint64_t MaxXvmProgramBytes =
            1ull * 1024ull * 1024ull;
        constexpr uint64_t MaxXvmFuelPerInvocation = 1000000;
        constexpr uint64_t MaxXvmMemoryBytes =
            1ull * 1024ull * 1024ull;
        constexpr uint64_t MaxXvmOutputBytes =
            1ull * 1024ull * 1024ull;
        constexpr uint32_t MaxXvmInvocationsPerTick = 32;
        constexpr size_t MaxLogEntries = 256;
        constexpr size_t MaxEventsPerTick = 256;
        constexpr size_t MaxRecentSemanticEvents = 64;
        constexpr size_t MaxRecentPhysicalInputs = 64;

        uint64_t PrimitiveTriangleCount(
            std::wstring const& primitive)
        {
            if (primitive == L"plane")
                return 2;
            if (primitive == L"cube")
                return 12;
            if (primitive == L"cylinder")
                return 64;
            if (primitive == L"sphere" ||
                primitive == L"capsule")
            {
                return 320;
            }
            return 0;
        }

        std::string WideToUtf8(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring Utf8ToWide(std::string const& value)
        {
            return std::wstring(winrt::to_hstring(value));
        }

        [[noreturn]] void FailForeground(
            std::string code,
            std::string message,
            std::string stage,
            std::string field = {},
            std::string path = {},
            std::string expected = {},
            std::string actual = {},
            std::string correction = {},
            bool retryable = false)
        {
            WorkerCreativeHostErrorDetails details;
            details.stage = std::move(stage);
            details.field = std::move(field);
            details.path = std::move(path);
            details.expected = std::move(expected);
            details.actual = std::move(actual);
            if (!correction.empty())
            {
                details.correction = std::move(correction);
            }
            details.retryable = retryable;
            throw WorkerCreativeHostError(
                std::move(code),
                std::move(message),
                std::move(details));
        }

        bool IsLowerHexSha256(std::wstring const& value)
        {
            return value.size() == 64 &&
                std::all_of(
                    value.begin(),
                    value.end(),
                    [](wchar_t ch)
                    {
                        return (ch >= L'0' && ch <= L'9') ||
                            (ch >= L'a' && ch <= L'f');
                    });
        }

        bool IsSafeIdentifier(
            std::wstring const& value,
            size_t maximum = 96,
            bool requireSeparator = false)
        {
            if (value.empty() ||
                value.size() > maximum ||
                value.front() < L'a' ||
                value.front() > L'z')
            {
                return false;
            }
            bool previousSeparator = false;
            bool hasSeparator = false;
            for (auto ch : value)
            {
                auto alphanumeric =
                    (ch >= L'a' && ch <= L'z') ||
                    (ch >= L'0' && ch <= L'9');
                auto separator =
                    ch == L'.' || ch == L'_' || ch == L'-';
                if (!alphanumeric && !separator)
                {
                    return false;
                }
                if (separator && previousSeparator)
                {
                    return false;
                }
                previousSeparator = separator;
                hasSeparator = hasSeparator || separator;
            }
            return !previousSeparator &&
                (!requireSeparator || hasSeparator);
        }

        bool IsSafeLaunchId(std::wstring const& value)
        {
            return !value.empty() &&
                value.size() <= 64 &&
                std::all_of(
                    value.begin(),
                    value.end(),
                    [](wchar_t ch)
                    {
                        return (ch >= L'a' && ch <= L'z') ||
                            (ch >= L'A' && ch <= L'Z') ||
                            (ch >= L'0' && ch <= L'9') ||
                            ch == L'_' || ch == L'-';
                    });
        }

        void RequireExactFields(
            JsonObject const& object,
            std::set<std::wstring> const& allowed,
            std::string const& path)
        {
            for (auto const& field : object)
            {
                auto name = std::wstring(field.Key());
                if (allowed.find(name) == allowed.end())
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative foreground document contains an unknown field",
                        "foreground_plan_validation",
                        WideToUtf8(name),
                        path,
                        "only fields declared by the authoritative module schema",
                        WideToUtf8(name),
                        "remove the unknown field and rebuild the project");
                }
            }
        }

        std::wstring RequiredString(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            size_t maximum = 4096)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::String)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground string field is missing or invalid",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "string",
                    "missing or non-string",
                    "correct the module using the published schema");
            }
            auto value = std::wstring(object.GetNamedString(name));
            if (value.size() > maximum)
            {
                FailForeground(
                    "xcp.creative.module_budget_exceeded",
                    "creative foreground string exceeds its bound",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "at most " + std::to_string(maximum) + " characters",
                    std::to_string(value.size()),
                    "shorten the value and rebuild");
            }
            return value;
        }

        std::wstring OptionalString(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            size_t maximum = 4096)
        {
            return object.HasKey(name)
                ? RequiredString(object, name, path, maximum)
                : std::wstring();
        }

        JsonObject RequiredObject(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Object)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground object field is missing or invalid",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "object",
                    "missing or non-object",
                    "correct the module using the published schema");
            }
            return object.GetNamedObject(name);
        }

        JsonArray RequiredArray(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            uint32_t minimum,
            uint32_t maximum)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Array)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground array field is missing or invalid",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "array",
                    "missing or non-array",
                    "correct the module using the published schema");
            }
            auto array = object.GetNamedArray(name);
            if (array.Size() < minimum || array.Size() > maximum)
            {
                FailForeground(
                    "xcp.creative.module_budget_exceeded",
                    "creative foreground array exceeds its item bound",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    std::to_string(minimum) + ".." +
                        std::to_string(maximum) + " items",
                    std::to_string(array.Size()),
                    "reduce the module and rebuild");
            }
            return array;
        }

        JsonObject ArrayObject(
            JsonArray const& array,
            uint32_t index,
            std::string const& path)
        {
            auto value = array.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground array item must be an object",
                    "foreground_plan_validation",
                    "item",
                    path + "[" + std::to_string(index) + "]",
                    "object",
                    "non-object",
                    "correct the module using the published schema");
            }
            return value.GetObject();
        }

        double RequiredNumber(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Number)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground numeric field is missing or invalid",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "finite number",
                    "missing or non-number",
                    "correct the module using the published schema");
            }
            auto value = object.GetNamedNumber(name);
            auto lowerInvalid =
                exclusiveMinimum ? value <= minimum : value < minimum;
            if (!std::isfinite(value) ||
                lowerInvalid ||
                value > maximum)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground number is outside its bound",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    (exclusiveMinimum ? "greater than " : "at least ") +
                        std::to_string(minimum) + " and at most " +
                        std::to_string(maximum),
                    std::to_string(value),
                    "use a value inside the published bound");
            }
            return value;
        }

        double OptionalNumber(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double fallback,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            return object.HasKey(name)
                ? RequiredNumber(
                    object,
                    name,
                    path,
                    minimum,
                    maximum,
                    exclusiveMinimum)
                : fallback;
        }

        int64_t RequiredInteger(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            int64_t minimum,
            int64_t maximum)
        {
            auto value = RequiredNumber(
                object,
                name,
                path,
                static_cast<double>(minimum),
                static_cast<double>(maximum));
            if (std::floor(value) != value)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground integer field is fractional",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "integer",
                    std::to_string(value),
                    "use an exact integer");
            }
            return static_cast<int64_t>(value);
        }

        bool RequiredBoolean(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Boolean)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground boolean field is missing or invalid",
                    "foreground_plan_validation",
                    WideToUtf8(name),
                    path,
                    "boolean",
                    "missing or non-boolean",
                    "correct the module using the published schema");
            }
            return object.GetNamedBoolean(name);
        }

        bool OptionalBoolean(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            bool fallback)
        {
            return object.HasKey(name)
                ? RequiredBoolean(object, name, path)
                : fallback;
        }

        void RequireIdentifier(
            std::wstring const& value,
            std::string const& field,
            std::string const& path,
            bool contract = false)
        {
            if (!IsSafeIdentifier(value, contract ? 128 : 96, contract))
            {
                FailForeground(
                    "xcp.creative.module_identity_invalid",
                    "creative foreground identifier is invalid",
                    "foreground_plan_validation",
                    field,
                    path,
                    contract
                        ? "bounded lowercase contract id"
                        : "bounded lowercase identifier",
                    WideToUtf8(value),
                    "use the identifier syntax published by the module schema");
            }
        }

        WorkerCreativeVec2 RequiredVec2(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            auto array = RequiredArray(object, name, path, 2, 2);
            WorkerCreativeVec2 result;
            for (uint32_t index = 0; index < 2; ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    (exclusiveMinimum
                        ? value.GetNumber() <= minimum
                        : value.GetNumber() < minimum) ||
                    value.GetNumber() > maximum)
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative foreground vector component is invalid",
                        "foreground_plan_validation",
                        WideToUtf8(name),
                        path,
                        "two finite bounded numbers",
                        "invalid component",
                        "correct the vector using the published schema");
                }
                (index == 0 ? result.x : result.y) =
                    static_cast<float>(value.GetNumber());
            }
            return result;
        }

        WorkerCreativeVec3 RequiredVec3(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            auto array = RequiredArray(object, name, path, 3, 3);
            WorkerCreativeVec3 result;
            for (uint32_t index = 0; index < 3; ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    (exclusiveMinimum
                        ? value.GetNumber() <= minimum
                        : value.GetNumber() < minimum) ||
                    value.GetNumber() > maximum)
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative foreground vector component is invalid",
                        "foreground_plan_validation",
                        WideToUtf8(name),
                        path,
                        "three finite bounded numbers",
                        "invalid component",
                        "correct the vector using the published schema");
                }
                auto projected = static_cast<float>(value.GetNumber());
                if (index == 0) result.x = projected;
                if (index == 1) result.y = projected;
                if (index == 2) result.z = projected;
            }
            return result;
        }

        uint8_t HexByte(std::wstring const& value, size_t offset)
        {
            auto nibble = [](wchar_t ch) -> uint8_t
            {
                if (ch >= L'0' && ch <= L'9')
                    return static_cast<uint8_t>(ch - L'0');
                if (ch >= L'a' && ch <= L'f')
                    return static_cast<uint8_t>(10 + ch - L'a');
                if (ch >= L'A' && ch <= L'F')
                    return static_cast<uint8_t>(10 + ch - L'A');
                return 0xff;
            };
            auto high = nibble(value[offset]);
            auto low = nibble(value[offset + 1]);
            if (high == 0xff || low == 0xff)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative foreground color contains invalid hexadecimal digits",
                    "foreground_plan_validation",
                    "color",
                    WideToUtf8(value),
                    "#RRGGBB or #RRGGBBAA",
                    WideToUtf8(value),
                    "correct the color and rebuild");
            }
            return static_cast<uint8_t>((high << 4) | low);
        }

        WorkerCreativeColor ParseColor(
            std::wstring const& value,
            bool allowToken)
        {
            if ((value.size() == 7 || value.size() == 9) &&
                value.front() == L'#')
            {
                WorkerCreativeColor color;
                color.r = HexByte(value, 1) / 255.0f;
                color.g = HexByte(value, 3) / 255.0f;
                color.b = HexByte(value, 5) / 255.0f;
                color.a = value.size() == 9
                    ? HexByte(value, 7) / 255.0f
                    : 1.0f;
                return color;
            }
            if (allowToken && IsSafeIdentifier(value, 64))
            {
                uint32_t hash = 2166136261u;
                for (auto ch : value)
                {
                    hash ^= static_cast<uint32_t>(ch);
                    hash *= 16777619u;
                }
                return {
                    0.25f + ((hash >> 0) & 0xff) / 510.0f,
                    0.25f + ((hash >> 8) & 0xff) / 510.0f,
                    0.25f + ((hash >> 16) & 0xff) / 510.0f,
                    1.0f
                };
            }
            FailForeground(
                "xcp.creative.module_schema_rejected",
                "creative foreground color is invalid",
                "foreground_plan_validation",
                "color",
                WideToUtf8(value),
                allowToken
                    ? "#RRGGBB, #RRGGBBAA or a style token"
                    : "#RRGGBB or #RRGGBBAA",
                WideToUtf8(value),
                "correct the color using the published schema");
        }

        WorkerCreativeScalar ParseScalar(
            IJsonValue const& value,
            std::string const& path,
            size_t maxString = 4096)
        {
            switch (value.ValueType())
            {
            case JsonValueType::Boolean:
                return value.GetBoolean();
            case JsonValueType::Number:
            {
                auto number = value.GetNumber();
                if (!std::isfinite(number) ||
                    number < -1000000000.0 ||
                    number > 1000000000.0)
                {
                    break;
                }
                if (std::floor(number) == number &&
                    number >= -2147483648.0 &&
                    number <= 2147483647.0)
                {
                    return static_cast<int64_t>(number);
                }
                return number;
            }
            case JsonValueType::String:
            {
                auto text = std::wstring(value.GetString());
                if (text.size() <= maxString)
                {
                    return text;
                }
                break;
            }
            default:
                break;
            }
            FailForeground(
                "xcp.creative.module_schema_rejected",
                "creative foreground scalar is invalid",
                "foreground_plan_validation",
                "scalar",
                path,
                "bounded boolean, integer, number or string",
                "unsupported or out of range",
                "correct the scalar using the published schema");
        }

        bool ScalarMatchesType(
            WorkerCreativeScalar const& value,
            std::wstring const& type)
        {
            if (type == L"boolean")
                return std::holds_alternative<bool>(value);
            if (type == L"integer")
                return std::holds_alternative<int64_t>(value);
            if (type == L"number")
                return std::holds_alternative<int64_t>(value) ||
                    std::holds_alternative<double>(value);
            if (type == L"string")
                return std::holds_alternative<std::wstring>(value);
            return false;
        }

        JsonObject ParseModuleDocument(
            WorkerCreativeInstalledFile const& file)
        {
            if (!fs::exists(file.casPath) ||
                !fs::is_regular_file(file.casPath) ||
                static_cast<uint64_t>(fs::file_size(file.casPath)) !=
                    file.bytes ||
                file.bytes > MaxModuleDocumentBytes ||
                WorkerContentSha256File(file.casPath) != file.sha256)
            {
                FailForeground(
                    "xcp.creative.foreground_content_invalid",
                    "creative foreground module no longer matches its installed binding",
                    "foreground_plan_validation",
                    "module",
                    WideToUtf8(file.runtimePath),
                    WideToUtf8(file.sha256) + ":" +
                        std::to_string(file.bytes),
                    "missing or changed",
                    "recommit and reactivate the exact bundle");
            }
            std::ifstream input(file.casPath, std::ios::binary);
            auto bytes = std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
            try
            {
                auto value = JsonValue::Parse(Utf8ToWide(bytes));
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw std::runtime_error("root");
                }
                return value.GetObject();
            }
            catch (WorkerCreativeHostError const&)
            {
                throw;
            }
            catch (...)
            {
                FailForeground(
                    "xcp.creative.module_json_invalid",
                    "creative foreground module is not a UTF-8 JSON object",
                    "foreground_plan_validation",
                    "module",
                    WideToUtf8(file.runtimePath),
                    "valid UTF-8 JSON object",
                    "parse failed",
                    "correct the module and rebuild");
            }
        }

        void ValidateDecodedAssetBinding(
            WorkerCreativeAsset const& asset)
        {
            if (!fs::exists(asset.casPath) ||
                !fs::is_regular_file(asset.casPath) ||
                static_cast<uint64_t>(
                    fs::file_size(asset.casPath)) != asset.bytes ||
                asset.bytes == 0 ||
                asset.bytes > MaxDecodedAssetFileBytes ||
                WorkerContentSha256File(asset.casPath) !=
                    asset.sha256)
            {
                FailForeground(
                    "xcp.creative.foreground_content_invalid",
                    "creative decoded asset no longer matches its installed binding",
                    "foreground_plan_validation",
                    "asset",
                    WideToUtf8(asset.runtimePath),
                    WideToUtf8(asset.sha256) + ":" +
                        std::to_string(asset.bytes) +
                        " and at most 16777216 bytes",
                    "missing, changed or oversized",
                    "rebuild, recommit and reactivate the exact bundle");
            }
        }

        void DecodePngAsset(WorkerCreativeAsset& asset)
        {
            ValidateDecodedAssetBinding(asset);
            if (asset.mediaType != L"image/png")
            {
                FailForeground(
                    "xcp.creative.sprite_asset_media_type_invalid",
                    "creative sprite asset does not use the published PNG format",
                    "foreground_plan_validation",
                    "media_type",
                    WideToUtf8(asset.runtimePath),
                    "image/png",
                    WideToUtf8(asset.mediaType),
                    "declare a bounded PNG asset and rebuild");
            }

            com_ptr<IWICImagingFactory2> factory;
            auto hr = CoCreateInstance(
                CLSID_WICImagingFactory2,
                nullptr,
                CLSCTX_INPROC_SERVER,
                __uuidof(IWICImagingFactory2),
                factory.put_void());
            if (FAILED(hr))
            {
                FailForeground(
                    "xcp.creative.sprite_decoder_unavailable",
                    "creative PNG decoder is unavailable",
                    "foreground_plan_validation",
                    "decoder",
                    "xcp.decoder.wic-png.v1",
                    "packaged WIC PNG decoder",
                    "initialization failed",
                    "retry on a host publishing the PNG decoder");
            }

            com_ptr<IWICBitmapDecoder> decoder;
            hr = factory->CreateDecoderFromFilename(
                asset.casPath.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.put());
            GUID container{};
            UINT frameCount = 0;
            if (FAILED(hr) ||
                FAILED(decoder->GetContainerFormat(&container)) ||
                !IsEqualGUID(container, GUID_ContainerFormatPng) ||
                FAILED(decoder->GetFrameCount(&frameCount)) ||
                frameCount != 1)
            {
                FailForeground(
                    "xcp.creative.sprite_asset_invalid",
                    "creative sprite is not a single-frame PNG",
                    "foreground_plan_validation",
                    "asset",
                    WideToUtf8(asset.runtimePath),
                    "one valid PNG frame",
                    "decode or container validation failed",
                    "export a single-frame PNG and rebuild");
            }

            com_ptr<IWICBitmapFrameDecode> frame;
            UINT width = 0;
            UINT height = 0;
            if (FAILED(decoder->GetFrame(0, frame.put())) ||
                FAILED(frame->GetSize(&width, &height)) ||
                width == 0 || height == 0 ||
                width > MaxSpriteDimension ||
                height > MaxSpriteDimension)
            {
                FailForeground(
                    "xcp.creative.sprite_asset_budget_exceeded",
                    "creative sprite dimensions exceed the published bound",
                    "foreground_plan_validation",
                    "dimensions",
                    WideToUtf8(asset.runtimePath),
                    "1..4096 pixels per dimension",
                    std::to_string(width) + "x" +
                        std::to_string(height),
                    "resize the PNG and rebuild");
            }
            auto stride = static_cast<uint64_t>(width) * 4ull;
            auto decodedBytes =
                stride * static_cast<uint64_t>(height);
            if (decodedBytes > MaxSpriteDecodedBytes)
            {
                FailForeground(
                    "xcp.creative.sprite_asset_budget_exceeded",
                    "creative sprite decoded bytes exceed the published bound",
                    "foreground_plan_validation",
                    "decoded_bytes",
                    WideToUtf8(asset.runtimePath),
                    "at most 67108864 BGRA bytes",
                    std::to_string(decodedBytes),
                    "resize the PNG and rebuild");
            }

            com_ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(
                    converter.put())) ||
                FAILED(converter->Initialize(
                    frame.get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                FailForeground(
                    "xcp.creative.sprite_asset_invalid",
                    "creative sprite cannot be converted to bounded premultiplied BGRA",
                    "foreground_plan_validation",
                    "pixel_format",
                    WideToUtf8(asset.runtimePath),
                    "WIC 32bpp premultiplied BGRA",
                    "conversion failed",
                    "export a standard RGBA PNG and rebuild");
            }
            asset.decodedPixels.resize(
                static_cast<size_t>(decodedBytes));
            if (FAILED(converter->CopyPixels(
                    nullptr,
                    static_cast<UINT>(stride),
                    static_cast<UINT>(decodedBytes),
                    asset.decodedPixels.data())))
            {
                FailForeground(
                    "xcp.creative.sprite_asset_invalid",
                    "creative sprite pixel decode failed",
                    "foreground_plan_validation",
                    "pixels",
                    WideToUtf8(asset.runtimePath),
                    "complete bounded BGRA decode",
                    "decode failed",
                    "export a standard RGBA PNG and rebuild");
            }
            asset.pixelWidth = width;
            asset.pixelHeight = height;
            asset.pixelStride = static_cast<uint32_t>(stride);
        }

        std::vector<uint8_t> ReadDecodedAssetBytes(
            WorkerCreativeAsset const& asset)
        {
            ValidateDecodedAssetBinding(asset);
            std::ifstream input(asset.casPath, std::ios::binary);
            if (!input)
            {
                FailForeground(
                    "xcp.creative.asset_read_failed",
                    "creative decoded asset cannot be read",
                    "foreground_plan_validation",
                    "asset",
                    WideToUtf8(asset.runtimePath),
                    "readable installed asset",
                    "open failed",
                    "recommit and reactivate the exact bundle");
            }
            return std::vector<uint8_t>(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }

        uint16_t ReadLe16(
            std::vector<uint8_t> const& bytes,
            size_t offset)
        {
            return static_cast<uint16_t>(bytes[offset]) |
                static_cast<uint16_t>(
                    static_cast<uint16_t>(bytes[offset + 1]) << 8);
        }

        uint32_t ReadLe32(
            std::vector<uint8_t> const& bytes,
            size_t offset)
        {
            return static_cast<uint32_t>(bytes[offset]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }

        void DecodeWavAsset(WorkerCreativeAsset& asset)
        {
            if (asset.mediaType != L"audio/wav")
            {
                FailForeground(
                    "xcp.creative.audio_asset_media_type_invalid",
                    "creative audio clip does not use the published WAV format",
                    "foreground_plan_validation",
                    "media_type",
                    WideToUtf8(asset.runtimePath),
                    "audio/wav",
                    WideToUtf8(asset.mediaType),
                    "declare a bounded PCM WAV asset and rebuild");
            }
            auto bytes = ReadDecodedAssetBytes(asset);
            auto matches = [&](size_t offset, char const* value)
            {
                return offset + 4 <= bytes.size() &&
                    bytes[offset] ==
                        static_cast<uint8_t>(value[0]) &&
                    bytes[offset + 1] ==
                        static_cast<uint8_t>(value[1]) &&
                    bytes[offset + 2] ==
                        static_cast<uint8_t>(value[2]) &&
                    bytes[offset + 3] ==
                        static_cast<uint8_t>(value[3]);
            };
            if (bytes.size() < 44 ||
                !matches(0, "RIFF") ||
                !matches(8, "WAVE") ||
                static_cast<uint64_t>(ReadLe32(bytes, 4)) + 8ull !=
                    bytes.size())
            {
                FailForeground(
                    "xcp.creative.audio_asset_invalid",
                    "creative audio asset is not an exact RIFF/WAVE file",
                    "foreground_plan_validation",
                    "asset",
                    WideToUtf8(asset.runtimePath),
                    "bounded RIFF/WAVE with exact container length",
                    "invalid container",
                    "export a canonical PCM WAV and rebuild");
            }

            bool foundFormat = false;
            bool foundData = false;
            size_t dataOffset = 0;
            uint32_t dataBytes = 0;
            size_t offset = 12;
            while (offset + 8 <= bytes.size())
            {
                auto chunkBytes = ReadLe32(bytes, offset + 4);
                auto payload = offset + 8;
                auto padded =
                    static_cast<uint64_t>(chunkBytes) +
                    static_cast<uint64_t>(chunkBytes & 1u);
                if (padded >
                    static_cast<uint64_t>(bytes.size() - payload))
                {
                    FailForeground(
                        "xcp.creative.audio_asset_invalid",
                        "creative WAV chunk exceeds the bounded container",
                        "foreground_plan_validation",
                        "asset",
                        WideToUtf8(asset.runtimePath),
                        "complete in-bounds WAV chunks",
                        "truncated chunk",
                        "export a canonical PCM WAV and rebuild");
                }
                if (matches(offset, "fmt "))
                {
                    if (foundFormat || chunkBytes < 16 ||
                        ReadLe16(bytes, payload) != 1)
                    {
                        FailForeground(
                            "xcp.creative.audio_asset_invalid",
                            "creative WAV format is not single PCM",
                            "foreground_plan_validation",
                            "format",
                            WideToUtf8(asset.runtimePath),
                            "one PCM fmt chunk",
                            "duplicate or non-PCM format",
                            "export 16-bit PCM WAV and rebuild");
                    }
                    foundFormat = true;
                    asset.audioChannels =
                        ReadLe16(bytes, payload + 2);
                    asset.audioSampleRate =
                        ReadLe32(bytes, payload + 4);
                    asset.audioAverageBytesPerSecond =
                        ReadLe32(bytes, payload + 8);
                    asset.audioBlockAlign =
                        ReadLe16(bytes, payload + 12);
                    asset.audioBitsPerSample =
                        ReadLe16(bytes, payload + 14);
                }
                else if (matches(offset, "data"))
                {
                    if (foundData)
                    {
                        FailForeground(
                            "xcp.creative.audio_asset_invalid",
                            "creative WAV contains multiple data chunks",
                            "foreground_plan_validation",
                            "data",
                            WideToUtf8(asset.runtimePath),
                            "one PCM data chunk",
                            "duplicate data chunk",
                            "export a canonical PCM WAV and rebuild");
                    }
                    foundData = true;
                    dataOffset = payload;
                    dataBytes = chunkBytes;
                }
                offset = payload + static_cast<size_t>(padded);
            }

            auto expectedBlockAlign =
                static_cast<uint32_t>(asset.audioChannels) *
                static_cast<uint32_t>(asset.audioBitsPerSample) / 8u;
            auto expectedAverage =
                static_cast<uint64_t>(asset.audioSampleRate) *
                expectedBlockAlign;
            if (!foundFormat || !foundData ||
                (asset.audioChannels != 1 &&
                    asset.audioChannels != 2) ||
                asset.audioBitsPerSample != 16 ||
                asset.audioSampleRate < 8000 ||
                asset.audioSampleRate > 48000 ||
                asset.audioBlockAlign != expectedBlockAlign ||
                asset.audioAverageBytesPerSecond != expectedAverage ||
                dataBytes == 0 ||
                dataBytes > MaxAudioPcmBytes ||
                dataBytes % asset.audioBlockAlign != 0)
            {
                FailForeground(
                    "xcp.creative.audio_asset_invalid",
                    "creative WAV violates the published PCM contract",
                    "foreground_plan_validation",
                    "format/data",
                    WideToUtf8(asset.runtimePath),
                    "16-bit mono/stereo PCM at 8000..48000 Hz and at most 16777216 PCM bytes",
                    "unsupported or inconsistent format",
                    "convert the clip to the published PCM WAV profile");
            }
            asset.decodedAudio.assign(
                bytes.begin() + dataOffset,
                bytes.begin() + dataOffset + dataBytes);
        }

        WorkerXvmProgram DecodeXvmProgramAsset(
            WorkerCreativeAsset const& asset)
        {
            if (asset.mediaType !=
                L"application/vnd.xcp.xvm-program-v2+json")
            {
                FailForeground(
                    "xcp.creative.xvm_asset_media_type_invalid",
                    "creative XVM service does not use the published program artifact format",
                    "foreground_plan_validation",
                    "media_type",
                    WideToUtf8(asset.runtimePath),
                    "application/vnd.xcp.xvm-program-v2+json",
                    WideToUtf8(asset.mediaType),
                    "assemble a canonical XVM v2 program artifact");
            }
            if (asset.bytes > MaxXvmProgramBytes)
            {
                FailForeground(
                    "xcp.creative.xvm_program_budget_exceeded",
                    "creative XVM program asset exceeds its byte budget",
                    "foreground_plan_validation",
                    "asset.bytes",
                    WideToUtf8(asset.runtimePath),
                    "at most 1048576 bytes",
                    std::to_string(asset.bytes),
                    "reduce or split the XVM program");
            }
            auto bytes = ReadDecodedAssetBytes(asset);
            WorkerXvmProgram program;
            try
            {
                program = WorkerParseAndVerifyXvmProgram(
                    std::string(bytes.begin(), bytes.end()));
            }
            catch (WorkerXvmError const& error)
            {
                FailForeground(
                    "xcp.creative.xvm_program_rejected",
                    "creative XVM program failed the authoritative verifier",
                    "foreground_plan_validation",
                    error.details.field,
                    WideToUtf8(asset.runtimePath),
                    error.details.expected,
                    error.code + ":" + error.details.actual,
                    "correct the program using describe_xvm_isa and reassemble");
            }
            if (program.maxFuel > MaxXvmFuelPerInvocation ||
                program.memoryBytes > MaxXvmMemoryBytes ||
                program.outputBytes > MaxXvmOutputBytes)
            {
                FailForeground(
                    "xcp.creative.xvm_program_budget_exceeded",
                    "creative XVM program exceeds the service resource budget",
                    "foreground_plan_validation",
                    "max_fuel/memory_bytes/output_bytes",
                    WideToUtf8(asset.runtimePath),
                    "fuel<=1000000, memory<=1048576, output<=1048576",
                    std::to_string(program.maxFuel) + "/" +
                        std::to_string(program.memoryBytes) + "/" +
                        std::to_string(program.outputBytes),
                    "reduce the program resource declaration");
            }
            return program;
        }

        std::vector<WorkerCreativeMesh::Triangle>
            DecodeMeshAsset(WorkerCreativeAsset const& asset)
        {
            ValidateDecodedAssetBinding(asset);
            if (asset.mediaType !=
                L"application/vnd.xcp.mesh-v1+json")
            {
                FailForeground(
                    "xcp.creative.mesh_asset_media_type_invalid",
                    "creative external mesh does not use the published format",
                    "foreground_plan_validation",
                    "media_type",
                    WideToUtf8(asset.runtimePath),
                    "application/vnd.xcp.mesh-v1+json",
                    WideToUtf8(asset.mediaType),
                    "export an xcp-mesh-asset-v1 document and rebuild");
            }

            std::ifstream input(asset.casPath, std::ios::binary);
            auto bytes = std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
            JsonObject object;
            try
            {
                auto value = JsonValue::Parse(Utf8ToWide(bytes));
                if (value.ValueType() != JsonValueType::Object)
                    throw std::runtime_error("root");
                object = value.GetObject();
            }
            catch (...)
            {
                FailForeground(
                    "xcp.creative.mesh_asset_invalid",
                    "creative external mesh is not a UTF-8 JSON object",
                    "foreground_plan_validation",
                    "asset",
                    WideToUtf8(asset.runtimePath),
                    "xcp-mesh-asset-v1 JSON object",
                    "parse failed",
                    "export the mesh with the published schema");
            }
            RequireExactFields(
                object,
                {
                    L"schema_version",
                    L"coordinate_system",
                    L"positions",
                    L"indices"
                },
                WideToUtf8(asset.runtimePath));
            if (RequiredString(
                    object,
                    L"schema_version",
                    WideToUtf8(asset.runtimePath)) !=
                    L"xcp-mesh-asset-v1" ||
                RequiredString(
                    object,
                    L"coordinate_system",
                    WideToUtf8(asset.runtimePath)) !=
                    L"left_handed_y_up")
            {
                FailForeground(
                    "xcp.creative.mesh_asset_schema_mismatch",
                    "creative external mesh schema or coordinate system is unsupported",
                    "foreground_plan_validation",
                    "schema_version/coordinate_system",
                    WideToUtf8(asset.runtimePath),
                    "xcp-mesh-asset-v1 / left_handed_y_up",
                    "other",
                    "export the mesh with the published schema");
            }
            auto positionValues = RequiredArray(
                object,
                L"positions",
                WideToUtf8(asset.runtimePath),
                3,
                MaxMeshVertices);
            std::vector<WorkerCreativeVec3> positions;
            positions.reserve(positionValues.Size());
            for (uint32_t index = 0;
                 index < positionValues.Size();
                 ++index)
            {
                auto value = positionValues.GetAt(index);
                if (value.ValueType() != JsonValueType::Array)
                {
                    FailForeground(
                        "xcp.creative.mesh_asset_invalid",
                        "creative mesh position is not a three-component array",
                        "foreground_plan_validation",
                        "positions",
                        WideToUtf8(asset.runtimePath),
                        "three finite bounded numbers",
                        "non-array",
                        "correct the mesh position");
                }
                auto array = value.GetArray();
                if (array.Size() != 3)
                {
                    FailForeground(
                        "xcp.creative.mesh_asset_invalid",
                        "creative mesh position has invalid arity",
                        "foreground_plan_validation",
                        "positions",
                        WideToUtf8(asset.runtimePath),
                        "exactly three components",
                        std::to_string(array.Size()),
                        "correct the mesh position");
                }
                WorkerCreativeVec3 position;
                float* components[] = {
                    &position.x, &position.y, &position.z
                };
                for (uint32_t component = 0;
                     component < 3;
                     ++component)
                {
                    auto number = array.GetAt(component);
                    if (number.ValueType() !=
                            JsonValueType::Number ||
                        !std::isfinite(number.GetNumber()) ||
                        number.GetNumber() < -1000000.0 ||
                        number.GetNumber() > 1000000.0)
                    {
                        FailForeground(
                            "xcp.creative.mesh_asset_invalid",
                            "creative mesh position component is outside its bound",
                            "foreground_plan_validation",
                            "positions",
                            WideToUtf8(asset.runtimePath),
                            "-1000000..1000000 finite number",
                            "invalid component",
                            "correct the mesh position");
                    }
                    *components[component] =
                        static_cast<float>(
                            number.GetNumber());
                }
                positions.push_back(position);
            }

            auto indexValues = RequiredArray(
                object,
                L"indices",
                WideToUtf8(asset.runtimePath),
                3,
                MaxMeshIndices);
            if (indexValues.Size() % 3 != 0)
            {
                FailForeground(
                    "xcp.creative.mesh_asset_invalid",
                    "creative mesh index count is not a triangle list",
                    "foreground_plan_validation",
                    "indices",
                    WideToUtf8(asset.runtimePath),
                    "a multiple of three",
                    std::to_string(indexValues.Size()),
                    "export a triangle-list mesh");
            }
            std::vector<uint32_t> indices;
            indices.reserve(indexValues.Size());
            for (uint32_t index = 0;
                 index < indexValues.Size();
                 ++index)
            {
                auto value = indexValues.GetAt(index);
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    std::floor(value.GetNumber()) !=
                        value.GetNumber() ||
                    value.GetNumber() < 0 ||
                    value.GetNumber() >= positions.size())
                {
                    FailForeground(
                        "xcp.creative.mesh_asset_index_invalid",
                        "creative mesh index is outside its position array",
                        "foreground_plan_validation",
                        "indices",
                        WideToUtf8(asset.runtimePath),
                        "integer smaller than positions length",
                        "invalid index",
                        "correct the mesh topology");
                }
                indices.push_back(
                    static_cast<uint32_t>(
                        value.GetNumber()));
            }

            std::vector<WorkerCreativeMesh::Triangle> triangles;
            triangles.reserve(indices.size() / 3);
            for (size_t index = 0;
                 index < indices.size();
                 index += 3)
            {
                auto a = positions[indices[index]];
                auto b = positions[indices[index + 1]];
                auto c = positions[indices[index + 2]];
                auto abx = b.x - a.x;
                auto aby = b.y - a.y;
                auto abz = b.z - a.z;
                auto acx = c.x - a.x;
                auto acy = c.y - a.y;
                auto acz = c.z - a.z;
                WorkerCreativeVec3 normal{
                    aby * acz - abz * acy,
                    abz * acx - abx * acz,
                    abx * acy - aby * acx
                };
                auto length = std::sqrt(
                    normal.x * normal.x +
                    normal.y * normal.y +
                    normal.z * normal.z);
                if (!std::isfinite(length) ||
                    length <= 0.000001f)
                {
                    FailForeground(
                        "xcp.creative.mesh_asset_triangle_degenerate",
                        "creative external mesh contains a degenerate triangle",
                        "foreground_plan_validation",
                        "indices",
                        WideToUtf8(asset.runtimePath),
                        "non-degenerate triangle list",
                        std::to_string(index / 3),
                        "remove or repair the degenerate triangle");
                }
                normal.x /= length;
                normal.y /= length;
                normal.z /= length;
                triangles.push_back(
                    { a, b, c, normal });
            }
            return triangles;
        }

        WorkerCreativeTransform2D ParseTransform2D(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(
                object,
                { L"position", L"rotation_degrees", L"scale" },
                path);
            WorkerCreativeTransform2D result;
            result.position = RequiredVec2(
                object,
                L"position",
                path,
                -1000000.0,
                1000000.0);
            result.rotationDegrees = static_cast<float>(
                OptionalNumber(
                    object,
                    L"rotation_degrees",
                    path,
                    0.0,
                    -360000.0,
                    360000.0));
            if (object.HasKey(L"scale"))
            {
                result.scale = RequiredVec2(
                    object,
                    L"scale",
                    path,
                    0.0001,
                    10000.0,
                    true);
            }
            return result;
        }

        WorkerCreativeInputBinding ParseInputBinding(
            JsonObject const& object,
            std::string const& path,
            bool scene)
        {
            RequireExactFields(
                object,
                scene
                    ? std::set<std::wstring>{
                        L"action", L"source", L"target", L"scale" }
                    : std::set<std::wstring>{
                        L"action", L"source", L"target_node", L"scale" },
                path);
            WorkerCreativeInputBinding result;
            result.action = RequiredString(
                object,
                L"action",
                path,
                128);
            result.source = RequiredString(
                object,
                L"source",
                path,
                64);
            result.target = OptionalString(
                object,
                scene ? L"target" : L"target_node",
                path,
                96);
            result.scale = static_cast<float>(
                OptionalNumber(
                    object,
                    L"scale",
                    path,
                    1.0,
                    -10000.0,
                    10000.0));
            RequireIdentifier(result.action, "action", path, true);
            if (!result.target.empty())
            {
                RequireIdentifier(result.target, "target", path);
            }
            return result;
        }

        WorkerCreativeCanvas ParseCanvas(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                { L"schema_version", L"canvas", L"nodes", L"input_bindings" },
                path);
            if (RequiredString(module, L"schema_version", path) !=
                L"xcp-canvas2d-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative canvas module schema version is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-canvas2d-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            auto canvasObject = RequiredObject(module, L"canvas", path);
            RequireExactFields(
                canvasObject,
                { L"width", L"height", L"background" },
                path + ".canvas");
            WorkerCreativeCanvas result;
            result.width = static_cast<uint32_t>(
                RequiredInteger(
                    canvasObject,
                    L"width",
                    path + ".canvas",
                    1,
                    8192));
            result.height = static_cast<uint32_t>(
                RequiredInteger(
                    canvasObject,
                    L"height",
                    path + ".canvas",
                    1,
                    8192));
            result.background = ParseColor(
                RequiredString(
                    canvasObject,
                    L"background",
                    path + ".canvas",
                    64),
                true);
            auto nodes = RequiredArray(
                module,
                L"nodes",
                path,
                1,
                4096);
            std::set<std::wstring> ids;
            for (uint32_t index = 0; index < nodes.Size(); ++index)
            {
                auto object = ArrayObject(nodes, index, path + ".nodes");
                auto itemPath =
                    path + ".nodes[" + std::to_string(index) + "]";
                RequireExactFields(
                    object,
                    {
                        L"id", L"type", L"transform", L"size",
                        L"line_end", L"text", L"font_size", L"asset_id",
                        L"style", L"z_index", L"visible"
                    },
                    itemPath);
                WorkerCreativeCanvasNode node;
                node.id = RequiredString(object, L"id", itemPath, 96);
                node.type = RequiredString(object, L"type", itemPath, 32);
                RequireIdentifier(node.id, "id", itemPath);
                if (!ids.insert(node.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative canvas node id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique node id",
                        WideToUtf8(node.id),
                        "rename the duplicate node");
                }
                if (node.type != L"rectangle" &&
                    node.type != L"ellipse" &&
                    node.type != L"line" &&
                    node.type != L"text" &&
                    node.type != L"sprite")
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative canvas node type is unsupported",
                        "foreground_plan_validation",
                        "type",
                        itemPath,
                        "rectangle, ellipse, line, text or sprite",
                        WideToUtf8(node.type),
                        "use a type published by the canvas schema");
                }
                node.transform = ParseTransform2D(
                    RequiredObject(object, L"transform", itemPath),
                    itemPath + ".transform");
                if (node.type == L"rectangle" ||
                    node.type == L"ellipse" ||
                    node.type == L"sprite")
                {
                    node.size = RequiredVec2(
                        object,
                        L"size",
                        itemPath,
                        0.0,
                        8192.0,
                        true);
                }
                if (node.type == L"line")
                {
                    node.lineEnd = RequiredVec2(
                        object,
                        L"line_end",
                        itemPath,
                        -1000000.0,
                        1000000.0);
                }
                if (node.type == L"text")
                {
                    node.text = RequiredString(
                        object,
                        L"text",
                        itemPath,
                        4096);
                    node.fontSize = static_cast<float>(
                        RequiredNumber(
                            object,
                            L"font_size",
                            itemPath,
                            4.0,
                            512.0));
                }
                if (node.type == L"sprite")
                {
                    node.assetId = RequiredString(
                        object,
                        L"asset_id",
                        itemPath,
                        96);
                    RequireIdentifier(
                        node.assetId,
                        "asset_id",
                        itemPath);
                }
                if (object.HasKey(L"style"))
                {
                    auto style = RequiredObject(
                        object,
                        L"style",
                        itemPath);
                    RequireExactFields(
                        style,
                        {
                            L"fill", L"stroke", L"stroke_width",
                            L"opacity"
                        },
                        itemPath + ".style");
                    if (style.HasKey(L"fill"))
                        node.fill = ParseColor(
                            RequiredString(
                                style,
                                L"fill",
                                itemPath + ".style",
                                64),
                            true);
                    if (style.HasKey(L"stroke"))
                        node.stroke = ParseColor(
                            RequiredString(
                                style,
                                L"stroke",
                                itemPath + ".style",
                                64),
                            true);
                    node.strokeWidth = static_cast<float>(
                        OptionalNumber(
                            style,
                            L"stroke_width",
                            itemPath + ".style",
                            0.0,
                            0.0,
                            256.0));
                    node.opacity = static_cast<float>(
                        OptionalNumber(
                            style,
                            L"opacity",
                            itemPath + ".style",
                            1.0,
                            0.0,
                            1.0));
                }
                node.zIndex = static_cast<int32_t>(
                    object.HasKey(L"z_index")
                        ? RequiredInteger(
                            object,
                            L"z_index",
                            itemPath,
                            -32768,
                            32767)
                        : 0);
                node.visible = OptionalBoolean(
                    object,
                    L"visible",
                    itemPath,
                    true);
                result.nodes.push_back(std::move(node));
            }
            auto bindings = RequiredArray(
                module,
                L"input_bindings",
                path,
                0,
                256);
            for (uint32_t index = 0; index < bindings.Size(); ++index)
            {
                auto binding = ParseInputBinding(
                    ArrayObject(
                        bindings,
                        index,
                        path + ".input_bindings"),
                    path + ".input_bindings[" +
                        std::to_string(index) + "]",
                    false);
                if (!binding.target.empty() &&
                    ids.find(binding.target) == ids.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative canvas input target is missing",
                        "foreground_plan_validation",
                        "target_node",
                        path,
                        "declared canvas node",
                        WideToUtf8(binding.target),
                        "correct the binding target");
                }
                result.inputBindings.push_back(std::move(binding));
            }
            std::stable_sort(
                result.nodes.begin(),
                result.nodes.end(),
                [](auto const& left, auto const& right)
                {
                    return left.zIndex < right.zIndex;
                });
            return result;
        }

        WorkerCreativeUi ParseUi(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                {
                    L"schema_version", L"root_id", L"elements",
                    L"initial_focus_id"
                },
                path);
            if (RequiredString(module, L"schema_version", path) !=
                L"xcp-ui-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative UI module schema version is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-ui-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeUi result;
            result.rootId = RequiredString(module, L"root_id", path, 96);
            result.initialFocusId = RequiredString(
                module,
                L"initial_focus_id",
                path,
                96);
            RequireIdentifier(result.rootId, "root_id", path);
            RequireIdentifier(
                result.initialFocusId,
                "initial_focus_id",
                path);
            auto elements = RequiredArray(
                module,
                L"elements",
                path,
                1,
                1024);
            std::set<std::wstring> ids;
            for (uint32_t index = 0; index < elements.Size(); ++index)
            {
                auto itemPath =
                    path + ".elements[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    elements,
                    index,
                    path + ".elements");
                RequireExactFields(
                    object,
                    {
                        L"id", L"type", L"parent_id", L"text", L"layout",
                        L"focusable", L"action", L"value_state",
                        L"minimum", L"maximum", L"visible_state",
                        L"style_token"
                    },
                    itemPath);
                WorkerCreativeUiElement element;
                element.id = RequiredString(object, L"id", itemPath, 96);
                element.type = RequiredString(
                    object,
                    L"type",
                    itemPath,
                    32);
                element.parentId = OptionalString(
                    object,
                    L"parent_id",
                    itemPath,
                    96);
                element.text = OptionalString(
                    object,
                    L"text",
                    itemPath,
                    4096);
                element.focusable = OptionalBoolean(
                    object,
                    L"focusable",
                    itemPath,
                    false);
                element.action = OptionalString(
                    object,
                    L"action",
                    itemPath,
                    128);
                element.valueState = OptionalString(
                    object,
                    L"value_state",
                    itemPath,
                    96);
                element.visibleState = OptionalString(
                    object,
                    L"visible_state",
                    itemPath,
                    96);
                element.styleToken = OptionalString(
                    object,
                    L"style_token",
                    itemPath,
                    128);
                RequireIdentifier(element.id, "id", itemPath);
                if (!element.parentId.empty())
                    RequireIdentifier(
                        element.parentId,
                        "parent_id",
                        itemPath);
                if (!element.action.empty())
                    RequireIdentifier(
                        element.action,
                        "action",
                        itemPath,
                        true);
                if (!element.valueState.empty())
                    RequireIdentifier(
                        element.valueState,
                        "value_state",
                        itemPath);
                if (!element.visibleState.empty())
                    RequireIdentifier(
                        element.visibleState,
                        "visible_state",
                        itemPath);
                if (!element.styleToken.empty())
                    RequireIdentifier(
                        element.styleToken,
                        "style_token",
                        itemPath,
                        true);
                static std::set<std::wstring> const Types{
                    L"panel", L"label", L"button", L"toggle", L"slider",
                    L"progress", L"list"
                };
                if (Types.find(element.type) == Types.end())
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative UI element type is unsupported",
                        "foreground_plan_validation",
                        "type",
                        itemPath,
                        "published UI element type",
                        WideToUtf8(element.type),
                        "use a type published by the UI schema");
                }
                if (object.HasKey(L"layout"))
                {
                    auto layout = RequiredObject(
                        object,
                        L"layout",
                        itemPath);
                    RequireExactFields(
                        layout,
                        {
                            L"direction", L"width", L"height", L"gap",
                            L"padding", L"align"
                        },
                        itemPath + ".layout");
                    element.direction = OptionalString(
                        layout,
                        L"direction",
                        itemPath + ".layout",
                        16);
                    if (element.direction.empty())
                        element.direction = L"column";
                    element.width = static_cast<float>(
                        OptionalNumber(
                            layout,
                            L"width",
                            itemPath + ".layout",
                            0.0,
                            0.0,
                            8192.0));
                    element.height = static_cast<float>(
                        OptionalNumber(
                            layout,
                            L"height",
                            itemPath + ".layout",
                            0.0,
                            0.0,
                            8192.0));
                    element.gap = static_cast<float>(
                        OptionalNumber(
                            layout,
                            L"gap",
                            itemPath + ".layout",
                            0.0,
                            0.0,
                            512.0));
                    element.padding = static_cast<float>(
                        OptionalNumber(
                            layout,
                            L"padding",
                            itemPath + ".layout",
                            0.0,
                            0.0,
                            1024.0));
                    element.align = OptionalString(
                        layout,
                        L"align",
                        itemPath + ".layout",
                        16);
                    if (element.align.empty())
                        element.align = L"start";
                }
                element.minimum = OptionalNumber(
                    object,
                    L"minimum",
                    itemPath,
                    0.0,
                    -1000000000.0,
                    1000000000.0);
                element.maximum = OptionalNumber(
                    object,
                    L"maximum",
                    itemPath,
                    1.0,
                    -1000000000.0,
                    1000000000.0);
                if (element.maximum < element.minimum)
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative UI range maximum is below minimum",
                        "foreground_plan_validation",
                        "minimum/maximum",
                        itemPath,
                        "maximum greater than or equal to minimum",
                        "inverted range",
                        "correct the UI range");
                }
                if (!ids.insert(element.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative UI element id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique UI element id",
                        WideToUtf8(element.id),
                        "rename the duplicate element");
                }
                result.elements.push_back(std::move(element));
            }
            if (ids.find(result.rootId) == ids.end() ||
                ids.find(result.initialFocusId) == ids.end())
            {
                FailForeground(
                    "xcp.creative.module_reference_invalid",
                    "creative UI root or initial focus target is missing",
                    "foreground_plan_validation",
                    "root_id/initial_focus_id",
                    path,
                    "declared UI element ids",
                    WideToUtf8(
                        result.rootId + L"/" +
                        result.initialFocusId),
                    "correct the UI root and initial focus");
            }
            for (auto const& element : result.elements)
            {
                if (!element.parentId.empty() &&
                    ids.find(element.parentId) == ids.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative UI parent element is missing",
                        "foreground_plan_validation",
                        "parent_id",
                        path,
                        "declared UI element",
                        WideToUtf8(element.parentId),
                        "correct the parent reference");
                }
            }
            auto focused = std::find_if(
                result.elements.begin(),
                result.elements.end(),
                [&](auto const& element)
                {
                    return element.id == result.initialFocusId;
                });
            if (focused == result.elements.end() || !focused->focusable)
            {
                FailForeground(
                    "xcp.creative.module_reference_invalid",
                    "creative UI initial focus target is not focusable",
                    "foreground_plan_validation",
                    "initial_focus_id",
                    path,
                    "declared focusable element",
                    WideToUtf8(result.initialFocusId),
                    "select a focusable initial element");
            }
            return result;
        }

        WorkerCreativeOperand ParseOperand(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(object, { L"state", L"value" }, path);
            auto hasState = object.HasKey(L"state");
            auto hasValue = object.HasKey(L"value");
            if (hasState == hasValue)
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior operand must select exactly one source",
                    "foreground_plan_validation",
                    "state/value",
                    path,
                    "exactly one of state or value",
                    hasState ? "both" : "neither",
                    "correct the behavior operand");
            }
            WorkerCreativeOperand result;
            result.fromState = hasState;
            if (hasState)
            {
                result.state = RequiredString(
                    object,
                    L"state",
                    path,
                    96);
                RequireIdentifier(result.state, "state", path);
            }
            else
            {
                result.value = ParseScalar(
                    object.GetNamedValue(L"value"),
                    path + ".value");
            }
            return result;
        }

        WorkerCreativeCondition ParseCondition(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(
                object,
                { L"left", L"operator", L"right" },
                path);
            WorkerCreativeCondition result;
            result.left = ParseOperand(
                RequiredObject(object, L"left", path),
                path + ".left");
            result.operation = RequiredString(
                object,
                L"operator",
                path,
                32);
            result.right = ParseOperand(
                RequiredObject(object, L"right", path),
                path + ".right");
            static std::set<std::wstring> const Operations{
                L"equal", L"not_equal", L"less", L"less_equal",
                L"greater", L"greater_equal"
            };
            if (Operations.find(result.operation) == Operations.end())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior condition operator is unsupported",
                    "foreground_plan_validation",
                    "operator",
                    path,
                    "published behavior operator",
                    WideToUtf8(result.operation),
                    "use an operator published by the behavior schema");
            }
            return result;
        }

        WorkerCreativeAction ParseAction(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(
                object,
                {
                    L"type", L"target", L"source", L"value", L"event",
                    L"vector", L"message"
                },
                path);
            WorkerCreativeAction result;
            result.type = RequiredString(object, L"type", path, 32);
            result.target = OptionalString(
                object,
                L"target",
                path,
                96);
            result.source = OptionalString(
                object,
                L"source",
                path,
                96);
            result.event = OptionalString(
                object,
                L"event",
                path,
                128);
            result.message = OptionalString(
                object,
                L"message",
                path,
                1024);
            if (!result.target.empty())
                RequireIdentifier(result.target, "target", path);
            if (!result.source.empty())
                RequireIdentifier(result.source, "source", path);
            if (!result.event.empty())
                RequireIdentifier(result.event, "event", path, true);
            if (object.HasKey(L"value"))
            {
                result.value = ParseScalar(
                    object.GetNamedValue(L"value"),
                    path + ".value");
            }
            if (object.HasKey(L"vector"))
            {
                auto vector = RequiredArray(
                    object,
                    L"vector",
                    path,
                    2,
                    3);
                for (uint32_t index = 0; index < vector.Size(); ++index)
                {
                    auto value = vector.GetAt(index);
                    if (value.ValueType() != JsonValueType::Number ||
                        !std::isfinite(value.GetNumber()) ||
                        value.GetNumber() < -1000000.0 ||
                        value.GetNumber() > 1000000.0)
                    {
                        FailForeground(
                            "xcp.creative.module_schema_rejected",
                            "creative behavior action vector is invalid",
                            "foreground_plan_validation",
                            "vector",
                            path,
                            "two or three bounded numbers",
                            "invalid component",
                            "correct the action vector");
                    }
                    result.vector.push_back(value.GetNumber());
                }
            }
            static std::set<std::wstring> const Types{
                L"state.set", L"state.add", L"state.toggle",
                L"event.emit", L"node.visibility", L"node.translate2d",
                L"node.translate3d", L"ui.focus", L"state.save",
                L"state.load", L"log.write"
            };
            if (Types.find(result.type) == Types.end())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior action type is unsupported",
                    "foreground_plan_validation",
                    "type",
                    path,
                    "published deterministic action",
                    WideToUtf8(result.type),
                    "use an action published by the behavior schema");
            }
            auto targetRequired =
                result.type != L"event.emit" &&
                result.type != L"log.write";
            if (targetRequired && result.target.empty())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior action target is required",
                    "foreground_plan_validation",
                    "target",
                    path,
                    "non-empty target",
                    "missing",
                    "add the action target");
            }
            if ((result.type == L"state.set" ||
                    result.type == L"state.add" ||
                    result.type == L"node.visibility") &&
                !result.value.has_value())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior action value is required",
                    "foreground_plan_validation",
                    "value",
                    path,
                    "bounded scalar value",
                    "missing",
                    "add the action value");
            }
            if (result.type == L"event.emit" && result.event.empty())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior emitted event is required",
                    "foreground_plan_validation",
                    "event",
                    path,
                    "contract event id",
                    "missing",
                    "add the emitted event id");
            }
            if ((result.type == L"node.translate2d" &&
                    result.vector.size() != 2) ||
                (result.type == L"node.translate3d" &&
                    result.vector.size() != 3))
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior translation vector has the wrong dimension",
                    "foreground_plan_validation",
                    "vector",
                    path,
                    result.type == L"node.translate2d"
                        ? "two numbers"
                        : "three numbers",
                    std::to_string(result.vector.size()),
                    "correct the translation vector");
            }
            if (result.type == L"log.write" && result.message.empty())
            {
                FailForeground(
                    "xcp.creative.module_schema_rejected",
                    "creative behavior log message is required",
                    "foreground_plan_validation",
                    "message",
                    path,
                    "non-empty bounded message",
                    "missing",
                    "add the log message");
            }
            return result;
        }

        WorkerCreativeBehavior ParseBehavior(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                { L"schema_version", L"state", L"timers", L"rules" },
                path);
            if (RequiredString(module, L"schema_version", path) !=
                L"xcp-behavior-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative behavior module schema version is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-behavior-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeBehavior result;
            std::map<std::wstring, std::wstring> stateTypes;
            auto state = RequiredArray(
                module,
                L"state",
                path,
                0,
                1024);
            for (uint32_t index = 0; index < state.Size(); ++index)
            {
                auto itemPath =
                    path + ".state[" + std::to_string(index) + "]";
                auto object = ArrayObject(state, index, path + ".state");
                RequireExactFields(
                    object,
                    { L"id", L"type", L"initial" },
                    itemPath);
                WorkerCreativeStateDefinition definition;
                definition.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                definition.type = RequiredString(
                    object,
                    L"type",
                    itemPath,
                    16);
                RequireIdentifier(definition.id, "id", itemPath);
                definition.initial = ParseScalar(
                    object.GetNamedValue(L"initial"),
                    itemPath + ".initial");
                if (!ScalarMatchesType(
                        definition.initial,
                        definition.type))
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative behavior initial value does not match its declared type",
                        "foreground_plan_validation",
                        "initial",
                        itemPath,
                        WideToUtf8(definition.type),
                        "type mismatch",
                        "correct the initial state value");
                }
                if (!stateTypes.emplace(
                        definition.id,
                        definition.type).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative behavior state id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique state id",
                        WideToUtf8(definition.id),
                        "rename the duplicate state");
                }
                result.state.push_back(std::move(definition));
            }
            std::set<std::wstring> timerIds;
            auto timers = RequiredArray(
                module,
                L"timers",
                path,
                0,
                256);
            for (uint32_t index = 0; index < timers.Size(); ++index)
            {
                auto itemPath =
                    path + ".timers[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    timers,
                    index,
                    path + ".timers");
                RequireExactFields(
                    object,
                    { L"id", L"interval_ms", L"repeating", L"event" },
                    itemPath);
                WorkerCreativeTimerDefinition timer;
                timer.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                timer.intervalMs = static_cast<uint32_t>(
                    RequiredInteger(
                        object,
                        L"interval_ms",
                        itemPath,
                        1,
                        86400000));
                timer.repeating = RequiredBoolean(
                    object,
                    L"repeating",
                    itemPath);
                timer.event = RequiredString(
                    object,
                    L"event",
                    itemPath,
                    128);
                RequireIdentifier(timer.id, "id", itemPath);
                RequireIdentifier(timer.event, "event", itemPath, true);
                if (!timerIds.insert(timer.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative behavior timer id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique timer id",
                        WideToUtf8(timer.id),
                        "rename the duplicate timer");
                }
                result.timers.push_back(std::move(timer));
            }
            std::set<std::wstring> ruleIds;
            auto rules = RequiredArray(
                module,
                L"rules",
                path,
                0,
                4096);
            for (uint32_t index = 0; index < rules.Size(); ++index)
            {
                auto itemPath =
                    path + ".rules[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    rules,
                    index,
                    path + ".rules");
                RequireExactFields(
                    object,
                    { L"id", L"event", L"conditions", L"actions" },
                    itemPath);
                WorkerCreativeRule rule;
                rule.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                rule.event = RequiredString(
                    object,
                    L"event",
                    itemPath,
                    128);
                RequireIdentifier(rule.id, "id", itemPath);
                RequireIdentifier(rule.event, "event", itemPath, true);
                if (!ruleIds.insert(rule.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative behavior rule id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique rule id",
                        WideToUtf8(rule.id),
                        "rename the duplicate rule");
                }
                auto conditions = RequiredArray(
                    object,
                    L"conditions",
                    itemPath,
                    0,
                    32);
                for (uint32_t condition = 0;
                     condition < conditions.Size();
                     ++condition)
                {
                    rule.conditions.push_back(
                        ParseCondition(
                            ArrayObject(
                                conditions,
                                condition,
                                itemPath + ".conditions"),
                            itemPath + ".conditions[" +
                                std::to_string(condition) + "]"));
                }
                auto actions = RequiredArray(
                    object,
                    L"actions",
                    itemPath,
                    1,
                    64);
                for (uint32_t action = 0;
                     action < actions.Size();
                     ++action)
                {
                    rule.actions.push_back(
                        ParseAction(
                            ArrayObject(
                                actions,
                                action,
                                itemPath + ".actions"),
                            itemPath + ".actions[" +
                                std::to_string(action) + "]"));
                }
                result.rules.push_back(std::move(rule));
            }
            for (auto const& rule : result.rules)
            {
                for (auto const& condition : rule.conditions)
                {
                    for (auto const* operand :
                         { &condition.left, &condition.right })
                    {
                        if (operand->fromState &&
                            stateTypes.find(operand->state) ==
                                stateTypes.end())
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative behavior condition references missing state",
                                "foreground_plan_validation",
                                "state",
                                path,
                                "declared behavior state",
                                WideToUtf8(operand->state),
                                "correct the condition state reference");
                        }
                    }
                }
                for (auto const& action : rule.actions)
                {
                    if ((action.type == L"state.set" ||
                            action.type == L"state.add" ||
                            action.type == L"state.toggle") &&
                        stateTypes.find(action.target) ==
                            stateTypes.end())
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative behavior action references missing state",
                            "foreground_plan_validation",
                            "target",
                            path,
                            "declared behavior state",
                            WideToUtf8(action.target),
                            "correct the action target");
                    }
                    auto targetType = stateTypes.find(action.target);
                    if (action.type == L"state.set" &&
                        (!action.value.has_value() ||
                         targetType == stateTypes.end() ||
                         !ScalarMatchesType(
                             *action.value,
                             targetType->second)))
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative state.set value does not match its target type",
                            "foreground_plan_validation",
                            "value",
                            path,
                            targetType == stateTypes.end()
                                ? "declared behavior state"
                                : WideToUtf8(targetType->second),
                            "missing or type mismatch",
                            "correct the state.set value");
                    }
                    if (action.type == L"state.add" &&
                        (targetType == stateTypes.end() ||
                         (targetType->second != L"integer" &&
                          targetType->second != L"number") ||
                         !action.value.has_value() ||
                         (!std::holds_alternative<int64_t>(
                              *action.value) &&
                          !std::holds_alternative<double>(
                              *action.value))))
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative state.add requires numeric source and target",
                            "foreground_plan_validation",
                            "value",
                            path,
                            "numeric behavior state and value",
                            "non-numeric or missing",
                            "correct the state.add action");
                    }
                    if (action.type == L"state.toggle" &&
                        (targetType == stateTypes.end() ||
                         targetType->second != L"boolean"))
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative state.toggle target is not boolean",
                            "foreground_plan_validation",
                            "target",
                            path,
                            "declared boolean behavior state",
                            WideToUtf8(action.target),
                            "correct the state.toggle action");
                    }
                    if (action.type == L"node.visibility" &&
                        (!action.value.has_value() ||
                         !std::holds_alternative<bool>(*action.value)))
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative node.visibility value is not boolean",
                            "foreground_plan_validation",
                            "value",
                            path,
                            "boolean",
                            "missing or other type",
                            "correct the node.visibility action");
                    }
                }
            }
            return result;
        }

        WorkerCreativeData ParseData(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                { L"schema_version", L"constants", L"local_state" },
                path);
            if (RequiredString(module, L"schema_version", path) !=
                L"xcp-data-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative data module schema version is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-data-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeData result;
            std::set<std::wstring> ids;
            auto constants = RequiredArray(
                module,
                L"constants",
                path,
                0,
                4096);
            for (uint32_t index = 0; index < constants.Size(); ++index)
            {
                auto itemPath =
                    path + ".constants[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    constants,
                    index,
                    path + ".constants");
                RequireExactFields(object, { L"id", L"value" }, itemPath);
                auto id = RequiredString(object, L"id", itemPath, 96);
                RequireIdentifier(id, "id", itemPath);
                if (!ids.insert(id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative data id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique data id",
                        WideToUtf8(id),
                        "rename the duplicate data entry");
                }
                result.constants.emplace(
                    id,
                    ParseScalar(
                        object.GetNamedValue(L"value"),
                        itemPath + ".value",
                        16384));
            }
            uint64_t declaredBytes = 0;
            auto local = RequiredArray(
                module,
                L"local_state",
                path,
                0,
                1024);
            for (uint32_t index = 0; index < local.Size(); ++index)
            {
                auto itemPath =
                    path + ".local_state[" +
                    std::to_string(index) + "]";
                auto object = ArrayObject(
                    local,
                    index,
                    path + ".local_state");
                RequireExactFields(
                    object,
                    {
                        L"id", L"type", L"default", L"max_bytes",
                        L"persistence"
                    },
                    itemPath);
                WorkerCreativeLocalStateDefinition definition;
                definition.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                definition.type = RequiredString(
                    object,
                    L"type",
                    itemPath,
                    16);
                definition.defaultValue = ParseScalar(
                    object.GetNamedValue(L"default"),
                    itemPath + ".default",
                    16384);
                definition.maxBytes = static_cast<uint32_t>(
                    RequiredInteger(
                        object,
                        L"max_bytes",
                        itemPath,
                        1,
                        1048576));
                auto persistence = RequiredString(
                    object,
                    L"persistence",
                    itemPath,
                    32);
                RequireIdentifier(definition.id, "id", itemPath);
                if (persistence != L"project_local" ||
                    !ScalarMatchesType(
                        definition.defaultValue,
                        definition.type))
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative local state declaration is inconsistent",
                        "foreground_plan_validation",
                        "type/default/persistence",
                        itemPath,
                        "matching scalar type and project_local persistence",
                        "declaration mismatch",
                        "correct the local state declaration");
                }
                if (!ids.insert(definition.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative data id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique data id",
                        WideToUtf8(definition.id),
                        "rename the duplicate data entry");
                }
                declaredBytes += definition.maxBytes;
                if (declaredBytes > MaxLocalStateBytes)
                {
                    FailForeground(
                        "xcp.creative.local_state_budget_exceeded",
                        "creative project declared local state exceeds the foreground budget",
                        "foreground_plan_validation",
                        "max_bytes",
                        path,
                        "at most 8388608 declared bytes",
                        std::to_string(declaredBytes),
                        "reduce declared local state");
                }
                result.localState.push_back(std::move(definition));
            }
            return result;
        }

        WorkerCreativeTransform3D ParseTransform3D(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(
                object,
                { L"position", L"rotation_degrees", L"scale" },
                path);
            WorkerCreativeTransform3D result;
            result.position = RequiredVec3(
                object,
                L"position",
                path,
                -1000000.0,
                1000000.0);
            result.rotationDegrees = RequiredVec3(
                object,
                L"rotation_degrees",
                path,
                -360000.0,
                360000.0);
            result.scale = RequiredVec3(
                object,
                L"scale",
                path,
                0.0001,
                1000000.0,
                true);
            return result;
        }

        WorkerCreativeScene3D ParseScene3D(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                {
                    L"schema_version", L"coordinate_system", L"materials",
                    L"meshes", L"nodes", L"cameras", L"active_camera",
                    L"lights", L"input_bindings", L"animations"
                },
                path);
            if (RequiredString(module, L"schema_version", path) !=
                    L"xcp-scene3d-module-v1" ||
                RequiredString(
                    module,
                    L"coordinate_system",
                    path) != L"left_handed_y_up")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative 3D module schema or coordinate system is unsupported",
                    "foreground_plan_validation",
                    "schema_version/coordinate_system",
                    path,
                    "xcp-scene3d-module-v1 / left_handed_y_up",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeScene3D result;
            std::set<std::wstring> materialIds;
            auto materials = RequiredArray(
                module,
                L"materials",
                path,
                0,
                1024);
            for (uint32_t index = 0; index < materials.Size(); ++index)
            {
                auto itemPath =
                    path + ".materials[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    materials,
                    index,
                    path + ".materials");
                RequireExactFields(
                    object,
                    {
                        L"id", L"base_color", L"roughness", L"metallic",
                        L"emissive"
                    },
                    itemPath);
                WorkerCreativeMaterial material;
                material.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                RequireIdentifier(material.id, "id", itemPath);
                material.baseColor = ParseColor(
                    RequiredString(
                        object,
                        L"base_color",
                        itemPath,
                        9),
                    false);
                material.roughness = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"roughness",
                        itemPath,
                        0.0,
                        1.0));
                material.metallic = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"metallic",
                        itemPath,
                        0.0,
                        1.0));
                if (object.HasKey(L"emissive"))
                {
                    material.emissive = ParseColor(
                        RequiredString(
                            object,
                            L"emissive",
                            itemPath,
                            9),
                        false);
                }
                if (!materialIds.insert(material.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative material id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique material id",
                        WideToUtf8(material.id),
                        "rename the duplicate material");
                }
                result.materials.push_back(std::move(material));
            }

            std::set<std::wstring> meshIds;
            auto meshes = RequiredArray(
                module,
                L"meshes",
                path,
                0,
                2048);
            for (uint32_t index = 0; index < meshes.Size(); ++index)
            {
                auto itemPath =
                    path + ".meshes[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    meshes,
                    index,
                    path + ".meshes");
                RequireExactFields(object, { L"id", L"source" }, itemPath);
                WorkerCreativeMesh mesh;
                mesh.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                RequireIdentifier(mesh.id, "id", itemPath);
                auto source = RequiredObject(
                    object,
                    L"source",
                    itemPath);
                RequireExactFields(
                    source,
                    { L"primitive", L"asset_id" },
                    itemPath + ".source");
                auto hasPrimitive = source.HasKey(L"primitive");
                auto hasAsset = source.HasKey(L"asset_id");
                if (hasPrimitive == hasAsset)
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative mesh source must select exactly one source",
                        "foreground_plan_validation",
                        "source",
                        itemPath,
                        "exactly one primitive or asset_id",
                        hasPrimitive ? "both" : "neither",
                        "correct the mesh source");
                }
                if (hasPrimitive)
                {
                    mesh.primitive = RequiredString(
                        source,
                        L"primitive",
                        itemPath,
                        16);
                    static std::set<std::wstring> const Primitives{
                        L"cube", L"sphere", L"plane", L"cylinder",
                        L"capsule"
                    };
                    if (Primitives.find(mesh.primitive) ==
                        Primitives.end())
                    {
                        FailForeground(
                            "xcp.creative.module_schema_rejected",
                            "creative primitive mesh is unsupported",
                            "foreground_plan_validation",
                            "primitive",
                            itemPath,
                            "published primitive",
                            WideToUtf8(mesh.primitive),
                            "use a primitive published by the 3D schema");
                    }
                }
                else
                {
                    mesh.assetId = RequiredString(
                        source,
                        L"asset_id",
                        itemPath,
                        96);
                    RequireIdentifier(
                        mesh.assetId,
                        "asset_id",
                        itemPath);
                }
                if (!meshIds.insert(mesh.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative mesh id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique mesh id",
                        WideToUtf8(mesh.id),
                        "rename the duplicate mesh");
                }
                result.meshes.push_back(std::move(mesh));
            }

            std::set<std::wstring> nodeIds;
            auto nodes = RequiredArray(
                module,
                L"nodes",
                path,
                1,
                4096);
            for (uint32_t index = 0; index < nodes.Size(); ++index)
            {
                auto itemPath =
                    path + ".nodes[" + std::to_string(index) + "]";
                auto object = ArrayObject(nodes, index, path + ".nodes");
                RequireExactFields(
                    object,
                    {
                        L"id", L"parent_id", L"mesh_id", L"material_id",
                        L"transform", L"collider", L"visible"
                    },
                    itemPath);
                WorkerCreativeSceneNode node;
                node.id = RequiredString(object, L"id", itemPath, 96);
                node.parentId = OptionalString(
                    object,
                    L"parent_id",
                    itemPath,
                    96);
                node.meshId = OptionalString(
                    object,
                    L"mesh_id",
                    itemPath,
                    96);
                node.materialId = OptionalString(
                    object,
                    L"material_id",
                    itemPath,
                    96);
                RequireIdentifier(node.id, "id", itemPath);
                if (!node.parentId.empty())
                    RequireIdentifier(
                        node.parentId,
                        "parent_id",
                        itemPath);
                if (!node.meshId.empty())
                    RequireIdentifier(
                        node.meshId,
                        "mesh_id",
                        itemPath);
                if (!node.materialId.empty())
                    RequireIdentifier(
                        node.materialId,
                        "material_id",
                        itemPath);
                node.transform = ParseTransform3D(
                    RequiredObject(object, L"transform", itemPath),
                    itemPath + ".transform");
                node.visible = OptionalBoolean(
                    object,
                    L"visible",
                    itemPath,
                    true);
                if (object.HasKey(L"collider"))
                {
                    auto colliderObject = RequiredObject(
                        object,
                        L"collider",
                        itemPath);
                    RequireExactFields(
                        colliderObject,
                        {
                            L"shape", L"size", L"radius", L"height",
                            L"is_trigger"
                        },
                        itemPath + ".collider");
                    WorkerCreativeCollider collider;
                    collider.shape = RequiredString(
                        colliderObject,
                        L"shape",
                        itemPath,
                        16);
                    collider.isTrigger = RequiredBoolean(
                        colliderObject,
                        L"is_trigger",
                        itemPath);
                    if (collider.shape == L"box")
                    {
                        collider.size = RequiredVec3(
                            colliderObject,
                            L"size",
                            itemPath,
                            0.0,
                            1000000.0,
                            true);
                    }
                    else if (collider.shape == L"sphere")
                    {
                        collider.radius = static_cast<float>(
                            RequiredNumber(
                                colliderObject,
                                L"radius",
                                itemPath,
                                0.0,
                                1000000.0,
                                true));
                    }
                    else if (collider.shape == L"capsule")
                    {
                        collider.radius = static_cast<float>(
                            RequiredNumber(
                                colliderObject,
                                L"radius",
                                itemPath,
                                0.0,
                                1000000.0,
                                true));
                        collider.height = static_cast<float>(
                            RequiredNumber(
                                colliderObject,
                                L"height",
                                itemPath,
                                0.0,
                                1000000.0,
                                true));
                    }
                    else
                    {
                        FailForeground(
                            "xcp.creative.module_schema_rejected",
                            "creative collider shape is unsupported",
                            "foreground_plan_validation",
                            "shape",
                            itemPath,
                            "box, sphere or capsule",
                            WideToUtf8(collider.shape),
                            "use a collider published by the 3D schema");
                    }
                    node.collider = collider;
                }
                if (!nodeIds.insert(node.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative scene node id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique scene node id",
                        WideToUtf8(node.id),
                        "rename the duplicate node");
                }
                result.nodes.push_back(std::move(node));
            }

            std::set<std::wstring> cameraIds;
            auto cameras = RequiredArray(
                module,
                L"cameras",
                path,
                1,
                16);
            for (uint32_t index = 0; index < cameras.Size(); ++index)
            {
                auto itemPath =
                    path + ".cameras[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    cameras,
                    index,
                    path + ".cameras");
                RequireExactFields(
                    object,
                    {
                        L"id", L"transform", L"field_of_view_degrees",
                        L"near_plane", L"far_plane"
                    },
                    itemPath);
                WorkerCreativeCamera camera;
                camera.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                RequireIdentifier(camera.id, "id", itemPath);
                camera.transform = ParseTransform3D(
                    RequiredObject(object, L"transform", itemPath),
                    itemPath + ".transform");
                camera.fieldOfViewDegrees = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"field_of_view_degrees",
                        itemPath,
                        10.0,
                        140.0));
                camera.nearPlane = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"near_plane",
                        itemPath,
                        0.0,
                        1000.0,
                        true));
                camera.farPlane = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"far_plane",
                        itemPath,
                        0.0,
                        1000000.0,
                        true));
                if (camera.farPlane <= camera.nearPlane)
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative camera far plane must exceed its near plane",
                        "foreground_plan_validation",
                        "near_plane/far_plane",
                        itemPath,
                        "far plane greater than near plane",
                        "invalid range",
                        "correct the camera clipping range");
                }
                if (!cameraIds.insert(camera.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative camera id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique camera id",
                        WideToUtf8(camera.id),
                        "rename the duplicate camera");
                }
                result.cameras.push_back(std::move(camera));
            }
            result.activeCamera = RequiredString(
                module,
                L"active_camera",
                path,
                96);
            RequireIdentifier(
                result.activeCamera,
                "active_camera",
                path);
            if (cameraIds.find(result.activeCamera) == cameraIds.end())
            {
                FailForeground(
                    "xcp.creative.module_reference_invalid",
                    "creative active camera is missing",
                    "foreground_plan_validation",
                    "active_camera",
                    path,
                    "declared camera",
                    WideToUtf8(result.activeCamera),
                    "select a declared camera");
            }

            std::set<std::wstring> lightIds;
            auto lights = RequiredArray(
                module,
                L"lights",
                path,
                0,
                64);
            for (uint32_t index = 0; index < lights.Size(); ++index)
            {
                auto itemPath =
                    path + ".lights[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    lights,
                    index,
                    path + ".lights");
                RequireExactFields(
                    object,
                    {
                        L"id", L"type", L"color", L"intensity",
                        L"position", L"direction", L"range"
                    },
                    itemPath);
                WorkerCreativeLight light;
                light.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                light.type = RequiredString(
                    object,
                    L"type",
                    itemPath,
                    16);
                RequireIdentifier(light.id, "id", itemPath);
                light.color = ParseColor(
                    RequiredString(
                        object,
                        L"color",
                        itemPath,
                        9),
                    false);
                light.intensity = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"intensity",
                        itemPath,
                        0.0,
                        1000000.0));
                if (light.type == L"directional")
                {
                    light.direction = RequiredVec3(
                        object,
                        L"direction",
                        itemPath,
                        -1000000.0,
                        1000000.0);
                }
                else if (light.type == L"point")
                {
                    light.position = RequiredVec3(
                        object,
                        L"position",
                        itemPath,
                        -1000000.0,
                        1000000.0);
                    light.range = static_cast<float>(
                        RequiredNumber(
                            object,
                            L"range",
                            itemPath,
                            0.0,
                            1000000.0,
                            true));
                }
                else
                {
                    FailForeground(
                        "xcp.creative.module_schema_rejected",
                        "creative light type is unsupported",
                        "foreground_plan_validation",
                        "type",
                        itemPath,
                        "directional or point",
                        WideToUtf8(light.type),
                        "use a light type published by the 3D schema");
                }
                if (!lightIds.insert(light.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative light id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique light id",
                        WideToUtf8(light.id),
                        "rename the duplicate light");
                }
                result.lights.push_back(std::move(light));
            }

            for (auto const& node : result.nodes)
            {
                if (!node.parentId.empty() &&
                    (node.parentId == node.id ||
                        nodeIds.find(node.parentId) == nodeIds.end()))
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative scene parent is missing or self-referential",
                        "foreground_plan_validation",
                        "parent_id",
                        path,
                        "different declared scene node",
                        WideToUtf8(node.parentId),
                        "correct the node parent");
                }
                if (!node.meshId.empty() &&
                    meshIds.find(node.meshId) == meshIds.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative scene mesh reference is missing",
                        "foreground_plan_validation",
                        "mesh_id",
                        path,
                        "declared mesh",
                        WideToUtf8(node.meshId),
                        "correct the node mesh");
                }
                if (!node.materialId.empty() &&
                    materialIds.find(node.materialId) ==
                        materialIds.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative scene material reference is missing",
                        "foreground_plan_validation",
                        "material_id",
                        path,
                        "declared material",
                        WideToUtf8(node.materialId),
                        "correct the node material");
                }
            }
            std::map<std::wstring, std::wstring> parents;
            for (auto const& node : result.nodes)
                parents[node.id] = node.parentId;
            for (auto const& node : result.nodes)
            {
                std::set<std::wstring> chain;
                auto current = node.id;
                while (!current.empty())
                {
                    if (!chain.insert(current).second)
                    {
                        FailForeground(
                            "xcp.creative.module_dependency_cycle",
                            "creative scene parent graph contains a cycle",
                            "foreground_plan_validation",
                            "parent_id",
                            path,
                            "acyclic scene graph",
                            WideToUtf8(current),
                            "remove the parent cycle");
                    }
                    current = parents[current];
                }
            }

            auto bindings = RequiredArray(
                module,
                L"input_bindings",
                path,
                0,
                256);
            for (uint32_t index = 0; index < bindings.Size(); ++index)
            {
                auto binding = ParseInputBinding(
                    ArrayObject(
                        bindings,
                        index,
                        path + ".input_bindings"),
                    path + ".input_bindings[" +
                        std::to_string(index) + "]",
                    true);
                if (nodeIds.find(binding.target) == nodeIds.end() &&
                    cameraIds.find(binding.target) == cameraIds.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative 3D input target is missing",
                        "foreground_plan_validation",
                        "target",
                        path,
                        "declared scene node or camera",
                        WideToUtf8(binding.target),
                        "correct the binding target");
                }
                result.inputBindings.push_back(std::move(binding));
            }

            std::set<std::wstring> animationIds;
            auto animations = RequiredArray(
                module,
                L"animations",
                path,
                0,
                MaxSceneAnimations);
            for (uint32_t index = 0;
                 index < animations.Size();
                 ++index)
            {
                auto itemPath =
                    path + ".animations[" +
                    std::to_string(index) + "]";
                auto object = ArrayObject(
                    animations,
                    index,
                    path + ".animations");
                RequireExactFields(
                    object,
                    {
                        L"id", L"target_node", L"from", L"to",
                        L"duration_ms", L"loop", L"autoplay",
                        L"start_event", L"complete_event"
                    },
                    itemPath);
                WorkerCreativeSceneAnimation animation;
                animation.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                animation.targetNode = RequiredString(
                    object,
                    L"target_node",
                    itemPath,
                    96);
                RequireIdentifier(animation.id, "id", itemPath);
                RequireIdentifier(
                    animation.targetNode,
                    "target_node",
                    itemPath);
                animation.from = RequiredVec3(
                    object,
                    L"from",
                    itemPath,
                    -1000000.0,
                    1000000.0);
                animation.to = RequiredVec3(
                    object,
                    L"to",
                    itemPath,
                    -1000000.0,
                    1000000.0);
                animation.durationMs = static_cast<uint32_t>(
                    RequiredInteger(
                        object,
                        L"duration_ms",
                        itemPath,
                        1,
                        3600000));
                animation.loop = RequiredBoolean(
                    object,
                    L"loop",
                    itemPath);
                animation.autoplay = RequiredBoolean(
                    object,
                    L"autoplay",
                    itemPath);
                animation.startEvent = OptionalString(
                    object,
                    L"start_event",
                    itemPath,
                    128);
                animation.completeEvent = OptionalString(
                    object,
                    L"complete_event",
                    itemPath,
                    128);
                if (!animation.startEvent.empty())
                    RequireIdentifier(
                        animation.startEvent,
                        "start_event",
                        itemPath,
                        true);
                if (!animation.completeEvent.empty())
                    RequireIdentifier(
                        animation.completeEvent,
                        "complete_event",
                        itemPath,
                        true);
                if (nodeIds.find(animation.targetNode) ==
                        nodeIds.end() ||
                    !animationIds.insert(animation.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative 3D animation target is missing or its id is duplicated",
                        "foreground_plan_validation",
                        "id/target_node",
                        itemPath,
                        "unique animation id and declared scene node",
                        WideToUtf8(
                            animation.id + L"/" +
                            animation.targetNode),
                        "correct the animation binding");
                }
                result.animations.push_back(
                    std::move(animation));
            }
            return result;
        }

        WorkerCreativeAudio ParseAudio(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                { L"schema_version", L"buses", L"clips", L"cues" },
                path);
            if (RequiredString(
                    module,
                    L"schema_version",
                    path) != L"xcp-audio-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative audio module schema is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-audio-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeAudio result;
            std::set<std::wstring> busIds;
            auto buses = RequiredArray(
                module,
                L"buses",
                path,
                1,
                16);
            for (uint32_t index = 0; index < buses.Size(); ++index)
            {
                auto itemPath =
                    path + ".buses[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    buses,
                    index,
                    path + ".buses");
                RequireExactFields(
                    object,
                    { L"id", L"gain" },
                    itemPath);
                WorkerCreativeAudioBus bus;
                bus.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                RequireIdentifier(bus.id, "id", itemPath);
                bus.gain = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"gain",
                        itemPath,
                        0.0,
                        4.0));
                if (!busIds.insert(bus.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative audio bus id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique bus id",
                        WideToUtf8(bus.id),
                        "rename the duplicate bus");
                }
                result.buses.push_back(std::move(bus));
            }

            std::set<std::wstring> clipIds;
            auto clips = RequiredArray(
                module,
                L"clips",
                path,
                1,
                MaxAudioClips);
            for (uint32_t index = 0; index < clips.Size(); ++index)
            {
                auto itemPath =
                    path + ".clips[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    clips,
                    index,
                    path + ".clips");
                RequireExactFields(
                    object,
                    {
                        L"id", L"asset_id", L"bus_id", L"gain",
                        L"loop"
                    },
                    itemPath);
                WorkerCreativeAudioClip clip;
                clip.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                clip.assetId = RequiredString(
                    object,
                    L"asset_id",
                    itemPath,
                    96);
                clip.busId = RequiredString(
                    object,
                    L"bus_id",
                    itemPath,
                    96);
                RequireIdentifier(clip.id, "id", itemPath);
                RequireIdentifier(
                    clip.assetId,
                    "asset_id",
                    itemPath);
                RequireIdentifier(
                    clip.busId,
                    "bus_id",
                    itemPath);
                clip.gain = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"gain",
                        itemPath,
                        0.0,
                        4.0));
                clip.loop = RequiredBoolean(
                    object,
                    L"loop",
                    itemPath);
                if (!clipIds.insert(clip.id).second ||
                    busIds.find(clip.busId) == busIds.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative audio clip id is duplicated or its bus is missing",
                        "foreground_plan_validation",
                        "id/bus_id",
                        itemPath,
                        "unique clip id and declared bus",
                        WideToUtf8(
                            clip.id + L"/" + clip.busId),
                        "correct the audio clip binding");
                }
                result.clips.push_back(std::move(clip));
            }

            std::set<std::wstring> cueIds;
            auto cues = RequiredArray(
                module,
                L"cues",
                path,
                1,
                256);
            for (uint32_t index = 0; index < cues.Size(); ++index)
            {
                auto itemPath =
                    path + ".cues[" + std::to_string(index) + "]";
                auto object = ArrayObject(
                    cues,
                    index,
                    path + ".cues");
                RequireExactFields(
                    object,
                    { L"id", L"event", L"clip_id", L"gain" },
                    itemPath);
                WorkerCreativeAudioCue cue;
                cue.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                cue.event = RequiredString(
                    object,
                    L"event",
                    itemPath,
                    128);
                cue.clipId = RequiredString(
                    object,
                    L"clip_id",
                    itemPath,
                    96);
                RequireIdentifier(cue.id, "id", itemPath);
                RequireIdentifier(
                    cue.event,
                    "event",
                    itemPath,
                    true);
                RequireIdentifier(
                    cue.clipId,
                    "clip_id",
                    itemPath);
                cue.gain = static_cast<float>(
                    RequiredNumber(
                        object,
                        L"gain",
                        itemPath,
                        0.0,
                        4.0));
                if (!cueIds.insert(cue.id).second ||
                    clipIds.find(cue.clipId) == clipIds.end())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "creative audio cue id is duplicated or its clip is missing",
                        "foreground_plan_validation",
                        "id/clip_id",
                        itemPath,
                        "unique cue id and declared clip",
                        WideToUtf8(
                            cue.id + L"/" + cue.clipId),
                        "correct the audio cue binding");
                }
                result.cues.push_back(std::move(cue));
            }
            return result;
        }

        WorkerCreativeXvm ParseXvm(
            JsonObject const& module,
            std::string const& path)
        {
            RequireExactFields(
                module,
                { L"schema_version", L"services" },
                path);
            if (RequiredString(
                    module,
                    L"schema_version",
                    path) != L"xcp-xvm-module-v1")
            {
                FailForeground(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative XVM module schema is unsupported",
                    "foreground_plan_validation",
                    "schema_version",
                    path,
                    "xcp-xvm-module-v1",
                    "other",
                    "use the schema published by describe_creative_host");
            }
            WorkerCreativeXvm result;
            std::set<std::wstring> serviceIds;
            auto services = RequiredArray(
                module,
                L"services",
                path,
                1,
                MaxXvmServices);
            for (uint32_t index = 0;
                 index < services.Size();
                 ++index)
            {
                auto itemPath =
                    path + ".services[" +
                    std::to_string(index) + "]";
                auto object = ArrayObject(
                    services,
                    index,
                    path + ".services");
                RequireExactFields(
                    object,
                    {
                        L"id", L"trigger_event",
                        L"program_asset_id", L"inputs", L"outputs",
                        L"success_event", L"failure_event"
                    },
                    itemPath);
                WorkerCreativeXvmService service;
                service.id = RequiredString(
                    object,
                    L"id",
                    itemPath,
                    96);
                service.triggerEvent = RequiredString(
                    object,
                    L"trigger_event",
                    itemPath,
                    128);
                service.programAssetId = RequiredString(
                    object,
                    L"program_asset_id",
                    itemPath,
                    96);
                service.successEvent = RequiredString(
                    object,
                    L"success_event",
                    itemPath,
                    128);
                service.failureEvent = RequiredString(
                    object,
                    L"failure_event",
                    itemPath,
                    128);
                RequireIdentifier(service.id, "id", itemPath);
                RequireIdentifier(
                    service.triggerEvent,
                    "trigger_event",
                    itemPath,
                    true);
                RequireIdentifier(
                    service.programAssetId,
                    "program_asset_id",
                    itemPath);
                RequireIdentifier(
                    service.successEvent,
                    "success_event",
                    itemPath,
                    true);
                RequireIdentifier(
                    service.failureEvent,
                    "failure_event",
                    itemPath,
                    true);
                if (!serviceIds.insert(service.id).second)
                {
                    FailForeground(
                        "xcp.creative.module_identity_duplicate",
                        "creative XVM service id is duplicated",
                        "foreground_plan_validation",
                        "id",
                        itemPath,
                        "unique service id",
                        WideToUtf8(service.id),
                        "rename the duplicate service");
                }

                auto inputs = RequiredArray(
                    object,
                    L"inputs",
                    itemPath,
                    11,
                    11);
                for (uint32_t inputIndex = 0;
                     inputIndex < inputs.Size();
                     ++inputIndex)
                {
                    auto inputPath =
                        itemPath + ".inputs[" +
                        std::to_string(inputIndex) + "]";
                    auto inputObject = ArrayObject(
                        inputs,
                        inputIndex,
                        itemPath + ".inputs");
                    RequireExactFields(
                        inputObject,
                        { L"kind", L"state_id", L"value" },
                        inputPath);
                    auto kind = RequiredString(
                        inputObject,
                        L"kind",
                        inputPath,
                        16);
                    WorkerCreativeXvmInput input;
                    if (kind == L"state")
                    {
                        if (inputObject.HasKey(L"value"))
                            FailForeground(
                                "xcp.creative.module_schema_rejected",
                                "creative XVM state input also declares a constant",
                                "foreground_plan_validation",
                                "inputs",
                                inputPath,
                                "state_id only",
                                "value also present",
                                "remove the constant value");
                        input.fromState = true;
                        input.stateId = RequiredString(
                            inputObject,
                            L"state_id",
                            inputPath,
                            96);
                        RequireIdentifier(
                            input.stateId,
                            "state_id",
                            inputPath);
                    }
                    else if (kind == L"constant")
                    {
                        if (inputObject.HasKey(L"state_id"))
                            FailForeground(
                                "xcp.creative.module_schema_rejected",
                                "creative XVM constant input also declares state",
                                "foreground_plan_validation",
                                "inputs",
                                inputPath,
                                "value only",
                                "state_id also present",
                                "remove the state binding");
                        input.constant = static_cast<uint32_t>(
                            RequiredInteger(
                                inputObject,
                                L"value",
                                inputPath,
                                0,
                                4294967295ll));
                    }
                    else
                    {
                        FailForeground(
                            "xcp.creative.module_schema_rejected",
                            "creative XVM input kind is unsupported",
                            "foreground_plan_validation",
                            "kind",
                            inputPath,
                            "state or constant",
                            WideToUtf8(kind),
                            "use a published input source");
                    }
                    service.inputs.push_back(std::move(input));
                }

                std::set<uint32_t> outputOffsets;
                std::set<std::wstring> outputStates;
                auto outputs = RequiredArray(
                    object,
                    L"outputs",
                    itemPath,
                    1,
                    64);
                for (uint32_t outputIndex = 0;
                     outputIndex < outputs.Size();
                     ++outputIndex)
                {
                    auto outputPath =
                        itemPath + ".outputs[" +
                        std::to_string(outputIndex) + "]";
                    auto outputObject = ArrayObject(
                        outputs,
                        outputIndex,
                        itemPath + ".outputs");
                    RequireExactFields(
                        outputObject,
                        { L"offset_bytes", L"state_id" },
                        outputPath);
                    WorkerCreativeXvmOutput output;
                    output.offsetBytes = static_cast<uint32_t>(
                        RequiredInteger(
                            outputObject,
                            L"offset_bytes",
                            outputPath,
                            0,
                            1048572));
                    output.stateId = RequiredString(
                        outputObject,
                        L"state_id",
                        outputPath,
                        96);
                    RequireIdentifier(
                        output.stateId,
                        "state_id",
                        outputPath);
                    if ((output.offsetBytes % 4) != 0 ||
                        !outputOffsets.insert(
                            output.offsetBytes).second ||
                        !outputStates.insert(
                            output.stateId).second)
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative XVM output binding is unaligned or duplicated",
                            "foreground_plan_validation",
                            "offset_bytes/state_id",
                            outputPath,
                            "unique aligned offset and target state",
                            std::to_string(output.offsetBytes) +
                                "/" +
                                WideToUtf8(output.stateId),
                            "correct the output binding");
                    }
                    service.outputs.push_back(
                        std::move(output));
                }
                result.services.push_back(std::move(service));
            }
            return result;
        }

        std::shared_ptr<WorkerCreativeExecutionPlan const>
            BuildExecutionPlan(
                WorkerCreativeActiveInstall const& install)
        {
            auto plan =
                std::make_shared<WorkerCreativeExecutionPlan>();
            plan->projectId = install.projectId;
            plan->projectVersion = install.projectVersion;
            plan->installId = install.installId;
            plan->bundleSha256 = install.bundleSha256;
            plan->contentSha256 = install.contentSha256;
            plan->hostProfileCanonicalSha256 =
                install.hostProfileCanonicalSha256;
            plan->activationRecordSha256 =
                install.activationRecordSha256;
            plan->activationSequence =
                install.activationSequence;
            plan->entryModule = install.entryModule;

            std::set<std::wstring> moduleIds;
            for (auto const& file : install.files)
            {
                if (file.role == L"asset")
                {
                    WorkerCreativeAsset asset;
                    asset.id = file.id;
                    asset.mediaType = file.kind;
                    asset.runtimePath = file.runtimePath;
                    asset.sha256 = file.sha256;
                    asset.bytes = file.bytes;
                    asset.casPath = file.casPath;
                    if (!plan->assets.emplace(
                            asset.id,
                            std::move(asset)).second)
                    {
                        FailForeground(
                            "xcp.creative.module_identity_duplicate",
                            "creative foreground asset id is duplicated",
                            "foreground_plan_validation",
                            "asset.id",
                            WideToUtf8(file.runtimePath),
                            "unique asset id",
                            WideToUtf8(file.id),
                            "rename the duplicate asset and rebuild");
                    }
                    continue;
                }
                if (file.role != L"module" ||
                    !moduleIds.insert(file.id).second)
                {
                    FailForeground(
                        "xcp.creative.foreground_install_invalid",
                        "creative foreground installed file role or module id is invalid",
                        "foreground_plan_validation",
                        "files",
                        WideToUtf8(file.runtimePath),
                        "unique installed module or asset",
                        WideToUtf8(file.role + L":" + file.id),
                        "remove and recommit the exact bundle");
                }
                auto module = ParseModuleDocument(file);
                if (file.kind == L"xcp.canvas2d.v1")
                {
                    if (plan->canvas.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one canvas module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.canvas2d.v1 module",
                            "multiple",
                            "compose the canvas into one module");
                    plan->canvas = ParseCanvas(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.ui.v1")
                {
                    if (plan->ui.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one UI module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.ui.v1 module",
                            "multiple",
                            "compose the UI into one module");
                    plan->ui = ParseUi(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.behavior.v1")
                {
                    if (plan->behavior.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one behavior module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.behavior.v1 module",
                            "multiple",
                            "compose behavior into one module");
                    plan->behavior = ParseBehavior(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.data.v1")
                {
                    if (plan->data.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one data module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.data.v1 module",
                            "multiple",
                            "compose data into one module");
                    plan->data = ParseData(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (
                    file.kind == L"xcp.world2d.v1" ||
                    file.kind == L"xcp.world2d.v2")
                {
                    auto parsed = ParseWorkerCreativeWorld2D(
                        module,
                        WideToUtf8(file.runtimePath));
                    if (!plan->world2dModules.emplace(
                            file.id,
                            std::move(parsed)).second)
                        FailForeground(
                            "xcp.creative.module_identity_duplicate",
                            "creative foreground world2d module id is duplicated",
                            "foreground_plan_validation",
                            "module_id",
                            WideToUtf8(file.runtimePath),
                            "unique world2d module id",
                            WideToUtf8(file.id),
                            "rename the duplicate module");
                }
                else if (file.kind == L"xcp.world2d.campaign.v1")
                {
                    if (plan->world2dCampaign.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground admits one world2d campaign module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.world2d.campaign.v1 module",
                            "multiple",
                            "compose progression into one campaign module");
                    plan->world2dCampaign =
                        ParseWorkerCreativeWorld2DCampaign(
                            module,
                            WideToUtf8(file.runtimePath));
                    plan->world2dCampaignModuleId = file.id;
                }
                else if (file.kind == L"xcp.world2d.motion.v1")
                {
                    if (plan->world2dMotion.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground admits one world2d motion module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.world2d.motion.v1 module",
                            "multiple",
                            "compose motion bindings into one module");
                    plan->world2dMotion =
                        ParseWorkerCreativeWorld2DMotion(
                            module,
                            WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.scene3d.v1")
                {
                    if (plan->scene3d.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one 3D scene module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.scene3d.v1 module",
                            "multiple",
                            "compose the scene into one module");
                    plan->scene3d = ParseScene3D(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.audio.v1")
                {
                    if (plan->audio.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one audio module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.audio.v1 module",
                            "multiple",
                            "compose audio into one module");
                    plan->audio = ParseAudio(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else if (file.kind == L"xcp.xvm.v2")
                {
                    if (plan->xvm.has_value())
                        FailForeground(
                            "xcp.creative.module_cardinality_exceeded",
                            "creative foreground V1 admits one XVM service module",
                            "foreground_plan_validation",
                            "kind",
                            WideToUtf8(file.runtimePath),
                            "at most one xcp.xvm.v2 module",
                            "multiple",
                            "compose XVM services into one module");
                    plan->xvm = ParseXvm(
                        module,
                        WideToUtf8(file.runtimePath));
                }
                else
                {
                    FailForeground(
                        "xcp.creative.module_kind_unsupported",
                        "creative foreground module kind is unsupported",
                        "foreground_plan_validation",
                        "kind",
                        WideToUtf8(file.runtimePath),
                        "module kind published by describe_creative_host",
                        WideToUtf8(file.kind),
                        "remove the module or upgrade the host");
                }
            }
            if (plan->world2dCampaign.has_value())
            {
                if (plan->entryModule !=
                    plan->world2dCampaignModuleId)
                {
                    FailForeground(
                        "xcp.creative.entry_module_invalid",
                        "world2d campaign must own the project entry module",
                        "foreground_plan_validation",
                        "entry_module",
                        WideToUtf8(plan->entryModule),
                        WideToUtf8(plan->world2dCampaignModuleId),
                        WideToUtf8(plan->entryModule),
                        "select the campaign module as entry");
                }
                std::set<std::wstring> referencedModules;
                for (auto const& scene :
                     plan->world2dCampaign->scenes)
                {
                    auto world =
                        plan->world2dModules.find(scene.moduleId);
                    if (world == plan->world2dModules.end() ||
                        !referencedModules.insert(
                            scene.moduleId).second)
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "world2d campaign scene module is missing or reused",
                            "foreground_plan_validation",
                            "scenes.module_id",
                            WideToUtf8(scene.id),
                            "one distinct installed world2d module",
                            WideToUtf8(scene.moduleId),
                            "correct the campaign scene binding");
                    }
                    if (scene.id ==
                        plan->world2dCampaign->entryScene)
                        plan->world2d = world->second;
                }
                if (referencedModules.size() !=
                        plan->world2dModules.size() ||
                    !plan->world2d.has_value())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "world2d campaign module coverage is incomplete",
                        "foreground_plan_validation",
                        "scenes",
                        WideToUtf8(plan->world2dCampaignModuleId),
                        "every installed world2d module referenced exactly once",
                        std::to_string(referencedModules.size()) + "/" +
                            std::to_string(
                                plan->world2dModules.size()),
                        "remove unreferenced worlds or complete the campaign");
                }
            }
            else if (plan->world2dModules.size() == 1)
            {
                plan->world2d =
                    plan->world2dModules.begin()->second;
            }
            else if (plan->world2dModules.size() > 1)
            {
                FailForeground(
                    "xcp.creative.module_cardinality_exceeded",
                    "multiple world2d modules require one campaign owner",
                    "foreground_plan_validation",
                    "modules",
                    WideToUtf8(plan->projectId),
                    "one world2d module or one campaign binding all worlds",
                    std::to_string(plan->world2dModules.size()),
                    "add xcp.world2d.campaign.v1 or compose one world");
            }
            if (plan->world2dMotion.has_value())
            {
                if (plan->world2dModules.empty())
                {
                    FailForeground(
                        "xcp.creative.module_reference_invalid",
                        "world2d motion requires a composed world2d module",
                        "foreground_plan_validation",
                        "modules",
                        WideToUtf8(plan->projectId),
                        "one or more xcp.world2d modules",
                        "none",
                        "add the world module referenced by the motion contract");
                }
                for (auto const& [moduleId, world] :
                     plan->world2dModules)
                {
                    ValidateWorkerCreativeWorld2DMotion(
                        *plan->world2dMotion,
                        world,
                        WideToUtf8(moduleId));
                }
            }
            if (moduleIds.find(plan->entryModule) == moduleIds.end())
            {
                FailForeground(
                    "xcp.creative.entry_module_missing",
                    "creative foreground entry module is not installed",
                    "foreground_plan_validation",
                    "entry_module",
                    WideToUtf8(plan->entryModule),
                    "installed module id",
                    "missing",
                    "rebuild the project with a declared entry module");
            }
            if (!plan->canvas.has_value() &&
                !plan->scene3d.has_value() &&
                !plan->world2d.has_value())
            {
                FailForeground(
                    "xcp.creative.foreground_surface_missing",
                    "creative foreground project has no renderable surface",
                    "foreground_plan_validation",
                    "modules",
                    WideToUtf8(plan->projectId),
                    "xcp.canvas2d.v1, xcp.world2d.v1, xcp.world2d.v2, xcp.world2d.campaign.v1 or xcp.scene3d.v1",
                    "neither",
                    "add a published foreground rendering module");
            }

            std::map<std::wstring, std::wstring> behaviorTypes;
            if (plan->behavior.has_value())
            {
                for (auto const& definition :
                     plan->behavior->state)
                {
                    behaviorTypes.emplace(
                        definition.id,
                        definition.type);
                }
            }
            std::map<std::wstring, std::wstring> localTypes;
            if (plan->data.has_value())
            {
                for (auto const& definition :
                     plan->data->localState)
                {
                    localTypes.emplace(
                        definition.id,
                        definition.type);
                }
            }
            std::set<std::wstring> canvasNodes;
            if (plan->canvas.has_value())
            {
                for (auto const& node : plan->canvas->nodes)
                    canvasNodes.insert(node.id);
            }
            std::set<std::wstring> sceneNodes;
            if (plan->scene3d.has_value())
            {
                for (auto const& node : plan->scene3d->nodes)
                    sceneNodes.insert(node.id);
            }
            std::set<std::wstring> focusableUi;
            if (plan->ui.has_value())
            {
                for (auto const& element : plan->ui->elements)
                {
                    if (element.focusable)
                        focusableUi.insert(element.id);
                    if (!element.valueState.empty())
                    {
                        auto value =
                            behaviorTypes.find(element.valueState);
                        auto numericProgress =
                            element.type != L"progress" ||
                            (value != behaviorTypes.end() &&
                             (value->second == L"integer" ||
                              value->second == L"number"));
                        if (value == behaviorTypes.end() ||
                            !numericProgress)
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative UI value_state is missing or incompatible",
                                "foreground_plan_validation",
                                "value_state",
                                WideToUtf8(element.id),
                                element.type == L"progress"
                                    ? "declared numeric behavior state"
                                    : "declared behavior state",
                                WideToUtf8(element.valueState),
                                "correct the UI state binding");
                        }
                    }
                    if (!element.visibleState.empty())
                    {
                        auto visible =
                            behaviorTypes.find(element.visibleState);
                        if (visible == behaviorTypes.end() ||
                            visible->second != L"boolean")
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative UI visible_state is missing or not boolean",
                                "foreground_plan_validation",
                                "visible_state",
                                WideToUtf8(element.id),
                                "declared boolean behavior state",
                                WideToUtf8(element.visibleState),
                                "correct the UI visibility binding");
                        }
                    }
                }
            }
            if (plan->world2d.has_value())
            {
                std::set<std::wstring> decodedSprites;
                auto validateAsset =
                    [&](std::wstring const& owner,
                        std::wstring const& assetId,
                        WorkerCreativeWorld2DRender const* render = nullptr)
                {
                    if (assetId.empty())
                        return;
                    auto asset = plan->assets.find(assetId);
                    if (asset == plan->assets.end())
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative world2d sprite asset is missing",
                            "foreground_plan_validation",
                            "asset_id",
                            WideToUtf8(owner),
                            "declared installed PNG asset",
                            WideToUtf8(assetId),
                            "add the asset or correct the world item");
                    }
                    if (decodedSprites.insert(assetId).second)
                        DecodePngAsset(asset->second);
                    if (render && render->hasSourceRect &&
                        (render->sourceRect.x +
                                render->sourceRect.width >
                            static_cast<float>(asset->second.pixelWidth) ||
                         render->sourceRect.y +
                                render->sourceRect.height >
                            static_cast<float>(asset->second.pixelHeight)))
                    {
                        FailForeground(
                            "xcp.creative.sprite_source_rect_invalid",
                            "creative world2d sprite source rectangle exceeds the decoded PNG",
                            "foreground_plan_validation",
                            "source_rect",
                            WideToUtf8(owner),
                            "rectangle inside decoded PNG dimensions",
                            std::to_string(
                                asset->second.pixelWidth) + "x" +
                                std::to_string(
                                    asset->second.pixelHeight),
                            "correct the atlas region or source asset");
                    }
                };
                auto validateWorld =
                    [&](std::wstring const& moduleId,
                        WorkerCreativeWorld2D const& world)
                    {
                        validateAsset(
                            moduleId + L".background",
                            world.backgroundAssetId);
                        for (auto const& tile : world.tiles)
                            validateAsset(
                                moduleId + L"." + tile.id,
                                tile.assetId,
                                tile.hasAuthoredRender
                                    ? &tile.render
                                    : nullptr);
                        for (auto const& entity : world.entities)
                            validateAsset(
                                moduleId + L"." + entity.id,
                                entity.assetId,
                                entity.hasAuthoredRender
                                    ? &entity.render
                                    : nullptr);
                    };
                if (plan->world2dCampaign.has_value())
                {
                    for (auto const& [moduleId, world] :
                         plan->world2dModules)
                        validateWorld(moduleId, world);
                }
                else
                {
                    validateWorld(L"world", *plan->world2d);
                }
            }
            if (plan->behavior.has_value())
            {
                for (auto const& rule : plan->behavior->rules)
                {
                    for (auto const& action : rule.actions)
                    {
                        bool referenceValid = true;
                        std::string field = "target";
                        std::string expected;
                        if (action.type == L"node.visibility")
                        {
                            referenceValid =
                                canvasNodes.find(action.target) !=
                                    canvasNodes.end() ||
                                sceneNodes.find(action.target) !=
                                    sceneNodes.end();
                            expected =
                                "declared canvas or scene node";
                        }
                        else if (action.type ==
                                 L"node.translate2d")
                        {
                            referenceValid =
                                canvasNodes.find(action.target) !=
                                    canvasNodes.end();
                            expected = "declared canvas node";
                        }
                        else if (action.type ==
                                 L"node.translate3d")
                        {
                            referenceValid =
                                sceneNodes.find(action.target) !=
                                    sceneNodes.end();
                            expected = "declared scene node";
                        }
                        else if (action.type == L"ui.focus")
                        {
                            referenceValid =
                                focusableUi.find(action.target) !=
                                    focusableUi.end();
                            expected =
                                "declared focusable UI element";
                        }
                        else if (action.type == L"state.save")
                        {
                            auto source = action.source.empty()
                                ? action.target
                                : action.source;
                            auto sourceType =
                                behaviorTypes.find(source);
                            auto targetType =
                                localTypes.find(action.target);
                            referenceValid =
                                sourceType != behaviorTypes.end() &&
                                targetType != localTypes.end() &&
                                sourceType->second ==
                                    targetType->second;
                            field = "source/target";
                            expected =
                                "compatible behavior source and local-state target";
                        }
                        else if (action.type == L"state.load")
                        {
                            auto source = action.source.empty()
                                ? action.target
                                : action.source;
                            auto sourceType =
                                localTypes.find(source);
                            auto targetType =
                                behaviorTypes.find(action.target);
                            referenceValid =
                                sourceType != localTypes.end() &&
                                targetType != behaviorTypes.end() &&
                                sourceType->second ==
                                    targetType->second;
                            field = "source/target";
                            expected =
                                "compatible local-state source and behavior target";
                        }
                        if (!referenceValid)
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative behavior action has an invalid cross-module reference",
                                "foreground_plan_validation",
                                field,
                                WideToUtf8(rule.id),
                                expected,
                                WideToUtf8(
                                    (action.source.empty()
                                         ? L""
                                         : action.source + L"/") +
                                    action.target),
                                "correct the behavior action binding");
                        }
                    }
                }
            }
            if (plan->canvas.has_value())
            {
                std::set<std::wstring> decodedSprites;
                for (auto const& node : plan->canvas->nodes)
                {
                    if (!node.assetId.empty() &&
                        plan->assets.find(node.assetId) ==
                            plan->assets.end())
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative canvas sprite asset is missing",
                            "foreground_plan_validation",
                            "asset_id",
                            WideToUtf8(node.id),
                            "declared installed asset",
                            WideToUtf8(node.assetId),
                            "add the asset or correct the sprite");
                    }
                    if (!node.assetId.empty() &&
                        decodedSprites.insert(
                            node.assetId).second)
                    {
                        DecodePngAsset(
                            plan->assets.at(node.assetId));
                    }
                }
            }
            if (plan->scene3d.has_value())
            {
                std::map<std::wstring, uint64_t> triangleCounts;
                for (auto& mesh : plan->scene3d->meshes)
                {
                    if (!mesh.assetId.empty())
                    {
                        auto asset =
                            plan->assets.find(mesh.assetId);
                        if (asset == plan->assets.end())
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative 3D mesh asset is missing",
                                "foreground_plan_validation",
                                "asset_id",
                                WideToUtf8(mesh.id),
                                "declared installed asset",
                                WideToUtf8(mesh.assetId),
                                "add the asset or correct the mesh");
                        }
                        mesh.triangles =
                            DecodeMeshAsset(asset->second);
                    }
                    triangleCounts.emplace(
                        mesh.id,
                        mesh.assetId.empty()
                            ? PrimitiveTriangleCount(
                                mesh.primitive)
                            : mesh.triangles.size());
                }
                uint64_t sceneTriangles = 0;
                for (auto const& node : plan->scene3d->nodes)
                {
                    if (node.meshId.empty())
                        continue;
                    auto count = triangleCounts.find(node.meshId);
                    if (count == triangleCounts.end() ||
                        count->second >
                            MaxSceneTriangles - sceneTriangles)
                    {
                        FailForeground(
                            "xcp.creative.scene_triangle_budget_exceeded",
                            "creative foreground scene exceeds its triangle budget",
                            "foreground_plan_validation",
                            "nodes",
                            WideToUtf8(plan->projectId),
                            std::to_string(MaxSceneTriangles) +
                                " triangles or fewer",
                            std::to_string(sceneTriangles +
                                (count == triangleCounts.end()
                                    ? 0
                                    : count->second)),
                            "reduce scene instances or primitive complexity");
                    }
                    sceneTriangles += count->second;
                }
            }
            if (plan->audio.has_value())
            {
                std::set<std::wstring> decodedAssets;
                uint64_t decodedAudioBytes = 0;
                for (auto const& clip : plan->audio->clips)
                {
                    auto asset = plan->assets.find(clip.assetId);
                    if (asset == plan->assets.end())
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative audio clip asset is missing",
                            "foreground_plan_validation",
                            "asset_id",
                            WideToUtf8(clip.id),
                            "declared installed audio/wav asset",
                            WideToUtf8(clip.assetId),
                            "add the asset or correct the clip");
                    }
                    if (decodedAssets.insert(
                            clip.assetId).second)
                    {
                        DecodeWavAsset(asset->second);
                        if (asset->second.decodedAudio.size() >
                            MaxAudioPcmBytes -
                                decodedAudioBytes)
                        {
                            FailForeground(
                                "xcp.creative.audio_pcm_budget_exceeded",
                                "creative project exceeds the decoded audio budget",
                                "foreground_plan_validation",
                                "clips",
                                WideToUtf8(plan->projectId),
                                "at most 16777216 decoded PCM bytes",
                                std::to_string(
                                    decodedAudioBytes +
                                    asset->second.decodedAudio.size()),
                                "shorten, reduce or reuse audio clips");
                        }
                        decodedAudioBytes +=
                            asset->second.decodedAudio.size();
                    }
                }
            }
            if (plan->xvm.has_value())
            {
                for (auto& service : plan->xvm->services)
                {
                    auto asset =
                        plan->assets.find(
                            service.programAssetId);
                    if (asset == plan->assets.end())
                    {
                        FailForeground(
                            "xcp.creative.module_reference_invalid",
                            "creative XVM program asset is missing",
                            "foreground_plan_validation",
                            "program_asset_id",
                            WideToUtf8(service.id),
                            "declared installed XVM v2 program asset",
                            WideToUtf8(
                                service.programAssetId),
                            "add the assembled program or correct the service");
                    }
                    service.program =
                        DecodeXvmProgramAsset(asset->second);
                    for (auto const& input :
                         service.inputs)
                    {
                        if (!input.fromState)
                            continue;
                        auto type =
                            behaviorTypes.find(
                                input.stateId);
                        if (type == behaviorTypes.end() ||
                            (type->second != L"integer" &&
                                type->second != L"boolean"))
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative XVM input state is missing or not word-compatible",
                                "foreground_plan_validation",
                                "inputs[].state_id",
                                WideToUtf8(service.id),
                                "declared integer or boolean behavior state",
                                WideToUtf8(input.stateId),
                                "correct the XVM input binding");
                        }
                    }
                    for (auto const& output :
                         service.outputs)
                    {
                        auto type =
                            behaviorTypes.find(
                                output.stateId);
                        if (type == behaviorTypes.end() ||
                            type->second != L"integer" ||
                            static_cast<uint64_t>(
                                output.offsetBytes) + 4ull >
                                service.program.outputBytes)
                        {
                            FailForeground(
                                "xcp.creative.module_reference_invalid",
                                "creative XVM output binding is missing, incompatible or out of range",
                                "foreground_plan_validation",
                                "outputs",
                                WideToUtf8(service.id),
                                "declared integer state and in-range aligned output word",
                                std::to_string(
                                    output.offsetBytes) + "/" +
                                    WideToUtf8(
                                        output.stateId),
                                "correct the XVM output binding");
                        }
                    }
                }
            }

            std::vector<std::wstring> bindings;
            for (auto const& file : install.files)
            {
                bindings.push_back(
                    file.role + L":" + file.id + L":" + file.kind +
                    L":" + file.sha256 + L":" +
                    std::to_wstring(file.bytes));
            }
            std::sort(bindings.begin(), bindings.end());
            std::wstring canonical =
                L"xcp-creative-foreground-execution-plan-v1\n" +
                plan->projectId + L"\n" +
                plan->projectVersion + L"\n" +
                plan->installId + L"\n" +
                plan->bundleSha256 + L"\n" +
                plan->contentSha256 + L"\n" +
                plan->hostProfileCanonicalSha256 + L"\n" +
                plan->activationRecordSha256 + L"\n" +
                std::to_wstring(plan->activationSequence) + L"\n" +
                plan->entryModule + L"\n";
            for (auto const& binding : bindings)
            {
                canonical += binding + L"\n";
            }
            plan->planSha256 =
                WorkerContentSha256(WideToUtf8(canonical));
            return plan;
        }

        fs::path ForegroundRoot(fs::path const& workerRoot)
        {
            return workerRoot.parent_path() /
                L"xcp-creative-v1" /
                L"foreground";
        }

        fs::path LaunchRecordRoot(fs::path const& workerRoot)
        {
            return ForegroundRoot(workerRoot) / L"records";
        }

        fs::path StatePath(
            fs::path const& workerRoot,
            std::wstring const& projectId)
        {
            return workerRoot.parent_path() /
                L"xcp-creative-v1" /
                L"states" /
                WorkerContentSha256(WideToUtf8(projectId)) /
                L"state.json";
        }

        struct ForegroundLaunchRecord
        {
            uint64_t sequence = 0;
            std::wstring previousRecordSha256;
            std::wstring launchId;
            std::wstring operation;
            std::wstring projectId;
            std::wstring installId;
            std::wstring hostProfileCanonicalSha256;
            std::wstring activationRecordSha256;
            uint64_t activationSequence = 0;
            std::wstring planSha256;
            std::wstring recordSha256;
        };

        std::wstring LaunchRecordJson(
            ForegroundLaunchRecord const& record)
        {
            return L"{\"activation_record_sha256\":" +
                JsonString(record.activationRecordSha256) +
                L",\"activation_sequence\":" +
                std::to_wstring(record.activationSequence) +
                L",\"host_profile_canonical_sha256\":" +
                JsonString(record.hostProfileCanonicalSha256) +
                L",\"install_id\":" + JsonString(record.installId) +
                L",\"launch_id\":" + JsonString(record.launchId) +
                L",\"operation\":" + JsonString(record.operation) +
                L",\"plan_sha256\":" + JsonString(record.planSha256) +
                L",\"previous_record_sha256\":" +
                JsonString(record.previousRecordSha256) +
                L",\"project_id\":" + JsonString(record.projectId) +
                L",\"schema_version\":\"xcp-creative-foreground-launch-record-v1\"" +
                L",\"sequence\":" + std::to_wstring(record.sequence) +
                L"}\n";
        }

        std::string ReadBoundedFile(
            fs::path const& path,
            uint64_t maximum)
        {
            if (!fs::exists(path) ||
                !fs::is_regular_file(path) ||
                static_cast<uint64_t>(fs::file_size(path)) > maximum)
            {
                FailForeground(
                    "xcp.creative.foreground_metadata_invalid",
                    "creative foreground metadata file is missing or oversized",
                    "foreground_metadata_validation",
                    "record",
                    WideToUtf8(path.wstring()),
                    "bounded regular file",
                    "missing or oversized",
                    "repair the app-private creative foreground metadata");
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                FailForeground(
                    "xcp.creative.foreground_metadata_invalid",
                    "creative foreground metadata cannot be read",
                    "foreground_metadata_validation",
                    "record",
                    WideToUtf8(path.wstring()),
                    "readable app-private metadata",
                    "open failed",
                    "repair the app-private creative foreground metadata");
            }
            return std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }

        JsonObject ParseObjectBytes(
            std::string const& bytes,
            std::string const& path)
        {
            try
            {
                auto value = JsonValue::Parse(Utf8ToWide(bytes));
                if (value.ValueType() != JsonValueType::Object)
                    throw std::runtime_error("root");
                return value.GetObject();
            }
            catch (...)
            {
                FailForeground(
                    "xcp.creative.foreground_metadata_invalid",
                    "creative foreground metadata is not valid JSON",
                    "foreground_metadata_validation",
                    "record",
                    path,
                    "UTF-8 JSON object",
                    "parse failed",
                    "repair the app-private creative foreground metadata");
            }
        }

        std::vector<ForegroundLaunchRecord> LoadLaunchRecords(
            fs::path const& workerRoot)
        {
            std::vector<ForegroundLaunchRecord> records;
            auto root = LaunchRecordRoot(workerRoot);
            if (!fs::exists(root))
            {
                return records;
            }
            std::vector<fs::path> paths;
            for (auto const& entry : fs::directory_iterator(root))
            {
                if (entry.is_regular_file() &&
                    entry.path().extension() == L".json")
                {
                    paths.push_back(entry.path());
                }
            }
            std::sort(paths.begin(), paths.end());
            if (paths.size() > MaxLaunchRecords)
            {
                FailForeground(
                    "xcp.creative.foreground_history_exceeded",
                    "creative foreground launch history exceeds its bound",
                    "foreground_metadata_validation",
                    "records",
                    WideToUtf8(root.wstring()),
                    "at most 4096 launch records",
                    std::to_string(paths.size()),
                    "compact launch history in a later measured maintenance gate");
            }
            std::wstring previousSha;
            std::set<std::wstring> launchIds;
            uint64_t expectedSequence = 1;
            for (auto const& path : paths)
            {
                auto bytes = ReadBoundedFile(
                    path,
                    MaxModuleDocumentBytes);
                auto object = ParseObjectBytes(
                    bytes,
                    WideToUtf8(path.wstring()));
                RequireExactFields(
                    object,
                    {
                        L"activation_record_sha256",
                        L"activation_sequence",
                        L"host_profile_canonical_sha256",
                        L"install_id",
                        L"launch_id",
                        L"operation",
                        L"plan_sha256",
                        L"previous_record_sha256",
                        L"project_id",
                        L"schema_version",
                        L"sequence"
                    },
                    WideToUtf8(path.wstring()));
                if (RequiredString(
                        object,
                        L"schema_version",
                        WideToUtf8(path.wstring())) !=
                    L"xcp-creative-foreground-launch-record-v1")
                {
                    FailForeground(
                        "xcp.creative.foreground_metadata_invalid",
                        "creative foreground launch record schema is invalid",
                        "foreground_metadata_validation",
                        "schema_version",
                        WideToUtf8(path.wstring()),
                        "xcp-creative-foreground-launch-record-v1",
                        "other",
                        "repair the launch journal");
                }
                ForegroundLaunchRecord record;
                record.sequence = static_cast<uint64_t>(
                    RequiredInteger(
                        object,
                        L"sequence",
                        WideToUtf8(path.wstring()),
                        1,
                        static_cast<int64_t>(MaxLaunchRecords)));
                record.previousRecordSha256 = RequiredString(
                    object,
                    L"previous_record_sha256",
                    WideToUtf8(path.wstring()),
                    64);
                record.launchId = RequiredString(
                    object,
                    L"launch_id",
                    WideToUtf8(path.wstring()),
                    64);
                record.operation = RequiredString(
                    object,
                    L"operation",
                    WideToUtf8(path.wstring()),
                    16);
                record.projectId = RequiredString(
                    object,
                    L"project_id",
                    WideToUtf8(path.wstring()),
                    96);
                record.installId = RequiredString(
                    object,
                    L"install_id",
                    WideToUtf8(path.wstring()),
                    64);
                record.hostProfileCanonicalSha256 = RequiredString(
                    object,
                    L"host_profile_canonical_sha256",
                    WideToUtf8(path.wstring()),
                    64);
                record.activationRecordSha256 = RequiredString(
                    object,
                    L"activation_record_sha256",
                    WideToUtf8(path.wstring()),
                    64);
                record.activationSequence =
                    static_cast<uint64_t>(
                        RequiredInteger(
                            object,
                            L"activation_sequence",
                            WideToUtf8(path.wstring()),
                            1,
                            static_cast<int64_t>(
                                MaxLaunchRecords)));
                record.planSha256 = RequiredString(
                    object,
                    L"plan_sha256",
                    WideToUtf8(path.wstring()),
                    64);
                record.recordSha256 =
                    WorkerContentSha256(bytes);
                if (record.sequence != expectedSequence ||
                    record.previousRecordSha256 != previousSha ||
                    !IsSafeLaunchId(record.launchId) ||
                    !launchIds.insert(record.launchId).second ||
                    (record.operation != L"launch" &&
                        record.operation != L"reload") ||
                    !IsSafeIdentifier(record.projectId) ||
                    !IsLowerHexSha256(record.installId) ||
                    !IsLowerHexSha256(
                        record.hostProfileCanonicalSha256) ||
                    !IsLowerHexSha256(
                        record.activationRecordSha256) ||
                    !IsLowerHexSha256(record.planSha256) ||
                    bytes != WideToUtf8(LaunchRecordJson(record)))
                {
                    FailForeground(
                        "xcp.creative.foreground_launch_chain_invalid",
                        "creative foreground launch record chain is invalid",
                        "foreground_metadata_validation",
                        "record",
                        WideToUtf8(path.wstring()),
                        "canonical contiguous hash-chained launch records",
                        "identity or chain mismatch",
                        "repair the launch journal before launching");
                }
                previousSha = record.recordSha256;
                ++expectedSequence;
                records.push_back(std::move(record));
            }
            return records;
        }

        std::wstring LaunchRecordName(
            uint64_t sequence,
            std::wstring const& launchId)
        {
            std::wostringstream name;
            name << std::setfill(L'0') << std::setw(20)
                << sequence << L"-" << launchId << L".json";
            return name.str();
        }

        void WriteImmutableFile(
            fs::path const& path,
            std::string const& bytes)
        {
            fs::create_directories(path.parent_path());
            if (fs::exists(path))
            {
                if (ReadBoundedFile(
                        path,
                        MaxModuleDocumentBytes) == bytes)
                {
                    return;
                }
                FailForeground(
                    "xcp.creative.foreground_launch_conflict",
                    "creative foreground immutable launch record conflicts",
                    "foreground_metadata_commit",
                    "record",
                    WideToUtf8(path.wstring()),
                    "absent or byte-identical record",
                    "conflicting bytes",
                    "use a fresh launch id");
            }
            auto staging = path;
            staging += L".staging";
            if (fs::exists(staging))
                fs::remove(staging);
            {
                std::ofstream output(
                    staging,
                    std::ios::binary | std::ios::trunc);
                output.write(
                    bytes.data(),
                    static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output)
                {
                    fs::remove(staging);
                    FailForeground(
                        "xcp.creative.foreground_metadata_write_failed",
                        "creative foreground launch record write failed",
                        "foreground_metadata_commit",
                        "record",
                        WideToUtf8(staging.wstring()),
                        "complete app-private staging write",
                        "write failed",
                        "retry after checking app-private storage",
                        true);
                }
            }
            if (WorkerContentSha256File(staging) !=
                WorkerContentSha256(bytes))
            {
                fs::remove(staging);
                FailForeground(
                    "xcp.creative.foreground_metadata_write_failed",
                    "creative foreground launch staging hash mismatch",
                    "foreground_metadata_commit",
                    "record",
                    WideToUtf8(staging.wstring()),
                    "exact staged bytes",
                    "hash mismatch",
                    "retry after checking app-private storage",
                    true);
            }
            std::error_code error;
            fs::rename(staging, path, error);
            if (error)
            {
                fs::remove(staging);
                FailForeground(
                    "xcp.creative.foreground_metadata_commit_failed",
                    "creative foreground launch record commit failed",
                    "foreground_metadata_commit",
                    "record",
                    WideToUtf8(path.wstring()),
                    "atomic rename",
                    error.message(),
                    "retry without changing the request",
                    true);
            }
        }

        JsonObject StripTransportFields(JsonObject const& wireRequest)
        {
            auto request = JsonObject::Parse(wireRequest.Stringify());
            for (auto const* field : {
                     L"session_id",
                     L"pairing_code",
                     L"_server_parse_json_ms",
                     L"_server_request_bytes",
                     L"_server_auth_ms" })
            {
                if (request.HasKey(field))
                    request.Remove(field);
            }
            return request;
        }

        std::wstring BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring ScalarJson(WorkerCreativeScalar const& value)
        {
            if (auto boolean = std::get_if<bool>(&value))
                return BoolJson(*boolean);
            if (auto integer = std::get_if<int64_t>(&value))
                return std::to_wstring(*integer);
            if (auto number = std::get_if<double>(&value))
            {
                std::wostringstream out;
                out << std::setprecision(17) << *number;
                return out.str();
            }
            return JsonString(std::get<std::wstring>(value));
        }

        std::wstring ScalarText(WorkerCreativeScalar const& value)
        {
            if (auto boolean = std::get_if<bool>(&value))
                return *boolean ? L"true" : L"false";
            if (auto integer = std::get_if<int64_t>(&value))
                return std::to_wstring(*integer);
            if (auto number = std::get_if<double>(&value))
            {
                std::wostringstream out;
                out << std::setprecision(8) << *number;
                return out.str();
            }
            return std::get<std::wstring>(value);
        }

        double ScalarNumber(WorkerCreativeScalar const& value)
        {
            if (auto integer = std::get_if<int64_t>(&value))
                return static_cast<double>(*integer);
            if (auto number = std::get_if<double>(&value))
                return *number;
            FailForeground(
                "xcp.creative.behavior_type_mismatch",
                "creative behavior expected a numeric state value",
                "foreground_execution",
                "state",
                {},
                "integer or number",
                "other type",
                "correct the state/action type");
        }

        bool ScalarEqual(
            WorkerCreativeScalar const& left,
            WorkerCreativeScalar const& right)
        {
            auto leftNumeric =
                std::holds_alternative<int64_t>(left) ||
                std::holds_alternative<double>(left);
            auto rightNumeric =
                std::holds_alternative<int64_t>(right) ||
                std::holds_alternative<double>(right);
            if (leftNumeric && rightNumeric)
                return ScalarNumber(left) == ScalarNumber(right);
            return left == right;
        }

        int ScalarCompare(
            WorkerCreativeScalar const& left,
            WorkerCreativeScalar const& right)
        {
            auto leftNumeric =
                std::holds_alternative<int64_t>(left) ||
                std::holds_alternative<double>(left);
            auto rightNumeric =
                std::holds_alternative<int64_t>(right) ||
                std::holds_alternative<double>(right);
            if (leftNumeric && rightNumeric)
            {
                auto a = ScalarNumber(left);
                auto b = ScalarNumber(right);
                return a < b ? -1 : (a > b ? 1 : 0);
            }
            if (auto a = std::get_if<std::wstring>(&left))
            {
                auto b = std::get_if<std::wstring>(&right);
                if (b == nullptr)
                    FailForeground(
                        "xcp.creative.behavior_type_mismatch",
                        "creative behavior comparison types differ",
                        "foreground_execution",
                        "condition",
                        {},
                        "matching numeric or string operands",
                        "different types",
                        "correct the behavior condition");
                return *a < *b ? -1 : (*a > *b ? 1 : 0);
            }
            FailForeground(
                "xcp.creative.behavior_type_mismatch",
                "creative behavior ordered comparison does not support this type",
                "foreground_execution",
                "condition",
                {},
                "numeric or string operands",
                "boolean or mismatched type",
                "use equal/not_equal or correct the condition");
        }

        std::wstring ScalarMapJson(
            std::map<std::wstring, WorkerCreativeScalar> const& values)
        {
            std::wstring result = L"{";
            bool first = true;
            for (auto const& [name, value] : values)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(name) + L":" + ScalarJson(value);
            }
            result += L"}";
            return result;
        }

        std::wstring StringArrayJson(
            std::vector<std::wstring> const& values)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0) result += L",";
                result += JsonString(values[index]);
            }
            result += L"]";
            return result;
        }

        std::wstring NumberJson(float value)
        {
            std::wostringstream out;
            out << std::setprecision(9) << value;
            return out.str();
        }

        std::wstring Vec2MapJson(
            std::map<std::wstring, WorkerCreativeVec2> const& values)
        {
            std::wstring result = L"{";
            bool first = true;
            for (auto const& [name, value] : values)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(name) + L":[" +
                    NumberJson(value.x) + L"," +
                    NumberJson(value.y) + L"]";
            }
            result += L"}";
            return result;
        }

        std::wstring Vec3MapJson(
            std::map<std::wstring, WorkerCreativeVec3> const& values)
        {
            std::wstring result = L"{";
            bool first = true;
            for (auto const& [name, value] : values)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(name) + L":[" +
                    NumberJson(value.x) + L"," +
                    NumberJson(value.y) + L"," +
                    NumberJson(value.z) + L"]";
            }
            result += L"}";
            return result;
        }

        std::wstring BoolMapJson(
            std::map<std::wstring, bool> const& values)
        {
            std::wstring result = L"{";
            bool first = true;
            for (auto const& [name, value] : values)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(name) + L":" + BoolJson(value);
            }
            result += L"}";
            return result;
        }

        std::wstring Transform3DMapJson(
            std::map<std::wstring, WorkerCreativeTransform3D> const& values)
        {
            std::wstring result = L"{";
            bool first = true;
            for (auto const& [name, value] : values)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(name) +
                    L":{\"position\":[" +
                    NumberJson(value.position.x) + L"," +
                    NumberJson(value.position.y) + L"," +
                    NumberJson(value.position.z) +
                    L"],\"rotation_degrees\":[" +
                    NumberJson(value.rotationDegrees.x) + L"," +
                    NumberJson(value.rotationDegrees.y) + L"," +
                    NumberJson(value.rotationDegrees.z) +
                    L"],\"scale\":[" +
                    NumberJson(value.scale.x) + L"," +
                    NumberJson(value.scale.y) + L"," +
                    NumberJson(value.scale.z) + L"]}";
            }
            result += L"}";
            return result;
        }

        std::wstring SemanticEventArrayJson(
            std::vector<WorkerCreativeSemanticEventObservation> const& values)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0) result += L",";
                auto const& value = values[index];
                result += L"{\"sequence\":" +
                    std::to_wstring(value.sequence) +
                    L",\"event\":" + JsonString(value.event) +
                    L",\"state_revision\":" +
                    std::to_wstring(value.stateRevision) + L"}";
            }
            result += L"]";
            return result;
        }

        std::wstring PhysicalInputArrayJson(
            std::vector<WorkerCreativePhysicalInputObservation> const& values)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0) result += L",";
                auto const& value = values[index];
                result += L"{\"sequence\":" +
                    std::to_wstring(value.sequence) +
                    L",\"source\":" + JsonString(value.source) +
                    L",\"focused_element_id\":" +
                    JsonString(value.focusedElementId) +
                    L",\"semantic_events\":" +
                    StringArrayJson(value.semanticEvents) +
                    L",\"state_revision\":" +
                    std::to_wstring(value.stateRevision) + L"}";
            }
            result += L"]";
            return result;
        }

        std::string Base64Encode(
            std::vector<uint8_t> const& data)
        {
            static constexpr char Alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";
            std::string encoded;
            encoded.reserve(
                ((data.size() + 2) / 3) * 4);
            for (size_t index = 0;
                 index < data.size();
                 index += 3)
            {
                uint32_t value =
                    static_cast<uint32_t>(
                        data[index]) << 16;
                auto hasSecond =
                    index + 1 < data.size();
                auto hasThird =
                    index + 2 < data.size();
                if (hasSecond)
                {
                    value |=
                        static_cast<uint32_t>(
                            data[index + 1]) << 8;
                }
                if (hasThird)
                {
                    value |=
                        static_cast<uint32_t>(
                            data[index + 2]);
                }
                encoded.push_back(
                    Alphabet[(value >> 18) & 0x3f]);
                encoded.push_back(
                    Alphabet[(value >> 12) & 0x3f]);
                encoded.push_back(
                    hasSecond
                        ? Alphabet[(value >> 6) & 0x3f]
                        : '=');
                encoded.push_back(
                    hasThird
                        ? Alphabet[value & 0x3f]
                        : '=');
            }
            return encoded;
        }
    }

    struct WorkerCreativeForegroundRuntime::Impl
    {
        mutable std::mutex mutex;
        fs::path workerRoot;
        std::shared_ptr<WorkerCreativeExecutionPlan const> plan;
        std::shared_ptr<WorkerCreativeForegroundSnapshot const> published;
        ForegroundLaunchRecord launch;
        std::map<std::wstring, WorkerCreativeScalar> state;
        std::map<std::wstring, WorkerCreativeScalar> localState;
        std::map<std::wstring, WorkerCreativeVec2> nodeTranslation2D;
        std::map<std::wstring, WorkerCreativeVec3> nodeTranslation3D;
        std::map<std::wstring, bool> nodeVisibility;
        std::map<std::wstring, WorkerCreativeTransform3D> cameraTransforms;
        WorkerCreativeWorld2DRuntime world2dRuntime;
        std::wstring activeCampaignScene;
        std::set<std::wstring> completedCampaignScenes;
        struct AnimationState
        {
            float elapsedSeconds = 0.0f;
            bool active = false;
            bool completed = false;
        };
        std::map<std::wstring, AnimationState> animationStates;
        struct ActiveAudioVoice
        {
            IXAudio2SourceVoice* voice = nullptr;
            std::wstring clipId;
        };
        com_ptr<IXAudio2> audioEngine;
        IXAudio2MasteringVoice* masteringVoice = nullptr;
        std::vector<ActiveAudioVoice> activeAudioVoices;
        std::map<std::wstring, std::chrono::steady_clock::time_point>
            timerNext;
        std::set<std::wstring> completedTimers;
        std::set<std::wstring> keyboardDown;
        std::set<std::wstring> activeTriggers;
        std::vector<std::wstring> logs;
        std::wstring focusedElementId;
        std::wstring lastEvent;
        uint64_t semanticEventSequence = 0;
        std::vector<WorkerCreativeSemanticEventObservation>
            recentSemanticEvents;
        uint64_t physicalInputSequence = 0;
        std::vector<WorkerCreativePhysicalInputObservation>
            recentPhysicalInputs;
        bool recordingPhysicalInput = false;
        std::vector<std::wstring> currentPhysicalSemanticEvents;
        std::wstring lastErrorCode;
        uint64_t stateRevision = 0;
        uint64_t frameCount = 0;
        uint64_t audioCueCount = 0;
        uint64_t xvmInvocationCount = 0;
        bool loadedFromPersistentLaunch = false;
        std::chrono::steady_clock::time_point lastTick{};
        GamepadButtons previousGamepadButtons = GamepadButtons::None;
        bool previousLeftTriggerPressed = false;
        bool previousRightTriggerPressed = false;
        std::mutex captureMutex;
        std::condition_variable captureReady;
        std::optional<WorkerCreativeFrameCaptureRequest>
            pendingCapture;
        std::optional<WorkerCreativeFrameCaptureRequest>
            inFlightCapture;
        std::optional<WorkerCreativeFrameCaptureResult>
            completedCapture;

        ~Impl()
        {
            StopAudio();
        }

        void StopAudio()
        {
            for (auto& active : activeAudioVoices)
            {
                if (active.voice != nullptr)
                {
                    active.voice->Stop();
                    active.voice->DestroyVoice();
                    active.voice = nullptr;
                }
            }
            activeAudioVoices.clear();
            if (masteringVoice != nullptr)
            {
                masteringVoice->DestroyVoice();
                masteringVoice = nullptr;
            }
            audioEngine = nullptr;
        }

        void InitializeAudio()
        {
            StopAudio();
            if (!plan || !plan->audio.has_value())
                return;
            auto hr = XAudio2Create(
                audioEngine.put(),
                0,
                XAUDIO2_DEFAULT_PROCESSOR);
            if (FAILED(hr) ||
                FAILED(audioEngine->CreateMasteringVoice(
                    &masteringVoice)))
            {
                StopAudio();
                FailForeground(
                    "xcp.creative.audio_initialization_failed",
                    "creative audio engine could not be initialized",
                    "foreground_activation",
                    "audio",
                    WideToUtf8(plan->projectId),
                    "available packaged XAudio2 engine",
                    "initialization failed",
                    "retry on a host publishing audio.playback",
                    true);
            }
        }

        void Publish()
        {
            auto snapshot =
                std::make_shared<WorkerCreativeForegroundSnapshot>();
            snapshot->plan = plan;
            snapshot->launchId = launch.launchId;
            snapshot->launchRecordSha256 =
                launch.recordSha256;
            snapshot->launchSequence = launch.sequence;
            snapshot->stateRevision = stateRevision;
            snapshot->frameCount = frameCount;
            snapshot->state = state;
            snapshot->localState = localState;
            snapshot->nodeTranslation2D =
                nodeTranslation2D;
            snapshot->nodeTranslation3D =
                nodeTranslation3D;
            snapshot->nodeVisibility = nodeVisibility;
            snapshot->focusedElementId =
                focusedElementId;
            snapshot->cameraTransforms =
                cameraTransforms;
            snapshot->world2dState =
                world2dRuntime.State();
            snapshot->logs = logs;
            snapshot->lastEvent = lastEvent;
            snapshot->semanticEventSequence =
                semanticEventSequence;
            snapshot->recentSemanticEvents =
                recentSemanticEvents;
            snapshot->physicalInputSequence =
                physicalInputSequence;
            snapshot->recentPhysicalInputs =
                recentPhysicalInputs;
            snapshot->lastErrorCode =
                lastErrorCode;
            snapshot->activeAudioVoices =
                activeAudioVoices.size();
            snapshot->audioCueCount =
                audioCueCount;
            snapshot->xvmInvocationCount =
                xvmInvocationCount;
            snapshot->loadedFromPersistentLaunch =
                loadedFromPersistentLaunch;
            published = std::move(snapshot);
        }

        std::wstring World2DStateJson() const
        {
            if (!plan || !plan->world2d.has_value())
                return L"null";
            auto const& runtime = world2dRuntime.State();
            std::wstring result =
                L"{\"completed\":" + BoolJson(runtime.completed) +
                L",\"turn\":" + std::to_wstring(runtime.turn) +
                L",\"entities\":[";
            bool first = true;
            for (auto const& entity : plan->world2d->entities)
            {
                if (!first) result += L",";
                first = false;
                auto position = runtime.entityPositions.at(entity.id);
                result +=
                    L"{\"archetype\":" +
                    JsonString(entity.archetype) +
                    L",\"id\":" + JsonString(entity.id) +
                    L",\"position\":[" +
                    std::to_wstring(position.x) + L"," +
                    std::to_wstring(position.y) +
                    L"],\"visible\":" +
                    BoolJson(runtime.entityVisibility.at(entity.id)) +
                    L"}";
            }
            result += L"],\"inventory\":{";
            first = true;
            for (auto const& [key, value] : runtime.inventory)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(key) + L":" +
                    std::to_wstring(value);
            }
            result += L"},\"completed_objectives\":[";
            first = true;
            for (auto const& id : runtime.completedObjectives)
            {
                if (!first) result += L",";
                first = false;
                result += JsonString(id);
            }
            result += L"]}";
            if (plan->world2dCampaign.has_value())
            {
                result.pop_back();
                result +=
                    L",\"campaign\":{\"active_scene\":" +
                    JsonString(activeCampaignScene) +
                    L",\"completed_scenes\":[";
                first = true;
                for (auto const& id : completedCampaignScenes)
                {
                    if (!first) result += L",";
                    first = false;
                    result += JsonString(id);
                }
                result +=
                    L"],\"scene_count\":" +
                    std::to_wstring(
                        plan->world2dCampaign->scenes.size()) +
                    L"}}";
            }
            return result;
        }

        WorkerCreativeLocalStateDefinition const*
            FindLocalDefinition(std::wstring const& id) const
        {
            if (!plan || !plan->data.has_value())
                return nullptr;
            auto found = std::find_if(
                plan->data->localState.begin(),
                plan->data->localState.end(),
                [&](auto const& definition)
                {
                    return definition.id == id;
                });
            return found == plan->data->localState.end()
                ? nullptr
                : &*found;
        }

        std::wstring StateFileJson() const
        {
            std::wstring campaign = L"null";
            if (plan->world2dCampaign.has_value())
            {
                campaign =
                    L"{\"active_scene\":" +
                    JsonString(activeCampaignScene) +
                    L",\"completed_scenes\":[";
                bool first = true;
                for (auto const& id : completedCampaignScenes)
                {
                    if (!first) campaign += L",";
                    first = false;
                    campaign += JsonString(id);
                }
                campaign += L"]}";
            }
            return L"{\"project_id\":" +
                JsonString(plan->projectId) +
                L",\"schema_version\":\"xcp-creative-project-local-state-v2\"" +
                L",\"state_revision\":" +
                std::to_wstring(stateRevision) +
                L",\"values\":" +
                ScalarMapJson(localState) +
                L",\"campaign\":" +
                campaign +
                L"}\n";
        }

        void SaveLocalState()
        {
            if (!plan ||
                (!plan->data.has_value() &&
                 !plan->world2dCampaign.has_value()))
                return;
            auto bytes = WideToUtf8(StateFileJson());
            if (bytes.size() > MaxLocalStateBytes)
            {
                FailForeground(
                    "xcp.creative.local_state_budget_exceeded",
                    "creative project local-state file exceeds its bound",
                    "foreground_state_persistence",
                    "values",
                    WideToUtf8(plan->projectId),
                    "at most 8388608 bytes",
                    std::to_string(bytes.size()),
                    "reduce saved state");
            }
            auto path = StatePath(workerRoot, plan->projectId);
            fs::create_directories(path.parent_path());
            auto staging = path;
            staging += L".staging";
            if (fs::exists(staging))
                fs::remove(staging);
            {
                std::ofstream output(
                    staging,
                    std::ios::binary | std::ios::trunc);
                output.write(
                    bytes.data(),
                    static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output)
                {
                    fs::remove(staging);
                    FailForeground(
                        "xcp.creative.local_state_write_failed",
                        "creative project local-state staging write failed",
                        "foreground_state_persistence",
                        "values",
                        WideToUtf8(staging.wstring()),
                        "complete app-private write",
                        "write failed",
                        "retry after checking app-private storage",
                        true);
                }
            }
            std::error_code error;
            fs::rename(staging, path, error);
            if (error)
            {
                fs::remove(path, error);
                error.clear();
                fs::rename(staging, path, error);
            }
            if (error)
            {
                fs::remove(staging);
                FailForeground(
                    "xcp.creative.local_state_commit_failed",
                    "creative project local-state atomic replacement failed",
                    "foreground_state_persistence",
                    "values",
                    WideToUtf8(path.wstring()),
                    "atomic app-private replacement",
                    error.message(),
                    "retry after checking app-private storage",
                    true);
            }
        }

        void LoadCampaignState(
            JsonObject const& object,
            std::string const& path)
        {
            RequireExactFields(
                object,
                { L"active_scene", L"completed_scenes" },
                path);
            auto active = RequiredString(
                object,
                L"active_scene",
                path,
                96);
            auto completed = RequiredArray(
                object,
                L"completed_scenes",
                path,
                0,
                128);
            std::set<std::wstring> loadedCompleted;
            for (uint32_t index = 0;
                 index < completed.Size();
                 ++index)
            {
                auto value = completed.GetAt(index);
                if (value.ValueType() != JsonValueType::String)
                {
                    FailForeground(
                        "xcp.creative.local_state_invalid",
                        "creative campaign state contains a non-string scene id",
                        "foreground_state_persistence",
                        "campaign.completed_scenes",
                        path + ".completed_scenes[" +
                            std::to_string(index) + "]",
                        "declared campaign scene id",
                        "non-string",
                        "remove or repair the project-local state file");
                }
                auto id = std::wstring(value.GetString());
                if (id.size() > 96 ||
                    !loadedCompleted.insert(id).second)
                {
                    FailForeground(
                        "xcp.creative.local_state_invalid",
                        "creative campaign state contains an invalid or duplicate completed scene",
                        "foreground_state_persistence",
                        "campaign.completed_scenes",
                        path,
                        "unique declared campaign scene ids",
                        WideToUtf8(id),
                        "remove or repair the project-local state file");
                }
            }

            std::set<std::wstring> expectedCompleted;
            auto reachable = false;
            auto cursor = plan->world2dCampaign->entryScene;
            for (size_t step = 0;
                 step <= plan->world2dCampaign->scenes.size();
                 ++step)
            {
                if (cursor == active)
                {
                    reachable = true;
                    break;
                }
                if (!expectedCompleted.insert(cursor).second)
                    break;
                auto transition = std::find_if(
                    plan->world2dCampaign->transitions.begin(),
                    plan->world2dCampaign->transitions.end(),
                    [&](auto const& candidate)
                    {
                        return candidate.fromScene == cursor &&
                            candidate.event ==
                                L"world.level-completed";
                    });
                if (transition ==
                    plan->world2dCampaign->transitions.end())
                    break;
                cursor = transition->toScene;
            }
            if (!reachable ||
                loadedCompleted != expectedCompleted)
            {
                FailForeground(
                    "xcp.creative.local_state_invalid",
                    "creative campaign state is not a reachable progression prefix",
                    "foreground_state_persistence",
                    "campaign",
                    path,
                    "active scene and completed set reachable from entry_scene",
                    WideToUtf8(active),
                    "remove or repair the project-local state file");
            }

            auto scene = std::find_if(
                plan->world2dCampaign->scenes.begin(),
                plan->world2dCampaign->scenes.end(),
                [&](auto const& candidate)
                {
                    return candidate.id == active;
                });
            if (scene == plan->world2dCampaign->scenes.end())
            {
                FailForeground(
                    "xcp.creative.local_state_invalid",
                    "creative campaign active scene is not declared",
                    "foreground_state_persistence",
                    "campaign.active_scene",
                    path,
                    "declared campaign scene id",
                    WideToUtf8(active),
                    "remove or repair the project-local state file");
            }
            auto world =
                plan->world2dModules.find(scene->moduleId);
            if (world == plan->world2dModules.end())
            {
                FailForeground(
                    "xcp.creative.local_state_invalid",
                    "creative campaign active scene module is unavailable",
                    "foreground_state_persistence",
                    "campaign.active_scene",
                    path,
                    "installed campaign world module",
                    WideToUtf8(scene->moduleId),
                    "reinstall the exact project or remove stale local state");
            }
            activeCampaignScene = std::move(active);
            completedCampaignScenes =
                std::move(loadedCompleted);
        }

        void MaterializeActiveCampaignWorld()
        {
            if (!plan || !plan->world2dCampaign.has_value())
                return;
            auto scene = std::find_if(
                plan->world2dCampaign->scenes.begin(),
                plan->world2dCampaign->scenes.end(),
                [&](auto const& candidate)
                {
                    return candidate.id == activeCampaignScene;
                });
            if (scene == plan->world2dCampaign->scenes.end())
            {
                FailForeground(
                    "xcp.creative.module_reference_invalid",
                    "creative campaign active scene is not declared",
                    "foreground_activation",
                    "campaign.active_scene",
                    WideToUtf8(plan->projectId),
                    "declared campaign scene id",
                    WideToUtf8(activeCampaignScene),
                    "rebuild or reinstall the exact campaign project");
            }
            auto world =
                plan->world2dModules.find(scene->moduleId);
            if (world == plan->world2dModules.end())
            {
                FailForeground(
                    "xcp.creative.module_reference_invalid",
                    "creative campaign active scene module is unavailable",
                    "foreground_activation",
                    "campaign.active_scene",
                    WideToUtf8(plan->projectId),
                    "installed campaign world module",
                    WideToUtf8(scene->moduleId),
                    "rebuild or reinstall the exact campaign project");
            }
            auto updated =
                std::make_shared<WorkerCreativeExecutionPlan>(*plan);
            updated->world2d = world->second;
            plan = std::move(updated);
        }

        void LoadLocalState()
        {
            if (!plan ||
                (!plan->data.has_value() &&
                 !plan->world2dCampaign.has_value()))
                return;
            auto path = StatePath(workerRoot, plan->projectId);
            if (!fs::exists(path))
                return;
            auto bytes = ReadBoundedFile(path, MaxLocalStateBytes);
            auto object = ParseObjectBytes(
                bytes,
                WideToUtf8(path.wstring()));
            auto statePath = WideToUtf8(path.wstring());
            auto schemaVersion = RequiredString(
                object,
                L"schema_version",
                statePath);
            auto legacy =
                schemaVersion ==
                    L"xcp-creative-project-local-state-v1";
            if (legacy)
            {
                RequireExactFields(
                    object,
                    {
                        L"project_id", L"schema_version",
                        L"state_revision", L"values"
                    },
                    statePath);
            }
            else if (
                schemaVersion ==
                    L"xcp-creative-project-local-state-v2")
            {
                RequireExactFields(
                    object,
                    {
                        L"project_id", L"schema_version",
                        L"state_revision", L"values",
                        L"campaign"
                    },
                    statePath);
            }
            else
            {
                FailForeground(
                    "xcp.creative.local_state_invalid",
                    "creative project local-state schema is unsupported",
                    "foreground_state_persistence",
                    "schema_version",
                    statePath,
                    "xcp-creative-project-local-state-v1 or v2",
                    WideToUtf8(schemaVersion),
                    "remove or migrate the project-local state file");
            }
            if (RequiredString(
                    object,
                    L"project_id",
                    statePath) !=
                    plan->projectId)
            {
                FailForeground(
                    "xcp.creative.local_state_invalid",
                    "creative project local-state identity is invalid",
                    "foreground_state_persistence",
                    "project_id/schema_version",
                    WideToUtf8(path.wstring()),
                    WideToUtf8(plan->projectId),
                    "identity mismatch",
                    "remove or repair the project-local state file");
            }
            auto revision = static_cast<uint64_t>(
                RequiredInteger(
                    object,
                    L"state_revision",
                    statePath,
                    0,
                    9007199254740991ll));
            auto values = RequiredObject(
                object,
                L"values",
                statePath);
            std::map<std::wstring, WorkerCreativeScalar> loaded;
            for (auto const& field : values)
            {
                auto id = std::wstring(field.Key());
                auto definition = FindLocalDefinition(id);
                if (definition == nullptr)
                {
                    FailForeground(
                        "xcp.creative.local_state_invalid",
                        "creative project local-state contains an undeclared key",
                        "foreground_state_persistence",
                        "values",
                        WideToUtf8(path.wstring()),
                        "declared local-state id",
                        WideToUtf8(id),
                        "remove the stale key or update the project declaration");
                }
                auto value = ParseScalar(
                    field.Value(),
                    statePath + ".values." +
                        WideToUtf8(id),
                    definition->maxBytes);
                if (!ScalarMatchesType(value, definition->type) ||
                    WideToUtf8(ScalarJson(value)).size() >
                        definition->maxBytes)
                {
                    FailForeground(
                        "xcp.creative.local_state_invalid",
                        "creative project local-state value violates its declaration",
                        "foreground_state_persistence",
                        "values",
                        WideToUtf8(id),
                        WideToUtf8(definition->type) + ":" +
                            std::to_string(definition->maxBytes),
                        "type or byte mismatch",
                        "remove or correct the saved value");
                }
                loaded.emplace(id, std::move(value));
            }
            if (plan->data.has_value())
            {
                for (auto const& definition :
                     plan->data->localState)
                {
                    if (loaded.find(definition.id) == loaded.end())
                    {
                        loaded.emplace(
                            definition.id,
                            definition.defaultValue);
                    }
                }
            }
            if (!legacy)
            {
                auto campaignValue =
                    object.GetNamedValue(L"campaign");
                if (plan->world2dCampaign.has_value())
                {
                    if (campaignValue.ValueType() !=
                        JsonValueType::Object)
                    {
                        FailForeground(
                            "xcp.creative.local_state_invalid",
                            "creative campaign state is missing",
                            "foreground_state_persistence",
                            "campaign",
                            statePath,
                            "campaign progress object",
                            "null or non-object",
                            "remove or repair the project-local state file");
                    }
                    LoadCampaignState(
                        campaignValue.GetObject(),
                        statePath + ".campaign");
                }
                else if (campaignValue.ValueType() !=
                         JsonValueType::Null)
                {
                    FailForeground(
                        "xcp.creative.local_state_invalid",
                        "non-campaign project contains campaign state",
                        "foreground_state_persistence",
                        "campaign",
                        statePath,
                        "null",
                        "object",
                        "remove or repair the project-local state file");
                }
            }
            localState = std::move(loaded);
            stateRevision =
                (std::max)(stateRevision, revision);
            for (auto const& [id, value] : localState)
            {
                auto behavior = state.find(id);
                if (behavior != state.end() &&
                    ScalarMatchesType(
                        value,
                        FindLocalDefinition(id)->type))
                {
                    behavior->second = value;
                }
            }
        }

        WorkerCreativeScalar ResolveOperand(
            WorkerCreativeOperand const& operand) const
        {
            if (!operand.fromState)
                return operand.value;
            auto found = state.find(operand.state);
            if (found == state.end())
            {
                FailForeground(
                    "xcp.creative.behavior_state_missing",
                    "creative behavior state is missing during execution",
                    "foreground_execution",
                    "state",
                    WideToUtf8(operand.state),
                    "declared state",
                    "missing",
                    "reload a valid project plan");
            }
            return found->second;
        }

        bool EvaluateCondition(
            WorkerCreativeCondition const& condition) const
        {
            auto left = ResolveOperand(condition.left);
            auto right = ResolveOperand(condition.right);
            if (condition.operation == L"equal")
                return ScalarEqual(left, right);
            if (condition.operation == L"not_equal")
                return !ScalarEqual(left, right);
            auto comparison = ScalarCompare(left, right);
            if (condition.operation == L"less")
                return comparison < 0;
            if (condition.operation == L"less_equal")
                return comparison <= 0;
            if (condition.operation == L"greater")
                return comparison > 0;
            return comparison >= 0;
        }

        void FocusMove(int direction)
        {
            if (!plan || !plan->ui.has_value())
                return;
            std::vector<std::wstring> focusable;
            for (auto const& element : plan->ui->elements)
            {
                if (!element.focusable)
                    continue;
                if (!element.visibleState.empty())
                {
                    auto visible = state.find(element.visibleState);
                    if (visible == state.end() ||
                        !std::holds_alternative<bool>(visible->second) ||
                        !std::get<bool>(visible->second))
                        continue;
                }
                focusable.push_back(element.id);
            }
            if (focusable.empty())
                return;
            auto found = std::find(
                focusable.begin(),
                focusable.end(),
                focusedElementId);
            auto index = found == focusable.end()
                ? 0
                : static_cast<int>(
                    std::distance(focusable.begin(), found));
            index =
                (index + direction +
                    static_cast<int>(focusable.size())) %
                static_cast<int>(focusable.size());
            focusedElementId = focusable[
                static_cast<size_t>(index)];
            ++stateRevision;
        }

        void AppendLog(std::wstring const& message)
        {
            if (logs.size() >= MaxLogEntries)
                logs.erase(logs.begin());
            logs.push_back(message);
        }

        void PruneAudioVoices()
        {
            auto next = activeAudioVoices.begin();
            while (next != activeAudioVoices.end())
            {
                XAUDIO2_VOICE_STATE voiceState{};
                next->voice->GetState(
                    &voiceState,
                    XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (voiceState.BuffersQueued != 0)
                {
                    ++next;
                    continue;
                }
                next->voice->DestroyVoice();
                next = activeAudioVoices.erase(next);
            }
        }

        void PlayAudioEvent(std::wstring const& event)
        {
            if (!plan || !plan->audio.has_value())
                return;
            PruneAudioVoices();
            for (auto const& cue : plan->audio->cues)
            {
                if (cue.event != event)
                    continue;
                if (activeAudioVoices.size() >=
                    MaxAudioVoices)
                {
                    FailForeground(
                        "xcp.creative.audio_voice_budget_exceeded",
                        "creative project exceeded its concurrent audio voice budget",
                        "foreground_execution",
                        "cues",
                        WideToUtf8(event),
                        "at most 32 concurrent voices",
                        std::to_string(
                            activeAudioVoices.size() + 1),
                        "reduce cue fan-out or shorten looping clips");
                }
                auto clip = std::find_if(
                    plan->audio->clips.begin(),
                    plan->audio->clips.end(),
                    [&](auto const& item)
                    {
                        return item.id == cue.clipId;
                    });
                auto bus = std::find_if(
                    plan->audio->buses.begin(),
                    plan->audio->buses.end(),
                    [&](auto const& item)
                    {
                        return item.id == clip->busId;
                    });
                auto const& asset =
                    plan->assets.at(clip->assetId);
                WAVEFORMATEX format{};
                format.wFormatTag = WAVE_FORMAT_PCM;
                format.nChannels = asset.audioChannels;
                format.nSamplesPerSec =
                    asset.audioSampleRate;
                format.nAvgBytesPerSec =
                    asset.audioAverageBytesPerSecond;
                format.nBlockAlign =
                    asset.audioBlockAlign;
                format.wBitsPerSample =
                    asset.audioBitsPerSample;
                format.cbSize = 0;
                IXAudio2SourceVoice* voice = nullptr;
                auto hr = audioEngine->CreateSourceVoice(
                    &voice,
                    &format);
                XAUDIO2_BUFFER buffer{};
                buffer.Flags = XAUDIO2_END_OF_STREAM;
                buffer.AudioBytes = static_cast<UINT32>(
                    asset.decodedAudio.size());
                buffer.pAudioData =
                    asset.decodedAudio.data();
                buffer.LoopCount = clip->loop
                    ? XAUDIO2_LOOP_INFINITE
                    : 0;
                auto gain = (std::min)(
                    4.0f,
                    cue.gain * clip->gain * bus->gain);
                if (FAILED(hr) ||
                    FAILED(voice->SetVolume(gain)) ||
                    FAILED(voice->SubmitSourceBuffer(
                        &buffer)) ||
                    FAILED(voice->Start()))
                {
                    if (voice != nullptr)
                        voice->DestroyVoice();
                    FailForeground(
                        "xcp.creative.audio_playback_failed",
                        "creative audio cue could not start",
                        "foreground_execution",
                        "cue",
                        WideToUtf8(cue.id),
                        "bounded decoded PCM voice",
                        "XAudio2 operation failed",
                        "retry or correct the published audio asset",
                        true);
                }
                activeAudioVoices.push_back(
                    { voice, clip->id });
                ++audioCueCount;
            }
        }

        uint32_t ResolveXvmInput(
            WorkerCreativeXvmInput const& input) const
        {
            if (!input.fromState)
                return input.constant;
            auto value = state.find(input.stateId);
            if (value == state.end())
            {
                FailForeground(
                    "xcp.creative.xvm_input_missing",
                    "creative XVM input state is missing",
                    "foreground_execution",
                    "inputs[].state_id",
                    WideToUtf8(input.stateId),
                    "declared integer or boolean state",
                    "missing",
                    "reload a valid project plan");
            }
            if (std::holds_alternative<bool>(value->second))
                return std::get<bool>(value->second) ? 1u : 0u;
            if (!std::holds_alternative<int64_t>(
                    value->second))
            {
                FailForeground(
                    "xcp.creative.xvm_input_type_mismatch",
                    "creative XVM input is not word-compatible",
                    "foreground_execution",
                    "inputs[].state_id",
                    WideToUtf8(input.stateId),
                    "integer or boolean",
                    "other type",
                    "correct the state binding");
            }
            auto integer = std::get<int64_t>(value->second);
            if (integer < INT32_MIN ||
                integer >
                    static_cast<int64_t>(UINT32_MAX))
            {
                FailForeground(
                    "xcp.creative.xvm_input_range_exceeded",
                    "creative XVM input exceeds one word",
                    "foreground_execution",
                    "inputs[].state_id",
                    WideToUtf8(input.stateId),
                    "-2147483648..4294967295",
                    std::to_string(integer),
                    "clamp or transform the state before invocation");
            }
            return static_cast<uint32_t>(integer);
        }

        void RunXvmServices(
            std::wstring const& event,
            std::vector<std::wstring>& pending,
            size_t& invocations)
        {
            if (!plan || !plan->xvm.has_value())
                return;
            for (auto const& service :
                 plan->xvm->services)
            {
                if (service.triggerEvent != event)
                    continue;
                if (++invocations >
                    MaxXvmInvocationsPerTick)
                {
                    FailForeground(
                        "xcp.creative.xvm_invocation_budget_exceeded",
                        "creative event dispatch exceeded its XVM invocation budget",
                        "foreground_execution",
                        "services",
                        WideToUtf8(event),
                        "at most 32 invocations per event dispatch",
                        std::to_string(invocations),
                        "remove the service event cycle or reduce fan-out");
                }
                ++xvmInvocationCount;
                std::array<
                    uint32_t,
                    WorkerXvmInputWordCountValue> inputs{};
                for (size_t index = 0;
                     index < inputs.size();
                     ++index)
                {
                    inputs[index] =
                        ResolveXvmInput(
                            service.inputs[index]);
                }
                try
                {
                    WorkerGraphNodeResourceLimits limits{
                        service.program.maxFuel,
                        service.program.memoryBytes,
                        service.program.outputBytes
                    };
                    auto result =
                        WorkerRunXvmCpuReference(
                            service.program,
                            limits,
                            inputs);
                    if (result.controlToken != L"pass")
                    {
                        lastErrorCode =
                            L"xcp.creative.xvm_control_failed";
                        AppendLog(
                            L"XVM service failed control: " +
                            service.id);
                        pending.push_back(
                            service.failureEvent);
                        continue;
                    }
                    for (auto const& output :
                         service.outputs)
                    {
                        auto offset =
                            static_cast<size_t>(
                                output.offsetBytes);
                        auto word =
                            static_cast<uint32_t>(
                                result.output[offset]) |
                            (static_cast<uint32_t>(
                                result.output[offset + 1]) << 8) |
                            (static_cast<uint32_t>(
                                result.output[offset + 2]) << 16) |
                            (static_cast<uint32_t>(
                                result.output[offset + 3]) << 24);
                        state.at(output.stateId) =
                            static_cast<int64_t>(word);
                    }
                    ++stateRevision;
                    pending.push_back(
                        service.successEvent);
                }
                catch (WorkerXvmError const& error)
                {
                    lastErrorCode =
                        L"xcp.creative.xvm_execution_failed";
                    AppendLog(
                        L"XVM service error " +
                        service.id + L": " +
                        Utf8ToWide(error.code));
                    pending.push_back(
                        service.failureEvent);
                }
                catch (WorkerGraphResourceError const& error)
                {
                    lastErrorCode =
                        L"xcp.creative.xvm_resource_failed";
                    AppendLog(
                        L"XVM service resource error " +
                        service.id + L": " +
                        Utf8ToWide(error.code));
                    pending.push_back(
                        service.failureEvent);
                }
            }
        }

        void StartAnimationsForEvent(
            std::wstring const& event)
        {
            if (!plan || !plan->scene3d.has_value())
                return;
            for (auto const& animation :
                 plan->scene3d->animations)
            {
                if (animation.startEvent != event)
                    continue;
                auto& runtime =
                    animationStates[animation.id];
                runtime.elapsedSeconds = 0.0f;
                runtime.active = true;
                runtime.completed = false;
                nodeTranslation3D[animation.targetNode] =
                    animation.from;
                ++stateRevision;
            }
        }

        void UpdateAnimations(float seconds)
        {
            if (!plan || !plan->scene3d.has_value())
                return;
            std::vector<std::wstring> completedEvents;
            for (auto const& animation :
                 plan->scene3d->animations)
            {
                auto& runtime =
                    animationStates[animation.id];
                if (!runtime.active)
                    continue;
                runtime.elapsedSeconds += seconds;
                auto duration =
                    static_cast<float>(
                        animation.durationMs) /
                    1000.0f;
                auto progress =
                    runtime.elapsedSeconds / duration;
                if (animation.loop)
                {
                    progress = std::fmod(
                        progress,
                        1.0f);
                }
                else if (progress >= 1.0f)
                {
                    progress = 1.0f;
                    runtime.active = false;
                    runtime.completed = true;
                    if (!animation.completeEvent.empty())
                        completedEvents.push_back(
                            animation.completeEvent);
                }
                WorkerCreativeVec3 value;
                value.x = animation.from.x +
                    (animation.to.x -
                        animation.from.x) * progress;
                value.y = animation.from.y +
                    (animation.to.y -
                        animation.from.y) * progress;
                value.z = animation.from.z +
                    (animation.to.z -
                        animation.from.z) * progress;
                nodeTranslation3D[
                    animation.targetNode] = value;
                ++stateRevision;
            }
            for (auto const& event : completedEvents)
                DispatchEvent(event);
        }

        void ExecuteAction(
            WorkerCreativeAction const& action,
            std::vector<std::wstring>& pending)
        {
            if (action.type == L"state.set")
            {
                auto found = state.find(action.target);
                if (found == state.end() ||
                    !action.value.has_value())
                    FailForeground(
                        "xcp.creative.behavior_state_missing",
                        "creative state.set target is missing",
                        "foreground_execution",
                        "target",
                        WideToUtf8(action.target),
                        "declared state",
                        "missing",
                        "correct the behavior action");
                auto const& definition = *std::find_if(
                    plan->behavior->state.begin(),
                    plan->behavior->state.end(),
                    [&](auto const& item)
                    {
                        return item.id == action.target;
                    });
                if (!ScalarMatchesType(
                        *action.value,
                        definition.type))
                    FailForeground(
                        "xcp.creative.behavior_type_mismatch",
                        "creative state.set value type does not match target",
                        "foreground_execution",
                        "value",
                        WideToUtf8(action.target),
                        WideToUtf8(definition.type),
                        "type mismatch",
                        "correct the behavior action");
                found->second = *action.value;
            }
            else if (action.type == L"state.add")
            {
                auto found = state.find(action.target);
                if (found == state.end() ||
                    !action.value.has_value())
                    FailForeground(
                        "xcp.creative.behavior_state_missing",
                        "creative state.add target is missing",
                        "foreground_execution",
                        "target",
                        WideToUtf8(action.target),
                        "declared numeric state",
                        "missing",
                        "correct the behavior action");
                auto sum =
                    ScalarNumber(found->second) +
                    ScalarNumber(*action.value);
                if (!std::isfinite(sum) ||
                    sum < -1000000000.0 ||
                    sum > 1000000000.0)
                    FailForeground(
                        "xcp.creative.behavior_numeric_overflow",
                        "creative state.add exceeded its numeric bound",
                        "foreground_execution",
                        "value",
                        WideToUtf8(action.target),
                        "-1000000000..1000000000",
                        std::to_string(sum),
                        "reduce the increment or reset the state");
                if (std::holds_alternative<int64_t>(
                        found->second) &&
                    std::holds_alternative<int64_t>(
                        *action.value))
                    found->second =
                        static_cast<int64_t>(sum);
                else
                    found->second = sum;
            }
            else if (action.type == L"state.toggle")
            {
                auto found = state.find(action.target);
                if (found == state.end() ||
                    !std::holds_alternative<bool>(
                        found->second))
                    FailForeground(
                        "xcp.creative.behavior_type_mismatch",
                        "creative state.toggle target is not boolean",
                        "foreground_execution",
                        "target",
                        WideToUtf8(action.target),
                        "declared boolean state",
                        "missing or other type",
                        "correct the behavior action");
                found->second =
                    !std::get<bool>(found->second);
            }
            else if (action.type == L"event.emit")
            {
                pending.push_back(action.event);
            }
            else if (action.type == L"node.visibility")
            {
                if (!std::holds_alternative<bool>(
                        *action.value))
                    FailForeground(
                        "xcp.creative.behavior_type_mismatch",
                        "creative node visibility value is not boolean",
                        "foreground_execution",
                        "value",
                        WideToUtf8(action.target),
                        "boolean",
                        "other type",
                        "correct the behavior action");
                nodeVisibility[action.target] =
                    std::get<bool>(*action.value);
            }
            else if (action.type == L"node.translate2d")
            {
                auto& value =
                    nodeTranslation2D[action.target];
                value.x +=
                    static_cast<float>(action.vector[0]);
                value.y +=
                    static_cast<float>(action.vector[1]);
            }
            else if (action.type == L"node.translate3d")
            {
                auto& value =
                    nodeTranslation3D[action.target];
                value.x +=
                    static_cast<float>(action.vector[0]);
                value.y +=
                    static_cast<float>(action.vector[1]);
                value.z +=
                    static_cast<float>(action.vector[2]);
            }
            else if (action.type == L"ui.focus")
            {
                focusedElementId = action.target;
            }
            else if (action.type == L"state.save")
            {
                auto source = action.source.empty()
                    ? action.target
                    : action.source;
                auto sourceValue = state.find(source);
                auto definition =
                    FindLocalDefinition(action.target);
                if (sourceValue == state.end() ||
                    definition == nullptr ||
                    !ScalarMatchesType(
                        sourceValue->second,
                        definition->type) ||
                    WideToUtf8(
                        ScalarJson(sourceValue->second)).size() >
                        definition->maxBytes)
                    FailForeground(
                        "xcp.creative.local_state_binding_invalid",
                        "creative state.save binding violates its declaration",
                        "foreground_state_persistence",
                        "source/target",
                        WideToUtf8(
                            source + L"/" +
                            action.target),
                        "declared compatible behavior and local state",
                        "missing or type/byte mismatch",
                        "correct the save source and target");
                localState[action.target] =
                    sourceValue->second;
                SaveLocalState();
            }
            else if (action.type == L"state.load")
            {
                LoadLocalState();
                auto source = action.source.empty()
                    ? action.target
                    : action.source;
                auto value = localState.find(source);
                auto target = state.find(action.target);
                if (value == localState.end() ||
                    target == state.end())
                    FailForeground(
                        "xcp.creative.local_state_binding_invalid",
                        "creative state.load binding is missing",
                        "foreground_state_persistence",
                        "source/target",
                        WideToUtf8(
                            source + L"/" +
                            action.target),
                        "declared local source and behavior target",
                        "missing",
                        "correct the load source and target");
                target->second = value->second;
            }
            else if (action.type == L"log.write")
            {
                AppendLog(action.message);
            }
            ++stateRevision;
        }

        void RecordSemanticEvent(std::wstring const& event)
        {
            if (recordingPhysicalInput)
                currentPhysicalSemanticEvents.push_back(event);
            recentSemanticEvents.push_back(
                WorkerCreativeSemanticEventObservation{
                    ++semanticEventSequence,
                    event,
                    stateRevision });
            if (recentSemanticEvents.size() >
                MaxRecentSemanticEvents)
            {
                recentSemanticEvents.erase(
                    recentSemanticEvents.begin());
            }
        }

        void BeginPhysicalInput()
        {
            currentPhysicalSemanticEvents.clear();
            recordingPhysicalInput = true;
        }

        void RecordPhysicalInput(std::wstring const& source)
        {
            recentPhysicalInputs.push_back(
                WorkerCreativePhysicalInputObservation{
                    ++physicalInputSequence,
                    source,
                    focusedElementId,
                    currentPhysicalSemanticEvents,
                    stateRevision });
            recordingPhysicalInput = false;
            currentPhysicalSemanticEvents.clear();
            if (recentPhysicalInputs.size() >
                MaxRecentPhysicalInputs)
            {
                recentPhysicalInputs.erase(
                    recentPhysicalInputs.begin());
            }
        }

        void DispatchDeclaredPhysicalSource(
            std::wstring const& source)
        {
            if (!plan)
                return;
            std::set<std::wstring> actions;
            if (plan->canvas.has_value())
            {
                for (auto const& binding :
                     plan->canvas->inputBindings)
                {
                    if (binding.source == source)
                        actions.insert(binding.action);
                }
            }
            if (plan->scene3d.has_value())
            {
                for (auto const& binding :
                     plan->scene3d->inputBindings)
                {
                    if (binding.source == source)
                        actions.insert(binding.action);
                }
            }
            for (auto const& action : actions)
                DispatchEvent(action);
        }

        void DispatchEvent(std::wstring const& event)
        {
            if (!plan)
            {
                lastEvent = event;
                return;
            }
            std::vector<std::wstring> pending{ event };
            size_t processed = 0;
            size_t xvmInvocations = 0;
            while (!pending.empty())
            {
                if (++processed > MaxEventsPerTick)
                    FailForeground(
                        "xcp.creative.behavior_event_budget_exceeded",
                        "creative behavior emitted too many events in one tick",
                        "foreground_execution",
                        "event",
                        WideToUtf8(event),
                        "at most 256 events per tick",
                        std::to_string(processed),
                        "remove the event cycle or reduce fan-out");
                auto current = pending.front();
                pending.erase(pending.begin());
                lastEvent = current;
                StartAnimationsForEvent(current);
                PlayAudioEvent(current);
                RunXvmServices(
                    current,
                    pending,
                    xvmInvocations);
                if (!plan->behavior.has_value())
                {
                    RecordSemanticEvent(current);
                    continue;
                }
                for (auto const& rule :
                     plan->behavior->rules)
                {
                    if (rule.event != current)
                        continue;
                    auto matches = std::all_of(
                        rule.conditions.begin(),
                        rule.conditions.end(),
                        [&](auto const& condition)
                        {
                            return EvaluateCondition(condition);
                        });
                    if (!matches)
                        continue;
                    for (auto const& action : rule.actions)
                        ExecuteAction(action, pending);
                }
                RecordSemanticEvent(current);
            }
        }

        void ActivateFocusedElement()
        {
            if (!plan || !plan->ui.has_value())
                return;
            auto found = std::find_if(
                plan->ui->elements.begin(),
                plan->ui->elements.end(),
                [&](auto const& element)
                {
                    return element.id ==
                        focusedElementId;
                });
            if (found != plan->ui->elements.end() &&
                !found->action.empty())
            {
                DispatchEvent(found->action);
            }
        }

        void ApplySceneBinding(
            WorkerCreativeInputBinding const& binding,
            float x,
            float y,
            float seconds)
        {
            auto camera =
                cameraTransforms.find(binding.target);
            if (camera == cameraTransforms.end())
            {
                DispatchEvent(binding.action);
                return;
            }
            auto amount = binding.scale * seconds;
            if (binding.source == L"gamepad.right_stick")
            {
                camera->second.rotationDegrees.y +=
                    x * amount;
                camera->second.rotationDegrees.x =
                    (std::max)(
                        -89.0f,
                        (std::min)(
                            89.0f,
                            camera->second.rotationDegrees.x -
                                y * amount));
            }
            else
            {
                auto yaw =
                    camera->second.rotationDegrees.y *
                    3.14159265358979323846f / 180.0f;
                camera->second.position.x +=
                    (x * std::cos(yaw) +
                        y * std::sin(yaw)) *
                    amount;
                camera->second.position.z +=
                    (y * std::cos(yaw) -
                        x * std::sin(yaw)) *
                    amount;
            }
            ++stateRevision;
        }

        void ApplyCanvasBinding(
            WorkerCreativeInputBinding const& binding,
            float x,
            float y,
            float seconds)
        {
            if (!binding.target.empty())
            {
                auto& translation =
                    nodeTranslation2D[binding.target];
                translation.x +=
                    x * binding.scale * seconds;
                translation.y +=
                    y * binding.scale * seconds;
                ++stateRevision;
            }
            DispatchEvent(binding.action);
        }

        void CheckCollisions()
        {
            if (!plan || !plan->scene3d.has_value())
                return;
            auto camera = cameraTransforms.find(
                plan->scene3d->activeCamera);
            if (camera == cameraTransforms.end())
                return;
            std::set<std::wstring> nowInside;
            for (auto const& node : plan->scene3d->nodes)
            {
                if (!node.collider.has_value() ||
                    !node.collider->isTrigger)
                    continue;
                auto position = node.transform.position;
                auto translation =
                    nodeTranslation3D.find(node.id);
                if (translation != nodeTranslation3D.end())
                {
                    position.x += translation->second.x;
                    position.y += translation->second.y;
                    position.z += translation->second.z;
                }
                auto dx =
                    camera->second.position.x - position.x;
                auto dy =
                    camera->second.position.y - position.y;
                auto dz =
                    camera->second.position.z - position.z;
                bool inside = false;
                if (node.collider->shape == L"sphere")
                {
                    auto radius =
                        node.collider->radius *
                        (std::max)(
                            node.transform.scale.x,
                            (std::max)(
                                node.transform.scale.y,
                                node.transform.scale.z));
                    inside =
                        dx * dx + dy * dy + dz * dz <=
                        radius * radius;
                }
                else if (node.collider->shape == L"box")
                {
                    inside =
                        std::fabs(dx) <=
                            node.collider->size.x * 0.5f &&
                        std::fabs(dy) <=
                            node.collider->size.y * 0.5f &&
                        std::fabs(dz) <=
                            node.collider->size.z * 0.5f;
                }
                else
                {
                    auto half =
                        (std::max)(
                            node.collider->height * 0.5f,
                            node.collider->radius);
                    inside =
                        std::fabs(dy) <= half &&
                        dx * dx + dz * dz <=
                            node.collider->radius *
                            node.collider->radius;
                }
                if (inside)
                {
                    nowInside.insert(node.id);
                    if (activeTriggers.find(node.id) ==
                        activeTriggers.end())
                    {
                        DispatchEvent(
                            L"collision." + node.id +
                            L"-enter");
                    }
                }
            }
            activeTriggers = std::move(nowInside);
        }

        WorkerCreativeWorld2DDispatchResult DispatchWorldAction(
            std::wstring const& action)
        {
            auto result = world2dRuntime.Dispatch(action);
            if (!result.handled)
                return result;
            if (result.changed)
                ++stateRevision;
            for (auto const& event : result.events)
            {
                DispatchEvent(event);
            }
            return result;
        }

        WorkerCreativeWorld2DDispatchResult DispatchCampaignAction(
            std::wstring const& action)
        {
            WorkerCreativeWorld2DDispatchResult result;
            if (!plan || !plan->world2dCampaign.has_value())
                return result;
            auto binding = std::find_if(
                plan->world2dCampaign->inputBindings.begin(),
                plan->world2dCampaign->inputBindings.end(),
                [&](auto const& candidate)
                {
                    return candidate.action == action;
                });
            if (binding ==
                plan->world2dCampaign->inputBindings.end())
                return result;
            result.handled = true;
            if (binding->command != L"advance_scene" ||
                !world2dRuntime.State().completed)
                return result;
            auto transition = std::find_if(
                plan->world2dCampaign->transitions.begin(),
                plan->world2dCampaign->transitions.end(),
                [&](auto const& candidate)
                {
                    return candidate.fromScene ==
                        activeCampaignScene &&
                        candidate.event ==
                        L"world.level-completed";
                });
            if (transition ==
                plan->world2dCampaign->transitions.end())
                return result;
            auto scene = std::find_if(
                plan->world2dCampaign->scenes.begin(),
                plan->world2dCampaign->scenes.end(),
                [&](auto const& candidate)
                {
                    return candidate.id == transition->toScene;
                });
            if (scene == plan->world2dCampaign->scenes.end())
                return result;
            auto world =
                plan->world2dModules.find(scene->moduleId);
            if (world == plan->world2dModules.end())
                return result;

            completedCampaignScenes.insert(activeCampaignScene);
            auto updated =
                std::make_shared<WorkerCreativeExecutionPlan>(*plan);
            updated->world2d = world->second;
            plan = std::move(updated);
            activeCampaignScene = scene->id;
            world2dRuntime.Activate(
                &*plan->world2d,
                plan->world2dMotion.has_value()
                    ? &*plan->world2dMotion
                    : nullptr);
            auto backdrop =
                nodeVisibility.find(L"hud.complete-backdrop");
            if (backdrop != nodeVisibility.end())
                backdrop->second = false;
            auto label = nodeVisibility.find(L"hud.complete");
            if (label != nodeVisibility.end())
                label->second = false;
            ++stateRevision;
            SaveLocalState();
            result.changed = true;
            result.events.push_back(L"campaign.scene-changed");
            return result;
        }

        WorkerCreativeWorld2DDispatchResult DispatchSemanticAction(
            std::wstring const& action)
        {
            auto result = DispatchCampaignAction(action);
            if (result.handled)
            {
                for (auto const& event : result.events)
                    DispatchEvent(event);
                return result;
            }
            result = DispatchWorldAction(action);
            if (result.handled || !plan)
                return result;
            auto declared = false;
            auto matchesInput =
                [&](auto const& binding)
                {
                    return binding.action == action;
                };
            if (plan->canvas.has_value())
            {
                declared = std::any_of(
                    plan->canvas->inputBindings.begin(),
                    plan->canvas->inputBindings.end(),
                    matchesInput);
            }
            if (!declared && plan->scene3d.has_value())
            {
                declared = std::any_of(
                    plan->scene3d->inputBindings.begin(),
                    plan->scene3d->inputBindings.end(),
                    matchesInput);
            }
            if (!declared && plan->ui.has_value())
            {
                declared = std::any_of(
                    plan->ui->elements.begin(),
                    plan->ui->elements.end(),
                    [&](auto const& element)
                    {
                        return element.action == action;
                    });
            }
            if (!declared && plan->behavior.has_value())
            {
                declared = std::any_of(
                    plan->behavior->rules.begin(),
                    plan->behavior->rules.end(),
                    [&](auto const& rule)
                    {
                        return rule.event == action;
                    });
            }
            if (!declared && plan->audio.has_value())
            {
                declared = std::any_of(
                    plan->audio->cues.begin(),
                    plan->audio->cues.end(),
                    [&](auto const& cue)
                    {
                        return cue.event == action;
                    });
            }
            if (!declared && plan->xvm.has_value())
            {
                declared = std::any_of(
                    plan->xvm->services.begin(),
                    plan->xvm->services.end(),
                    [&](auto const& service)
                    {
                        return service.triggerEvent == action;
                    });
            }
            if (!declared)
                return result;
            auto before = stateRevision;
            DispatchEvent(action);
            result.handled = true;
            result.changed = stateRevision != before;
            result.events.push_back(action);
            return result;
        }

        void DispatchWorldPhysicalSource(
            std::wstring const& source)
        {
            if (!plan || !plan->world2d.has_value())
                return;
            if (plan->world2dCampaign.has_value())
            {
                for (auto const& binding :
                     plan->world2dCampaign->inputBindings)
                {
                    if (binding.source == source)
                    {
                        auto campaign =
                            DispatchCampaignAction(binding.action);
                        for (auto const& event : campaign.events)
                            DispatchEvent(event);
                        if (campaign.changed)
                            return;
                    }
                }
            }
            for (auto const& binding :
                 plan->world2d->inputBindings)
            {
                if (binding.source == source)
                    DispatchWorldAction(binding.action);
            }
        }

        void ActivatePlan(
            std::shared_ptr<WorkerCreativeExecutionPlan const>
                nextPlan,
            ForegroundLaunchRecord const& nextLaunch,
            bool fromPersistent)
        {
            StopAudio();
            plan = std::move(nextPlan);
            launch = nextLaunch;
            state.clear();
            localState.clear();
            nodeTranslation2D.clear();
            nodeTranslation3D.clear();
            nodeVisibility.clear();
            cameraTransforms.clear();
            animationStates.clear();
            timerNext.clear();
            completedTimers.clear();
            activeTriggers.clear();
            logs.clear();
            lastEvent.clear();
            semanticEventSequence = 0;
            recentSemanticEvents.clear();
            physicalInputSequence = 0;
            recentPhysicalInputs.clear();
            recordingPhysicalInput = false;
            currentPhysicalSemanticEvents.clear();
            lastErrorCode.clear();
            stateRevision = 0;
            frameCount = 0;
            audioCueCount = 0;
            xvmInvocationCount = 0;
            previousGamepadButtons = GamepadButtons::None;
            previousLeftTriggerPressed = false;
            previousRightTriggerPressed = false;
            activeCampaignScene =
                plan->world2dCampaign.has_value()
                    ? plan->world2dCampaign->entryScene
                    : std::wstring();
            completedCampaignScenes.clear();
            loadedFromPersistentLaunch =
                fromPersistent;
            lastTick = std::chrono::steady_clock::now();
            if (plan->behavior.has_value())
            {
                for (auto const& definition :
                     plan->behavior->state)
                    state.emplace(
                        definition.id,
                        definition.initial);
                for (auto const& timer :
                     plan->behavior->timers)
                    timerNext.emplace(
                        timer.id,
                        lastTick +
                            std::chrono::milliseconds(
                                timer.intervalMs));
            }
            if (plan->data.has_value())
            {
                for (auto const& definition :
                     plan->data->localState)
                    localState.emplace(
                        definition.id,
                        definition.defaultValue);
            }
            LoadLocalState();
            // Fresh entry scenes and restored scenes must cross the same
            // immutable materialization boundary before the runtime and the
            // renderer publish their first snapshot.
            MaterializeActiveCampaignWorld();
            world2dRuntime.Activate(
                plan->world2d.has_value()
                    ? &*plan->world2d
                    : nullptr,
                plan->world2dMotion.has_value()
                    ? &*plan->world2dMotion
                    : nullptr);
            if (plan->canvas.has_value())
            {
                for (auto const& node : plan->canvas->nodes)
                    nodeVisibility[node.id] =
                        node.visible;
            }
            if (plan->ui.has_value())
                focusedElementId =
                    plan->ui->initialFocusId;
            else
                focusedElementId.clear();
            if (plan->scene3d.has_value())
            {
                for (auto const& node : plan->scene3d->nodes)
                    nodeVisibility[node.id] =
                        node.visible;
                for (auto const& camera :
                     plan->scene3d->cameras)
                    cameraTransforms.emplace(
                        camera.id,
                        camera.transform);
                for (auto const& animation :
                     plan->scene3d->animations)
                {
                    AnimationState runtime;
                    runtime.active =
                        animation.autoplay;
                    animationStates.emplace(
                        animation.id,
                        runtime);
                    if (animation.autoplay)
                    {
                        nodeTranslation3D[
                            animation.targetNode] =
                            animation.from;
                    }
                }
            }
            InitializeAudio();
            AppendLog(
                L"foreground loaded " +
                plan->projectId + L"@" +
                plan->projectVersion);
            Publish();
        }

        void Tick()
        {
            if (!plan)
                return;
            auto now = std::chrono::steady_clock::now();
            auto seconds =
                std::chrono::duration<float>(
                    now - lastTick).count();
            seconds = (std::max)(
                0.0f,
                (std::min)(seconds, 0.1f));
            lastTick = now;
            try
            {
                if (plan->behavior.has_value())
                {
                    for (auto const& timer :
                         plan->behavior->timers)
                    {
                        if (completedTimers.find(timer.id) !=
                            completedTimers.end())
                            continue;
                        auto& next = timerNext[timer.id];
                        uint32_t firings = 0;
                        while (now >= next && firings < 8)
                        {
                            DispatchEvent(timer.event);
                            ++firings;
                            if (!timer.repeating)
                            {
                                completedTimers.insert(timer.id);
                                break;
                            }
                            next += std::chrono::milliseconds(
                                timer.intervalMs);
                        }
                    }
                }
                auto pads = Gamepad::Gamepads();
                if (pads.Size() != 0)
                {
                    auto reading =
                        pads.GetAt(0).GetCurrentReading();
                    auto deadzone = [](double value)
                    {
                        return std::fabs(value) < 0.18
                            ? 0.0f
                            : static_cast<float>(value);
                    };
                    auto leftX =
                        deadzone(reading.LeftThumbstickX);
                    auto leftY =
                        deadzone(reading.LeftThumbstickY);
                    auto rightX =
                        deadzone(reading.RightThumbstickX);
                    auto rightY =
                        deadzone(reading.RightThumbstickY);
                    if (plan->scene3d.has_value())
                    {
                        for (auto const& binding :
                             plan->scene3d->inputBindings)
                        {
                            if (binding.source ==
                                L"gamepad.left_stick")
                                ApplySceneBinding(
                                    binding,
                                    leftX,
                                    leftY,
                                    seconds);
                            else if (binding.source ==
                                L"gamepad.right_stick")
                                ApplySceneBinding(
                                    binding,
                                    rightX,
                                    rightY,
                                    seconds);
                        }
                    }
                    if (plan->canvas.has_value())
                    {
                        for (auto const& binding :
                             plan->canvas->inputBindings)
                        {
                            if (binding.source ==
                                L"gamepad.left_stick")
                                ApplyCanvasBinding(
                                    binding,
                                    leftX,
                                    -leftY,
                                    seconds);
                            else if (binding.source ==
                                L"gamepad.right_stick")
                                ApplyCanvasBinding(
                                    binding,
                                    rightX,
                                    -rightY,
                                    seconds);
                        }
                    }
                    auto buttons = reading.Buttons;
                    auto pressed =
                        static_cast<uint32_t>(buttons) &
                        ~static_cast<uint32_t>(
                            previousGamepadButtons);
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::DPadUp)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(
                            L"gamepad.dpad_up");
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.dpad_up");
                        FocusMove(-1);
                        RecordPhysicalInput(
                            L"gamepad.dpad_up");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::DPadDown)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(
                            L"gamepad.dpad_down");
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.dpad_down");
                        FocusMove(1);
                        RecordPhysicalInput(
                            L"gamepad.dpad_down");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::DPadLeft)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(
                            L"gamepad.dpad_left");
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.dpad_left");
                        FocusMove(-1);
                        RecordPhysicalInput(
                            L"gamepad.dpad_left");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::DPadRight)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(
                            L"gamepad.dpad_right");
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.dpad_right");
                        FocusMove(1);
                        RecordPhysicalInput(
                            L"gamepad.dpad_right");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::A)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(L"gamepad.a");
                        DispatchDeclaredPhysicalSource(L"gamepad.a");
                        ActivateFocusedElement();
                        RecordPhysicalInput(L"gamepad.a");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::B)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(L"gamepad.b");
                        DispatchDeclaredPhysicalSource(L"gamepad.b");
                        RecordPhysicalInput(L"gamepad.b");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::X)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(L"gamepad.x");
                        DispatchDeclaredPhysicalSource(L"gamepad.x");
                        RecordPhysicalInput(L"gamepad.x");
                    }
                    if ((pressed &
                            static_cast<uint32_t>(
                                GamepadButtons::Y)) != 0)
                    {
                        BeginPhysicalInput();
                        DispatchWorldPhysicalSource(L"gamepad.y");
                        DispatchDeclaredPhysicalSource(L"gamepad.y");
                        RecordPhysicalInput(L"gamepad.y");
                    }
                    auto leftTriggerPressed =
                        reading.LeftTrigger >= 0.5;
                    if (leftTriggerPressed &&
                        !previousLeftTriggerPressed)
                    {
                        BeginPhysicalInput();
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.left_trigger");
                        RecordPhysicalInput(
                            L"gamepad.left_trigger");
                    }
                    auto rightTriggerPressed =
                        reading.RightTrigger >= 0.5;
                    if (rightTriggerPressed &&
                        !previousRightTriggerPressed)
                    {
                        BeginPhysicalInput();
                        DispatchDeclaredPhysicalSource(
                            L"gamepad.right_trigger");
                        RecordPhysicalInput(
                            L"gamepad.right_trigger");
                    }
                    previousGamepadButtons = buttons;
                    previousLeftTriggerPressed =
                        leftTriggerPressed;
                    previousRightTriggerPressed =
                        rightTriggerPressed;
                }
                else
                {
                    previousGamepadButtons = GamepadButtons::None;
                    previousLeftTriggerPressed = false;
                    previousRightTriggerPressed = false;
                }
                if (plan->scene3d.has_value() &&
                    (!keyboardDown.empty()))
                {
                    auto x =
                        (keyboardDown.count(L"d") ||
                            keyboardDown.count(L"right") ? 1.0f : 0.0f) -
                        (keyboardDown.count(L"a") ||
                            keyboardDown.count(L"left") ? 1.0f : 0.0f);
                    auto y =
                        (keyboardDown.count(L"w") ||
                            keyboardDown.count(L"up") ? 1.0f : 0.0f) -
                        (keyboardDown.count(L"s") ||
                            keyboardDown.count(L"down") ? 1.0f : 0.0f);
                    for (auto const& binding :
                         plan->scene3d->inputBindings)
                    {
                        if (binding.source == L"keyboard.wasd" ||
                            binding.source == L"keyboard.arrows")
                            ApplySceneBinding(
                                binding,
                                x,
                                y,
                                seconds);
                    }
                }
                auto worldMotion = world2dRuntime.Tick(seconds);
                if (worldMotion.changed)
                    ++stateRevision;
                for (auto const& event : worldMotion.events)
                    DispatchEvent(event);
                UpdateAnimations(seconds);
                PruneAudioVoices();
                CheckCollisions();
                ++frameCount;
            }
            catch (WorkerCreativeHostError const& error)
            {
                recordingPhysicalInput = false;
                currentPhysicalSemanticEvents.clear();
                lastErrorCode = Utf8ToWide(error.code);
                AppendLog(
                    L"runtime error: " +
                    Utf8ToWide(error.code));
            }
            Publish();
        }

        static std::wstring KeyName(VirtualKey key)
        {
            switch (key)
            {
            case VirtualKey::W: return L"w";
            case VirtualKey::A: return L"a";
            case VirtualKey::S: return L"s";
            case VirtualKey::D: return L"d";
            case VirtualKey::Up: return L"up";
            case VirtualKey::Down: return L"down";
            case VirtualKey::Left: return L"left";
            case VirtualKey::Right: return L"right";
            default: return {};
            }
        }

        bool KeyDown(VirtualKey key)
        {
            if (!plan)
                return false;
            auto keyName = KeyName(key);
            if (!keyName.empty())
                keyboardDown.insert(keyName);
            if (key == VirtualKey::Up)
                DispatchWorldPhysicalSource(L"keyboard.up");
            else if (key == VirtualKey::Down)
                DispatchWorldPhysicalSource(L"keyboard.down");
            else if (key == VirtualKey::Left)
                DispatchWorldPhysicalSource(L"keyboard.left");
            else if (key == VirtualKey::Right)
                DispatchWorldPhysicalSource(L"keyboard.right");
            else if (key == VirtualKey::Space)
                DispatchWorldPhysicalSource(L"keyboard.space");
            else if (key == VirtualKey::R)
                DispatchWorldPhysicalSource(L"keyboard.r");
            if (key == VirtualKey::Up ||
                key == VirtualKey::Left ||
                key == VirtualKey::GamepadDPadUp ||
                key == VirtualKey::GamepadDPadLeft)
            {
                FocusMove(-1);
                Publish();
                return true;
            }
            if (key == VirtualKey::Down ||
                key == VirtualKey::Right ||
                key == VirtualKey::GamepadDPadDown ||
                key == VirtualKey::GamepadDPadRight)
            {
                FocusMove(1);
                Publish();
                return true;
            }
            if (key == VirtualKey::Enter ||
                key == VirtualKey::Space ||
                key == VirtualKey::GamepadA)
            {
                ActivateFocusedElement();
                Publish();
                return true;
            }
            if (key == VirtualKey::Escape ||
                key == VirtualKey::GamepadB)
            {
                DispatchEvent(L"input.back");
                Publish();
                return true;
            }
            return !keyName.empty();
        }

        void KeyUp(VirtualKey key)
        {
            auto keyName = KeyName(key);
            if (!keyName.empty())
                keyboardDown.erase(keyName);
        }
    };

    WorkerCreativeForegroundRuntime::WorkerCreativeForegroundRuntime() :
        impl_(std::make_unique<Impl>())
    {
        impl_->Publish();
    }

    WorkerCreativeForegroundRuntime::~WorkerCreativeForegroundRuntime() =
        default;

    void WorkerCreativeForegroundRuntime::Initialize(
        fs::path const& workerRoot)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->workerRoot = workerRoot;
        try
        {
            auto records = LoadLaunchRecords(workerRoot);
            if (records.empty())
            {
                impl_->Publish();
                return;
            }
            auto record = records.back();
            auto active = WorkerLoadActiveCreativeInstall(
                workerRoot,
                record.projectId,
                record.installId);
            auto plan = BuildExecutionPlan(active);
            if (record.hostProfileCanonicalSha256 !=
                    WorkerCreativeHostProfileCanonicalSha256() ||
                record.activationRecordSha256 !=
                    active.activationRecordSha256 ||
                record.activationSequence !=
                    active.activationSequence ||
                record.planSha256 != plan->planSha256)
            {
                FailForeground(
                    "xcp.creative.foreground_launch_stale",
                    "persisted creative launch no longer matches the exact active install",
                    "foreground_restore",
                    "launch_record",
                    WideToUtf8(record.launchId),
                    "current exact activation and execution plan",
                    "stale binding",
                    "reload the current active install with a fresh launch id");
            }
            impl_->ActivatePlan(
                std::move(plan),
                record,
                true);
        }
        catch (WorkerCreativeHostError const& error)
        {
            impl_->plan.reset();
            impl_->lastErrorCode =
                Utf8ToWide(error.code);
            impl_->AppendLog(
                L"restore refused: " +
                Utf8ToWide(error.code));
            impl_->Publish();
        }
    }

    void WorkerCreativeForegroundRuntime::Tick()
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->Tick();
    }

    bool WorkerCreativeForegroundRuntime::HandleKeyDown(
        VirtualKey key)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->KeyDown(key);
    }

    void WorkerCreativeForegroundRuntime::HandleKeyUp(
        VirtualKey key)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->KeyUp(key);
    }

    void WorkerCreativeForegroundRuntime::OnSuspending()
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->plan)
        {
            if (impl_->audioEngine)
                impl_->audioEngine->StopEngine();
            try
            {
                impl_->SaveLocalState();
            }
            catch (WorkerCreativeHostError const& error)
            {
                impl_->lastErrorCode =
                    Utf8ToWide(error.code);
                impl_->AppendLog(
                    L"suspend save refused: " +
                    Utf8ToWide(error.code));
            }
            impl_->Publish();
        }
    }

    void WorkerCreativeForegroundRuntime::OnResuming()
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->lastTick = std::chrono::steady_clock::now();
        if (impl_->plan)
        {
            if (impl_->audioEngine &&
                FAILED(impl_->audioEngine->StartEngine()))
            {
                impl_->lastErrorCode =
                    L"xcp.creative.audio_resume_failed";
                impl_->AppendLog(
                    L"audio resume failed");
            }
            impl_->AppendLog(L"foreground resumed");
            impl_->Publish();
        }
    }

    bool WorkerCreativeForegroundRuntime::HasActiveProject() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->plan != nullptr;
    }

    std::shared_ptr<WorkerCreativeForegroundSnapshot const>
        WorkerCreativeForegroundRuntime::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->published;
    }

    std::optional<WorkerCreativeFrameCaptureRequest>
        WorkerCreativeForegroundRuntime::TakeFrameCaptureRequest()
    {
        std::lock_guard<std::mutex> lock(
            impl_->captureMutex);
        if (!impl_->pendingCapture.has_value())
            return std::nullopt;
        impl_->inFlightCapture =
            std::move(impl_->pendingCapture);
        impl_->pendingCapture.reset();
        return impl_->inFlightCapture;
    }

    void WorkerCreativeForegroundRuntime::CompleteFrameCapture(
        WorkerCreativeFrameCaptureResult result)
    {
        std::lock_guard<std::mutex> lock(
            impl_->captureMutex);
        if (!impl_->inFlightCapture.has_value() ||
            impl_->inFlightCapture->captureId !=
                result.request.captureId)
        {
            return;
        }
        impl_->completedCapture = std::move(result);
        impl_->inFlightCapture.reset();
        impl_->captureReady.notify_all();
    }

    std::wstring WorkerCreativeForegroundRuntime::Launch(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& workerRoot)
    {
        auto request = StripTransportFields(wireRequest);
        RequireExactFields(
            request,
            {
                L"command",
                L"schema_version",
                L"expected_host_profile_sha256",
                L"project_id",
                L"install_id",
                L"launch_id",
                L"expected_launch_record_sha256"
            },
            "launch_request");
        if (RequiredString(
                request,
                L"command",
                "launch_request") !=
                L"launch_creative_project" ||
            RequiredString(
                request,
                L"schema_version",
                "launch_request") !=
                L"xcp-creative-launch-request-v1")
        {
            FailForeground(
                "xcp.creative.request_schema_unsupported",
                "creative launch request command or schema is unsupported",
                "foreground_launch",
                "command/schema_version",
                {},
                "launch_creative_project / xcp-creative-launch-request-v1",
                "other",
                "refresh describe_creative_host and retry");
        }
        auto expectedProfile = RequiredString(
            request,
            L"expected_host_profile_sha256",
            "launch_request",
            64);
        auto projectId = RequiredString(
            request,
            L"project_id",
            "launch_request",
            96);
        auto installId = RequiredString(
            request,
            L"install_id",
            "launch_request",
            64);
        auto launchId = RequiredString(
            request,
            L"launch_id",
            "launch_request",
            64);
        auto expectedRecord = RequiredString(
            request,
            L"expected_launch_record_sha256",
            "launch_request",
            64);
        if (expectedProfile !=
                WorkerCreativeHostProfileCanonicalSha256() ||
            !IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(installId) ||
            !IsSafeLaunchId(launchId) ||
            (!expectedRecord.empty() &&
                !IsLowerHexSha256(expectedRecord)))
        {
            FailForeground(
                "xcp.creative.foreground_binding_invalid",
                "creative launch binding is invalid",
                "foreground_launch",
                "identity",
                {},
                "current profile, safe ids and exact SHA-256 bindings",
                "invalid binding",
                "refresh discovery/list state and retry");
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->workerRoot = workerRoot;
        auto records = LoadLaunchRecords(workerRoot);
        auto currentSha = records.empty()
            ? std::wstring()
            : records.back().recordSha256;
        for (auto const& record : records)
        {
            if (record.launchId != launchId)
                continue;
            if (record.operation != L"launch" ||
                record.projectId != projectId ||
                record.installId != installId ||
                record.hostProfileCanonicalSha256 !=
                    expectedProfile ||
                record.previousRecordSha256 !=
                    expectedRecord)
            {
                FailForeground(
                    "xcp.creative.foreground_launch_conflict",
                    "creative launch id was reused with different inputs",
                    "foreground_launch",
                    "launch_id",
                    WideToUtf8(launchId),
                    "same exact launch binding",
                    "conflicting replay",
                    "use a fresh launch id");
            }
            return L"{\"ok\":true,\"protocol_version\":" +
                JsonString(protocolVersion) +
                L",\"command\":\"launch_creative_project\"" +
                L",\"schema_version\":\"xcp-creative-launch-result-v1\"" +
                L",\"project_id\":" +
                JsonString(record.projectId) +
                L",\"install_id\":" +
                JsonString(record.installId) +
                L",\"launch_id\":" +
                JsonString(record.launchId) +
                L",\"launch_sequence\":" +
                std::to_wstring(record.sequence) +
                L",\"launch_record_sha256\":" +
                JsonString(record.recordSha256) +
                L",\"plan_sha256\":" +
                JsonString(record.planSha256) +
                L",\"replayed\":true,\"changed\":false}";
        }
        if (expectedRecord != currentSha)
        {
            FailForeground(
                "xcp.creative.foreground_state_conflict",
                "creative launch precondition does not match current foreground state",
                "foreground_launch",
                "expected_launch_record_sha256",
                {},
                WideToUtf8(currentSha),
                WideToUtf8(expectedRecord),
                "observe the foreground and retry with its exact launch record");
        }
        auto active = WorkerLoadActiveCreativeInstall(
            workerRoot,
            projectId,
            installId);
        auto plan = BuildExecutionPlan(active);
        ForegroundLaunchRecord record;
        record.sequence = records.size() + 1;
        record.previousRecordSha256 = currentSha;
        record.launchId = launchId;
        record.operation = L"launch";
        record.projectId = projectId;
        record.installId = installId;
        record.hostProfileCanonicalSha256 =
            expectedProfile;
        record.activationRecordSha256 =
            active.activationRecordSha256;
        record.activationSequence =
            active.activationSequence;
        record.planSha256 = plan->planSha256;
        auto bytes = WideToUtf8(LaunchRecordJson(record));
        record.recordSha256 =
            WorkerContentSha256(bytes);
        WriteImmutableFile(
            LaunchRecordRoot(workerRoot) /
                LaunchRecordName(
                    record.sequence,
                    record.launchId),
            bytes);
        impl_->ActivatePlan(plan, record, false);
        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion) +
            L",\"command\":\"launch_creative_project\"" +
            L",\"schema_version\":\"xcp-creative-launch-result-v1\"" +
            L",\"project_id\":" + JsonString(projectId) +
            L",\"install_id\":" + JsonString(installId) +
            L",\"launch_id\":" + JsonString(launchId) +
            L",\"launch_sequence\":" +
            std::to_wstring(record.sequence) +
            L",\"launch_record_sha256\":" +
            JsonString(record.recordSha256) +
            L",\"plan_sha256\":" +
            JsonString(plan->planSha256) +
            L",\"activation_record_sha256\":" +
            JsonString(active.activationRecordSha256) +
            L",\"replayed\":false,\"changed\":true}";
    }

    std::wstring WorkerCreativeForegroundRuntime::Reload(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& workerRoot)
    {
        auto request = StripTransportFields(wireRequest);
        RequireExactFields(
            request,
            {
                L"command",
                L"schema_version",
                L"expected_host_profile_sha256",
                L"project_id",
                L"expected_active_install_id",
                L"launch_id",
                L"expected_launch_record_sha256"
            },
            "reload_request");
        if (RequiredString(
                request,
                L"command",
                "reload_request") !=
                L"reload_creative_project" ||
            RequiredString(
                request,
                L"schema_version",
                "reload_request") !=
                L"xcp-creative-reload-request-v1")
        {
            FailForeground(
                "xcp.creative.request_schema_unsupported",
                "creative reload request command or schema is unsupported",
                "foreground_reload",
                "command/schema_version",
                {},
                "reload_creative_project / xcp-creative-reload-request-v1",
                "other",
                "refresh describe_creative_host and retry");
        }
        auto expectedProfile = RequiredString(
            request,
            L"expected_host_profile_sha256",
            "reload_request",
            64);
        auto projectId = RequiredString(
            request,
            L"project_id",
            "reload_request",
            96);
        auto installId = RequiredString(
            request,
            L"expected_active_install_id",
            "reload_request",
            64);
        auto launchId = RequiredString(
            request,
            L"launch_id",
            "reload_request",
            64);
        auto expectedRecord = RequiredString(
            request,
            L"expected_launch_record_sha256",
            "reload_request",
            64);
        if (expectedProfile !=
                WorkerCreativeHostProfileCanonicalSha256() ||
            !IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(installId) ||
            !IsSafeLaunchId(launchId) ||
            !IsLowerHexSha256(expectedRecord))
        {
            FailForeground(
                "xcp.creative.foreground_binding_invalid",
                "creative reload binding is invalid",
                "foreground_reload",
                "identity",
                {},
                "current profile and exact foreground/activation identities",
                "invalid binding",
                "observe foreground and activation state before retrying");
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->workerRoot = workerRoot;
        auto records = LoadLaunchRecords(workerRoot);
        if (records.empty() ||
            records.back().recordSha256 != expectedRecord ||
            records.back().projectId != projectId)
        {
            FailForeground(
                "xcp.creative.foreground_state_conflict",
                "creative reload precondition does not match the current foreground",
                "foreground_reload",
                "expected_launch_record_sha256/project_id",
                {},
                records.empty()
                    ? "no foreground launch"
                    : WideToUtf8(
                        records.back().recordSha256 + L"/" +
                        records.back().projectId),
                WideToUtf8(expectedRecord + L"/" + projectId),
                "observe the foreground and retry with exact current identities");
        }
        for (auto const& existing : records)
        {
            if (existing.launchId == launchId)
            {
                FailForeground(
                    "xcp.creative.foreground_launch_conflict",
                    "creative reload launch id already exists",
                    "foreground_reload",
                    "launch_id",
                    WideToUtf8(launchId),
                    "fresh launch id",
                    "already committed",
                    "use a fresh launch id");
            }
        }
        auto active = WorkerLoadActiveCreativeInstall(
            workerRoot,
            projectId,
            installId);
        auto plan = BuildExecutionPlan(active);
        ForegroundLaunchRecord record;
        record.sequence = records.size() + 1;
        record.previousRecordSha256 =
            records.back().recordSha256;
        record.launchId = launchId;
        record.operation = L"reload";
        record.projectId = projectId;
        record.installId = installId;
        record.hostProfileCanonicalSha256 =
            expectedProfile;
        record.activationRecordSha256 =
            active.activationRecordSha256;
        record.activationSequence =
            active.activationSequence;
        record.planSha256 = plan->planSha256;
        auto bytes = WideToUtf8(LaunchRecordJson(record));
        record.recordSha256 =
            WorkerContentSha256(bytes);
        WriteImmutableFile(
            LaunchRecordRoot(workerRoot) /
                LaunchRecordName(
                    record.sequence,
                    record.launchId),
            bytes);
        impl_->ActivatePlan(plan, record, false);
        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion) +
            L",\"command\":\"reload_creative_project\"" +
            L",\"schema_version\":\"xcp-creative-reload-result-v1\"" +
            L",\"project_id\":" + JsonString(projectId) +
            L",\"install_id\":" + JsonString(installId) +
            L",\"launch_id\":" + JsonString(launchId) +
            L",\"launch_sequence\":" +
            std::to_wstring(record.sequence) +
            L",\"launch_record_sha256\":" +
            JsonString(record.recordSha256) +
            L",\"plan_sha256\":" +
            JsonString(plan->planSha256) +
            L",\"replayed\":false,\"changed\":true}";
    }

    std::wstring WorkerCreativeForegroundRuntime::Observe(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion) const
    {
        auto request = StripTransportFields(wireRequest);
        RequireExactFields(
            request,
            {
                L"command",
                L"schema_version",
                L"project_id",
                L"expected_install_id",
                L"expected_plan_sha256"
            },
            "observation_request");
        if (RequiredString(
                request,
                L"command",
                "observation_request") !=
                L"observe_creative_foreground" ||
            RequiredString(
                request,
                L"schema_version",
                "observation_request") !=
                L"xcp-creative-observation-request-v1")
        {
            FailForeground(
                "xcp.creative.request_schema_unsupported",
                "creative observation request command or schema is unsupported",
                "foreground_observation",
                "command/schema_version",
                {},
                "observe_creative_foreground / xcp-creative-observation-request-v1",
                "other",
                "refresh describe_creative_host and retry");
        }
        auto projectId = RequiredString(
            request,
            L"project_id",
            "observation_request",
            96);
        auto expectedInstall = RequiredString(
            request,
            L"expected_install_id",
            "observation_request",
            64);
        auto expectedPlan = RequiredString(
            request,
            L"expected_plan_sha256",
            "observation_request",
            64);
        auto discovery =
            projectId.empty() &&
            expectedInstall.empty() &&
            expectedPlan.empty();
        if (!discovery &&
            (!IsSafeIdentifier(projectId) ||
                !IsLowerHexSha256(expectedInstall) ||
                !IsLowerHexSha256(expectedPlan)))
        {
            FailForeground(
                "xcp.creative.foreground_observation_binding_invalid",
                "creative observation binding is partial or invalid",
                "foreground_observation",
                "project_id/expected_install_id/expected_plan_sha256",
                {},
                "all three exact identities or three empty discovery fields",
                "partial or invalid binding",
                "use empty fields to discover, then reuse the returned exact identities");
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->plan && discovery)
        {
            return L"{\"ok\":true,\"protocol_version\":" +
                JsonString(protocolVersion) +
                L",\"command\":\"observe_creative_foreground\"" +
                L",\"schema_version\":\"xcp-creative-foreground-observation-v1\"" +
                L",\"active\":false,\"observation_class\":\"structured_runtime_snapshot\"}";
        }
        if (!impl_->plan ||
            (!discovery &&
                (impl_->plan->projectId != projectId ||
                    impl_->plan->installId != expectedInstall ||
                    impl_->plan->planSha256 != expectedPlan)))
        {
            FailForeground(
                "xcp.creative.foreground_observation_conflict",
                "creative observation binding does not match the foreground",
                "foreground_observation",
                "project_id/expected_install_id/expected_plan_sha256",
                {},
                impl_->plan
                    ? WideToUtf8(
                        impl_->plan->projectId + L"/" +
                        impl_->plan->installId + L"/" +
                        impl_->plan->planSha256)
                    : "no active foreground",
                WideToUtf8(
                    projectId + L"/" +
                    expectedInstall + L"/" +
                    expectedPlan),
                "use identities returned by launch or reload");
        }
        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion) +
            L",\"command\":\"observe_creative_foreground\"" +
            L",\"schema_version\":\"xcp-creative-foreground-observation-v1\"" +
            L",\"active\":true" +
            L",\"project_id\":" +
            JsonString(impl_->plan->projectId) +
            L",\"project_version\":" +
            JsonString(impl_->plan->projectVersion) +
            L",\"install_id\":" +
            JsonString(impl_->plan->installId) +
            L",\"plan_sha256\":" +
            JsonString(impl_->plan->planSha256) +
            L",\"launch_id\":" +
            JsonString(impl_->launch.launchId) +
            L",\"launch_sequence\":" +
            std::to_wstring(impl_->launch.sequence) +
            L",\"launch_record_sha256\":" +
            JsonString(impl_->launch.recordSha256) +
            L",\"activation_record_sha256\":" +
            JsonString(
                impl_->plan->activationRecordSha256) +
            L",\"state_revision\":" +
            std::to_wstring(impl_->stateRevision) +
            L",\"frame_count\":" +
            std::to_wstring(impl_->frameCount) +
            L",\"state\":" +
            ScalarMapJson(impl_->state) +
            L",\"local_state\":" +
            ScalarMapJson(impl_->localState) +
            L",\"world2d\":" +
            impl_->World2DStateJson() +
            L",\"focused_element_id\":" +
            JsonString(impl_->focusedElementId) +
            L",\"last_event\":" +
            JsonString(impl_->lastEvent) +
            L",\"semantic_event_sequence\":" +
            std::to_wstring(
                impl_->semanticEventSequence) +
            L",\"recent_semantic_events\":" +
            SemanticEventArrayJson(
                impl_->recentSemanticEvents) +
            L",\"physical_input_sequence\":" +
            std::to_wstring(
                impl_->physicalInputSequence) +
            L",\"recent_physical_inputs\":" +
            PhysicalInputArrayJson(
                impl_->recentPhysicalInputs) +
            L",\"node_translation_2d\":" +
            Vec2MapJson(impl_->nodeTranslation2D) +
            L",\"node_translation_3d\":" +
            Vec3MapJson(impl_->nodeTranslation3D) +
            L",\"node_visibility\":" +
            BoolMapJson(impl_->nodeVisibility) +
            L",\"camera_transforms\":" +
            Transform3DMapJson(
                impl_->cameraTransforms) +
            L",\"last_error_code\":" +
            JsonString(impl_->lastErrorCode) +
            L",\"active_audio_voices\":" +
            std::to_wstring(
                impl_->activeAudioVoices.size()) +
            L",\"audio_cue_count\":" +
            std::to_wstring(
                impl_->audioCueCount) +
            L",\"xvm_invocation_count\":" +
            std::to_wstring(
                impl_->xvmInvocationCount) +
            L",\"logs\":" +
            StringArrayJson(impl_->logs) +
            L",\"observation_class\":\"structured_runtime_snapshot\"" +
            L"}";
    }

    std::wstring WorkerCreativeForegroundRuntime::DispatchInput(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion)
    {
        auto request = StripTransportFields(wireRequest);
        RequireExactFields(
            request,
            {
                L"command",
                L"schema_version",
                L"expected_host_profile_sha256",
                L"project_id",
                L"expected_install_id",
                L"expected_plan_sha256",
                L"action",
                L"repeat"
            },
            "creative_input_dispatch_request");
        if (RequiredString(
                request,
                L"command",
                "creative_input_dispatch_request") !=
                L"dispatch_creative_input" ||
            RequiredString(
                request,
                L"schema_version",
                "creative_input_dispatch_request") !=
                L"xcp-creative-input-dispatch-request-v1")
        {
            FailForeground(
                "xcp.creative.request_schema_unsupported",
                "creative input dispatch request command or schema is unsupported",
                "creative_input_dispatch",
                "command/schema_version",
                {},
                "dispatch_creative_input / xcp-creative-input-dispatch-request-v1",
                "other",
                "refresh describe_creative_host and retry");
        }
        auto hostProfile = RequiredString(
            request,
            L"expected_host_profile_sha256",
            "creative_input_dispatch_request",
            64);
        auto projectId = RequiredString(
            request,
            L"project_id",
            "creative_input_dispatch_request",
            96);
        auto expectedInstall = RequiredString(
            request,
            L"expected_install_id",
            "creative_input_dispatch_request",
            64);
        auto expectedPlan = RequiredString(
            request,
            L"expected_plan_sha256",
            "creative_input_dispatch_request",
            64);
        auto action = RequiredString(
            request,
            L"action",
            "creative_input_dispatch_request",
            128);
        auto repeat = static_cast<uint32_t>(
            RequiredInteger(
                request,
                L"repeat",
                "creative_input_dispatch_request",
                1,
                64));
        if (hostProfile !=
                WorkerCreativeHostProfileCanonicalSha256() ||
            !IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(expectedInstall) ||
            !IsLowerHexSha256(expectedPlan) ||
            !IsSafeIdentifier(action, 128, true))
        {
            FailForeground(
                "xcp.creative.input_dispatch_binding_invalid",
                "creative input dispatch identities or action are invalid",
                "creative_input_dispatch",
                "expected_host_profile_sha256/project_id/expected_install_id/expected_plan_sha256/action",
                {},
                "exact active identities and a declared semantic action",
                "invalid binding",
                "reuse identities from discovery and launch, then choose a published project action");
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->plan ||
            impl_->plan->projectId != projectId ||
            impl_->plan->installId != expectedInstall ||
            impl_->plan->planSha256 != expectedPlan)
        {
            FailForeground(
                "xcp.creative.input_dispatch_conflict",
                "creative input dispatch binding does not match the foreground",
                "creative_input_dispatch",
                "project_id/expected_install_id/expected_plan_sha256",
                {},
                impl_->plan
                    ? WideToUtf8(
                        impl_->plan->projectId + L"/" +
                        impl_->plan->installId + L"/" +
                        impl_->plan->planSha256)
                    : "active foreground",
                WideToUtf8(
                    projectId + L"/" +
                    expectedInstall + L"/" +
                    expectedPlan),
                "reuse the exact identities returned by launch");
        }
        bool changed = false;
        std::vector<std::wstring> events;
        for (uint32_t index = 0; index < repeat; ++index)
        {
            auto result = impl_->DispatchSemanticAction(action);
            if (!result.handled)
            {
                FailForeground(
                    "xcp.creative.input_action_unsupported",
                    "creative semantic input action is not declared by the active project",
                    "creative_input_dispatch",
                    "action",
                    WideToUtf8(projectId),
                    "semantic action declared by an active input, UI, behavior, audio or XVM contract",
                    WideToUtf8(action),
                    "inspect the generated project or adaptation mapping and select a declared action");
            }
            changed = changed || result.changed;
            events.insert(
                events.end(),
                result.events.begin(),
                result.events.end());
        }
        impl_->Publish();
        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion) +
            L",\"command\":\"dispatch_creative_input\"" +
            L",\"schema_version\":\"xcp-creative-input-dispatch-result-v1\"" +
            L",\"project_id\":" + JsonString(projectId) +
            L",\"install_id\":" +
            JsonString(expectedInstall) +
            L",\"plan_sha256\":" +
            JsonString(expectedPlan) +
            L",\"action\":" + JsonString(action) +
            L",\"repeat\":" + std::to_wstring(repeat) +
            L",\"changed\":" + BoolJson(changed) +
            L",\"events\":" + StringArrayJson(events) +
            L",\"state_revision\":" +
            std::to_wstring(impl_->stateRevision) +
            L",\"world2d\":" + impl_->World2DStateJson() +
            L"}";
    }

    std::wstring WorkerCreativeForegroundRuntime::CaptureFrame(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion)
    {
        auto request = StripTransportFields(wireRequest);
        RequireExactFields(
            request,
            {
                L"command",
                L"schema_version",
                L"expected_host_profile_sha256",
                L"project_id",
                L"expected_install_id",
                L"expected_plan_sha256",
                L"capture_id"
            },
            "frame_capture_request");
        if (RequiredString(
                request,
                L"command",
                "frame_capture_request") !=
                L"capture_creative_frame" ||
            RequiredString(
                request,
                L"schema_version",
                "frame_capture_request") !=
                L"xcp-creative-frame-capture-request-v1")
        {
            FailForeground(
                "xcp.creative.request_schema_unsupported",
                "creative frame-capture request command or schema is unsupported",
                "foreground_frame_capture",
                "command/schema_version",
                {},
                "capture_creative_frame / xcp-creative-frame-capture-request-v1",
                "other",
                "refresh describe_creative_host and retry");
        }
        auto expectedProfile = RequiredString(
            request,
            L"expected_host_profile_sha256",
            "frame_capture_request",
            64);
        auto projectId = RequiredString(
            request,
            L"project_id",
            "frame_capture_request",
            96);
        auto expectedInstall = RequiredString(
            request,
            L"expected_install_id",
            "frame_capture_request",
            64);
        auto expectedPlan = RequiredString(
            request,
            L"expected_plan_sha256",
            "frame_capture_request",
            64);
        auto captureId = RequiredString(
            request,
            L"capture_id",
            "frame_capture_request",
            64);
        if (expectedProfile !=
                WorkerCreativeHostProfileCanonicalSha256() ||
            !IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(expectedInstall) ||
            !IsLowerHexSha256(expectedPlan) ||
            !IsSafeLaunchId(captureId))
        {
            FailForeground(
                "xcp.creative.frame_capture_binding_invalid",
                "creative frame-capture binding is invalid",
                "foreground_frame_capture",
                "identity",
                {},
                "current profile, active foreground identities and safe capture id",
                "invalid binding",
                "refresh discovery and observe the foreground before retrying");
        }

        WorkerCreativeFrameCaptureRequest capture;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->plan ||
                impl_->plan->projectId != projectId ||
                impl_->plan->installId != expectedInstall ||
                impl_->plan->planSha256 != expectedPlan)
            {
                FailForeground(
                    "xcp.creative.frame_capture_conflict",
                    "creative frame-capture binding does not match the foreground",
                    "foreground_frame_capture",
                    "project_id/expected_install_id/expected_plan_sha256",
                    {},
                    impl_->plan
                        ? WideToUtf8(
                            impl_->plan->projectId + L"/" +
                            impl_->plan->installId + L"/" +
                            impl_->plan->planSha256)
                        : "no active foreground",
                    WideToUtf8(
                        projectId + L"/" +
                        expectedInstall + L"/" +
                        expectedPlan),
                    "use identities returned by launch or reload");
            }
            capture.captureId = captureId;
            capture.projectId = projectId;
            capture.installId = expectedInstall;
            capture.planSha256 = expectedPlan;
            capture.stateRevision = impl_->stateRevision;
            capture.frameCount = impl_->frameCount;
        }

        WorkerCreativeFrameCaptureResult result;
        {
            std::unique_lock<std::mutex> lock(
                impl_->captureMutex);
            if (impl_->pendingCapture.has_value() ||
                impl_->inFlightCapture.has_value())
            {
                FailForeground(
                    "xcp.creative.frame_capture_busy",
                    "creative renderer already has a pending frame capture",
                    "foreground_frame_capture",
                    "capture_id",
                    WideToUtf8(captureId),
                    "one capture at a time",
                    "capture already pending",
                    "retry after the current capture completes",
                    true);
            }
            impl_->completedCapture.reset();
            impl_->pendingCapture = capture;
            auto completed = impl_->captureReady.wait_for(
                lock,
                std::chrono::seconds(5),
                [&]
                {
                    return impl_->completedCapture.has_value() &&
                        impl_->completedCapture->request.captureId ==
                            captureId;
                });
            if (!completed)
            {
                if (impl_->pendingCapture.has_value() &&
                    impl_->pendingCapture->captureId ==
                        captureId)
                {
                    impl_->pendingCapture.reset();
                }
                if (impl_->inFlightCapture.has_value() &&
                    impl_->inFlightCapture->captureId ==
                        captureId)
                {
                    impl_->inFlightCapture.reset();
                }
                FailForeground(
                    "xcp.creative.frame_capture_timeout",
                    "creative renderer did not complete frame capture in time",
                    "foreground_frame_capture",
                    "capture_id",
                    WideToUtf8(captureId),
                    "visible foreground frame within five seconds",
                    "timeout",
                    "make the worker foreground visible and retry",
                    true);
            }
            result =
                std::move(*impl_->completedCapture);
            impl_->completedCapture.reset();
        }
        if (!result.errorCode.empty() ||
            result.width == 0 ||
            result.height == 0 ||
            result.stride != result.width * 4u ||
            result.bgra.size() !=
                static_cast<size_t>(result.stride) *
                    result.height ||
            result.bgra.size() > 640u * 360u * 4u)
        {
            FailForeground(
                result.errorCode.empty()
                    ? "xcp.creative.frame_capture_invalid"
                    : WideToUtf8(result.errorCode),
                "creative renderer returned an invalid bounded frame capture",
                "foreground_frame_capture",
                "capture",
                WideToUtf8(captureId),
                "bounded 32-bit BGRA frame up to 640x360",
                "invalid render result",
                "retry after restoring the visible foreground",
                true);
        }
        auto content = std::string(
            reinterpret_cast<char const*>(
                result.bgra.data()),
            result.bgra.size());
        auto sha256 =
            WorkerContentSha256(content);
        auto encoded =
            Base64Encode(result.bgra);
        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion) +
            L",\"command\":\"capture_creative_frame\"" +
            L",\"schema_version\":\"xcp-creative-frame-capture-v1\"" +
            L",\"capture_id\":" +
            JsonString(captureId) +
            L",\"project_id\":" +
            JsonString(projectId) +
            L",\"install_id\":" +
            JsonString(expectedInstall) +
            L",\"plan_sha256\":" +
            JsonString(expectedPlan) +
            L",\"state_revision\":" +
            std::to_wstring(
                result.request.stateRevision) +
            L",\"frame_count\":" +
            std::to_wstring(
                result.request.frameCount) +
            L",\"pixel_format\":\"bgra8-premultiplied\"" +
            L",\"sampling\":\"nearest-neighbor-v1\"" +
            L",\"width\":" +
            std::to_wstring(result.width) +
            L",\"height\":" +
            std::to_wstring(result.height) +
            L",\"stride\":" +
            std::to_wstring(result.stride) +
            L",\"content_bytes\":" +
            std::to_wstring(result.bgra.size()) +
            L",\"content_sha256\":" +
            JsonString(sha256) +
            L",\"encoding\":\"base64\"" +
            L",\"data_base64\":" +
            JsonString(Utf8ToWide(encoded)) +
            L"}";
    }
}
