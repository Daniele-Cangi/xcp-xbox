#include "pch.h"
#include "WorkerProcessTopologyStorageProbe.h"
#include "WorkerProcessTopologyTransport.h"
#include <winrt/Windows.Storage.h>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage;
namespace fs = std::filesystem;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {
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
        void RemoveKnownFile(fs::path const& path) noexcept
        {
            std::error_code error;
            fs::remove(path, error);
        }
        void WriteExact(fs::path const& path, std::vector<uint8_t> const& payload)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                Fail(L"topology.storage_probe_failed", L"the foreground could not create its bounded storage probe file");
            }
            output.write(reinterpret_cast<char const*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            output.flush();
            if (!output)
            {
                Fail(L"topology.storage_probe_failed", L"the foreground storage probe write was incomplete");
            }
        }
        std::vector<uint8_t> ReadExact(fs::path const& path, uint64_t expectedBytes)
        {
            std::error_code sizeError;
            auto size = fs::file_size(path, sizeError);
            if (sizeError || size != expectedBytes)
            {
                Fail(L"topology.storage_probe_failed", L"the broker-to-foreground storage probe size changed");
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                Fail(L"topology.storage_probe_failed", L"the foreground could not open the broker storage probe file");
            }
            std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
            if (bytes.size() != expectedBytes)
            {
                Fail(L"topology.storage_probe_failed", L"the broker-to-foreground storage probe read was incomplete");
            }
            return bytes;
        }
        JsonObject Transfer(uint64_t bytes, std::wstring const& payloadSha256, std::wstring const& writerSha256, std::wstring const& readerSha256)
        {
            JsonObject transfer;
            PutNumber(transfer, L"bytes", bytes);
            PutString(transfer, L"payload_sha256", payloadSha256);
            PutString(transfer, L"writer_sha256", writerSha256);
            PutString(transfer, L"reader_sha256", readerSha256);
            PutBool(transfer, L"writer_completed", true);
            PutBool(transfer, L"reader_verified", true);
            PutBool(transfer, L"exact_hash_match", payloadSha256 == writerSha256 && writerSha256 == readerSha256);
            return transfer;
        }
    }

    WorkerProcessTopologyStorageProbeResult WorkerProcessTopologyStorageProbe::Run(
        WorkerProcessTopologyTransport& transport,
        std::wstring const& phaseRunId,
        uint64_t payloadBytes,
        MonotonicDeadline const& deadline)
    {
        if (!IsSafeToken(phaseRunId, 64) || payloadBytes == 0 || payloadBytes > MaximumStorageProbeBytes)
        {
            Fail(L"topology.storage_probe_failed", L"the storage probe binding or payload is outside Gate T2");
        }
        auto workerPath = StoragePath(StorageFileName(phaseRunId, L"foreground"));
        auto brokerPath = StoragePath(StorageFileName(phaseRunId, L"broker"));
        RemoveKnownFile(workerPath);
        RemoveKnownFile(brokerPath);
        try
        {
            std::vector<uint8_t> payload(static_cast<size_t>(payloadBytes), static_cast<uint8_t>('x'));
            auto payloadSha256 = Sha256(payload);
            WriteExact(workerPath, payload);
            auto exchangeRequest = NewBrokerCommand(L"storage_exchange");
            PutString(exchangeRequest, L"phase_run_id", phaseRunId);
            PutNumber(exchangeRequest, L"bytes", payloadBytes);
            PutString(exchangeRequest, L"payload_sha256", payloadSha256);
            auto exchangeReply = SendBrokerCommand(transport, exchangeRequest, deadline.Remaining());
            auto workerReaderSha256 = RequiredString(exchangeReply.result, L"worker_reader_sha256", L"topology.storage_probe_failed");
            auto brokerWriterSha256 = RequiredString(exchangeReply.result, L"broker_writer_sha256", L"topology.storage_probe_failed");
            auto workerFileDeleted = RequiredBool(exchangeReply.result, L"worker_file_deleted", L"topology.storage_probe_failed");
            auto brokerFileCreated = RequiredBool(exchangeReply.result, L"broker_file_created", L"topology.storage_probe_failed");
            if (!workerFileDeleted || !brokerFileCreated || workerReaderSha256 != payloadSha256 || brokerWriterSha256 != payloadSha256)
            {
                Fail(L"topology.storage_probe_failed", L"the broker did not verify and reverse the bounded storage exchange");
            }
            auto brokerPayload = ReadExact(brokerPath, payloadBytes);
            auto brokerReaderSha256 = Sha256(brokerPayload);
            if (brokerReaderSha256 != payloadSha256)
            {
                Fail(L"topology.storage_probe_failed", L"the foreground readback differs from the broker writer hash");
            }
            RemoveKnownFile(brokerPath);
            auto cleanupRequest = NewBrokerCommand(L"storage_cleanup_status");
            PutString(cleanupRequest, L"phase_run_id", phaseRunId);
            auto cleanupReply = SendBrokerCommand(transport, cleanupRequest, deadline.Remaining());
            auto absent = RequiredBool(cleanupReply.result, L"temporary_files_absent", L"topology.storage_probe_failed");
            if (!absent || fs::exists(workerPath) || fs::exists(brokerPath))
            {
                Fail(L"topology.storage_probe_failed", L"the storage probe cleanup was not idempotently verified");
            }
            JsonObject measurement;
            PutString(measurement, L"path_class", L"app_private_shared");
            PutBool(measurement, L"probe_attempted", true);
            PutString(measurement, L"boundary_outcome", L"BIDIRECTIONAL_VERIFIED");
            measurement.SetNamedValue(L"worker_to_broker", Transfer(payloadBytes, payloadSha256, payloadSha256, workerReaderSha256));
            measurement.SetNamedValue(L"broker_to_worker", Transfer(payloadBytes, payloadSha256, brokerWriterSha256, brokerReaderSha256));
            PutBool(measurement, L"temporary_files_absent_after_cleanup", true);
            WorkerProcessTopologyStorageProbeResult result;
            result.measurement = measurement;
            result.brokerReplies.push_back(exchangeReply);
            result.brokerReplies.push_back(cleanupReply);
            return result;
        }
        catch (...)
        {
            RemoveKnownFile(workerPath);
            RemoveKnownFile(brokerPath);
            throw;
        }
    }
}