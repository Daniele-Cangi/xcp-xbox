#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace XComputeProbe
{
    struct WorkerXvmSnapshotAuthority
    {
        std::wstring keyId;
        std::function<std::wstring(std::wstring const& canonicalInput)> sealCanonical;

        bool Ready() const noexcept
        {
            return keyId.size() == 64 && static_cast<bool>(sealCanonical);
        }
    };

    wchar_t const* WorkerXvmSnapshotSealSchemaVersion();

    WorkerXvmSnapshotAuthority WorkerLoadXvmSnapshotAuthority(
        std::filesystem::path const& privateAuthorityRoot);

    WorkerXvmSnapshotAuthority WorkerLoadOrCreateXvmSnapshotAuthority(
        std::filesystem::path const& privateAuthorityRoot);
}