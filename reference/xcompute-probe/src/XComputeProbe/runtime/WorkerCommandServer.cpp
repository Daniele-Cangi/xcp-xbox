#include "pch.h"
#include "WorkerCommandServer.h"
#include "WorkerCreativeInstallRuntime.h"
#include "WorkerCreativeHostRuntime.h"
#include "WorkerCpuCapsuleRuntime.h"
#include "WorkerArtifactManifestCompute.h"
#include "WorkerArtifactPublicationRuntime.h"
#include "WorkerArtifactStore.h"
#include "WorkerDiagnostics.h"
#include "WorkerGraphExecution.h"
#include "WorkerGraphExpansion.h"
#include "WorkerGraphOrchestration.h"
#include "WorkerGraphPublishing.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphRuntime.h"
#include "WorkerGraphValidation.h"
#include "WorkerMacroCatalog.h"
#include "WorkerPrecompiledD3DCommandRuntime.h"
#include "WorkerRuntimeDescription.h"
#include "WorkerXvmCpuCapsuleBackend.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmRuntime.h"
#include "WorkerXvmSnapshotAuthority.h"
#include "../native/StaticNativeModule.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    constexpr size_t MaxRequestBytes = 512 * 1024;
    constexpr size_t MaxTextWriteBytes = 256 * 1024;
    constexpr size_t MaxChunkBytes = 64 * 1024;
    constexpr size_t MaxArtifactJsonChunkBytes = 64 * 1024;
    constexpr size_t MaxArtifactManifestJsonBytes = 1024 * 1024;
    constexpr size_t ArtifactManifestJobReadBufferBytes = 1024 * 1024;
    constexpr size_t MaxStreamReadBytes = 4 * 1024 * 1024;
    constexpr size_t MaxStreamWriteBytes = 4 * 1024 * 1024;
    constexpr uint64_t MaxArtifactManifestParts = 4096;
    constexpr uint64_t MaxArtifactExpectedBytes = 4ull * 1024ull * 1024ull * 1024ull;
    constexpr uint64_t DefaultArtifactRollbackHeadroomBytes = 256ull * 1024ull * 1024ull;
    constexpr uint64_t DefaultArtifactWorkspaceBudgetBytes = 6ull * 1024ull * 1024ull * 1024ull;
    constexpr uint64_t MaxArtifactWorkspaceBudgetBytes = 16ull * 1024ull * 1024ull * 1024ull;
    constexpr uint64_t MaxJobDataBytes = 32ull * 1024ull * 1024ull;
    constexpr uint64_t MaxJobRounds = 4096;
    constexpr uint64_t MaxVectorLength = 1024ull * 1024ull;
    constexpr uint64_t MaxInterpreterProgramOps = 4096;
    constexpr uint64_t MaxInterpreterIterations = 1000000;
    constexpr uint64_t MaxMemoryInterpreterInputBytes = 64ull * 1024ull;
    constexpr uint64_t MaxMemoryInterpreterBytes = 1024ull * 1024ull;
    constexpr uint64_t MaxMemoryInterpreterOutputBytes = 64ull * 1024ull;
    constexpr uint64_t MaxD3D11ComputeElements = 1024ull * 1024ull;
    constexpr uint64_t MaxD3D12Fp32TimingElements = 8ull * 1024ull * 1024ull;
    constexpr uint64_t MaxD3D12ShaderShapeElements = 8ull * 1024ull * 1024ull;
    constexpr uint64_t MaxD3D11ComputeSweepEntries = 16;
    constexpr uint64_t MaxD3D11ComputeSweepRepeats = 32;
    constexpr uint64_t MaxD3D11ShaderMatrixVariants = 8;
    constexpr uint64_t MaxD3D11ShaderMatrixStatsBatches = 16;
    constexpr uint64_t MaxD3D11ShaderMatrixStatsPauseMs = 60000;
    constexpr uint64_t MaxD3D11SustainedSoakDurationSeconds = 3600;
    constexpr uint64_t MaxD3D11SustainedSoakWindowSeconds = 600;
    constexpr uint64_t MaxD3D11SustainedSoakRepeatsPerBatch = 512;
    constexpr uint64_t MaxD3D11SustainedSoakBatches = 100000;
    constexpr uint64_t MaxD3D11ResidentHotLoopDurationSeconds = 3600;
    constexpr uint64_t MaxD3D11ResidentHotLoopWindowSeconds = 600;
    constexpr uint64_t MaxD3D11ResidentHotLoopDispatchesPerSample = 1024;
    constexpr uint64_t MaxD3D11ResidentHotLoopWarmupDispatches = 65536;
    constexpr uint64_t MaxD3D11ResidentHotLoopDispatches = 5000000;
    constexpr uint64_t MaxD3D12ShaderShapeSoakWindows = 256;
    constexpr uint64_t MaxD3D12ShaderShapeSoakWindowPauseMs = 60000;
    constexpr uint64_t MaxD3D11QueryWaitMs = 10000;
    constexpr uint32_t D3D11ComputeThreadsPerGroup = 64;
    constexpr uint64_t D3D11FloatOpsPerElement = 32ull * 4ull * 2ull;
    constexpr double D3D11FloatAbsoluteTolerance = 0.05;
    constexpr double D3D11FloatRelativeTolerance = 0.001;
    constexpr uint32_t InterpreterArtifactMagic = 0x49504358u; // XCPI
    constexpr uint32_t MemoryProgramArtifactMagic = 0x4d504358u; // XCPM
    constexpr uint32_t InterpreterArtifactVersion = 1;
    constexpr uint32_t RequestReadBufferBytes = 4096;
    constexpr uint32_t BinaryPayloadReadBufferBytes = 256 * 1024;
    constexpr uint64_t DefaultSessionTtlSeconds = WorkerSessionRuntime::DefaultTtlSeconds;
    constexpr uint64_t MinSessionTtlSeconds = WorkerSessionRuntime::MinTtlSeconds;
    constexpr uint64_t MaxSessionTtlSeconds = WorkerSessionRuntime::MaxTtlSeconds;
    constexpr uint64_t MaxTrustedControllers = 16;
    constexpr uint64_t MaxControllerPublicKeyBytes = 4096;
    constexpr uint64_t MaxTrustNonceAgeSeconds = 24 * 60 * 60;
    constexpr int64_t MaxTrustClockSkewSeconds = 5 * 60;
    constexpr uint64_t MaxPhysicsGridWidth = 1024;
    constexpr uint64_t MaxPhysicsGridHeight = 1024;
    constexpr uint64_t MaxPhysicsCellCount = 1024ull * 1024ull;
    constexpr uint64_t MaxPhysicsSteps = 4096;
    constexpr uint64_t MaxPhysicsCellUpdates = 128ull * 1024ull * 1024ull;
    constexpr wchar_t const* WorkerProtocolVersion = L"0.73";
    constexpr wchar_t const* ArtifactUploadSchemaVersion = L"worker-artifact-upload-0.1";
    constexpr wchar_t const* ArtifactBinaryFramingSchemaVersion = L"worker-artifact-binary-framing-v2-0.1";
    constexpr wchar_t const* JobResultArtifactSchemaVersion = L"worker-job-result-artifact-0.1";
    constexpr wchar_t const* ArtifactManifestJobSchemaVersion = L"worker-artifact-manifest-job-0.1";
    constexpr wchar_t const* ArtifactManifestComputeJobSchemaVersion = L"worker-artifact-manifest-compute-job-0.1";
    constexpr wchar_t const* PhysicsKernelJobSchemaVersion = L"worker-physics-kernel-job-0.1";
    constexpr wchar_t const* PhysicsCellularResultSchemaVersion = L"worker-physics-cellular-material-spread-result-0.1";

    struct CommandSearchStats
    {
        uint64_t filesScanned = 0;
        uint64_t hitCount = 0;
        std::vector<std::wstring> hits;
    };

    struct TreeEntry
    {
        std::wstring path;
        std::wstring relativePath;
        uint64_t size = 0;
        std::wstring sha256;
    };

    struct BinaryChunkResponse
    {
        std::wstring headerJson;
        std::vector<uint8_t> payload;
    };

    struct InterpreterArtifact
    {
        std::wstring programId;
        uint32_t seed = 0;
        std::vector<uint32_t> program;
    };

    struct MemoryInterpreterResult
    {
        uint64_t accumulator = 0;
        std::vector<uint8_t> output;
        uint32_t memoryHash = 0;
        uint32_t outputHash = 0;
    };

    using DoubleSampleStats = D3DDoubleSampleStats;

    struct D3D11ShaderMatrixStatsMeasurement
    {
        D3D11ShaderMatrixVariant variant;
        uint64_t requestedElements = 0;
        uint64_t elements = 0;
        uint64_t dispatchGroups = 0;
        uint64_t inputBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t gpuBytesPerDispatch = 0;
        uint64_t repeats = 1;
        uint64_t warmupRepeats = 0;
        uint64_t batchCount = 1;
        uint64_t batchPauseMs = 0;
        uint64_t shaderBytes = 0;
        uint64_t gpuTimingSampleCount = 0;
        uint64_t gpuDisjointCount = 0;
        uint64_t cpuSubmitSampleCount = 0;
        uint64_t fp32OpsTimed = 0;
        uint64_t gpuBytesTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;
        std::vector<double> gpuDispatchSamplesMs;
        std::vector<double> cpuSubmitSamplesMs;
        std::vector<double> gflopsSamples;
        std::vector<double> gpuMiBPerSecondSamples;
        std::vector<double> batchMedianGpuMs;
        DoubleSampleStats gpuDispatchStats;
        DoubleSampleStats cpuSubmitStats;
        DoubleSampleStats gflopsStats;
        DoubleSampleStats gpuMiBPerSecondStats;
        double firstBatchMedianGpuMs = 0.0;
        double lastBatchMedianGpuMs = 0.0;
        double driftGpuMs = 0.0;
        double driftPercent = 0.0;
    };

    static std::wstring Utf8ToWide(std::string const& value)
    {
        return winrt::to_hstring(value).c_str();
    }

    static std::string WideToUtf8(std::wstring const& value)
    {
        return winrt::to_string(winrt::hstring(value));
    }

    static std::wstring GenerateSessionId()
    {
        auto ticks = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        uint64_t first = ticks;
        uint64_t second = ticks ^ 0xa5a5a5a55a5a5a5aull;
        try
        {
            std::random_device random;
            first ^= (static_cast<uint64_t>(random()) << 32) ^ random();
            second ^= (static_cast<uint64_t>(random()) << 32) ^ random();
        }
        catch (...)
        {
        }

        std::wostringstream out;
        out << std::hex << std::setfill(L'0')
            << std::setw(16) << first
            << std::setw(16) << second;
        return out.str();
    }

    static std::string ReadTextFile(fs::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("file.read_failed", "could not open file for read");
        }
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    static void WriteInternalTextFile(fs::path const& path, std::string const& content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw WorkerProtocolError("file.write_failed", "could not open file for write");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output.good())
        {
            throw WorkerProtocolError("file.write_failed", "file write did not complete");
        }
    }

    static void WriteTextFile(fs::path const& path, std::string const& content)
    {
        if (content.size() > MaxTextWriteBytes)
        {
            throw WorkerProtocolError("content.too_large", "content exceeds 256 KiB command limit");
        }
        WriteInternalTextFile(path, content);
    }

    static std::vector<uint8_t> ReadBinaryFileBounded(fs::path const& path, uint64_t maxBytes)
    {
        if (!fs::is_regular_file(path))
        {
            throw WorkerProtocolError("path.not_file", "input path is not a file");
        }
        auto size = static_cast<uint64_t>(fs::file_size(path));
        if (size > maxBytes)
        {
            throw WorkerProtocolError("file.too_large", "input file exceeds command byte limit");
        }

        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("file.read_failed", "could not open binary file for read");
        }
        if (!bytes.empty())
        {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            {
                throw WorkerProtocolError("file.read_failed", "binary file read did not complete");
            }
        }
        return bytes;
    }

    static void WriteBinaryFile(fs::path const& path, std::vector<uint8_t> const& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw WorkerProtocolError("file.write_failed", "could not open binary file for write");
        }
        if (!bytes.empty())
        {
            output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        output.flush();
        if (!output.good())
        {
            throw WorkerProtocolError("file.write_failed", "binary file write did not complete");
        }
    }

    static fs::path WorkerRoot()
    {
        auto localFolder = ApplicationData::Current().LocalFolder();
        return fs::path(std::wstring(localFolder.Path().c_str())) / L"codex-worker-prototype";
    }

    static void EnsureWorkspace(fs::path const& root)
    {
        if (fs::exists(root / L"README.md") && fs::exists(root / L"src" / L"worker.txt"))
        {
            return;
        }

        WriteTextFile(root / L"README.md",
            "# Codex Worker Prototype\n"
            "\n"
            "Command worker workspace inside Xbox UWP LocalFolder.\n");
        WriteTextFile(root / L"src" / L"worker.txt",
            "name=xcompute-worker\n"
            "mode=command-server\n"
            "needle=codex\n");
    }

    static bool IsSafeRelativePath(std::wstring const& value)
    {
        if (value.empty() ||
            value.size() > 180 ||
            value.find(L':') != std::wstring::npos ||
            value.find(L'\0') != std::wstring::npos ||
            value.front() == L'\\')
        {
            return false;
        }

        fs::path path(value);
        if (path.is_absolute() || path.has_root_path())
        {
            return false;
        }

        for (auto const& part : path)
        {
            auto text = part.wstring();
            if (text.empty() || text == L"." || text == L".." || text.find(L'\0') != std::wstring::npos)
            {
                return false;
            }
        }
        return true;
    }

    static bool IsSafeProgramId(std::wstring const& value)
    {
        if (value.empty() || value.size() > 64)
        {
            return false;
        }
        for (auto ch : value)
        {
            bool ok = (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'_' ||
                ch == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    static fs::path ResolveWorkspacePath(fs::path const& root, std::wstring relative)
    {
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        if (!IsSafeRelativePath(relative))
        {
            throw WorkerProtocolError("path.unsafe", "unsafe relative path");
        }
        return (root / fs::path(relative)).lexically_normal();
    }

    static fs::path InterpreterProgramDirectory(fs::path const& root)
    {
        return root / L"jobs" / L"interpreter";
    }

    static fs::path ResolveInterpreterProgramPath(fs::path const& root, std::wstring const& programId)
    {
        if (!IsSafeProgramId(programId))
        {
            throw WorkerProtocolError("program_id.invalid", "program_id must be 1..64 chars using letters, digits, underscore, or dash");
        }
        return InterpreterProgramDirectory(root) / (programId + L".xcpbc");
    }

    static fs::path MemoryProgramDirectory(fs::path const& root)
    {
        return root / L"jobs" / L"memory-programs";
    }

    static fs::path ResolveMemoryProgramPath(fs::path const& root, std::wstring const& programId)
    {
        if (!IsSafeProgramId(programId))
        {
            throw WorkerProtocolError("program_id.invalid", "program_id must be 1..64 chars using letters, digits, underscore, or dash");
        }
        return MemoryProgramDirectory(root) / (programId + L".xcpbc");
    }

    static void InsertJsonString(JsonObject& object, wchar_t const* name, std::wstring const& value)
    {
        object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
    }

    static void InsertJsonNumber(JsonObject& object, wchar_t const* name, uint64_t value)
    {
        object.Insert(name, JsonValue::CreateNumberValue(static_cast<double>(value)));
    }

    static void InsertJsonBool(JsonObject& object, wchar_t const* name, bool value)
    {
        object.Insert(name, JsonValue::CreateBooleanValue(value));
    }

    static uint64_t StableWideStringMix64(std::wstring const& value, uint64_t seed = 0xcbf29ce484222325ull)
    {
        uint64_t mix = seed;
        for (auto ch : value)
        {
            mix ^= static_cast<uint64_t>(ch);
            mix *= 0x100000001b3ull;
            mix = ((mix << 13) | (mix >> (64 - 13))) ^ 0x9e3779b97f4a7c15ull;
        }
        return mix;
    }

    static uint64_t WorkspaceBytes(fs::path const& root)
    {
        uint64_t total = 0;
        if (!fs::exists(root))
        {
            return 0;
        }
        for (auto const& entry : fs::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                total += static_cast<uint64_t>(entry.file_size());
            }
        }
        return total;
    }

    static uint64_t WorkspaceFileCount(fs::path const& root)
    {
        uint64_t total = 0;
        if (!fs::exists(root))
        {
            return 0;
        }
        for (auto const& entry : fs::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                ++total;
            }
        }
        return total;
    }

    static uint64_t WorkspaceDirectoryCount(fs::path const& root)
    {
        uint64_t total = 0;
        if (!fs::exists(root))
        {
            return 0;
        }
        for (auto const& entry : fs::recursive_directory_iterator(root))
        {
            if (entry.is_directory())
            {
                ++total;
            }
        }
        return total;
    }

    static uint64_t AppMemoryUsageBytes()
    {
        try
        {
            return winrt::Windows::System::MemoryManager::AppMemoryUsage();
        }
        catch (...)
        {
            return 0;
        }
    }

    static uint64_t AppMemoryUsageLimitBytes()
    {
        try
        {
            return winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
        }
        catch (...)
        {
            return 0;
        }
    }

    static uint64_t AppMemoryUsageLevelValue()
    {
        try
        {
            return static_cast<uint64_t>(winrt::Windows::System::MemoryManager::AppMemoryUsageLevel());
        }
        catch (...)
        {
            return 0;
        }
    }

    static WorkerD3DEnvironmentObservation ObserveD3DEnvironment(
        fs::path const& root)
    {
        WorkerD3DEnvironmentObservation observation;
        observation.appMemoryUsageBytes = AppMemoryUsageBytes();
        observation.appMemoryUsageLimitBytes = AppMemoryUsageLimitBytes();
        observation.appMemoryUsageLevel = AppMemoryUsageLevelValue();
        observation.workspaceBytes = WorkspaceBytes(root);
        return observation;
    }

    static CommandSearchStats SearchWorkspace(fs::path const& root, std::string const& needle)
    {
        if (needle.empty() || needle.size() > 128)
        {
            throw WorkerProtocolError("search.invalid_needle", "needle must be 1..128 bytes");
        }

        CommandSearchStats stats;
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
                if (stats.hits.size() < 20)
                {
                    std::wostringstream out;
                    out << relative << L":" << lineNumber << L":" << (column + 1) << L": " << Utf8ToWide(line);
                    stats.hits.push_back(out.str());
                }
            }
        }
        return stats;
    }

    static std::wstring JsonStringArray(std::vector<std::wstring> const& values)
    {
        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << JsonString(values[i]);
        }
        out << L"]";
        return out.str();
    }

    static std::wstring OkBase(std::wstring const& command)
    {
        return L"{\"ok\":true,\"protocol_version\":" + JsonString(WorkerProtocolVersion) + L",\"command\":" + JsonString(command);
    }

    static std::wstring BoolJson(bool value)
    {
        return value ? L"true" : L"false";
    }

    static std::wstring GetOptionalString(JsonObject const& object, wchar_t const* name, std::wstring const& fallback = L"")
    {
        if (!object.HasKey(name))
        {
            return fallback;
        }
        auto value = object.GetNamedString(name, fallback);
        return std::wstring(value.data(), value.size());
    }

    static uint64_t GetOptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
    {
        if (!object.HasKey(name))
        {
            return fallback;
        }

        auto value = object.GetNamedNumber(name);
        if (value < 0)
        {
            throw WorkerProtocolError("argument.invalid_number", "numeric argument cannot be negative");
        }
        return static_cast<uint64_t>(value);
    }

    static uint64_t GetBoundedOptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback, uint64_t minimum, uint64_t maximum)
    {
        auto value = GetOptionalUInt64(object, name, fallback);
        if (value < minimum || value > maximum)
        {
            throw WorkerProtocolError("argument.out_of_range", "numeric argument is outside the supported range");
        }
        return value;
    }

    static uint64_t GetPhysicsBoundedOptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback, uint64_t minimum, uint64_t maximum, char const* errorCode)
    {
        auto value = GetOptionalUInt64(object, name, fallback);
        if (value < minimum || value > maximum)
        {
            throw WorkerProtocolError(errorCode, "physics kernel parameter is outside the supported bounded range");
        }
        return value;
    }


    static double ElapsedMilliseconds(std::chrono::steady_clock::time_point const& started)
    {
        auto elapsed = std::chrono::steady_clock::now() - started;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

    static std::wstring DoubleJson(double value, int precision = 3)
    {
        std::wostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(precision) << value;
        return out.str();
    }

    static double GetOptionalDoubleNoThrow(JsonObject const& object, wchar_t const* name, double fallback = 0.0)
    {
        try
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            return object.GetNamedNumber(name);
        }
        catch (...)
        {
            return fallback;
        }
    }

    static uint64_t GetOptionalUInt64NoThrow(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
    {
        auto value = GetOptionalDoubleNoThrow(object, name, static_cast<double>(fallback));
        if (value < 0)
        {
            return fallback;
        }
        return static_cast<uint64_t>(value);
    }

    static std::wstring SyncServerTimingJson(
        JsonObject const& request,
        double authMs,
        double decodeBase64Ms,
        double writeFileMs,
        double responseBuildMs,
        uint64_t payloadBytes,
        uint64_t decodedBytes)
    {
        auto requestBytes = GetOptionalUInt64NoThrow(request, L"_server_request_bytes", 0);
        auto binaryReadMs = GetOptionalDoubleNoThrow(request, L"_server_binary_read_ms", 0.0);
        auto parseJsonMs = GetOptionalDoubleNoThrow(request, L"_server_parse_json_ms", 0.0);
        if (authMs <= 0.0)
        {
            authMs = GetOptionalDoubleNoThrow(request, L"_server_auth_ms", 0.0);
        }

        return L",\"server_timing_schema\":\"worker-sync-command-timing-0.1\""
            L",\"server_timing_ms\":{"
            L"\"parse_json\":" + DoubleJson(parseJsonMs, 6) +
            L",\"auth\":" + DoubleJson(authMs, 6) +
            L",\"binary_body_read\":" + DoubleJson(binaryReadMs, 6) +
            L",\"decode_base64\":" + DoubleJson(decodeBase64Ms, 6) +
            L",\"write_file\":" + DoubleJson(writeFileMs, 6) +
            L",\"response_build\":" + DoubleJson(responseBuildMs, 6) +
            L"},\"server_timing_bytes\":{"
            L"\"request\":" + std::to_wstring(requestBytes) +
            L",\"payload\":" + std::to_wstring(payloadBytes) +
            L",\"decoded\":" + std::to_wstring(decodedBytes) +
            L"}";
    }

    static DoubleSampleStats ComputeDoubleSampleStats(std::vector<double> values)
    {
        DoubleSampleStats stats;
        stats.count = static_cast<uint64_t>(values.size());
        if (values.empty())
        {
            return stats;
        }

        std::sort(values.begin(), values.end());
        stats.min = values.front();
        stats.max = values.back();
        auto const n = values.size();
        if ((n % 2) == 0)
        {
            stats.median = (values[(n / 2) - 1] + values[n / 2]) / 2.0;
        }
        else
        {
            stats.median = values[n / 2];
        }

        auto p95Index = static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.95));
        if (p95Index == 0)
        {
            p95Index = 1;
        }
        if (p95Index > n)
        {
            p95Index = n;
        }
        stats.p95 = values[p95Index - 1];

        double sum = 0.0;
        for (auto value : values)
        {
            sum += value;
        }
        stats.mean = sum / static_cast<double>(n);

        double variance = 0.0;
        for (auto value : values)
        {
            auto delta = value - stats.mean;
            variance += delta * delta;
        }
        variance /= static_cast<double>(n);
        stats.stddev = std::sqrt(variance);
        return stats;
    }

    static std::wstring DoubleSampleStatsJson(DoubleSampleStats const& stats, int precision = 6)
    {
        return L"{\"count\":" + std::to_wstring(stats.count) +
            L",\"min\":" + DoubleJson(stats.min, precision) +
            L",\"median\":" + DoubleJson(stats.median, precision) +
            L",\"p95\":" + DoubleJson(stats.p95, precision) +
            L",\"max\":" + DoubleJson(stats.max, precision) +
            L",\"mean\":" + DoubleJson(stats.mean, precision) +
            L",\"stddev\":" + DoubleJson(stats.stddev, precision) +
            L"}";
    }

    static std::wstring DoubleArrayJson(std::vector<double> const& values, int precision = 6)
    {
        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << DoubleJson(values[i], precision);
        }
        out << L"]";
        return out.str();
    }

    static std::wstring HResultToString(HRESULT hr)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
        return out.str();
    }

    static std::wstring D3DFeatureLevelToString(D3D_FEATURE_LEVEL featureLevel)
    {
        switch (featureLevel)
        {
        case D3D_FEATURE_LEVEL_12_1:
            return L"12_1";
        case D3D_FEATURE_LEVEL_12_0:
            return L"12_0";
        case D3D_FEATURE_LEVEL_11_1:
            return L"11_1";
        case D3D_FEATURE_LEVEL_11_0:
            return L"11_0";
        case D3D_FEATURE_LEVEL_10_1:
            return L"10_1";
        case D3D_FEATURE_LEVEL_10_0:
            return L"10_0";
        default:
            return L"unknown";
        }
    }

    static std::vector<uint64_t> ParseComputeElementsList(
        std::wstring const& text,
        uint64_t maxElements,
        char const* errorCode)
    {
        auto source = text.empty()
            ? std::wstring(L"1024,4096,16384,65536,262144,1048576")
            : text;

        std::vector<uint64_t> values;
        std::wstringstream input(source);
        std::wstring token;
        while (std::getline(input, token, L','))
        {
            token.erase(
                std::remove_if(token.begin(), token.end(), [](wchar_t value) { return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n'; }),
                token.end());
            if (token.empty())
            {
                continue;
            }

            try
            {
                size_t parsed = 0;
                auto value = std::stoull(token, &parsed, 10);
                if (parsed != token.size() || value < 1 || value > maxElements)
                {
                    throw std::out_of_range("elements out of range");
                }
                values.push_back(value);
            }
            catch (...)
            {
                throw WorkerProtocolError(errorCode, "elements_list must be a comma-separated list of integers in the supported range");
            }
        }

        if (values.empty() || values.size() > MaxD3D11ComputeSweepEntries)
        {
            throw WorkerProtocolError(errorCode, "elements_list must contain 1..16 entries");
        }
        return values;
    }

    static std::vector<uint64_t> ParseD3D11ComputeElementsList(std::wstring const& text)
    {
        return ParseComputeElementsList(text, MaxD3D11ComputeElements, "d3d11_compute.elements_list_invalid");
    }

    static std::vector<uint64_t> ParseD3D12Fp32TimingElementsList(std::wstring const& text)
    {
        return ParseComputeElementsList(text, MaxD3D12Fp32TimingElements, "d3d12_fp32_timing.elements_list_invalid");
    }

    static std::vector<uint64_t> ParseD3D12ShaderShapeElementsList(std::wstring const& text)
    {
        return ParseComputeElementsList(text, MaxD3D12ShaderShapeElements, "d3d12_shader_shape.elements_list_invalid");
    }

    static std::vector<D3D11ShaderMatrixVariant> D3D11ShaderMatrixCatalog()
    {
        return {
            { L"memory_copy", L"MatrixMemoryCopy.cso", L"memory", 0, 0, sizeof(D3D11Float4) * 2ull },
            { L"fp32_alu_8", L"MatrixAlu8.cso", L"fp32_alu", 8, 8ull * 4ull * 2ull, sizeof(D3D11Float4) * 2ull },
            { L"fp32_alu_32", L"MatrixAlu32.cso", L"fp32_alu", 32, 32ull * 4ull * 2ull, sizeof(D3D11Float4) * 2ull },
            { L"fp32_alu_128", L"MatrixAlu128.cso", L"fp32_alu", 128, 128ull * 4ull * 2ull, sizeof(D3D11Float4) * 2ull },
            { L"domain_hash_mix", L"DomainHashMix.cso", L"domain_hash", 0, 0, sizeof(D3D11Float4) * 2ull },
            { L"domain_image_kernel", L"DomainImageKernel.cso", L"domain_image", 12, 12ull * 24ull, sizeof(D3D11Float4) * 2ull },
            { L"domain_matrix_tile", L"DomainMatrixTile.cso", L"domain_matrix", 16, 16ull * 32ull, sizeof(D3D11Float4) * 2ull },
        };
    }

    static D3D11ShaderMatrixVariant FindD3D11ShaderMatrixVariant(std::wstring const& id)
    {
        for (auto const& variant : D3D11ShaderMatrixCatalog())
        {
            if (variant.id == id)
            {
                return variant;
            }
        }
        throw WorkerProtocolError("d3d11_shader_matrix.variant_invalid", "unknown shader matrix variant: " + WideToUtf8(id));
    }

    static std::vector<D3D11ShaderMatrixVariant> ParseD3D11ShaderMatrixVariants(std::wstring const& text)
    {
        auto source = text.empty()
            ? std::wstring(L"memory_copy,fp32_alu_8,fp32_alu_32,fp32_alu_128")
            : text;

        std::vector<D3D11ShaderMatrixVariant> variants;
        std::vector<std::wstring> seen;
        std::wstringstream input(source);
        std::wstring token;
        while (std::getline(input, token, L','))
        {
            token.erase(
                std::remove_if(token.begin(), token.end(), [](wchar_t value) { return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n'; }),
                token.end());
            if (token.empty())
            {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), token) != seen.end())
            {
                throw WorkerProtocolError("d3d11_shader_matrix.variant_duplicate", "shader matrix variants must be unique");
            }
            seen.push_back(token);
            variants.push_back(FindD3D11ShaderMatrixVariant(token));
        }

        if (variants.empty() || variants.size() > MaxD3D11ShaderMatrixVariants)
        {
            throw WorkerProtocolError("d3d11_shader_matrix.variants_invalid", "variants must contain 1..8 entries");
        }
        return variants;
    }

    static std::wstring D3D12AdapterProbeJson(D3D12AdapterProbe const& adapter)
    {
        return L"{\"index\":" + std::to_wstring(adapter.index) +
            L",\"description\":" + JsonString(adapter.description) +
            L",\"vendor_id\":" + std::to_wstring(adapter.vendorId) +
            L",\"device_id\":" + std::to_wstring(adapter.deviceId) +
            L",\"sub_sys_id\":" + std::to_wstring(adapter.subSysId) +
            L",\"revision\":" + std::to_wstring(adapter.revision) +
            L",\"dedicated_video_memory\":" + std::to_wstring(adapter.dedicatedVideoMemory) +
            L",\"dedicated_system_memory\":" + std::to_wstring(adapter.dedicatedSystemMemory) +
            L",\"shared_system_memory\":" + std::to_wstring(adapter.sharedSystemMemory) +
            L",\"flags\":" + std::to_wstring(adapter.flags) +
            L",\"software\":" + BoolJson(adapter.software) +
            L"}";
    }

    static std::wstring D3D12AdapterListJson(std::vector<D3D12AdapterProbe> const& adapters)
    {
        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << D3D12AdapterProbeJson(adapters[i]);
        }
        out << L"]";
        return out.str();
    }

    static std::wstring D3D12ComputeSweepMeasurementJson(D3D12ComputeSweepMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"total_gpu_submit_and_wait_ms\":" + DoubleJson(measurement.totalGpuSubmitAndWaitMs, 6) +
            L",\"avg_gpu_submit_and_wait_ms\":" + DoubleJson(measurement.averageGpuSubmitAndWaitMs, 6) +
            L",\"readback_verify_ms\":" + DoubleJson(measurement.readbackVerifyMs, 6) +
            L",\"elements_per_second\":" + DoubleJson(measurement.elementsPerSecond, 1) +
            L",\"mib_per_second\":" + DoubleJson(measurement.mibPerSecond, 3) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L",\"output_buffer_hresult\":" + JsonString(HResultToString(measurement.outputBufferHr)) +
            L",\"readback_buffer_hresult\":" + JsonString(HResultToString(measurement.readbackBufferHr)) +
            L",\"create_fence_hresult\":" + JsonString(HResultToString(measurement.createFenceHr)) +
            L",\"device_removed_reason\":" + JsonString(HResultToString(measurement.deviceRemovedReason)) +
            L",\"fence_value\":" + std::to_wstring(measurement.fenceValue) +
            L",\"completed_fence_value\":" + std::to_wstring(measurement.completedFenceValue) +
            L",\"fence_completed\":" + BoolJson(measurement.fenceCompleted) +
            L"}";
    }

    static std::wstring D3D12ComputeTimingMeasurementJson(D3D12ComputeTimingMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"timestamp_frequency\":" + std::to_wstring(measurement.timestampFrequency) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs, 6) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(measurement.averageGpuDispatchMs, 6) +
            L",\"total_cpu_submit_and_wait_ms\":" + DoubleJson(measurement.totalCpuSubmitAndWaitMs, 6) +
            L",\"avg_cpu_submit_and_wait_ms\":" + DoubleJson(measurement.averageCpuSubmitAndWaitMs, 6) +
            L",\"readback_verify_ms\":" + DoubleJson(measurement.readbackVerifyMs, 6) +
            L",\"gpu_elements_per_second\":" + DoubleJson(measurement.gpuElementsPerSecond, 1) +
            L",\"gpu_mib_per_second\":" + DoubleJson(measurement.gpuMiBPerSecond, 3) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L",\"output_buffer_hresult\":" + JsonString(HResultToString(measurement.outputBufferHr)) +
            L",\"readback_buffer_hresult\":" + JsonString(HResultToString(measurement.readbackBufferHr)) +
            L",\"timestamp_query_heap_hresult\":" + JsonString(HResultToString(measurement.timestampQueryHeapHr)) +
            L",\"timestamp_readback_buffer_hresult\":" + JsonString(HResultToString(measurement.timestampReadbackBufferHr)) +
            L",\"timestamp_frequency_hresult\":" + JsonString(HResultToString(measurement.timestampFrequencyHr)) +
            L",\"create_fence_hresult\":" + JsonString(HResultToString(measurement.createFenceHr)) +
            L",\"device_removed_reason\":" + JsonString(HResultToString(measurement.deviceRemovedReason)) +
            L",\"output_buffer_created\":" + BoolJson(measurement.outputBufferCreated) +
            L",\"readback_buffer_created\":" + BoolJson(measurement.readbackBufferCreated) +
            L",\"timestamp_query_heap_created\":" + BoolJson(measurement.timestampQueryHeapCreated) +
            L",\"timestamp_readback_buffer_created\":" + BoolJson(measurement.timestampReadbackBufferCreated) +
            L",\"fence_value\":" + std::to_wstring(measurement.fenceValue) +
            L",\"completed_fence_value\":" + std::to_wstring(measurement.completedFenceValue) +
            L",\"fence_completed\":" + BoolJson(measurement.fenceCompleted) +
            L"}";
    }

    static std::wstring D3D12Float4Json(D3D11Float4 const& value)
    {
        return L"{\"x\":" + DoubleJson(value.x, 6) +
            L",\"y\":" + DoubleJson(value.y, 6) +
            L",\"z\":" + DoubleJson(value.z, 6) +
            L",\"w\":" + DoubleJson(value.w, 6) +
            L"}";
    }

    static std::wstring D3D12FloatTimingMeasurementJson(D3D12FloatTimingMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(measurement.fp32OpsPerElement) +
            L",\"timestamp_frequency\":" + std::to_wstring(measurement.timestampFrequency) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"fp32_ops_timed\":" + std::to_wstring(measurement.fp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs, 6) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(measurement.averageGpuDispatchMs, 6) +
            L",\"fp32_ops_per_second\":" + DoubleJson(measurement.fp32OpsPerSecond, 1) +
            L",\"gflops\":" + DoubleJson(measurement.gflops, 3) +
            L",\"total_cpu_submit_and_wait_ms\":" + DoubleJson(measurement.totalCpuSubmitAndWaitMs, 6) +
            L",\"avg_cpu_submit_and_wait_ms\":" + DoubleJson(measurement.averageCpuSubmitAndWaitMs, 6) +
            L",\"readback_verify_ms\":" + DoubleJson(measurement.readbackVerifyMs, 6) +
            L",\"gpu_mib_per_second\":" + DoubleJson(measurement.gpuMiBPerSecond, 3) +
            L",\"first_value\":" + D3D12Float4Json(measurement.firstValue) +
            L",\"last_value\":" + D3D12Float4Json(measurement.lastValue) +
            L",\"checksum\":" + DoubleJson(measurement.checksum, 6) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"max_abs_error\":" + DoubleJson(measurement.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(measurement.maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L",\"output_buffer_hresult\":" + JsonString(HResultToString(measurement.outputBufferHr)) +
            L",\"readback_buffer_hresult\":" + JsonString(HResultToString(measurement.readbackBufferHr)) +
            L",\"timestamp_query_heap_hresult\":" + JsonString(HResultToString(measurement.timestampQueryHeapHr)) +
            L",\"timestamp_readback_buffer_hresult\":" + JsonString(HResultToString(measurement.timestampReadbackBufferHr)) +
            L",\"timestamp_frequency_hresult\":" + JsonString(HResultToString(measurement.timestampFrequencyHr)) +
            L",\"create_fence_hresult\":" + JsonString(HResultToString(measurement.createFenceHr)) +
            L",\"device_removed_reason\":" + JsonString(HResultToString(measurement.deviceRemovedReason)) +
            L",\"output_buffer_created\":" + BoolJson(measurement.outputBufferCreated) +
            L",\"readback_buffer_created\":" + BoolJson(measurement.readbackBufferCreated) +
            L",\"timestamp_query_heap_created\":" + BoolJson(measurement.timestampQueryHeapCreated) +
            L",\"timestamp_readback_buffer_created\":" + BoolJson(measurement.timestampReadbackBufferCreated) +
            L",\"fence_value\":" + std::to_wstring(measurement.fenceValue) +
            L",\"completed_fence_value\":" + std::to_wstring(measurement.completedFenceValue) +
            L",\"fence_completed\":" + BoolJson(measurement.fenceCompleted) +
            L"}";
    }

    static std::wstring D3D11ComputeMeasurementJson(D3D11ComputeMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"total_dispatch_and_readback_ms\":" + DoubleJson(measurement.totalDispatchAndReadbackMs) +
            L",\"avg_dispatch_and_readback_ms\":" + DoubleJson(measurement.averageDispatchAndReadbackMs) +
            L",\"elements_per_second\":" + DoubleJson(measurement.elementsPerSecond, 1) +
            L",\"mib_per_second\":" + DoubleJson(measurement.mibPerSecond, 3) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L"}";
    }

    static std::wstring D3D11ComputeTimingMeasurementJson(D3D11ComputeTimingMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(measurement.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(measurement.gpuDisjointCount) +
            L",\"gpu_timestamp_frequency\":" + std::to_wstring(measurement.gpuTimestampFrequency) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(measurement.averageGpuDispatchMs) +
            L",\"gpu_elements_per_second\":" + DoubleJson(measurement.gpuElementsPerSecond, 1) +
            L",\"gpu_mib_per_second\":" + DoubleJson(measurement.gpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(measurement.cpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(measurement.totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(measurement.averageCpuSubmitMs) +
            L",\"verification_readback_ms\":" + DoubleJson(measurement.verificationReadbackMs) +
            L",\"timing_error\":" + JsonString(measurement.timingError) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L"}";
    }

    static std::wstring D3D11Float4Json(D3D11Float4 const& value)
    {
        return L"{\"x\":" + DoubleJson(value.x, 6) +
            L",\"y\":" + DoubleJson(value.y, 6) +
            L",\"z\":" + DoubleJson(value.z, 6) +
            L",\"w\":" + DoubleJson(value.w, 6) +
            L"}";
    }

    static std::wstring D3D11FloatTimingMeasurementJson(D3D11FloatTimingMeasurement const& measurement)
    {
        return L"{\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(measurement.fp32OpsPerElement) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(measurement.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(measurement.gpuDisjointCount) +
            L",\"gpu_timestamp_frequency\":" + std::to_wstring(measurement.gpuTimestampFrequency) +
            L",\"fp32_ops_timed\":" + std::to_wstring(measurement.fp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(measurement.averageGpuDispatchMs) +
            L",\"fp32_ops_per_second\":" + DoubleJson(measurement.fp32OpsPerSecond, 1) +
            L",\"gflops\":" + DoubleJson(measurement.gflops, 3) +
            L",\"gpu_mib_per_second\":" + DoubleJson(measurement.gpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(measurement.cpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(measurement.totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(measurement.averageCpuSubmitMs) +
            L",\"verification_readback_ms\":" + DoubleJson(measurement.verificationReadbackMs) +
            L",\"timing_error\":" + JsonString(measurement.timingError) +
            L",\"first_value\":" + D3D11Float4Json(measurement.firstValue) +
            L",\"last_value\":" + D3D11Float4Json(measurement.lastValue) +
            L",\"checksum\":" + DoubleJson(measurement.checksum, 6) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"max_abs_error\":" + DoubleJson(measurement.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(measurement.maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L"}";
    }

    static std::wstring D3D11ShaderMatrixMeasurementJson(D3D11ShaderMatrixMeasurement const& measurement)
    {
        return L"{\"variant_id\":" + JsonString(measurement.variant.id) +
            L",\"shader\":" + JsonString(measurement.variant.shaderFileName) +
            L",\"category\":" + JsonString(measurement.variant.category) +
            L",\"loop_count\":" + std::to_wstring(measurement.variant.loopCount) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(measurement.variant.fp32OpsPerElement) +
            L",\"gpu_bytes_per_element\":" + std::to_wstring(measurement.variant.gpuBytesPerElement) +
            L",\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"input_bytes\":" + std::to_wstring(measurement.inputBytes) +
            L",\"output_bytes\":" + std::to_wstring(measurement.outputBytes) +
            L",\"gpu_bytes_per_dispatch\":" + std::to_wstring(measurement.gpuBytesPerDispatch) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"shader_bytes\":" + std::to_wstring(measurement.shaderBytes) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(measurement.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(measurement.gpuDisjointCount) +
            L",\"gpu_timestamp_frequency\":" + std::to_wstring(measurement.gpuTimestampFrequency) +
            L",\"fp32_ops_timed\":" + std::to_wstring(measurement.fp32OpsTimed) +
            L",\"gpu_bytes_timed\":" + std::to_wstring(measurement.gpuBytesTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(measurement.averageGpuDispatchMs) +
            L",\"fp32_ops_per_second\":" + DoubleJson(measurement.fp32OpsPerSecond, 1) +
            L",\"gflops\":" + DoubleJson(measurement.gflops, 3) +
            L",\"gpu_mib_per_second\":" + DoubleJson(measurement.gpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(measurement.cpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(measurement.totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(measurement.averageCpuSubmitMs) +
            L",\"verification_readback_ms\":" + DoubleJson(measurement.verificationReadbackMs) +
            L",\"timing_error\":" + JsonString(measurement.timingError) +
            L",\"first_value\":" + D3D11Float4Json(measurement.firstValue) +
            L",\"last_value\":" + D3D11Float4Json(measurement.lastValue) +
            L",\"checksum\":" + DoubleJson(measurement.checksum, 6) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"max_abs_error\":" + DoubleJson(measurement.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(measurement.maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L"}";
    }

    static std::wstring D3D11ShaderMatrixVariantSummaryJson(D3D11ShaderMatrixVariantSummary const& summary)
    {
        auto aggregateFp32OpsPerSecond = summary.totalGpuDispatchMs > 0.0
            ? static_cast<double>(summary.totalFp32OpsTimed) / (summary.totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = summary.totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(summary.totalGpuBytesTimed) / 1048576.0) / (summary.totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = summary.totalCpuSubmitSampleCount > 0
            ? summary.totalCpuSubmitMs / static_cast<double>(summary.totalCpuSubmitSampleCount)
            : 0.0;

        std::wostringstream measurementsJson;
        measurementsJson << L"[";
        for (size_t i = 0; i < summary.measurements.size(); ++i)
        {
            if (i != 0)
            {
                measurementsJson << L",";
            }
            measurementsJson << D3D11ShaderMatrixMeasurementJson(summary.measurements[i]);
        }
        measurementsJson << L"]";

        return L"{\"variant_id\":" + JsonString(summary.variant.id) +
            L",\"shader\":" + JsonString(summary.variant.shaderFileName) +
            L",\"category\":" + JsonString(summary.variant.category) +
            L",\"loop_count\":" + std::to_wstring(summary.variant.loopCount) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(summary.variant.fp32OpsPerElement) +
            L",\"gpu_bytes_per_element\":" + std::to_wstring(summary.variant.gpuBytesPerElement) +
            L",\"shader_bytes\":" + std::to_wstring(summary.shaderBytes) +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(summary.featureLevel)) +
            L",\"result_count\":" + std::to_wstring(summary.measurements.size()) +
            L",\"timestamp_query_supported\":" + BoolJson(summary.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(summary.gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(summary.totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(summary.totalGpuDisjointCount) +
            L",\"total_elements_timed\":" + std::to_wstring(summary.totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" + std::to_wstring(summary.totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(summary.totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(summary.totalGpuDispatchMs) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(summary.totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(summary.totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(summary.totalVerificationReadbackMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(summary.aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(summary.maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(summary.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(summary.maxRelativeError, 9) +
            L",\"timing_error\":" + JsonString(summary.firstTimingError) +
            L",\"verified\":" + BoolJson(summary.verified) +
            L",\"measurements\":" + measurementsJson.str() +
            L"}";
    }

    static std::wstring D3D11ShaderMatrixStatsMeasurementJson(D3D11ShaderMatrixStatsMeasurement const& measurement)
    {
        return L"{\"variant_id\":" + JsonString(measurement.variant.id) +
            L",\"shader\":" + JsonString(measurement.variant.shaderFileName) +
            L",\"category\":" + JsonString(measurement.variant.category) +
            L",\"loop_count\":" + std::to_wstring(measurement.variant.loopCount) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(measurement.variant.fp32OpsPerElement) +
            L",\"gpu_bytes_per_element\":" + std::to_wstring(measurement.variant.gpuBytesPerElement) +
            L",\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"input_bytes\":" + std::to_wstring(measurement.inputBytes) +
            L",\"output_bytes\":" + std::to_wstring(measurement.outputBytes) +
            L",\"gpu_bytes_per_dispatch\":" + std::to_wstring(measurement.gpuBytesPerDispatch) +
            L",\"repeats\":" + std::to_wstring(measurement.repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(measurement.warmupRepeats) +
            L",\"batch_count\":" + std::to_wstring(measurement.batchCount) +
            L",\"batch_pause_ms\":" + std::to_wstring(measurement.batchPauseMs) +
            L",\"shader_bytes\":" + std::to_wstring(measurement.shaderBytes) +
            L",\"sample_count\":" + std::to_wstring(measurement.gpuTimingSampleCount) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(measurement.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(measurement.gpuDisjointCount) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(measurement.cpuSubmitSampleCount) +
            L",\"fp32_ops_timed\":" + std::to_wstring(measurement.fp32OpsTimed) +
            L",\"gpu_bytes_timed\":" + std::to_wstring(measurement.gpuBytesTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(measurement.totalGpuDispatchMs) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(measurement.totalCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(measurement.totalVerificationReadbackMs) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(measurement.gpuDispatchStats, 6) +
            L",\"cpu_submit_ms_stats\":" + DoubleSampleStatsJson(measurement.cpuSubmitStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(measurement.gflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(measurement.gpuMiBPerSecondStats, 6) +
            L",\"gpu_dispatch_ms_samples\":" + DoubleArrayJson(measurement.gpuDispatchSamplesMs, 6) +
            L",\"cpu_submit_ms_samples\":" + DoubleArrayJson(measurement.cpuSubmitSamplesMs, 6) +
            L",\"gflops_samples\":" + DoubleArrayJson(measurement.gflopsSamples, 6) +
            L",\"gpu_mib_per_second_samples\":" + DoubleArrayJson(measurement.gpuMiBPerSecondSamples, 6) +
            L",\"batch_median_gpu_ms\":" + DoubleArrayJson(measurement.batchMedianGpuMs, 6) +
            L",\"first_batch_median_gpu_ms\":" + DoubleJson(measurement.firstBatchMedianGpuMs, 6) +
            L",\"last_batch_median_gpu_ms\":" + DoubleJson(measurement.lastBatchMedianGpuMs, 6) +
            L",\"drift_gpu_ms\":" + DoubleJson(measurement.driftGpuMs, 6) +
            L",\"drift_percent\":" + DoubleJson(measurement.driftPercent, 6) +
            L",\"aggregate_hash32\":" + std::to_wstring(measurement.aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(measurement.maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(measurement.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(measurement.maxRelativeError, 9) +
            L",\"timing_error\":" + JsonString(measurement.firstTimingError) +
            L",\"timestamp_query_supported\":" + BoolJson(measurement.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(measurement.gpuTimingAvailable) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L"}";
    }

    static std::wstring D3D11SustainedSoakWindowJson(D3D11SustainedSoakWindow const& window)
    {
        auto appMemoryDelta = static_cast<int64_t>(window.appMemoryUsageEndBytes) -
            static_cast<int64_t>(window.appMemoryUsageStartBytes);
        return L"{\"window_index\":" + std::to_wstring(window.index) +
            L",\"started_ms\":" + DoubleJson(window.startedMs, 3) +
            L",\"ended_ms\":" + DoubleJson(window.endedMs, 3) +
            L",\"duration_ms\":" + DoubleJson(window.durationMs, 3) +
            L",\"batch_count\":" + std::to_wstring(window.batchCount) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(window.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(window.gpuDisjointCount) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(window.cpuSubmitSampleCount) +
            L",\"elements_timed\":" + std::to_wstring(window.elementsTimed) +
            L",\"gpu_bytes_timed\":" + std::to_wstring(window.gpuBytesTimed) +
            L",\"fp32_ops_timed\":" + std::to_wstring(window.fp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(window.totalGpuDispatchMs, 6) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(window.totalCpuSubmitMs, 6) +
            L",\"total_verification_readback_ms\":" + DoubleJson(window.totalVerificationReadbackMs, 6) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(window.gpuDispatchStats, 6) +
            L",\"cpu_submit_ms_stats\":" + DoubleSampleStatsJson(window.cpuSubmitStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(window.gflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(window.gpuMiBPerSecondStats, 6) +
            L",\"app_memory_usage_start_bytes\":" + std::to_wstring(window.appMemoryUsageStartBytes) +
            L",\"app_memory_usage_end_bytes\":" + std::to_wstring(window.appMemoryUsageEndBytes) +
            L",\"app_memory_usage_delta_bytes\":" + std::to_wstring(appMemoryDelta) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(window.appMemoryUsageLimitBytes) +
            L",\"app_memory_usage_level\":" + std::to_wstring(window.appMemoryUsageLevel) +
            L",\"aggregate_hash32\":" + std::to_wstring(window.aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(window.maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(window.maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(window.maxRelativeError, 9) +
            L",\"timing_error\":" + JsonString(window.firstTimingError) +
            L",\"timestamp_query_supported\":" + BoolJson(window.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(window.gpuTimingAvailable) +
            L",\"verified\":" + BoolJson(window.verified) +
            L"}";
    }

    static std::wstring D3D11ResidentHotLoopWindowJson(D3D11ResidentHotLoopWindow const& window)
    {
        auto appMemoryDelta = static_cast<int64_t>(window.appMemoryUsageEndBytes) -
            static_cast<int64_t>(window.appMemoryUsageStartBytes);
        return L"{\"window_index\":" + std::to_wstring(window.index) +
            L",\"started_ms\":" + DoubleJson(window.startedMs, 3) +
            L",\"ended_ms\":" + DoubleJson(window.endedMs, 3) +
            L",\"duration_ms\":" + DoubleJson(window.durationMs, 3) +
            L",\"dispatch_count\":" + std::to_wstring(window.dispatchCount) +
            L",\"timed_dispatch_count\":" + std::to_wstring(window.timedDispatchCount) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(window.gpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(window.gpuDisjointCount) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(window.cpuSubmitSampleCount) +
            L",\"elements_timed\":" + std::to_wstring(window.elementsTimed) +
            L",\"gpu_bytes_timed\":" + std::to_wstring(window.gpuBytesTimed) +
            L",\"fp32_ops_timed\":" + std::to_wstring(window.fp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(window.totalGpuDispatchMs, 6) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(window.totalCpuSubmitMs, 6) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(window.gpuDispatchStats, 6) +
            L",\"cpu_submit_ms_stats\":" + DoubleSampleStatsJson(window.cpuSubmitStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(window.gflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(window.gpuMiBPerSecondStats, 6) +
            L",\"app_memory_usage_start_bytes\":" + std::to_wstring(window.appMemoryUsageStartBytes) +
            L",\"app_memory_usage_end_bytes\":" + std::to_wstring(window.appMemoryUsageEndBytes) +
            L",\"app_memory_usage_delta_bytes\":" + std::to_wstring(appMemoryDelta) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(window.appMemoryUsageLimitBytes) +
            L",\"app_memory_usage_level\":" + std::to_wstring(window.appMemoryUsageLevel) +
            L",\"timing_error\":" + JsonString(window.firstTimingError) +
            L",\"timestamp_query_supported\":" + BoolJson(window.timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(window.gpuTimingAvailable) +
            L"}";
    }

    static std::vector<uint8_t> MakeJobBytes(size_t byteCount, uint32_t seed)
    {
        std::vector<uint8_t> data(byteCount);
        uint32_t state = seed == 0 ? 0x9e3779b9u : seed;
        for (size_t i = 0; i < data.size(); ++i)
        {
            state = state * 1664525u + 1013904223u;
            data[i] = static_cast<uint8_t>((state >> 24) ^ (state >> 11) ^ i);
        }
        return data;
    }

    static std::vector<uint32_t> MakeVectorInput(size_t length, uint32_t seed)
    {
        std::vector<uint32_t> data(length);
        uint32_t state = seed == 0 ? 0x85ebca6bu : seed;
        for (size_t i = 0; i < data.size(); ++i)
        {
            state = state * 1103515245u + 12345u;
            data[i] = state ^ static_cast<uint32_t>(i + 1);
        }
        return data;
    }

    static uint32_t RotateLeft32(uint32_t value, uint32_t bits)
    {
        bits &= 31u;
        if (bits == 0)
        {
            return value;
        }
        return (value << bits) | (value >> (32u - bits));
    }

    static std::vector<uint32_t> MakeInterpreterProgram(size_t operationCount, uint32_t seed)
    {
        std::vector<uint32_t> program(operationCount);
        uint32_t state = seed == 0 ? 0xc2b2ae35u : seed;
        for (size_t i = 0; i < program.size(); ++i)
        {
            state = state * 747796405u + 2891336453u;
            auto opcode = (state >> 28) & 0x7u;
            auto dst = (state >> 25) & 0x7u;
            auto src = (state >> 22) & 0x7u;
            auto immediate = state & 0x003fffffu;
            program[i] = (opcode << 29) | (dst << 26) | (src << 23) | immediate;
        }
        return program;
    }

    static uint64_t RunInterpreterProgram(std::vector<uint32_t> const& program, uint64_t iterations, uint32_t seed)
    {
        std::array<uint32_t, 8> registers{};
        for (size_t i = 0; i < registers.size(); ++i)
        {
            registers[i] = seed ^ static_cast<uint32_t>(0x9e3779b9u * (i + 1));
        }

        uint64_t accumulator = 0;
        for (uint64_t iteration = 0; iteration < iterations; ++iteration)
        {
            registers[0] ^= static_cast<uint32_t>(iteration);
            for (auto instruction : program)
            {
                auto opcode = (instruction >> 29) & 0x7u;
                auto dst = (instruction >> 26) & 0x7u;
                auto src = (instruction >> 23) & 0x7u;
                auto immediate = instruction & 0x003fffffu;
                auto left = registers[dst];
                auto right = registers[src] + immediate;

                switch (opcode)
                {
                case 0:
                    registers[dst] = left + right;
                    break;
                case 1:
                    registers[dst] = left ^ right;
                    break;
                case 2:
                    registers[dst] = left * ((right & 0xffffu) + 1u);
                    break;
                case 3:
                    registers[dst] = RotateLeft32(left ^ immediate, (right & 31u) + 1u);
                    break;
                case 4:
                    registers[dst] = left + (registers[(src + 1u) & 0x7u] ^ RotateLeft32(right, immediate & 31u));
                    break;
                case 5:
                    registers[dst] = left - (right ^ registers[(dst + 3u) & 0x7u]);
                    break;
                case 6:
                    registers[dst] = RotateLeft32(left + registers[(src + 5u) & 0x7u], (immediate >> 5) & 31u) ^ right;
                    break;
                default:
                    registers[dst] = left + immediate + static_cast<uint32_t>(iteration);
                    break;
                }
            }
            accumulator += registers[iteration & 0x7u];
        }

        for (auto value : registers)
        {
            accumulator ^= (static_cast<uint64_t>(value) << 32) | value;
        }
        return accumulator;
    }

    static std::vector<uint8_t> Base64Decode(std::string const& encoded);

    static uint32_t InterpreterProgramHash(std::vector<uint32_t> const& program)
    {
        if (program.empty())
        {
            return 0;
        }
        return NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(program.data()),
            program.size() * sizeof(uint32_t));
    }

    static uint64_t InterpreterProgramByteCount(std::vector<uint32_t> const& program)
    {
        return static_cast<uint64_t>((4 + program.size()) * sizeof(uint32_t));
    }

    static std::vector<uint32_t> DecodeInterpreterProgramBase64(std::wstring const& value)
    {
        auto bytes = Base64Decode(WideToUtf8(value));
        if (bytes.empty() || (bytes.size() % sizeof(uint32_t)) != 0)
        {
            throw WorkerProtocolError("interpreter.bytecode_invalid", "bytecode_base64 must decode to one or more 32-bit instructions");
        }

        auto opCount = bytes.size() / sizeof(uint32_t);
        if (opCount > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("interpreter.bytecode_invalid", "uploaded bytecode op count is outside the supported range");
        }

        std::vector<uint32_t> program(opCount);
        for (size_t i = 0; i < opCount; ++i)
        {
            auto offset = i * sizeof(uint32_t);
            program[i] =
                static_cast<uint32_t>(bytes[offset]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }
        return program;
    }

    static void WriteInterpreterArtifact(fs::path const& path, uint32_t seed, std::vector<uint32_t> const& program)
    {
        if (program.empty() || program.size() > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("interpreter.program_invalid", "interpreter program op count is outside the supported range");
        }

        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw WorkerProtocolError("interpreter.store_failed", "could not open interpreter artifact for write");
        }

        std::array<uint32_t, 4> header{
            InterpreterArtifactMagic,
            InterpreterArtifactVersion,
            seed,
            static_cast<uint32_t>(program.size())
        };
        output.write(reinterpret_cast<char const*>(header.data()), static_cast<std::streamsize>(header.size() * sizeof(uint32_t)));
        output.write(reinterpret_cast<char const*>(program.data()), static_cast<std::streamsize>(program.size() * sizeof(uint32_t)));
        output.flush();
        if (!output.good())
        {
            throw WorkerProtocolError("interpreter.store_failed", "interpreter artifact write did not complete");
        }
    }

    static InterpreterArtifact ReadInterpreterArtifact(fs::path const& path, std::wstring const& programId)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("interpreter.not_found", "interpreter artifact was not found");
        }

        std::array<uint32_t, 4> header{};
        input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size() * sizeof(uint32_t)));
        if (input.gcount() != static_cast<std::streamsize>(header.size() * sizeof(uint32_t)))
        {
            throw WorkerProtocolError("interpreter.artifact_invalid", "interpreter artifact header is incomplete");
        }
        if (header[0] != InterpreterArtifactMagic || header[1] != InterpreterArtifactVersion)
        {
            throw WorkerProtocolError("interpreter.artifact_invalid", "interpreter artifact magic or version is invalid");
        }
        auto opCount = header[3];
        if (opCount == 0 || opCount > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("interpreter.artifact_invalid", "interpreter artifact op count is outside the supported range");
        }

        InterpreterArtifact artifact;
        artifact.programId = programId;
        artifact.seed = header[2];
        artifact.program.resize(opCount);
        input.read(
            reinterpret_cast<char*>(artifact.program.data()),
            static_cast<std::streamsize>(artifact.program.size() * sizeof(uint32_t)));
        if (input.gcount() != static_cast<std::streamsize>(artifact.program.size() * sizeof(uint32_t)))
        {
            throw WorkerProtocolError("interpreter.artifact_invalid", "interpreter artifact program body is incomplete");
        }
        return artifact;
    }

    static std::wstring JsonInterpreterArtifacts(fs::path const& root)
    {
        auto directory = InterpreterProgramDirectory(root);
        std::vector<std::wstring> entries;
        if (fs::exists(directory))
        {
            for (auto const& entry : fs::directory_iterator(directory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".xcpbc")
                {
                    continue;
                }
                auto programId = entry.path().stem().wstring();
                try
                {
                    auto artifact = ReadInterpreterArtifact(entry.path(), programId);
                    std::wstring item = L"{\"program_id\":" + JsonString(programId) +
                        L",\"path\":" + JsonString(entry.path().lexically_relative(root).wstring()) +
                        L",\"program_ops\":" + std::to_wstring(artifact.program.size()) +
                        L",\"seed\":" + std::to_wstring(artifact.seed) +
                        L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(artifact.program)) +
                        L",\"bytecode_hash32\":" + std::to_wstring(InterpreterProgramHash(artifact.program)) +
                        L"}";
                    entries.push_back(std::move(item));
                }
                catch (...)
                {
                }
            }
        }
        std::sort(entries.begin(), entries.end());

        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << entries[i];
        }
        out << L"]";
        return out.str();
    }

    static void WriteMemoryProgramArtifact(fs::path const& path, uint32_t seed, std::vector<uint32_t> const& program)
    {
        if (program.empty() || program.size() > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("memory_program.program_invalid", "memory program op count is outside the supported range");
        }

        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw WorkerProtocolError("memory_program.store_failed", "could not open memory program artifact for write");
        }

        std::array<uint32_t, 4> header{
            MemoryProgramArtifactMagic,
            InterpreterArtifactVersion,
            seed,
            static_cast<uint32_t>(program.size())
        };
        output.write(reinterpret_cast<char const*>(header.data()), static_cast<std::streamsize>(header.size() * sizeof(uint32_t)));
        output.write(reinterpret_cast<char const*>(program.data()), static_cast<std::streamsize>(program.size() * sizeof(uint32_t)));
        output.flush();
        if (!output.good())
        {
            throw WorkerProtocolError("memory_program.store_failed", "memory program artifact write did not complete");
        }
    }

    static InterpreterArtifact ReadMemoryProgramArtifact(fs::path const& path, std::wstring const& programId)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("memory_program.not_found", "memory program artifact was not found");
        }

        std::array<uint32_t, 4> header{};
        input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size() * sizeof(uint32_t)));
        if (input.gcount() != static_cast<std::streamsize>(header.size() * sizeof(uint32_t)))
        {
            throw WorkerProtocolError("memory_program.artifact_invalid", "memory program artifact header is incomplete");
        }
        if (header[0] != MemoryProgramArtifactMagic || header[1] != InterpreterArtifactVersion)
        {
            throw WorkerProtocolError("memory_program.artifact_invalid", "memory program artifact magic or version is invalid");
        }
        auto opCount = header[3];
        if (opCount == 0 || opCount > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("memory_program.artifact_invalid", "memory program artifact op count is outside the supported range");
        }

        InterpreterArtifact artifact;
        artifact.programId = programId;
        artifact.seed = header[2];
        artifact.program.resize(opCount);
        input.read(
            reinterpret_cast<char*>(artifact.program.data()),
            static_cast<std::streamsize>(artifact.program.size() * sizeof(uint32_t)));
        if (input.gcount() != static_cast<std::streamsize>(artifact.program.size() * sizeof(uint32_t)))
        {
            throw WorkerProtocolError("memory_program.artifact_invalid", "memory program artifact body is incomplete");
        }
        return artifact;
    }

    static std::wstring JsonMemoryProgramArtifacts(fs::path const& root)
    {
        auto directory = MemoryProgramDirectory(root);
        std::vector<std::wstring> entries;
        if (fs::exists(directory))
        {
            for (auto const& entry : fs::directory_iterator(directory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".xcpbc")
                {
                    continue;
                }
                auto programId = entry.path().stem().wstring();
                try
                {
                    auto artifact = ReadMemoryProgramArtifact(entry.path(), programId);
                    std::wstring item = L"{\"program_id\":" + JsonString(programId) +
                        L",\"path\":" + JsonString(entry.path().lexically_relative(root).wstring()) +
                        L",\"program_ops\":" + std::to_wstring(artifact.program.size()) +
                        L",\"seed\":" + std::to_wstring(artifact.seed) +
                        L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(artifact.program)) +
                        L",\"bytecode_hash32\":" + std::to_wstring(InterpreterProgramHash(artifact.program)) +
                        L"}";
                    entries.push_back(std::move(item));
                }
                catch (...)
                {
                }
            }
        }
        std::sort(entries.begin(), entries.end());

        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << entries[i];
        }
        out << L"]";
        return out.str();
    }

    static std::string Base64Encode(uint8_t const* data, size_t length)
    {
        static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve(((length + 2) / 3) * 4);

        for (size_t i = 0; i < length; i += 3)
        {
            uint32_t value = static_cast<uint32_t>(data[i]) << 16;
            bool hasSecond = i + 1 < length;
            bool hasThird = i + 2 < length;
            if (hasSecond)
            {
                value |= static_cast<uint32_t>(data[i + 1]) << 8;
            }
            if (hasThird)
            {
                value |= static_cast<uint32_t>(data[i + 2]);
            }

            encoded.push_back(Alphabet[(value >> 18) & 0x3F]);
            encoded.push_back(Alphabet[(value >> 12) & 0x3F]);
            encoded.push_back(hasSecond ? Alphabet[(value >> 6) & 0x3F] : '=');
            encoded.push_back(hasThird ? Alphabet[value & 0x3F] : '=');
        }

        return encoded;
    }

    static int Base64Value(char value)
    {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    }

    static std::vector<uint8_t> Base64Decode(std::string const& encoded)
    {
        std::vector<uint8_t> output;
        int bitBuffer = 0;
        int bitCount = -8;
        for (char c : encoded)
        {
            if (c == '=')
            {
                break;
            }
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
            {
                continue;
            }

            auto value = Base64Value(c);
            if (value < 0)
            {
                throw WorkerProtocolError("base64.invalid", "data_base64 contains an invalid character");
            }

            bitBuffer = (bitBuffer << 6) | value;
            bitCount += 6;
            if (bitCount >= 0)
            {
                output.push_back(static_cast<uint8_t>((bitBuffer >> bitCount) & 0xFF));
                bitCount -= 8;
            }
        }

        return output;
    }

    static uint32_t HashBytes(std::vector<uint8_t> const& bytes)
    {
        if (bytes.empty())
        {
            return 0;
        }
        return NativeStatic::Hash32(bytes.data(), bytes.size());
    }

    static MemoryInterpreterResult RunMemoryInterpreterProgram(
        std::vector<uint32_t> const& program,
        std::vector<uint8_t> const& input,
        uint64_t iterations,
        uint32_t seed,
        uint64_t memoryBytes,
        uint64_t outputBytes,
        std::atomic_bool const* cancelRequested = nullptr)
    {
        if (program.empty() || program.size() > MaxInterpreterProgramOps)
        {
            throw WorkerProtocolError("memory_interpreter.program_invalid", "memory interpreter program op count is outside the supported range");
        }
        if (input.size() > MaxMemoryInterpreterInputBytes)
        {
            throw WorkerProtocolError("memory_interpreter.input_too_large", "input_base64 exceeds memory interpreter input limit");
        }
        if (memoryBytes == 0 || memoryBytes > MaxMemoryInterpreterBytes)
        {
            throw WorkerProtocolError("memory_interpreter.memory_invalid", "memory_bytes is outside the supported range");
        }
        if (memoryBytes < input.size())
        {
            throw WorkerProtocolError("memory_interpreter.memory_too_small", "memory_bytes must be at least input byte length");
        }
        if (outputBytes == 0 || outputBytes > MaxMemoryInterpreterOutputBytes)
        {
            throw WorkerProtocolError("memory_interpreter.output_invalid", "output_bytes is outside the supported range");
        }

        std::vector<uint8_t> memory(static_cast<size_t>(memoryBytes));
        std::copy(input.begin(), input.end(), memory.begin());
        uint32_t state = seed == 0 ? 0x27d4eb2du : seed;
        for (size_t i = input.size(); i < memory.size(); ++i)
        {
            state = state * 1664525u + 1013904223u;
            memory[i] = static_cast<uint8_t>((state >> 24) ^ i);
        }

        MemoryInterpreterResult result;
        result.output.resize(static_cast<size_t>(outputBytes));

        std::array<uint32_t, 8> registers{};
        registers[0] = seed;
        registers[1] = static_cast<uint32_t>(input.size());
        registers[2] = static_cast<uint32_t>(memory.size());
        registers[3] = static_cast<uint32_t>(result.output.size());
        registers[4] = HashBytes(input);
        registers[5] = 0x9e3779b9u;
        registers[6] = 0x85ebca6bu;
        registers[7] = 0xc2b2ae35u;

        for (uint64_t iteration = 0; iteration < iterations; ++iteration)
        {
            if (cancelRequested != nullptr && ((iteration & 0x3ffull) == 0) && cancelRequested->load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }
            registers[0] ^= static_cast<uint32_t>(iteration);
            for (auto instruction : program)
            {
                auto opcode = (instruction >> 29) & 0x7u;
                auto dst = (instruction >> 26) & 0x7u;
                auto src = (instruction >> 23) & 0x7u;
                auto immediate = instruction & 0x003fffffu;
                auto memoryIndex = static_cast<size_t>((static_cast<uint64_t>(registers[src]) + immediate + iteration) % memory.size());
                auto outputIndex = static_cast<size_t>((static_cast<uint64_t>(registers[src]) + immediate + iteration) % result.output.size());
                auto memoryValue = static_cast<uint32_t>(memory[memoryIndex]);

                switch (opcode)
                {
                case 0: // add
                    registers[dst] = registers[dst] + registers[src] + immediate;
                    break;
                case 1: // xor
                    registers[dst] = registers[dst] ^ registers[src] ^ immediate;
                    break;
                case 2: // load
                    registers[dst] = memoryValue;
                    break;
                case 3: // store
                    memory[memoryIndex] = static_cast<uint8_t>(registers[dst] & 0xffu);
                    break;
                case 4: // addm
                    registers[dst] = registers[dst] + memoryValue + immediate;
                    break;
                case 5: // xorm
                    registers[dst] = registers[dst] ^ (memoryValue + (immediate & 0xffu));
                    break;
                case 6: // rotl
                    registers[dst] = RotateLeft32(registers[dst] ^ registers[src], immediate & 31u);
                    break;
                default: // out
                    result.output[outputIndex] = static_cast<uint8_t>((registers[dst] ^ memoryValue ^ immediate) & 0xffu);
                    break;
                }

                result.accumulator += registers[dst] ^ memory[memoryIndex] ^ static_cast<uint32_t>(iteration);
            }
        }

        result.memoryHash = HashBytes(memory);
        result.outputHash = HashBytes(result.output);
        for (auto value : registers)
        {
            result.accumulator ^= (static_cast<uint64_t>(value) << 32) | value;
        }
        return result;
    }

    struct Sha256
    {
        std::array<uint8_t, 64> data{};
        std::array<uint32_t, 8> state{
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        uint32_t dataLength = 0;
        uint64_t bitLength = 0;

        static uint32_t RotateRight(uint32_t value, uint32_t bits)
        {
            return (value >> bits) | (value << (32 - bits));
        }

        void Transform(uint8_t const block[64])
        {
            static constexpr std::array<uint32_t, 64> K{
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
            };

            std::array<uint32_t, 64> m{};
            for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4)
            {
                m[i] = (static_cast<uint32_t>(block[j]) << 24) |
                    (static_cast<uint32_t>(block[j + 1]) << 16) |
                    (static_cast<uint32_t>(block[j + 2]) << 8) |
                    static_cast<uint32_t>(block[j + 3]);
            }
            for (uint32_t i = 16; i < 64; ++i)
            {
                auto s0 = RotateRight(m[i - 15], 7) ^ RotateRight(m[i - 15], 18) ^ (m[i - 15] >> 3);
                auto s1 = RotateRight(m[i - 2], 17) ^ RotateRight(m[i - 2], 19) ^ (m[i - 2] >> 10);
                m[i] = m[i - 16] + s0 + m[i - 7] + s1;
            }

            auto a = state[0];
            auto b = state[1];
            auto c = state[2];
            auto d = state[3];
            auto e = state[4];
            auto f = state[5];
            auto g = state[6];
            auto h = state[7];

            for (uint32_t i = 0; i < 64; ++i)
            {
                auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                auto ch = (e & f) ^ (~e & g);
                auto temp1 = h + s1 + ch + K[i] + m[i];
                auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                auto maj = (a & b) ^ (a & c) ^ (b & c);
                auto temp2 = s0 + maj;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }

        void Update(uint8_t const* input, size_t length)
        {
            for (size_t i = 0; i < length; ++i)
            {
                data[dataLength++] = input[i];
                if (dataLength == 64)
                {
                    Transform(data.data());
                    bitLength += 512;
                    dataLength = 0;
                }
            }
        }

        std::wstring FinalHex()
        {
            auto i = dataLength;
            data[i++] = 0x80;
            if (i > 56)
            {
                while (i < 64)
                {
                    data[i++] = 0x00;
                }
                Transform(data.data());
                data.fill(0);
            }
            while (i < 56)
            {
                data[i++] = 0x00;
            }

            bitLength += static_cast<uint64_t>(dataLength) * 8;
            for (uint32_t j = 0; j < 8; ++j)
            {
                data[63 - j] = static_cast<uint8_t>((bitLength >> (j * 8)) & 0xFF);
            }
            Transform(data.data());

            std::wostringstream out;
            out << std::hex << std::setfill(L'0');
            for (auto value : state)
            {
                out << std::setw(8) << value;
            }
            return out.str();
        }
    };

    static std::wstring Sha256File(fs::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("file.read_failed", "could not open file for hash");
        }

        Sha256 sha;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (input)
        {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto read = input.gcount();
            if (read > 0)
            {
                sha.Update(buffer.data(), static_cast<size_t>(read));
            }
        }
        return sha.FinalHex();
    }

    static std::wstring Sha256BytesHex(std::vector<uint8_t> const& bytes)
    {
        Sha256 sha;
        if (!bytes.empty())
        {
            sha.Update(bytes.data(), bytes.size());
        }
        return sha.FinalHex();
    }

    static std::wstring Sha256TextHex(std::string const& text)
    {
        Sha256 sha;
        if (!text.empty())
        {
            sha.Update(reinterpret_cast<uint8_t const*>(text.data()), text.size());
        }
        return sha.FinalHex();
    }

    static bool IsSafeIdentifier(std::wstring const& value, size_t maxLength)
    {
        if (value.empty() || value.size() > maxLength)
        {
            return false;
        }
        for (auto ch : value)
        {
            bool ok = (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'-' ||
                ch == L'_';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    static int64_t CurrentUnixSeconds()
    {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static fs::path TrustedControllerDirectory(fs::path const& root)
    {
        return root / L"trust" / L"controllers";
    }

    static fs::path TrustNonceDirectory(fs::path const& root)
    {
        return root / L"trust" / L"nonces";
    }

    static fs::path TrustedControllerPath(fs::path const& root, std::wstring const& controllerId)
    {
        if (!IsSafeIdentifier(controllerId, 64))
        {
            throw WorkerProtocolError("trust.controller_id_invalid", "controller_id must use 1..64 ASCII letters, digits, '-' or '_'");
        }
        return TrustedControllerDirectory(root) / (controllerId + L".json");
    }

    static fs::path TrustNoncePath(fs::path const& root, std::wstring const& controllerId, std::wstring const& nonce)
    {
        auto nonceKey = WideToUtf8(controllerId + L"\n" + nonce);
        return TrustNonceDirectory(root) / (Sha256TextHex(nonceKey) + L".json");
    }

    static uint64_t CountTrustedControllers(fs::path const& root)
    {
        auto directory = TrustedControllerDirectory(root);
        if (!fs::exists(directory))
        {
            return 0;
        }

        uint64_t count = 0;
        for (auto const& entry : fs::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".json")
            {
                ++count;
            }
        }
        return count;
    }

    static void PruneTrustNonces(fs::path const& root)
    {
        auto directory = TrustNonceDirectory(root);
        if (!fs::exists(directory))
        {
            return;
        }

        auto now = fs::file_time_type::clock::now();
        for (auto const& entry : fs::directory_iterator(directory))
        {
            try
            {
                if (entry.is_regular_file() &&
                    now - entry.last_write_time() > std::chrono::seconds(MaxTrustNonceAgeSeconds))
                {
                    fs::remove(entry.path());
                }
            }
            catch (...)
            {
            }
        }
    }

    static bool VerifyControllerSignature(
        std::wstring const& publicKeyBase64,
        std::wstring const& canonicalMessage,
        std::wstring const& signatureBase64)
    {
        try
        {
            auto provider = AsymmetricKeyAlgorithmProvider::OpenAlgorithm(AsymmetricAlgorithmNames::RsaSignPkcs1Sha256());
            auto publicKeyBuffer = CryptographicBuffer::DecodeFromBase64String(publicKeyBase64);
            auto key = provider.ImportPublicKey(publicKeyBuffer, CryptographicPublicKeyBlobType::X509SubjectPublicKeyInfo);
            auto messageBuffer = CryptographicBuffer::ConvertStringToBinary(canonicalMessage, BinaryStringEncoding::Utf8);
            auto signatureBuffer = CryptographicBuffer::DecodeFromBase64String(signatureBase64);
            return CryptographicEngine::VerifySignature(key, messageBuffer, signatureBuffer);
        }
        catch (...)
        {
            return false;
        }
    }

    static std::wstring JsonTreeEntries(std::vector<TreeEntry> const& entries)
    {
        std::wostringstream out;
        out << L"[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto const& entry = entries[i];
            if (i != 0)
            {
                out << L",";
            }
            out << L"{\"path\":" << JsonString(entry.path)
                << L",\"relative_path\":" << JsonString(entry.relativePath)
                << L",\"bytes\":" << entry.size;
            if (!entry.sha256.empty())
            {
                out << L",\"sha256\":" << JsonString(entry.sha256);
            }
            out << L"}";
        }
        out << L"]";
        return out.str();
    }

    static fs::path ResolveOptionalWorkspacePath(fs::path const& root, std::wstring relative)
    {
        if (relative.empty())
        {
            return root;
        }
        return ResolveWorkspacePath(root, std::move(relative));
    }

    static void WriteBinaryChunk(
        fs::path const& path,
        uint64_t offset,
        std::vector<uint8_t> const& data,
        bool truncate,
        size_t maxBytes,
        std::string const& tooLargeCode,
        std::string const& tooLargeMessage);

    static std::wstring ToLowerAscii(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            if (ch >= L'A' && ch <= L'Z')
            {
                return static_cast<wchar_t>(ch - L'A' + L'a');
            }
            return ch;
        });
        return value;
    }

    static bool IsSha256Hex(std::wstring const& value)
    {
        if (value.size() != 64)
        {
            return false;
        }
        for (auto ch : value)
        {
            bool ok = (ch >= L'0' && ch <= L'9') ||
                (ch >= L'a' && ch <= L'f') ||
                (ch >= L'A' && ch <= L'F');
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    static std::wstring NormalizeOptionalSha256(std::wstring const& value)
    {
        if (value.empty())
        {
            return L"";
        }
        if (!IsSha256Hex(value))
        {
            throw WorkerProtocolError("artifact.sha256_invalid", "expected_sha256 must be 64 hex characters");
        }
        return ToLowerAscii(value);
    }

    static std::wstring NormalizeArtifactId(std::wstring const& value, bool required)
    {
        auto id = value;
        if (id.empty())
        {
            if (required)
            {
                throw WorkerProtocolError("artifact_id.required", "artifact_id is required");
            }
            auto suffix = GenerateSessionId();
            if (suffix.size() > 16)
            {
                suffix.resize(16);
            }
            id = L"artifact-" + suffix;
        }
        if (!IsSafeProgramId(id))
        {
            throw WorkerProtocolError("artifact_id.invalid", "artifact_id must be 1..64 chars using letters, digits, underscore, or dash");
        }
        return id;
    }

    static uint64_t SaturatingAdd(uint64_t left, uint64_t right)
    {
        if (UINT64_MAX - left < right)
        {
            return UINT64_MAX;
        }
        return left + right;
    }

    static uint32_t NextXorShift32(uint32_t& state)
    {
        if (state == 0)
        {
            state = 0x6d2b79f5u;
        }
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    static WorkerArtifactStoreConfig ArtifactStoreConfig();

    static PublishedArtifactResult PublishUtf8ArtifactPayload(
        JsonObject const& request,
        fs::path const& root,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields)
    {
        return WorkerPublishUtf8ArtifactPayload(
            request,
            root,
            defaultArtifactId,
            defaultArtifactKind,
            payloadUtf8,
            extraManifestFields,
            ArtifactStoreConfig(),
            [] { return GenerateSessionId(); });
    }

    static WorkerArtifactStoreConfig ArtifactStoreConfig()
    {
        WorkerArtifactStoreConfig config;
        config.protocolVersion = WorkerProtocolVersion;
        config.artifactUploadSchemaVersion = ArtifactUploadSchemaVersion;
        config.artifactBinaryFramingSchemaVersion = ArtifactBinaryFramingSchemaVersion;
        config.maxArtifactJsonChunkBytes = MaxArtifactJsonChunkBytes;
        config.maxStreamWriteBytes = MaxStreamWriteBytes;
        config.maxArtifactExpectedBytes = MaxArtifactExpectedBytes;
        config.defaultArtifactRollbackHeadroomBytes = DefaultArtifactRollbackHeadroomBytes;
        config.defaultArtifactWorkspaceBudgetBytes = DefaultArtifactWorkspaceBudgetBytes;
        config.maxArtifactWorkspaceBudgetBytes = MaxArtifactWorkspaceBudgetBytes;
        return config;
    }

    static WorkerArtifactPublicationConfig ArtifactPublicationConfig()
    {
        WorkerArtifactPublicationConfig config;
        config.protocolVersion = WorkerProtocolVersion;
        config.artifactUploadSchemaVersion = ArtifactUploadSchemaVersion;
        config.jobResultArtifactSchemaVersion = JobResultArtifactSchemaVersion;
        config.artifactStore = ArtifactStoreConfig();
        config.artifactIdGenerator = [] { return GenerateSessionId(); };
        return config;
    }

    static std::wstring ExecuteArtifactQuotaPreflight(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteArtifactQuotaPreflight(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteBeginArtifactUpload(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteBeginArtifactUpload(request, root, ArtifactStoreConfig(), [] { return GenerateSessionId(); });
    }

    static std::wstring ExecuteAppendArtifactChunk(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteAppendArtifactChunk(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteAppendArtifactChunkBinary(
        JsonObject const& request,
        fs::path const& root,
        std::vector<uint8_t> const& data,
        double authMs)
    {
        return WorkerExecuteAppendArtifactChunkBinary(request, root, data, authMs, ArtifactStoreConfig());
    }

    static std::wstring ExecuteCommitArtifactUpload(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteCommitArtifactUpload(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteGenerateArtifactDataset(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteGenerateArtifactDataset(request, root, ArtifactStoreConfig(), [] { return GenerateSessionId(); });
    }

    static std::wstring ExecuteAbortArtifactUpload(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteAbortArtifactUpload(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteDeleteArtifact(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteDeleteArtifact(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteReapArtifacts(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteReapArtifacts(request, root, ArtifactStoreConfig());
    }

    static std::wstring ExecuteGetArtifactStatus(JsonObject const& request, fs::path const& root)
    {
        return WorkerExecuteGetArtifactStatus(request, root, ArtifactStoreConfig());
    }
    static std::wstring UInt64Hex(uint64_t value)
    {
        std::wostringstream out;
        out << std::hex << std::setfill(L'0') << std::setw(16) << value;
        return out.str();
    }

    static uint64_t RotateLeft64(uint64_t value, unsigned int bits)
    {
        bits &= 63;
        if (bits == 0)
        {
            return value;
        }
        return (value << bits) | (value >> (64 - bits));
    }

    static std::wstring UInt64VectorJson(std::vector<uint64_t> const& values)
    {
        std::wstring json = L"[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                json += L",";
            }
            json += std::to_wstring(values[i]);
        }
        json += L"]";
        return json;
    }

    static std::wstring StringVectorJson(std::vector<std::wstring> const& values)
    {
        std::wstring json = L"[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                json += L",";
            }
            json += JsonString(values[i]);
        }
        json += L"]";
        return json;
    }

    static std::wstring PhysicsFrameHashesJson(std::vector<std::pair<uint64_t, std::wstring>> const& values)
    {
        std::wstring json = L"[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                json += L",";
            }
            json += L"{\"step\":" + std::to_wstring(values[i].first) +
                L",\"grid_sha256\":" + JsonString(values[i].second) +
                L"}";
        }
        json += L"]";
        return json;
    }

    static std::wstring ExecuteRunPhysicsKernelJob(JsonObject const& request, fs::path const& root)
    {
        auto kernelId = GetOptionalString(request, L"kernel_id", L"cellular_material_spread_v1");
        if (kernelId != L"cellular_material_spread_v1")
        {
            throw WorkerProtocolError("physics.kernel_id_invalid", "physics kernel_id is not implemented by the bounded public-UWP runtime");
        }

        auto width = GetPhysicsBoundedOptionalUInt64(request, L"width", 256, 16, MaxPhysicsGridWidth, "physics.width_out_of_range");
        auto height = GetPhysicsBoundedOptionalUInt64(request, L"height", 144, 16, MaxPhysicsGridHeight, "physics.height_out_of_range");
        auto states = GetPhysicsBoundedOptionalUInt64(request, L"states", 6, 2, 8, "physics.states_out_of_range");
        auto steps = GetPhysicsBoundedOptionalUInt64(request, L"steps", 256, 1, MaxPhysicsSteps, "physics.steps_out_of_range");
        auto seed = static_cast<uint32_t>(GetPhysicsBoundedOptionalUInt64(request, L"seed", 119, 1, UINT32_MAX, "physics.seed_out_of_range"));
        auto visualWidth = GetPhysicsBoundedOptionalUInt64(request, L"visual_width", 48, 8, 96, "physics.visual_width_out_of_range");
        auto visualHeight = GetPhysicsBoundedOptionalUInt64(request, L"visual_height", 24, 8, 64, "physics.visual_height_out_of_range");

        auto cellCount = width * height;
        if (cellCount == 0 || cellCount > MaxPhysicsCellCount)
        {
            throw WorkerProtocolError("physics.cell_count_out_of_range", "physics grid exceeds the supported cell count");
        }
        auto requestedCellUpdates = SaturatingAdd(0, cellCount * steps);
        if (requestedCellUpdates > MaxPhysicsCellUpdates)
        {
            throw WorkerProtocolError("physics.work_budget_exceeded", "physics cell_updates exceeds the bounded live-demo budget");
        }

        auto started = std::chrono::steady_clock::now();
        auto widthSize = static_cast<size_t>(width);
        auto heightSize = static_cast<size_t>(height);
        auto cellsSize = static_cast<size_t>(cellCount);
        auto statesSize = static_cast<size_t>(states);
        std::vector<uint8_t> grid(cellsSize, 0);
        std::vector<uint8_t> next(cellsSize, 0);
        uint32_t rng = seed;

        auto indexOf = [widthSize](size_t x, size_t y) {
            return y * widthSize + x;
        };
        auto centerX = widthSize / 2;
        auto centerY = heightSize / 2;
        auto nucleusRadius = (std::min<size_t>)(widthSize, heightSize) / 10;
        if (nucleusRadius < 3)
        {
            nucleusRadius = 3;
        }

        for (size_t y = 0; y < heightSize; ++y)
        {
            for (size_t x = 0; x < widthSize; ++x)
            {
                auto r = NextXorShift32(rng);
                auto dx = x > centerX ? x - centerX : centerX - x;
                auto dy = y > centerY ? y - centerY : centerY - y;
                auto distance = dx + dy;
                uint8_t state = 0;
                if (distance <= nucleusRadius)
                {
                    state = static_cast<uint8_t>(1 + (r % (states - 1)));
                }
                else if ((r & 0x3Fu) == 0u)
                {
                    state = static_cast<uint8_t>(1 + ((r >> 8) % (states - 1)));
                }
                grid[indexOf(x, y)] = state;
            }
        }

        std::vector<uint64_t> sampleSteps;
        sampleSteps.push_back(0);
        auto middleStep = steps / 2;
        if (middleStep != 0 && middleStep != steps)
        {
            sampleSteps.push_back(middleStep);
        }
        sampleSteps.push_back(steps);
        std::sort(sampleSteps.begin(), sampleSteps.end());
        sampleSteps.erase(std::unique(sampleSteps.begin(), sampleSteps.end()), sampleSteps.end());

        std::vector<std::pair<uint64_t, std::wstring>> frameHashes;
        frameHashes.emplace_back(0, Sha256BytesHex(grid));
        uint64_t stateChanges = 0;
        uint64_t transitionMix = 0x6d1190b5755f2369ull ^ static_cast<uint64_t>(seed);

        auto shouldSample = [&sampleSteps](uint64_t step) {
            return std::find(sampleSteps.begin(), sampleSteps.end(), step) != sampleSteps.end();
        };

        for (uint64_t step = 1; step <= steps; ++step)
        {
            for (size_t y = 0; y < heightSize; ++y)
            {
                for (size_t x = 0; x < widthSize; ++x)
                {
                    std::array<uint16_t, 8> counts{};
                    auto visit = [&](size_t nx, size_t ny) {
                        auto value = grid[indexOf(nx, ny)];
                        if (value != 0 && value < counts.size())
                        {
                            ++counts[value];
                        }
                    };

                    if (x > 0) { visit(x - 1, y); }
                    if (x + 1 < widthSize) { visit(x + 1, y); }
                    if (y > 0) { visit(x, y - 1); }
                    if (y + 1 < heightSize) { visit(x, y + 1); }
                    if (x > 0 && y > 0) { visit(x - 1, y - 1); }
                    if (x + 1 < widthSize && y > 0) { visit(x + 1, y - 1); }
                    if (x > 0 && y + 1 < heightSize) { visit(x - 1, y + 1); }
                    if (x + 1 < widthSize && y + 1 < heightSize) { visit(x + 1, y + 1); }

                    uint8_t dominantState = 0;
                    uint16_t dominantCount = 0;
                    for (uint8_t candidate = 1; candidate < states; ++candidate)
                    {
                        auto count = counts[candidate];
                        if (count > dominantCount)
                        {
                            dominantCount = count;
                            dominantState = candidate;
                        }
                    }

                    auto oldState = grid[indexOf(x, y)];
                    auto noise = NextXorShift32(rng);
                    uint8_t newState = oldState;
                    if (oldState == 0)
                    {
                        if (dominantState != 0)
                        {
                            auto threshold = static_cast<uint32_t>(dominantCount * 13u + ((x + y + step) & 7u));
                            if ((noise % 64u) < threshold)
                            {
                                newState = dominantState;
                            }
                        }
                    }
                    else if (dominantState != 0 && dominantState != oldState && dominantCount >= 3 && ((noise >> 8) & 3u) == 0u)
                    {
                        newState = dominantState;
                    }
                    else if (((noise >> 16) & 63u) == 0u)
                    {
                        newState = static_cast<uint8_t>(1 + (oldState % (states - 1)));
                    }

                    if (newState >= states)
                    {
                        newState = static_cast<uint8_t>(states - 1);
                    }
                    next[indexOf(x, y)] = newState;
                    if (newState != oldState)
                    {
                        ++stateChanges;
                        auto mixed = (static_cast<uint64_t>(oldState) << 56) ^
                            (static_cast<uint64_t>(newState) << 48) ^
                            (static_cast<uint64_t>(x) << 24) ^
                            (static_cast<uint64_t>(y) << 8) ^
                            step;
                        transitionMix = RotateLeft64(transitionMix ^ mixed, 11) * 0x9e3779b185ebca87ull;
                    }
                }
            }
            grid.swap(next);
            std::fill(next.begin(), next.end(), uint8_t{ 0 });
            if (shouldSample(step))
            {
                frameHashes.emplace_back(step, Sha256BytesHex(grid));
            }
        }

        std::vector<uint64_t> histogram(statesSize, 0);
        uint64_t frontierCount = 0;
        uint64_t activeCells = 0;
        bool stateIdsBounded = true;
        for (size_t y = 0; y < heightSize; ++y)
        {
            for (size_t x = 0; x < widthSize; ++x)
            {
                auto value = grid[indexOf(x, y)];
                if (value >= states)
                {
                    stateIdsBounded = false;
                    continue;
                }
                ++histogram[value];
                if (value == 0)
                {
                    continue;
                }
                ++activeCells;
                bool touchesEmpty = false;
                if (x == 0 || grid[indexOf(x - 1, y)] == 0) { touchesEmpty = true; }
                if (x + 1 == widthSize || grid[indexOf(x + 1, y)] == 0) { touchesEmpty = true; }
                if (y == 0 || grid[indexOf(x, y - 1)] == 0) { touchesEmpty = true; }
                if (y + 1 == heightSize || grid[indexOf(x, y + 1)] == 0) { touchesEmpty = true; }
                if (touchesEmpty)
                {
                    ++frontierCount;
                }
            }
        }

        std::vector<std::wstring> visualRows;
        auto vw = static_cast<size_t>((std::min<uint64_t>)(visualWidth, width));
        auto vh = static_cast<size_t>((std::min<uint64_t>)(visualHeight, height));
        wchar_t const* palette = L".1234567";
        for (size_t row = 0; row < vh; ++row)
        {
            auto y = (row * heightSize) / vh;
            std::wstring line;
            line.reserve(vw);
            for (size_t col = 0; col < vw; ++col)
            {
                auto x = (col * widthSize) / vw;
                auto value = grid[indexOf(x, y)];
                line.push_back(palette[(std::min<size_t>)(value, 7)]);
            }
            visualRows.push_back(line);
        }

        auto finalGridSha256 = Sha256BytesHex(grid);
        auto transitionTableHash = Sha256TextHex("cellular_material_spread_v1:moore8:dominant-frontier:bounded-v1");
        auto resultDigest = Sha256TextHex(WideToUtf8(
            kernelId + L":" +
            std::to_wstring(width) + L"x" + std::to_wstring(height) + L":" +
            std::to_wstring(states) + L":" +
            std::to_wstring(steps) + L":" +
            std::to_wstring(seed) + L":" +
            finalGridSha256 + L":" +
            UInt64Hex(transitionMix)));
        auto elapsedMs = ElapsedMilliseconds(started);
        auto cellCountConserved = true;
        uint64_t histogramTotal = 0;
        for (auto value : histogram)
        {
            histogramTotal += value;
        }
        cellCountConserved = histogramTotal == cellCount;
        auto expectedFinalGridSha256 = GetOptionalString(request, L"expected_final_grid_sha256");
        auto expectedResultDigest = GetOptionalString(request, L"expected_result_digest");
        auto finalGridMatchesExpected = expectedFinalGridSha256.empty() || expectedFinalGridSha256 == finalGridSha256;
        auto resultDigestMatchesExpected = expectedResultDigest.empty() || expectedResultDigest == resultDigest;
        uint64_t mismatchCount = 0;
        if (!finalGridMatchesExpected)
        {
            ++mismatchCount;
        }
        if (!resultDigestMatchesExpected)
        {
            ++mismatchCount;
        }
        auto verdict = mismatchCount == 0 ? L"PASS" : L"MISMATCH";
        auto verified = cellCountConserved && stateIdsBounded && !finalGridSha256.empty() && !resultDigest.empty() && mismatchCount == 0;

        auto resultPayload =
            L"{\"schema_version\":" + JsonString(PhysicsCellularResultSchemaVersion) +
            L",\"protocol_version\":" + JsonString(WorkerProtocolVersion) +
            L",\"kernel_id\":" + JsonString(kernelId) +
            L",\"kernel_family\":\"bounded_cellular_automata\"" +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"width\":" + std::to_wstring(width) +
            L",\"height\":" + std::to_wstring(height) +
            L",\"states\":" + std::to_wstring(states) +
            L",\"steps\":" + std::to_wstring(steps) +
            L",\"cell_count\":" + std::to_wstring(cellCount) +
            L",\"cell_updates\":" + std::to_wstring(requestedCellUpdates) +
            L",\"state_histogram\":" + UInt64VectorJson(histogram) +
            L",\"active_cells\":" + std::to_wstring(activeCells) +
            L",\"frontier_count\":" + std::to_wstring(frontierCount) +
            L",\"state_changes\":" + std::to_wstring(stateChanges) +
            L",\"transition_mix64\":" + JsonString(UInt64Hex(transitionMix)) +
            L",\"transition_table_hash\":" + JsonString(transitionTableHash) +
            L",\"final_grid_sha256\":" + JsonString(finalGridSha256) +
            L",\"result_digest\":" + JsonString(resultDigest) +
            L",\"expected_final_grid_sha256\":" + JsonString(expectedFinalGridSha256) +
            L",\"expected_result_digest\":" + JsonString(expectedResultDigest) +
            L",\"final_grid_matches_expected\":" + BoolJson(finalGridMatchesExpected) +
            L",\"result_digest_matches_expected\":" + BoolJson(resultDigestMatchesExpected) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"frame_sample_hashes\":" + PhysicsFrameHashesJson(frameHashes) +
            L",\"visual_summary_rows\":" + StringVectorJson(visualRows) +
            L",\"invariants\":{\"cell_count_conserved\":" + BoolJson(cellCountConserved) +
            L",\"state_ids_bounded\":" + BoolJson(stateIdsBounded) +
            L",\"deterministic_seed_required\":true" +
            L",\"mismatch_count\":" + std::to_wstring(mismatchCount) + L"}" +
            L",\"claim_boundary\":{\"bounded_simulation_data_generation\":true" +
            L",\"high_fidelity_physics\":false" +
            L",\"terabyte_scale_data_generation\":false" +
            L",\"multi_console_cluster\":false" +
            L",\"full_console_12_1_tflops\":false}" +
            L",\"verified\":" + BoolJson(verified) +
            L"}";

        auto suffix = GenerateSessionId();
        if (suffix.size() > 16)
        {
            suffix.resize(16);
        }
        auto manifestFields =
            L",\"job_output_schema\":" + JsonString(PhysicsCellularResultSchemaVersion) +
            L",\"kernel_id\":" + JsonString(kernelId) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"width\":" + std::to_wstring(width) +
            L",\"height\":" + std::to_wstring(height) +
            L",\"steps\":" + std::to_wstring(steps) +
            L",\"result_digest\":" + JsonString(resultDigest) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"mismatch_count\":" + std::to_wstring(mismatchCount) +
            L",\"verified\":" + BoolJson(verified);
        auto published = PublishUtf8ArtifactPayload(
            request,
            root,
            L"physics-kernel-result-" + suffix,
            L"physics-kernel-result",
            WideToUtf8(resultPayload),
            manifestFields);

        return OkBase(L"run_physics_kernel_job") +
            L",\"schema_version\":" + JsonString(PhysicsKernelJobSchemaVersion) +
            L",\"result_schema\":" + JsonString(PhysicsCellularResultSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(ArtifactUploadSchemaVersion) +
            L",\"kernel_id\":" + JsonString(kernelId) +
            L",\"result_artifact_id\":" + JsonString(published.artifactId) +
            L",\"result_artifact_kind\":" + JsonString(published.artifactKind) +
            L",\"result_bytes\":" + std::to_wstring(published.bytes) +
            L",\"result_sha256\":" + JsonString(published.sha256) +
            L",\"result_blob_handle\":" + JsonString(published.blobHandle) +
            L",\"result_manifest_path\":" + JsonString(published.manifestPath) +
            L",\"result_deduplicated\":" + BoolJson(published.deduplicated) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"width\":" + std::to_wstring(width) +
            L",\"height\":" + std::to_wstring(height) +
            L",\"states\":" + std::to_wstring(states) +
            L",\"steps\":" + std::to_wstring(steps) +
            L",\"cell_count\":" + std::to_wstring(cellCount) +
            L",\"cell_updates\":" + std::to_wstring(requestedCellUpdates) +
            L",\"state_histogram\":" + UInt64VectorJson(histogram) +
            L",\"frontier_count\":" + std::to_wstring(frontierCount) +
            L",\"transition_mix64\":" + JsonString(UInt64Hex(transitionMix)) +
            L",\"transition_table_hash\":" + JsonString(transitionTableHash) +
            L",\"final_grid_sha256\":" + JsonString(finalGridSha256) +
            L",\"result_digest\":" + JsonString(resultDigest) +
            L",\"expected_final_grid_sha256\":" + JsonString(expectedFinalGridSha256) +
            L",\"expected_result_digest\":" + JsonString(expectedResultDigest) +
            L",\"final_grid_matches_expected\":" + BoolJson(finalGridMatchesExpected) +
            L",\"result_digest_matches_expected\":" + BoolJson(resultDigestMatchesExpected) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"frame_sample_hashes\":" + PhysicsFrameHashesJson(frameHashes) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"mismatch_count\":" + std::to_wstring(mismatchCount) +
            L",\"cell_count_conserved\":" + BoolJson(cellCountConserved) +
            L",\"state_ids_bounded\":" + BoolJson(stateIdsBounded) +
            L",\"bounded_simulation_data_generation\":true" +
            L",\"high_fidelity_physics\":false" +
            L",\"terabyte_scale_data_generation\":false" +
            L",\"workspace_bytes_before\":" + std::to_wstring(published.workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(published.workspaceBytesAfter) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(published.workspaceBudgetBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(published.rollbackHeadroomBytes) +
            L",\"storage_available_known\":" + BoolJson(published.storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(published.storageAvailableBytes) +
            L",\"execution_ms\":" + DoubleJson(elapsedMs, 6) +
            L",\"publish_ms\":" + DoubleJson(published.publishMs, 6) +
            L"}";
    }

    static std::wstring ExecuteRunArtifactManifestJob(JsonObject const& request, fs::path const& root)
    {
        auto manifestArtifactId = NormalizeArtifactId(GetOptionalString(request, L"manifest_artifact_id"), true);
        auto manifestTarget = ResolveArtifactReadTarget(root, manifestArtifactId);
        if (!manifestTarget.committed)
        {
            throw WorkerProtocolError("artifact_manifest_job.manifest_not_committed", "manifest artifact must be committed");
        }
        if (manifestTarget.bytes == 0 || manifestTarget.bytes > MaxArtifactManifestJsonBytes)
        {
            throw WorkerProtocolError("artifact_manifest_job.manifest_size_invalid", "manifest artifact JSON size is outside the supported range");
        }

        auto manifestUtf8 = ReadTextFile(manifestTarget.path);
        auto manifest = JsonObject::Parse(Utf8ToWide(manifestUtf8));
        auto manifestSchema = GetOptionalString(manifest, L"schema_version");
        if (manifestSchema != L"worker-multi-artifact-manifest-v1")
        {
            throw WorkerProtocolError("artifact_manifest_job.schema_invalid", "manifest artifact must use worker-multi-artifact-manifest-v1");
        }
        if (!manifest.HasKey(L"parts"))
        {
            throw WorkerProtocolError("artifact_manifest_job.parts_missing", "manifest artifact must contain parts[]");
        }

        auto parts = manifest.GetNamedArray(L"parts");
        if (parts.Size() == 0 || parts.Size() > MaxArtifactManifestParts)
        {
            throw WorkerProtocolError("artifact_manifest_job.part_count_invalid", "manifest part count is outside the supported range");
        }

        auto declaredPartCount = GetOptionalUInt64(manifest, L"part_count", parts.Size());
        auto declaredLogicalBytes = GetOptionalUInt64(manifest, L"logical_payload_bytes", 0);
        auto expectedLogicalSha256 = NormalizeOptionalSha256(GetOptionalString(request, L"expected_logical_sha256", GetOptionalString(manifest, L"logical_upload_sha256")));

        auto started = std::chrono::steady_clock::now();
        Sha256 logicalSha;
        uint64_t logicalBytesRead = 0;
        uint64_t verifiedPartCount = 0;
        uint64_t readOperationCount = 0;
        uint64_t checksum = 1469598103934665603ull;
        bool verified = declaredPartCount == parts.Size();
        std::vector<std::wstring> partResults;
        std::vector<uint8_t> buffer(ArtifactManifestJobReadBufferBytes);

        for (uint32_t i = 0; i < parts.Size(); ++i)
        {
            auto part = parts.GetObjectAt(i);
            auto partIndex = GetOptionalUInt64(part, L"index", i);
            auto partArtifactId = NormalizeArtifactId(GetOptionalString(part, L"artifact_id"), true);
            auto expectedPartSha256 = NormalizeOptionalSha256(GetOptionalString(part, L"sha256"));
            auto expectedPartBytes = GetOptionalUInt64(part, L"part_bytes", 0);
            if (expectedPartSha256.empty())
            {
                throw WorkerProtocolError("artifact_manifest_job.part_sha256_missing", "manifest part is missing sha256");
            }
            if (expectedPartBytes == 0 || expectedPartBytes > MaxArtifactExpectedBytes)
            {
                throw WorkerProtocolError("artifact_manifest_job.part_bytes_invalid", "manifest part_bytes is outside the supported range");
            }

            auto partTarget = ResolveArtifactReadTarget(root, partArtifactId);
            if (!partTarget.committed)
            {
                throw WorkerProtocolError("artifact_manifest_job.part_not_committed", "manifest part artifact must be committed");
            }

            std::ifstream input(partTarget.path, std::ios::binary);
            if (!input)
            {
                throw WorkerProtocolError("artifact_manifest_job.part_read_failed", "could not open manifest part artifact");
            }

            Sha256 partSha;
            uint64_t partBytesRead = 0;
            uint64_t partReadOperations = 0;
            while (input)
            {
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                auto read = input.gcount();
                if (read <= 0)
                {
                    break;
                }
                auto readSize = static_cast<size_t>(read);
                partSha.Update(buffer.data(), readSize);
                logicalSha.Update(buffer.data(), readSize);
                partBytesRead += static_cast<uint64_t>(readSize);
                logicalBytesRead += static_cast<uint64_t>(readSize);
                partReadOperations += 1;
                readOperationCount += 1;

                checksum ^= static_cast<uint64_t>(buffer[0]);
                checksum *= 1099511628211ull;
                checksum ^= static_cast<uint64_t>(buffer[readSize - 1]) << 8;
                checksum ^= (logicalBytesRead << 1) ^ static_cast<uint64_t>(readSize);
                checksum *= 1099511628211ull;
            }
            if (input.bad())
            {
                throw WorkerProtocolError("artifact_manifest_job.part_read_failed", "could not read manifest part artifact");
            }

            auto actualPartSha256 = partSha.FinalHex();
            auto partVerified = partTarget.sha256 == expectedPartSha256 &&
                partTarget.bytes == expectedPartBytes &&
                partBytesRead == expectedPartBytes &&
                actualPartSha256 == expectedPartSha256;
            if (partVerified)
            {
                verifiedPartCount += 1;
            }
            else
            {
                verified = false;
            }

            partResults.push_back(
                L"{\"index\":" + std::to_wstring(partIndex) +
                L",\"artifact_id\":" + JsonString(partArtifactId) +
                L",\"artifact_kind\":" + JsonString(partTarget.artifactKind) +
                L",\"expected_bytes\":" + std::to_wstring(expectedPartBytes) +
                L",\"read_bytes\":" + std::to_wstring(partBytesRead) +
                L",\"manifest_sha256\":" + JsonString(expectedPartSha256) +
                L",\"artifact_sha256\":" + JsonString(partTarget.sha256) +
                L",\"read_sha256\":" + JsonString(actualPartSha256) +
                L",\"read_operations\":" + std::to_wstring(partReadOperations) +
                L",\"verified\":" + BoolJson(partVerified) +
                L"}");
        }

        auto logicalSha256 = logicalSha.FinalHex();
        auto expectedLogicalKnown = !expectedLogicalSha256.empty();
        if (declaredLogicalBytes != 0 && declaredLogicalBytes != logicalBytesRead)
        {
            verified = false;
        }
        if (expectedLogicalKnown && expectedLogicalSha256 != logicalSha256)
        {
            verified = false;
        }

        std::wstring partResultsJson = L"[";
        for (size_t i = 0; i < partResults.size(); ++i)
        {
            if (i != 0)
            {
                partResultsJson += L",";
            }
            partResultsJson += partResults[i];
        }
        partResultsJson += L"]";

        auto elapsedMs = ElapsedMilliseconds(started);
        auto verdict = verified ? L"passed" : L"failed";
        auto resultPayload =
            L"{\"schema_version\":" + JsonString(ArtifactManifestJobSchemaVersion) +
            L",\"protocol_version\":" + JsonString(WorkerProtocolVersion) +
            L",\"job_kind\":\"artifact_manifest_streaming_verdict\"" +
            L",\"manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"manifest_schema\":" + JsonString(manifestSchema) +
            L",\"manifest_sha256\":" + JsonString(manifestTarget.sha256) +
            L",\"manifest_bytes\":" + std::to_wstring(manifestTarget.bytes) +
            L",\"declared_part_count\":" + std::to_wstring(declaredPartCount) +
            L",\"part_count\":" + std::to_wstring(parts.Size()) +
            L",\"verified_part_count\":" + std::to_wstring(verifiedPartCount) +
            L",\"declared_logical_bytes\":" + std::to_wstring(declaredLogicalBytes) +
            L",\"logical_bytes_read\":" + std::to_wstring(logicalBytesRead) +
            L",\"expected_logical_sha256_known\":" + BoolJson(expectedLogicalKnown) +
            L",\"expected_logical_sha256\":" + JsonString(expectedLogicalSha256) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"read_buffer_bytes\":" + std::to_wstring(ArtifactManifestJobReadBufferBytes) +
            L",\"read_operation_count\":" + std::to_wstring(readOperationCount) +
            L",\"verdict_checksum64\":" + JsonString(UInt64Hex(checksum)) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs, 6) +
            L",\"part_results\":" + partResultsJson +
            L"}";

        auto suffix = GenerateSessionId();
        if (suffix.size() > 16)
        {
            suffix.resize(16);
        }
        auto defaultArtifactId = L"manifest-job-result-" + suffix;
        auto manifestFields =
            L",\"job_output_schema\":" + JsonString(ArtifactManifestJobSchemaVersion) +
            L",\"source_manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"verified\":" + BoolJson(verified);

        auto published = PublishUtf8ArtifactPayload(
            request,
            root,
            defaultArtifactId,
            L"artifact-manifest-job-result",
            WideToUtf8(resultPayload),
            manifestFields);

        return OkBase(L"run_artifact_manifest_job") +
            L",\"schema_version\":" + JsonString(ArtifactManifestJobSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(ArtifactUploadSchemaVersion) +
            L",\"manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"manifest_sha256\":" + JsonString(manifestTarget.sha256) +
            L",\"result_artifact_id\":" + JsonString(published.artifactId) +
            L",\"result_artifact_kind\":" + JsonString(published.artifactKind) +
            L",\"result_bytes\":" + std::to_wstring(published.bytes) +
            L",\"result_sha256\":" + JsonString(published.sha256) +
            L",\"result_blob_handle\":" + JsonString(published.blobHandle) +
            L",\"result_manifest_path\":" + JsonString(published.manifestPath) +
            L",\"result_deduplicated\":" + BoolJson(published.deduplicated) +
            L",\"declared_part_count\":" + std::to_wstring(declaredPartCount) +
            L",\"part_count\":" + std::to_wstring(parts.Size()) +
            L",\"verified_part_count\":" + std::to_wstring(verifiedPartCount) +
            L",\"declared_logical_bytes\":" + std::to_wstring(declaredLogicalBytes) +
            L",\"logical_bytes_read\":" + std::to_wstring(logicalBytesRead) +
            L",\"expected_logical_sha256\":" + JsonString(expectedLogicalSha256) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"read_buffer_bytes\":" + std::to_wstring(ArtifactManifestJobReadBufferBytes) +
            L",\"read_operation_count\":" + std::to_wstring(readOperationCount) +
            L",\"verdict_checksum64\":" + JsonString(UInt64Hex(checksum)) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes_before\":" + std::to_wstring(published.workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(published.workspaceBytesAfter) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(published.workspaceBudgetBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(published.rollbackHeadroomBytes) +
            L",\"storage_available_known\":" + BoolJson(published.storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(published.storageAvailableBytes) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs, 6) +
            L",\"publish_ms\":" + DoubleJson(published.publishMs, 6) +
            L"}";
    }

    static std::wstring ExecuteRunArtifactManifestComputeJob(JsonObject const& request, fs::path const& root)
    {
        WorkerArtifactManifestComputeInput input;
        input.request = request;
        input.protocolVersion = WorkerProtocolVersion;
        input.artifactUploadSchemaVersion = ArtifactUploadSchemaVersion;
        input.artifactManifestComputeJobSchemaVersion = ArtifactManifestComputeJobSchemaVersion;
        input.maxArtifactManifestJsonBytes = MaxArtifactManifestJsonBytes;
        input.maxArtifactManifestParts = MaxArtifactManifestParts;
        input.maxArtifactExpectedBytes = MaxArtifactExpectedBytes;
        input.readBufferBytes = ArtifactManifestJobReadBufferBytes;

        return WorkerRunArtifactManifestComputeJob(
            input,
            [&](std::wstring const& artifactId)
            {
                auto target = ResolveArtifactReadTarget(root, artifactId);
                WorkerArtifactManifestReadTarget moved;
                moved.path = target.path;
                moved.artifactKind = target.artifactKind;
                moved.sha256 = target.sha256;
                moved.bytes = target.bytes;
                moved.committed = target.committed;
                return moved;
            },
            [](fs::path const& path)
            {
                return ReadTextFile(path);
            },
            [&](JsonObject const& publishRequest,
                std::wstring const& defaultArtifactId,
                std::wstring const& defaultArtifactKind,
                std::string const& payloadUtf8,
                std::wstring const& extraManifestFields)
            {
                auto published = PublishUtf8ArtifactPayload(
                    publishRequest,
                    root,
                    defaultArtifactId,
                    defaultArtifactKind,
                    payloadUtf8,
                    extraManifestFields);

                WorkerArtifactManifestPublishedResult moved;
                moved.artifactId = published.artifactId;
                moved.artifactKind = published.artifactKind;
                moved.sha256 = published.sha256;
                moved.blobHandle = published.blobHandle;
                moved.manifestPath = published.manifestPath;
                moved.bytes = published.bytes;
                moved.workspaceBytesBefore = published.workspaceBytesBefore;
                moved.workspaceBytesAfter = published.workspaceBytesAfter;
                moved.workspaceBudgetBytes = published.workspaceBudgetBytes;
                moved.rollbackHeadroomBytes = published.rollbackHeadroomBytes;
                moved.storageAvailableBytes = published.storageAvailableBytes;
                moved.storageAvailableKnown = published.storageAvailableKnown;
                moved.deduplicated = published.deduplicated;
                moved.publishMs = published.publishMs;
                return moved;
            },
            []()
            {
                return GenerateSessionId();
            });
    }

    static std::wstring ExecuteRunD3D12ShaderShapeJob(JsonObject const& request, fs::path const& root);

    static std::wstring ExecuteSubmitGraph(
        JsonObject const& request,
        fs::path const& root,
        std::atomic_bool const* cancelRequested = nullptr,
        std::function<void(uint64_t, std::wstring const&)> const& checkpointObserver = {})
    {
        WorkerGraphOrchestrationServices services;
        services.protocolVersion = WorkerProtocolVersion;
        services.generateSessionId = []() { return GenerateSessionId(); };
        services.xvmResolver = [&](std::wstring const& artifactId)
        {
            auto target = ResolveArtifactReadTarget(root, artifactId);
            WorkerXvmProgramReadTarget moved;
            moved.path = target.path;
            moved.artifactKind = target.artifactKind;
            moved.sha256 = target.sha256;
            moved.bytes = target.bytes;
            moved.committed = target.committed;
            return moved;
        };
        services.xvmTextReader = [](fs::path const& path) { return ReadTextFile(path); };
        services.computeRunner = [&](JsonObject const& nodeRequest)
        {
            auto command = GetOptionalString(nodeRequest, L"command");
            if (command == L"run_artifact_manifest_compute_job")
            {
                return ExecuteRunArtifactManifestComputeJob(nodeRequest, root);
            }
            if (command == L"run_d3d12_shader_shape_job")
            {
                return ExecuteRunD3D12ShaderShapeJob(nodeRequest, root);
            }
            throw WorkerProtocolError("submit_graph.compute_runner_command_invalid", "graph compute runner command is not allowlisted");
        };
        services.artifactPublisher = [&](JsonObject const& publishRequest,
            std::wstring const& defaultArtifactId,
            std::wstring const& defaultArtifactKind,
            std::string const& payloadUtf8,
            std::wstring const& extraManifestFields)
        {
            return WorkerPublishGraphArtifact(
                publishRequest,
                root,
                defaultArtifactId,
                defaultArtifactKind,
                payloadUtf8,
                extraManifestFields,
                ArtifactPublicationConfig());
        };
        services.sha256Text = [](std::string const& text) { return Sha256TextHex(text); };
        services.wideMix64 = [](std::wstring const& text, uint64_t seed) { return StableWideStringMix64(text, seed); };
        services.xvmSnapshotAuthorityProvider = [root]()
        {
            return WorkerLoadOrCreateXvmSnapshotAuthority(root.parent_path() / L"xcompute-runtime-authority");
        };
        services.xvmSnapshotAuthorityLoader = [root]()
        {
            return WorkerLoadXvmSnapshotAuthority(root.parent_path() / L"xcompute-runtime-authority");
        };
        auto cancellationRequested = [cancelRequested]()
        {
            return cancelRequested != nullptr && cancelRequested->load();
        };
        return WorkerGraphExecuteSubmitGraph(request, services, cancellationRequested, checkpointObserver);
    }
    static std::wstring ExecuteSubmitMacro(JsonObject const& request, fs::path const& root)
    {
        auto macroId = GetOptionalString(request, L"macro_id");
        auto macro = TryGetWorkerMacroDefinition(macroId);
        if (!macro)
        {
            throw WorkerProtocolError("submit_macro.macro_id_not_allowlisted", "submit_macro macro_id is not allowlisted");
        }

        if (!request.HasKey(L"manifest_artifact_ids"))
        {
            throw WorkerProtocolError("submit_macro.manifest_artifact_ids_required", "manifest_artifact_ids array is required");
        }
        auto manifestValue = request.GetNamedValue(L"manifest_artifact_ids");
        if (manifestValue.ValueType() != JsonValueType::Array)
        {
            throw WorkerProtocolError("submit_macro.manifest_artifact_ids_invalid", "manifest_artifact_ids must be an array");
        }
        auto manifestIds = manifestValue.GetArray();
        if (manifestIds.Size() != macro->requiredInputCount)
        {
            throw WorkerProtocolError("submit_macro.input_count_invalid", "macro requires a fixed manifest_artifact_ids count");
        }

        std::vector<std::wstring> inputManifestIds;
        for (uint32_t i = 0; i < manifestIds.Size(); ++i)
        {
            auto value = manifestIds.GetAt(i);
            if (value.ValueType() != JsonValueType::String)
            {
                throw WorkerProtocolError("submit_macro.manifest_artifact_id_invalid", "manifest artifact ids must be strings");
            }
            auto manifestId = std::wstring(value.GetString().c_str());
            if (!IsSafeProgramId(manifestId))
            {
                throw WorkerProtocolError("submit_macro.manifest_artifact_id_invalid", "manifest artifact ids must be safe artifact ids");
            }
            inputManifestIds.push_back(manifestId);
        }

        for (size_t i = 0; i < inputManifestIds.size(); ++i)
        {
            for (size_t j = i + 1; j < inputManifestIds.size(); ++j)
            {
                if (inputManifestIds[i] == inputManifestIds[j])
                {
                    throw WorkerProtocolError("submit_macro.manifest_artifact_ids_duplicate", "manifest artifact ids must be distinct");
                }
            }
        }

        for (auto const& manifestId : inputManifestIds)
        {
            ArtifactReadTarget status;
            try
            {
                status = ResolveArtifactReadTarget(root, manifestId);
            }
            catch (WorkerProtocolError const& ex)
            {
                if (ex.code == "artifact.not_found")
                {
                    throw WorkerProtocolError("submit_macro.manifest_artifact_missing", "manifest artifact must exist and be committed");
                }
                throw;
            }
            catch (WorkerArtifactStoreError const& ex)
            {
                if (ex.code == "artifact.not_found")
                {
                    throw WorkerProtocolError("submit_macro.manifest_artifact_missing", "manifest artifact must exist and be committed");
                }
                throw;
            }
            if (!status.committed)
            {
                throw WorkerProtocolError("submit_macro.manifest_artifact_missing", "manifest artifact must exist and be committed");
            }
            if (status.artifactKind != L"multi-artifact-manifest-v1")
            {
                throw WorkerProtocolError("submit_macro.manifest_artifact_kind_invalid", "macro inputs must be worker-multi-artifact-manifest-v1 artifacts");
            }
        }

        auto graphId = GetOptionalString(request, L"graph_id", L"m134-evidence-reduce-pipeline");
        if (!IsSafeProgramId(graphId))
        {
            throw WorkerProtocolError("submit_macro.graph_id_invalid", "graph_id must be a safe id");
        }

        auto resultPrefix = GetOptionalString(request, L"result_prefix", graphId);
        if (!IsSafeProgramId(resultPrefix))
        {
            throw WorkerProtocolError("submit_macro.result_prefix_invalid", "result_prefix must be a safe id");
        }
        if (resultPrefix.size() > 40)
        {
            resultPrefix.resize(40);
        }

        auto reduceResult = resultPrefix + L"-reduce-result";
        auto graphResult = GetOptionalString(request, L"graph_result_artifact_id", resultPrefix + L"-graph-result");
        auto evidenceBundle = GetOptionalString(request, L"evidence_bundle_artifact_id", resultPrefix + L"-evidence-bundle");

        std::vector<std::wstring> branchResultIds;
        for (auto const& branch : macro->branches)
        {
            branchResultIds.push_back(resultPrefix + L"-" + branch.nodeId + L"-result");
        }
        std::vector<std::wstring> expandedArtifactIds = branchResultIds;
        expandedArtifactIds.push_back(reduceResult);
        expandedArtifactIds.push_back(graphResult);
        expandedArtifactIds.push_back(evidenceBundle);
        for (auto const& artifactId : expandedArtifactIds)
        {
            if (!IsSafeProgramId(artifactId))
            {
                throw WorkerProtocolError("submit_macro.artifact_id_invalid", "expanded artifact ids must be safe ids");
            }
        }

        JsonObject graph;
        InsertJsonString(graph, L"schema_version", L"worker-on-device-graph-v1");
        InsertJsonString(graph, L"graph_id", graphId);
        InsertJsonString(graph, L"macro_id", macroId);
        InsertJsonString(graph, L"macro_expansion", L"worker_side_bounded_static_graph_template");
        InsertJsonString(graph, L"graph_result_artifact_id", graphResult);
        InsertJsonBool(graph, L"publish_evidence_bundle", request.GetNamedBoolean(L"publish_evidence_bundle", true));
        InsertJsonString(graph, L"evidence_bundle_artifact_id", evidenceBundle);

        JsonArray nodes;
        JsonArray reduceEdges;
        for (size_t i = 0; i < macro->branches.size(); ++i)
        {
            auto const& branch = macro->branches[i];
            JsonObject node;
            InsertJsonString(node, L"node_id", branch.nodeId);
            InsertJsonString(node, L"command", L"run_artifact_manifest_compute_job");
            InsertJsonString(node, L"kernel_id", branch.kernelId);
            InsertJsonString(node, L"manifest_artifact_id", inputManifestIds[branch.manifestInputIndex]);
            InsertJsonString(node, L"result_artifact_id", branchResultIds[i]);
            InsertJsonString(node, L"artifact_kind", L"on-device-graph-node-result");
            nodes.Append(node);

            JsonObject edge;
            InsertJsonString(edge, L"from_node_id", branch.nodeId);
            InsertJsonString(edge, L"role", branch.edgeRole);
            InsertJsonString(edge, L"expected_result_artifact_id", branchResultIds[i]);
            reduceEdges.Append(edge);
        }

        JsonObject inputBinding;
        InsertJsonString(inputBinding, L"mode", L"mix_bound_input_edges_v1");

        JsonObject reduceNode;
        InsertJsonString(reduceNode, L"node_id", L"final_reduce");
        InsertJsonString(reduceNode, L"command", L"reduce_graph_verdict");
        InsertJsonString(reduceNode, L"kernel_id", L"graph_reduce_verdict_v1");
        InsertJsonString(reduceNode, L"result_artifact_id", reduceResult);
        InsertJsonString(reduceNode, L"artifact_kind", L"on-device-graph-reduce-result");
        reduceNode.Insert(L"input_edges", reduceEdges);
        reduceNode.Insert(L"input_binding", inputBinding);
        nodes.Append(reduceNode);
        graph.Insert(L"nodes", nodes);

        JsonObject graphRequest;
        InsertJsonString(graphRequest, L"command", L"submit_graph");
        graphRequest.Insert(L"graph", graph);
        auto graphResultJson = ExecuteSubmitGraph(graphRequest, root);
        auto suffix = std::wstring(L",\"macro_schema_version\":\"worker-on-device-graph-macro-result-0.1\"") +
            L",\"macro_id\":" + JsonString(macroId) +
            L",\"macro_expansion\":\"worker_side_bounded_static_graph_template\"" +
            L",\"macro_input_count\":" + std::to_wstring(macro->requiredInputCount) +
            L",\"macro_expanded_node_count\":" + std::to_wstring(macro->branches.size() + 1) +
            L",\"macro_worker_side_expanded\":true" +
            L",\"macro_dynamic_graph_branching\":false";
        auto insert = graphResultJson.rfind(L"}");
        if (insert == std::wstring::npos)
        {
            return graphResultJson;
        }
        graphResultJson.insert(insert, suffix);
        return graphResultJson;
    }

    static std::vector<TreeEntry> ListTree(fs::path const& root, fs::path const& base, bool includeHash)
    {
        std::vector<TreeEntry> entries;
        if (!fs::exists(base))
        {
            return entries;
        }
        if (!fs::is_directory(base))
        {
            throw WorkerProtocolError("path.not_directory", "list_tree path is not a directory");
        }

        for (auto const& entry : fs::recursive_directory_iterator(base))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            TreeEntry item;
            item.path = entry.path().lexically_relative(root).wstring();
            item.relativePath = entry.path().lexically_relative(base).wstring();
            item.size = static_cast<uint64_t>(entry.file_size());
            if (includeHash)
            {
                item.sha256 = Sha256File(entry.path());
            }
            entries.push_back(std::move(item));
        }

        std::sort(entries.begin(), entries.end(), [](TreeEntry const& left, TreeEntry const& right)
        {
            return left.path < right.path;
        });
        return entries;
    }

    static std::vector<uint8_t> ReadBinaryChunk(fs::path const& path, uint64_t offset, uint64_t length, bool& eof)
    {
        if (length > MaxChunkBytes)
        {
            throw WorkerProtocolError("chunk.too_large", "read_chunk length exceeds 64 KiB");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("file.read_failed", "could not open file for chunk read");
        }

        input.seekg(0, std::ios::end);
        auto endPosition = input.tellg();
        if (endPosition < 0)
        {
            throw WorkerProtocolError("file.read_failed", "could not determine file size");
        }

        uint64_t fileSize = static_cast<uint64_t>(endPosition);
        if (offset > fileSize)
        {
            throw WorkerProtocolError("chunk.offset_past_eof", "read_chunk offset is past EOF");
        }

        auto available = fileSize - offset;
        auto toRead = static_cast<size_t>((std::min)(length, available));
        std::vector<uint8_t> buffer(toRead);
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (toRead != 0)
        {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(toRead));
        }
        eof = offset + toRead >= fileSize;
        return buffer;
    }

    static std::vector<uint8_t> ReadBinaryStreamRange(fs::path const& path, uint64_t offset, uint64_t length, bool& eof)
    {
        if (length > MaxStreamReadBytes)
        {
            throw WorkerProtocolError("stream.too_large", "read_chunks_stream length exceeds 4 MiB");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw WorkerProtocolError("file.read_failed", "could not open file for stream read");
        }

        input.seekg(0, std::ios::end);
        auto endPosition = input.tellg();
        if (endPosition < 0)
        {
            throw WorkerProtocolError("file.read_failed", "could not determine file size");
        }

        uint64_t fileSize = static_cast<uint64_t>(endPosition);
        if (offset > fileSize)
        {
            throw WorkerProtocolError("stream.offset_past_eof", "read_chunks_stream offset is past EOF");
        }

        auto available = fileSize - offset;
        auto toRead = static_cast<size_t>((std::min)(length, available));
        std::vector<uint8_t> buffer(toRead);
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (toRead != 0)
        {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(toRead));
            if (!input && !input.eof())
            {
                throw WorkerProtocolError("file.read_failed", "stream read did not complete");
            }
        }
        eof = offset + toRead >= fileSize;
        return buffer;
    }

    static void WriteBinaryChunk(
        fs::path const& path,
        uint64_t offset,
        std::vector<uint8_t> const& data,
        bool truncate,
        size_t maxBytes = MaxChunkBytes,
        std::string const& tooLargeCode = "chunk.too_large",
        std::string const& tooLargeMessage = "write_chunk data exceeds 64 KiB")
    {
        if (data.size() > maxBytes)
        {
            throw WorkerProtocolError(tooLargeCode, tooLargeMessage);
        }

        fs::create_directories(path.parent_path());
        bool exists = fs::exists(path);
        uint64_t currentSize = exists ? static_cast<uint64_t>(fs::file_size(path)) : 0;
        if (!exists && offset != 0)
        {
            throw WorkerProtocolError("chunk.offset_past_eof", "write_chunk cannot create sparse file");
        }
        if (offset > currentSize)
        {
            throw WorkerProtocolError("chunk.offset_past_eof", "write_chunk offset is past EOF");
        }

        if (!exists)
        {
            std::ofstream create(path, std::ios::binary);
            if (!create)
            {
                throw WorkerProtocolError("file.write_failed", "could not create file for chunk write");
            }
        }

        std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!output)
        {
            throw WorkerProtocolError("file.write_failed", "could not open file for chunk write");
        }
        output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!data.empty())
        {
            output.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        output.flush();
        if (!output.good())
        {
            throw WorkerProtocolError("file.write_failed", "chunk write did not complete");
        }

        if (truncate)
        {
            fs::resize_file(path, offset + data.size());
        }
    }

    static Windows::Foundation::IAsyncAction ReadBinaryPayloadAsync(DataReader reader, std::vector<uint8_t>& payload, size_t byteCount, size_t maxBytes)
    {
        if (byteCount > maxBytes)
        {
            throw WorkerProtocolError("chunk.too_large", "binary payload byte_count exceeds command limit");
        }

        payload.resize(byteCount);
        size_t offset = 0;
        while (offset < byteCount)
        {
            auto available = static_cast<size_t>(reader.UnconsumedBufferLength());
            auto consume = (std::min)(available, byteCount - offset);
            if (consume > 0)
            {
                reader.ReadBytes(winrt::array_view<uint8_t>(payload.data() + offset, payload.data() + offset + consume));
                offset += consume;
            }

            if (offset >= byteCount)
            {
                break;
            }

            auto remaining = byteCount - offset;
            auto requestBytes = static_cast<uint32_t>((std::min)(remaining, static_cast<size_t>(BinaryPayloadReadBufferBytes)));
            uint32_t loaded = 0;
            try
            {
                loaded = co_await reader.LoadAsync(requestBytes);
            }
            catch (hresult_error const&)
            {
                throw WorkerProtocolError("request.body_incomplete", "binary payload ended before byte_count");
            }
            if (loaded == 0)
            {
                throw WorkerProtocolError("request.body_incomplete", "binary payload ended before byte_count");
            }
        }

        co_return;
    }

    static std::wstring ExecuteWriteChunkBinary(JsonObject const& request, fs::path const& root, std::vector<uint8_t> const& data, double authMs)
    {
        auto expectedBytes = GetOptionalUInt64(request, L"byte_count", data.size());
        if (expectedBytes != data.size())
        {
            throw WorkerProtocolError("chunk.length_mismatch", "write_chunk_binary byte_count does not match received payload");
        }

        auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
        auto offset = GetOptionalUInt64(request, L"offset", 0);
        auto truncate = request.GetNamedBoolean(L"truncate", false);
        auto writeStarted = std::chrono::steady_clock::now();
        WriteBinaryChunk(path, offset, data, truncate);
        auto writeMs = ElapsedMilliseconds(writeStarted);
        auto responseStarted = std::chrono::steady_clock::now();
        auto response = OkBase(L"write_chunk_binary") +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_written\":" + std::to_wstring(data.size()) +
            L",\"truncated\":" + BoolJson(truncate) +
            L",\"encoding\":\"binary\"";
        auto responseMs = ElapsedMilliseconds(responseStarted);
        response += SyncServerTimingJson(request, authMs, 0.0, writeMs, responseMs, data.size(), data.size());
        response += L"}";
        return response;
    }

    static std::wstring ExecuteWriteChunksStream(JsonObject const& request, fs::path const& root, std::vector<uint8_t> const& data, double authMs)
    {
        auto expectedBytes = GetOptionalUInt64(request, L"byte_count", data.size());
        if (expectedBytes != data.size())
        {
            throw WorkerProtocolError("stream.length_mismatch", "write_chunks_stream byte_count does not match received payload");
        }

        auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
        auto offset = GetOptionalUInt64(request, L"offset", 0);
        auto truncate = request.GetNamedBoolean(L"truncate", false);
        auto writeStarted = std::chrono::steady_clock::now();
        WriteBinaryChunk(
            path,
            offset,
            data,
            truncate,
            MaxStreamWriteBytes,
            "stream.too_large",
            "write_chunks_stream data exceeds 4 MiB");
        auto writeMs = ElapsedMilliseconds(writeStarted);
        auto responseStarted = std::chrono::steady_clock::now();
        auto response = OkBase(L"write_chunks_stream") +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_written\":" + std::to_wstring(data.size()) +
            L",\"byte_count\":" + std::to_wstring(data.size()) +
            L",\"stream_max_bytes\":" + std::to_wstring(MaxStreamWriteBytes) +
            L",\"truncated\":" + BoolJson(truncate) +
            L",\"encoding\":\"binary\"";
        auto responseMs = ElapsedMilliseconds(responseStarted);
        response += SyncServerTimingJson(request, authMs, 0.0, writeMs, responseMs, data.size(), data.size());
        response += L"}";
        return response;
    }

    static BinaryChunkResponse ExecuteReadChunkBinary(JsonObject const& request, fs::path const& root)
    {
        auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
        auto offset = GetOptionalUInt64(request, L"offset", 0);
        auto length = GetOptionalUInt64(request, L"length", MaxChunkBytes);
        bool eof = false;
        auto data = ReadBinaryChunk(path, offset, length, eof);

        BinaryChunkResponse response;
        response.headerJson = OkBase(L"read_chunk_binary") +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_read\":" + std::to_wstring(data.size()) +
            L",\"byte_count\":" + std::to_wstring(data.size()) +
            L",\"eof\":" + BoolJson(eof) +
            L",\"encoding\":\"binary\"" +
            L"}";
        response.payload = std::move(data);
        return response;
    }

    static BinaryChunkResponse ExecuteReadChunksStream(JsonObject const& request, fs::path const& root)
    {
        auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
        auto offset = GetOptionalUInt64(request, L"offset", 0);
        auto requestedBytes = GetOptionalUInt64(request, L"max_bytes", MaxStreamReadBytes);
        auto length = (std::min)(requestedBytes, static_cast<uint64_t>(MaxStreamReadBytes));
        bool eof = false;
        auto data = ReadBinaryStreamRange(path, offset, length, eof);

        BinaryChunkResponse response;
        response.headerJson = OkBase(L"read_chunks_stream") +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_read\":" + std::to_wstring(data.size()) +
            L",\"byte_count\":" + std::to_wstring(data.size()) +
            L",\"requested_max_bytes\":" + std::to_wstring(requestedBytes) +
            L",\"stream_max_bytes\":" + std::to_wstring(MaxStreamReadBytes) +
            L",\"eof\":" + BoolJson(eof) +
            L",\"encoding\":\"binary\"" +
            L"}";
        response.payload = std::move(data);
        return response;
    }

    static BinaryChunkResponse ExecuteReadArtifactChunkBinary(JsonObject const& request, fs::path const& root)
    {
        auto artifactId = NormalizeArtifactId(GetOptionalString(request, L"artifact_id"), true);
        auto offset = GetOptionalUInt64(request, L"offset", 0);
        auto requestedBytes = GetOptionalUInt64(request, L"max_bytes", MaxStreamReadBytes);
        auto length = (std::min)(requestedBytes, static_cast<uint64_t>(MaxStreamReadBytes));
        auto target = ResolveArtifactReadTarget(root, artifactId);
        bool eof = false;
        auto data = ReadBinaryStreamRange(target.path, offset, length, eof);

        BinaryChunkResponse response;
        response.headerJson = OkBase(L"read_artifact_chunk_binary") +
            L",\"schema_version\":" + JsonString(ArtifactBinaryFramingSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(ArtifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(target.artifactKind) +
            L",\"artifact_status\":" + JsonString(target.status) +
            L",\"committed\":" + BoolJson(target.committed) +
            L",\"artifact_bytes\":" + std::to_wstring(target.bytes) +
            L",\"artifact_sha256\":" + JsonString(target.sha256) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_read\":" + std::to_wstring(data.size()) +
            L",\"byte_count\":" + std::to_wstring(data.size()) +
            L",\"requested_max_bytes\":" + std::to_wstring(requestedBytes) +
            L",\"stream_max_bytes\":" + std::to_wstring(MaxStreamReadBytes) +
            L",\"eof\":" + BoolJson(eof) +
            L",\"encoding\":\"binary\"" +
            L"}";
        response.payload = std::move(data);
        return response;
    }

    WorkerCommandServer::WorkerCommandServer()
        : m_sessionRuntime(WorkerProtocolVersion),
          m_protocolBoundary(WorkerProtocolVersion, m_sessionRuntime),
          m_asyncJobRuntime(
            WorkerProtocolVersion,
            [this](std::wstring const& eventText) { RecordEvent(eventText); },
            [this](std::wstring const& errorText) { RecordError(errorText); }),
          m_taskPlanRuntime(
            WorkerProtocolVersion,
            [this](
                std::wstring const& action,
                JsonObject const& step,
                fs::path const& root)
            {
                try
                {
                    return ExecuteTaskPlanAction(action, step, root);
                }
                catch (WorkerProtocolError const& error)
                {
                    throw WorkerTaskPlanError(error.code, error.message);
                }
            })
    {
    }

    bool WorkerCommandServer::Start(uint16_t port)
    {
        if (m_running || m_starting)
        {
            return true;
        }

        try
        {
            m_sessionRuntime.EnsurePairingCode();
            m_port = port;
            m_starting = true;
            RecordEvent(L"listener starting on port " + std::to_wstring(port));
            BindAsync(port);
            return true;
        }
        catch (...)
        {
            m_listener = nullptr;
            m_running = false;
            m_port = 0;
            RecordError(L"listener start failed");
            return false;
        }
    }

    void WorkerCommandServer::Stop()
    {
        try
        {
            if (m_listener)
            {
                m_listener.Close();
            }
        }
        catch (...)
        {
        }

        m_listener = nullptr;
        m_running = false;
        m_starting = false;
        RecordEvent(L"listener stopped");
    }

    bool WorkerCommandServer::Restart()
    {
        auto port = m_port == 0 ? static_cast<uint16_t>(8787) : m_port;
        Stop();
        return Start(port);
    }

    WorkerCommandServer::StatusSnapshot WorkerCommandServer::Snapshot() const
    {
        StatusSnapshot snapshot;
        snapshot.running = m_running;
        snapshot.starting = m_starting;
        snapshot.port = m_port;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            snapshot.acceptedConnectionCount = m_acceptedConnectionCount;
            snapshot.completedRequestCount = m_completedRequestCount;
            snapshot.failedRequestCount = m_failedRequestCount;
            snapshot.lastError = m_lastError;
            snapshot.recentEvents = m_recentEvents;
        }
        snapshot.activeSessionCount = m_sessionRuntime.ActiveSessionCount();
        try
        {
            snapshot.trustedControllerCount =
                CountTrustedControllers(WorkerRoot());
        }
        catch (...)
        {
            snapshot.trustedControllerCount = 0;
        }
        {
            auto asyncSnapshot = m_asyncJobRuntime.Snapshot();
            snapshot.activeAsyncJobCount = asyncSnapshot.activeJobCount;
            snapshot.queuedAsyncJobCount = asyncSnapshot.queuedJobCount;
            snapshot.asyncJobRunning = asyncSnapshot.jobRunning;
            snapshot.latestAsyncJobId = asyncSnapshot.latestJobId;
            snapshot.latestAsyncJobStatus = asyncSnapshot.latestJobStatus;
            snapshot.latestAsyncJobError = asyncSnapshot.latestJobError;
        }
        {
            auto taskPlanSnapshot = m_taskPlanRuntime.Snapshot();
            snapshot.activeTaskPlanRunCount = taskPlanSnapshot.activeRunCount;
            snapshot.activeTaskPlanId = taskPlanSnapshot.activeTaskPlanId;
            snapshot.activeTaskPlanRunId = taskPlanSnapshot.activeRunId;
            snapshot.latestTaskPlanId = taskPlanSnapshot.latestTaskPlanId;
            snapshot.latestTaskPlanRunId = taskPlanSnapshot.latestRunId;
            snapshot.latestTaskPlanRunStatus = taskPlanSnapshot.latestRunStatus;
            snapshot.latestTaskPlanBlockedReason = taskPlanSnapshot.latestBlockedReason;
            snapshot.latestTaskPlanError = taskPlanSnapshot.latestError;
        }
        return snapshot;
    }

    void WorkerCommandServer::RecordEvent(std::wstring const& eventText)
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_recentEvents.push_back(eventText);
        if (m_recentEvents.size() > 8)
        {
            m_recentEvents.erase(m_recentEvents.begin(), m_recentEvents.begin() + (m_recentEvents.size() - 8));
        }
    }

    void WorkerCommandServer::RecordError(std::wstring const& errorText)
    {
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_lastError = errorText;
        }
        RecordEvent(L"error: " + errorText);
    }

    void WorkerCommandServer::RecordCommandResult(std::wstring const& command, bool ok)
    {
        std::wstring safeCommand = command.empty() ? L"<empty>" : command;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            if (ok)
            {
                ++m_completedRequestCount;
            }
            else
            {
                ++m_failedRequestCount;
            }
        }
        RecordEvent((ok ? L"ok: " : L"fail: ") + safeCommand);
    }

    std::wstring WorkerCommandServer::ExportDiagnostics(fs::path const& root)
    {
        auto snapshot = Snapshot();
        std::wstring fileName = L"xcompute-worker-diagnostics.json";
        auto path = root.parent_path() / fileName;
        auto fileCount = WorkspaceFileCount(root);
        auto directoryCount = WorkspaceDirectoryCount(root);
        auto byteCount = WorkspaceBytes(root);
        WorkerDiagnosticsInput input;
        input.state.running = snapshot.running;
        input.state.starting = snapshot.starting;
        input.state.port = snapshot.port;
        input.state.acceptedConnectionCount = snapshot.acceptedConnectionCount;
        input.state.completedRequestCount = snapshot.completedRequestCount;
        input.state.failedRequestCount = snapshot.failedRequestCount;
        input.state.activeSessionCount = snapshot.activeSessionCount;
        input.state.activeAsyncJobCount = snapshot.activeAsyncJobCount;
        input.state.queuedAsyncJobCount = snapshot.queuedAsyncJobCount;
        input.state.asyncJobRunning = snapshot.asyncJobRunning;
        input.state.latestAsyncJobId = snapshot.latestAsyncJobId;
        input.state.latestAsyncJobStatus = snapshot.latestAsyncJobStatus;
        input.state.latestAsyncJobError = snapshot.latestAsyncJobError;
        input.state.activeTaskPlanRunCount = snapshot.activeTaskPlanRunCount;
        input.state.activeTaskPlanId = snapshot.activeTaskPlanId;
        input.state.activeTaskPlanRunId = snapshot.activeTaskPlanRunId;
        input.state.latestTaskPlanId = snapshot.latestTaskPlanId;
        input.state.latestTaskPlanRunId = snapshot.latestTaskPlanRunId;
        input.state.latestTaskPlanRunStatus = snapshot.latestTaskPlanRunStatus;
        input.state.latestTaskPlanBlockedReason = snapshot.latestTaskPlanBlockedReason;
        input.state.latestTaskPlanError = snapshot.latestTaskPlanError;
        input.state.lastError = snapshot.lastError;
        input.state.recentEvents = snapshot.recentEvents;
        input.workspace.exists = fs::exists(root);
        input.workspace.fileCount = fileCount;
        input.workspace.directoryCount = directoryCount;
        input.workspace.byteCount = byteCount;

        auto utf8 = WideToUtf8(WorkerBuildDiagnosticsJson(input));
        WriteTextFile(path, utf8);

        return OkBase(L"export_diagnostics") +
            L",\"file_name\":" + JsonString(fileName) +
            L",\"local_folder_relative_path\":" + JsonString(fileName) +
            L",\"absolute_path\":" + JsonString(path.wstring()) +
            L",\"bytes\":" + std::to_wstring(static_cast<uint64_t>(utf8.size())) +
            L",\"workspace_file_count\":" + std::to_wstring(fileCount) +
            L"}";
    }

    std::wstring WorkerCommandServer::ReadDiagnostics(fs::path const& root)
    {
        std::wstring fileName = L"xcompute-worker-diagnostics.json";
        auto path = root.parent_path() / fileName;
        if (!fs::is_regular_file(path))
        {
            throw WorkerProtocolError("diagnostics.not_found", "xcompute-worker-diagnostics.json has not been exported yet");
        }

        auto content = ReadTextFile(path);
        return OkBase(L"read_diagnostics") +
            L",\"file_name\":" + JsonString(fileName) +
            L",\"local_folder_relative_path\":" + JsonString(fileName) +
            L",\"absolute_path\":" + JsonString(path.wstring()) +
            L",\"bytes\":" + std::to_wstring(static_cast<uint64_t>(content.size())) +
            L",\"content\":" + JsonString(Utf8ToWide(content)) +
            L"}";
    }

    std::wstring WorkerCommandServer::DescribeRuntime(fs::path const& root)
    {
        auto snapshot = Snapshot();

        uint64_t persistedJobCount = 0;
        auto jobDirectory = m_asyncJobRuntime.SnapshotDirectory(root);
        if (fs::exists(jobDirectory))
        {
            for (auto const& entry : fs::directory_iterator(jobDirectory))
            {
                if (entry.is_regular_file() && entry.path().extension() == L".json")
                {
                    ++persistedJobCount;
                }
            }
        }

        auto taskPlanCount = m_taskPlanRuntime.CountTaskPlans(root);

        WorkerRuntimeDescriptionInput input;
        input.protocolVersion = WorkerProtocolVersion;
        input.port = m_port;

        input.limits.maxRequestBytes = MaxRequestBytes;
        input.limits.binaryPayloadReadBufferBytes = BinaryPayloadReadBufferBytes;
        input.limits.requestReadBufferBytes = RequestReadBufferBytes;
        input.limits.defaultSessionTtlSeconds = DefaultSessionTtlSeconds;
        input.limits.minSessionTtlSeconds = MinSessionTtlSeconds;
        input.limits.maxSessionTtlSeconds = MaxSessionTtlSeconds;
        input.limits.maxTrustedControllers = MaxTrustedControllers;
        input.limits.maxArtifactJsonChunkBytes = MaxArtifactJsonChunkBytes;
        input.limits.maxArtifactManifestJsonBytes = MaxArtifactManifestJsonBytes;
        input.limits.artifactManifestJobReadBufferBytes = ArtifactManifestJobReadBufferBytes;
        input.limits.maxStreamReadBytes = MaxStreamReadBytes;
        input.limits.maxStreamWriteBytes = MaxStreamWriteBytes;
        input.limits.maxArtifactManifestParts = MaxArtifactManifestParts;
        input.limits.maxArtifactExpectedBytes = MaxArtifactExpectedBytes;
        input.limits.defaultArtifactRollbackHeadroomBytes = DefaultArtifactRollbackHeadroomBytes;
        input.limits.defaultArtifactWorkspaceBudgetBytes = DefaultArtifactWorkspaceBudgetBytes;
        input.limits.maxArtifactWorkspaceBudgetBytes = MaxArtifactWorkspaceBudgetBytes;
        input.limits.maxAsyncJobs = WorkerAsyncJobRuntime::MaxJobs;
        input.limits.maxPhysicsGridWidth = MaxPhysicsGridWidth;
        input.limits.maxPhysicsGridHeight = MaxPhysicsGridHeight;
        input.limits.maxPhysicsCellCount = MaxPhysicsCellCount;
        input.limits.maxPhysicsSteps = MaxPhysicsSteps;
        input.limits.maxPhysicsCellUpdates = MaxPhysicsCellUpdates;
        input.limits.maxD3D12Fp32TimingElements = MaxD3D12Fp32TimingElements;
        input.limits.maxD3D12ShaderShapeElements = MaxD3D12ShaderShapeElements;

        input.state.running = snapshot.running;
        input.state.starting = snapshot.starting;
        input.state.asyncJobRunning = snapshot.asyncJobRunning;
        input.state.activeSessionCount = snapshot.activeSessionCount;
        input.state.activeAsyncJobCount = snapshot.activeAsyncJobCount;
        input.state.queuedAsyncJobCount = snapshot.queuedAsyncJobCount;
        input.state.persistedJobCount = persistedJobCount;
        input.state.taskPlanCount = taskPlanCount;
        input.state.activeTaskPlanRunCount = snapshot.activeTaskPlanRunCount;
        input.state.latestAsyncJobId = snapshot.latestAsyncJobId;
        input.state.latestAsyncJobStatus = snapshot.latestAsyncJobStatus;
        input.state.latestTaskPlanId = snapshot.latestTaskPlanId;
        input.state.latestTaskPlanRunStatus = snapshot.latestTaskPlanRunStatus;

        return WorkerBuildRuntimeDescription(input);
    }

    fire_and_forget WorkerCommandServer::BindAsync(uint16_t port)
    {
        try
        {
            co_await resume_background();
            StreamSocketListener listener;
            listener.Control().KeepAlive(true);
            auto token = listener.ConnectionReceived({ this, &WorkerCommandServer::OnConnectionReceived });
            co_await listener.BindServiceNameAsync(std::to_wstring(port));
            m_listener = listener;
            m_connectionToken = token;
            m_running = true;
            RecordEvent(L"listener bound on port " + std::to_wstring(port));
        }
        catch (hresult_error const& ex)
        {
            m_listener = nullptr;
            m_running = false;
            m_port = 0;
            RecordError(L"listener bind failed: " + std::wstring(ex.message().c_str()));
        }
        catch (...)
        {
            m_listener = nullptr;
            m_running = false;
            m_port = 0;
            RecordError(L"listener bind failed");
        }
        m_starting = false;
    }

    void WorkerCommandServer::OnConnectionReceived(
        StreamSocketListener const&,
        StreamSocketListenerConnectionReceivedEventArgs const& args)
    {
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            ++m_acceptedConnectionCount;
        }
        RecordEvent(L"connection accepted");
        HandleClientAsync(args.Socket());
    }

    fire_and_forget WorkerCommandServer::HandleClientAsync(StreamSocket socket)
    {
        try
        {
            DataReader reader(socket.InputStream());
            reader.InputStreamOptions(InputStreamOptions::Partial);
            DataWriter writer(socket.OutputStream());
            writer.UnicodeEncoding(UnicodeEncoding::Utf8);

            bool closeConnection = false;
            while (!closeConnection)
            {
                std::string request;
                bool lineComplete = false;
                bool connectionClosed = false;
                bool requestTooLarge = false;

                while (!lineComplete && !connectionClosed && !requestTooLarge)
                {
                    while (reader.UnconsumedBufferLength() > 0)
                    {
                        auto byte = reader.ReadByte();
                        if (byte == '\n')
                        {
                            lineComplete = true;
                            break;
                        }
                        if (byte != '\r')
                        {
                            request.push_back(static_cast<char>(byte));
                            if (request.size() >= MaxRequestBytes)
                            {
                                requestTooLarge = true;
                                break;
                            }
                        }
                    }

                    if (lineComplete || requestTooLarge)
                    {
                        break;
                    }

                    uint32_t loaded = co_await reader.LoadAsync(RequestReadBufferBytes);
                    if (loaded == 0)
                    {
                        connectionClosed = true;
                    }
                }

                if (connectionClosed && request.empty())
                {
                    break;
                }

                std::wstring response;
                std::vector<uint8_t> binaryResponse;
                bool hasBinaryResponse = false;
                std::wstring commandName;
                try
                {
                    if (requestTooLarge)
                    {
                        response = m_protocolBoundary.ErrorResponse(L"request.too_large", L"request exceeds 512 KiB");
                        commandName = L"request.too_large";
                        closeConnection = true;
                    }
                    else
                    {
                        auto parseStarted = std::chrono::steady_clock::now();
                        auto requestObject = JsonObject::Parse(Utf8ToWide(request));
                        auto parseMs = ElapsedMilliseconds(parseStarted);
                        requestObject.Insert(L"_server_parse_json_ms", JsonValue::CreateNumberValue(parseMs));
                        requestObject.Insert(L"_server_request_bytes", JsonValue::CreateNumberValue(static_cast<double>(request.size())));
                        auto command = GetOptionalString(requestObject, L"command");
                        commandName = command;
                        if (command == L"write_chunk_binary" || command == L"write_chunks_stream" || command == L"append_artifact_chunk_binary")
                        {
                            auto byteCount = GetOptionalUInt64(requestObject, L"byte_count", 0);
                            auto maxBytes = (command == L"write_chunks_stream" || command == L"append_artifact_chunk_binary") ? MaxStreamWriteBytes : MaxChunkBytes;
                            if (byteCount > maxBytes)
                            {
                                auto message = (command == L"write_chunks_stream" || command == L"append_artifact_chunk_binary") ?
                                    "binary payload byte_count exceeds 4 MiB" :
                                    "write_chunk_binary byte_count exceeds 64 KiB";
                                throw WorkerProtocolError("chunk.too_large", message);
                            }
                            auto preAuthStarted = std::chrono::steady_clock::now();
                            m_protocolBoundary.RequireBinaryAuthorized(requestObject, command, false);
                            auto preAuthMs = ElapsedMilliseconds(preAuthStarted);
                            requestObject.Insert(L"_server_pre_auth_ms", JsonValue::CreateNumberValue(preAuthMs));
                            std::vector<uint8_t> body;
                            auto bodyReadStarted = std::chrono::steady_clock::now();
                            try
                            {
                                co_await ReadBinaryPayloadAsync(reader, body, static_cast<size_t>(byteCount), maxBytes);
                            }
                            catch (WorkerProtocolError const&)
                            {
                                throw;
                            }
                            catch (hresult_error const&)
                            {
                                throw WorkerProtocolError("request.body_incomplete", "binary payload ended before byte_count");
                            }
                            auto bodyReadMs = ElapsedMilliseconds(bodyReadStarted);
                            requestObject.Insert(L"_server_binary_read_ms", JsonValue::CreateNumberValue(bodyReadMs));
                            requestObject.Insert(L"_server_binary_payload_bytes", JsonValue::CreateNumberValue(static_cast<double>(body.size())));
                            response = ProcessBinaryCommand(requestObject, body);
                        }
                        else if (command == L"read_chunk_binary")
                        {
                            auto root = WorkerRoot();
                            EnsureWorkspace(root);
                            m_protocolBoundary.RequireBinaryAuthorized(requestObject, command);
                            auto chunkResponse = ExecuteReadChunkBinary(requestObject, root);
                            response = std::move(chunkResponse.headerJson);
                            binaryResponse = std::move(chunkResponse.payload);
                            hasBinaryResponse = true;
                        }
                        else if (command == L"read_chunks_stream")
                        {
                            auto root = WorkerRoot();
                            EnsureWorkspace(root);
                            m_protocolBoundary.RequireBinaryAuthorized(requestObject, command);
                            auto streamResponse = ExecuteReadChunksStream(requestObject, root);
                            response = std::move(streamResponse.headerJson);
                            binaryResponse = std::move(streamResponse.payload);
                            hasBinaryResponse = true;
                        }
                        else if (command == L"read_artifact_chunk_binary")
                        {
                            auto root = WorkerRoot();
                            EnsureWorkspace(root);
                            m_protocolBoundary.RequireBinaryAuthorized(requestObject, command);
                            auto artifactResponse = ExecuteReadArtifactChunkBinary(requestObject, root);
                            response = std::move(artifactResponse.headerJson);
                            binaryResponse = std::move(artifactResponse.payload);
                            hasBinaryResponse = true;
                        }
                        else
                        {
                            response = ProcessCommand(Utf8ToWide(request));
                        }
                    }
                }
                catch (WorkerProtocolError const& ex)
                {
                    response = m_protocolBoundary.ErrorResponse(Utf8ToWide(ex.code), Utf8ToWide(ex.message));
                    if (commandName.empty())
                    {
                        commandName = Utf8ToWide(ex.code);
                    }
                }
                catch (std::exception const& ex)
                {
                    response = m_protocolBoundary.ErrorResponse(L"server.exception", Utf8ToWide(ex.what()));
                    if (commandName.empty())
                    {
                        commandName = L"server.exception";
                    }
                }
                catch (hresult_error const& ex)
                {
                    response = m_protocolBoundary.ErrorResponse(L"server.hresult", ex.message().c_str());
                    if (commandName.empty())
                    {
                        commandName = L"server.hresult";
                    }
                }
                catch (...)
                {
                    response = m_protocolBoundary.ErrorResponse(L"server.unhandled", L"unhandled command server error");
                    if (commandName.empty())
                    {
                        commandName = L"server.unhandled";
                    }
                }

                RecordCommandResult(commandName, response.find(L"\"ok\":false") == std::wstring::npos);

                writer.WriteString(response + L"\n");
                if (hasBinaryResponse && !binaryResponse.empty())
                {
                    writer.WriteBytes(winrt::array_view<uint8_t const>(binaryResponse.data(), binaryResponse.data() + binaryResponse.size()));
                }
                co_await writer.StoreAsync();
                co_await writer.FlushAsync();

                if (connectionClosed)
                {
                    break;
                }
            }
            writer.DetachStream();
            reader.DetachStream();
        }
        catch (...)
        {
            // The client disconnected or the socket failed; the listener stays alive.
        }
    }

    std::wstring WorkerCommandServer::RegisterTrustedController(JsonObject const& request, fs::path const& root)
    {
        m_sessionRuntime.RequirePairing(request);

        auto controllerId = GetOptionalString(request, L"controller_id");
        if (!IsSafeIdentifier(controllerId, 64))
        {
            throw WorkerProtocolError("trust.controller_id_invalid", "controller_id must use 1..64 ASCII letters, digits, '-' or '_'");
        }

        auto publicKeyBase64 = GetOptionalString(request, L"public_key_spki_base64");
        if (publicKeyBase64.empty())
        {
            throw WorkerProtocolError("trust.public_key_required", "public_key_spki_base64 is required");
        }

        std::vector<uint8_t> publicKeyBytes;
        try
        {
            publicKeyBytes = Base64Decode(WideToUtf8(publicKeyBase64));
        }
        catch (...)
        {
            throw WorkerProtocolError("trust.public_key_invalid", "public_key_spki_base64 is not valid base64");
        }
        if (publicKeyBytes.empty() || publicKeyBytes.size() > MaxControllerPublicKeyBytes)
        {
            throw WorkerProtocolError("trust.public_key_invalid", "public key size is outside the supported range");
        }

        auto path = TrustedControllerPath(root, controllerId);
        auto replacing = fs::exists(path);
        auto currentCount = CountTrustedControllers(root);
        if (!replacing && currentCount >= MaxTrustedControllers)
        {
            throw WorkerProtocolError("trust.controller_limit", "trusted controller limit reached");
        }

        auto label = GetOptionalString(request, L"controller_label");
        if (label.size() > 80)
        {
            throw WorkerProtocolError("trust.label_too_long", "controller_label exceeds 80 characters");
        }

        auto fingerprint = Sha256BytesHex(publicKeyBytes);
        auto createdUnix = CurrentUnixSeconds();
        std::wstring document =
            L"{\"schema_version\":\"worker-trusted-controller-0.1\"" +
            std::wstring(L",\"controller_id\":") + JsonString(controllerId) +
            L",\"controller_label\":" + JsonString(label) +
            L",\"public_key_algorithm\":\"RSASIGN_PKCS1_SHA256\"" +
            L",\"public_key_blob_type\":\"X509SubjectPublicKeyInfo\"" +
            L",\"public_key_spki_base64\":" + JsonString(publicKeyBase64) +
            L",\"public_key_sha256\":" + JsonString(fingerprint) +
            L",\"created_unix_seconds\":" + std::to_wstring(createdUnix) +
            L",\"xbox_stores_private_key\":false" +
            L",\"xbox_stores_pairing_code\":false" +
            L"}";

        WriteInternalTextFile(path, WideToUtf8(document));
        auto finalCount = CountTrustedControllers(root);

        return OkBase(L"register_trusted_controller") +
            L",\"trust_schema\":\"worker-trusted-controller-0.1\"" +
            L",\"controller_id\":" + JsonString(controllerId) +
            L",\"public_key_sha256\":" + JsonString(fingerprint) +
            L",\"public_key_algorithm\":\"RSASIGN_PKCS1_SHA256\"" +
            L",\"replaced\":" + BoolJson(replacing) +
            L",\"trusted_controller_count\":" + std::to_wstring(finalCount) +
            L",\"xbox_stores_private_key\":false" +
            L",\"xbox_stores_pairing_code\":false" +
            L"}";
    }

    std::wstring WorkerCommandServer::ListTrustedControllers(fs::path const& root)
    {
        auto directory = TrustedControllerDirectory(root);
        std::vector<std::wstring> entries;
        if (fs::exists(directory))
        {
            for (auto const& entry : fs::directory_iterator(directory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".json")
                {
                    continue;
                }
                try
                {
                    auto document = JsonObject::Parse(Utf8ToWide(ReadTextFile(entry.path())));
                    auto controllerId = GetOptionalString(document, L"controller_id");
                    auto label = GetOptionalString(document, L"controller_label");
                    auto fingerprint = GetOptionalString(document, L"public_key_sha256");
                    auto created = GetOptionalUInt64(document, L"created_unix_seconds", 0);
                    entries.push_back(
                        L"{\"controller_id\":" + JsonString(controllerId) +
                        L",\"controller_label\":" + JsonString(label) +
                        L",\"public_key_sha256\":" + JsonString(fingerprint) +
                        L",\"created_unix_seconds\":" + std::to_wstring(created) +
                        L"}");
                }
                catch (...)
                {
                }
            }
        }
        std::sort(entries.begin(), entries.end());

        std::wostringstream items;
        items << L"[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i != 0)
            {
                items << L",";
            }
            items << entries[i];
        }
        items << L"]";

        return OkBase(L"list_trusted_controllers") +
            L",\"trust_schema\":\"worker-trusted-controller-0.1\"" +
            L",\"trusted_controller_count\":" + std::to_wstring(entries.size()) +
            L",\"trusted_controllers\":" + items.str() +
            L"}";
    }

    std::wstring WorkerCommandServer::RemoveTrustedController(JsonObject const& request, fs::path const& root)
    {
        auto controllerId = GetOptionalString(request, L"controller_id");
        auto path = TrustedControllerPath(root, controllerId);
        auto existed = fs::exists(path);
        auto removed = existed && fs::remove(path);
        return OkBase(L"remove_trusted_controller") +
            L",\"controller_id\":" + JsonString(controllerId) +
            L",\"existed\":" + BoolJson(existed) +
            L",\"removed\":" + BoolJson(removed) +
            L",\"trusted_controller_count\":" + std::to_wstring(CountTrustedControllers(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::OpenSessionWithTrust(JsonObject const& request, fs::path const& root)
    {
        auto controllerId = GetOptionalString(request, L"controller_id");
        if (!IsSafeIdentifier(controllerId, 64))
        {
            throw WorkerProtocolError("trust.controller_id_invalid", "controller_id must use 1..64 ASCII letters, digits, '-' or '_'");
        }

        auto nonce = GetOptionalString(request, L"nonce");
        if (!IsSafeIdentifier(nonce, 96))
        {
            throw WorkerProtocolError("trust.nonce_invalid", "nonce must use 1..96 ASCII letters, digits, '-' or '_'");
        }

        auto signatureBase64 = GetOptionalString(request, L"signature_base64");
        if (signatureBase64.empty())
        {
            throw WorkerProtocolError("trust.signature_required", "signature_base64 is required");
        }

        auto timestamp = static_cast<int64_t>(GetOptionalUInt64(request, L"timestamp_unix_seconds", 0));
        if (timestamp <= 0)
        {
            throw WorkerProtocolError("trust.timestamp_required", "timestamp_unix_seconds is required");
        }
        auto now = CurrentUnixSeconds();
        auto skew = now > timestamp ? now - timestamp : timestamp - now;
        if (skew > MaxTrustClockSkewSeconds)
        {
            throw WorkerProtocolError("trust.timestamp_out_of_window", "timestamp_unix_seconds is outside the allowed clock-skew window");
        }

        auto requestedTtl = GetOptionalUInt64(request, L"ttl_seconds", DefaultSessionTtlSeconds);
        auto ttl = (std::min)(MaxSessionTtlSeconds, (std::max)(MinSessionTtlSeconds, requestedTtl));
        auto controllerPath = TrustedControllerPath(root, controllerId);
        if (!fs::is_regular_file(controllerPath))
        {
            throw WorkerProtocolError("trust.controller_unknown", "trusted controller was not found");
        }

        auto controller = JsonObject::Parse(Utf8ToWide(ReadTextFile(controllerPath)));
        auto publicKeyBase64 = GetOptionalString(controller, L"public_key_spki_base64");
        auto fingerprint = GetOptionalString(controller, L"public_key_sha256");
        if (publicKeyBase64.empty() || fingerprint.empty())
        {
            throw WorkerProtocolError("trust.controller_invalid", "trusted controller record is incomplete");
        }

        PruneTrustNonces(root);
        auto noncePath = TrustNoncePath(root, controllerId, nonce);
        if (fs::exists(noncePath))
        {
            throw WorkerProtocolError("trust.nonce_replay", "nonce was already used");
        }

        auto canonical =
            L"xcompute-worker-trust-v1\n" +
            controllerId + L"\n" +
            nonce + L"\n" +
            std::to_wstring(timestamp) + L"\n" +
            std::to_wstring(ttl);
        if (!VerifyControllerSignature(publicKeyBase64, canonical, signatureBase64))
        {
            throw WorkerProtocolError("trust.signature_invalid", "trusted controller signature did not verify");
        }

        WriteInternalTextFile(noncePath,
            WideToUtf8(
                L"{\"schema_version\":\"worker-trust-nonce-0.1\"" +
                std::wstring(L",\"controller_id\":") + JsonString(controllerId) +
                L",\"nonce_sha256\":" + JsonString(Sha256TextHex(WideToUtf8(nonce))) +
                L",\"used_unix_seconds\":" + std::to_wstring(now) +
                L"}"));

        auto extra =
            L",\"auth_method\":\"trusted_controller_signature\"" +
            std::wstring(L",\"trust_schema\":\"worker-trusted-controller-0.1\"") +
            L",\"controller_id\":" + JsonString(controllerId) +
            L",\"public_key_sha256\":" + JsonString(fingerprint) +
            L",\"signature_verified\":true" +
            L",\"nonce_recorded\":true" +
            L",\"xbox_stores_private_key\":false" +
            L",\"xbox_stores_pairing_code\":false";
        return m_sessionRuntime.CreateSession(ttl, L"open_session_with_trust", extra);
    }

    std::wstring WorkerCommandServer::ProcessBinaryCommand(JsonObject const& request, std::vector<uint8_t> const& data)
    {
        auto command = GetOptionalString(request, L"command");
        auto root = WorkerRoot();
        EnsureWorkspace(root);
        auto authStarted = std::chrono::steady_clock::now();
        m_protocolBoundary.RequireBinaryAuthorized(request, command);
        auto authMs = ElapsedMilliseconds(authStarted);
        authMs += GetOptionalDoubleNoThrow(request, L"_server_pre_auth_ms", 0.0);
        if (command == L"write_chunks_stream")
        {
            return ExecuteWriteChunksStream(request, root, data, authMs);
        }
        if (command == L"append_artifact_chunk_binary")
        {
            return ExecuteAppendArtifactChunkBinary(request, root, data, authMs);
        }
        return ExecuteWriteChunkBinary(request, root, data, authMs);
    }

    std::wstring WorkerCommandServer::RunD3D12DeviceProbeJob(JsonObject const& request, fs::path const& root)
    {
        (void)request;
        auto started = std::chrono::steady_clock::now();
        auto probe = WorkerRunD3D12DeviceProbe();
        auto elapsedMs = ElapsedMilliseconds(started);

        auto selectedAdapterFound = probe.selectedAdapterIndex != UINT32_MAX;
        auto selectedAdapterJson = selectedAdapterFound
            ? D3D12AdapterProbeJson(probe.selectedAdapter)
            : std::wstring(L"null");

        return OkBase(L"run_d3d12_device_probe_job") +
            L",\"job\":\"d3d12_device_probe\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_device_creation_probe\"" +
            L",\"boundary\":\"device_probe_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"gdk_claim\":false" +
            L",\"d3d12_available\":" + BoolJson(probe.deviceCreated) +
            L",\"verified\":" + BoolJson(probe.deviceCreated) +
            L",\"factory_created\":" + BoolJson(probe.factoryCreated) +
            L",\"factory_hresult\":" + JsonString(HResultToString(probe.factoryHr)) +
            L",\"adapter_count\":" + std::to_wstring(probe.adapterCount) +
            L",\"selected_adapter_found\":" + BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" + std::to_wstring(probe.selectedAdapterIndex) +
            L",\"selected_adapter\":" + selectedAdapterJson +
            L",\"d3d12_probe_succeeded\":" + BoolJson(probe.d3d12ProbeSucceeded) +
            L",\"probe_null_hresult\":" + JsonString(HResultToString(probe.probeNullHr)) +
            L",\"device_created\":" + BoolJson(probe.deviceCreated) +
            L",\"create_device_hresult\":" + JsonString(HResultToString(probe.createDeviceHr)) +
            L",\"feature_levels_checked\":" + BoolJson(probe.featureLevelsChecked) +
            L",\"check_feature_levels_hresult\":" + JsonString(HResultToString(probe.checkFeatureLevelsHr)) +
            L",\"max_supported_feature_level\":" + JsonString(D3DFeatureLevelToString(probe.maxSupportedFeatureLevel)) +
            L",\"options_hresult\":" + JsonString(HResultToString(probe.optionsHr)) +
            L",\"resource_binding_tier\":" + std::to_wstring(static_cast<uint32_t>(probe.options.ResourceBindingTier)) +
            L",\"tiled_resources_tier\":" + std::to_wstring(static_cast<uint32_t>(probe.options.TiledResourcesTier)) +
            L",\"conservative_rasterization_tier\":" + std::to_wstring(static_cast<uint32_t>(probe.options.ConservativeRasterizationTier)) +
            L",\"standard_swizzle_64kb_supported\":" + BoolJson(probe.options.StandardSwizzle64KBSupported) +
            L",\"cross_node_sharing_tier\":" + std::to_wstring(static_cast<uint32_t>(probe.options.CrossNodeSharingTier)) +
            L",\"device_removed_reason\":" + JsonString(HResultToString(probe.deviceRemovedReason)) +
            L",\"app_memory_usage_bytes\":" + std::to_wstring(AppMemoryUsageBytes()) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(AppMemoryUsageLimitBytes()) +
            L",\"app_memory_usage_level\":" + std::to_wstring(AppMemoryUsageLevelValue()) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L",\"adapters\":" + D3D12AdapterListJson(probe.adapters) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D12ComputeSmokeJob(JsonObject const& request, fs::path const& root)
    {
        auto requestedElements = GetBoundedOptionalUInt64(request, L"elements", 4096, 1, MaxD3D11ComputeElements);
        auto started = std::chrono::steady_clock::now();
        auto measurement = WorkerRunD3D12ComputeSmoke(requestedElements);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto selectedAdapterFound = measurement.selectedAdapterIndex != UINT32_MAX;

        return OkBase(L"run_d3d12_compute_smoke_job") +
            L",\"job\":\"d3d12_compute_smoke\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_precompiled_compute_shader\"" +
            L",\"boundary\":\"precompiled_compute_smoke_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"gdk_claim\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(measurement.shaderBytes) +
            L",\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"device_created\":" + BoolJson(measurement.deviceCreated) +
            L",\"root_signature_created\":" + BoolJson(measurement.rootSignatureCreated) +
            L",\"pipeline_state_created\":" + BoolJson(measurement.pipelineStateCreated) +
            L",\"command_queue_created\":" + BoolJson(measurement.commandQueueCreated) +
            L",\"command_list_created\":" + BoolJson(measurement.commandListCreated) +
            L",\"output_buffer_created\":" + BoolJson(measurement.outputBufferCreated) +
            L",\"readback_buffer_created\":" + BoolJson(measurement.readbackBufferCreated) +
            L",\"fence_completed\":" + BoolJson(measurement.fenceCompleted) +
            L",\"factory_hresult\":" + JsonString(HResultToString(measurement.factoryHr)) +
            L",\"create_device_hresult\":" + JsonString(HResultToString(measurement.createDeviceHr)) +
            L",\"root_signature_hresult\":" + JsonString(HResultToString(measurement.rootSignatureHr)) +
            L",\"pipeline_state_hresult\":" + JsonString(HResultToString(measurement.pipelineStateHr)) +
            L",\"create_command_queue_hresult\":" + JsonString(HResultToString(measurement.createCommandQueueHr)) +
            L",\"create_command_allocator_hresult\":" + JsonString(HResultToString(measurement.createCommandAllocatorHr)) +
            L",\"create_command_list_hresult\":" + JsonString(HResultToString(measurement.createCommandListHr)) +
            L",\"output_buffer_hresult\":" + JsonString(HResultToString(measurement.outputBufferHr)) +
            L",\"readback_buffer_hresult\":" + JsonString(HResultToString(measurement.readbackBufferHr)) +
            L",\"close_command_list_hresult\":" + JsonString(HResultToString(measurement.closeCommandListHr)) +
            L",\"create_fence_hresult\":" + JsonString(HResultToString(measurement.createFenceHr)) +
            L",\"signal_fence_hresult\":" + JsonString(HResultToString(measurement.signalFenceHr)) +
            L",\"map_hresult\":" + JsonString(HResultToString(measurement.mapHr)) +
            L",\"device_removed_reason\":" + JsonString(HResultToString(measurement.deviceRemovedReason)) +
            L",\"fence_value\":" + std::to_wstring(measurement.fenceValue) +
            L",\"completed_fence_value\":" + std::to_wstring(measurement.completedFenceValue) +
            L",\"gpu_submit_and_wait_ms\":" + DoubleJson(measurement.gpuSubmitAndWaitMs) +
            L",\"readback_verify_ms\":" + DoubleJson(measurement.readbackVerifyMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L",\"selected_adapter_found\":" + BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" + std::to_wstring(measurement.selectedAdapterIndex) +
            L",\"selected_adapter\":" + (selectedAdapterFound ? D3D12AdapterProbeJson(measurement.selectedAdapter) : std::wstring(L"null")) +
            L",\"app_memory_usage_bytes\":" + std::to_wstring(AppMemoryUsageBytes()) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(AppMemoryUsageLimitBytes()) +
            L",\"app_memory_usage_level\":" + std::to_wstring(AppMemoryUsageLevelValue()) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D12ComputeSweepJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 3, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 1, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto context = WorkerCreateD3D12ComputeContext();
        std::vector<D3D12ComputeSweepMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsProcessed = 0;
        uint64_t totalBytesProcessed = 0;
        double totalGpuSubmitAndWaitMs = 0.0;
        double totalReadbackVerifyMs = 0.0;
        uint64_t maxMismatchCount = 0;
        uint32_t aggregateHash32 = 2166136261u;
        bool verified = true;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D12ComputeSweepDispatch(context, requestedElements, repeats, warmupRepeats);
            totalElementsProcessed += measurement.elements * repeats;
            totalBytesProcessed += measurement.bytes * repeats;
            totalGpuSubmitAndWaitMs += measurement.totalGpuSubmitAndWaitMs;
            totalReadbackVerifyMs += measurement.readbackVerifyMs;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            verified = verified && measurement.verified && measurement.fenceCompleted;
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateElementsPerSecond = totalGpuSubmitAndWaitMs > 0.0
            ? static_cast<double>(totalElementsProcessed) / (totalGpuSubmitAndWaitMs / 1000.0)
            : 0.0;
        auto aggregateMiBPerSecond = totalGpuSubmitAndWaitMs > 0.0
            ? ((static_cast<double>(totalBytesProcessed) / 1048576.0) / (totalGpuSubmitAndWaitMs / 1000.0))
            : 0.0;

        std::wostringstream sweepJson;
        sweepJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                sweepJson << L",";
            }
            sweepJson << D3D12ComputeSweepMeasurementJson(measurements[i]);
        }
        sweepJson << L"]";

        auto selectedAdapterFound = context.selectedAdapterIndex != UINT32_MAX;
        return OkBase(L"run_d3d12_compute_sweep_job") +
            L",\"job\":\"d3d12_compute_sweep\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_precompiled_compute_shader_sweep\"" +
            L",\"boundary\":\"precompiled_compute_sweep_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"gdk_claim\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(context.shaderBytes) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"total_elements_processed\":" + std::to_wstring(totalElementsProcessed) +
            L",\"total_bytes_processed\":" + std::to_wstring(totalBytesProcessed) +
            L",\"total_gpu_submit_and_wait_ms\":" + DoubleJson(totalGpuSubmitAndWaitMs, 6) +
            L",\"total_readback_verify_ms\":" + DoubleJson(totalReadbackVerifyMs, 6) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_elements_per_second\":" + DoubleJson(aggregateElementsPerSecond, 1) +
            L",\"aggregate_mib_per_second\":" + DoubleJson(aggregateMiBPerSecond, 3) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"device_created\":" + BoolJson(context.deviceCreated) +
            L",\"root_signature_created\":" + BoolJson(context.rootSignatureCreated) +
            L",\"pipeline_state_created\":" + BoolJson(context.pipelineStateCreated) +
            L",\"command_queue_created\":" + BoolJson(context.commandQueueCreated) +
            L",\"factory_hresult\":" + JsonString(HResultToString(context.factoryHr)) +
            L",\"create_device_hresult\":" + JsonString(HResultToString(context.createDeviceHr)) +
            L",\"root_signature_hresult\":" + JsonString(HResultToString(context.rootSignatureHr)) +
            L",\"pipeline_state_hresult\":" + JsonString(HResultToString(context.pipelineStateHr)) +
            L",\"create_command_queue_hresult\":" + JsonString(HResultToString(context.createCommandQueueHr)) +
            L",\"selected_adapter_found\":" + BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" + std::to_wstring(context.selectedAdapterIndex) +
            L",\"selected_adapter\":" + (selectedAdapterFound ? D3D12AdapterProbeJson(context.selectedAdapter) : std::wstring(L"null")) +
            L",\"app_memory_usage_bytes\":" + std::to_wstring(AppMemoryUsageBytes()) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(AppMemoryUsageLimitBytes()) +
            L",\"app_memory_usage_level\":" + std::to_wstring(AppMemoryUsageLevelValue()) +
            L",\"sweep\":" + sweepJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D12ComputeTimingJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto context = WorkerCreateD3D12ComputeContext();
        std::vector<D3D12ComputeTimingMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsProcessed = 0;
        uint64_t totalBytesProcessed = 0;
        uint64_t timestampFrequency = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitAndWaitMs = 0.0;
        double totalReadbackVerifyMs = 0.0;
        uint64_t maxMismatchCount = 0;
        uint32_t aggregateHash32 = 2166136261u;
        bool verified = true;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D12ComputeTimingDispatch(context, requestedElements, repeats, warmupRepeats);
            totalElementsProcessed += measurement.elements * repeats;
            totalBytesProcessed += measurement.bytes * repeats;
            totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            totalCpuSubmitAndWaitMs += measurement.totalCpuSubmitAndWaitMs;
            totalReadbackVerifyMs += measurement.readbackVerifyMs;
            timestampFrequency = measurement.timestampFrequency;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            verified = verified && measurement.verified && measurement.fenceCompleted;
            timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
            gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateGpuElementsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalElementsProcessed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytesProcessed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageGpuDispatchMs = measurements.empty()
            ? 0.0
            : totalGpuDispatchMs / static_cast<double>(measurements.size() * repeats);
        auto averageCpuSubmitAndWaitMs = measurements.empty()
            ? 0.0
            : totalCpuSubmitAndWaitMs / static_cast<double>(measurements.size() * repeats);

        std::wostringstream timingJson;
        timingJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                timingJson << L",";
            }
            timingJson << D3D12ComputeTimingMeasurementJson(measurements[i]);
        }
        timingJson << L"]";

        auto selectedAdapterFound = context.selectedAdapterIndex != UINT32_MAX;
        return OkBase(L"run_d3d12_compute_timing_job") +
            L",\"job\":\"d3d12_compute_timing\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_precompiled_compute_shader_timestamp_timing\"" +
            L",\"boundary\":\"precompiled_compute_timestamp_timing_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"gdk_claim\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(context.shaderBytes) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"timestamp_frequency\":" + std::to_wstring(timestampFrequency) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"total_elements_processed\":" + std::to_wstring(totalElementsProcessed) +
            L",\"total_bytes_processed\":" + std::to_wstring(totalBytesProcessed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs, 6) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(averageGpuDispatchMs, 6) +
            L",\"total_cpu_submit_and_wait_ms\":" + DoubleJson(totalCpuSubmitAndWaitMs, 6) +
            L",\"avg_cpu_submit_and_wait_ms\":" + DoubleJson(averageCpuSubmitAndWaitMs, 6) +
            L",\"total_readback_verify_ms\":" + DoubleJson(totalReadbackVerifyMs, 6) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_gpu_elements_per_second\":" + DoubleJson(aggregateGpuElementsPerSecond, 1) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"device_created\":" + BoolJson(context.deviceCreated) +
            L",\"root_signature_created\":" + BoolJson(context.rootSignatureCreated) +
            L",\"pipeline_state_created\":" + BoolJson(context.pipelineStateCreated) +
            L",\"command_queue_created\":" + BoolJson(context.commandQueueCreated) +
            L",\"factory_hresult\":" + JsonString(HResultToString(context.factoryHr)) +
            L",\"create_device_hresult\":" + JsonString(HResultToString(context.createDeviceHr)) +
            L",\"root_signature_hresult\":" + JsonString(HResultToString(context.rootSignatureHr)) +
            L",\"pipeline_state_hresult\":" + JsonString(HResultToString(context.pipelineStateHr)) +
            L",\"create_command_queue_hresult\":" + JsonString(HResultToString(context.createCommandQueueHr)) +
            L",\"selected_adapter_found\":" + BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" + std::to_wstring(context.selectedAdapterIndex) +
            L",\"selected_adapter\":" + (selectedAdapterFound ? D3D12AdapterProbeJson(context.selectedAdapter) : std::wstring(L"null")) +
            L",\"app_memory_usage_bytes\":" + std::to_wstring(AppMemoryUsageBytes()) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(AppMemoryUsageLimitBytes()) +
            L",\"app_memory_usage_level\":" + std::to_wstring(AppMemoryUsageLevelValue()) +
            L",\"timing\":" + timingJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D12Fp32TimingJob(JsonObject const& request, fs::path const& root)
    {
        auto kernelId = GetOptionalString(request, L"kernel_id", L"vector_transform_fp32_v1");
        if (kernelId != L"vector_transform_fp32_v1")
        {
            throw WorkerProtocolError("d3d12_fp32_timing.kernel_id_invalid", "kernel_id must be vector_transform_fp32_v1 for run_d3d12_fp32_timing_job");
        }
        auto elementsList = ParseD3D12Fp32TimingElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto context = WorkerCreateD3D12ComputeContext(L"FloatCompute.cso", D3D12_COMMAND_LIST_TYPE_DIRECT);
        std::vector<D3D12FloatTimingMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsProcessed = 0;
        uint64_t totalBytesProcessed = 0;
        uint64_t timestampFrequency = 0;
        uint64_t totalFp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitAndWaitMs = 0.0;
        double totalReadbackVerifyMs = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        bool verified = true;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D12FloatTimingDispatch(context, requestedElements, repeats, warmupRepeats);
            totalElementsProcessed += measurement.elements * repeats;
            totalBytesProcessed += measurement.bytes * repeats;
            totalFp32OpsTimed += measurement.fp32OpsTimed;
            totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            totalCpuSubmitAndWaitMs += measurement.totalCpuSubmitAndWaitMs;
            totalReadbackVerifyMs += measurement.readbackVerifyMs;
            timestampFrequency = measurement.timestampFrequency;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            if (measurement.maxAbsError > maxAbsError)
            {
                maxAbsError = measurement.maxAbsError;
            }
            if (measurement.maxRelativeError > maxRelativeError)
            {
                maxRelativeError = measurement.maxRelativeError;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            verified = verified && measurement.verified && measurement.fenceCompleted;
            timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
            gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytesProcessed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageGpuDispatchMs = measurements.empty()
            ? 0.0
            : totalGpuDispatchMs / static_cast<double>(measurements.size() * repeats);
        auto averageCpuSubmitAndWaitMs = measurements.empty()
            ? 0.0
            : totalCpuSubmitAndWaitMs / static_cast<double>(measurements.size() * repeats);

        std::wostringstream timingJson;
        timingJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                timingJson << L",";
            }
            timingJson << D3D12FloatTimingMeasurementJson(measurements[i]);
        }
        timingJson << L"]";

        auto selectedAdapterFound = context.selectedAdapterIndex != UINT32_MAX;
        return OkBase(L"run_d3d12_fp32_timing_job") +
            L",\"kernel_id\":" + JsonString(kernelId) +
            L",\"job\":\"d3d12_fp32_timing\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_precompiled_fp32_shader_timestamp_timing\"" +
            L",\"boundary\":\"precompiled_fp32_timestamp_timing_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"gdk_claim\":false" +
            L",\"shader\":" + JsonString(context.shaderName) +
            L",\"shader_bytes\":" + std::to_wstring(context.shaderBytes) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"max_elements_supported\":" + std::to_wstring(MaxD3D12Fp32TimingElements) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(D3D11FloatOpsPerElement) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"timestamp_frequency\":" + std::to_wstring(timestampFrequency) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"total_elements_processed\":" + std::to_wstring(totalElementsProcessed) +
            L",\"total_bytes_processed\":" + std::to_wstring(totalBytesProcessed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs, 6) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(averageGpuDispatchMs, 6) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"total_cpu_submit_and_wait_ms\":" + DoubleJson(totalCpuSubmitAndWaitMs, 6) +
            L",\"avg_cpu_submit_and_wait_ms\":" + DoubleJson(averageCpuSubmitAndWaitMs, 6) +
            L",\"total_readback_verify_ms\":" + DoubleJson(totalReadbackVerifyMs, 6) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"device_created\":" + BoolJson(context.deviceCreated) +
            L",\"root_signature_created\":" + BoolJson(context.rootSignatureCreated) +
            L",\"pipeline_state_created\":" + BoolJson(context.pipelineStateCreated) +
            L",\"command_queue_created\":" + BoolJson(context.commandQueueCreated) +
            L",\"factory_hresult\":" + JsonString(HResultToString(context.factoryHr)) +
            L",\"create_device_hresult\":" + JsonString(HResultToString(context.createDeviceHr)) +
            L",\"root_signature_hresult\":" + JsonString(HResultToString(context.rootSignatureHr)) +
            L",\"pipeline_state_hresult\":" + JsonString(HResultToString(context.pipelineStateHr)) +
            L",\"create_command_queue_hresult\":" + JsonString(HResultToString(context.createCommandQueueHr)) +
            L",\"selected_adapter_found\":" + BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" + std::to_wstring(context.selectedAdapterIndex) +
            L",\"selected_adapter\":" + (selectedAdapterFound ? D3D12AdapterProbeJson(context.selectedAdapter) : std::wstring(L"null")) +
            L",\"app_memory_usage_bytes\":" + std::to_wstring(AppMemoryUsageBytes()) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(AppMemoryUsageLimitBytes()) +
            L",\"app_memory_usage_level\":" + std::to_wstring(AppMemoryUsageLevelValue()) +
            L",\"timing\":" + timingJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring D3D12ShaderShapeMatrixResultJson(
        std::wstring const& kernelId,
        D3D12ShaderShapeMatrixOptions const& options,
        D3D12ShaderShapeMatrixResult const& result,
        WorkerD3DEnvironmentObservation const& environment)
    {
        std::wostringstream variantsJson;
        variantsJson << L"[";
        for (size_t i = 0; i < result.summaries.size(); ++i)
        {
            if (i != 0)
            {
                variantsJson << L",";
            }
            variantsJson << D3D11ShaderMatrixVariantSummaryJson(
                result.summaries[i]);
        }
        variantsJson << L"]";

        std::wostringstream matrixJson;
        matrixJson << L"[";
        bool first = true;
        for (auto const& summary : result.summaries)
        {
            for (auto const& measurement : summary.measurements)
            {
                if (!first)
                {
                    matrixJson << L",";
                }
                first = false;
                matrixJson << D3D11ShaderMatrixMeasurementJson(measurement);
            }
        }
        matrixJson << L"]";

        auto selectedAdapterFound =
            result.selectedAdapterIndex != UINT32_MAX;
        return OkBase(L"run_d3d12_shader_shape_job") +
            L",\"kernel_id\":" + JsonString(kernelId) +
            L",\"job\":\"d3d12_shader_shape\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"public_uwp_d3d12_precompiled_shader_shape_matrix\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only\"" +
            L",\"boundary\":\"precompiled_shader_shape_matrix_only_no_gdk_claim\"" +
            L",\"dynamic_code\":false" +
            L",\"runtime_shader_compilation_used\":false" +
            L",\"gdk_claim\":false" +
            L",\"max_elements_supported\":" +
                std::to_wstring(MaxD3D12ShaderShapeElements) +
            L",\"threads_per_group\":" +
                std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(options.repeats) +
            L",\"warmup_repeats\":" +
                std::to_wstring(options.warmupRepeats) +
            L",\"variant_count\":" +
                std::to_wstring(result.summaries.size()) +
            L",\"element_count\":" +
                std::to_wstring(options.elementsList.size()) +
            L",\"result_count\":" + std::to_wstring(result.resultCount) +
            L",\"timestamp_query_supported\":" +
                BoolJson(result.timestampQuerySupported) +
            L",\"gpu_timing_available\":" +
                BoolJson(result.gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" +
                std::to_wstring(result.totalGpuTimingSampleCount) +
            L",\"total_elements_timed\":" +
                std::to_wstring(result.totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" +
                std::to_wstring(result.totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" +
                std::to_wstring(result.totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" +
                DoubleJson(result.totalGpuDispatchMs) +
            L",\"aggregate_fp32_ops_per_second\":" +
                DoubleJson(result.aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" +
                DoubleJson(result.aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" +
                DoubleJson(result.aggregateGpuMiBPerSecond, 3) +
            L",\"best_fp32_variant_id\":" +
                JsonString(result.bestFp32VariantId) +
            L",\"best_fp32_variant_gflops\":" +
                DoubleJson(result.bestFp32VariantGflops, 3) +
            L",\"best_bandwidth_variant_id\":" +
                JsonString(result.bestBandwidthVariantId) +
            L",\"best_bandwidth_mib_per_second\":" +
                DoubleJson(result.bestBandwidthMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" +
                std::to_wstring(result.totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" +
                DoubleJson(result.totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" +
                DoubleJson(result.averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" +
                DoubleJson(result.totalVerificationReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(result.elapsedMs) +
            L",\"aggregate_hash32\":" +
                std::to_wstring(result.aggregateHash32) +
            L",\"max_mismatch_count\":" +
                std::to_wstring(result.maxMismatchCount) +
            L",\"max_abs_error\":" +
                DoubleJson(result.maxAbsError, 6) +
            L",\"max_relative_error\":" +
                DoubleJson(result.maxRelativeError, 9) +
            L",\"absolute_tolerance\":" +
                DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" +
                DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"verified\":" + BoolJson(result.verified) +
            L",\"device_created\":" + BoolJson(result.deviceCreated) +
            L",\"root_signature_created\":" +
                BoolJson(result.rootSignatureCreated) +
            L",\"pipeline_state_created\":" +
                BoolJson(result.pipelineStateCreated) +
            L",\"command_queue_created\":" +
                BoolJson(result.commandQueueCreated) +
            L",\"selected_adapter_found\":" +
                BoolJson(selectedAdapterFound) +
            L",\"selected_adapter_index\":" +
                std::to_wstring(result.selectedAdapterIndex) +
            L",\"selected_adapter\":" +
                (selectedAdapterFound
                    ? D3D12AdapterProbeJson(result.selectedAdapter)
                    : std::wstring(L"null")) +
            L",\"app_memory_usage_bytes\":" +
                std::to_wstring(environment.appMemoryUsageBytes) +
            L",\"app_memory_usage_limit_bytes\":" +
                std::to_wstring(environment.appMemoryUsageLimitBytes) +
            L",\"app_memory_usage_level\":" +
                std::to_wstring(environment.appMemoryUsageLevel) +
            L",\"variants\":" + variantsJson.str() +
            L",\"matrix\":" + matrixJson.str() +
            L",\"workspace_bytes\":" +
                std::to_wstring(environment.workspaceBytes) +
            L"}";
    }

    static std::wstring ExecuteRunD3D12ShaderShapeJob(JsonObject const& request, fs::path const& root)
    {
        auto kernelId = GetOptionalString(request, L"kernel_id", L"matrix_block_fp32_v1");
        if (kernelId != L"matrix_block_fp32_v1")
        {
            throw WorkerProtocolError("d3d12_shader_shape.kernel_id_invalid", "kernel_id must be matrix_block_fp32_v1 for run_d3d12_shader_shape_job");
        }
        auto elementsList = ParseD3D12ShaderShapeElementsList(GetOptionalString(request, L"elements_list"));
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variants"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        D3D12ShaderShapeMatrixOptions options;
        options.elementsList = elementsList;
        options.variants = variants;
        options.repeats = repeats;
        options.warmupRepeats = warmupRepeats;
        auto runtimeResult = WorkerRunD3D12ShaderShapeMatrix(options);
        return D3D12ShaderShapeMatrixResultJson(
            kernelId,
            options,
            runtimeResult,
            ObserveD3DEnvironment(root));
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D12ShaderShapeJob(JsonObject const& request, fs::path const& root)
    {
        return ExecuteRunD3D12ShaderShapeJob(request, root);
    }

    std::wstring WorkerCommandServer::RunD3D11ComputeJob(JsonObject const& request, fs::path const& root)
    {
        auto requestedElements = GetBoundedOptionalUInt64(request, L"elements", 4096, 1, MaxD3D11ComputeElements);
        auto started = std::chrono::steady_clock::now();
        auto compute = WorkerCreateD3D11ComputeContext();
        auto measurement = WorkerRunD3D11ComputeDispatch(compute, requestedElements, 1, 0);
        auto elapsedMs = ElapsedMilliseconds(started);

        return OkBase(L"run_d3d11_compute_job") +
            L",\"job\":\"d3d11_compute\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_compute_shader\"" +
            L",\"dynamic_code\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(compute.shaderBytes) +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(compute.featureLevel)) +
            L",\"requested_elements\":" + std::to_wstring(measurement.requestedElements) +
            L",\"elements\":" + std::to_wstring(measurement.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(measurement.dispatchGroups) +
            L",\"bytes\":" + std::to_wstring(measurement.bytes) +
            L",\"dispatch_and_readback_ms\":" + DoubleJson(measurement.totalDispatchAndReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"elements_per_second\":" + DoubleJson(measurement.elementsPerSecond, 1) +
            L",\"first_value\":" + std::to_wstring(measurement.firstValue) +
            L",\"last_value\":" + std::to_wstring(measurement.lastValue) +
            L",\"checksum\":" + std::to_wstring(measurement.checksum) +
            L",\"hash32\":" + std::to_wstring(measurement.hash32) +
            L",\"mismatch_count\":" + std::to_wstring(measurement.mismatches) +
            L",\"verified\":" + BoolJson(measurement.verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11ComputeSweepJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 3, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 1, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto compute = WorkerCreateD3D11ComputeContext();
        std::vector<D3D11ComputeMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsProcessed = 0;
        uint64_t totalBytesProcessed = 0;
        double totalDispatchAndReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        uint32_t aggregateHash32 = 2166136261u;
        bool verified = true;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D11ComputeDispatch(compute, requestedElements, repeats, warmupRepeats);
            totalElementsProcessed += measurement.elements * repeats;
            totalBytesProcessed += measurement.bytes * repeats;
            totalDispatchAndReadbackMs += measurement.totalDispatchAndReadbackMs;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            verified = verified && measurement.verified;
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateElementsPerSecond = totalDispatchAndReadbackMs > 0.0
            ? static_cast<double>(totalElementsProcessed) / (totalDispatchAndReadbackMs / 1000.0)
            : 0.0;
        auto aggregateMiBPerSecond = totalDispatchAndReadbackMs > 0.0
            ? ((static_cast<double>(totalBytesProcessed) / 1048576.0) / (totalDispatchAndReadbackMs / 1000.0))
            : 0.0;

        std::wostringstream sweepJson;
        sweepJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                sweepJson << L",";
            }
            sweepJson << D3D11ComputeMeasurementJson(measurements[i]);
        }
        sweepJson << L"]";

        return OkBase(L"run_d3d11_compute_sweep_job") +
            L",\"job\":\"d3d11_compute_sweep\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_compute_shader_sweep\"" +
            L",\"dynamic_code\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(compute.shaderBytes) +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(compute.featureLevel)) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"total_elements_processed\":" + std::to_wstring(totalElementsProcessed) +
            L",\"total_bytes_processed\":" + std::to_wstring(totalBytesProcessed) +
            L",\"total_dispatch_and_readback_ms\":" + DoubleJson(totalDispatchAndReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_elements_per_second\":" + DoubleJson(aggregateElementsPerSecond, 1) +
            L",\"aggregate_mib_per_second\":" + DoubleJson(aggregateMiBPerSecond, 3) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"sweep\":" + sweepJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11ComputeTimingJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto compute = WorkerCreateD3D11ComputeContext();
        std::vector<D3D11ComputeTimingMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsTimed = 0;
        uint64_t totalBytesTimed = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        uint32_t aggregateHash32 = 2166136261u;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D11ComputeTimingDispatch(compute, requestedElements, repeats, warmupRepeats);
            totalElementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
            totalBytesTimed += measurement.bytes * measurement.gpuTimingSampleCount;
            totalGpuTimingSampleCount += measurement.gpuTimingSampleCount;
            totalGpuDisjointCount += measurement.gpuDisjointCount;
            totalCpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
            totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            totalCpuSubmitMs += measurement.totalCpuSubmitMs;
            totalVerificationReadbackMs += measurement.verificationReadbackMs;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
            gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
            verified = verified && measurement.verified;
            if (firstTimingError.empty() && !measurement.timingError.empty())
            {
                firstTimingError = measurement.timingError;
            }
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateGpuElementsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalElementsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;

        std::wostringstream timingJson;
        timingJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                timingJson << L",";
            }
            timingJson << D3D11ComputeTimingMeasurementJson(measurements[i]);
        }
        timingJson << L"]";

        return OkBase(L"run_d3d11_compute_timing_job") +
            L",\"job\":\"d3d11_compute_timing\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_compute_shader_timestamp_query\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only\"" +
            L",\"dynamic_code\":false" +
            L",\"shader\":\"IntCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(compute.shaderBytes) +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(compute.featureLevel)) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_bytes_timed\":" + std::to_wstring(totalBytesTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs) +
            L",\"aggregate_gpu_elements_per_second\":" + DoubleJson(aggregateGpuElementsPerSecond, 1) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(totalVerificationReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"timing\":" + timingJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11Fp32TimingJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        auto compute = WorkerCreateD3D11ComputeContext(L"FloatCompute.cso");
        std::vector<D3D11FloatTimingMeasurement> measurements;
        measurements.reserve(elementsList.size());

        uint64_t totalElementsTimed = 0;
        uint64_t totalBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        uint32_t aggregateHash32 = 2166136261u;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;

        for (auto requestedElements : elementsList)
        {
            auto measurement = WorkerRunD3D11FloatTimingDispatch(compute, requestedElements, repeats, warmupRepeats);
            totalElementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
            totalBytesTimed += measurement.bytes * measurement.gpuTimingSampleCount;
            totalFp32OpsTimed += measurement.fp32OpsTimed;
            totalGpuTimingSampleCount += measurement.gpuTimingSampleCount;
            totalGpuDisjointCount += measurement.gpuDisjointCount;
            totalCpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
            totalGpuDispatchMs += measurement.totalGpuDispatchMs;
            totalCpuSubmitMs += measurement.totalCpuSubmitMs;
            totalVerificationReadbackMs += measurement.verificationReadbackMs;
            if (measurement.mismatches > maxMismatchCount)
            {
                maxMismatchCount = measurement.mismatches;
            }
            if (measurement.maxAbsError > maxAbsError)
            {
                maxAbsError = measurement.maxAbsError;
            }
            if (measurement.maxRelativeError > maxRelativeError)
            {
                maxRelativeError = measurement.maxRelativeError;
            }
            aggregateHash32 ^= measurement.hash32;
            aggregateHash32 *= 16777619u;
            timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
            gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
            verified = verified && measurement.verified;
            if (firstTimingError.empty() && !measurement.timingError.empty())
            {
                firstTimingError = measurement.timingError;
            }
            measurements.push_back(measurement);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;

        std::wostringstream timingJson;
        timingJson << L"[";
        for (size_t i = 0; i < measurements.size(); ++i)
        {
            if (i != 0)
            {
                timingJson << L",";
            }
            timingJson << D3D11FloatTimingMeasurementJson(measurements[i]);
        }
        timingJson << L"]";

        return OkBase(L"run_d3d11_fp32_timing_job") +
            L",\"job\":\"d3d11_fp32_timing\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_fp32_alu_shader_timestamp_query\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only\"" +
            L",\"dynamic_code\":false" +
            L",\"shader\":\"FloatCompute.cso\"" +
            L",\"shader_bytes\":" + std::to_wstring(compute.shaderBytes) +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(compute.featureLevel)) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(D3D11FloatOpsPerElement) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"result_count\":" + std::to_wstring(measurements.size()) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_bytes_timed\":" + std::to_wstring(totalBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(totalVerificationReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"timing\":" + timingJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11ShaderMatrixJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variants"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11ComputeSweepRepeats);

        auto started = std::chrono::steady_clock::now();
        std::vector<D3D11ShaderMatrixVariantSummary> summaries;
        summaries.reserve(variants.size());

        uint64_t resultCount = 0;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;
        std::wstring featureLevel = L"";

        for (auto const& variant : variants)
        {
            auto compute = WorkerCreateD3D11ComputeContext(variant.shaderFileName);
            if (featureLevel.empty())
            {
                featureLevel = D3DFeatureLevelToString(compute.featureLevel);
            }

            D3D11ShaderMatrixVariantSummary summary;
            summary.variant = variant;
            summary.shaderBytes = compute.shaderBytes;
            summary.featureLevel = compute.featureLevel;
            summary.measurements.reserve(elementsList.size());

            for (auto requestedElements : elementsList)
            {
                auto measurement = WorkerRunD3D11ShaderMatrixTimingDispatch(compute, variant, requestedElements, repeats, warmupRepeats);
                summary.totalElementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
                summary.totalGpuBytesTimed += measurement.gpuBytesTimed;
                summary.totalFp32OpsTimed += measurement.fp32OpsTimed;
                summary.totalGpuTimingSampleCount += measurement.gpuTimingSampleCount;
                summary.totalGpuDisjointCount += measurement.gpuDisjointCount;
                summary.totalCpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
                summary.totalGpuDispatchMs += measurement.totalGpuDispatchMs;
                summary.totalCpuSubmitMs += measurement.totalCpuSubmitMs;
                summary.totalVerificationReadbackMs += measurement.verificationReadbackMs;
                if (measurement.mismatches > summary.maxMismatchCount)
                {
                    summary.maxMismatchCount = measurement.mismatches;
                }
                if (measurement.maxAbsError > summary.maxAbsError)
                {
                    summary.maxAbsError = measurement.maxAbsError;
                }
                if (measurement.maxRelativeError > summary.maxRelativeError)
                {
                    summary.maxRelativeError = measurement.maxRelativeError;
                }
                summary.aggregateHash32 ^= measurement.hash32;
                summary.aggregateHash32 *= 16777619u;
                summary.timestampQuerySupported = summary.timestampQuerySupported && measurement.timestampQuerySupported;
                summary.gpuTimingAvailable = summary.gpuTimingAvailable && measurement.gpuTimingAvailable;
                summary.verified = summary.verified && measurement.verified;
                if (summary.firstTimingError.empty() && !measurement.timingError.empty())
                {
                    summary.firstTimingError = measurement.timingError;
                }

                totalElementsTimed += measurement.elements * measurement.gpuTimingSampleCount;
                totalGpuBytesTimed += measurement.gpuBytesTimed;
                totalFp32OpsTimed += measurement.fp32OpsTimed;
                totalGpuTimingSampleCount += measurement.gpuTimingSampleCount;
                totalGpuDisjointCount += measurement.gpuDisjointCount;
                totalCpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
                totalGpuDispatchMs += measurement.totalGpuDispatchMs;
                totalCpuSubmitMs += measurement.totalCpuSubmitMs;
                totalVerificationReadbackMs += measurement.verificationReadbackMs;
                if (measurement.mismatches > maxMismatchCount)
                {
                    maxMismatchCount = measurement.mismatches;
                }
                if (measurement.maxAbsError > maxAbsError)
                {
                    maxAbsError = measurement.maxAbsError;
                }
                if (measurement.maxRelativeError > maxRelativeError)
                {
                    maxRelativeError = measurement.maxRelativeError;
                }
                aggregateHash32 ^= measurement.hash32;
                aggregateHash32 *= 16777619u;
                timestampQuerySupported = timestampQuerySupported && measurement.timestampQuerySupported;
                gpuTimingAvailable = gpuTimingAvailable && measurement.gpuTimingAvailable;
                verified = verified && measurement.verified;
                if (firstTimingError.empty() && !measurement.timingError.empty())
                {
                    firstTimingError = measurement.timingError;
                }
                ++resultCount;
                summary.measurements.push_back(measurement);
            }

            summaries.push_back(summary);
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalGpuBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;

        std::wostringstream variantsJson;
        variantsJson << L"[";
        for (size_t i = 0; i < summaries.size(); ++i)
        {
            if (i != 0)
            {
                variantsJson << L",";
            }
            variantsJson << D3D11ShaderMatrixVariantSummaryJson(summaries[i]);
        }
        variantsJson << L"]";

        std::wostringstream matrixJson;
        matrixJson << L"[";
        bool first = true;
        for (auto const& summary : summaries)
        {
            for (auto const& measurement : summary.measurements)
            {
                if (!first)
                {
                    matrixJson << L",";
                }
                first = false;
                matrixJson << D3D11ShaderMatrixMeasurementJson(measurement);
            }
        }
        matrixJson << L"]";

        return OkBase(L"run_d3d11_shader_matrix_job") +
            L",\"job\":\"d3d11_shader_matrix\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_shader_matrix_timestamp_query\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only\"" +
            L",\"dynamic_code\":false" +
            L",\"feature_level\":" + JsonString(featureLevel) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"variant_count\":" + std::to_wstring(summaries.size()) +
            L",\"element_count\":" + std::to_wstring(elementsList.size()) +
            L",\"result_count\":" + std::to_wstring(resultCount) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" + std::to_wstring(totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(totalVerificationReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"variants\":" + variantsJson.str() +
            L",\"matrix\":" + matrixJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11ShaderMatrixStatsJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsList = ParseD3D11ComputeElementsList(GetOptionalString(request, L"elements_list"));
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variants"));
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 5, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 1, 0, MaxD3D11ComputeSweepRepeats);
        auto batchCount = GetBoundedOptionalUInt64(request, L"batch_count", 3, 1, MaxD3D11ShaderMatrixStatsBatches);
        auto batchPauseMs = GetBoundedOptionalUInt64(request, L"batch_pause_ms", 0, 0, MaxD3D11ShaderMatrixStatsPauseMs);

        auto started = std::chrono::steady_clock::now();
        std::vector<D3D11ShaderMatrixStatsMeasurement> statsMeasurements;
        statsMeasurements.reserve(variants.size() * elementsList.size());

        uint64_t totalStatsResultCount = 0;
        uint64_t totalGpuTimingSampleCount = 0;
        uint64_t totalGpuDisjointCount = 0;
        uint64_t totalCpuSubmitSampleCount = 0;
        uint64_t totalElementsTimed = 0;
        uint64_t totalGpuBytesTimed = 0;
        uint64_t totalFp32OpsTimed = 0;
        double totalGpuDispatchMs = 0.0;
        double totalCpuSubmitMs = 0.0;
        double totalVerificationReadbackMs = 0.0;
        uint64_t maxMismatchCount = 0;
        double maxAbsError = 0.0;
        double maxRelativeError = 0.0;
        uint32_t aggregateHash32 = 2166136261u;
        bool timestampQuerySupported = true;
        bool gpuTimingAvailable = true;
        bool verified = true;
        std::wstring firstTimingError;
        std::wstring featureLevel = L"";
        std::vector<double> allGpuDispatchSamplesMs;
        std::vector<double> allGflopsSamples;
        std::vector<double> allGpuMiBPerSecondSamples;

        for (auto const& variant : variants)
        {
            auto compute = WorkerCreateD3D11ComputeContext(variant.shaderFileName);
            if (featureLevel.empty())
            {
                featureLevel = D3DFeatureLevelToString(compute.featureLevel);
            }

            for (auto requestedElements : elementsList)
            {
                D3D11ShaderMatrixStatsMeasurement stats;
                stats.variant = variant;
                stats.requestedElements = requestedElements;
                stats.repeats = repeats;
                stats.warmupRepeats = warmupRepeats;
                stats.batchCount = batchCount;
                stats.batchPauseMs = batchPauseMs;
                stats.shaderBytes = compute.shaderBytes;

                for (uint64_t batch = 0; batch < batchCount; ++batch)
                {
                    auto measurement = WorkerRunD3D11ShaderMatrixTimingDispatch(compute, variant, requestedElements, repeats, warmupRepeats);
                    if (batch == 0)
                    {
                        stats.elements = measurement.elements;
                        stats.dispatchGroups = measurement.dispatchGroups;
                        stats.inputBytes = measurement.inputBytes;
                        stats.outputBytes = measurement.outputBytes;
                        stats.gpuBytesPerDispatch = measurement.gpuBytesPerDispatch;
                    }

                    stats.gpuTimingSampleCount += measurement.gpuTimingSampleCount;
                    stats.gpuDisjointCount += measurement.gpuDisjointCount;
                    stats.cpuSubmitSampleCount += measurement.cpuSubmitSampleCount;
                    stats.totalGpuDispatchMs += measurement.totalGpuDispatchMs;
                    stats.totalCpuSubmitMs += measurement.totalCpuSubmitMs;
                    stats.totalVerificationReadbackMs += measurement.verificationReadbackMs;
                    if (measurement.mismatches > stats.maxMismatchCount)
                    {
                        stats.maxMismatchCount = measurement.mismatches;
                    }
                    if (measurement.maxAbsError > stats.maxAbsError)
                    {
                        stats.maxAbsError = measurement.maxAbsError;
                    }
                    if (measurement.maxRelativeError > stats.maxRelativeError)
                    {
                        stats.maxRelativeError = measurement.maxRelativeError;
                    }
                    stats.aggregateHash32 ^= measurement.hash32;
                    stats.aggregateHash32 *= 16777619u;
                    stats.timestampQuerySupported = stats.timestampQuerySupported && measurement.timestampQuerySupported;
                    stats.gpuTimingAvailable = stats.gpuTimingAvailable && measurement.gpuTimingAvailable;
                    stats.verified = stats.verified && measurement.verified;
                    if (stats.firstTimingError.empty() && !measurement.timingError.empty())
                    {
                        stats.firstTimingError = measurement.timingError;
                    }

                    auto batchStats = ComputeDoubleSampleStats(measurement.gpuDispatchSamplesMs);
                    if (batchStats.count > 0)
                    {
                        stats.batchMedianGpuMs.push_back(batchStats.median);
                    }

                    for (auto value : measurement.cpuSubmitSamplesMs)
                    {
                        stats.cpuSubmitSamplesMs.push_back(value);
                    }
                    for (auto value : measurement.gpuDispatchSamplesMs)
                    {
                        stats.gpuDispatchSamplesMs.push_back(value);
                        auto sampleGflops = (variant.fp32OpsPerElement > 0 && value > 0.0)
                            ? ((static_cast<double>(measurement.elements * variant.fp32OpsPerElement) / (value / 1000.0)) / 1000000000.0)
                            : 0.0;
                        auto sampleMiBPerSecond = value > 0.0
                            ? ((static_cast<double>(measurement.gpuBytesPerDispatch) / 1048576.0) / (value / 1000.0))
                            : 0.0;
                        stats.gflopsSamples.push_back(sampleGflops);
                        stats.gpuMiBPerSecondSamples.push_back(sampleMiBPerSecond);
                    }

                    if (batchPauseMs > 0 && (batch + 1) < batchCount)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(batchPauseMs));
                    }
                }

                stats.fp32OpsTimed = stats.elements * stats.gpuTimingSampleCount * variant.fp32OpsPerElement;
                stats.gpuBytesTimed = stats.gpuBytesPerDispatch * stats.gpuTimingSampleCount;
                stats.gpuDispatchStats = ComputeDoubleSampleStats(stats.gpuDispatchSamplesMs);
                stats.cpuSubmitStats = ComputeDoubleSampleStats(stats.cpuSubmitSamplesMs);
                stats.gflopsStats = ComputeDoubleSampleStats(stats.gflopsSamples);
                stats.gpuMiBPerSecondStats = ComputeDoubleSampleStats(stats.gpuMiBPerSecondSamples);
                if (!stats.batchMedianGpuMs.empty())
                {
                    stats.firstBatchMedianGpuMs = stats.batchMedianGpuMs.front();
                    stats.lastBatchMedianGpuMs = stats.batchMedianGpuMs.back();
                    stats.driftGpuMs = stats.lastBatchMedianGpuMs - stats.firstBatchMedianGpuMs;
                    stats.driftPercent = stats.firstBatchMedianGpuMs != 0.0
                        ? (stats.driftGpuMs / stats.firstBatchMedianGpuMs) * 100.0
                        : 0.0;
                }

                totalStatsResultCount += 1;
                totalGpuTimingSampleCount += stats.gpuTimingSampleCount;
                totalGpuDisjointCount += stats.gpuDisjointCount;
                totalCpuSubmitSampleCount += stats.cpuSubmitSampleCount;
                totalElementsTimed += stats.elements * stats.gpuTimingSampleCount;
                totalGpuBytesTimed += stats.gpuBytesTimed;
                totalFp32OpsTimed += stats.fp32OpsTimed;
                totalGpuDispatchMs += stats.totalGpuDispatchMs;
                totalCpuSubmitMs += stats.totalCpuSubmitMs;
                totalVerificationReadbackMs += stats.totalVerificationReadbackMs;
                if (stats.maxMismatchCount > maxMismatchCount)
                {
                    maxMismatchCount = stats.maxMismatchCount;
                }
                if (stats.maxAbsError > maxAbsError)
                {
                    maxAbsError = stats.maxAbsError;
                }
                if (stats.maxRelativeError > maxRelativeError)
                {
                    maxRelativeError = stats.maxRelativeError;
                }
                aggregateHash32 ^= stats.aggregateHash32;
                aggregateHash32 *= 16777619u;
                timestampQuerySupported = timestampQuerySupported && stats.timestampQuerySupported;
                gpuTimingAvailable = gpuTimingAvailable && stats.gpuTimingAvailable;
                verified = verified && stats.verified;
                if (firstTimingError.empty() && !stats.firstTimingError.empty())
                {
                    firstTimingError = stats.firstTimingError;
                }
                allGpuDispatchSamplesMs.insert(allGpuDispatchSamplesMs.end(), stats.gpuDispatchSamplesMs.begin(), stats.gpuDispatchSamplesMs.end());
                allGflopsSamples.insert(allGflopsSamples.end(), stats.gflopsSamples.begin(), stats.gflopsSamples.end());
                allGpuMiBPerSecondSamples.insert(allGpuMiBPerSecondSamples.end(), stats.gpuMiBPerSecondSamples.begin(), stats.gpuMiBPerSecondSamples.end());
                statsMeasurements.push_back(stats);
            }
        }

        auto elapsedMs = ElapsedMilliseconds(started);
        auto aggregateFp32OpsPerSecond = totalGpuDispatchMs > 0.0
            ? static_cast<double>(totalFp32OpsTimed) / (totalGpuDispatchMs / 1000.0)
            : 0.0;
        auto aggregateGflops = aggregateFp32OpsPerSecond / 1000000000.0;
        auto aggregateGpuMiBPerSecond = totalGpuDispatchMs > 0.0
            ? ((static_cast<double>(totalGpuBytesTimed) / 1048576.0) / (totalGpuDispatchMs / 1000.0))
            : 0.0;
        auto averageCpuSubmitMs = totalCpuSubmitSampleCount > 0
            ? totalCpuSubmitMs / static_cast<double>(totalCpuSubmitSampleCount)
            : 0.0;
        auto aggregateGpuDispatchStats = ComputeDoubleSampleStats(allGpuDispatchSamplesMs);
        auto aggregateGflopsStats = ComputeDoubleSampleStats(allGflopsSamples);
        auto aggregateGpuMiBPerSecondStats = ComputeDoubleSampleStats(allGpuMiBPerSecondSamples);

        std::wostringstream statsJson;
        statsJson << L"[";
        for (size_t i = 0; i < statsMeasurements.size(); ++i)
        {
            if (i != 0)
            {
                statsJson << L",";
            }
            statsJson << D3D11ShaderMatrixStatsMeasurementJson(statsMeasurements[i]);
        }
        statsJson << L"]";

        return OkBase(L"run_d3d11_shader_matrix_stats_job") +
            L",\"job\":\"d3d11_shader_matrix_stats\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_shader_matrix_timestamp_query_stats\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only_samples\"" +
            L",\"dynamic_code\":false" +
            L",\"feature_level\":" + JsonString(featureLevel) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"batch_count\":" + std::to_wstring(batchCount) +
            L",\"batch_pause_ms\":" + std::to_wstring(batchPauseMs) +
            L",\"variant_count\":" + std::to_wstring(variants.size()) +
            L",\"element_count\":" + std::to_wstring(elementsList.size()) +
            L",\"stats_result_count\":" + std::to_wstring(totalStatsResultCount) +
            L",\"sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" + std::to_wstring(totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(aggregateGpuDispatchStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(aggregateGflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(aggregateGpuMiBPerSecondStats, 6) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs) +
            L",\"total_verification_readback_ms\":" + DoubleJson(totalVerificationReadbackMs) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"stats\":" + statsJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11SustainedSoakJob(JsonObject const& request, fs::path const& root)
    {
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variant", L"fp32_alu_128"));
        if (variants.size() != 1)
        {
            throw WorkerProtocolError("d3d11_sustained_soak.variant_invalid", "sustained soak requires exactly one shader matrix variant");
        }
        auto variant = variants.front();
        auto requestedElements = GetBoundedOptionalUInt64(request, L"elements", 1048576, 1, MaxD3D11ComputeElements);
        auto durationSeconds = GetBoundedOptionalUInt64(request, L"duration_seconds", 60, 1, MaxD3D11SustainedSoakDurationSeconds);
        auto windowSeconds = GetBoundedOptionalUInt64(request, L"window_seconds", 10, 1, MaxD3D11SustainedSoakWindowSeconds);
        auto repeatsPerBatch = GetBoundedOptionalUInt64(request, L"repeats_per_batch", 64, 1, MaxD3D11SustainedSoakRepeatsPerBatch);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 2, 0, MaxD3D11SustainedSoakRepeatsPerBatch);
        auto maxBatches = GetBoundedOptionalUInt64(request, L"max_batches", 50000, 1, MaxD3D11SustainedSoakBatches);

        D3D11SustainedSoakOptions options;
        options.variant = variant;
        options.requestedElements = requestedElements;
        options.durationSeconds = durationSeconds;
        options.windowSeconds = windowSeconds;
        options.repeatsPerBatch = repeatsPerBatch;
        options.warmupRepeats = warmupRepeats;
        options.maxBatches = maxBatches;
        auto runtimeResult = WorkerRunD3D11SustainedSoak(
            options,
            []()
            {
                WorkerD3DEnvironmentObservation observation;
                observation.appMemoryUsageBytes = AppMemoryUsageBytes();
                observation.appMemoryUsageLimitBytes = AppMemoryUsageLimitBytes();
                observation.appMemoryUsageLevel = AppMemoryUsageLevelValue();
                return observation;
            });
        auto const& windows = runtimeResult.windows;
        auto elapsedMs = runtimeResult.elapsedMs;
        auto totalBatchCount = runtimeResult.totalBatchCount;
        auto totalGpuTimingSampleCount = runtimeResult.totalGpuTimingSampleCount;
        auto totalGpuDisjointCount = runtimeResult.totalGpuDisjointCount;
        auto totalCpuSubmitSampleCount = runtimeResult.totalCpuSubmitSampleCount;
        auto totalElementsTimed = runtimeResult.totalElementsTimed;
        auto totalGpuBytesTimed = runtimeResult.totalGpuBytesTimed;
        auto totalFp32OpsTimed = runtimeResult.totalFp32OpsTimed;
        auto totalGpuDispatchMs = runtimeResult.totalGpuDispatchMs;
        auto aggregateFp32OpsPerSecond = runtimeResult.aggregateFp32OpsPerSecond;
        auto aggregateGflops = runtimeResult.aggregateGflops;
        auto aggregateGpuMiBPerSecond = runtimeResult.aggregateGpuMiBPerSecond;
        auto const& aggregateGpuDispatchStats = runtimeResult.aggregateGpuDispatchStats;
        auto const& aggregateCpuSubmitStats = runtimeResult.aggregateCpuSubmitStats;
        auto const& aggregateGflopsStats = runtimeResult.aggregateGflopsStats;
        auto const& aggregateGpuMiBPerSecondStats = runtimeResult.aggregateGpuMiBPerSecondStats;
        auto initialWindowMedianGflops = runtimeResult.initialWindowMedianGflops;
        auto finalWindowMedianGflops = runtimeResult.finalWindowMedianGflops;
        auto medianGflopsDriftPercent = runtimeResult.medianGflopsDriftPercent;
        auto minWindowMedianGflops = runtimeResult.minWindowMedianGflops;
        auto maxWindowMedianGflops = runtimeResult.maxWindowMedianGflops;
        auto totalCpuSubmitMs = runtimeResult.totalCpuSubmitMs;
        auto averageCpuSubmitMs = runtimeResult.averageCpuSubmitMs;
        auto totalVerificationReadbackMs = runtimeResult.totalVerificationReadbackMs;
        auto memoryAtStart = runtimeResult.memoryAtStart;
        auto peakAppMemoryUsageBytes = runtimeResult.peakAppMemoryUsageBytes;
        auto memoryLimit = runtimeResult.memoryLimit;
        auto deviceRemoved = runtimeResult.deviceRemoved;
        auto const& deviceRemovedReason = runtimeResult.deviceRemovedReason;
        auto stoppedEarly = runtimeResult.stoppedEarly;
        auto const& stopReason = runtimeResult.stopReason;
        auto aggregateHash32 = runtimeResult.aggregateHash32;
        auto maxMismatchCount = runtimeResult.maxMismatchCount;
        auto maxAbsError = runtimeResult.maxAbsError;
        auto maxRelativeError = runtimeResult.maxRelativeError;
        auto const& firstTimingError = runtimeResult.firstTimingError;
        auto verified = runtimeResult.verified;
        auto timestampQuerySupported = runtimeResult.timestampQuerySupported;
        auto gpuTimingAvailable = runtimeResult.gpuTimingAvailable;

        std::wostringstream windowsJson;
        windowsJson << L"[";
        for (size_t i = 0; i < windows.size(); ++i)
        {
            if (i != 0)
            {
                windowsJson << L",";
            }
            windowsJson << D3D11SustainedSoakWindowJson(windows[i]);
        }
        windowsJson << L"]";

        return OkBase(L"run_d3d11_sustained_soak_job") +
            L",\"job\":\"d3d11_sustained_soak\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_shader_matrix_sustained_soak\"" +
            L",\"timing_mode\":\"gpu_timestamp_dispatch_only_windowed_soak\"" +
            L",\"dynamic_code\":false" +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(runtimeResult.featureLevel)) +
            L",\"variant_id\":" + JsonString(variant.id) +
            L",\"shader\":" + JsonString(variant.shaderFileName) +
            L",\"category\":" + JsonString(variant.category) +
            L",\"loop_count\":" + std::to_wstring(variant.loopCount) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(variant.fp32OpsPerElement) +
            L",\"gpu_bytes_per_element\":" + std::to_wstring(variant.gpuBytesPerElement) +
            L",\"requested_elements\":" + std::to_wstring(requestedElements) +
            L",\"duration_seconds_requested\":" + std::to_wstring(durationSeconds) +
            L",\"duration_ms_actual\":" + DoubleJson(elapsedMs, 3) +
            L",\"window_seconds\":" + std::to_wstring(windowSeconds) +
            L",\"repeats_per_batch\":" + std::to_wstring(repeatsPerBatch) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"max_batches\":" + std::to_wstring(maxBatches) +
            L",\"batch_count\":" + std::to_wstring(totalBatchCount) +
            L",\"window_count\":" + std::to_wstring(windows.size()) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" + std::to_wstring(totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs, 6) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(aggregateGpuDispatchStats, 6) +
            L",\"cpu_submit_ms_stats\":" + DoubleSampleStatsJson(aggregateCpuSubmitStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(aggregateGflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(aggregateGpuMiBPerSecondStats, 6) +
            L",\"initial_window_median_gflops\":" + DoubleJson(initialWindowMedianGflops, 6) +
            L",\"final_window_median_gflops\":" + DoubleJson(finalWindowMedianGflops, 6) +
            L",\"median_gflops_drift_percent\":" + DoubleJson(medianGflopsDriftPercent, 6) +
            L",\"min_window_median_gflops\":" + DoubleJson(minWindowMedianGflops, 6) +
            L",\"max_window_median_gflops\":" + DoubleJson(maxWindowMedianGflops, 6) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs, 6) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs, 6) +
            L",\"total_verification_readback_ms\":" + DoubleJson(totalVerificationReadbackMs, 6) +
            L",\"app_memory_usage_start_bytes\":" + std::to_wstring(memoryAtStart) +
            L",\"app_memory_usage_end_bytes\":" + std::to_wstring(runtimeResult.environmentEnd.appMemoryUsageBytes) +
            L",\"app_memory_usage_peak_bytes\":" + std::to_wstring(peakAppMemoryUsageBytes) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(memoryLimit) +
            L",\"app_memory_usage_level\":" + std::to_wstring(runtimeResult.environmentEnd.appMemoryUsageLevel) +
            L",\"device_removed\":" + BoolJson(deviceRemoved) +
            L",\"device_removed_reason\":" + JsonString(deviceRemovedReason) +
            L",\"stopped_early\":" + BoolJson(stoppedEarly) +
            L",\"stop_reason\":" + JsonString(stopReason) +
            L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified && maxMismatchCount == 0 && !deviceRemoved) +
            L",\"windows\":" + windowsJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    std::wstring WorkerCommandServer::RunD3D11ResidentHotLoopJob(JsonObject const& request, fs::path const& root)
    {
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variant", L"fp32_alu_128"));
        if (variants.size() != 1)
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.variant_invalid", "resident hot-loop requires exactly one shader matrix variant");
        }
        auto variant = variants.front();
        auto requestedElements = GetBoundedOptionalUInt64(request, L"elements", 1048576, 1, MaxD3D11ComputeElements);
        auto durationSeconds = GetBoundedOptionalUInt64(request, L"duration_seconds", 60, 1, MaxD3D11ResidentHotLoopDurationSeconds);
        auto windowSeconds = GetBoundedOptionalUInt64(request, L"window_seconds", 10, 1, MaxD3D11ResidentHotLoopWindowSeconds);
        auto dispatchesPerSample = GetBoundedOptionalUInt64(request, L"dispatches_per_sample", 16, 1, MaxD3D11ResidentHotLoopDispatchesPerSample);
        auto warmupDispatches = GetBoundedOptionalUInt64(request, L"warmup_dispatches", 16, 0, MaxD3D11ResidentHotLoopWarmupDispatches);
        auto maxDispatches = GetBoundedOptionalUInt64(request, L"max_dispatches", MaxD3D11ResidentHotLoopDispatches, 1, MaxD3D11ResidentHotLoopDispatches);

        D3D11ResidentHotLoopOptions options;
        options.variant = variant;
        options.requestedElements = requestedElements;
        options.durationSeconds = durationSeconds;
        options.windowSeconds = windowSeconds;
        options.dispatchesPerSample = dispatchesPerSample;
        options.warmupDispatches = warmupDispatches;
        options.maxDispatches = maxDispatches;
        auto runtimeResult = WorkerRunD3D11ResidentHotLoop(
            options,
            []()
            {
                WorkerD3DEnvironmentObservation observation;
                observation.appMemoryUsageBytes = AppMemoryUsageBytes();
                observation.appMemoryUsageLimitBytes = AppMemoryUsageLimitBytes();
                observation.appMemoryUsageLevel = AppMemoryUsageLevelValue();
                return observation;
            });
        auto const& windows = runtimeResult.windows;
        auto elapsedMs = runtimeResult.elapsedMs;
        auto totalDispatchCount = runtimeResult.totalDispatchCount;
        auto totalTimedDispatchCount = runtimeResult.totalTimedDispatchCount;
        auto totalGpuTimingSampleCount = runtimeResult.totalGpuTimingSampleCount;
        auto totalGpuDisjointCount = runtimeResult.totalGpuDisjointCount;
        auto totalCpuSubmitSampleCount = runtimeResult.totalCpuSubmitSampleCount;
        auto totalElementsTimed = runtimeResult.totalElementsTimed;
        auto totalGpuBytesTimed = runtimeResult.totalGpuBytesTimed;
        auto totalFp32OpsTimed = runtimeResult.totalFp32OpsTimed;
        auto totalGpuDispatchMs = runtimeResult.totalGpuDispatchMs;
        auto averageGpuDispatchMs = runtimeResult.averageGpuDispatchMs;
        auto aggregateFp32OpsPerSecond = runtimeResult.aggregateFp32OpsPerSecond;
        auto aggregateGflops = runtimeResult.aggregateGflops;
        auto aggregateGpuMiBPerSecond = runtimeResult.aggregateGpuMiBPerSecond;
        auto const& aggregateGpuDispatchStats = runtimeResult.aggregateGpuDispatchStats;
        auto const& aggregateCpuSubmitStats = runtimeResult.aggregateCpuSubmitStats;
        auto const& aggregateGflopsStats = runtimeResult.aggregateGflopsStats;
        auto const& aggregateGpuMiBPerSecondStats = runtimeResult.aggregateGpuMiBPerSecondStats;
        auto initialWindowMedianGflops = runtimeResult.initialWindowMedianGflops;
        auto finalWindowMedianGflops = runtimeResult.finalWindowMedianGflops;
        auto medianGflopsDriftPercent = runtimeResult.medianGflopsDriftPercent;
        auto minWindowMedianGflops = runtimeResult.minWindowMedianGflops;
        auto maxWindowMedianGflops = runtimeResult.maxWindowMedianGflops;
        auto totalCpuSubmitMs = runtimeResult.totalCpuSubmitMs;
        auto averageCpuSubmitMs = runtimeResult.averageCpuSubmitMs;
        auto finalVerificationReadbackMs = runtimeResult.finalVerificationReadbackMs;
        auto memoryAtStart = runtimeResult.memoryAtStart;
        auto memoryAtEnd = runtimeResult.environmentEnd.appMemoryUsageBytes;
        auto peakAppMemoryUsageBytes = runtimeResult.peakAppMemoryUsageBytes;
        auto memoryLimit = runtimeResult.memoryLimit;
        auto deviceRemoved = runtimeResult.deviceRemoved;
        auto const& deviceRemovedReason = runtimeResult.deviceRemovedReason;
        auto stoppedEarly = runtimeResult.stoppedEarly;
        auto const& stopReason = runtimeResult.stopReason;
        auto const& firstValue = runtimeResult.firstValue;
        auto const& lastValue = runtimeResult.lastValue;
        auto checksum = runtimeResult.checksum;
        auto hash32 = runtimeResult.hash32;
        auto maxMismatchCount = runtimeResult.maxMismatchCount;
        auto maxAbsError = runtimeResult.maxAbsError;
        auto maxRelativeError = runtimeResult.maxRelativeError;
        auto const& firstTimingError = runtimeResult.firstTimingError;
        auto verified = runtimeResult.verified;
        auto timestampQuerySupported = runtimeResult.timestampQuerySupported;
        auto gpuTimingAvailable = runtimeResult.gpuTimingAvailable;

        std::wostringstream windowsJson;
        windowsJson << L"[";
        for (size_t i = 0; i < windows.size(); ++i)
        {
            if (i != 0)
            {
                windowsJson << L",";
            }
            windowsJson << D3D11ResidentHotLoopWindowJson(windows[i]);
        }
        windowsJson << L"]";

        return OkBase(L"run_d3d11_resident_hotloop_job") +
            L",\"job\":\"d3d11_resident_hotloop\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_d3d11_shader_matrix_resident_hotloop\"" +
            L",\"timing_mode\":\"gpu_timestamp_resident_dispatch_blocks_final_readback\"" +
            L",\"resource_residency\":\"shader_srv_uav_staging_queries_created_once\"" +
            L",\"final_verification_mode\":\"single_readback_after_loop\"" +
            L",\"dynamic_code\":false" +
            L",\"feature_level\":" + JsonString(D3DFeatureLevelToString(runtimeResult.featureLevel)) +
            L",\"variant_id\":" + JsonString(variant.id) +
            L",\"shader\":" + JsonString(variant.shaderFileName) +
            L",\"category\":" + JsonString(variant.category) +
            L",\"loop_count\":" + std::to_wstring(variant.loopCount) +
            L",\"fp32_ops_per_element\":" + std::to_wstring(variant.fp32OpsPerElement) +
            L",\"gpu_bytes_per_element\":" + std::to_wstring(variant.gpuBytesPerElement) +
            L",\"requested_elements\":" + std::to_wstring(requestedElements) +
            L",\"elements\":" + std::to_wstring(runtimeResult.elements) +
            L",\"threads_per_group\":" + std::to_wstring(D3D11ComputeThreadsPerGroup) +
            L",\"dispatch_groups\":" + std::to_wstring(runtimeResult.dispatchGroups) +
            L",\"duration_seconds_requested\":" + std::to_wstring(durationSeconds) +
            L",\"duration_ms_actual\":" + DoubleJson(elapsedMs, 3) +
            L",\"window_seconds\":" + std::to_wstring(windowSeconds) +
            L",\"dispatches_per_sample\":" + std::to_wstring(dispatchesPerSample) +
            L",\"warmup_dispatches\":" + std::to_wstring(warmupDispatches) +
            L",\"max_dispatches\":" + std::to_wstring(maxDispatches) +
            L",\"dispatch_count\":" + std::to_wstring(totalDispatchCount) +
            L",\"timed_dispatch_count\":" + std::to_wstring(totalTimedDispatchCount) +
            L",\"window_count\":" + std::to_wstring(windows.size()) +
            L",\"timestamp_query_supported\":" + BoolJson(timestampQuerySupported) +
            L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
            L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSampleCount) +
            L",\"gpu_disjoint_count\":" + std::to_wstring(totalGpuDisjointCount) +
            L",\"cpu_submit_sample_count\":" + std::to_wstring(totalCpuSubmitSampleCount) +
            L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
            L",\"total_gpu_bytes_timed\":" + std::to_wstring(totalGpuBytesTimed) +
            L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
            L",\"total_gpu_dispatch_ms\":" + DoubleJson(totalGpuDispatchMs, 6) +
            L",\"avg_gpu_dispatch_ms\":" + DoubleJson(averageGpuDispatchMs, 6) +
            L",\"aggregate_fp32_ops_per_second\":" + DoubleJson(aggregateFp32OpsPerSecond, 1) +
            L",\"aggregate_gflops\":" + DoubleJson(aggregateGflops, 3) +
            L",\"aggregate_gpu_mib_per_second\":" + DoubleJson(aggregateGpuMiBPerSecond, 3) +
            L",\"gpu_dispatch_ms_stats\":" + DoubleSampleStatsJson(aggregateGpuDispatchStats, 6) +
            L",\"cpu_submit_ms_stats\":" + DoubleSampleStatsJson(aggregateCpuSubmitStats, 6) +
            L",\"gflops_stats\":" + DoubleSampleStatsJson(aggregateGflopsStats, 6) +
            L",\"gpu_mib_per_second_stats\":" + DoubleSampleStatsJson(aggregateGpuMiBPerSecondStats, 6) +
            L",\"initial_window_median_gflops\":" + DoubleJson(initialWindowMedianGflops, 6) +
            L",\"final_window_median_gflops\":" + DoubleJson(finalWindowMedianGflops, 6) +
            L",\"median_gflops_drift_percent\":" + DoubleJson(medianGflopsDriftPercent, 6) +
            L",\"min_window_median_gflops\":" + DoubleJson(minWindowMedianGflops, 6) +
            L",\"max_window_median_gflops\":" + DoubleJson(maxWindowMedianGflops, 6) +
            L",\"total_cpu_submit_ms\":" + DoubleJson(totalCpuSubmitMs, 6) +
            L",\"avg_cpu_submit_ms\":" + DoubleJson(averageCpuSubmitMs, 6) +
            L",\"final_verification_readback_ms\":" + DoubleJson(finalVerificationReadbackMs, 6) +
            L",\"app_memory_usage_start_bytes\":" + std::to_wstring(memoryAtStart) +
            L",\"app_memory_usage_end_bytes\":" + std::to_wstring(memoryAtEnd) +
            L",\"app_memory_usage_peak_bytes\":" + std::to_wstring(peakAppMemoryUsageBytes) +
            L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(memoryLimit) +
            L",\"app_memory_usage_level\":" + std::to_wstring(runtimeResult.environmentEnd.appMemoryUsageLevel) +
            L",\"device_removed\":" + BoolJson(deviceRemoved) +
            L",\"device_removed_reason\":" + JsonString(deviceRemovedReason) +
            L",\"stopped_early\":" + BoolJson(stoppedEarly) +
            L",\"stop_reason\":" + JsonString(stopReason) +
            L",\"first_value\":" + D3D11Float4Json(firstValue) +
            L",\"last_value\":" + D3D11Float4Json(lastValue) +
            L",\"checksum\":" + DoubleJson(checksum, 6) +
            L",\"hash32\":" + std::to_wstring(hash32) +
            L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
            L",\"max_abs_error\":" + DoubleJson(maxAbsError, 6) +
            L",\"max_relative_error\":" + DoubleJson(maxRelativeError, 9) +
            L",\"absolute_tolerance\":" + DoubleJson(D3D11FloatAbsoluteTolerance, 6) +
            L",\"relative_tolerance\":" + DoubleJson(D3D11FloatRelativeTolerance, 6) +
            L",\"timing_error\":" + JsonString(firstTimingError) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"windows\":" + windowsJson.str() +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunNativeBench(JsonObject const& request, fs::path const& root)
    {
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 100000, 1, 5000000);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto bytes = MakeJobBytes(4096, seed);
        auto vectorInput = MakeVectorInput(4096, seed ^ 0x9e3779b9u);

        auto started = std::chrono::steady_clock::now();
        uint64_t accumulator = 0;
        for (uint64_t i = 0; i < iterations; ++i)
        {
            auto left = static_cast<uint32_t>((i + seed) & 0xffffu);
            auto right = static_cast<uint32_t>(((i >> 1) + 3u) & 0xffffu);
            accumulator += NativeStatic::Add(left, right);
            accumulator ^= NativeStatic::Mul((left & 0xffu) + 1u, (right & 0xffu) + 1u);
        }
        auto hash = NativeStatic::Hash32(bytes.data(), bytes.size());
        auto vectorResult = NativeStatic::VectorLoop(vectorInput.data(), vectorInput.size(), 32);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto operations = iterations * 2 + bytes.size() + (static_cast<uint64_t>(vectorInput.size()) * 32ull);
        auto operationsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(operations) / (elapsedMs / 1000.0)) : 0.0;
        auto verified = NativeStatic::Add(17, 25) == 42 && NativeStatic::Mul(13, 11) == 143 && hash != 0 && vectorResult != 0;

        return OkBase(L"run_native_bench") +
            L",\"job\":\"native_bench\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_static_native_module\"" +
            L",\"dynamic_code\":false" +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"operations\":" + std::to_wstring(operations) +
            L",\"operations_per_second\":" + DoubleJson(operationsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(accumulator) +
            L",\"hash32\":" + std::to_wstring(hash) +
            L",\"vector_result\":" + std::to_wstring(vectorResult) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunHashJob(JsonObject const& request, fs::path const& root)
    {
        auto byteCount = GetBoundedOptionalUInt64(request, L"bytes", 1024 * 1024, 1, MaxJobDataBytes);
        auto rounds = GetBoundedOptionalUInt64(request, L"rounds", 8, 1, MaxJobRounds);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto data = MakeJobBytes(static_cast<size_t>(byteCount), seed);

        auto started = std::chrono::steady_clock::now();
        uint32_t hash = 0;
        for (uint64_t round = 0; round < rounds; ++round)
        {
            hash ^= NativeStatic::Hash32(data.data(), data.size()) + static_cast<uint32_t>(round * 2654435761u);
            if (!data.empty())
            {
                data[round % data.size()] ^= static_cast<uint8_t>(round + seed);
            }
        }
        auto elapsedMs = ElapsedMilliseconds(started);
        auto processedBytes = byteCount * rounds;
        auto mibPerSecond = elapsedMs > 0.0 ? ((static_cast<double>(processedBytes) / 1048576.0) / (elapsedMs / 1000.0)) : 0.0;

        return OkBase(L"run_hash_job") +
            L",\"job\":\"hash\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_static_native_module\"" +
            L",\"dynamic_code\":false" +
            L",\"bytes\":" + std::to_wstring(byteCount) +
            L",\"rounds\":" + std::to_wstring(rounds) +
            L",\"processed_bytes\":" + std::to_wstring(processedBytes) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"mib_per_second\":" + DoubleJson(mibPerSecond) +
            L",\"hash32\":" + std::to_wstring(hash) +
            L",\"verified\":" + BoolJson(hash != 0) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunVectorJob(JsonObject const& request, fs::path const& root)
    {
        auto length = GetBoundedOptionalUInt64(request, L"vector_length", 262144, 1, MaxVectorLength);
        auto rounds = GetBoundedOptionalUInt64(request, L"rounds", 256, 1, MaxJobRounds);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto data = MakeVectorInput(static_cast<size_t>(length), seed);

        auto started = std::chrono::steady_clock::now();
        auto result = NativeStatic::VectorLoop(data.data(), data.size(), static_cast<uint32_t>(rounds));
        auto elapsedMs = ElapsedMilliseconds(started);
        auto operations = length * rounds;
        auto operationsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(operations) / (elapsedMs / 1000.0)) : 0.0;

        return OkBase(L"run_vector_job") +
            L",\"job\":\"vector\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"precompiled_static_native_module\"" +
            L",\"dynamic_code\":false" +
            L",\"vector_length\":" + std::to_wstring(length) +
            L",\"rounds\":" + std::to_wstring(rounds) +
            L",\"operations\":" + std::to_wstring(operations) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"operations_per_second\":" + DoubleJson(operationsPerSecond, 1) +
            L",\"vector_result\":" + std::to_wstring(result) +
            L",\"verified\":" + BoolJson(result != 0) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunInterpreterJob(JsonObject const& request, fs::path const& root)
    {
        auto programOps = GetBoundedOptionalUInt64(request, L"program_ops", 256, 1, MaxInterpreterProgramOps);
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 10000, 1, MaxInterpreterIterations);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto program = MakeInterpreterProgram(static_cast<size_t>(programOps), seed);
        auto bytecodeBytes = static_cast<uint64_t>(program.size() * sizeof(uint32_t));
        auto bytecodeHash = NativeStatic::Hash32(
            reinterpret_cast<uint8_t const*>(program.data()),
            program.size() * sizeof(uint32_t));

        auto started = std::chrono::steady_clock::now();
        auto accumulator = RunInterpreterProgram(program, iterations, seed);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto instructionCount = programOps * iterations;
        auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
        auto verified = accumulator != 0 && bytecodeHash != 0 && instructionCount > 0;

        return OkBase(L"run_interpreter_job") +
            L",\"job\":\"interpreter\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"bounded_register_bytecode_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"bytecode_format\":\"xcompute-register-v0\"" +
            L",\"program_ops\":" + std::to_wstring(programOps) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"bytecode_bytes\":" + std::to_wstring(bytecodeBytes) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(accumulator) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteStoreInterpreterProgram(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto path = ResolveInterpreterProgramPath(root, programId);
        auto bytecodeBase64 = GetOptionalString(request, L"bytecode_base64");
        auto programSource = std::wstring(L"generated_on_worker");
        std::vector<uint32_t> program;
        if (bytecodeBase64.empty())
        {
            auto programOps = GetBoundedOptionalUInt64(request, L"program_ops", 256, 1, MaxInterpreterProgramOps);
            program = MakeInterpreterProgram(static_cast<size_t>(programOps), seed);
        }
        else
        {
            program = DecodeInterpreterProgramBase64(bytecodeBase64);
            programSource = L"uploaded_base64";
        }

        auto bytecodeHash = InterpreterProgramHash(program);
        if (request.HasKey(L"expected_bytecode_hash32"))
        {
            auto expectedHash = GetBoundedOptionalUInt64(request, L"expected_bytecode_hash32", bytecodeHash, 0, UINT32_MAX);
            if (expectedHash != bytecodeHash)
            {
                throw WorkerProtocolError("interpreter.hash_mismatch", "uploaded bytecode hash did not match expected_bytecode_hash32");
            }
        }

        WriteInterpreterArtifact(path, seed, program);

        return OkBase(L"store_interpreter_program") +
            L",\"job\":\"interpreter_store\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"persistent_bounded_register_bytecode_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"artifact_persistent\":true" +
            L",\"bytecode_format\":\"xcompute-register-v0\"" +
            L",\"program_source\":" + JsonString(programSource) +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"program_ops\":" + std::to_wstring(program.size()) +
            L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(program)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"stored\":true" +
            L",\"verified\":true" +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunStoredInterpreterJob(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 10000, 1, MaxInterpreterIterations);
        auto path = ResolveInterpreterProgramPath(root, programId);
        auto artifact = ReadInterpreterArtifact(path, programId);
        auto bytecodeHash = InterpreterProgramHash(artifact.program);

        auto started = std::chrono::steady_clock::now();
        auto accumulator = RunInterpreterProgram(artifact.program, iterations, artifact.seed);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto instructionCount = static_cast<uint64_t>(artifact.program.size()) * iterations;
        auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
        auto verified = accumulator != 0 && bytecodeHash != 0 && instructionCount > 0;

        return OkBase(L"run_stored_interpreter_job") +
            L",\"job\":\"interpreter_stored\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"persistent_bounded_register_bytecode_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"artifact_persistent\":true" +
            L",\"bytecode_format\":\"xcompute-register-v0\"" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"program_ops\":" + std::to_wstring(artifact.program.size()) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(artifact.program)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"seed\":" + std::to_wstring(artifact.seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(accumulator) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteListInterpreterPrograms(fs::path const& root)
    {
        auto artifactsJson = JsonInterpreterArtifacts(root);
        auto artifactCount = static_cast<uint64_t>(std::count(artifactsJson.begin(), artifactsJson.end(), L'{'));
        return OkBase(L"list_interpreter_programs") +
            L",\"job\":\"interpreter_list\"" +
            L",\"artifact_persistent\":true" +
            L",\"artifact_count\":" + std::to_wstring(artifactCount) +
            L",\"artifacts\":" + artifactsJson +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteDeleteInterpreterProgram(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto path = ResolveInterpreterProgramPath(root, programId);
        auto existed = fs::exists(path);
        auto removed = existed && fs::remove(path);
        return OkBase(L"delete_interpreter_program") +
            L",\"job\":\"interpreter_delete\"" +
            L",\"artifact_persistent\":true" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"existed\":" + BoolJson(existed) +
            L",\"removed\":" + BoolJson(removed) +
            L",\"verified\":true" +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteStoreMemoryProgram(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));
        auto path = ResolveMemoryProgramPath(root, programId);
        auto bytecodeBase64 = GetOptionalString(request, L"bytecode_base64");
        if (bytecodeBase64.empty())
        {
            throw WorkerProtocolError("memory_program.bytecode_required", "bytecode_base64 is required for store_memory_program");
        }

        auto program = DecodeInterpreterProgramBase64(bytecodeBase64);
        auto bytecodeHash = InterpreterProgramHash(program);
        if (request.HasKey(L"expected_bytecode_hash32"))
        {
            auto expectedHash = GetBoundedOptionalUInt64(request, L"expected_bytecode_hash32", bytecodeHash, 0, UINT32_MAX);
            if (expectedHash != bytecodeHash)
            {
                throw WorkerProtocolError("memory_program.hash_mismatch", "uploaded memory bytecode hash did not match expected_bytecode_hash32");
            }
        }

        WriteMemoryProgramArtifact(path, seed, program);

        return OkBase(L"store_memory_program") +
            L",\"job\":\"memory_program_store\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"persistent_bounded_linear_memory_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"artifact_persistent\":true" +
            L",\"bytecode_format\":\"xcompute-memory-v0\"" +
            L",\"program_source\":\"uploaded_base64\"" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"program_ops\":" + std::to_wstring(program.size()) +
            L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(program)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"stored\":true" +
            L",\"verified\":true" +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteListMemoryPrograms(fs::path const& root)
    {
        auto artifactsJson = JsonMemoryProgramArtifacts(root);
        auto artifactCount = static_cast<uint64_t>(std::count(artifactsJson.begin(), artifactsJson.end(), L'{'));
        return OkBase(L"list_memory_programs") +
            L",\"job\":\"memory_program_list\"" +
            L",\"artifact_persistent\":true" +
            L",\"artifact_count\":" + std::to_wstring(artifactCount) +
            L",\"artifacts\":" + artifactsJson +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunStoredMemoryFileJob(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto path = ResolveMemoryProgramPath(root, programId);
        auto artifact = ReadMemoryProgramArtifact(path, programId);

        auto inputPathText = GetOptionalString(request, L"input_path");
        if (inputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.input_path_required", "input_path is required for run_stored_memory_file_job");
        }
        auto outputPathText = GetOptionalString(request, L"output_path");
        if (outputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.output_path_required", "output_path is required for run_stored_memory_file_job");
        }

        auto inputPath = ResolveWorkspacePath(root, inputPathText);
        auto outputPath = ResolveWorkspacePath(root, outputPathText);
        auto bytecodeHash = InterpreterProgramHash(artifact.program);
        auto input = ReadBinaryFileBounded(inputPath, MaxMemoryInterpreterInputBytes);
        auto defaultMemoryBytes = std::max<uint64_t>(256, static_cast<uint64_t>(input.size()));
        auto memoryBytes = GetBoundedOptionalUInt64(request, L"memory_bytes", defaultMemoryBytes, 1, MaxMemoryInterpreterBytes);
        auto outputBytes = GetBoundedOptionalUInt64(request, L"output_bytes", 64, 1, MaxMemoryInterpreterOutputBytes);
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 1000, 1, MaxInterpreterIterations);

        auto started = std::chrono::steady_clock::now();
        auto result = RunMemoryInterpreterProgram(artifact.program, input, iterations, artifact.seed, memoryBytes, outputBytes);
        WriteBinaryFile(outputPath, result.output);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto instructionCount = static_cast<uint64_t>(artifact.program.size()) * iterations;
        auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
        auto inputHash = HashBytes(input);
        auto outputSha256 = Sha256File(outputPath);
        auto verified = result.outputHash != 0 && bytecodeHash != 0 && instructionCount > 0 && fs::is_regular_file(outputPath);

        return OkBase(L"run_stored_memory_file_job") +
            L",\"job\":\"memory_file_stored\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"persistent_bounded_linear_memory_file_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"artifact_persistent\":true" +
            L",\"bytecode_format\":\"xcompute-memory-v0\"" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"input_path\":" + JsonString(inputPath.lexically_relative(root).wstring()) +
            L",\"output_path\":" + JsonString(outputPath.lexically_relative(root).wstring()) +
            L",\"program_ops\":" + std::to_wstring(artifact.program.size()) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(artifact.program)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"input_bytes\":" + std::to_wstring(input.size()) +
            L",\"input_hash32\":" + std::to_wstring(inputHash) +
            L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
            L",\"memory_hash32\":" + std::to_wstring(result.memoryHash) +
            L",\"output_bytes\":" + std::to_wstring(result.output.size()) +
            L",\"output_hash32\":" + std::to_wstring(result.outputHash) +
            L",\"output_sha256\":" + JsonString(outputSha256) +
            L",\"seed\":" + std::to_wstring(artifact.seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(result.accumulator) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteDeleteMemoryProgram(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto path = ResolveMemoryProgramPath(root, programId);
        auto existed = fs::exists(path);
        auto removed = existed && fs::remove(path);
        return OkBase(L"delete_memory_program") +
            L",\"job\":\"memory_program_delete\"" +
            L",\"artifact_persistent\":true" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
            L",\"existed\":" + BoolJson(existed) +
            L",\"removed\":" + BoolJson(removed) +
            L",\"verified\":true" +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunMemoryInterpreterJob(JsonObject const& request, fs::path const& root)
    {
        auto bytecodeBase64 = GetOptionalString(request, L"bytecode_base64");
        if (bytecodeBase64.empty())
        {
            throw WorkerProtocolError("memory_interpreter.bytecode_required", "bytecode_base64 is required for run_memory_interpreter_job");
        }

        auto program = DecodeInterpreterProgramBase64(bytecodeBase64);
        auto bytecodeHash = InterpreterProgramHash(program);
        if (request.HasKey(L"expected_bytecode_hash32"))
        {
            auto expectedHash = GetBoundedOptionalUInt64(request, L"expected_bytecode_hash32", bytecodeHash, 0, UINT32_MAX);
            if (expectedHash != bytecodeHash)
            {
                throw WorkerProtocolError("memory_interpreter.hash_mismatch", "uploaded memory bytecode hash did not match expected_bytecode_hash32");
            }
        }

        auto inputBase64 = GetOptionalString(request, L"input_base64");
        auto input = inputBase64.empty() ? std::vector<uint8_t>{} : Base64Decode(WideToUtf8(inputBase64));
        if (input.size() > MaxMemoryInterpreterInputBytes)
        {
            throw WorkerProtocolError("memory_interpreter.input_too_large", "input_base64 exceeds memory interpreter input limit");
        }

        auto defaultMemoryBytes = std::max<uint64_t>(256, static_cast<uint64_t>(input.size()));
        auto memoryBytes = GetBoundedOptionalUInt64(request, L"memory_bytes", defaultMemoryBytes, 1, MaxMemoryInterpreterBytes);
        auto outputBytes = GetBoundedOptionalUInt64(request, L"output_bytes", 64, 1, MaxMemoryInterpreterOutputBytes);
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 1000, 1, MaxInterpreterIterations);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));

        auto started = std::chrono::steady_clock::now();
        auto result = RunMemoryInterpreterProgram(program, input, iterations, seed, memoryBytes, outputBytes);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto instructionCount = static_cast<uint64_t>(program.size()) * iterations;
        auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
        auto inputHash = HashBytes(input);
        auto previewBytes = std::min<size_t>(result.output.size(), 64);
        auto outputPreview = Base64Encode(result.output.data(), previewBytes);
        auto verified = result.outputHash != 0 && bytecodeHash != 0 && instructionCount > 0;

        return OkBase(L"run_memory_interpreter_job") +
            L",\"job\":\"memory_interpreter\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"bounded_linear_memory_bytecode_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"bytecode_format\":\"xcompute-memory-v0\"" +
            L",\"program_ops\":" + std::to_wstring(program.size()) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"bytecode_bytes\":" + std::to_wstring(program.size() * sizeof(uint32_t)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"input_bytes\":" + std::to_wstring(input.size()) +
            L",\"input_hash32\":" + std::to_wstring(inputHash) +
            L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
            L",\"memory_hash32\":" + std::to_wstring(result.memoryHash) +
            L",\"output_bytes\":" + std::to_wstring(result.output.size()) +
            L",\"output_hash32\":" + std::to_wstring(result.outputHash) +
            L",\"output_preview_bytes\":" + std::to_wstring(previewBytes) +
            L",\"output_preview_base64\":" + JsonString(Utf8ToWide(outputPreview)) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(result.accumulator) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    static std::wstring ExecuteRunMemoryFileJob(JsonObject const& request, fs::path const& root)
    {
        auto bytecodeBase64 = GetOptionalString(request, L"bytecode_base64");
        if (bytecodeBase64.empty())
        {
            throw WorkerProtocolError("memory_file.bytecode_required", "bytecode_base64 is required for run_memory_file_job");
        }

        auto inputPathText = GetOptionalString(request, L"input_path");
        if (inputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.input_path_required", "input_path is required for run_memory_file_job");
        }
        auto outputPathText = GetOptionalString(request, L"output_path");
        if (outputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.output_path_required", "output_path is required for run_memory_file_job");
        }

        auto inputPath = ResolveWorkspacePath(root, inputPathText);
        auto outputPath = ResolveWorkspacePath(root, outputPathText);
        auto program = DecodeInterpreterProgramBase64(bytecodeBase64);
        auto bytecodeHash = InterpreterProgramHash(program);
        if (request.HasKey(L"expected_bytecode_hash32"))
        {
            auto expectedHash = GetBoundedOptionalUInt64(request, L"expected_bytecode_hash32", bytecodeHash, 0, UINT32_MAX);
            if (expectedHash != bytecodeHash)
            {
                throw WorkerProtocolError("memory_file.hash_mismatch", "uploaded memory bytecode hash did not match expected_bytecode_hash32");
            }
        }

        auto input = ReadBinaryFileBounded(inputPath, MaxMemoryInterpreterInputBytes);
        auto defaultMemoryBytes = std::max<uint64_t>(256, static_cast<uint64_t>(input.size()));
        auto memoryBytes = GetBoundedOptionalUInt64(request, L"memory_bytes", defaultMemoryBytes, 1, MaxMemoryInterpreterBytes);
        auto outputBytes = GetBoundedOptionalUInt64(request, L"output_bytes", 64, 1, MaxMemoryInterpreterOutputBytes);
        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 1000, 1, MaxInterpreterIterations);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64(request, L"seed", 17, 0, UINT32_MAX));

        auto started = std::chrono::steady_clock::now();
        auto result = RunMemoryInterpreterProgram(program, input, iterations, seed, memoryBytes, outputBytes);
        WriteBinaryFile(outputPath, result.output);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto instructionCount = static_cast<uint64_t>(program.size()) * iterations;
        auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
        auto inputHash = HashBytes(input);
        auto outputSha256 = Sha256File(outputPath);
        auto verified = result.outputHash != 0 && bytecodeHash != 0 && instructionCount > 0 && fs::is_regular_file(outputPath);

        return OkBase(L"run_memory_file_job") +
            L",\"job\":\"memory_file\"" +
            L",\"evidence_class\":\"MEASURED\"" +
            L",\"implementation\":\"bounded_linear_memory_file_interpreter\"" +
            L",\"dynamic_code\":false" +
            L",\"bytecode_format\":\"xcompute-memory-v0\"" +
            L",\"input_path\":" + JsonString(inputPath.lexically_relative(root).wstring()) +
            L",\"output_path\":" + JsonString(outputPath.lexically_relative(root).wstring()) +
            L",\"program_ops\":" + std::to_wstring(program.size()) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"instruction_count\":" + std::to_wstring(instructionCount) +
            L",\"bytecode_bytes\":" + std::to_wstring(program.size() * sizeof(uint32_t)) +
            L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
            L",\"input_bytes\":" + std::to_wstring(input.size()) +
            L",\"input_hash32\":" + std::to_wstring(inputHash) +
            L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
            L",\"memory_hash32\":" + std::to_wstring(result.memoryHash) +
            L",\"output_bytes\":" + std::to_wstring(result.output.size()) +
            L",\"output_hash32\":" + std::to_wstring(result.outputHash) +
            L",\"output_sha256\":" + JsonString(outputSha256) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
            L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
            L",\"accumulator\":" + std::to_wstring(result.accumulator) +
            L",\"verified\":" + BoolJson(verified) +
            L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
            L"}";
    }

    void WorkerCommandServer::TryStartNextJob(fs::path const& root)
    {
        m_asyncJobRuntime.TryStartNext(
            [this, root](std::shared_ptr<WorkerAsyncJobState> const& job)
            {
                DispatchAsyncJob(job, root);
            });
    }

    void WorkerCommandServer::DispatchAsyncJob(
        std::shared_ptr<WorkerAsyncJobState> const& job,
        fs::path root)
    {
        if (job->kind == L"async_graph")
        {
            RunAsyncGraphJob(job, root);
        }
        else if (job->kind == L"async_d3d11_resident_hotloop")
        {
            RunAsyncD3D11ResidentHotLoopJob(job, root);
        }
        else if (job->kind == L"async_d3d12_shader_shape")
        {
            RunAsyncD3D12ShaderShapeJob(job, root);
        }
        else if (job->kind == L"async_d3d12_shader_shape_soak")
        {
            RunAsyncD3D12ShaderShapeSoakJob(job, root);
        }
        else
        {
            RunAsyncStoredMemoryFileJob(job, root);
        }
    }

    void WorkerCommandServer::RunAsyncGraphJob(std::shared_ptr<WorkerAsyncJobState> job, fs::path root)
    {
        std::wstring status = L"succeeded";
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;

        try
        {
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            auto graphRequest = JsonObject::Parse(job->graphRequestJson);
            resultJson = ExecuteSubmitGraph(
                graphRequest,
                root,
                &job->cancelRequested,
                [this, job](uint64_t checkpointCount, std::wstring const& checkpointArtifactId)
                {
                    m_asyncJobRuntime.UpdateGraphCheckpoint(job, checkpointCount, checkpointArtifactId);
                });
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            auto result = JsonObject::Parse(resultJson);
            auto checkpointCount = static_cast<uint64_t>(
                result.GetNamedNumber(L"checkpoint_count", static_cast<double>(job->graphCheckpointCount)));
            m_asyncJobRuntime.UpdateGraphCheckpoint(
                job, checkpointCount, job->graphLastCheckpointArtifactId);
        }
        catch (WorkerProtocolError const& ex)
        {
            status = (ex.code == "job.canceled" || job->cancelRequested.load()) ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (WorkerGraphValidationError const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (WorkerGraphExecutionError const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (WorkerGraphResourceError const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (WorkerXvmError const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (std::exception const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.exception";
            errorMessage = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.unhandled";
            errorMessage = L"unhandled async graph job failure";
        }

        m_asyncJobRuntime.Complete(
            root, job, status, std::move(resultJson), std::move(errorCode), std::move(errorMessage));

        TryStartNextJob(root);
    }

    std::wstring WorkerCommandServer::SubmitGraphJob(JsonObject const& request, fs::path const& root)
    {
        auto graphRequest = JsonObject::Parse(request.Stringify());
        InsertJsonString(graphRequest, L"command", L"submit_graph");
        if (graphRequest.HasKey(L"pairing_code")) { graphRequest.Remove(L"pairing_code"); }
        if (graphRequest.HasKey(L"session_id")) { graphRequest.Remove(L"session_id"); }
        if (graphRequest.HasKey(L"_server_parse_json_ms")) { graphRequest.Remove(L"_server_parse_json_ms"); }
        if (graphRequest.HasKey(L"_server_request_bytes")) { graphRequest.Remove(L"_server_request_bytes"); }
        if (graphRequest.HasKey(L"_server_auth_ms")) { graphRequest.Remove(L"_server_auth_ms"); }

        auto generatedGraphId = GenerateSessionId();
        if (generatedGraphId.size() > 16)
        {
            generatedGraphId.resize(16);
        }
        generatedGraphId = L"graph-" + generatedGraphId;
        auto graphValidation = WorkerGraphValidateRequest(graphRequest, generatedGraphId, WorkerMaxOnDeviceGraphNodes());
        (void)WorkerGraphAdmitResourceLedger(graphValidation.graph, graphValidation.nodes);

        auto job = std::make_shared<WorkerAsyncJobState>();
        job->kind = L"async_graph";
        job->graphRequestJson = std::wstring(graphRequest.Stringify().c_str());
        job->graphId = graphValidation.graphId;
        job->graphNodeCount = graphValidation.nodes.Size();

        auto submission = m_asyncJobRuntime.Enqueue(root, job);
        if (!submission.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(submission.errorCode),
                WideToUtf8(submission.errorMessage));
        }

        TryStartNextJob(root);

        auto observation = m_asyncJobRuntime.Observe(job);
        auto status = observation.status;
        auto queueDepth = observation.queueDepth;

        return OkBase(L"submit_graph_job") +
            L",\"job\":\"async_graph_submit\"" +
            L",\"job_id\":" + JsonString(job->jobId) +
            L",\"status\":" + JsonString(status) +
            L",\"queued\":true" +
            L",\"graph_id\":" + JsonString(job->graphId) +
            L",\"graph_node_count\":" + std::to_wstring(job->graphNodeCount) +
            L",\"checkpoint_schema\":\"worker-graph-checkpoint-log-0.1\"" +
            L",\"checkpoint_artifacts_declared\":true" +
            L",\"cancelable\":true" +
            L",\"queue_depth\":" + std::to_wstring(queueDepth) +
            L",\"concurrency\":1" +
            L"}";
    }

    void WorkerCommandServer::RunAsyncStoredMemoryFileJob(std::shared_ptr<WorkerAsyncJobState> job, fs::path root)
    {
        std::wstring status = L"succeeded";
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;

        try
        {
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            auto programPath = ResolveMemoryProgramPath(root, job->programId);
            auto artifact = ReadMemoryProgramArtifact(programPath, job->programId);
            auto inputPath = ResolveWorkspacePath(root, job->inputPath);
            auto outputPath = ResolveWorkspacePath(root, job->outputPath);
            auto bytecodeHash = InterpreterProgramHash(artifact.program);
            auto input = ReadBinaryFileBounded(inputPath, MaxMemoryInterpreterInputBytes);
            auto memoryBytes = job->memoryBytes == 0
                ? std::max<uint64_t>(256, static_cast<uint64_t>(input.size()))
                : job->memoryBytes;

            auto started = std::chrono::steady_clock::now();
            auto result = RunMemoryInterpreterProgram(
                artifact.program,
                input,
                job->iterations,
                artifact.seed,
                memoryBytes,
                job->outputBytes,
                &job->cancelRequested);
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            WriteBinaryFile(outputPath, result.output);
            auto elapsedMs = ElapsedMilliseconds(started);
            auto instructionCount = static_cast<uint64_t>(artifact.program.size()) * job->iterations;
            auto instructionsPerSecond = elapsedMs > 0.0 ? (static_cast<double>(instructionCount) / (elapsedMs / 1000.0)) : 0.0;
            auto inputHash = HashBytes(input);
            auto outputSha256 = Sha256File(outputPath);
            auto verified = result.outputHash != 0 && bytecodeHash != 0 && instructionCount > 0 && fs::is_regular_file(outputPath);

            resultJson = OkBase(L"run_stored_memory_file_job") +
                L",\"async_job_id\":" + JsonString(job->jobId) +
                L",\"job\":\"async_memory_file_stored\"" +
                L",\"evidence_class\":\"MEASURED\"" +
                L",\"implementation\":\"async_persistent_bounded_linear_memory_file_interpreter\"" +
                L",\"dynamic_code\":false" +
                L",\"artifact_persistent\":true" +
                L",\"bytecode_format\":\"xcompute-memory-v0\"" +
                L",\"program_id\":" + JsonString(job->programId) +
                L",\"path\":" + JsonString(programPath.lexically_relative(root).wstring()) +
                L",\"input_path\":" + JsonString(inputPath.lexically_relative(root).wstring()) +
                L",\"output_path\":" + JsonString(outputPath.lexically_relative(root).wstring()) +
                L",\"program_ops\":" + std::to_wstring(artifact.program.size()) +
                L",\"iterations\":" + std::to_wstring(job->iterations) +
                L",\"instruction_count\":" + std::to_wstring(instructionCount) +
                L",\"bytecode_bytes\":" + std::to_wstring(InterpreterProgramByteCount(artifact.program)) +
                L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
                L",\"input_bytes\":" + std::to_wstring(input.size()) +
                L",\"input_hash32\":" + std::to_wstring(inputHash) +
                L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
                L",\"memory_hash32\":" + std::to_wstring(result.memoryHash) +
                L",\"output_bytes\":" + std::to_wstring(result.output.size()) +
                L",\"output_hash32\":" + std::to_wstring(result.outputHash) +
                L",\"output_sha256\":" + JsonString(outputSha256) +
                L",\"seed\":" + std::to_wstring(artifact.seed) +
                L",\"elapsed_ms\":" + DoubleJson(elapsedMs) +
                L",\"instructions_per_second\":" + DoubleJson(instructionsPerSecond, 1) +
                L",\"accumulator\":" + std::to_wstring(result.accumulator) +
                L",\"verified\":" + BoolJson(verified) +
                L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
                L"}";
        }
        catch (WorkerProtocolError const& ex)
        {
            status = (ex.code == "job.canceled" || job->cancelRequested.load()) ? L"canceled" : L"failed";
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (std::exception const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            errorCode = L"job.exception";
            errorMessage = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            errorCode = L"job.unhandled";
            errorMessage = L"unhandled async job failure";
        }

        m_asyncJobRuntime.Complete(
            root, job, status, std::move(resultJson), std::move(errorCode), std::move(errorMessage));

        TryStartNextJob(root);
    }

    std::wstring WorkerCommandServer::SubmitStoredMemoryFileJob(JsonObject const& request, fs::path const& root)
    {
        auto programId = GetOptionalString(request, L"program_id", L"default");
        auto programPath = ResolveMemoryProgramPath(root, programId);
        if (!fs::is_regular_file(programPath))
        {
            throw WorkerProtocolError("memory_program.not_found", "memory program artifact was not found");
        }

        auto inputPathText = GetOptionalString(request, L"input_path");
        if (inputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.input_path_required", "input_path is required for submit_stored_memory_file_job");
        }
        auto outputPathText = GetOptionalString(request, L"output_path");
        if (outputPathText.empty())
        {
            throw WorkerProtocolError("memory_file.output_path_required", "output_path is required for submit_stored_memory_file_job");
        }
        auto inputPath = ResolveWorkspacePath(root, inputPathText);
        auto outputPath = ResolveWorkspacePath(root, outputPathText);
        if (!fs::is_regular_file(inputPath))
        {
            throw WorkerProtocolError("path.not_file", "input path is not a file");
        }

        auto iterations = GetBoundedOptionalUInt64(request, L"iterations", 1000, 1, MaxInterpreterIterations);
        auto outputBytes = GetBoundedOptionalUInt64(request, L"output_bytes", 64, 1, MaxMemoryInterpreterOutputBytes);
        uint64_t memoryBytes = 0;
        if (request.HasKey(L"memory_bytes"))
        {
            memoryBytes = GetBoundedOptionalUInt64(request, L"memory_bytes", 0, 1, MaxMemoryInterpreterBytes);
        }

        auto job = std::make_shared<WorkerAsyncJobState>();
        job->kind = L"async_memory_file_stored";
        job->programId = programId;
        job->inputPath = inputPath.lexically_relative(root).wstring();
        job->outputPath = outputPath.lexically_relative(root).wstring();
        job->iterations = iterations;
        job->memoryBytes = memoryBytes;
        job->outputBytes = outputBytes;

        auto submission = m_asyncJobRuntime.Enqueue(root, job);
        if (!submission.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(submission.errorCode),
                WideToUtf8(submission.errorMessage));
        }

        TryStartNextJob(root);

        auto observation = m_asyncJobRuntime.Observe(job);
        auto status = observation.status;
        auto queueDepth = observation.queueDepth;

        return OkBase(L"submit_stored_memory_file_job") +
            L",\"job\":\"async_memory_file_submit\"" +
            L",\"job_id\":" + JsonString(job->jobId) +
            L",\"status\":" + JsonString(status) +
            L",\"queued\":true" +
            L",\"program_id\":" + JsonString(programId) +
            L",\"program_path\":" + JsonString(programPath.lexically_relative(root).wstring()) +
            L",\"input_path\":" + JsonString(inputPath.lexically_relative(root).wstring()) +
            L",\"output_path\":" + JsonString(outputPath.lexically_relative(root).wstring()) +
            L",\"iterations\":" + std::to_wstring(iterations) +
            L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(outputBytes) +
            L",\"queue_depth\":" + std::to_wstring(queueDepth) +
            L",\"concurrency\":1" +
            L"}";
    }

    void WorkerCommandServer::RunAsyncD3D11ResidentHotLoopJob(std::shared_ptr<WorkerAsyncJobState> job, fs::path root)
    {
        std::wstring status = L"succeeded";
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;

        try
        {
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            JsonObject hotLoopRequest;
            InsertJsonString(hotLoopRequest, L"command", L"run_d3d11_resident_hotloop_job");
            InsertJsonString(hotLoopRequest, L"variant", job->variant);
            InsertJsonNumber(hotLoopRequest, L"elements", job->elements);
            InsertJsonNumber(hotLoopRequest, L"duration_seconds", job->durationSeconds);
            InsertJsonNumber(hotLoopRequest, L"window_seconds", job->windowSeconds);
            InsertJsonNumber(hotLoopRequest, L"dispatches_per_sample", job->dispatchesPerSample);
            InsertJsonNumber(hotLoopRequest, L"warmup_dispatches", job->warmupDispatches);
            InsertJsonNumber(hotLoopRequest, L"max_dispatches", job->maxDispatches);

            resultJson = RunD3D11ResidentHotLoopJob(hotLoopRequest, root);
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }
        }
        catch (WorkerProtocolError const& ex)
        {
            status = (ex.code == "job.canceled" || job->cancelRequested.load()) ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (std::exception const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.exception";
            errorMessage = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.unhandled";
            errorMessage = L"unhandled async D3D11 resident hot-loop failure";
        }

        m_asyncJobRuntime.Complete(
            root, job, status, std::move(resultJson), std::move(errorCode), std::move(errorMessage));

        TryStartNextJob(root);
    }

    std::wstring WorkerCommandServer::SubmitD3D11ResidentHotLoopJob(JsonObject const& request, fs::path const& root)
    {
        auto variants = ParseD3D11ShaderMatrixVariants(GetOptionalString(request, L"variant", L"fp32_alu_128"));
        if (variants.size() != 1)
        {
            throw WorkerProtocolError("d3d11_resident_hotloop.variant_invalid", "resident hot-loop requires exactly one shader matrix variant");
        }
        auto variant = variants.front();
        auto requestedElements = GetBoundedOptionalUInt64(request, L"elements", 1048576, 1, MaxD3D11ComputeElements);
        auto durationSeconds = GetBoundedOptionalUInt64(request, L"duration_seconds", 60, 1, MaxD3D11ResidentHotLoopDurationSeconds);
        auto windowSeconds = GetBoundedOptionalUInt64(request, L"window_seconds", 10, 1, MaxD3D11ResidentHotLoopWindowSeconds);
        auto dispatchesPerSample = GetBoundedOptionalUInt64(request, L"dispatches_per_sample", 16, 1, MaxD3D11ResidentHotLoopDispatchesPerSample);
        auto warmupDispatches = GetBoundedOptionalUInt64(request, L"warmup_dispatches", 16, 0, MaxD3D11ResidentHotLoopWarmupDispatches);
        auto maxDispatches = GetBoundedOptionalUInt64(request, L"max_dispatches", MaxD3D11ResidentHotLoopDispatches, 1, MaxD3D11ResidentHotLoopDispatches);

        auto job = std::make_shared<WorkerAsyncJobState>();
        job->kind = L"async_d3d11_resident_hotloop";
        job->variant = variant.id;
        job->elements = requestedElements;
        job->durationSeconds = durationSeconds;
        job->windowSeconds = windowSeconds;
        job->dispatchesPerSample = dispatchesPerSample;
        job->warmupDispatches = warmupDispatches;
        job->maxDispatches = maxDispatches;

        auto submission = m_asyncJobRuntime.Enqueue(root, job);
        if (!submission.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(submission.errorCode),
                WideToUtf8(submission.errorMessage));
        }

        TryStartNextJob(root);

        auto observation = m_asyncJobRuntime.Observe(job);
        auto status = observation.status;
        auto queueDepth = observation.queueDepth;

        return OkBase(L"submit_d3d11_resident_hotloop_job") +
            L",\"job\":\"async_d3d11_resident_hotloop_submit\"" +
            L",\"job_id\":" + JsonString(job->jobId) +
            L",\"status\":" + JsonString(status) +
            L",\"queued\":true" +
            L",\"variant_id\":" + JsonString(variant.id) +
            L",\"elements\":" + std::to_wstring(requestedElements) +
            L",\"duration_seconds\":" + std::to_wstring(durationSeconds) +
            L",\"window_seconds\":" + std::to_wstring(windowSeconds) +
            L",\"dispatches_per_sample\":" + std::to_wstring(dispatchesPerSample) +
            L",\"warmup_dispatches\":" + std::to_wstring(warmupDispatches) +
            L",\"max_dispatches\":" + std::to_wstring(maxDispatches) +
            L",\"queue_depth\":" + std::to_wstring(queueDepth) +
            L",\"concurrency\":1" +
            L"}";
    }

    void WorkerCommandServer::RunAsyncD3D12ShaderShapeJob(std::shared_ptr<WorkerAsyncJobState> job, fs::path root)
    {
        std::wstring status = L"succeeded";
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;

        try
        {
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }

            JsonObject shaderShapeRequest;
            InsertJsonString(shaderShapeRequest, L"command", L"run_d3d12_shader_shape_job");
            InsertJsonString(shaderShapeRequest, L"elements_list", job->elementsList);
            InsertJsonString(shaderShapeRequest, L"variants", job->shaderVariants);
            InsertJsonNumber(shaderShapeRequest, L"repeats", job->repeats);
            InsertJsonNumber(shaderShapeRequest, L"warmup_repeats", job->warmupRepeats);

            resultJson = RunD3D12ShaderShapeJob(shaderShapeRequest, root);
            if (job->cancelRequested.load())
            {
                throw WorkerProtocolError("job.canceled", "job was canceled");
            }
        }
        catch (WorkerProtocolError const& ex)
        {
            status = (ex.code == "job.canceled" || job->cancelRequested.load()) ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (std::exception const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.exception";
            errorMessage = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.unhandled";
            errorMessage = L"unhandled async D3D12 shader-shape failure";
        }

        m_asyncJobRuntime.Complete(
            root, job, status, std::move(resultJson), std::move(errorCode), std::move(errorMessage));

        TryStartNextJob(root);
    }

    void WorkerCommandServer::RunAsyncD3D12ShaderShapeSoakJob(std::shared_ptr<WorkerAsyncJobState> job, fs::path root)
    {
        std::wstring status = L"succeeded";
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;

        try
        {
            D3D12ShaderShapeSoakOptions options;
            options.matrix.elementsList =
                ParseD3D12ShaderShapeElementsList(job->elementsList);
            options.matrix.variants =
                ParseD3D11ShaderMatrixVariants(job->shaderVariants);
            options.matrix.repeats = job->repeats;
            options.matrix.warmupRepeats = job->warmupRepeats;
            options.windowCount = job->windowCount;
            options.discardInitialWindows = job->discardInitialWindows;
            options.windowPauseMs = job->windowPauseMs;
            auto runtimeResult = WorkerRunD3D12ShaderShapeSoak(
                options,
                [&root]()
                {
                    return ObserveD3DEnvironment(root);
                },
                &job->cancelRequested);

            std::wostringstream windowsJson;
            windowsJson << L"[";
            for (size_t i = 0; i < runtimeResult.windows.size(); ++i)
            {
                if (i != 0)
                {
                    windowsJson << L",";
                }
                auto const& window = runtimeResult.windows[i];
                windowsJson << L"{\"index\":" << window.index <<
                    L",\"measured\":" << BoolJson(window.measured) <<
                    L",\"result\":" << D3D12ShaderShapeMatrixResultJson(
                        L"matrix_block_fp32_v1",
                        options.matrix,
                        window.result,
                        window.environment) <<
                    L"}";
            }
            windowsJson << L"]";

            resultJson = OkBase(L"run_d3d12_shader_shape_soak_job") +
                L",\"job\":\"d3d12_shader_shape_soak\"" +
                L",\"evidence_class\":\"MEASURED\"" +
                L",\"implementation\":\"async_public_uwp_d3d12_precompiled_shader_shape_soak\"" +
                L",\"dynamic_code\":false" +
                L",\"gdk_claim\":false" +
                L",\"runtime_shader_compilation\":false" +
                L",\"elements_list\":" + JsonString(job->elementsList) +
                L",\"variants\":" + JsonString(job->shaderVariants) +
                L",\"repeats\":" + std::to_wstring(job->repeats) +
                L",\"warmup_repeats\":" + std::to_wstring(job->warmupRepeats) +
                L",\"window_count\":" + std::to_wstring(job->windowCount) +
                L",\"discard_initial_windows\":" + std::to_wstring(job->discardInitialWindows) +
                L",\"measured_window_count\":" + std::to_wstring(runtimeResult.measuredWindowCount) +
                L",\"window_pause_ms\":" + std::to_wstring(job->windowPauseMs) +
                L",\"total_fp32_ops_timed\":" + std::to_wstring(runtimeResult.totalFp32OpsTimed) +
                L",\"average_best_fp32_gflops\":" + DoubleJson(runtimeResult.averageBestFp32Gflops, 3) +
                L",\"min_best_fp32_gflops\":" + DoubleJson(runtimeResult.minBestFp32Gflops, 3) +
                L",\"max_best_fp32_gflops\":" + DoubleJson(runtimeResult.maxBestFp32Gflops, 3) +
                L",\"spread_best_fp32_percent\":" + DoubleJson(runtimeResult.spreadBestFp32Percent, 6) +
                L",\"average_aggregate_gflops\":" + DoubleJson(runtimeResult.averageAggregateGflops, 3) +
                L",\"average_total_gpu_dispatch_ms\":" + DoubleJson(runtimeResult.averageTotalGpuDispatchMs, 6) +
                L",\"max_mismatch_count\":" + std::to_wstring(runtimeResult.maxMismatchCount) +
                L",\"max_relative_error\":" + DoubleJson(runtimeResult.maxRelativeError, 9) +
                L",\"app_memory_usage_limit_bytes\":" + std::to_wstring(runtimeResult.minAppMemoryLimitBytes) +
                L",\"verified\":" + BoolJson(runtimeResult.verified) +
                L",\"windows\":" + windowsJson.str() +
                L"}";
        }
        catch (WorkerProtocolError const& ex)
        {
            status = (ex.code == "job.canceled" || job->cancelRequested.load()) ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = Utf8ToWide(ex.code);
            errorMessage = Utf8ToWide(ex.message);
        }
        catch (std::exception const& ex)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.exception";
            errorMessage = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            status = job->cancelRequested.load() ? L"canceled" : L"failed";
            resultJson.clear();
            errorCode = L"job.unhandled";
            errorMessage = L"unhandled async D3D12 shader-shape soak failure";
        }

        m_asyncJobRuntime.Complete(
            root, job, status, std::move(resultJson), std::move(errorCode), std::move(errorMessage));

        TryStartNextJob(root);
    }

    std::wstring WorkerCommandServer::SubmitD3D12ShaderShapeJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsListText = GetOptionalString(request, L"elements_list", L"8388608");
        auto variantsText = GetOptionalString(request, L"variants", L"fp32_alu_128");
        auto elementsList = ParseD3D12ShaderShapeElementsList(elementsListText);
        auto variants = ParseD3D11ShaderMatrixVariants(variantsText);
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 32, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 8, 0, MaxD3D11ComputeSweepRepeats);

        auto job = std::make_shared<WorkerAsyncJobState>();
        job->kind = L"async_d3d12_shader_shape";
        job->elementsList = elementsListText;
        job->shaderVariants = variantsText;
        job->repeats = repeats;
        job->warmupRepeats = warmupRepeats;

        auto submission = m_asyncJobRuntime.Enqueue(root, job);
        if (!submission.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(submission.errorCode),
                WideToUtf8(submission.errorMessage));
        }

        TryStartNextJob(root);

        auto observation = m_asyncJobRuntime.Observe(job);
        auto status = observation.status;
        auto queueDepth = observation.queueDepth;

        return OkBase(L"submit_d3d12_shader_shape_job") +
            L",\"job\":\"async_d3d12_shader_shape_submit\"" +
            L",\"job_id\":" + JsonString(job->jobId) +
            L",\"status\":" + JsonString(status) +
            L",\"queued\":true" +
            L",\"elements_list\":" + JsonString(elementsListText) +
            L",\"variants\":" + JsonString(variantsText) +
            L",\"element_count\":" + std::to_wstring(elementsList.size()) +
            L",\"variant_count\":" + std::to_wstring(variants.size()) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"queue_depth\":" + std::to_wstring(queueDepth) +
            L",\"concurrency\":1" +
            L"}";
    }

    std::wstring WorkerCommandServer::SubmitD3D12ShaderShapeSoakJob(JsonObject const& request, fs::path const& root)
    {
        auto elementsListText = GetOptionalString(request, L"elements_list", L"8388608");
        auto variantsText = GetOptionalString(request, L"variants", L"fp32_alu_128");
        auto elementsList = ParseD3D12ShaderShapeElementsList(elementsListText);
        auto variants = ParseD3D11ShaderMatrixVariants(variantsText);
        auto repeats = GetBoundedOptionalUInt64(request, L"repeats", 32, 1, MaxD3D11ComputeSweepRepeats);
        auto warmupRepeats = GetBoundedOptionalUInt64(request, L"warmup_repeats", 8, 0, MaxD3D11ComputeSweepRepeats);
        auto windowCount = GetBoundedOptionalUInt64(request, L"window_count", 16, 2, MaxD3D12ShaderShapeSoakWindows);
        auto discardInitialWindows = GetBoundedOptionalUInt64(request, L"discard_initial_windows", 1, 0, MaxD3D12ShaderShapeSoakWindows - 1);
        auto windowPauseMs = GetBoundedOptionalUInt64(request, L"window_pause_ms", 0, 0, MaxD3D12ShaderShapeSoakWindowPauseMs);
        if (discardInitialWindows >= windowCount)
        {
            throw WorkerProtocolError("d3d12_shader_shape_soak.discard_invalid", "discard_initial_windows must be smaller than window_count");
        }

        auto job = std::make_shared<WorkerAsyncJobState>();
        job->kind = L"async_d3d12_shader_shape_soak";
        job->elementsList = elementsListText;
        job->shaderVariants = variantsText;
        job->repeats = repeats;
        job->warmupRepeats = warmupRepeats;
        job->windowCount = windowCount;
        job->discardInitialWindows = discardInitialWindows;
        job->windowPauseMs = windowPauseMs;

        auto submission = m_asyncJobRuntime.Enqueue(root, job);
        if (!submission.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(submission.errorCode),
                WideToUtf8(submission.errorMessage));
        }

        TryStartNextJob(root);

        auto observation = m_asyncJobRuntime.Observe(job);
        auto status = observation.status;
        auto queueDepth = observation.queueDepth;

        return OkBase(L"submit_d3d12_shader_shape_soak_job") +
            L",\"job\":\"async_d3d12_shader_shape_soak_submit\"" +
            L",\"job_id\":" + JsonString(job->jobId) +
            L",\"status\":" + JsonString(status) +
            L",\"queued\":true" +
            L",\"elements_list\":" + JsonString(elementsListText) +
            L",\"variants\":" + JsonString(variantsText) +
            L",\"element_count\":" + std::to_wstring(elementsList.size()) +
            L",\"variant_count\":" + std::to_wstring(variants.size()) +
            L",\"repeats\":" + std::to_wstring(repeats) +
            L",\"warmup_repeats\":" + std::to_wstring(warmupRepeats) +
            L",\"window_count\":" + std::to_wstring(windowCount) +
            L",\"discard_initial_windows\":" + std::to_wstring(discardInitialWindows) +
            L",\"window_pause_ms\":" + std::to_wstring(windowPauseMs) +
            L",\"queue_depth\":" + std::to_wstring(queueDepth) +
            L",\"concurrency\":1" +
            L"}";
    }

    std::wstring WorkerCommandServer::GetJobStatus(JsonObject const& request, fs::path const& root)
    {
        auto jobId = GetOptionalString(request, L"job_id");
        if (jobId.empty())
        {
            throw WorkerProtocolError("job_id.required", "job_id is required");
        }

        auto record = m_asyncJobRuntime.Read(root, jobId, false);
        if (!record.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(record.operationErrorCode),
                WideToUtf8(record.operationErrorMessage));
        }

        return OkBase(L"get_job_status") +
            L",\"persisted\":" + BoolJson(record.persisted) +
            L",\"job_status\":" + record.jobStatusJson +
            L"}";
    }

    std::wstring WorkerCommandServer::GetJobResult(JsonObject const& request, fs::path const& root)
    {
        auto jobId = GetOptionalString(request, L"job_id");
        if (jobId.empty())
        {
            throw WorkerProtocolError("job_id.required", "job_id is required");
        }

        auto record = m_asyncJobRuntime.Read(root, jobId, true);
        if (!record.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(record.operationErrorCode),
                WideToUtf8(record.operationErrorMessage));
        }

        std::wstring response = OkBase(L"get_job_result") +
            L",\"persisted\":" + BoolJson(record.persisted);
        if (!record.persisted)
        {
            response += L",\"snapshot_persisted\":" + BoolJson(record.snapshotPersisted) +
                L",\"snapshot_persist_error\":" + JsonString(record.snapshotPersistError);
        }
        response += L",\"job_id\":" + JsonString(record.jobId) +
            L",\"status\":" + JsonString(record.status) +
            L",\"completed\":" + BoolJson(record.completed) +
            L",\"result_available\":" + BoolJson(record.resultAvailable);
        if (record.resultAvailable)
        {
            response += L",\"result\":" + record.resultJson;
        }
        if (!record.errorCode.empty())
        {
            response += L",\"error\":{\"code\":" + JsonString(record.errorCode) +
                L",\"message\":" + JsonString(record.errorMessage) + L"}";
        }
        response += L"}";
        return response;
    }

    std::wstring WorkerCommandServer::CancelJob(JsonObject const& request, fs::path const& root)
    {
        auto jobId = GetOptionalString(request, L"job_id");
        if (jobId.empty())
        {
            throw WorkerProtocolError("job_id.required", "job_id is required");
        }

        auto result = m_asyncJobRuntime.RequestCancel(root, jobId);
        if (!result.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(result.errorCode),
                WideToUtf8(result.errorMessage));
        }

        return OkBase(L"cancel_job") +
            L",\"job_id\":" + JsonString(result.jobId) +
            L",\"status\":" + JsonString(result.status) +
            L",\"cancel_requested\":" + BoolJson(result.cancelRequested) +
            L",\"cancel_accepted\":" + BoolJson(result.cancelAccepted) +
            L",\"snapshot_persisted\":" + BoolJson(result.snapshotPersisted) +
            L"}";
    }

    std::wstring WorkerCommandServer::PurgeJob(JsonObject const& request, fs::path const& root)
    {
        auto jobId = GetOptionalString(request, L"job_id");
        if (jobId.empty())
        {
            throw WorkerProtocolError("job_id.required", "job_id is required");
        }

        auto preserveSnapshot = request.GetNamedBoolean(L"preserve_snapshot", false);
        auto result = m_asyncJobRuntime.Purge(root, jobId, preserveSnapshot);
        if (!result.ok)
        {
            throw WorkerProtocolError(
                WideToUtf8(result.errorCode),
                WideToUtf8(result.errorMessage));
        }

        return OkBase(L"purge_job") +
            L",\"job_id\":" + JsonString(result.jobId) +
            L",\"memory_existed\":" + BoolJson(result.memoryExisted) +
            L",\"queue_removed\":" + BoolJson(result.queueRemoved) +
            L",\"persisted_existed\":" + BoolJson(result.persistedExisted) +
            L",\"preserve_snapshot\":" + BoolJson(result.preserveSnapshot) +
            L",\"persisted_removed\":" + BoolJson(result.persistedRemoved) +
            L",\"purged\":" + BoolJson(result.purged) +
            L"}";
    }

    std::wstring WorkerCommandServer::ListJobs(fs::path const& root)
    {
        auto result = m_asyncJobRuntime.List(root);
        return OkBase(L"list_jobs") +
            L",\"job_count\":" + std::to_wstring(result.jobCount) +
            L",\"memory_job_count\":" + std::to_wstring(result.memoryJobCount) +
            L",\"persisted_job_count\":" + std::to_wstring(result.persistedJobCount) +
            L",\"concurrency\":1" +
            L",\"jobs\":" + result.jobsJson +
            L"}";
    }

    std::wstring WorkerCommandServer::ExecuteTaskPlanCommand(
        std::wstring const& command,
        JsonObject const& request,
        fs::path const& root)
    {
        try
        {
            return m_taskPlanRuntime.ExecuteCommand(command, request, root);
        }
        catch (WorkerTaskPlanError const& error)
        {
            throw WorkerProtocolError(error.code, error.message);
        }
    }

    std::wstring WorkerCommandServer::ExecuteTaskPlanAction(
        std::wstring const& action,
        JsonObject const& step,
        fs::path const& root)
    {
        if (action == L"run_d3d11_fp32_timing_job")
        {
            return RunD3D11Fp32TimingJob(step, root);
        }
        if (action == L"run_d3d11_compute_timing_job")
        {
            return RunD3D11ComputeTimingJob(step, root);
        }
        if (action == L"run_d3d11_compute_sweep_job")
        {
            return RunD3D11ComputeSweepJob(step, root);
        }
        if (action == L"run_d3d11_compute_job")
        {
            return RunD3D11ComputeJob(step, root);
        }
        if (action == L"store_memory_program")
        {
            return ExecuteStoreMemoryProgram(step, root);
        }
        if (action == L"submit_stored_memory_file_job")
        {
            return SubmitStoredMemoryFileJob(step, root);
        }
        if (action == L"get_job_status")
        {
            return GetJobStatus(step, root);
        }
        if (action == L"get_job_result")
        {
            return GetJobResult(step, root);
        }
        if (action == L"purge_job")
        {
            return PurgeJob(step, root);
        }

        auto stepId = GetOptionalString(step, L"step_id");
        if (action == L"write_chunk")
        {
            auto pathText = GetOptionalString(step, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "write_chunk task step requires path");
            }
            auto dataBase64 = GetOptionalString(step, L"data_base64");
            if (dataBase64.empty())
            {
                throw WorkerProtocolError("task_plan.data_required", "write_chunk task step requires data_base64");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            auto offset = GetOptionalUInt64(step, L"chunk_offset", 0);
            auto data = Base64Decode(WideToUtf8(dataBase64));
            auto truncate = step.GetNamedBoolean(L"truncate", false);
            WriteBinaryChunk(path, offset, data, truncate);
            auto expectedBytes = GetOptionalUInt64(
                step,
                L"chunk_bytes",
                static_cast<uint64_t>(data.size()));
            auto verified = expectedBytes == static_cast<uint64_t>(data.size());
            return L"{\"step_id\":" + JsonString(stepId) +
                L",\"action\":\"write_chunk\",\"path\":" +
                JsonString(path.lexically_relative(root).wstring()) +
                L",\"offset\":" + std::to_wstring(offset) +
                L",\"bytes_written\":" + std::to_wstring(data.size()) +
                L",\"verified\":" + BoolJson(verified) +
                L"}";
        }
        if (action == L"read_chunk")
        {
            auto pathText = GetOptionalString(step, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "read_chunk task step requires path");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            auto offset = GetOptionalUInt64(step, L"chunk_offset", 0);
            auto length = GetOptionalUInt64(
                step,
                L"expected_bytes",
                GetOptionalUInt64(step, L"chunk_bytes", MaxChunkBytes));
            bool eof = false;
            auto data = ReadBinaryChunk(path, offset, length, eof);
            auto encoded = Base64Encode(data.data(), data.size());
            auto expectedBase64 = WideToUtf8(GetOptionalString(step, L"expected_base64"));
            auto verified = expectedBase64.empty() || encoded == expectedBase64;
            if (!verified)
            {
                throw WorkerProtocolError(
                    "task_plan.verify_failed",
                    "read_chunk task step expected_base64 mismatch");
            }
            return L"{\"step_id\":" + JsonString(stepId) +
                L",\"action\":\"read_chunk\",\"path\":" +
                JsonString(path.lexically_relative(root).wstring()) +
                L",\"offset\":" + std::to_wstring(offset) +
                L",\"bytes_read\":" + std::to_wstring(data.size()) +
                L",\"eof\":" + BoolJson(eof) +
                L",\"verified\":" + BoolJson(verified) +
                L"}";
        }
        if (action == L"hash_file")
        {
            auto pathText = GetOptionalString(step, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "hash_file task step requires path");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            if (!fs::is_regular_file(path))
            {
                throw WorkerProtocolError("path.not_file", "hash_file task step path is not a file");
            }
            auto sha256 = Sha256File(path);
            auto expectedSha256 = GetOptionalString(step, L"expected_sha256");
            auto verified = expectedSha256.empty() || expectedSha256 == sha256;
            if (!verified)
            {
                throw WorkerProtocolError(
                    "task_plan.verify_failed",
                    "hash_file task step expected_sha256 mismatch");
            }
            return L"{\"step_id\":" + JsonString(stepId) +
                L",\"action\":\"hash_file\",\"path\":" +
                JsonString(path.lexically_relative(root).wstring()) +
                L",\"bytes\":" +
                std::to_wstring(static_cast<uint64_t>(fs::file_size(path))) +
                L",\"sha256\":" + JsonString(sha256) +
                L",\"verified\":" + BoolJson(verified) +
                L"}";
        }
        if (action == L"delete")
        {
            auto pathText = GetOptionalString(step, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "delete task step requires path");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            auto recursive = step.GetNamedBoolean(L"recursive", false);
            bool existed = fs::exists(path);
            uint64_t removed = 0;
            if (existed)
            {
                if (fs::is_directory(path))
                {
                    if (!recursive)
                    {
                        throw WorkerProtocolError(
                            "delete.recursive_required",
                            "delete directory task step requires recursive=true");
                    }
                    removed = static_cast<uint64_t>(fs::remove_all(path));
                }
                else
                {
                    removed = fs::remove(path) ? 1 : 0;
                }
            }
            auto verified = !fs::exists(path);
            return L"{\"step_id\":" + JsonString(stepId) +
                L",\"action\":\"delete\",\"path\":" +
                JsonString(path.lexically_relative(root).wstring()) +
                L",\"existed\":" + BoolJson(existed) +
                L",\"removed\":" + std::to_wstring(removed) +
                L",\"verified\":" + BoolJson(verified) +
                L"}";
        }
        if (action == L"mkdir")
        {
            auto pathText = GetOptionalString(step, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "mkdir task step requires path");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            fs::create_directories(path);
            return L"{\"step_id\":" + JsonString(stepId) +
                L",\"action\":\"mkdir\",\"path\":" +
                JsonString(path.lexically_relative(root).wstring()) +
                L",\"verified\":" + BoolJson(fs::is_directory(path)) +
                L"}";
        }

        throw WorkerProtocolError(
            "task_plan.action_unsupported",
            "task-plan action is not supported by the injected executor");
    }

    std::wstring WorkerCommandServer::ExecuteCommand(JsonObject const& request, fs::path const& root)
    {
        auto command = GetOptionalString(request, L"command");
        if (command.empty())
        {
            throw WorkerProtocolError("command.required", "command is required");
        }

        if (command == L"ping")
        {
            return OkBase(command) + L",\"message\":\"pong\",\"workspace_path\":" + JsonString(root.wstring()) + L"}";
        }

        if (command == L"stat")
        {
            return OkBase(command) +
                L",\"workspace_path\":" + JsonString(root.wstring()) +
                L",\"file_count\":" + std::to_wstring(WorkspaceFileCount(root)) +
                L",\"workspace_bytes\":" + std::to_wstring(WorkspaceBytes(root)) +
                L"}";
        }

        if (command == L"export_diagnostics")
        {
            return ExportDiagnostics(root);
        }

        if (command == L"read_diagnostics")
        {
            return ReadDiagnostics(root);
        }
        if (command == L"register_trusted_controller")
        {
            return RegisterTrustedController(request, root);
        }

        if (command == L"list_trusted_controllers")
        {
            return ListTrustedControllers(root);
        }

        if (command == L"remove_trusted_controller")
        {
            return RemoveTrustedController(request, root);
        }
        if (command == L"describe_runtime")
        {
            return DescribeRuntime(root);
        }
        if (command == L"describe_creative_host")
        {
            return OkBase(command) +
                L",\"schema_version\":\"xcp-creative-host-description-v1\"" +
                L",\"source_profile_path\":\"profiles/creative/xcp-creative-host-development-v1.json\"" +
                L",\"source_profile_sha256\":" +
                JsonString(WorkerCreativeHostProfileSourceSha256()) +
                L",\"canonical_profile_sha256\":" +
                JsonString(WorkerCreativeHostProfileCanonicalSha256()) +
                L",\"creative_host\":" +
                WorkerCreativeHostProfileJson() +
                L"}";
        }
        if (command == L"prepare_creative_install")
        {
            return WorkerPrepareCreativeInstall(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"commit_creative_install")
        {
            return WorkerCommitCreativeInstall(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"list_creative_installs")
        {
            return WorkerListCreativeInstalls(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"activate_creative_install")
        {
            return WorkerActivateCreativeInstall(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"rollback_creative_activation")
        {
            return WorkerRollbackCreativeActivation(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"remove_creative_install")
        {
            return WorkerRemoveCreativeInstall(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"launch_creative_project")
        {
            return m_creativeForegroundRuntime.Launch(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"reload_creative_project")
        {
            return m_creativeForegroundRuntime.Reload(
                request,
                WorkerProtocolVersion,
                root);
        }
        if (command == L"observe_creative_foreground")
        {
            return m_creativeForegroundRuntime.Observe(
                request,
                WorkerProtocolVersion);
        }
        if (command == L"dispatch_creative_input")
        {
            return m_creativeForegroundRuntime.DispatchInput(
                request,
                WorkerProtocolVersion);
        }
        if (command == L"capture_creative_frame")
        {
            return m_creativeForegroundRuntime.CaptureFrame(
                request,
                WorkerProtocolVersion);
        }
        if (command == L"describe_submission_profile")
        {
            return OkBase(command) +
                L",\"schema_version\":\"xvm-submission-profile-description-v1\"" +
                L",\"submission_profile\":" +
                WorkerXvmSubmissionProfileJson() +
                L"}";
        }
        if (command == L"describe_xvm_isa")
        {
            return OkBase(command) +
                L",\"schema_version\":\"xvm-isa-description-v1\"" +
                L",\"source_schema_path\":\"schemas/xvm-isa-v2.json\"" +
                L",\"source_schema_sha256\":" +
                JsonString(WorkerXvmIsaDefinitionSha256()) +
                L",\"isa\":" +
                WorkerXvmIsaDefinitionJson() +
                L"}";
        }
        if (command == L"probe_process_topology") return m_processTopologyRuntime.Probe(request, WorkerProtocolVersion);
        if (command == L"probe_persistent_compute_coordinator") return m_persistentComputeCoordinatorRuntime.Probe(request, WorkerProtocolVersion);
        if (command == L"probe_content_addressed_store_recovery") return m_persistentComputeCoordinatorRuntime.ProbeContentAddressedStoreRecovery(request, WorkerProtocolVersion, root);
        if (command == L"run_provable_worlds_streaming_tiled_v1") return m_persistentComputeCoordinatorRuntime.RunProvableWorldsStreamingTiledV1(request, WorkerProtocolVersion, root, ArtifactStoreConfig(), [] { return GenerateSessionId(); });
        if (command == L"run_storage_scale_characterization_v1") return m_persistentComputeCoordinatorRuntime.RunStorageScaleCharacterizationV1(request, WorkerProtocolVersion, root);
        if (command == L"probe_cpu_capsule_resolution")
        {
            return WorkerCpuCapsuleResolutionProbeJson(WorkerProtocolVersion);
        }
        if (command == L"probe_cpu_capsule_package_graph")
        {
            return m_cpuCapsuleRuntime.Probe(WorkerProtocolVersion);
        }
        if (command == L"test_cpu_capsule_quarantine_lifecycle")
        {
            return WorkerXvmCpuCapsuleQuarantineDiagnosticJson(WorkerProtocolVersion);
        }

        if (command == L"list_tree")
        {
            auto base = ResolveOptionalWorkspacePath(root, GetOptionalString(request, L"path"));
            auto includeHash = request.GetNamedBoolean(L"include_hash", false);
            auto entries = ListTree(root, base, includeHash);
            return OkBase(command) +
                L",\"path\":" + JsonString(base.lexically_relative(root).wstring()) +
                L",\"include_hash\":" + BoolJson(includeHash) +
                L",\"file_count\":" + std::to_wstring(entries.size()) +
                L",\"files\":" + JsonTreeEntries(entries) +
                L"}";
        }

        if (command == L"hash_file")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            if (!fs::is_regular_file(path))
            {
                throw WorkerProtocolError("path.not_file", "hash_file path is not a file");
            }
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"bytes\":" + std::to_wstring(static_cast<uint64_t>(fs::file_size(path))) +
                L",\"sha256\":" + JsonString(Sha256File(path)) +
                L"}";
        }

        if (command == L"mkdir")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            fs::create_directories(path);
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"exists\":true" +
                L"}";
        }

        if (command == L"delete")
        {
            auto pathText = GetOptionalString(request, L"path");
            if (pathText.empty())
            {
                throw WorkerProtocolError("path.required", "delete path is required");
            }
            auto path = ResolveWorkspacePath(root, pathText);
            auto recursive = request.GetNamedBoolean(L"recursive", false);
            bool existed = fs::exists(path);
            uint64_t removed = 0;
            if (existed)
            {
                if (fs::is_directory(path))
                {
                    if (!recursive)
                    {
                        throw WorkerProtocolError("delete.recursive_required", "delete directory requires recursive=true");
                    }
                    removed = static_cast<uint64_t>(fs::remove_all(path));
                }
                else
                {
                    removed = fs::remove(path) ? 1 : 0;
                }
            }
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"existed\":" + BoolJson(existed) +
                L",\"removed\":" + std::to_wstring(removed) +
                L"}";
        }

        if (command == L"artifact_quota_preflight")
        {
            return ExecuteArtifactQuotaPreflight(request, root);
        }

        if (command == L"begin_artifact_upload")
        {
            return ExecuteBeginArtifactUpload(request, root);
        }

        if (command == L"append_artifact_chunk")
        {
            return ExecuteAppendArtifactChunk(request, root);
        }

        if (command == L"commit_artifact_upload")
        {
            return ExecuteCommitArtifactUpload(request, root);
        }

        if (command == L"generate_artifact_dataset")
        {
            return ExecuteGenerateArtifactDataset(request, root);
        }

        if (command == L"abort_artifact_upload")
        {
            return ExecuteAbortArtifactUpload(request, root);
        }

        if (command == L"delete_artifact")
        {
            return ExecuteDeleteArtifact(request, root);
        }

        if (command == L"reap_artifacts")
        {
            return ExecuteReapArtifacts(request, root);
        }

        if (command == L"get_artifact_status")
        {
            return ExecuteGetArtifactStatus(request, root);
        }

        if (command == L"run_artifact_manifest_job")
        {
            return ExecuteRunArtifactManifestJob(request, root);
        }
        if (command == L"run_artifact_manifest_compute_job")
        {
            return ExecuteRunArtifactManifestComputeJob(request, root);
        }

        if (command == L"submit_graph")
        {
            return ExecuteSubmitGraph(request, root);
        }

        if (command == L"submit_graph_job")
        {
            return SubmitGraphJob(request, root);
        }

        if (command == L"submit_macro")
        {
            return ExecuteSubmitMacro(request, root);
        }

        if (command == L"run_physics_kernel_job")
        {
            return ExecuteRunPhysicsKernelJob(request, root);
        }

        if (command == L"search")
        {
            auto needle = WideToUtf8(GetOptionalString(request, L"needle"));
            auto stats = SearchWorkspace(root, needle);
            return OkBase(command) +
                L",\"files_scanned\":" + std::to_wstring(stats.filesScanned) +
                L",\"hit_count\":" + std::to_wstring(stats.hitCount) +
                L",\"hits\":" + JsonStringArray(stats.hits) +
                L"}";
        }

        if (command == L"run_native_bench")
        {
            return ExecuteRunNativeBench(request, root);
        }

        if (command == L"run_hash_job")
        {
            return ExecuteRunHashJob(request, root);
        }

        if (command == L"run_vector_job")
        {
            return ExecuteRunVectorJob(request, root);
        }

        if (command == L"run_d3d11_compute_job")
        {
            return RunD3D11ComputeJob(request, root);
        }

        if (command == L"run_d3d11_compute_sweep_job")
        {
            return RunD3D11ComputeSweepJob(request, root);
        }

        if (command == L"run_d3d11_compute_timing_job")
        {
            return RunD3D11ComputeTimingJob(request, root);
        }

        if (command == L"run_d3d11_fp32_timing_job")
        {
            return RunD3D11Fp32TimingJob(request, root);
        }

        if (command == L"run_d3d11_shader_matrix_job")
        {
            return RunD3D11ShaderMatrixJob(request, root);
        }

        if (command == L"run_d3d11_shader_matrix_stats_job")
        {
            return RunD3D11ShaderMatrixStatsJob(request, root);
        }

        if (command == L"run_d3d11_sustained_soak_job")
        {
            return RunD3D11SustainedSoakJob(request, root);
        }

        if (command == L"run_d3d11_resident_hotloop_job")
        {
            return RunD3D11ResidentHotLoopJob(request, root);
        }

        if (command == L"submit_d3d11_resident_hotloop_job")
        {
            return SubmitD3D11ResidentHotLoopJob(request, root);
        }

        if (command == L"run_d3d12_device_probe_job")
        {
            return RunD3D12DeviceProbeJob(request, root);
        }

        if (command == L"run_d3d12_compute_smoke_job")
        {
            return RunD3D12ComputeSmokeJob(request, root);
        }

        if (command == L"run_d3d12_compute_sweep_job")
        {
            return RunD3D12ComputeSweepJob(request, root);
        }

        if (command == L"run_d3d12_compute_timing_job")
        {
            return RunD3D12ComputeTimingJob(request, root);
        }

        if (command == L"run_d3d12_fp32_timing_job")
        {
            return RunD3D12Fp32TimingJob(request, root);
        }

        if (command == L"run_d3d12_shader_shape_job")
        {
            return RunD3D12ShaderShapeJob(request, root);
        }

        if (command == L"submit_d3d12_shader_shape_job")
        {
            return SubmitD3D12ShaderShapeJob(request, root);
        }

        if (command == L"submit_d3d12_shader_shape_soak_job")
        {
            return SubmitD3D12ShaderShapeSoakJob(request, root);
        }

        if (command == L"run_interpreter_job")
        {
            return ExecuteRunInterpreterJob(request, root);
        }

        if (command == L"store_interpreter_program")
        {
            return ExecuteStoreInterpreterProgram(request, root);
        }

        if (command == L"run_stored_interpreter_job")
        {
            return ExecuteRunStoredInterpreterJob(request, root);
        }

        if (command == L"list_interpreter_programs")
        {
            return ExecuteListInterpreterPrograms(root);
        }

        if (command == L"delete_interpreter_program")
        {
            return ExecuteDeleteInterpreterProgram(request, root);
        }

        if (command == L"store_memory_program")
        {
            return ExecuteStoreMemoryProgram(request, root);
        }

        if (command == L"list_memory_programs")
        {
            return ExecuteListMemoryPrograms(root);
        }

        if (command == L"run_stored_memory_file_job")
        {
            return ExecuteRunStoredMemoryFileJob(request, root);
        }

        if (command == L"delete_memory_program")
        {
            return ExecuteDeleteMemoryProgram(request, root);
        }

        if (command == L"submit_stored_memory_file_job")
        {
            return SubmitStoredMemoryFileJob(request, root);
        }

        if (command == L"get_job_status")
        {
            return GetJobStatus(request, root);
        }

        if (command == L"get_job_result")
        {
            return GetJobResult(request, root);
        }

        if (command == L"publish_job_result_artifact")
        {
            return WorkerPublishJobResultArtifact(
                request,
                root,
                ArtifactPublicationConfig(),
                [this](fs::path const& jobRoot, std::wstring const& jobId)
                {
                    return m_asyncJobRuntime.Read(jobRoot, jobId, true);
                });
        }

        if (command == L"cancel_job")
        {
            return CancelJob(request, root);
        }

        if (command == L"purge_job")
        {
            return PurgeJob(request, root);
        }

        if (command == L"list_jobs")
        {
            return ListJobs(root);
        }

        if (WorkerTaskPlanRuntime::HandlesCommand(command))
        {
            return ExecuteTaskPlanCommand(command, request, root);
        }

        if (command == L"run_memory_interpreter_job")
        {
            return ExecuteRunMemoryInterpreterJob(request, root);
        }

        if (command == L"run_memory_file_job")
        {
            return ExecuteRunMemoryFileJob(request, root);
        }

        if (command == L"read")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            auto content = ReadTextFile(path);
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"content\":" + JsonString(Utf8ToWide(content)) +
                L",\"bytes\":" + std::to_wstring(content.size()) +
                L"}";
        }

        if (command == L"write")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            auto content = WideToUtf8(GetOptionalString(request, L"content"));
            WriteTextFile(path, content);
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"bytes_written\":" + std::to_wstring(content.size()) +
                L"}";
        }

        if (command == L"patch")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            auto before = WideToUtf8(GetOptionalString(request, L"before"));
            auto after = WideToUtf8(GetOptionalString(request, L"after"));
            if (before.empty())
            {
                throw WorkerProtocolError("patch.empty_before", "patch.before cannot be empty");
            }

            auto content = ReadTextFile(path);
            auto position = content.find(before);
            if (position == std::string::npos)
            {
                throw WorkerProtocolError("patch.before_not_found", "patch.before not found");
            }
            content.replace(position, before.size(), after);
            WriteTextFile(path, content);
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"replaced\":true,\"bytes_written\":" + std::to_wstring(content.size()) +
                L"}";
        }

        if (command == L"read_chunk")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            auto offset = GetOptionalUInt64(request, L"offset", 0);
            auto length = GetOptionalUInt64(request, L"length", MaxChunkBytes);
            bool eof = false;
            auto data = ReadBinaryChunk(path, offset, length, eof);
            auto encoded = Base64Encode(data.data(), data.size());
            return OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"offset\":" + std::to_wstring(offset) +
                L",\"bytes_read\":" + std::to_wstring(data.size()) +
                L",\"eof\":" + BoolJson(eof) +
                L",\"encoding\":\"base64\",\"data_base64\":" + JsonString(Utf8ToWide(encoded)) +
                L"}";
        }

        if (command == L"write_chunk")
        {
            auto path = ResolveWorkspacePath(root, GetOptionalString(request, L"path"));
            auto offset = GetOptionalUInt64(request, L"offset", 0);
            auto dataBase64 = WideToUtf8(GetOptionalString(request, L"data_base64"));
            auto truncate = request.GetNamedBoolean(L"truncate", false);
            auto decodeStarted = std::chrono::steady_clock::now();
            auto data = Base64Decode(dataBase64);
            auto decodeMs = ElapsedMilliseconds(decodeStarted);
            auto writeStarted = std::chrono::steady_clock::now();
            WriteBinaryChunk(path, offset, data, truncate);
            auto writeMs = ElapsedMilliseconds(writeStarted);
            auto responseStarted = std::chrono::steady_clock::now();
            auto response = OkBase(command) +
                L",\"path\":" + JsonString(path.lexically_relative(root).wstring()) +
                L",\"offset\":" + std::to_wstring(offset) +
                L",\"bytes_written\":" + std::to_wstring(data.size()) +
                L",\"truncated\":" + BoolJson(truncate);
            auto responseMs = ElapsedMilliseconds(responseStarted);
            response += SyncServerTimingJson(request, 0.0, decodeMs, writeMs, responseMs, dataBase64.size(), data.size());
            response += L"}";
            return response;
        }

        throw WorkerProtocolError("command.unknown", "unknown command");
    }

    std::wstring WorkerCommandServer::ProcessCommand(std::wstring const& requestJson)
    {
        return m_protocolBoundary.TranslateRequest([&]()
        {
            auto parseStarted = std::chrono::steady_clock::now();
            auto request = JsonObject::Parse(requestJson);
            auto parseMs = ElapsedMilliseconds(parseStarted);
            request.Insert(L"_server_parse_json_ms", JsonValue::CreateNumberValue(parseMs));
            request.Insert(L"_server_request_bytes", JsonValue::CreateNumberValue(static_cast<double>(WideToUtf8(requestJson).size())));

            auto root = WorkerRoot();
            EnsureWorkspace(root);

            WorkerProtocolDispatchCallbacks callbacks;
            callbacks.openTrustedSession = [this](JsonObject const& trustedRequest, fs::path const& workerRoot)
            {
                return OpenSessionWithTrust(trustedRequest, workerRoot);
            };
            callbacks.executeAuthorizedCommand = [this](JsonObject const& authorizedRequest, fs::path const& workerRoot)
            {
                return ExecuteCommand(authorizedRequest, workerRoot);
            };
            return m_protocolBoundary.Dispatch(request, root, callbacks);
        });
    }
}
