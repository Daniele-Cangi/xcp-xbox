#include "pch.h"
#include "WorkerPrototype.h"

using namespace winrt;
using namespace Windows::Storage;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    struct SearchStats
    {
        uint64_t filesScanned = 0;
        uint64_t hitCount = 0;
        std::wstring firstHit;
    };

    static void FinishWorkerProbe(ProbeResult& result, std::chrono::steady_clock::time_point started)
    {
        result.endedUtc = UtcNow();
        result.durationMs = ElapsedMs(started);
    }

    static std::wstring NarrowToWide(std::string const& value)
    {
        return std::wstring(value.begin(), value.end());
    }

    static std::string ReadText(fs::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("could not open text file for read");
        }
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    static void WriteText(fs::path const& path, std::string const& content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("could not open text file for write");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output.good())
        {
            throw std::runtime_error("text file write did not complete");
        }
    }

    static uint64_t WorkspaceBytes(fs::path const& root)
    {
        uint64_t total = 0;
        for (auto const& entry : fs::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                total += static_cast<uint64_t>(entry.file_size());
            }
        }
        return total;
    }

    static void SeedWorkspace(fs::path const& root)
    {
        if (root.filename() != L"probe-fixture")
        {
            throw std::runtime_error("unexpected worker probe fixture root");
        }

        fs::remove_all(root);
        WriteText(root / L"README.md",
            "# Codex Worker Prototype\n"
            "\n"
            "This sandbox workspace is created by the Xbox UWP worker probe.\n"
            "TODO: replace the probe marker after search.\n");
        WriteText(root / L"src" / L"worker.txt",
            "name=xcompute-worker\n"
            "mode=probe\n"
            "needle=codex\n");
        WriteText(root / L"notes" / L"constraints.txt",
            "runtime=uwp\n"
            "dynamic_code=false\n"
            "workspace=localfolder\n"
            "search=codex\n");
    }

    static SearchStats SearchWorkspace(fs::path const& root, std::string const& needle)
    {
        SearchStats stats;
        for (auto const& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            ++stats.filesScanned;
            auto relative = entry.path().lexically_relative(root).wstring();
            std::ifstream input(entry.path(), std::ios::binary);
            std::string line;
            uint64_t lineNumber = 0;
            while (std::getline(input, line))
            {
                ++lineNumber;
                auto column = line.find(needle);
                if (column == std::string::npos)
                {
                    continue;
                }

                ++stats.hitCount;
                if (stats.firstHit.empty())
                {
                    std::wostringstream out;
                    out << relative << L":" << lineNumber << L":" << (column + 1) << L": " << NarrowToWide(line);
                    stats.firstHit = out.str();
                }
            }
        }
        return stats;
    }

    static uint64_t CountNeedle(std::string const& text, std::string const& needle)
    {
        uint64_t count = 0;
        size_t position = 0;
        while ((position = text.find(needle, position)) != std::string::npos)
        {
            ++count;
            position += needle.size();
        }
        return count;
    }

    static bool ApplyExactPatch(fs::path const& workspaceRoot)
    {
        auto target = workspaceRoot / L"src" / L"worker.txt";
        auto content = ReadText(target);
        std::string before = "mode=probe";
        std::string after = "mode=worker-prototype";
        auto position = content.find(before);
        if (position == std::string::npos)
        {
            return false;
        }

        content.replace(position, before.size(), after);
        WriteText(target, content);
        WriteText(workspaceRoot / L"patches" / L"worker.patch",
            "--- a/src/worker.txt\n"
            "+++ b/src/worker.txt\n"
            "@@\n"
            "-mode=probe\n"
            "+mode=worker-prototype\n");

        auto verify = ReadText(target);
        return verify.find(after) != std::string::npos && verify.find(before) == std::string::npos;
    }

    ProbeResult RunWorkerPrototypeProbe()
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"worker.prototype";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = {
            L"ApplicationData.Current.LocalFolder",
            L"std::filesystem::recursive_directory_iterator",
            L"std::ifstream/std::ofstream",
            L"exact text replacement patch"
        };

        try
        {
            auto localFolder = ApplicationData::Current().LocalFolder();
            fs::path workspaceRoot = fs::path(std::wstring(localFolder.Path().c_str())) / L"codex-worker-prototype";
            fs::path fixtureRoot = workspaceRoot / L"probe-fixture";

            SeedWorkspace(fixtureRoot);
            auto beforeSearch = SearchWorkspace(fixtureRoot, "codex");
            auto patchApplied = ApplyExactPatch(fixtureRoot);
            auto afterSearch = SearchWorkspace(fixtureRoot, "worker-prototype");
            auto patchedTarget = ReadText(fixtureRoot / L"src" / L"worker.txt");
            auto oldMarkerCountInTarget = CountNeedle(patchedTarget, "mode=probe");
            auto newMarkerCountInTarget = CountNeedle(patchedTarget, "mode=worker-prototype");
            auto fixtureBytes = WorkspaceBytes(fixtureRoot);

            auto patchPath = fixtureRoot / L"patches" / L"worker.patch";
            result.AddString(L"workspace_path", workspaceRoot.wstring());
            result.AddString(L"probe_fixture_path", fixtureRoot.wstring());
            result.AddNumber(L"seeded_file_count", 3);
            result.AddNumber(L"files_scanned_before_patch", beforeSearch.filesScanned);
            result.AddNumber(L"search_hit_count_before_patch", beforeSearch.hitCount);
            result.AddString(L"first_search_hit", beforeSearch.firstHit);
            result.AddString(L"patch_target", L"src\\worker.txt");
            result.AddString(L"patch_strategy", L"exact_replace");
            result.AddBool(L"patch_applied", patchApplied);
            result.AddBool(L"patch_file_written", fs::exists(patchPath));
            result.AddNumber(L"search_hit_count_after_patch", afterSearch.hitCount);
            result.AddNumber(L"old_marker_hit_count_in_target_after_patch", oldMarkerCountInTarget);
            result.AddNumber(L"new_marker_hit_count_in_target_after_patch", newMarkerCountInTarget);
            result.AddNumber(L"workspace_bytes", fixtureBytes);
            result.AddNumber(L"probe_fixture_bytes", fixtureBytes);

            if (!patchApplied || afterSearch.hitCount == 0 || oldMarkerCountInTarget != 0 || newMarkerCountInTarget == 0)
            {
                result.status = L"fail";
            }
            result.notes.push_back(L"This is a deterministic local worker smoke test, not a full Codex runtime.");
        }
        catch (std::exception const& ex)
        {
            result.status = L"error";
            result.errors.push_back(NarrowToWide(ex.what()));
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }

        FinishWorkerProbe(result, started);
        return result;
    }
}
