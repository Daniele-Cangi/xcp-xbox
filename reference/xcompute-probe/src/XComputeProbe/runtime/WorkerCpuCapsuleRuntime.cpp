#include "pch.h"
#include "WorkerCpuCapsuleRuntime.h"

#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* CapsulePackageName = L"XComputeProbe.CpuCapsule.Framework";
        constexpr wchar_t const* CapsulePublisher = L"CN=LocalDev";
        constexpr wchar_t const* CapsuleModuleName = L"XComputeCpuCapsuleV1.dll";
        constexpr uint32_t CapsuleAbiVersion = 1u;
        constexpr uint32_t CapsuleSelfTestMagic = 0x43504331u;
        constexpr uint64_t CapsuleSeed = 0x123456789ABCDEF0ull;
        constexpr uint32_t CapsuleRounds = 65536u;

        wchar_t const* BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        double ElapsedMilliseconds(std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }

        uint64_t AppMemoryUsageBytes()
        {
            try
            {
                return Windows::System::MemoryManager::AppMemoryUsage();
            }
            catch (...)
            {
                return 0;
            }
        }

        std::wstring PackageVersionString(PackageVersion const& version)
        {
            std::wostringstream out;
            out << version.Major << L"." << version.Minor << L"." << version.Build << L"." << version.Revision;
            return out.str();
        }

        uint64_t PackageVersionPacked(PackageVersion const& version)
        {
            return
                (static_cast<uint64_t>(version.Major) << 48) |
                (static_cast<uint64_t>(version.Minor) << 32) |
                (static_cast<uint64_t>(version.Build) << 16) |
                static_cast<uint64_t>(version.Revision);
        }

        std::wstring Sha256File(std::filesystem::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.module_not_found",
                    "the fixed CPU capsule module is absent from the selected dependency package");
            }
            std::vector<uint8_t> bytes(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
            auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
            auto buffer = CryptographicBuffer::CreateFromByteArray(bytes);
            auto value = std::wstring(CryptographicBuffer::EncodeToHexString(provider.HashData(buffer)).c_str());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }

        uint64_t CanonicalTransform(uint64_t seed, uint32_t rounds)
        {
            uint64_t value = seed ^ 0x9E3779B97F4A7C15ull;
            for (uint32_t round = 0; round < rounds; ++round)
            {
                value ^= value << 13;
                value ^= value >> 7;
                value ^= value << 17;
                value += 0xD1B54A32D192ED03ull + static_cast<uint64_t>(round);
            }
            return value;
        }

        std::wstring NewActivationId()
        {
            auto bytes = CryptographicBuffer::GenerateRandom(16);
            auto value = std::wstring(CryptographicBuffer::EncodeToHexString(bytes).c_str());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }
    }

    WorkerCpuCapsuleRuntime::WorkerCpuCapsuleRuntime()
        : activationId_(NewActivationId())
    {
    }

    wchar_t const* WorkerCpuCapsuleRuntime::GateId()
    {
        return L"CPU_CAPSULE_PACKAGE_GRAPH_V1";
    }

    wchar_t const* WorkerCpuCapsuleRuntime::ProbeSchemaVersion()
    {
        return L"cpu-capsule-package-graph-probe-v1";
    }

    wchar_t const* WorkerCpuCapsuleRuntime::DependencyPackageName()
    {
        return CapsulePackageName;
    }

    wchar_t const* WorkerCpuCapsuleRuntime::ModuleName()
    {
        return CapsuleModuleName;
    }

    uint32_t WorkerCpuCapsuleRuntime::AbiVersion()
    {
        return CapsuleAbiVersion;
    }

    std::wstring WorkerCpuCapsuleRuntime::Probe(std::wstring const& protocolVersion) const
    {
        auto current = Package::Current();
        auto dependencies = current.Dependencies();
        Package capsule{ nullptr };
        for (auto const& dependency : dependencies)
        {
            if (std::wstring(dependency.Id().Name().c_str()) == CapsulePackageName)
            {
                capsule = dependency;
                break;
            }
        }
        if (!capsule)
        {
            throw WorkerCpuCapsuleError(
                "cpu_capsule.package_dependency_missing",
                "the fixed CPU capsule framework is absent from the process package graph");
        }

        auto capsuleId = capsule.Id();
        auto publisher = std::wstring(capsuleId.Publisher().c_str());
        if (publisher != CapsulePublisher)
        {
            throw WorkerCpuCapsuleError(
                "cpu_capsule.package_identity_mismatch",
                "the CPU capsule publisher does not match the admitted package identity");
        }

        auto version = capsuleId.Version();
        auto installedRoot = std::filesystem::path(std::wstring(capsule.InstalledLocation().Path().c_str()));
        auto modulePath = installedRoot / CapsuleModuleName;
        auto hashStarted = std::chrono::steady_clock::now();
        auto moduleSha256 = Sha256File(modulePath);
        auto hashMs = ElapsedMilliseconds(hashStarted);
        auto moduleBytes = static_cast<uint64_t>(std::filesystem::file_size(modulePath));

        using AbiFunction = uint32_t(__cdecl*)();
        using BuildFunction = uint64_t(__cdecl*)();
        using SelfTestFunction = uint32_t(__cdecl*)();
        using TransformFunction = uint64_t(__cdecl*)(uint64_t, uint32_t);

        auto moduleWasLoadedBeforeProbe = GetModuleHandleW(CapsuleModuleName) != nullptr;
        auto memoryBefore = AppMemoryUsageBytes();
        HMODULE first = nullptr;
        HMODULE second = nullptr;
        bool firstReleased = false;
        bool secondReleased = false;
        try
        {
            auto firstLoadStarted = std::chrono::steady_clock::now();
            first = LoadPackagedLibrary(CapsuleModuleName, 0);
            auto firstLoadMs = ElapsedMilliseconds(firstLoadStarted);
            if (!first)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.load_failed",
                    "LoadPackagedLibrary failed for the fixed CPU capsule; os_error=" + std::to_string(GetLastError()));
            }
            auto memoryAfterFirstLoad = AppMemoryUsageBytes();

            auto abi = reinterpret_cast<AbiFunction>(GetProcAddress(first, "XComputeCpuCapsuleAbiVersion"));
            auto build = reinterpret_cast<BuildFunction>(GetProcAddress(first, "XComputeCpuCapsuleBuildVersionPacked"));
            auto selfTest = reinterpret_cast<SelfTestFunction>(GetProcAddress(first, "XComputeCpuCapsuleSelfTest"));
            auto transform = reinterpret_cast<TransformFunction>(GetProcAddress(first, "XComputeCpuCapsuleTransform"));
            if (!abi || !build || !selfTest || !transform)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.export_missing",
                    "one or more fixed CPU capsule ABI exports are missing");
            }

            auto abiVersion = abi();
            if (abiVersion != CapsuleAbiVersion)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.abi_mismatch",
                    "the CPU capsule ABI version does not match the worker contract");
            }
            auto buildVersionPacked = build();
            if (buildVersionPacked != PackageVersionPacked(version))
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.package_identity_mismatch",
                    "the CPU capsule build version does not match its selected package identity");
            }
            if (selfTest() != CapsuleSelfTestMagic)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.self_test_failed",
                    "the fixed CPU capsule self-test returned an unexpected value");
            }

            auto firstExecuteStarted = std::chrono::steady_clock::now();
            auto firstValue = transform(CapsuleSeed, CapsuleRounds);
            auto firstExecuteMs = ElapsedMilliseconds(firstExecuteStarted);
            auto referenceValue = CanonicalTransform(CapsuleSeed, CapsuleRounds);
            if (firstValue != referenceValue)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.differential_mismatch",
                    "the CPU capsule result diverged from the in-package canonical reference");
            }

            auto secondLoadStarted = std::chrono::steady_clock::now();
            second = LoadPackagedLibrary(CapsuleModuleName, 0);
            auto secondLoadMs = ElapsedMilliseconds(secondLoadStarted);
            if (!second)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.load_failed",
                    "the second fixed CPU capsule reference could not be acquired; os_error=" + std::to_string(GetLastError()));
            }
            auto handlesSame = first == second;
            auto memoryAfterSecondLoad = AppMemoryUsageBytes();

            if (!FreeLibrary(first))
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.load_failed",
                    "the first CPU capsule reference could not be released; os_error=" + std::to_string(GetLastError()));
            }
            firstReleased = true;
            first = nullptr;

            auto afterSingleFreeStarted = std::chrono::steady_clock::now();
            auto afterSingleFreeValue = transform(CapsuleSeed, CapsuleRounds);
            auto afterSingleFreeExecuteMs = ElapsedMilliseconds(afterSingleFreeStarted);
            auto survivedSingleFree = afterSingleFreeValue == referenceValue && selfTest() == CapsuleSelfTestMagic;
            if (!survivedSingleFree)
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.self_test_failed",
                    "the second CPU capsule reference did not survive release of the first reference");
            }

            if (!FreeLibrary(second))
            {
                throw WorkerCpuCapsuleError(
                    "cpu_capsule.load_failed",
                    "the final CPU capsule reference could not be released; os_error=" + std::to_string(GetLastError()));
            }
            secondReleased = true;
            second = nullptr;
            auto moduleLoadedAfterFinalFree = GetModuleHandleW(CapsuleModuleName) != nullptr;
            auto memoryAfterFinalFree = AppMemoryUsageBytes();
            auto memoryPeak = memoryBefore;
            if (memoryAfterFirstLoad > memoryPeak) { memoryPeak = memoryAfterFirstLoad; }
            if (memoryAfterSecondLoad > memoryPeak) { memoryPeak = memoryAfterSecondLoad; }
            if (memoryAfterFinalFree > memoryPeak) { memoryPeak = memoryAfterFinalFree; }

            std::wostringstream out;
            out << L"{\"ok\":true,\"protocol_version\":" << JsonString(protocolVersion)
                << L",\"command\":\"probe_cpu_capsule_package_graph\""
                << L",\"schema_version\":" << JsonString(ProbeSchemaVersion())
                << L",\"gate_id\":" << JsonString(GateId())
                << L",\"activation_id\":" << JsonString(activationId_)
                << L",\"package_graph\":{\"dependency_model\":\"static_manifest_framework_package\""
                << L",\"dependency_count\":" << dependencies.Size()
                << L",\"capsule_dependency\":{\"name\":" << JsonString(capsuleId.Name().c_str())
                << L",\"full_name\":" << JsonString(capsuleId.FullName().c_str())
                << L",\"family_name\":" << JsonString(capsuleId.FamilyName().c_str())
                << L",\"publisher\":" << JsonString(publisher)
                << L",\"version\":" << JsonString(PackageVersionString(version))
                << L",\"architecture\":" << static_cast<uint32_t>(capsuleId.Architecture())
                << L",\"signature_kind\":" << static_cast<uint32_t>(capsule.SignatureKind()) << L"}}"
                << L",\"capsule\":{\"module_name\":" << JsonString(CapsuleModuleName)
                << L",\"module_bytes\":" << moduleBytes
                << L",\"module_sha256\":" << JsonString(moduleSha256)
                << L",\"abi_version\":" << abiVersion
                << L",\"build_version_packed\":" << buildVersionPacked
                << L",\"exports\":[\"XComputeCpuCapsuleAbiVersion\",\"XComputeCpuCapsuleBuildVersionPacked\",\"XComputeCpuCapsuleSelfTest\",\"XComputeCpuCapsuleTransform\"]}"
                << L",\"canonical_result\":{\"seed\":" << CapsuleSeed
                << L",\"rounds\":" << CapsuleRounds
                << L",\"capsule_value\":" << firstValue
                << L",\"reference_value\":" << referenceValue
                << L",\"after_single_free_value\":" << afterSingleFreeValue
                << L",\"exact_match\":true,\"self_test_magic\":" << CapsuleSelfTestMagic << L"}"
                << L",\"telemetry\":{\"module_was_loaded_before_probe\":" << BoolJson(moduleWasLoadedBeforeProbe)
                << L",\"first_load_ms\":" << std::fixed << std::setprecision(6) << firstLoadMs
                << L",\"second_load_ms\":" << secondLoadMs
                << L",\"module_hash_ms\":" << hashMs
                << L",\"first_execute_ms\":" << firstExecuteMs
                << L",\"after_single_free_execute_ms\":" << afterSingleFreeExecuteMs
                << L",\"handles_same\":" << BoolJson(handlesSame)
                << L",\"survived_single_free\":" << BoolJson(survivedSingleFree)
                << L",\"module_loaded_after_final_free\":" << BoolJson(moduleLoadedAfterFinalFree)
                << L",\"app_memory_before_bytes\":" << memoryBefore
                << L",\"app_memory_after_first_load_bytes\":" << memoryAfterFirstLoad
                << L",\"app_memory_after_second_load_bytes\":" << memoryAfterSecondLoad
                << L",\"app_memory_after_final_free_bytes\":" << memoryAfterFinalFree
                << L",\"app_memory_peak_bytes\":" << memoryPeak << L"}"
                << L",\"security\":{\"static_manifest_dependency\":true"
                << L",\"client_native_payload_accepted\":false"
                << L",\"runtime_native_codegen\":false"
                << L",\"dynamic_dependency_api_used\":false"
                << L",\"restricted_capability_required\":false}}";
            return out.str();
        }
        catch (...)
        {
            if (second && !secondReleased)
            {
                FreeLibrary(second);
            }
            if (first && !firstReleased)
            {
                FreeLibrary(first);
            }
            throw;
        }
    }
}