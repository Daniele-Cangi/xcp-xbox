#include "pch.h"
#include "WorkerProcessTopologyBrokerStorageRuntime.h"
#include "WorkerProcessTopologyBrokerProtocol.h"
#include <winrt/Windows.Storage.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage;
namespace fs = std::filesystem;

namespace winrt::XComputeTopologyBroker::implementation
{
    namespace
    {
        constexpr uint64_t MaximumStorageProbeBytes = 1024ull * 1024ull;
        std::wstring RequiredString(JsonObject const& request, wchar_t const* name)
        {
            if (!request.HasKey(name) || request.GetNamedValue(name).ValueType() != JsonValueType::String)
            {
                FailBrokerRequest(L"STORAGE_PROBE_INVALID", std::wstring(L"storage field must be a string: ") + name);
            }
            return std::wstring(request.GetNamedString(name).c_str());
        }
        bool IsLowerHex(std::wstring const& value)
        {
            return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
            });
        }
        std::wstring StorageFileName(std::wstring const& runId, std::wstring_view owner)
        {
            return L"xcp-t2-storage-" + runId + L"-" + std::wstring(owner) + L".bin";
        }
        fs::path StoragePath(std::wstring const& filename)
        {
            auto folder = ApplicationData::Current().LocalFolder();
            return fs::path(std::wstring(folder.Path().c_str())) / filename;
        }
        std::wstring Sha256(std::vector<uint8_t> const& bytes)
        {
            auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
            auto encoded = CryptographicBuffer::EncodeToHexString(
                provider.HashData(CryptographicBuffer::CreateFromByteArray(bytes)));
            auto value = std::wstring(encoded.c_str());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }
        std::vector<uint8_t> ReadExact(fs::path const& path, uint64_t expectedBytes)
        {
            std::error_code error;
            auto size = fs::file_size(path, error);
            if (error || size != expectedBytes)
            {
                FailBrokerRequest(L"STORAGE_IO_FAILED", L"foreground storage file size differs from the admitted payload");
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                FailBrokerRequest(L"STORAGE_IO_FAILED", L"foreground storage file is not visible to the broker");
            }
            std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
            if (bytes.size() != expectedBytes)
            {
                FailBrokerRequest(L"STORAGE_IO_FAILED", L"foreground storage file read was incomplete");
            }
            return bytes;
        }
        void WriteExact(fs::path const& path, std::vector<uint8_t> const& bytes)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                FailBrokerRequest(L"STORAGE_IO_FAILED", L"broker storage file could not be created");
            }
            output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output)
            {
                FailBrokerRequest(L"STORAGE_IO_FAILED", L"broker storage file write was incomplete");
            }
        }
        void RequireActiveStoragePhase(WorkerProcessTopologyBrokerPhaseRuntime const& runtime, std::wstring const& runId)
        {
            if (!runtime.AllowsDiagnosticStorage(runId))
            {
                FailBrokerRequest(L"STORAGE_PHASE_INACTIVE", L"storage commands require the exact active storage_visibility phase run");
            }
        }
    }

    JsonObject WorkerProcessTopologyBrokerStorageRuntime::Exchange(
        WorkerProcessTopologyBrokerPhaseRuntime const& runtime,
        JsonObject const& request)
    {
        auto runId = RequiredString(request, L"phase_run_id");
        RequireActiveStoragePhase(runtime, runId);
        auto bytes = ReadBrokerIntegral(request, L"bytes", 0, true);
        auto expectedSha256 = RequiredString(request, L"payload_sha256");
        if (bytes == 0 || bytes > MaximumStorageProbeBytes || !IsLowerHex(expectedSha256))
        {
            FailBrokerRequest(L"STORAGE_PROBE_INVALID", L"storage bytes or hash are outside the bounded diagnostic contract");
        }
        auto workerPath = StoragePath(StorageFileName(runId, L"foreground"));
        auto brokerPath = StoragePath(StorageFileName(runId, L"broker"));
        if (fs::exists(brokerPath))
        {
            FailBrokerRequest(L"STORAGE_IO_FAILED", L"broker storage output already exists before exchange");
        }
        auto workerBytes = ReadExact(workerPath, bytes);
        auto workerReaderSha256 = Sha256(workerBytes);
        if (workerReaderSha256 != expectedSha256)
        {
            FailBrokerRequest(L"STORAGE_IO_FAILED", L"foreground storage hash differs at the broker boundary");
        }
        std::error_code removeError;
        auto workerFileDeleted = fs::remove(workerPath, removeError);
        if (removeError || !workerFileDeleted)
        {
            FailBrokerRequest(L"STORAGE_IO_FAILED", L"broker could not delete the verified foreground probe file");
        }
        std::vector<uint8_t> brokerBytes(static_cast<size_t>(bytes), static_cast<uint8_t>('x'));
        auto brokerWriterSha256 = Sha256(brokerBytes);
        if (brokerWriterSha256 != expectedSha256)
        {
            FailBrokerRequest(L"STORAGE_IO_FAILED", L"broker deterministic payload hash differs from the request");
        }
        WriteExact(brokerPath, brokerBytes);
        JsonObject result;
        PutBrokerNumber(result, L"bytes", bytes);
        PutBrokerString(result, L"payload_sha256", expectedSha256);
        PutBrokerString(result, L"worker_reader_sha256", workerReaderSha256);
        PutBrokerString(result, L"broker_writer_sha256", brokerWriterSha256);
        PutBrokerBoolean(result, L"worker_file_deleted", true);
        PutBrokerBoolean(result, L"broker_file_created", true);
        return result;
    }

    JsonObject WorkerProcessTopologyBrokerStorageRuntime::CleanupStatus(
        WorkerProcessTopologyBrokerPhaseRuntime const& runtime,
        JsonObject const& request)
    {
        auto runId = RequiredString(request, L"phase_run_id");
        RequireActiveStoragePhase(runtime, runId);
        auto workerPath = StoragePath(StorageFileName(runId, L"foreground"));
        auto brokerPath = StoragePath(StorageFileName(runId, L"broker"));
        auto absent = !fs::exists(workerPath) && !fs::exists(brokerPath);
        if (!absent)
        {
            FailBrokerRequest(L"STORAGE_IO_FAILED", L"temporary storage files remain after bidirectional verification");
        }
        JsonObject result;
        PutBrokerBoolean(result, L"temporary_files_absent", true);
        PutBrokerBoolean(result, L"idempotent_success", true);
        return result;
    }
}