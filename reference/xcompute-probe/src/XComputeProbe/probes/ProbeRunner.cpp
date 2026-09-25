#include "pch.h"
#include "ProbeRunner.h"
#include "../native/StaticNativeModule.h"
#include "../runtime/WorkerPrototype.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Security::ExchangeActiveSyncProvisioning;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::System::Profile;
using namespace Windows::Web::Http;

namespace XComputeProbe
{
    static void Finish(ProbeResult& result, std::chrono::steady_clock::time_point started)
    {
        result.endedUtc = UtcNow();
        result.durationMs = ElapsedMs(started);
    }

    static std::wstring VersionToString(PackageVersion const& version)
    {
        std::wostringstream out;
        out << version.Major << L"." << version.Minor << L"." << version.Build << L"." << version.Revision;
        return out.str();
    }

    static std::wstring ErrorCodeToString(DWORD error)
    {
        std::wostringstream out;
        out << L"Win32 error " << error;
        return out.str();
    }

    static bool StartsWith(std::wstring const& value, std::wstring const& prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    static std::wstring ReadLanResultEndpoint()
    {
        try
        {
            auto package = Package::Current();
            std::wstring path = package.InstalledLocation().Path().c_str();
            path += L"\\ProbeConfig.json";

            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return L"";
            }

            std::string utf8((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (utf8.empty())
            {
                return L"";
            }

            auto config = JsonObject::Parse(winrt::to_hstring(utf8));
            if (!config.HasKey(L"lan_result_endpoint"))
            {
                return L"";
            }

            return std::wstring(config.GetNamedString(L"lan_result_endpoint", L"").c_str());
        }
        catch (...)
        {
            return L"";
        }
    }

    static bool IsSupportedLanEndpoint(std::wstring const& endpoint)
    {
        return StartsWith(endpoint, L"http://") || StartsWith(endpoint, L"https://");
    }

    std::vector<ProbeResult> ProbeRunner::RunSafeSuite(std::vector<ProbeResult> const& graphicsProbes)
    {
        auto flavor = std::wstring(XCOMPUTE_MANIFEST_FLAVOR);
        bool minimalGameBucket = flavor == L"gamebucket-minimal";
        bool d3d11ComputeGameBucket = flavor == L"gamebucket-d3d11compute";
        bool shaderRuntimeGameBucket = flavor == L"gamebucket-shaderruntime";
        std::vector<ProbeResult> probes;
        probes.push_back(RunPlatformProbe());
        probes.push_back(RunMemoryProbe());
        probes.push_back(RunCpuProbe());
        for (auto const& graphicsProbe : graphicsProbes)
        {
            probes.push_back(graphicsProbe);
        }
        probes.push_back(RunStorageProbe());
        probes.push_back(RunNetworkProbe());
        probes.push_back(RunInputLifecycleProbe());
        probes.push_back(RunGameBucketProbe());
        if (!minimalGameBucket && !d3d11ComputeGameBucket && !shaderRuntimeGameBucket)
        {
            probes.push_back(RunPackagedNativeModulesProbe());
        }
        if (flavor == L"worker-prototype")
        {
            probes.push_back(RunWorkerPrototypeProbe());
        }
        if (flavor == L"codegen-candidate")
        {
            probes.push_back(RunDynamicCodeProbe());
        }
        if (flavor == L"expanded-candidate")
        {
            probes.push_back(RunExpandedResourcesProbe());
        }
        return probes;
    }

    ProbeResult ProbeRunner::RunPlatformProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"platform.identity";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"AnalyticsInfo.VersionInfo", L"Package.Current", L"EasClientDeviceInformation" };
        try
        {
            auto versionInfo = AnalyticsInfo::VersionInfo();
            result.AddString(L"device_family", versionInfo.DeviceFamily().c_str());
            result.AddString(L"device_family_version", versionInfo.DeviceFamilyVersion().c_str());
            auto package = Package::Current();
            result.AddString(L"package_name", package.Id().Name().c_str());
            result.AddString(L"package_version", VersionToString(package.Id().Version()));
#if defined(_M_X64)
            result.AddString(L"process_architecture", L"x64");
#else
            result.AddString(L"process_architecture", L"non-x64");
#endif
            EasClientDeviceInformation deviceInfo;
            result.AddString(L"system_product_name", deviceInfo.SystemProductName().c_str());
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunMemoryProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"memory.budget";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"MemoryManager.AppMemoryUsageLimit", L"MemoryManager.AppMemoryUsage", L"MemoryManager.AppMemoryUsageLevel" };
        try
        {
            result.AddNumber(L"app_memory_usage_limit_bytes", MemoryManager::AppMemoryUsageLimit());
            result.AddNumber(L"app_memory_usage_bytes", MemoryManager::AppMemoryUsage());
            result.AddNumber(L"app_memory_usage_level", static_cast<uint64_t>(MemoryManager::AppMemoryUsageLevel()));
            result.notes.push_back(L"Safe allocation stress test is disabled by default and must be explicitly confirmed on screen.");
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunCpuProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"cpu.visibility";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"std::thread::hardware_concurrency", L"bounded integer workload" };

        unsigned int visibleThreads = (std::max)(1u, std::thread::hardware_concurrency());
        result.AddNumber(L"logical_processor_count_visible", visibleThreads);

        std::atomic<bool> stop{ false };
        std::atomic<uint64_t> iterations{ 0 };
        auto worker = [&]()
        {
            uint64_t local = 0;
            uint64_t value = 0x9e3779b97f4a7c15ull;
            while (!stop.load(std::memory_order_relaxed))
            {
                value ^= value << 13;
                value ^= value >> 7;
                value ^= value << 17;
                ++local;
            }
            iterations.fetch_add(local, std::memory_order_relaxed);
        };

        unsigned int threadCount = (std::min)(visibleThreads, 4u);
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < threadCount; ++i) threads.emplace_back(worker);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : threads) thread.join();

        result.AddNumber(L"workload_duration_ms", 250);
        result.AddNumber(L"workload_thread_count", threadCount);
        result.AddNumber(L"integer_iterations", iterations.load());
        result.notes.push_back(L"Throughput is a bounded probe result, not a universal console benchmark.");
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunStorageProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"storage.local";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"ApplicationData.Current.LocalFolder", L"app-private file write/read" };
        try
        {
            auto folder = ApplicationData::Current().LocalFolder();
            std::wstring path = folder.Path().c_str();
            result.AddString(L"location_class", L"ApplicationData.LocalFolder");
            result.AddString(L"local_folder_path_observed", path);
            auto filePath = path + L"\\xcompute-storage-probe.bin";
            std::string payload(256 * 1024, 'x');
            auto writeStart = std::chrono::steady_clock::now();
            {
                std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
            auto writeMs = ElapsedMs(writeStart);

            std::ifstream in(filePath, std::ios::binary);
            std::string readBack((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            result.AddNumber(L"sequential_bytes", payload.size());
            result.AddNumber(L"write_duration_ms", writeMs);
            result.AddBool(L"readback_matches", readBack == payload);
            if (readBack != payload) result.status = L"fail";
        }
        catch (std::exception const& ex)
        {
            result.status = L"error";
            std::wstring message(ex.what(), ex.what() + strlen(ex.what()));
            result.errors.push_back(message);
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunNetworkProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"network.lan_sender";
        result.status = L"skipped";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"Package.InstalledLocation", L"ProbeConfig.json", L"HttpClient.PostAsync" };

        auto endpoint = ReadLanResultEndpoint();
        result.AddBool(L"configured", !endpoint.empty());
        if (endpoint.empty())
        {
            result.AddString(L"default_state", L"disabled");
            result.notes.push_back(L"LAN sender requires explicit receiver URL in ProbeConfig.json; no discovery broadcast is performed by default.");
            Finish(result, started);
            return result;
        }

        result.AddString(L"endpoint", endpoint);
        result.AddBool(L"send_attempted_after_report_build", IsSupportedLanEndpoint(endpoint));
        if (!IsSupportedLanEndpoint(endpoint))
        {
            result.status = L"blocked";
            result.errors.push_back(L"ProbeConfig.json lan_result_endpoint must start with http:// or https://.");
            Finish(result, started);
            return result;
        }

        result.status = L"pass";
        result.notes.push_back(L"Configured LAN send is attempted after the report JSON is built; HTTP outcome is observed in the PC receiver log.");
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunInputLifecycleProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"input.lifecycle";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"Gamepad.Gamepads", L"CoreApplication lifecycle events registered by App" };
        result.AddNumber(L"gamepads_detected_at_probe", Gamepad::Gamepads().Size());
        result.AddBool(L"keyboard_event_handler_registered", true);
        result.AddBool(L"suspend_resume_handlers_registered", true);
        result.notes.push_back(L"Precise button/key event timing is collected during interactive runtime; startup probe only records handler availability.");
        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunGameBucketProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"platform.gamebucket_candidate";
        result.status = L"skipped";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = { L"Package.Current", L"MemoryManager.AppMemoryUsageLimit" };
        result.AddString(L"compiled_manifest_flavor", XCOMPUTE_MANIFEST_FLAVOR);

        auto flavor = std::wstring(XCOMPUTE_MANIFEST_FLAVOR);
        if (flavor != L"gamebucket-candidate" &&
            flavor != L"gamebucket-minimal" &&
            flavor != L"gamebucket-native" &&
            flavor != L"gamebucket-d3d11compute" &&
            flavor != L"gamebucket-shaderruntime")
        {
            result.notes.push_back(L"Game bucket probe only runs as the gamebucket manifest flavors.");
            Finish(result, started);
            return result;
        }

        try
        {
            auto package = Package::Current();
            result.status = L"pass";
            result.AddString(L"package_name", package.Id().Name().c_str());
            result.AddString(L"candidate_target_device_family", L"Windows.Xbox");
            result.AddBool(L"minimal_runtime_profile", flavor == L"gamebucket-minimal");
            result.AddBool(L"native_runtime_profile", flavor == L"gamebucket-native");
            result.AddBool(L"d3d11_compute_runtime_profile", flavor == L"gamebucket-d3d11compute");
            result.AddBool(L"shader_runtime_profile", flavor == L"gamebucket-shaderruntime");
            result.AddBool(L"restricted_capabilities_intentionally_absent", true);
            result.AddNumber(L"app_memory_usage_limit_bytes", MemoryManager::AppMemoryUsageLimit());
            result.AddNumber(L"app_memory_usage_bytes", MemoryManager::AppMemoryUsage());
            result.AddNumber(L"app_memory_usage_level", static_cast<uint64_t>(MemoryManager::AppMemoryUsageLevel()));
            result.notes.push_back(L"No public UWP API is used here to claim Game Mode resources. Compare this result with baseline memory, CPU, and graphics probes in the same JSON.");
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }

        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunPackagedNativeModulesProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"native.packaged_modules";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = {
            L"static-linked C++ module",
            L"LoadPackagedLibrary",
            L"GetProcAddress"
        };

        std::array<uint8_t, 32> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<uint8_t>(i * 7 + 3);
        }
        std::array<uint32_t, 256> vectorInput{};
        for (size_t i = 0; i < vectorInput.size(); ++i)
        {
            vectorInput[i] = static_cast<uint32_t>(i + 1);
        }

        auto staticStarted = std::chrono::steady_clock::now();
        uint32_t staticAdd = NativeStatic::Add(17, 25);
        uint32_t staticMul = NativeStatic::Mul(13, 11);
        uint32_t staticHash = NativeStatic::Hash32(bytes.data(), bytes.size());
        uint64_t staticVector = NativeStatic::VectorLoop(vectorInput.data(), vectorInput.size(), 256);
        result.AddNumber(L"static_duration_ms", ElapsedMs(staticStarted));
        result.AddNumber(L"static_add_17_25", staticAdd);
        result.AddNumber(L"static_mul_13_11", staticMul);
        result.AddNumber(L"static_hash32", staticHash);
        result.AddNumber(L"static_vector_loop", staticVector);
        result.AddBool(L"static_results_verified", staticAdd == 42 && staticMul == 143);
        if (staticAdd != 42 || staticMul != 143)
        {
            result.status = L"fail";
        }

        HMODULE module = LoadPackagedLibrary(L"XComputeNativeModule.dll", 0);
        result.AddBool(L"dll_load_succeeded", module != nullptr);
        if (!module)
        {
            if (result.status == L"pass")
            {
                result.status = L"blocked";
            }
            result.errors.push_back(ErrorCodeToString(GetLastError()));
            result.notes.push_back(L"Packaged DLL loading failed; static native module results above still measure precompiled native code inside the package.");
            Finish(result, started);
            return result;
        }

        using DllAdd = uint32_t(__cdecl*)(uint32_t, uint32_t);
        using DllMul = uint32_t(__cdecl*)(uint32_t, uint32_t);
        using DllHash32 = uint32_t(__cdecl*)(uint8_t const*, size_t);
        using DllVectorLoop = uint64_t(__cdecl*)(uint32_t const*, size_t, uint32_t);

        auto add = reinterpret_cast<DllAdd>(GetProcAddress(module, "XComputeDllAdd"));
        auto mul = reinterpret_cast<DllMul>(GetProcAddress(module, "XComputeDllMul"));
        auto hash = reinterpret_cast<DllHash32>(GetProcAddress(module, "XComputeDllHash32"));
        auto vectorLoop = reinterpret_cast<DllVectorLoop>(GetProcAddress(module, "XComputeDllVectorLoop"));
        bool exportsFound = add && mul && hash && vectorLoop;
        result.AddBool(L"dll_exports_found", exportsFound);
        if (!exportsFound)
        {
            result.status = L"blocked";
            result.errors.push_back(ErrorCodeToString(GetLastError()));
            Finish(result, started);
            return result;
        }

        auto dllStarted = std::chrono::steady_clock::now();
        uint32_t dllAdd = add(17, 25);
        uint32_t dllMul = mul(13, 11);
        uint32_t dllHash = hash(bytes.data(), bytes.size());
        uint64_t dllVector = vectorLoop(vectorInput.data(), vectorInput.size(), 256);
        result.AddNumber(L"dll_duration_ms", ElapsedMs(dllStarted));
        result.AddNumber(L"dll_add_17_25", dllAdd);
        result.AddNumber(L"dll_mul_13_11", dllMul);
        result.AddNumber(L"dll_hash32", dllHash);
        result.AddNumber(L"dll_vector_loop", dllVector);
        bool dllVerified = dllAdd == staticAdd && dllMul == staticMul && dllHash == staticHash && dllVector == staticVector;
        result.AddBool(L"dll_results_match_static", dllVerified);
        if (!dllVerified)
        {
            result.status = L"fail";
        }

        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunDynamicCodeProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"dynamic_code.candidate";
        result.status = L"skipped";
        result.evidenceClass = L"UNKNOWN";
        result.startedUtc = UtcNow();
        result.AddString(L"compiled_manifest_flavor", XCOMPUTE_MANIFEST_FLAVOR);
        result.apiCalls = { L"VirtualAllocFromApp", L"VirtualProtectFromApp", L"FlushInstructionCache", L"generated x64 return stub" };

        if (std::wstring(XCOMPUTE_MANIFEST_FLAVOR) != L"codegen-candidate")
        {
            result.AddString(L"baseline_behavior", L"not_present_in_baseline_manifest");
            result.notes.push_back(L"Dynamic code execution is only attempted by the codegen candidate build.");
            Finish(result, started);
            return result;
        }

        result.evidenceClass = L"MEASURED";
        result.status = L"pass";
        result.notes.push_back(L"Executes only a fixed local six-byte x64 stub: mov eax, 42; ret.");

        constexpr SIZE_T allocationBytes = 4096;
        constexpr uint8_t code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
        result.AddNumber(L"allocation_bytes", allocationBytes);
        result.AddNumber(L"generated_code_bytes", sizeof(code));

        void* memory = VirtualAlloc(nullptr, allocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        result.AddBool(L"virtual_alloc_succeeded", memory != nullptr);
        if (!memory)
        {
            result.status = L"blocked";
            result.errors.push_back(ErrorCodeToString(GetLastError()));
            Finish(result, started);
            return result;
        }

        try
        {
            std::memcpy(memory, code, sizeof(code));

            DWORD oldProtect = 0;
            BOOL protectOk = VirtualProtect(memory, allocationBytes, PAGE_EXECUTE_READ, &oldProtect);
            result.AddBool(L"virtual_protect_execute_read_succeeded", protectOk != FALSE);
            result.AddNumber(L"previous_page_protection", oldProtect);
            if (!protectOk)
            {
                result.status = L"blocked";
                result.errors.push_back(ErrorCodeToString(GetLastError()));
            }
            else
            {
                BOOL flushOk = FlushInstructionCache(GetCurrentProcess(), memory, sizeof(code));
                result.AddBool(L"flush_instruction_cache_succeeded", flushOk != FALSE);
                if (!flushOk)
                {
                    result.status = L"blocked";
                    result.errors.push_back(ErrorCodeToString(GetLastError()));
                }
                else
                {
                    using GeneratedFunction = int(*)();
                    auto generated = reinterpret_cast<GeneratedFunction>(memory);
                    int value = generated();
                    result.AddNumber(L"expected_return_value", 42);
                    result.AddNumber(L"actual_return_value", static_cast<uint64_t>(value));
                    if (value != 42)
                    {
                        result.status = L"fail";
                    }
                }
            }
        }
        catch (std::exception const& ex)
        {
            result.status = L"error";
            std::wstring message(ex.what(), ex.what() + strlen(ex.what()));
            result.errors.push_back(message);
        }

        if (!VirtualFree(memory, 0, MEM_RELEASE))
        {
            result.errors.push_back(ErrorCodeToString(GetLastError()));
            if (result.status == L"pass")
            {
                result.status = L"error";
            }
        }

        Finish(result, started);
        return result;
    }

    ProbeResult ProbeRunner::RunExpandedResourcesProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"expanded_resources.candidate";
        result.status = L"skipped";
        result.evidenceClass = L"UNKNOWN";
        result.startedUtc = UtcNow();
        result.AddString(L"baseline_behavior", L"not_present_in_baseline_manifest");
        result.notes.push_back(L"Only the expanded candidate manifest may measure expanded resources.");
        Finish(result, started);
        return result;
    }

    std::wstring ProbeRunner::BuildReportJson(std::vector<ProbeResult> const& probes) const
    {
        std::wstring started = probes.empty() ? UtcNow() : probes.front().startedUtc;
        std::wstring ended = probes.empty() ? started : probes.back().endedUtc;
        std::wstring deviceFamily = JsonString(L"UNKNOWN");
        for (auto const& probe : probes)
        {
            auto iter = probe.values.find(L"device_family");
            if (iter != probe.values.end())
            {
                deviceFamily = iter->second;
                break;
            }
        }

        std::wostringstream out;
        out << L"{";
        out << L"\"schema_version\":\"1.0\",";
        out << L"\"app_version\":\"0.1.0\",";
        out << L"\"git_commit\":null,";
        out << L"\"build\":{\"configuration\":\"";
#if defined(NDEBUG)
        out << L"Release";
#else
        out << L"Debug";
#endif
        out << L"\",\"platform\":\"x64\",\"compiler\":\"MSVC\"},";
        out << L"\"manifest_flavor\":" << JsonString(XCOMPUTE_MANIFEST_FLAVOR) << L",";
        out << L"\"started_utc\":" << JsonString(started) << L",";
        out << L"\"ended_utc\":" << JsonString(ended) << L",";
        out << L"\"device_family\":" << deviceFamily << L",";
        out << L"\"probes\":[";
        for (size_t i = 0; i < probes.size(); ++i)
        {
            if (i != 0) out << L",";
            out << probes[i].ToJson();
        }
        out << L"],";
        out << L"\"user_confirmed_actions\":[],";
        out << L"\"warnings\":[],";
        out << L"\"findings\":{";
        out << L"\"MEASURED\":[\"Safe baseline probe suite executed.\"],";
        out << L"\"DOCUMENTED\":[\"See docs/SOURCES.md for platform and Codex claims.\"],";
        out << L"\"INFERRED\":[\"Architecture branch requires Xbox hardware results.\"],";
        out << L"\"UNKNOWN\":[\"Xbox-specific resource grants remain unknown until physical console run.\"]";
        out << L"}}";
        return out.str();
    }

    bool ProbeRunner::SaveReport(std::wstring const& reportJson)
    {
        try
        {
            auto folder = ApplicationData::Current().LocalFolder();
            std::wstring path = folder.Path().c_str();
            path += L"\\xcompute-probe-result.json";

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }

            auto utf8 = winrt::to_string(reportJson);
            out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            out.flush();
            return out.good();
        }
        catch (...)
        {
            return false;
        }
    }

    fire_and_forget ProbeRunner::SaveReportAsync(std::wstring reportJson)
    {
        try
        {
            auto folder = ApplicationData::Current().LocalFolder();
            auto file = co_await folder.CreateFileAsync(L"xcompute-probe-result.json", CreationCollisionOption::ReplaceExisting);
            co_await FileIO::WriteTextAsync(file, reportJson);
        }
        catch (...)
        {
            // The on-screen UI still shows probe status if persistence fails.
        }
    }

    fire_and_forget ProbeRunner::SendReportToLanReceiverAsync(std::wstring reportJson)
    {
        try
        {
            auto endpoint = ReadLanResultEndpoint();
            if (endpoint.empty() || !IsSupportedLanEndpoint(endpoint))
            {
                co_return;
            }

            HttpClient client;
            HttpStringContent content(reportJson, UnicodeEncoding::Utf8, L"application/json");
            auto response = co_await client.PostAsync(Uri(endpoint), content);
            response.EnsureSuccessStatusCode();
        }
        catch (...)
        {
            // Device Portal collection remains the fallback if LAN delivery fails.
        }
    }
}
