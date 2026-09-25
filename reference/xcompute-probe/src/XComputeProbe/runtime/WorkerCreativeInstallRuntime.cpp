#include "pch.h"
#include "WorkerCreativeInstallRuntime.h"

#include <cmath>
#include <set>

#include "WorkerArtifactStore.h"
#include "WorkerCreativeHostRuntime.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;
namespace fs = std::filesystem;

namespace XComputeProbe
{
    namespace
    {
        constexpr uint64_t MaxBundleManifestBytes = 4ull * 1024ull * 1024ull;
        constexpr uint64_t MaxModuleDocumentBytes = 16ull * 1024ull * 1024ull;
        constexpr uint64_t MaxProjectBytes = 4ull * 1024ull * 1024ull * 1024ull;
        constexpr uint64_t MaxInstalledWorkspaceBytes = 8ull * 1024ull * 1024ull * 1024ull;
        constexpr uint64_t MaxFileBytes = 4ull * 1024ull * 1024ull * 1024ull;
        constexpr uint64_t MaxModuleCount = 128;
        constexpr uint64_t MaxAssetCount = 256;
        constexpr uint64_t MaxFileCount = 384;
        constexpr uint64_t MaxActivationRecords = 4096;

        struct CreativeFile
        {
            std::wstring role;
            std::wstring id;
            std::wstring kind;
            std::wstring path;
            std::wstring sha256;
            uint64_t bytes = 0;
            std::vector<std::wstring> dependencies;
        };

        struct CreativeBundle
        {
            std::wstring artifactId;
            std::wstring bundleSha256;
            uint64_t bundleBytes = 0;
            std::wstring projectId;
            std::wstring projectVersion;
            std::wstring entryModule;
            std::wstring contentSha256;
            std::vector<std::wstring> requestedCapabilities;
            std::vector<CreativeFile> files;
        };

        struct InstalledCreativeBundle
        {
            std::wstring installId;
            std::wstring projectId;
            std::wstring projectVersion;
            std::wstring entryModule;
            std::wstring bundleSha256;
            std::wstring contentSha256;
            std::wstring hostProfileCanonicalSha256;
            uint64_t bundleBytes = 0;
            uint64_t totalBytes = 0;
            std::vector<CreativeFile> files;
            std::vector<std::wstring> casReferences;
            fs::path recordPath;
        };

        struct CreativeTransition
        {
            uint64_t sequence = 0;
            std::wstring transitionId;
            std::wstring operation;
            std::wstring projectId;
            std::wstring requestedInstallId;
            std::wstring expectedActiveInstallId;
            std::wstring expectedPreviousInstallId;
            std::wstring activeInstallId;
            std::wstring previousInstallId;
            std::wstring previousRecordSha256;
            std::wstring recordSha256;
        };

        struct CreativeActivationState
        {
            std::wstring projectId;
            uint64_t sequence = 0;
            std::wstring activeInstallId;
            std::wstring previousInstallId;
            std::wstring lastRecordSha256;
            std::vector<CreativeTransition> transitions;
        };

        std::mutex CreativeLifecycleMutex;

        std::string WideToUtf8(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring Utf8ToWide(std::string const& value)
        {
            return std::wstring(winrt::to_hstring(value));
        }

        [[noreturn]] void Fail(
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

        std::wstring BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring OkBase(
            std::wstring const& command,
            std::wstring const& protocolVersion)
        {
            return L"{\"ok\":true,\"protocol_version\":" +
                JsonString(protocolVersion) +
                L",\"command\":" + JsonString(command);
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
                {
                    request.Remove(field);
                }
            }
            return request;
        }

        void RequireExactFields(
            JsonObject const& object,
            std::set<std::wstring> const& allowed,
            std::string const& stage,
            std::string const& field)
        {
            for (auto const& entry : object)
            {
                auto name = std::wstring(entry.Key());
                if (allowed.find(name) == allowed.end())
                {
                    Fail(
                        "xcp.creative.schema_rejected",
                        "creative JSON object contains an unknown field",
                        stage,
                        field,
                        WideToUtf8(name),
                        "only fields declared by the current creative contract",
                        WideToUtf8(name),
                        "remove the unknown field and rebuild the bundle or request");
                }
            }
        }

        std::wstring RequiredString(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& stage,
            std::string const& field)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() != JsonValueType::String)
            {
                Fail(
                    "xcp.creative.schema_rejected",
                    "required creative string field is missing or invalid",
                    stage,
                    field,
                    {},
                    "string",
                    "missing or non-string");
            }
            return std::wstring(object.GetNamedString(name));
        }

        uint64_t RequiredUInt64(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& stage,
            std::string const& field)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() != JsonValueType::Number)
            {
                Fail(
                    "xcp.creative.schema_rejected",
                    "required creative integer field is missing or invalid",
                    stage,
                    field,
                    {},
                    "non-negative integer",
                    "missing or non-number");
            }
            auto value = object.GetNamedNumber(name);
            if (!std::isfinite(value) ||
                value < 0.0 ||
                std::floor(value) != value ||
                value > 9007199254740991.0)
            {
                Fail(
                    "xcp.creative.schema_rejected",
                    "creative integer field is outside the exact JSON range",
                    stage,
                    field,
                    {},
                    "integer in 0..9007199254740991",
                    std::to_string(value));
            }
            return static_cast<uint64_t>(value);
        }

        JsonObject RequiredObject(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& stage,
            std::string const& field)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() != JsonValueType::Object)
            {
                Fail(
                    "xcp.creative.schema_rejected",
                    "required creative object field is missing or invalid",
                    stage,
                    field,
                    {},
                    "object",
                    "missing or non-object");
            }
            return object.GetNamedObject(name);
        }

        JsonArray RequiredArray(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& stage,
            std::string const& field)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() != JsonValueType::Array)
            {
                Fail(
                    "xcp.creative.schema_rejected",
                    "required creative array field is missing or invalid",
                    stage,
                    field,
                    {},
                    "array",
                    "missing or non-array");
            }
            return object.GetNamedArray(name);
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

        bool IsSafeIdentifier(std::wstring const& value, size_t maximum = 96)
        {
            if (value.empty() ||
                value.size() > maximum ||
                value.front() < L'a' ||
                value.front() > L'z')
            {
                return false;
            }
            auto separator = false;
            for (auto ch : value)
            {
                auto alphanumeric =
                    (ch >= L'a' && ch <= L'z') ||
                    (ch >= L'0' && ch <= L'9');
                auto isSeparator = ch == L'.' || ch == L'_' || ch == L'-';
                if (!alphanumeric && !isSeparator)
                {
                    return false;
                }
                if (isSeparator && separator)
                {
                    return false;
                }
                separator = isSeparator;
            }
            return !separator;
        }

        bool IsSafeArtifactId(std::wstring const& value)
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
                            ch == L'_' ||
                            ch == L'-';
                    });
        }

        bool IsCanonicalRelativePath(std::wstring const& value)
        {
            if (value.empty() ||
                value.size() > 240 ||
                value.front() == L'/' ||
                value.back() == L'/' ||
                value.find(L'\\') != std::wstring::npos ||
                value.find(L"//") != std::wstring::npos)
            {
                return false;
            }
            size_t start = 0;
            while (start < value.size())
            {
                auto end = value.find(L'/', start);
                auto part = value.substr(
                    start,
                    end == std::wstring::npos
                        ? std::wstring::npos
                        : end - start);
                if (part.empty() || part == L"." || part == L"..")
                {
                    return false;
                }
                if (!std::all_of(
                        part.begin(),
                        part.end(),
                        [](wchar_t ch)
                        {
                            return (ch >= L'a' && ch <= L'z') ||
                                (ch >= L'A' && ch <= L'Z') ||
                                (ch >= L'0' && ch <= L'9') ||
                                ch == L'.' ||
                                ch == L'_' ||
                                ch == L'-';
                        }))
                {
                    return false;
                }
                if (end == std::wstring::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return value != L"xcp-project.json" &&
                value != L"xcp-bundle.json";
        }

        std::wstring CanonicalJsonValue(IJsonValue const& value);

        std::wstring CanonicalJsonObject(JsonObject const& object)
        {
            std::map<std::wstring, IJsonValue> fields;
            for (auto const& field : object)
            {
                fields.emplace(
                    std::wstring(field.Key()),
                    field.Value());
            }
            std::wstring result = L"{";
            auto first = true;
            for (auto const& [name, fieldValue] : fields)
            {
                if (!first)
                {
                    result += L",";
                }
                first = false;
                result += JsonString(name) +
                    L":" +
                    CanonicalJsonValue(fieldValue);
            }
            result += L"}";
            return result;
        }

        std::wstring CanonicalJsonValue(IJsonValue const& value)
        {
            switch (value.ValueType())
            {
            case JsonValueType::Null:
                return L"null";
            case JsonValueType::Boolean:
                return value.GetBoolean() ? L"true" : L"false";
            case JsonValueType::String:
                return std::wstring(value.Stringify());
            case JsonValueType::Number:
            {
                auto number = value.GetNumber();
                if (!std::isfinite(number) ||
                    std::floor(number) != number ||
                    std::fabs(number) > 9007199254740991.0)
                {
                    Fail(
                        "xcp.creative.canonical_json_required",
                        "creative bundle numbers must be exact integers",
                        "bundle_validation",
                        "number",
                        {},
                        "exact JSON integer",
                        std::to_string(number),
                        "rebuild the bundle with the canonical XCP builder");
                }
                if (number < 0.0)
                {
                    return L"-" +
                        std::to_wstring(
                            static_cast<uint64_t>(-number));
                }
                return std::to_wstring(
                    static_cast<uint64_t>(number));
            }
            case JsonValueType::Array:
            {
                auto array = value.GetArray();
                std::wstring result = L"[";
                for (uint32_t index = 0; index < array.Size(); ++index)
                {
                    if (index != 0)
                    {
                        result += L",";
                    }
                    result += CanonicalJsonValue(array.GetAt(index));
                }
                result += L"]";
                return result;
            }
            case JsonValueType::Object:
                return CanonicalJsonObject(value.GetObject());
            default:
                Fail(
                    "xcp.creative.canonical_json_required",
                    "creative bundle contains an unsupported JSON value",
                    "bundle_validation",
                    "json",
                    {},
                    "JSON null, boolean, integer, string, array or object",
                    "unsupported value",
                    "rebuild the bundle with the canonical XCP builder");
            }
        }

        std::string ReadUtf8File(
            fs::path const& path,
            uint64_t maximumBytes,
            std::string const& stage,
            std::string const& field)
        {
            if (!fs::exists(path) || !fs::is_regular_file(path))
            {
                Fail(
                    "xcp.creative.file_missing",
                    "creative artifact blob is missing",
                    stage,
                    field,
                    WideToUtf8(path.wstring()),
                    "existing regular file",
                    "missing",
                    "upload and commit the exact artifact before retrying",
                    true);
            }
            auto bytes = static_cast<uint64_t>(fs::file_size(path));
            if (bytes > maximumBytes)
            {
                Fail(
                    "xcp.creative.file_size_exceeded",
                    "creative JSON document exceeds its host budget",
                    stage,
                    field,
                    WideToUtf8(path.wstring()),
                    "at most " + std::to_string(maximumBytes) + " bytes",
                    std::to_string(bytes),
                    "reduce the document and rebuild the bundle");
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                Fail(
                    "xcp.creative.file_unreadable",
                    "creative artifact blob cannot be opened",
                    stage,
                    field,
                    WideToUtf8(path.wstring()),
                    "readable file",
                    "open failed",
                    "restore or upload the exact artifact",
                    true);
            }
            return std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }

        JsonObject ParseJsonObject(
            std::string const& bytes,
            std::string const& stage,
            std::string const& field,
            std::string const& path)
        {
            try
            {
                auto value = JsonValue::Parse(Utf8ToWide(bytes));
                if (value.ValueType() != JsonValueType::Object)
                {
                    Fail(
                        "xcp.creative.json_root_invalid",
                        "creative JSON document root must be an object",
                        stage,
                        field,
                        path,
                        "JSON object",
                        "non-object",
                        "replace the document with an object and rebuild");
                }
                return value.GetObject();
            }
            catch (WorkerCreativeHostError const&)
            {
                throw;
            }
            catch (...)
            {
                Fail(
                    "xcp.creative.json_invalid",
                    "creative document is not valid UTF-8 JSON",
                    stage,
                    field,
                    path,
                    "valid UTF-8 JSON",
                    "parse failed",
                    "correct the JSON and rebuild the bundle");
            }
        }

        fs::path CreativeRoot(fs::path const& root)
        {
            return root.parent_path() / L"xcp-creative-v1";
        }

        fs::path CreativeInstallRoot(fs::path const& root)
        {
            return CreativeRoot(root) / L"installs";
        }

        fs::path CreativeInstallPath(
            fs::path const& root,
            std::wstring const& installId)
        {
            return CreativeInstallRoot(root) / (installId + L".json");
        }

        fs::path CreativeActivationRecordRoot(
            fs::path const& root,
            std::wstring const& projectId)
        {
            auto key = WorkerContentSha256(WideToUtf8(projectId));
            return CreativeRoot(root) /
                L"activations" /
                key /
                L"records";
        }

        std::wstring RequiredRequestString(
            JsonObject const& request,
            wchar_t const* name,
            std::string const& stage)
        {
            return RequiredString(
                request,
                name,
                stage,
                WideToUtf8(name));
        }

        void ValidateLifecycleRequestEnvelope(
            JsonObject const& request,
            std::wstring const& command,
            std::wstring const& schemaVersion,
            std::set<std::wstring> const& additionalFields)
        {
            auto allowed = additionalFields;
            allowed.insert(L"command");
            allowed.insert(L"schema_version");
            RequireExactFields(
                request,
                allowed,
                "request_validation",
                "$");
            if (RequiredRequestString(
                    request,
                    L"command",
                    "request_validation") != command)
            {
                Fail(
                    "xcp.creative.command_mismatch",
                    "creative request command does not match the routed command",
                    "request_validation",
                    "command",
                    {},
                    WideToUtf8(command),
                    WideToUtf8(
                        std::wstring(
                            request.GetNamedString(
                                L"command").c_str())),
                    "send the request through the matching creative command");
            }
            if (RequiredRequestString(
                    request,
                    L"schema_version",
                    "request_validation") != schemaVersion)
            {
                Fail(
                    "xcp.creative.request_schema_unsupported",
                    "creative request schema version is not supported",
                    "request_validation",
                    "schema_version",
                    {},
                    WideToUtf8(schemaVersion),
                    WideToUtf8(
                        std::wstring(
                            request.GetNamedString(
                                L"schema_version").c_str())),
                    "refresh describe_creative_host and use the published request schema");
            }
        }

        void ValidateExpectedProfile(JsonObject const& request)
        {
            auto expected = RequiredRequestString(
                request,
                L"expected_host_profile_sha256",
                "host_binding");
            if (!IsLowerHexSha256(expected) ||
                expected != WorkerCreativeHostProfileCanonicalSha256())
            {
                Fail(
                    "xcp.creative.host_profile_mismatch",
                    "creative request is not bound to the current host profile",
                    "host_binding",
                    "expected_host_profile_sha256",
                    {},
                    WideToUtf8(
                        WorkerCreativeHostProfileCanonicalSha256()),
                    WideToUtf8(expected),
                    "refresh describe_creative_host and rebuild or resubmit against its canonical profile hash");
            }
        }

        std::vector<std::wstring> StringArray(
            JsonArray const& array,
            std::string const& stage,
            std::string const& field,
            bool requireSortedUnique)
        {
            std::vector<std::wstring> result;
            std::set<std::wstring> seen;
            for (uint32_t index = 0; index < array.Size(); ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::String)
                {
                    Fail(
                        "xcp.creative.schema_rejected",
                        "creative string array contains a non-string item",
                        stage,
                        field,
                        {},
                        "array of strings",
                        "non-string item",
                        "correct the array and rebuild the bundle");
                }
                auto text = std::wstring(value.GetString());
                if (!seen.insert(text).second)
                {
                    Fail(
                        "xcp.creative.schema_rejected",
                        "creative string array contains a duplicate",
                        stage,
                        field,
                        WideToUtf8(text),
                        "unique values",
                        WideToUtf8(text),
                        "remove the duplicate and rebuild the bundle");
                }
                result.push_back(std::move(text));
            }
            if (requireSortedUnique &&
                !std::is_sorted(result.begin(), result.end()))
            {
                Fail(
                    "xcp.creative.canonical_json_required",
                    "creative string array is not in canonical order",
                    stage,
                    field,
                    {},
                    "ascending canonical order",
                    "unsorted",
                    "rebuild the bundle with the canonical XCP builder");
            }
            return result;
        }

        std::wstring ExpectedModuleSchemaVersion(std::wstring const& kind)
        {
            if (kind == L"xcp.canvas2d.v1") return L"xcp-canvas2d-module-v1";
            if (kind == L"xcp.ui.v1") return L"xcp-ui-module-v1";
            if (kind == L"xcp.behavior.v1") return L"xcp-behavior-module-v1";
            if (kind == L"xcp.data.v1") return L"xcp-data-module-v1";
            if (kind == L"xcp.world2d.v1") return L"xcp-world2d-module-v1";
            if (kind == L"xcp.world2d.v2") return L"xcp-world2d-module-v2";
            if (kind == L"xcp.world2d.campaign.v1")
                return L"xcp-world2d-campaign-module-v1";
            if (kind == L"xcp.world2d.motion.v1")
                return L"xcp-world2d-motion-module-v1";
            if (kind == L"xcp.scene3d.v1") return L"xcp-scene3d-module-v1";
            if (kind == L"xcp.audio.v1") return L"xcp-audio-module-v1";
            if (kind == L"xcp.xvm.v2") return L"xcp-xvm-module-v1";
            return {};
        }

        std::set<std::wstring> RequiredCapabilitiesForKind(
            std::wstring const& kind)
        {
            if (kind == L"xcp.canvas2d.v1")
            {
                return { L"input.gamepad", L"render.canvas2d" };
            }
            if (kind == L"xcp.ui.v1")
            {
                return { L"input.gamepad", L"ui.controller" };
            }
            if (kind == L"xcp.behavior.v1")
            {
                return { L"behavior.deterministic" };
            }
            if (kind == L"xcp.data.v1")
            {
                return { L"state.local" };
            }
            if (kind == L"xcp.world2d.v1")
            {
                return {
                    L"input.gamepad",
                    L"render.canvas2d",
                    L"world2d.deterministic"
                };
            }
            if (kind == L"xcp.world2d.v2")
            {
                return {
                    L"input.gamepad",
                    L"render.canvas2d",
                    L"render.sprite_atlas",
                    L"world2d.deterministic",
                    L"world2d.gravity"
                };
            }
            if (kind == L"xcp.world2d.campaign.v1")
            {
                return {
                    L"input.gamepad",
                    L"render.canvas2d",
                    L"render.sprite_atlas",
                    L"state.local",
                    L"world2d.campaign",
                    L"world2d.deterministic",
                    L"world2d.gravity"
                };
            }
            if (kind == L"xcp.world2d.motion.v1")
            {
                return {
                    L"world2d.fixed-step-interpolation",
                    L"world2d.incremental-gravity"
                };
            }
            if (kind == L"xcp.scene3d.v1")
            {
                return {
                    L"animation.scene3d",
                    L"collision.basic",
                    L"input.gamepad",
                    L"render.scene3d"
                };
            }
            if (kind == L"xcp.audio.v1")
            {
                return { L"audio.playback" };
            }
            if (kind == L"xcp.xvm.v2")
            {
                return { L"xvm.cpu-reference" };
            }
            return {};
        }

        std::set<std::wstring> ModuleTopLevelFields(
            std::wstring const& kind)
        {
            if (kind == L"xcp.canvas2d.v1")
            {
                return {
                    L"schema_version",
                    L"canvas",
                    L"nodes",
                    L"input_bindings"
                };
            }
            if (kind == L"xcp.ui.v1")
            {
                return {
                    L"schema_version",
                    L"root_id",
                    L"elements",
                    L"initial_focus_id"
                };
            }
            if (kind == L"xcp.behavior.v1")
            {
                return {
                    L"schema_version",
                    L"state",
                    L"timers",
                    L"rules"
                };
            }
            if (kind == L"xcp.data.v1")
            {
                return {
                    L"schema_version",
                    L"constants",
                    L"local_state"
                };
            }
            if (kind == L"xcp.world2d.v1" ||
                kind == L"xcp.world2d.v2")
            {
                return {
                    L"schema_version",
                    L"world",
                    L"tiles",
                    L"entities",
                    L"input_bindings",
                    L"objectives"
                };
            }
            if (kind == L"xcp.world2d.campaign.v1")
            {
                return {
                    L"schema_version",
                    L"entry_scene",
                    L"scenes",
                    L"transitions",
                    L"input_bindings"
                };
            }
            if (kind == L"xcp.world2d.motion.v1")
            {
                return {
                    L"schema_version",
                    L"fixed_step_hz",
                    L"bindings"
                };
            }
            if (kind == L"xcp.scene3d.v1")
            {
                return {
                    L"schema_version",
                    L"coordinate_system",
                    L"materials",
                    L"meshes",
                    L"nodes",
                    L"cameras",
                    L"active_camera",
                    L"lights",
                    L"input_bindings",
                    L"animations"
                };
            }
            if (kind == L"xcp.audio.v1")
            {
                return {
                    L"schema_version",
                    L"buses",
                    L"clips",
                    L"cues"
                };
            }
            if (kind == L"xcp.xvm.v2")
            {
                return {
                    L"schema_version",
                    L"services"
                };
            }
            return {};
        }

        void ValidateModuleDocument(
            fs::path const& path,
            CreativeFile const& file)
        {
            auto bytes = ReadUtf8File(
                path,
                MaxModuleDocumentBytes,
                "module_validation",
                "files[].path");
            auto module = ParseJsonObject(
                bytes,
                "module_validation",
                "module",
                WideToUtf8(file.path));
            auto fields = ModuleTopLevelFields(file.kind);
            RequireExactFields(
                module,
                fields,
                "module_validation",
                WideToUtf8(file.path));
            for (auto const& field : fields)
            {
                if (!module.HasKey(field))
                {
                    Fail(
                        "xcp.creative.module_schema_rejected",
                        "creative module is missing a required top-level field",
                        "module_validation",
                        WideToUtf8(field),
                        WideToUtf8(file.path),
                        "field required by the authoritative module schema",
                        "missing",
                        "correct the module using the published schema and rebuild");
                }
            }
            auto expected = ExpectedModuleSchemaVersion(file.kind);
            auto actual = RequiredString(
                module,
                L"schema_version",
                "module_validation",
                "schema_version");
            if (actual != expected)
            {
                Fail(
                    "xcp.creative.module_schema_version_mismatch",
                    "creative module schema version does not match its bundle kind",
                    "module_validation",
                    "schema_version",
                    WideToUtf8(file.path),
                    WideToUtf8(expected),
                    WideToUtf8(actual),
                    "use the schema version published for the declared module kind");
            }
        }

        CreativeFile ParseBundleFile(
            JsonObject const& object,
            std::wstring const& role,
            uint32_t index)
        {
            auto fieldPrefix =
                WideToUtf8(role) +
                "s[" +
                std::to_string(index) +
                "]";
            auto allowed = role == L"module"
                ? std::set<std::wstring>{
                    L"id", L"kind", L"path", L"depends_on", L"bytes", L"sha256" }
                : std::set<std::wstring>{
                    L"id", L"path", L"media_type", L"bytes", L"sha256" };
            RequireExactFields(
                object,
                allowed,
                "bundle_validation",
                fieldPrefix);

            CreativeFile file;
            file.role = role;
            file.id = RequiredString(
                object,
                L"id",
                "bundle_validation",
                fieldPrefix + ".id");
            file.path = RequiredString(
                object,
                L"path",
                "bundle_validation",
                fieldPrefix + ".path");
            file.sha256 = RequiredString(
                object,
                L"sha256",
                "bundle_validation",
                fieldPrefix + ".sha256");
            file.bytes = RequiredUInt64(
                object,
                L"bytes",
                "bundle_validation",
                fieldPrefix + ".bytes");
            if (!IsSafeIdentifier(file.id) ||
                !IsCanonicalRelativePath(file.path) ||
                !IsLowerHexSha256(file.sha256) ||
                file.bytes == 0 ||
                file.bytes > MaxFileBytes)
            {
                Fail(
                    "xcp.creative.bundle_file_invalid",
                    "creative bundle file identity is invalid",
                    "bundle_validation",
                    fieldPrefix,
                    WideToUtf8(file.path),
                    "safe id/path, lowercase SHA-256, and bounded nonzero bytes",
                    "invalid identity",
                    "rebuild the bundle with the canonical XCP builder");
            }
            if (role == L"module")
            {
                file.kind = RequiredString(
                    object,
                    L"kind",
                    "bundle_validation",
                    fieldPrefix + ".kind");
                if (ExpectedModuleSchemaVersion(file.kind).empty())
                {
                    Fail(
                        "xcp.creative.module_kind_unsupported",
                        "creative bundle declares a module kind unknown to this candidate host",
                        "bundle_validation",
                        fieldPrefix + ".kind",
                        {},
                        "module kind published by describe_creative_host",
                        WideToUtf8(file.kind),
                        "use a published module kind or upgrade the host");
                }
                file.dependencies = StringArray(
                    RequiredArray(
                        object,
                        L"depends_on",
                        "bundle_validation",
                        fieldPrefix + ".depends_on"),
                    "bundle_validation",
                    fieldPrefix + ".depends_on",
                    false);
                for (auto const& dependency : file.dependencies)
                {
                    if (!IsSafeIdentifier(dependency))
                    {
                        Fail(
                            "xcp.creative.module_dependency_invalid",
                            "creative module dependency id is invalid",
                            "bundle_validation",
                            fieldPrefix + ".depends_on",
                            WideToUtf8(dependency),
                            "safe module id",
                            WideToUtf8(dependency),
                            "correct the dependency and rebuild");
                    }
                }
            }
            else
            {
                auto mediaType = RequiredString(
                    object,
                    L"media_type",
                    "bundle_validation",
                    fieldPrefix + ".media_type");
                if (mediaType.size() < 3 ||
                    mediaType.size() > 128 ||
                    mediaType.find(L'/') == std::wstring::npos)
                {
                    Fail(
                        "xcp.creative.asset_media_type_invalid",
                        "creative asset media type is invalid",
                        "bundle_validation",
                        fieldPrefix + ".media_type",
                        WideToUtf8(file.path),
                        "bounded type/subtype media type",
                        WideToUtf8(mediaType),
                        "correct the media type and rebuild");
                }
                file.kind = std::move(mediaType);
            }
            return file;
        }

        std::string ContentRecordsCanonical(
            std::vector<CreativeFile> files)
        {
            std::sort(
                files.begin(),
                files.end(),
                [](CreativeFile const& left, CreativeFile const& right)
                {
                    return std::tie(left.role, left.id, left.path) <
                        std::tie(right.role, right.id, right.path);
                });
            std::wstring result = L"[";
            for (size_t index = 0; index < files.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                auto const& file = files[index];
                result +=
                    L"{\"bytes\":" + std::to_wstring(file.bytes) +
                    L",\"id\":" + JsonString(file.id) +
                    L",\"path\":" + JsonString(file.path) +
                    L",\"role\":" + JsonString(file.role) +
                    L",\"sha256\":" + JsonString(file.sha256) +
                    L"}";
            }
            result += L"]\n";
            return WideToUtf8(result);
        }

        CreativeBundle LoadAndValidateBundle(
            JsonObject const& request,
            fs::path const& root)
        {
            CreativeBundle bundle;
            bundle.artifactId = RequiredRequestString(
                request,
                L"bundle_artifact_id",
                "bundle_validation");
            auto expectedBundleSha256 = RequiredRequestString(
                request,
                L"expected_bundle_sha256",
                "bundle_validation");
            if (!IsSafeArtifactId(bundle.artifactId) ||
                !IsLowerHexSha256(expectedBundleSha256))
            {
                Fail(
                    "xcp.creative.bundle_binding_invalid",
                    "creative bundle artifact binding is invalid",
                    "bundle_validation",
                    "bundle_artifact_id/expected_bundle_sha256",
                    {},
                    "safe artifact id and lowercase SHA-256",
                    "invalid binding",
                    "use the exact committed bundle-manifest artifact identity");
            }

            ArtifactReadTarget target;
            try
            {
                target = ResolveArtifactReadTarget(root, bundle.artifactId);
            }
            catch (WorkerArtifactStoreError const& error)
            {
                Fail(
                    "xcp.creative.bundle_artifact_missing",
                    "creative bundle artifact is not available",
                    "bundle_validation",
                    "bundle_artifact_id",
                    WideToUtf8(bundle.artifactId),
                    "committed xcp-creative-bundle-v1 artifact",
                    error.code,
                    "upload and commit the exact bundle manifest artifact",
                    true);
            }
            if (!target.committed ||
                target.artifactKind != L"xcp-creative-bundle-v1" ||
                target.sha256 != expectedBundleSha256)
            {
                Fail(
                    "xcp.creative.bundle_artifact_mismatch",
                    "creative bundle artifact does not match the sealed request",
                    "bundle_validation",
                    "bundle_artifact_id",
                    WideToUtf8(bundle.artifactId),
                    "committed xcp-creative-bundle-v1 at expected SHA-256",
                    WideToUtf8(target.artifactKind + L":" + target.sha256),
                    "commit the canonical bundle manifest with the expected kind and hash");
            }
            bundle.bundleSha256 = target.sha256;
            bundle.bundleBytes =
                static_cast<uint64_t>(fs::file_size(target.path));
            if (bundle.bundleBytes != target.bytes ||
                bundle.bundleBytes > MaxBundleManifestBytes ||
                WorkerContentSha256File(target.path) != target.sha256)
            {
                Fail(
                    "xcp.creative.bundle_artifact_corrupt",
                    "creative bundle artifact failed exact size or hash verification",
                    "bundle_validation",
                    "bundle_artifact_id",
                    WideToUtf8(bundle.artifactId),
                    "manifest bytes and hash matching the artifact record",
                    "size or hash mismatch",
                    "delete the corrupt artifact and upload the canonical bundle again");
            }

            auto bytes = ReadUtf8File(
                target.path,
                MaxBundleManifestBytes,
                "bundle_validation",
                "bundle_artifact_id");
            auto manifest = ParseJsonObject(
                bytes,
                "bundle_validation",
                "bundle_artifact_id",
                WideToUtf8(bundle.artifactId));
            auto canonical = WideToUtf8(
                CanonicalJsonObject(manifest) + L"\n");
            if (bytes != canonical)
            {
                Fail(
                    "xcp.creative.canonical_json_required",
                    "creative bundle manifest is not canonical XCP JSON",
                    "bundle_validation",
                    "bundle_artifact_id",
                    WideToUtf8(bundle.artifactId),
                    "sorted compact UTF-8 JSON with one trailing LF",
                    "non-canonical bytes",
                    "rebuild with tools/xcp_creative_project.py");
            }

            RequireExactFields(
                manifest,
                {
                    L"schema_version",
                    L"project",
                    L"entry_module",
                    L"requested_capabilities",
                    L"modules",
                    L"assets",
                    L"integrity"
                },
                "bundle_validation",
                "$");
            auto schemaVersion = RequiredString(
                manifest,
                L"schema_version",
                "bundle_validation",
                "schema_version");
            if (schemaVersion != L"xcp-creative-bundle-v1")
            {
                Fail(
                    "xcp.creative.bundle_schema_unsupported",
                    "creative bundle schema is not supported",
                    "bundle_validation",
                    "schema_version",
                    {},
                    "xcp-creative-bundle-v1",
                    WideToUtf8(schemaVersion),
                    "rebuild the project with a compatible builder");
            }

            auto project = RequiredObject(
                manifest,
                L"project",
                "bundle_validation",
                "project");
            RequireExactFields(
                project,
                { L"id", L"version", L"title", L"description", L"tags" },
                "bundle_validation",
                "project");
            bundle.projectId = RequiredString(
                project,
                L"id",
                "bundle_validation",
                "project.id");
            bundle.projectVersion = RequiredString(
                project,
                L"version",
                "bundle_validation",
                "project.version");
            if (!IsSafeIdentifier(bundle.projectId) ||
                bundle.projectVersion.empty() ||
                bundle.projectVersion.size() > 64)
            {
                Fail(
                    "xcp.creative.project_identity_invalid",
                    "creative project identity is invalid",
                    "bundle_validation",
                    "project.id/project.version",
                    {},
                    "safe project id and bounded semantic version",
                    WideToUtf8(bundle.projectId + L":" + bundle.projectVersion),
                    "correct the project identity and rebuild");
            }
            RequiredString(
                project,
                L"title",
                "bundle_validation",
                "project.title");
            RequiredString(
                project,
                L"description",
                "bundle_validation",
                "project.description");
            StringArray(
                RequiredArray(
                    project,
                    L"tags",
                    "bundle_validation",
                    "project.tags"),
                "bundle_validation",
                "project.tags",
                true);

            bundle.entryModule = RequiredString(
                manifest,
                L"entry_module",
                "bundle_validation",
                "entry_module");
            bundle.requestedCapabilities = StringArray(
                RequiredArray(
                    manifest,
                    L"requested_capabilities",
                    "bundle_validation",
                    "requested_capabilities"),
                "bundle_validation",
                "requested_capabilities",
                true);
            std::set<std::wstring> admittedCapabilityContracts{
                L"animation.scene3d",
                L"audio.playback",
                L"behavior.deterministic",
                L"collision.basic",
                L"input.gamepad",
                L"render.canvas2d",
                L"render.sprite_atlas",
                L"render.scene3d",
                L"state.local",
                L"ui.controller",
                L"world2d.campaign",
                L"world2d.deterministic",
                L"world2d.fixed-step-interpolation",
                L"world2d.gravity",
                L"world2d.incremental-gravity",
                L"xvm.cpu-reference"
            };
            std::set<std::wstring> requestedSet(
                bundle.requestedCapabilities.begin(),
                bundle.requestedCapabilities.end());
            for (auto const& capability : bundle.requestedCapabilities)
            {
                if (admittedCapabilityContracts.find(capability) ==
                    admittedCapabilityContracts.end())
                {
                    Fail(
                        "xcp.creative.capability_unsupported",
                        "creative bundle requests a capability unknown to this candidate host",
                        "bundle_validation",
                        "requested_capabilities",
                        {},
                        "capability published by describe_creative_host",
                        WideToUtf8(capability),
                        "remove the capability or upgrade the host");
                }
            }

            auto modules = RequiredArray(
                manifest,
                L"modules",
                "bundle_validation",
                "modules");
            auto assets = RequiredArray(
                manifest,
                L"assets",
                "bundle_validation",
                "assets");
            if (modules.Size() == 0 ||
                modules.Size() > MaxModuleCount ||
                assets.Size() > MaxAssetCount ||
                static_cast<uint64_t>(modules.Size()) +
                    static_cast<uint64_t>(assets.Size()) >
                    MaxFileCount)
            {
                Fail(
                    "xcp.creative.bundle_count_exceeded",
                    "creative bundle exceeds the candidate host count budget",
                    "bundle_validation",
                    "modules/assets",
                    {},
                    "1..128 modules, 0..256 assets, at most 384 files",
                    std::to_string(modules.Size()) + "/" +
                        std::to_string(assets.Size()),
                    "split or reduce the project and rebuild");
            }

            std::set<std::wstring> moduleIds;
            std::set<std::wstring> assetIds;
            std::set<std::wstring> runtimePaths;
            uint64_t totalBytes = 0;
            for (uint32_t index = 0; index < modules.Size(); ++index)
            {
                auto value = modules.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    Fail(
                        "xcp.creative.schema_rejected",
                        "creative modules array contains a non-object",
                        "bundle_validation",
                        "modules",
                        {},
                        "module objects",
                        "non-object",
                        "correct the module entry and rebuild");
                }
                auto file = ParseBundleFile(
                    value.GetObject(),
                    L"module",
                    index);
                if (!moduleIds.insert(file.id).second ||
                    !runtimePaths.insert(file.path).second)
                {
                    Fail(
                        "xcp.creative.bundle_identity_duplicate",
                        "creative bundle contains a duplicate module id or runtime path",
                        "bundle_validation",
                        "modules",
                        WideToUtf8(file.path),
                        "unique module ids and runtime paths",
                        WideToUtf8(file.id),
                        "rename the duplicate and rebuild");
                }
                for (auto const& capability :
                     RequiredCapabilitiesForKind(file.kind))
                {
                    if (requestedSet.find(capability) == requestedSet.end())
                    {
                        Fail(
                            "xcp.creative.module_capability_not_requested",
                            "creative module requires a capability not requested by the project",
                            "bundle_validation",
                            "requested_capabilities",
                            WideToUtf8(file.path),
                            WideToUtf8(capability),
                            "missing",
                            "add the capability to xcp-project.json and rebuild");
                    }
                }
                if (totalBytes > MaxProjectBytes - file.bytes)
                {
                    Fail(
                        "xcp.creative.project_budget_exceeded",
                        "creative project exceeds the candidate host byte budget",
                        "bundle_validation",
                        "integrity.total_bytes",
                        {},
                        "at most 4294967296 bytes",
                        "overflow",
                        "reduce the project and rebuild");
                }
                totalBytes += file.bytes;
                bundle.files.push_back(std::move(file));
            }
            std::map<std::wstring, std::vector<std::wstring>>
                dependencyMap;
            for (auto const& file : bundle.files)
            {
                if (file.role != L"module")
                {
                    continue;
                }
                for (auto const& dependency :
                     file.dependencies)
                {
                    if (moduleIds.find(dependency) ==
                            moduleIds.end() ||
                        dependency == file.id)
                    {
                        Fail(
                            "xcp.creative.module_dependency_invalid",
                            "creative module dependency is missing or self-referential",
                            "bundle_validation",
                            "modules[].depends_on",
                            WideToUtf8(file.path),
                            "a different declared module id",
                            WideToUtf8(dependency),
                            "correct the dependency graph and rebuild");
                    }
                }
                dependencyMap.emplace(
                    file.id,
                    file.dependencies);
            }
            std::map<std::wstring, uint32_t> dependencyState;
            std::vector<std::wstring> dependencyChain;
            std::function<void(std::wstring const&)> visitDependency =
                [&](std::wstring const& moduleId)
            {
                if (dependencyState[moduleId] == 2)
                {
                    return;
                }
                if (dependencyState[moduleId] == 1)
                {
                    Fail(
                        "xcp.creative.module_dependency_cycle",
                        "creative module dependency graph contains a cycle",
                        "bundle_validation",
                        "modules[].depends_on",
                        {},
                        "acyclic dependency graph",
                        WideToUtf8(moduleId),
                        "remove at least one dependency in the cycle and rebuild");
                }
                dependencyState[moduleId] = 1;
                dependencyChain.push_back(moduleId);
                for (auto const& dependency :
                     dependencyMap[moduleId])
                {
                    visitDependency(dependency);
                }
                dependencyChain.pop_back();
                dependencyState[moduleId] = 2;
            };
            for (auto const& moduleId : moduleIds)
            {
                visitDependency(moduleId);
            }
            for (uint32_t index = 0; index < assets.Size(); ++index)
            {
                auto value = assets.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    Fail(
                        "xcp.creative.schema_rejected",
                        "creative assets array contains a non-object",
                        "bundle_validation",
                        "assets",
                        {},
                        "asset objects",
                        "non-object",
                        "correct the asset entry and rebuild");
                }
                auto file = ParseBundleFile(
                    value.GetObject(),
                    L"asset",
                    index);
                static std::set<std::wstring> const
                    AdmittedAssetMediaTypes{
                        L"application/vnd.xcp.mesh-v1+json",
                        L"application/vnd.xcp.xvm-program-v2+json",
                        L"audio/wav",
                        L"image/png"
                    };
                if (AdmittedAssetMediaTypes.find(file.kind) ==
                    AdmittedAssetMediaTypes.end())
                {
                    Fail(
                        "xcp.creative.asset_format_unsupported",
                        "creative asset format is not published by this host",
                        "bundle_validation",
                        "assets[].media_type",
                        WideToUtf8(file.path),
                        "admitted media type from describe_creative_host",
                        WideToUtf8(file.kind),
                        "remove the asset or convert it to a published format");
                }
                auto formatLimit =
                    file.kind ==
                        L"application/vnd.xcp.xvm-program-v2+json"
                    ? 1ull * 1024ull * 1024ull
                    : 16ull * 1024ull * 1024ull;
                if (file.bytes > formatLimit)
                {
                    Fail(
                        "xcp.creative.asset_format_budget_exceeded",
                        "creative asset exceeds its published format byte budget",
                        "bundle_validation",
                        "assets[].bytes",
                        WideToUtf8(file.path),
                        "at most " +
                            std::to_string(formatLimit) +
                            " bytes",
                        std::to_string(file.bytes),
                        "reduce or split the asset and rebuild");
                }
                if (!assetIds.insert(file.id).second ||
                    !runtimePaths.insert(file.path).second)
                {
                    Fail(
                        "xcp.creative.bundle_identity_duplicate",
                        "creative bundle contains a duplicate asset id or runtime path",
                        "bundle_validation",
                        "assets",
                        WideToUtf8(file.path),
                        "unique asset ids and runtime paths",
                        WideToUtf8(file.id),
                        "rename the duplicate and rebuild");
                }
                if (totalBytes > MaxProjectBytes - file.bytes)
                {
                    Fail(
                        "xcp.creative.project_budget_exceeded",
                        "creative project exceeds the candidate host byte budget",
                        "bundle_validation",
                        "integrity.total_bytes",
                        {},
                        "at most 4294967296 bytes",
                        "overflow",
                        "reduce the project and rebuild");
                }
                totalBytes += file.bytes;
                bundle.files.push_back(std::move(file));
            }
            if (moduleIds.find(bundle.entryModule) == moduleIds.end())
            {
                Fail(
                    "xcp.creative.entry_module_missing",
                    "creative entry module does not name a declared module",
                    "bundle_validation",
                    "entry_module",
                    {},
                    "declared module id",
                    WideToUtf8(bundle.entryModule),
                    "select a declared module and rebuild");
            }

            auto integrity = RequiredObject(
                manifest,
                L"integrity",
                "bundle_validation",
                "integrity");
            RequireExactFields(
                integrity,
                {
                    L"algorithm",
                    L"file_count",
                    L"total_bytes",
                    L"project_manifest_sha256",
                    L"content_sha256"
                },
                "bundle_validation",
                "integrity");
            auto algorithm = RequiredString(
                integrity,
                L"algorithm",
                "bundle_validation",
                "integrity.algorithm");
            auto fileCount = RequiredUInt64(
                integrity,
                L"file_count",
                "bundle_validation",
                "integrity.file_count");
            auto declaredTotal = RequiredUInt64(
                integrity,
                L"total_bytes",
                "bundle_validation",
                "integrity.total_bytes");
            auto projectManifestSha = RequiredString(
                integrity,
                L"project_manifest_sha256",
                "bundle_validation",
                "integrity.project_manifest_sha256");
            bundle.contentSha256 = RequiredString(
                integrity,
                L"content_sha256",
                "bundle_validation",
                "integrity.content_sha256");
            auto actualContentSha =
                WorkerContentSha256(
                    ContentRecordsCanonical(bundle.files));
            if (algorithm != L"sha256" ||
                fileCount != bundle.files.size() ||
                declaredTotal != totalBytes ||
                !IsLowerHexSha256(projectManifestSha) ||
                !IsLowerHexSha256(bundle.contentSha256) ||
                bundle.contentSha256 != actualContentSha)
            {
                Fail(
                    "xcp.creative.bundle_integrity_invalid",
                    "creative bundle integrity record is invalid",
                    "bundle_validation",
                    "integrity",
                    {},
                    "exact file count, bytes and canonical content SHA-256",
                    WideToUtf8(bundle.contentSha256),
                    "rebuild the bundle from trusted project source");
            }
            return bundle;
        }

        std::vector<std::wstring> ValidateContentBlobs(
            CreativeBundle const& bundle,
            fs::path const& root,
            bool requireComplete)
        {
            std::vector<std::wstring> missing;
            for (auto const& file : bundle.files)
            {
                auto path = ArtifactBlobPath(root, file.sha256);
                if (!fs::exists(path) ||
                    !fs::is_regular_file(path))
                {
                    missing.push_back(file.sha256);
                    continue;
                }
                auto actualBytes =
                    static_cast<uint64_t>(fs::file_size(path));
                if (actualBytes != file.bytes ||
                    WorkerContentSha256File(path) != file.sha256)
                {
                    Fail(
                        "xcp.creative.content_blob_invalid",
                        "creative content blob failed exact size or hash verification",
                        "content_validation",
                        "files[].sha256",
                        WideToUtf8(file.path),
                        WideToUtf8(file.sha256) + ":" +
                            std::to_string(file.bytes),
                        WideToUtf8(
                            WorkerContentSha256File(path)) +
                            ":" +
                            std::to_string(actualBytes),
                        "delete the corrupt artifact and upload the exact bundle file");
                }
                if (file.role == L"module")
                {
                    ValidateModuleDocument(path, file);
                }
            }
            std::sort(missing.begin(), missing.end());
            missing.erase(
                std::unique(missing.begin(), missing.end()),
                missing.end());
            if (requireComplete && !missing.empty())
            {
                Fail(
                    "xcp.creative.content_missing",
                    "creative install is missing one or more content-addressed blobs",
                    "content_validation",
                    "files[].sha256",
                    {},
                    "all bundle file hashes present in the worker CAS",
                    WideToUtf8(missing.front()),
                    "upload and commit every missing file hash before committing the install",
                    true);
            }
            return missing;
        }

        void ValidateInstalledContentBlobs(
            InstalledCreativeBundle const& install,
            fs::path const& root)
        {
            auto validate =
                [&](std::wstring const& sha256,
                    uint64_t expectedBytes,
                    std::wstring const& runtimePath)
            {
                auto path = ArtifactBlobPath(root, sha256);
                if (!fs::exists(path) ||
                    !fs::is_regular_file(path))
                {
                    Fail(
                        "xcp.creative.installed_content_missing",
                        "creative installed content blob is missing",
                        "activation_content_validation",
                        "cas_references",
                        WideToUtf8(runtimePath),
                        WideToUtf8(sha256),
                        "missing",
                        "restore or recommit the exact installed bundle before activation");
                }
                auto actualBytes =
                    static_cast<uint64_t>(fs::file_size(path));
                auto actualSha256 =
                    WorkerContentSha256File(path);
                if (actualBytes != expectedBytes ||
                    actualSha256 != sha256)
                {
                    Fail(
                        "xcp.creative.installed_content_invalid",
                        "creative installed content blob failed exact size or hash verification",
                        "activation_content_validation",
                        "cas_references",
                        WideToUtf8(runtimePath),
                        WideToUtf8(sha256) + ":" +
                            std::to_string(expectedBytes),
                        WideToUtf8(actualSha256) + ":" +
                            std::to_string(actualBytes),
                        "remove the corrupt install and recommit the exact bundle");
                }
            };

            validate(
                install.bundleSha256,
                install.bundleBytes,
                L"xcp-bundle.json");
            for (auto const& file : install.files)
            {
                validate(
                    file.sha256,
                    file.bytes,
                    file.path);
                if (file.role == L"module")
                {
                    ValidateModuleDocument(
                        ArtifactBlobPath(root, file.sha256),
                        file);
                }
            }
        }

        std::wstring InstallIdFor(
            std::wstring const& projectId,
            std::wstring const& projectVersion,
            std::wstring const& bundleSha256,
            std::wstring const& hostProfileCanonicalSha256)
        {
            return WorkerContentSha256(
                std::string("xcp-creative-install-v1\n") +
                WideToUtf8(projectId) + "\n" +
                WideToUtf8(projectVersion) + "\n" +
                WideToUtf8(bundleSha256) + "\n" +
                WideToUtf8(hostProfileCanonicalSha256) + "\n");
        }

        std::wstring InstallIdFor(CreativeBundle const& bundle)
        {
            return InstallIdFor(
                bundle.projectId,
                bundle.projectVersion,
                bundle.bundleSha256,
                WorkerCreativeHostProfileCanonicalSha256());
        }

        JsonObject InstalledRecordJson(
            CreativeBundle const& bundle,
            std::wstring const& installId)
        {
            JsonObject record;
            record.Insert(
                L"schema_version",
                JsonValue::CreateStringValue(
                    L"xcp-creative-installed-bundle-v1"));
            record.Insert(
                L"install_id",
                JsonValue::CreateStringValue(installId));
            record.Insert(
                L"project_id",
                JsonValue::CreateStringValue(bundle.projectId));
            record.Insert(
                L"project_version",
                JsonValue::CreateStringValue(bundle.projectVersion));
            record.Insert(
                L"entry_module",
                JsonValue::CreateStringValue(bundle.entryModule));
            record.Insert(
                L"bundle_sha256",
                JsonValue::CreateStringValue(bundle.bundleSha256));
            record.Insert(
                L"bundle_bytes",
                JsonValue::CreateNumberValue(
                    static_cast<double>(bundle.bundleBytes)));
            record.Insert(
                L"content_sha256",
                JsonValue::CreateStringValue(bundle.contentSha256));
            uint64_t totalBytes = 0;
            JsonArray files;
            std::map<std::wstring, uint64_t> references{
                { bundle.bundleSha256, bundle.bundleBytes }
            };
            for (auto const& file : bundle.files)
            {
                totalBytes += file.bytes;
                auto found = references.find(file.sha256);
                if (found != references.end() &&
                    found->second != file.bytes)
                {
                    Fail(
                        "xcp.creative.content_identity_conflict",
                        "same creative content hash declares different byte lengths",
                        "install_commit",
                        "files[].sha256",
                        WideToUtf8(file.path),
                        std::to_string(found->second),
                        std::to_string(file.bytes),
                        "rebuild the bundle from trusted source");
                }
                references[file.sha256] = file.bytes;
                JsonObject entry;
                entry.Insert(
                    L"role",
                    JsonValue::CreateStringValue(file.role));
                entry.Insert(
                    L"id",
                    JsonValue::CreateStringValue(file.id));
                entry.Insert(
                    L"kind",
                    JsonValue::CreateStringValue(file.kind));
                entry.Insert(
                    L"path",
                    JsonValue::CreateStringValue(file.path));
                entry.Insert(
                    L"bytes",
                    JsonValue::CreateNumberValue(
                        static_cast<double>(file.bytes)));
                entry.Insert(
                    L"sha256",
                    JsonValue::CreateStringValue(file.sha256));
                files.Append(entry);
            }
            record.Insert(L"files", files);
            record.Insert(
                L"file_count",
                JsonValue::CreateNumberValue(
                    static_cast<double>(bundle.files.size())));
            record.Insert(
                L"total_bytes",
                JsonValue::CreateNumberValue(
                    static_cast<double>(totalBytes)));
            JsonArray casReferences;
            for (auto const& [sha256, ignoredBytes] : references)
            {
                (void)ignoredBytes;
                casReferences.Append(
                    JsonValue::CreateStringValue(sha256));
            }
            record.Insert(L"cas_references", casReferences);
            record.Insert(
                L"host_profile_id",
                JsonValue::CreateStringValue(
                    L"xcp.creative.host.development-v1"));
            record.Insert(
                L"host_profile_canonical_sha256",
                JsonValue::CreateStringValue(
                    WorkerCreativeHostProfileCanonicalSha256()));
            return record;
        }

        std::string CanonicalBytes(JsonObject const& object)
        {
            return WideToUtf8(
                CanonicalJsonObject(object) + L"\n");
        }

        void WriteImmutableRecord(
            fs::path const& path,
            std::string const& bytes,
            std::string const& conflictCode)
        {
            fs::create_directories(path.parent_path());
            if (fs::exists(path))
            {
                auto existing = ReadUtf8File(
                    path,
                    MaxBundleManifestBytes,
                    "metadata_validation",
                    "record");
                if (existing == bytes)
                {
                    return;
                }
                Fail(
                    conflictCode,
                    "creative immutable record already exists with different bytes",
                    "metadata_commit",
                    "record",
                    WideToUtf8(path.wstring()),
                    "absent or byte-identical immutable record",
                    "conflicting record",
                    "use a fresh exact version or repair the corrupted private metadata");
            }
            auto staging = path;
            staging += L".staging";
            if (fs::exists(staging))
            {
                fs::remove(staging);
            }
            {
                std::ofstream output(
                    staging,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    Fail(
                        "xcp.creative.metadata_write_failed",
                        "creative immutable record staging file cannot be created",
                        "metadata_commit",
                        "record",
                        WideToUtf8(staging.wstring()),
                        "writable app-private metadata",
                        "create failed",
                        "retry after checking app-private storage health",
                        true);
                }
                output.write(
                    bytes.data(),
                    static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output)
                {
                    fs::remove(staging);
                    Fail(
                        "xcp.creative.metadata_write_failed",
                        "creative immutable record staging write failed",
                        "metadata_commit",
                        "record",
                        WideToUtf8(staging.wstring()),
                        "complete durable staging write",
                        "write failed",
                        "retry after checking app-private storage health",
                        true);
                }
            }
            if (WorkerContentSha256File(staging) !=
                WorkerContentSha256(bytes))
            {
                fs::remove(staging);
                Fail(
                    "xcp.creative.metadata_write_failed",
                    "creative immutable record staging hash mismatch",
                    "metadata_commit",
                    "record",
                    WideToUtf8(staging.wstring()),
                    "staged bytes equal source bytes",
                    "hash mismatch",
                    "retry after checking app-private storage health",
                    true);
            }
            std::error_code error;
            fs::rename(staging, path, error);
            if (error)
            {
                if (fs::exists(path) &&
                    ReadUtf8File(
                        path,
                        MaxBundleManifestBytes,
                        "metadata_validation",
                        "record") == bytes)
                {
                    fs::remove(staging);
                    return;
                }
                fs::remove(staging);
                Fail(
                    "xcp.creative.metadata_commit_failed",
                    "creative immutable record could not be atomically committed",
                    "metadata_commit",
                    "record",
                    WideToUtf8(path.wstring()),
                    "atomic rename to a new immutable record",
                    error.message(),
                    "retry without changing the request",
                    true);
            }
        }

        InstalledCreativeBundle ParseInstalledRecord(fs::path const& path)
        {
            auto bytes = ReadUtf8File(
                path,
                MaxBundleManifestBytes,
                "installed_record_validation",
                "record");
            auto record = ParseJsonObject(
                bytes,
                "installed_record_validation",
                "record",
                WideToUtf8(path.wstring()));
            if (bytes != CanonicalBytes(record))
            {
                Fail(
                    "xcp.creative.installed_record_invalid",
                    "creative installed record is not canonical",
                    "installed_record_validation",
                    "record",
                    WideToUtf8(path.wstring()),
                    "canonical immutable installed record",
                    "non-canonical bytes",
                    "repair or remove the corrupt private metadata before continuing");
            }
            RequireExactFields(
                record,
                {
                    L"schema_version",
                    L"install_id",
                    L"project_id",
                    L"project_version",
                    L"entry_module",
                    L"bundle_sha256",
                    L"bundle_bytes",
                    L"content_sha256",
                    L"files",
                    L"file_count",
                    L"total_bytes",
                    L"cas_references",
                    L"host_profile_id",
                    L"host_profile_canonical_sha256"
                },
                "installed_record_validation",
                "record");
            if (RequiredString(
                    record,
                    L"schema_version",
                    "installed_record_validation",
                    "schema_version") !=
                L"xcp-creative-installed-bundle-v1")
            {
                Fail(
                    "xcp.creative.installed_record_invalid",
                    "creative installed record schema is invalid",
                    "installed_record_validation",
                    "schema_version",
                    WideToUtf8(path.wstring()),
                    "xcp-creative-installed-bundle-v1",
                    "other",
                    "repair or remove the corrupt private metadata");
            }
            InstalledCreativeBundle result;
            result.recordPath = path;
            result.installId = RequiredString(
                record,
                L"install_id",
                "installed_record_validation",
                "install_id");
            result.projectId = RequiredString(
                record,
                L"project_id",
                "installed_record_validation",
                "project_id");
            result.projectVersion = RequiredString(
                record,
                L"project_version",
                "installed_record_validation",
                "project_version");
            result.entryModule = RequiredString(
                record,
                L"entry_module",
                "installed_record_validation",
                "entry_module");
            result.bundleSha256 = RequiredString(
                record,
                L"bundle_sha256",
                "installed_record_validation",
                "bundle_sha256");
            result.bundleBytes = RequiredUInt64(
                record,
                L"bundle_bytes",
                "installed_record_validation",
                "bundle_bytes");
            result.contentSha256 = RequiredString(
                record,
                L"content_sha256",
                "installed_record_validation",
                "content_sha256");
            result.hostProfileCanonicalSha256 =
                RequiredString(
                    record,
                    L"host_profile_canonical_sha256",
                    "installed_record_validation",
                    "host_profile_canonical_sha256");
            auto hostProfileId = RequiredString(
                record,
                L"host_profile_id",
                "installed_record_validation",
                "host_profile_id");
            result.totalBytes = RequiredUInt64(
                record,
                L"total_bytes",
                "installed_record_validation",
                "total_bytes");
            auto references = StringArray(
                RequiredArray(
                    record,
                    L"cas_references",
                    "installed_record_validation",
                    "cas_references"),
                "installed_record_validation",
                "cas_references",
                true);
            result.casReferences = std::move(references);
            auto files = RequiredArray(
                record,
                L"files",
                "installed_record_validation",
                "files");
            for (uint32_t index = 0; index < files.Size(); ++index)
            {
                auto value = files.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    Fail(
                        "xcp.creative.installed_record_invalid",
                        "creative installed record files entry is invalid",
                        "installed_record_validation",
                        "files",
                        WideToUtf8(path.wstring()),
                        "file object",
                        "non-object",
                        "repair or remove the corrupt private metadata");
                }
                auto object = value.GetObject();
                RequireExactFields(
                    object,
                    { L"role", L"id", L"kind", L"path", L"bytes", L"sha256" },
                    "installed_record_validation",
                    "files[]");
                CreativeFile file;
                file.role = RequiredString(
                    object,
                    L"role",
                    "installed_record_validation",
                    "files[].role");
                file.id = RequiredString(
                    object,
                    L"id",
                    "installed_record_validation",
                    "files[].id");
                file.kind = RequiredString(
                    object,
                    L"kind",
                    "installed_record_validation",
                    "files[].kind");
                file.path = RequiredString(
                    object,
                    L"path",
                    "installed_record_validation",
                    "files[].path");
                file.bytes = RequiredUInt64(
                    object,
                    L"bytes",
                    "installed_record_validation",
                    "files[].bytes");
                file.sha256 = RequiredString(
                    object,
                    L"sha256",
                    "installed_record_validation",
                    "files[].sha256");
                if ((file.role != L"module" &&
                        file.role != L"asset") ||
                    !IsSafeIdentifier(file.id) ||
                    !IsCanonicalRelativePath(file.path) ||
                    !IsLowerHexSha256(file.sha256) ||
                    (file.role == L"module" &&
                        ExpectedModuleSchemaVersion(
                            file.kind).empty()) ||
                    (file.role == L"asset" &&
                        (file.kind.size() < 3 ||
                            file.kind.size() > 128 ||
                            file.kind.find(L'/') ==
                                std::wstring::npos)) ||
                    file.bytes == 0 ||
                    file.bytes > MaxFileBytes)
                {
                    Fail(
                        "xcp.creative.installed_record_invalid",
                        "creative installed record contains an invalid file identity",
                        "installed_record_validation",
                        "files[]",
                        WideToUtf8(path.wstring()),
                        "valid bounded installed file identity",
                        WideToUtf8(file.path),
                        "repair or remove the corrupt private metadata");
                }
                result.files.push_back(std::move(file));
            }
            uint64_t computedTotalBytes = 0;
            std::map<std::wstring, uint64_t> computedReferences{
                { result.bundleSha256, result.bundleBytes }
            };
            for (auto const& file : result.files)
            {
                if (computedTotalBytes >
                    MaxProjectBytes - file.bytes)
                {
                    Fail(
                        "xcp.creative.installed_record_invalid",
                        "creative installed record byte total overflows its budget",
                        "installed_record_validation",
                        "files[].bytes",
                        WideToUtf8(path.wstring()),
                        "bounded project total",
                        "overflow",
                        "repair or remove the corrupt private metadata");
                }
                computedTotalBytes += file.bytes;
                auto found =
                    computedReferences.find(file.sha256);
                if (found != computedReferences.end() &&
                    found->second != file.bytes)
                {
                    Fail(
                        "xcp.creative.installed_record_invalid",
                        "creative installed record has conflicting CAS byte lengths",
                        "installed_record_validation",
                        "cas_references",
                        WideToUtf8(file.sha256),
                        std::to_string(found->second),
                        std::to_string(file.bytes),
                        "repair or remove the corrupt private metadata");
                }
                computedReferences[file.sha256] =
                    file.bytes;
            }
            std::vector<std::wstring> expectedReferences;
            for (auto const& [sha256, ignoredBytes] :
                 computedReferences)
            {
                (void)ignoredBytes;
                expectedReferences.push_back(sha256);
            }
            auto expectedInstallId = InstallIdFor(
                result.projectId,
                result.projectVersion,
                result.bundleSha256,
                result.hostProfileCanonicalSha256);
            if (result.installId != path.stem().wstring() ||
                result.installId != expectedInstallId ||
                !IsLowerHexSha256(result.installId) ||
                !IsSafeIdentifier(result.projectId) ||
                result.projectVersion.empty() ||
                result.projectVersion.size() > 64 ||
                !IsSafeIdentifier(result.entryModule) ||
                !IsLowerHexSha256(result.bundleSha256) ||
                !IsLowerHexSha256(result.contentSha256) ||
                result.contentSha256 !=
                    WorkerContentSha256(
                        ContentRecordsCanonical(result.files)) ||
                !IsLowerHexSha256(
                    result.hostProfileCanonicalSha256) ||
                hostProfileId !=
                    L"xcp.creative.host.development-v1" ||
                result.bundleBytes == 0 ||
                result.bundleBytes > MaxBundleManifestBytes ||
                result.totalBytes != computedTotalBytes ||
                result.casReferences != expectedReferences ||
                RequiredUInt64(
                    record,
                    L"file_count",
                    "installed_record_validation",
                    "file_count") != result.files.size())
            {
                Fail(
                    "xcp.creative.installed_record_invalid",
                    "creative installed record identity is inconsistent",
                    "installed_record_validation",
                    "record",
                    WideToUtf8(path.wstring()),
                    "self-consistent immutable record",
                    "identity mismatch",
                    "repair or remove the corrupt private metadata");
            }
            return result;
        }

        std::vector<InstalledCreativeBundle> LoadInstalledRecords(
            fs::path const& root)
        {
            std::vector<InstalledCreativeBundle> records;
            auto installRoot = CreativeInstallRoot(root);
            if (!fs::exists(installRoot))
            {
                return records;
            }
            for (auto const& entry : fs::directory_iterator(installRoot))
            {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != L".json")
                {
                    continue;
                }
                records.push_back(ParseInstalledRecord(entry.path()));
            }
            std::sort(
                records.begin(),
                records.end(),
                [](auto const& left, auto const& right)
                {
                    return left.installId < right.installId;
                });
            return records;
        }

        InstalledCreativeBundle const* FindInstall(
            std::vector<InstalledCreativeBundle> const& records,
            std::wstring const& installId)
        {
            auto found = std::find_if(
                records.begin(),
                records.end(),
                [&](auto const& record)
                {
                    return record.installId == installId;
                });
            return found == records.end() ? nullptr : &*found;
        }

        uint64_t ProjectedInstalledWorkspaceBytes(
            std::vector<InstalledCreativeBundle> const& records,
            CreativeBundle const& candidate)
        {
            std::map<std::wstring, uint64_t> references;
            auto add = [&](std::wstring const& sha256, uint64_t bytes)
            {
                auto found = references.find(sha256);
                if (found != references.end() &&
                    found->second != bytes)
                {
                    Fail(
                        "xcp.creative.installed_record_invalid",
                        "creative installed records disagree on CAS byte length",
                        "workspace_admission",
                        "cas_references",
                        WideToUtf8(sha256),
                        std::to_string(found->second),
                        std::to_string(bytes),
                        "repair the inconsistent private metadata");
                }
                references[sha256] = bytes;
            };
            for (auto const& record : records)
            {
                add(record.bundleSha256, record.bundleBytes);
                for (auto const& file : record.files)
                {
                    add(file.sha256, file.bytes);
                }
            }
            add(candidate.bundleSha256, candidate.bundleBytes);
            for (auto const& file : candidate.files)
            {
                add(file.sha256, file.bytes);
            }
            uint64_t total = 0;
            for (auto const& [ignoredSha, bytes] : references)
            {
                (void)ignoredSha;
                if (total > MaxInstalledWorkspaceBytes - bytes)
                {
                    Fail(
                        "xcp.creative.installed_workspace_exceeded",
                        "creative installed workspace exceeds the host budget",
                        "workspace_admission",
                        "max_installed_workspace_bytes",
                        {},
                        "at most 8589934592 unique referenced bytes",
                        "overflow",
                        "remove inactive installs or reduce the candidate bundle");
                }
                total += bytes;
            }
            return total;
        }

        CreativeActivationState LoadActivationState(
            fs::path const& root,
            std::wstring const& projectId)
        {
            CreativeActivationState state;
            state.projectId = projectId;
            auto recordRoot =
                CreativeActivationRecordRoot(root, projectId);
            if (!fs::exists(recordRoot))
            {
                return state;
            }
            std::vector<fs::path> paths;
            for (auto const& entry : fs::directory_iterator(recordRoot))
            {
                if (entry.is_regular_file() &&
                    entry.path().extension() == L".json")
                {
                    paths.push_back(entry.path());
                }
            }
            std::sort(paths.begin(), paths.end());
            if (paths.size() > MaxActivationRecords)
            {
                Fail(
                    "xcp.creative.activation_history_exceeded",
                    "creative activation history exceeds the bounded record count",
                    "activation_validation",
                    "records",
                    WideToUtf8(recordRoot.wstring()),
                    "at most 4096 records",
                    std::to_string(paths.size()),
                    "compact activation history in a later measured maintenance gate");
            }
            std::set<std::wstring> transitionIds;
            std::wstring previousSha;
            uint64_t expectedSequence = 1;
            for (auto const& path : paths)
            {
                auto bytes = ReadUtf8File(
                    path,
                    MaxBundleManifestBytes,
                    "activation_validation",
                    "record");
                auto record = ParseJsonObject(
                    bytes,
                    "activation_validation",
                    "record",
                    WideToUtf8(path.wstring()));
                if (bytes != CanonicalBytes(record))
                {
                    Fail(
                        "xcp.creative.activation_record_invalid",
                        "creative activation record is not canonical",
                        "activation_validation",
                        "record",
                        WideToUtf8(path.wstring()),
                        "canonical immutable activation record",
                        "non-canonical bytes",
                        "repair the activation journal before continuing");
                }
                RequireExactFields(
                    record,
                    {
                        L"schema_version",
                        L"sequence",
                        L"previous_record_sha256",
                        L"transition_id",
                        L"operation",
                        L"project_id",
                        L"requested_install_id",
                        L"expected_active_install_id",
                        L"expected_previous_install_id",
                        L"active_install_id",
                        L"previous_install_id"
                    },
                    "activation_validation",
                    "record");
                if (RequiredString(
                        record,
                        L"schema_version",
                        "activation_validation",
                        "schema_version") !=
                        L"xcp-creative-activation-record-v1" ||
                    RequiredString(
                        record,
                        L"project_id",
                        "activation_validation",
                        "project_id") != projectId ||
                    RequiredUInt64(
                        record,
                        L"sequence",
                        "activation_validation",
                        "sequence") != expectedSequence ||
                    RequiredString(
                        record,
                        L"previous_record_sha256",
                        "activation_validation",
                        "previous_record_sha256") != previousSha)
                {
                    Fail(
                        "xcp.creative.activation_chain_invalid",
                        "creative activation record chain is invalid",
                        "activation_validation",
                        "sequence/previous_record_sha256",
                        WideToUtf8(path.wstring()),
                        "contiguous hash-chained activation records",
                        "chain mismatch",
                        "repair the activation journal before continuing");
                }
                CreativeTransition transition;
                transition.sequence = expectedSequence;
                transition.transitionId = RequiredString(
                    record,
                    L"transition_id",
                    "activation_validation",
                    "transition_id");
                transition.operation = RequiredString(
                    record,
                    L"operation",
                    "activation_validation",
                    "operation");
                transition.projectId = projectId;
                transition.requestedInstallId = RequiredString(
                    record,
                    L"requested_install_id",
                    "activation_validation",
                    "requested_install_id");
                transition.expectedActiveInstallId = RequiredString(
                    record,
                    L"expected_active_install_id",
                    "activation_validation",
                    "expected_active_install_id");
                transition.expectedPreviousInstallId = RequiredString(
                    record,
                    L"expected_previous_install_id",
                    "activation_validation",
                    "expected_previous_install_id");
                transition.activeInstallId = RequiredString(
                    record,
                    L"active_install_id",
                    "activation_validation",
                    "active_install_id");
                transition.previousInstallId = RequiredString(
                    record,
                    L"previous_install_id",
                    "activation_validation",
                    "previous_install_id");
                transition.previousRecordSha256 = previousSha;
                transition.recordSha256 =
                    WorkerContentSha256(bytes);
                if (!IsSafeArtifactId(transition.transitionId) ||
                    !transitionIds.insert(
                        transition.transitionId).second ||
                    (transition.operation != L"activate" &&
                        transition.operation != L"rollback"))
                {
                    Fail(
                        "xcp.creative.activation_record_invalid",
                        "creative activation record transition identity is invalid",
                        "activation_validation",
                        "transition_id/operation",
                        WideToUtf8(path.wstring()),
                        "unique safe transition id and known operation",
                        WideToUtf8(
                            transition.transitionId + L":" +
                            transition.operation),
                        "repair the activation journal before continuing");
                }
                auto emptyOrInstallId =
                    [](std::wstring const& value)
                    {
                        return value.empty() ||
                            IsLowerHexSha256(value);
                    };
                auto stateIdentifiersValid =
                    emptyOrInstallId(
                        transition.requestedInstallId) &&
                    emptyOrInstallId(
                        transition.expectedActiveInstallId) &&
                    emptyOrInstallId(
                        transition.expectedPreviousInstallId) &&
                    emptyOrInstallId(
                        transition.activeInstallId) &&
                    emptyOrInstallId(
                        transition.previousInstallId);
                auto preconditionsMatch =
                    transition.expectedActiveInstallId ==
                        state.activeInstallId &&
                    transition.expectedPreviousInstallId ==
                        state.previousInstallId;
                auto transitionMatches =
                    transition.operation == L"activate"
                    ? transition.activeInstallId ==
                            transition.requestedInstallId &&
                        (transition.requestedInstallId ==
                                state.activeInstallId
                            ? transition.previousInstallId ==
                                state.previousInstallId
                            : transition.previousInstallId ==
                                state.activeInstallId)
                    : !state.previousInstallId.empty() &&
                        transition.requestedInstallId ==
                            state.previousInstallId &&
                        transition.activeInstallId ==
                            state.previousInstallId &&
                        transition.previousInstallId ==
                            state.activeInstallId;
                if (!stateIdentifiersValid ||
                    !preconditionsMatch ||
                    !transitionMatches)
                {
                    Fail(
                        "xcp.creative.activation_chain_invalid",
                        "creative activation transition does not follow the prior exact state",
                        "activation_validation",
                        "active_install_id/previous_install_id",
                        WideToUtf8(path.wstring()),
                        "preconditioned activate or rollback transition",
                        "state transition mismatch",
                        "repair the activation journal before continuing");
                }
                previousSha = transition.recordSha256;
                state.sequence = transition.sequence;
                state.activeInstallId =
                    transition.activeInstallId;
                state.previousInstallId =
                    transition.previousInstallId;
                state.lastRecordSha256 =
                    transition.recordSha256;
                state.transitions.push_back(
                    std::move(transition));
                ++expectedSequence;
            }
            return state;
        }

        void ValidateActivationReferences(
            CreativeActivationState const& state,
            std::vector<InstalledCreativeBundle> const& records)
        {
            for (auto const& [role, installId] :
                 std::array<std::pair<
                     std::wstring,
                     std::wstring>, 2>{
                     std::pair{
                         std::wstring(L"active_install_id"),
                         state.activeInstallId },
                     std::pair{
                         std::wstring(L"previous_install_id"),
                         state.previousInstallId } })
            {
                if (installId.empty())
                {
                    continue;
                }
                auto install = FindInstall(records, installId);
                if (install == nullptr ||
                    install->projectId != state.projectId)
                {
                    Fail(
                        "xcp.creative.activation_reference_missing",
                        "creative activation state references a missing or cross-project install",
                        "activation_validation",
                        WideToUtf8(role),
                        WideToUtf8(state.projectId),
                        "installed bundle for the same exact project",
                        WideToUtf8(installId),
                        "repair the activation journal or installed metadata before continuing");
                }
            }
        }

        std::wstring ActivationRecordName(uint64_t sequence)
        {
            std::wostringstream name;
            name << std::setfill(L'0')
                << std::setw(20)
                << sequence
                << L".json";
            return name.str();
        }

        std::wstring TransitionResponse(
            std::wstring const& command,
            std::wstring const& protocolVersion,
            CreativeTransition const& transition,
            bool replayed,
            bool changed)
        {
            return OkBase(command, protocolVersion) +
                L",\"schema_version\":\"xcp-creative-activation-result-v1\"" +
                L",\"project_id\":" +
                JsonString(transition.projectId) +
                L",\"operation\":" +
                JsonString(transition.operation) +
                L",\"transition_id\":" +
                JsonString(transition.transitionId) +
                L",\"sequence\":" +
                std::to_wstring(transition.sequence) +
                L",\"active_install_id\":" +
                JsonString(transition.activeInstallId) +
                L",\"previous_install_id\":" +
                JsonString(transition.previousInstallId) +
                L",\"record_sha256\":" +
                JsonString(transition.recordSha256) +
                L",\"replayed\":" + BoolJson(replayed) +
                L",\"changed\":" + BoolJson(changed) +
                L"}";
        }

        CreativeTransition const* FindTransition(
            CreativeActivationState const& state,
            std::wstring const& transitionId)
        {
            auto found = std::find_if(
                state.transitions.begin(),
                state.transitions.end(),
                [&](auto const& transition)
                {
                    return transition.transitionId ==
                        transitionId;
                });
            return found == state.transitions.end()
                ? nullptr
                : &*found;
        }

        CreativeTransition CommitTransition(
            fs::path const& root,
            CreativeActivationState const& state,
            std::wstring const& operation,
            std::wstring const& transitionId,
            std::wstring const& requestedInstallId,
            std::wstring const& expectedActiveInstallId,
            std::wstring const& expectedPreviousInstallId,
            std::wstring const& activeInstallId,
            std::wstring const& previousInstallId)
        {
            if (state.sequence >= MaxActivationRecords)
            {
                Fail(
                    "xcp.creative.activation_history_exceeded",
                    "creative activation history reached its bounded record count",
                    "activation_commit",
                    "records",
                    WideToUtf8(state.projectId),
                    "fewer than 4096 committed activation records",
                    std::to_string(state.sequence),
                    "compact activation history in a later measured maintenance gate");
            }
            JsonObject record;
            record.Insert(
                L"schema_version",
                JsonValue::CreateStringValue(
                    L"xcp-creative-activation-record-v1"));
            record.Insert(
                L"sequence",
                JsonValue::CreateNumberValue(
                    static_cast<double>(state.sequence + 1)));
            record.Insert(
                L"previous_record_sha256",
                JsonValue::CreateStringValue(
                    state.lastRecordSha256));
            record.Insert(
                L"transition_id",
                JsonValue::CreateStringValue(transitionId));
            record.Insert(
                L"operation",
                JsonValue::CreateStringValue(operation));
            record.Insert(
                L"project_id",
                JsonValue::CreateStringValue(state.projectId));
            record.Insert(
                L"requested_install_id",
                JsonValue::CreateStringValue(requestedInstallId));
            record.Insert(
                L"expected_active_install_id",
                JsonValue::CreateStringValue(
                    expectedActiveInstallId));
            record.Insert(
                L"expected_previous_install_id",
                JsonValue::CreateStringValue(
                    expectedPreviousInstallId));
            record.Insert(
                L"active_install_id",
                JsonValue::CreateStringValue(activeInstallId));
            record.Insert(
                L"previous_install_id",
                JsonValue::CreateStringValue(previousInstallId));
            auto bytes = CanonicalBytes(record);
            auto path =
                CreativeActivationRecordRoot(
                    root,
                    state.projectId) /
                ActivationRecordName(state.sequence + 1);
            WriteImmutableRecord(
                path,
                bytes,
                "xcp.creative.activation_record_conflict");
            CreativeTransition result;
            result.sequence = state.sequence + 1;
            result.transitionId = transitionId;
            result.operation = operation;
            result.projectId = state.projectId;
            result.requestedInstallId = requestedInstallId;
            result.expectedActiveInstallId =
                expectedActiveInstallId;
            result.expectedPreviousInstallId =
                expectedPreviousInstallId;
            result.activeInstallId = activeInstallId;
            result.previousInstallId = previousInstallId;
            result.previousRecordSha256 =
                state.lastRecordSha256;
            result.recordSha256 =
                WorkerContentSha256(bytes);
            return result;
        }

        std::wstring ShaArrayJson(
            std::vector<std::wstring> const& values)
        {
            std::wstring result = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    result += L",";
                }
                result += JsonString(values[index]);
            }
            result += L"]";
            return result;
        }
    }

    WorkerCreativeHostError::WorkerCreativeHostError(
        std::string codeValue,
        std::string messageValue,
        WorkerCreativeHostErrorDetails detailsValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue)),
        details(std::move(detailsValue))
    {
    }

    char const* WorkerCreativeHostError::what() const noexcept
    {
        return message.c_str();
    }

    WorkerCreativeActiveInstall WorkerLoadActiveCreativeInstall(
        fs::path const& root,
        std::wstring const& projectId,
        std::wstring const& installId)
    {
        if (!IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(installId))
        {
            Fail(
                "xcp.creative.foreground_binding_invalid",
                "creative foreground binding identity is invalid",
                "foreground_binding",
                "project_id/install_id",
                {},
                "safe project id and exact active install SHA-256",
                WideToUtf8(projectId + L":" + installId),
                "use exact identities returned by list_creative_installs");
        }

        std::lock_guard<std::mutex> lock(CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        auto state = LoadActivationState(root, projectId);
        ValidateActivationReferences(state, records);
        auto install = FindInstall(records, installId);
        if (install == nullptr ||
            install->projectId != projectId ||
            state.activeInstallId != installId)
        {
            Fail(
                "xcp.creative.foreground_install_not_active",
                "creative foreground can load only the exact active install",
                "foreground_binding",
                "install_id",
                WideToUtf8(installId),
                WideToUtf8(state.activeInstallId),
                install == nullptr
                    ? "missing install"
                    : "install is not the active project version",
                "activate the exact installed bundle, then launch it");
        }
        if (install->hostProfileCanonicalSha256 !=
            WorkerCreativeHostProfileCanonicalSha256())
        {
            Fail(
                "xcp.creative.install_profile_mismatch",
                "creative foreground install was validated against a different host profile",
                "foreground_binding",
                "install_id",
                WideToUtf8(installId),
                WideToUtf8(WorkerCreativeHostProfileCanonicalSha256()),
                WideToUtf8(install->hostProfileCanonicalSha256),
                "prepare and commit the bundle again against the current host profile");
        }
        ValidateInstalledContentBlobs(*install, root);

        WorkerCreativeActiveInstall result;
        result.installId = install->installId;
        result.projectId = install->projectId;
        result.projectVersion = install->projectVersion;
        result.entryModule = install->entryModule;
        result.bundleSha256 = install->bundleSha256;
        result.contentSha256 = install->contentSha256;
        result.hostProfileCanonicalSha256 =
            install->hostProfileCanonicalSha256;
        result.activationRecordSha256 = state.lastRecordSha256;
        result.activationSequence = state.sequence;
        result.totalBytes = install->totalBytes;
        result.files.reserve(install->files.size());
        for (auto const& file : install->files)
        {
            WorkerCreativeInstalledFile projected;
            projected.role = file.role;
            projected.id = file.id;
            projected.kind = file.kind;
            projected.runtimePath = file.path;
            projected.sha256 = file.sha256;
            projected.bytes = file.bytes;
            projected.casPath = ArtifactBlobPath(root, file.sha256);
            result.files.push_back(std::move(projected));
        }
        return result;
    }

    std::wstring WorkerPrepareCreativeInstall(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"prepare_creative_install",
            L"xcp-creative-prepare-install-request-v1",
            {
                L"bundle_artifact_id",
                L"expected_bundle_sha256",
                L"expected_host_profile_sha256"
            });
        ValidateExpectedProfile(request);
        auto bundle = LoadAndValidateBundle(request, root);
        auto missing = ValidateContentBlobs(
            bundle,
            root,
            false);
        return OkBase(
                L"prepare_creative_install",
                protocolVersion) +
            L",\"schema_version\":\"xcp-creative-prepare-install-result-v1\"" +
            L",\"project_id\":" +
            JsonString(bundle.projectId) +
            L",\"project_version\":" +
            JsonString(bundle.projectVersion) +
            L",\"bundle_sha256\":" +
            JsonString(bundle.bundleSha256) +
            L",\"content_sha256\":" +
            JsonString(bundle.contentSha256) +
            L",\"file_count\":" +
            std::to_wstring(bundle.files.size()) +
            L",\"missing_count\":" +
            std::to_wstring(missing.size()) +
            L",\"missing_sha256\":" +
            ShaArrayJson(missing) +
            L",\"ready_to_commit\":" +
            BoolJson(missing.empty()) +
            L",\"staging_surface\":\"existing_worker_artifacts\"" +
            L"}";
    }

    std::wstring WorkerCommitCreativeInstall(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"commit_creative_install",
            L"xcp-creative-commit-install-request-v1",
            {
                L"bundle_artifact_id",
                L"expected_bundle_sha256",
                L"expected_host_profile_sha256"
            });
        ValidateExpectedProfile(request);
        auto bundle = LoadAndValidateBundle(request, root);
        ValidateContentBlobs(bundle, root, true);

        std::lock_guard<std::mutex> lock(
            CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        auto installId = InstallIdFor(bundle);
        for (auto const& record : records)
        {
            if (record.projectId == bundle.projectId &&
                record.projectVersion == bundle.projectVersion &&
                record.bundleSha256 != bundle.bundleSha256)
            {
                Fail(
                    "xcp.creative.project_version_conflict",
                    "creative project version is already installed with different exact bytes",
                    "install_commit",
                    "project.version",
                    {},
                    WideToUtf8(record.bundleSha256),
                    WideToUtf8(bundle.bundleSha256),
                    "increment the project version or reinstall the exact existing bundle");
            }
        }
        auto projectedBytes =
            ProjectedInstalledWorkspaceBytes(
                records,
                bundle);
        auto record = InstalledRecordJson(
            bundle,
            installId);
        auto bytes = CanonicalBytes(record);
        auto path = CreativeInstallPath(root, installId);
        auto replayed = fs::exists(path);
        WriteImmutableRecord(
            path,
            bytes,
            "xcp.creative.install_record_conflict");
        return OkBase(
                L"commit_creative_install",
                protocolVersion) +
            L",\"schema_version\":\"xcp-creative-commit-install-result-v1\"" +
            L",\"install_id\":" +
            JsonString(installId) +
            L",\"project_id\":" +
            JsonString(bundle.projectId) +
            L",\"project_version\":" +
            JsonString(bundle.projectVersion) +
            L",\"bundle_sha256\":" +
            JsonString(bundle.bundleSha256) +
            L",\"content_sha256\":" +
            JsonString(bundle.contentSha256) +
            L",\"file_count\":" +
            std::to_wstring(bundle.files.size()) +
            L",\"installed_workspace_unique_bytes\":" +
            std::to_wstring(projectedBytes) +
            L",\"replayed\":" + BoolJson(replayed) +
            L",\"validated\":true" +
            L",\"active\":false" +
            L"}";
    }

    std::wstring WorkerListCreativeInstalls(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"list_creative_installs",
            L"xcp-creative-list-installs-request-v1",
            {});
        std::lock_guard<std::mutex> lock(
            CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        std::map<std::wstring, CreativeActivationState> activations;
        for (auto const& record : records)
        {
            if (activations.find(record.projectId) ==
                activations.end())
            {
                activations.emplace(
                    record.projectId,
                    LoadActivationState(
                        root,
                        record.projectId));
                ValidateActivationReferences(
                    activations.at(record.projectId),
                    records);
            }
        }
        std::wstring installsJson = L"[";
        for (size_t index = 0; index < records.size(); ++index)
        {
            if (index != 0)
            {
                installsJson += L",";
            }
            auto const& record = records[index];
            auto const& activation =
                activations.at(record.projectId);
            installsJson +=
                L"{\"install_id\":" +
                JsonString(record.installId) +
                L",\"project_id\":" +
                JsonString(record.projectId) +
                L",\"project_version\":" +
                JsonString(record.projectVersion) +
                L",\"bundle_sha256\":" +
                JsonString(record.bundleSha256) +
                L",\"content_sha256\":" +
                JsonString(record.contentSha256) +
                L",\"file_count\":" +
                std::to_wstring(record.files.size()) +
                L",\"total_bytes\":" +
                std::to_wstring(record.totalBytes) +
                L",\"active\":" +
                BoolJson(
                    activation.activeInstallId ==
                    record.installId) +
                L",\"rollback_target\":" +
                BoolJson(
                    activation.previousInstallId ==
                    record.installId) +
                L"}";
        }
        installsJson += L"]";
        return OkBase(
                L"list_creative_installs",
                protocolVersion) +
            L",\"schema_version\":\"xcp-creative-list-installs-result-v1\"" +
            L",\"install_count\":" +
            std::to_wstring(records.size()) +
            L",\"installs\":" + installsJson +
            L"}";
    }

    std::wstring WorkerActivateCreativeInstall(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"activate_creative_install",
            L"xcp-creative-activate-install-request-v1",
            {
                L"project_id",
                L"install_id",
                L"transition_id",
                L"expected_active_install_id",
                L"expected_previous_install_id"
            });
        auto projectId = RequiredRequestString(
            request,
            L"project_id",
            "activation_request");
        auto installId = RequiredRequestString(
            request,
            L"install_id",
            "activation_request");
        auto transitionId = RequiredRequestString(
            request,
            L"transition_id",
            "activation_request");
        auto expectedActive = RequiredRequestString(
            request,
            L"expected_active_install_id",
            "activation_request");
        auto expectedPrevious = RequiredRequestString(
            request,
            L"expected_previous_install_id",
            "activation_request");
        if (!IsSafeIdentifier(projectId) ||
            !IsLowerHexSha256(installId) ||
            !IsSafeArtifactId(transitionId) ||
            (!expectedActive.empty() &&
                !IsLowerHexSha256(expectedActive)) ||
            (!expectedPrevious.empty() &&
                !IsLowerHexSha256(expectedPrevious)))
        {
            Fail(
                "xcp.creative.activation_request_invalid",
                "creative activation request identity is invalid",
                "activation_request",
                "project_id/install_id/transition_id",
                {},
                "safe project id, exact install SHA-256 and safe transition id",
                "invalid identity",
                "correct the request using list_creative_installs");
        }

        std::lock_guard<std::mutex> lock(
            CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        auto install = FindInstall(records, installId);
        if (install == nullptr ||
            install->projectId != projectId)
        {
            Fail(
                "xcp.creative.install_not_found",
                "creative install is not present for the requested project",
                "activation_request",
                "install_id",
                WideToUtf8(installId),
                WideToUtf8(projectId),
                "missing or belongs to another project",
                "refresh list_creative_installs and select an exact installed bundle");
        }
        if (install->hostProfileCanonicalSha256 !=
            WorkerCreativeHostProfileCanonicalSha256())
        {
            Fail(
                "xcp.creative.install_profile_mismatch",
                "creative install was validated against a different host profile",
                "activation_request",
                "install_id",
                WideToUtf8(installId),
                WideToUtf8(
                    WorkerCreativeHostProfileCanonicalSha256()),
                WideToUtf8(
                    install->hostProfileCanonicalSha256),
                "prepare and commit the bundle again against the current host profile");
        }
        ValidateInstalledContentBlobs(*install, root);
        auto state = LoadActivationState(root, projectId);
        ValidateActivationReferences(state, records);
        if (auto replay = FindTransition(
                state,
                transitionId))
        {
            if (replay->operation != L"activate" ||
                replay->requestedInstallId != installId ||
                replay->expectedActiveInstallId !=
                    expectedActive ||
                replay->expectedPreviousInstallId !=
                    expectedPrevious)
            {
                Fail(
                    "xcp.creative.transition_conflict",
                    "creative transition id was already used with different inputs",
                    "activation_request",
                    "transition_id",
                    WideToUtf8(transitionId),
                    "same exact request binding",
                    "conflicting replay",
                    "use a new transition id for a different transition");
            }
            return TransitionResponse(
                L"activate_creative_install",
                protocolVersion,
                *replay,
                true,
                false);
        }
        if (state.activeInstallId != expectedActive ||
            state.previousInstallId != expectedPrevious)
        {
            Fail(
                "xcp.creative.activation_state_conflict",
                "creative activation precondition does not match current state",
                "activation_request",
                "expected_active_install_id/expected_previous_install_id",
                {},
                WideToUtf8(
                    state.activeInstallId + L"/" +
                    state.previousInstallId),
                WideToUtf8(
                    expectedActive + L"/" +
                    expectedPrevious),
                "refresh list_creative_installs and retry with the exact current state");
        }
        if (state.activeInstallId == installId)
        {
            auto unchanged = CommitTransition(
                root,
                state,
                L"activate",
                transitionId,
                installId,
                expectedActive,
                expectedPrevious,
                state.activeInstallId,
                state.previousInstallId);
            return TransitionResponse(
                L"activate_creative_install",
                protocolVersion,
                unchanged,
                false,
                false);
        }
        auto transition = CommitTransition(
            root,
            state,
            L"activate",
            transitionId,
            installId,
            expectedActive,
            expectedPrevious,
            installId,
            state.activeInstallId);
        return TransitionResponse(
            L"activate_creative_install",
            protocolVersion,
            transition,
            false,
            true);
    }

    std::wstring WorkerRollbackCreativeActivation(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"rollback_creative_activation",
            L"xcp-creative-rollback-activation-request-v1",
            {
                L"project_id",
                L"transition_id",
                L"expected_active_install_id",
                L"expected_previous_install_id"
            });
        auto projectId = RequiredRequestString(
            request,
            L"project_id",
            "rollback_request");
        auto transitionId = RequiredRequestString(
            request,
            L"transition_id",
            "rollback_request");
        auto expectedActive = RequiredRequestString(
            request,
            L"expected_active_install_id",
            "rollback_request");
        auto expectedPrevious = RequiredRequestString(
            request,
            L"expected_previous_install_id",
            "rollback_request");
        if (!IsSafeIdentifier(projectId) ||
            !IsSafeArtifactId(transitionId) ||
            !IsLowerHexSha256(expectedActive) ||
            !IsLowerHexSha256(expectedPrevious))
        {
            Fail(
                "xcp.creative.rollback_request_invalid",
                "creative rollback request identity is invalid",
                "rollback_request",
                "project_id/transition_id",
                {},
                "safe project and transition ids",
                "invalid identity",
                "correct the request using list_creative_installs");
        }

        std::lock_guard<std::mutex> lock(
            CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        auto state = LoadActivationState(root, projectId);
        ValidateActivationReferences(state, records);
        if (auto replay = FindTransition(
                state,
                transitionId))
        {
            if (replay->operation != L"rollback" ||
                replay->expectedActiveInstallId !=
                    expectedActive ||
                replay->expectedPreviousInstallId !=
                    expectedPrevious)
            {
                Fail(
                    "xcp.creative.transition_conflict",
                    "creative transition id was already used with different inputs",
                    "rollback_request",
                    "transition_id",
                    WideToUtf8(transitionId),
                    "same exact rollback binding",
                    "conflicting replay",
                    "use a new transition id for a different transition");
            }
            return TransitionResponse(
                L"rollback_creative_activation",
                protocolVersion,
                *replay,
                true,
                false);
        }
        if (state.activeInstallId != expectedActive ||
            state.previousInstallId != expectedPrevious ||
            state.previousInstallId.empty())
        {
            Fail(
                "xcp.creative.rollback_state_conflict",
                "creative rollback precondition does not match an available rollback target",
                "rollback_request",
                "expected_active_install_id/expected_previous_install_id",
                {},
                WideToUtf8(
                    state.activeInstallId + L"/" +
                    state.previousInstallId),
                WideToUtf8(
                    expectedActive + L"/" +
                    expectedPrevious),
                "refresh list_creative_installs and retry only when an exact prior activation exists");
        }
        if (FindInstall(records, state.activeInstallId) ==
                nullptr ||
            FindInstall(records, state.previousInstallId) ==
                nullptr)
        {
            Fail(
                "xcp.creative.rollback_target_missing",
                "creative active or rollback install metadata is missing",
                "rollback_request",
                "install_id",
                {},
                "both exact installed bundles present",
                "missing install record",
                "repair the installed metadata before rollback");
        }
        auto activeInstall =
            FindInstall(records, state.activeInstallId);
        auto previousInstall =
            FindInstall(records, state.previousInstallId);
        if (activeInstall->hostProfileCanonicalSha256 !=
                WorkerCreativeHostProfileCanonicalSha256() ||
            previousInstall->hostProfileCanonicalSha256 !=
                WorkerCreativeHostProfileCanonicalSha256())
        {
            Fail(
                "xcp.creative.install_profile_mismatch",
                "creative rollback pair was validated against a different host profile",
                "rollback_request",
                "install_id",
                {},
                WideToUtf8(
                    WorkerCreativeHostProfileCanonicalSha256()),
                "stale install profile",
                "recommit both bundles against the current host profile before activation");
        }
        ValidateInstalledContentBlobs(
            *previousInstall,
            root);
        auto transition = CommitTransition(
            root,
            state,
            L"rollback",
            transitionId,
            state.previousInstallId,
            expectedActive,
            expectedPrevious,
            state.previousInstallId,
            state.activeInstallId);
        return TransitionResponse(
            L"rollback_creative_activation",
            protocolVersion,
            transition,
            false,
            true);
    }

    std::wstring WorkerRemoveCreativeInstall(
        JsonObject const& wireRequest,
        std::wstring const& protocolVersion,
        fs::path const& root)
    {
        auto request = StripTransportFields(wireRequest);
        ValidateLifecycleRequestEnvelope(
            request,
            L"remove_creative_install",
            L"xcp-creative-remove-install-request-v1",
            { L"install_id", L"delete_unreferenced_blobs" });
        auto installId = RequiredRequestString(
            request,
            L"install_id",
            "remove_request");
        if (!IsLowerHexSha256(installId))
        {
            Fail(
                "xcp.creative.remove_request_invalid",
                "creative remove request install id is invalid",
                "remove_request",
                "install_id",
                {},
                "exact installed bundle SHA-256 identity",
                WideToUtf8(installId),
                "use an install id returned by list_creative_installs");
        }
        if (!request.HasKey(L"delete_unreferenced_blobs") ||
            request.GetNamedValue(
                L"delete_unreferenced_blobs").ValueType() !=
                JsonValueType::Boolean)
        {
            Fail(
                "xcp.creative.remove_request_invalid",
                "creative remove request must declare blob cleanup policy",
                "remove_request",
                "delete_unreferenced_blobs",
                {},
                "boolean",
                "missing or non-boolean",
                "send an explicit true or false cleanup policy");
        }
        auto deleteBlobs = request.GetNamedBoolean(
            L"delete_unreferenced_blobs");

        std::lock_guard<std::mutex> lock(
            CreativeLifecycleMutex);
        auto records = LoadInstalledRecords(root);
        auto install = FindInstall(records, installId);
        if (install == nullptr)
        {
            return OkBase(
                    L"remove_creative_install",
                    protocolVersion) +
                L",\"schema_version\":\"xcp-creative-remove-install-result-v1\"" +
                L",\"install_id\":" +
                JsonString(installId) +
                L",\"existed\":false,\"removed\":false" +
                L",\"candidate_blob_count\":0" +
                L",\"removed_blob_count\":0" +
                L",\"removed_blob_bytes\":0" +
                L"}";
        }
        std::set<std::wstring> projectIds;
        for (auto const& record : records)
        {
            projectIds.insert(record.projectId);
        }
        std::wstring referencedAs;
        for (auto const& projectId : projectIds)
        {
            auto activation = LoadActivationState(
                root,
                projectId);
            ValidateActivationReferences(
                activation,
                records);
            if (activation.activeInstallId == installId)
            {
                referencedAs = L"active";
                break;
            }
            if (activation.previousInstallId == installId)
            {
                referencedAs = L"rollback_target";
                break;
            }
        }
        if (!referencedAs.empty())
        {
            Fail(
                "xcp.creative.install_referenced",
                "creative install is active or retained as the rollback target",
                "remove_request",
                "install_id",
                WideToUtf8(installId),
                "inactive install not referenced by activation state",
                WideToUtf8(referencedAs),
                "activate another exact bundle and move the rollback window before removal");
        }
        auto references = install->casReferences;
        auto recordPath = install->recordPath;
        if (!fs::remove(recordPath))
        {
            Fail(
                "xcp.creative.install_remove_failed",
                "creative install metadata could not be removed",
                "remove_commit",
                "install_id",
                WideToUtf8(installId),
                "removed immutable install record",
                "remove failed",
                "retry after checking app-private storage health",
                true);
        }
        WorkerContentAddressedBlobCleanup cleanup;
        if (deleteBlobs)
        {
            cleanup =
                WorkerRemoveUnreferencedContentAddressedBlobs(
                    root,
                    references);
        }
        return OkBase(
                L"remove_creative_install",
                protocolVersion) +
            L",\"schema_version\":\"xcp-creative-remove-install-result-v1\"" +
            L",\"install_id\":" +
            JsonString(installId) +
            L",\"existed\":true,\"removed\":true" +
            L",\"delete_unreferenced_blobs\":" +
            BoolJson(deleteBlobs) +
            L",\"candidate_blob_count\":" +
            std::to_wstring(cleanup.candidateCount) +
            L",\"removed_blob_count\":" +
            std::to_wstring(cleanup.removedBlobCount) +
            L",\"removed_blob_bytes\":" +
            std::to_wstring(cleanup.removedBytes) +
            L",\"protected_by_unreadable_metadata\":" +
            BoolJson(cleanup.protectedByUnreadableMetadata) +
            L"}";
    }
}
