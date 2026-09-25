#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace XComputeProbe
{
    struct WorkerMacroBranchDefinition
    {
        std::wstring nodeId;
        std::wstring edgeRole;
        std::wstring kernelId;
        uint32_t manifestInputIndex = 0;
    };

    struct WorkerMacroDefinition
    {
        std::wstring macroId;
        uint32_t requiredInputCount = 0;
        std::vector<WorkerMacroBranchDefinition> branches;
    };

    std::vector<std::wstring> WorkerMacroCatalogIds();
    std::optional<WorkerMacroDefinition> TryGetWorkerMacroDefinition(std::wstring const& macroId);
}
