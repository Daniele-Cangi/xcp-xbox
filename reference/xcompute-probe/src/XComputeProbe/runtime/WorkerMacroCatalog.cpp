#include "pch.h"
#include "WorkerMacroCatalog.h"

namespace XComputeProbe
{
    namespace
    {
        std::vector<WorkerMacroDefinition> BuildWorkerMacroCatalog()
        {
            return {
                {
                    L"evidence_reduce_pipeline_v1",
                    2,
                    {
                        { L"left_branch", L"left_input", L"hash_reduce_v1", 0 },
                        { L"right_branch", L"right_input", L"tiled_scan_u8_v1", 1 },
                    },
                },
                {
                    L"dual_manifest_compare_v1",
                    2,
                    {
                        { L"left_branch", L"left_input", L"hash_reduce_v1", 0 },
                        { L"right_branch", L"right_input", L"stream_mix_v1", 1 },
                    },
                },
                {
                    L"triple_fanin_integrity_v1",
                    3,
                    {
                        { L"first_branch", L"first_input", L"hash_reduce_v1", 0 },
                        { L"second_branch", L"second_input", L"stream_mix_v1", 1 },
                        { L"third_branch", L"third_input", L"tiled_scan_u8_v1", 2 },
                    },
                },
                {
                    L"single_manifest_multi_kernel_v1",
                    1,
                    {
                        { L"hash_branch", L"hash_input", L"hash_reduce_v1", 0 },
                        { L"stream_branch", L"stream_input", L"stream_mix_v1", 0 },
                        { L"scan_branch", L"scan_input", L"tiled_scan_u8_v1", 0 },
                    },
                },
            };
        }
    }

    std::vector<std::wstring> WorkerMacroCatalogIds()
    {
        std::vector<std::wstring> ids;
        for (auto const& definition : BuildWorkerMacroCatalog())
        {
            ids.push_back(definition.macroId);
        }
        return ids;
    }

    std::optional<WorkerMacroDefinition> TryGetWorkerMacroDefinition(std::wstring const& macroId)
    {
        for (auto const& definition : BuildWorkerMacroCatalog())
        {
            if (definition.macroId == macroId)
            {
                return definition;
            }
        }
        return std::nullopt;
    }
}
