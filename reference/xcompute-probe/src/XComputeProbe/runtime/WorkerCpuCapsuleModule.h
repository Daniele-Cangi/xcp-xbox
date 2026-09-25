#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <windows.h>

namespace XComputeProbe
{
    struct WorkerCpuCapsuleModuleSelection
    {
        uint32_t dependencyCount = 0;
        std::wstring packageName;
        std::wstring packageFullName;
        std::wstring packageFamilyName;
        std::wstring publisher;
        std::wstring version;
        uint64_t versionPacked = 0;
        uint32_t architecture = 0;
        uint32_t signatureKind = 0;
        std::filesystem::path installedRoot;
        std::filesystem::path modulePath;
        uint64_t moduleBytes = 0;
        std::wstring moduleSha256;
    };

    wchar_t const* WorkerCpuCapsuleDependencyPackageName();
    wchar_t const* WorkerCpuCapsuleExpectedPublisher();
    wchar_t const* WorkerCpuCapsuleModuleName();

    WorkerCpuCapsuleModuleSelection WorkerResolveCpuCapsuleModule();
    wchar_t const* WorkerCpuCapsuleResolutionProbeSchemaVersion();
    std::wstring WorkerCpuCapsuleResolutionProbeJson(std::wstring const& protocolVersion);

    class WorkerCpuCapsuleModuleReference
    {
    public:
        explicit WorkerCpuCapsuleModuleReference(WorkerCpuCapsuleModuleSelection const& selection);
        ~WorkerCpuCapsuleModuleReference();

        WorkerCpuCapsuleModuleReference(WorkerCpuCapsuleModuleReference const&) = delete;
        WorkerCpuCapsuleModuleReference& operator=(WorkerCpuCapsuleModuleReference const&) = delete;

        FARPROC Export(char const* name) const;

        HMODULE Handle() const noexcept { return module_; }
        double LoadElapsedMs() const noexcept { return loadElapsedMs_; }
        bool WasLoadedBefore() const noexcept { return wasLoadedBefore_; }
        std::filesystem::path const& LoadedModulePath() const noexcept { return loadedModulePath_; }

    private:
        HMODULE module_ = nullptr;
        double loadElapsedMs_ = 0.0;
        bool wasLoadedBefore_ = false;
        std::filesystem::path loadedModulePath_;
    };
}
