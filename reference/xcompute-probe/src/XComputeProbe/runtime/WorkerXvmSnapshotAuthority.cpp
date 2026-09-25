#include "pch.h"
#include "WorkerXvmSnapshotAuthority.h"

#include "WorkerXvmTypes.h"

using namespace winrt;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage::Streams;

namespace XComputeProbe
{
    namespace
    {
        constexpr size_t SnapshotAuthorityKeyBytes = 32;
        constexpr wchar_t const* SnapshotAuthorityKeyFile = L"xvm-snapshot-hmac-v1.key";
        constexpr wchar_t const* SnapshotSealSchemaVersionValue = L"xvm-state-snapshot-hmac-sha256-v1";
        std::mutex SnapshotAuthorityMutex;

        std::vector<uint8_t> ReadAuthorityKey(std::filesystem::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority key could not be opened");
            }
            std::vector<uint8_t> key((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (key.size() != SnapshotAuthorityKeyBytes)
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority key has an invalid size");
            }
            return key;
        }

        std::vector<uint8_t> CreateAuthorityKey(std::filesystem::path const& path)
        {
            auto buffer = CryptographicBuffer::GenerateRandom(static_cast<uint32_t>(SnapshotAuthorityKeyBytes));
            winrt::com_array<uint8_t> generated;
            CryptographicBuffer::CopyToByteArray(buffer, generated);
            std::vector<uint8_t> key(generated.begin(), generated.end());
            if (key.size() != SnapshotAuthorityKeyBytes)
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority random key generation failed");
            }

            std::filesystem::create_directories(path.parent_path());
            auto temporary = path;
            temporary += L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority key could not be persisted");
                }
                output.write(reinterpret_cast<char const*>(key.data()), static_cast<std::streamsize>(key.size()));
                output.flush();
                if (!output.good())
                {
                    throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority key persistence did not complete");
                }
            }
            std::filesystem::rename(temporary, path);
            return key;
        }

        std::wstring LowerHex(IBuffer const& buffer)
        {
            auto value = std::wstring(CryptographicBuffer::EncodeToHexString(buffer).c_str());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }

        WorkerXvmSnapshotAuthority BuildAuthority(std::vector<uint8_t> key)
        {
            auto keyBuffer = CryptographicBuffer::CreateFromByteArray(key);
            auto hashProvider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());

            WorkerXvmSnapshotAuthority authority;
            authority.keyId = LowerHex(hashProvider.HashData(keyBuffer));
            authority.sealCanonical = [key = std::move(key)](std::wstring const& canonicalInput)
            {
                auto provider = MacAlgorithmProvider::OpenAlgorithm(MacAlgorithmNames::HmacSha256());
                auto keyBufferLocal = CryptographicBuffer::CreateFromByteArray(key);
                auto cryptographicKey = provider.CreateKey(keyBufferLocal);
                auto message = CryptographicBuffer::ConvertStringToBinary(
                    hstring(canonicalInput),
                    BinaryStringEncoding::Utf8);
                return LowerHex(CryptographicEngine::Sign(cryptographicKey, message));
            };
            return authority;
        }
    }

    wchar_t const* WorkerXvmSnapshotSealSchemaVersion()
    {
        return SnapshotSealSchemaVersionValue;
    }

    WorkerXvmSnapshotAuthority WorkerLoadXvmSnapshotAuthority(
        std::filesystem::path const& privateAuthorityRoot)
    {
        std::lock_guard<std::mutex> lock(SnapshotAuthorityMutex);
        auto keyPath = privateAuthorityRoot / SnapshotAuthorityKeyFile;
        if (!std::filesystem::exists(keyPath))
        {
            throw WorkerXvmError("xvm.snapshot_authority_missing", "worker snapshot authority key does not exist");
        }
        return BuildAuthority(ReadAuthorityKey(keyPath));
    }

    WorkerXvmSnapshotAuthority WorkerLoadOrCreateXvmSnapshotAuthority(
        std::filesystem::path const& privateAuthorityRoot)
    {
        std::lock_guard<std::mutex> lock(SnapshotAuthorityMutex);
        auto keyPath = privateAuthorityRoot / SnapshotAuthorityKeyFile;
        auto key = std::filesystem::exists(keyPath)
            ? ReadAuthorityKey(keyPath)
            : CreateAuthorityKey(keyPath);
        return BuildAuthority(std::move(key));
    }
}
