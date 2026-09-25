#include "pch.h"
#include "WorkerCpuCapsuleModule.h"

#include "WorkerXvmTypes.h"
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
        constexpr uint64_t CapsuleModuleMaximumBytes = 1024ull * 1024ull;
        constexpr DWORD CapsuleModulePathCapacity = 32768u;

        std::wstring NormalizedModulePath(std::filesystem::path const& path)
        {
            auto value = path.lexically_normal().wstring();
            if (value.rfind(L"\\\\?\\", 0) == 0)
            {
                value.erase(0, 4);
            }
            std::replace(value.begin(), value.end(), L'/', L'\\');
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
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
                throw WorkerXvmError(
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
    }

    wchar_t const* WorkerCpuCapsuleDependencyPackageName()
    {
        return CapsulePackageName;
    }

    wchar_t const* WorkerCpuCapsuleExpectedPublisher()
    {
        return CapsulePublisher;
    }

    wchar_t const* WorkerCpuCapsuleModuleName()
    {
        return CapsuleModuleName;
    }

    WorkerCpuCapsuleModuleSelection WorkerResolveCpuCapsuleModule()
    {
        auto dependencies = Package::Current().Dependencies();
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
            throw WorkerXvmError(
                "cpu_capsule.package_dependency_missing",
                "the fixed CPU capsule framework is absent from the process package graph");
        }

        auto id = capsule.Id();
        auto publisher = std::wstring(id.Publisher().c_str());
        if (publisher != CapsulePublisher)
        {
            throw WorkerXvmError(
                "cpu_capsule.package_identity_mismatch",
                "the CPU capsule publisher does not match the admitted package identity");
        }

        if (id.Architecture() != Windows::System::ProcessorArchitecture::X64)
        {
            throw WorkerXvmError(
                "cpu_capsule.package_architecture_mismatch",
                "the CPU capsule dependency is not the admitted x64 architecture");
        }

        auto version = id.Version();
        WorkerCpuCapsuleModuleSelection selection;
        selection.dependencyCount = dependencies.Size();
        selection.packageName = std::wstring(id.Name().c_str());
        selection.packageFullName = std::wstring(id.FullName().c_str());
        selection.packageFamilyName = std::wstring(id.FamilyName().c_str());
        selection.publisher = std::move(publisher);
        selection.version = PackageVersionString(version);
        selection.versionPacked = PackageVersionPacked(version);
        selection.architecture = static_cast<uint32_t>(id.Architecture());
        selection.signatureKind = static_cast<uint32_t>(capsule.SignatureKind());
        selection.installedRoot = std::filesystem::path(std::wstring(capsule.InstalledLocation().Path().c_str()));
        selection.modulePath = selection.installedRoot / CapsuleModuleName;
        if (!std::filesystem::is_regular_file(selection.modulePath))
        {
            throw WorkerXvmError(
                "cpu_capsule.module_not_found",
                "the fixed CPU capsule module is absent from the selected dependency package");
        }
        selection.moduleBytes = static_cast<uint64_t>(std::filesystem::file_size(selection.modulePath));
        if (selection.moduleBytes == 0 || selection.moduleBytes > CapsuleModuleMaximumBytes)
        {
            throw WorkerXvmError(
                "cpu_capsule.module_size_invalid",
                "the fixed CPU capsule module is empty or exceeds the one-megabyte admission bound");
        }
        selection.moduleSha256 = Sha256File(selection.modulePath);
        return selection;
    }

    wchar_t const* WorkerCpuCapsuleResolutionProbeSchemaVersion()
    {
        return L"cpu-capsule-resolution-probe-v1";
    }

    std::wstring WorkerCpuCapsuleResolutionProbeJson(std::wstring const& protocolVersion)
    {
        auto selection = WorkerResolveCpuCapsuleModule();
        std::wostringstream out;
        out << L"{\"ok\":true,\"protocol_version\":" << JsonString(protocolVersion)
            << L",\"command\":\"probe_cpu_capsule_resolution\""
            << L",\"schema_version\":" << JsonString(WorkerCpuCapsuleResolutionProbeSchemaVersion())
            << L",\"selection\":{\"dependency_count\":" << selection.dependencyCount
            << L",\"package_name\":" << JsonString(selection.packageName)
            << L",\"package_full_name\":" << JsonString(selection.packageFullName)
            << L",\"package_family_name\":" << JsonString(selection.packageFamilyName)
            << L",\"publisher\":" << JsonString(selection.publisher)
            << L",\"package_version\":" << JsonString(selection.version)
            << L",\"architecture\":" << selection.architecture
            << L",\"signature_kind\":" << selection.signatureKind
            << L",\"package_installed_path\":" << JsonString(selection.installedRoot.wstring())
            << L",\"module_path\":" << JsonString(selection.modulePath.wstring())
            << L",\"module_bytes\":" << selection.moduleBytes
            << L",\"module_sha256\":" << JsonString(selection.moduleSha256) << L"}"
            << L",\"security\":{\"side_effect_free\":true,\"module_load_attempted\":false"
            << L",\"exports_invoked\":false,\"runtime_native_codegen\":false}}";
        return out.str();
    }

    WorkerCpuCapsuleModuleReference::WorkerCpuCapsuleModuleReference(
        WorkerCpuCapsuleModuleSelection const& selection)
    {
        wasLoadedBefore_ = GetModuleHandleW(CapsuleModuleName) != nullptr;
        auto started = std::chrono::steady_clock::now();
        auto loaded = LoadPackagedLibrary(CapsuleModuleName, 0);
        loadElapsedMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (!loaded)
        {
            throw WorkerXvmError(
                "cpu_capsule.load_failed",
                "LoadPackagedLibrary failed for the fixed CPU capsule; os_error=" +
                    std::to_string(GetLastError()));
        }

        std::vector<wchar_t> pathBuffer(CapsuleModulePathCapacity);
        SetLastError(ERROR_SUCCESS);
        auto pathLength = GetModuleFileNameW(
            loaded,
            pathBuffer.data(),
            static_cast<DWORD>(pathBuffer.size()));
        if (pathLength == 0 || pathLength >= pathBuffer.size())
        {
            auto error = GetLastError();
            (void)FreeLibrary(loaded);
            throw WorkerXvmError(
                "cpu_capsule.loaded_module_path_unavailable",
                "the loaded CPU capsule path could not be resolved without truncation; os_error=" +
                    std::to_string(error));
        }
        loadedModulePath_ = std::filesystem::path(std::wstring(pathBuffer.data(), pathLength));
        if (NormalizedModulePath(loadedModulePath_) != NormalizedModulePath(selection.modulePath))
        {
            (void)FreeLibrary(loaded);
            throw WorkerXvmError(
                "cpu_capsule.loaded_module_path_mismatch",
                "LoadPackagedLibrary resolved a module outside the immutable selected capsule plan");
        }
        module_ = loaded;
    }

    WorkerCpuCapsuleModuleReference::~WorkerCpuCapsuleModuleReference()
    {
        if (module_)
        {
            (void)FreeLibrary(module_);
        }
    }

    FARPROC WorkerCpuCapsuleModuleReference::Export(char const* name) const
    {
        auto value = GetProcAddress(module_, name);
        if (!value)
        {
            throw WorkerXvmError(
                "cpu_capsule.export_missing",
                std::string("the fixed CPU capsule ABI export is missing: ") + name);
        }
        return value;
    }
}
