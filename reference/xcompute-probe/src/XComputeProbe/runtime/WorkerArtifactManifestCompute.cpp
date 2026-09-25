#include "pch.h"
#include "WorkerArtifactManifestCompute.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
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

        struct ArtifactManifestKernelBinding
        {
            std::wstring kernelId;
            std::wstring computeKind;
            uint64_t tileBytes = 0;
        };

        std::string WideToUtf8Local(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring Utf8ToWideLocal(std::string const& value)
        {
            return winrt::to_hstring(value).c_str();
        }

        std::wstring DoubleJsonLocal(double value, int precision = 3)
        {
            if (!std::isfinite(value))
            {
                return L"null";
            }
            std::wostringstream out;
            out << std::fixed << std::setprecision(precision) << value;
            return out.str();
        }

        std::wstring BoolJsonLocal(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring UInt64HexLocal(uint64_t value)
        {
            std::wostringstream out;
            out << std::hex << std::setfill(L'0') << std::setw(16) << value;
            return out.str();
        }

        uint64_t RotateLeft64Local(uint64_t value, unsigned int bits)
        {
            return (value << bits) | (value >> (64 - bits));
        }

        uint64_t StableWideStringMix64Local(std::wstring const& value, uint64_t seed)
        {
            uint64_t hash = seed;
            for (auto ch : value)
            {
                hash ^= static_cast<uint64_t>(ch);
                hash *= 1099511628211ull;
                hash = RotateLeft64Local(hash, 7);
            }
            return hash;
        }

        std::wstring ToLowerAscii(std::wstring value)
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

        bool IsSha256Hex(std::wstring const& value)
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

        std::wstring NormalizeOptionalSha256(std::wstring const& value)
        {
            if (value.empty())
            {
                return L"";
            }
            if (!IsSha256Hex(value))
            {
                throw WorkerArtifactManifestComputeError("artifact.sha256_invalid", "expected_sha256 must be 64 hex characters");
            }
            return ToLowerAscii(value);
        }

        bool IsSafeArtifactId(std::wstring const& value)
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

        std::wstring NormalizeArtifactId(std::wstring const& value)
        {
            if (value.empty())
            {
                throw WorkerArtifactManifestComputeError("artifact_id.required", "artifact_id is required");
            }
            if (!IsSafeArtifactId(value))
            {
                throw WorkerArtifactManifestComputeError("artifact_id.invalid", "artifact_id must be 1..64 chars using letters, digits, underscore, or dash");
            }
            return value;
        }

        std::wstring OptionalString(JsonObject const& object, wchar_t const* name, std::wstring const& fallback = L"")
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedValue(name);
            if (value.ValueType() != JsonValueType::String)
            {
                return fallback;
            }
            return std::wstring(value.GetString().c_str());
        }

        uint64_t OptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedNumber(name, static_cast<double>(fallback));
            if (value < 0)
            {
                throw WorkerArtifactManifestComputeError("argument.invalid_number", "numeric argument cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }

        uint64_t BoundedOptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback, uint64_t minimum, uint64_t maximum)
        {
            auto value = OptionalUInt64(object, name, fallback);
            if (value < minimum || value > maximum)
            {
                throw WorkerArtifactManifestComputeError("argument.out_of_range", "numeric argument is outside the supported range");
            }
            return value;
        }

        std::wstring UInt64ArrayJson(std::array<uint64_t, 16> const& values)
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

        ArtifactManifestKernelBinding ResolveArtifactManifestKernelBinding(
            JsonObject const& request,
            uint64_t readBufferBytes)
        {
            ArtifactManifestKernelBinding binding;
            binding.kernelId = OptionalString(request, L"kernel_id", L"stream_mix_v1");
            auto requestedComputeMode = OptionalString(request, L"compute_mode");

            if (binding.kernelId == L"hash_reduce_v1")
            {
                binding.computeKind = L"logical_sha256_reduce_v1";
            }
            else if (binding.kernelId == L"stream_mix_v1")
            {
                binding.computeKind = L"stream_byte_mix_v1";
            }
            else if (binding.kernelId == L"tiled_scan_u8_v1")
            {
                binding.computeKind = L"tiled_scan_u8_v1";
                binding.tileBytes = BoundedOptionalUInt64(
                    request,
                    L"tile_bytes",
                    readBufferBytes,
                    4096,
                    readBufferBytes * 64ull);
            }
            else
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.kernel_id_invalid", "kernel_id is not allowlisted for artifact manifest compute");
            }

            if (!requestedComputeMode.empty() && requestedComputeMode != binding.computeKind)
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.compute_mode_mismatch", "compute_mode does not match the allowlisted kernel_id");
            }

            return binding;
        }
    }

    WorkerArtifactManifestComputeError::WorkerArtifactManifestComputeError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue))
    {
    }

    char const* WorkerArtifactManifestComputeError::what() const noexcept
    {
        return message.c_str();
    }

    std::wstring WorkerRunArtifactManifestComputeJob(
        WorkerArtifactManifestComputeInput const& input,
        WorkerArtifactManifestResolver const& resolver,
        WorkerArtifactManifestTextReader const& textReader,
        WorkerArtifactManifestPublisher const& publisher,
        WorkerArtifactManifestIdGenerator const& idGenerator)
    {
        auto request = input.request;
        auto kernelBinding = ResolveArtifactManifestKernelBinding(request, input.readBufferBytes);
        auto manifestArtifactId = NormalizeArtifactId(OptionalString(request, L"manifest_artifact_id"));
        auto manifestTarget = resolver(manifestArtifactId);
        if (!manifestTarget.committed)
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.manifest_not_committed", "manifest artifact must be committed");
        }
        if (manifestTarget.bytes == 0 || manifestTarget.bytes > input.maxArtifactManifestJsonBytes)
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.manifest_size_invalid", "manifest artifact JSON size is outside the supported range");
        }

        auto manifestUtf8 = textReader(manifestTarget.path);
        auto manifest = JsonObject::Parse(Utf8ToWideLocal(manifestUtf8));
        auto manifestSchema = OptionalString(manifest, L"schema_version");
        if (manifestSchema != L"worker-multi-artifact-manifest-v1")
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.schema_invalid", "manifest artifact must use worker-multi-artifact-manifest-v1");
        }
        if (!manifest.HasKey(L"parts"))
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.parts_missing", "manifest artifact must contain parts[]");
        }

        auto parts = manifest.GetNamedArray(L"parts");
        if (parts.Size() == 0 || parts.Size() > input.maxArtifactManifestParts)
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_count_invalid", "manifest part count is outside the supported range");
        }

        auto declaredPartCount = OptionalUInt64(manifest, L"part_count", parts.Size());
        auto declaredLogicalBytes = OptionalUInt64(manifest, L"logical_payload_bytes", 0);
        auto expectedLogicalSha256 = NormalizeOptionalSha256(OptionalString(request, L"expected_logical_sha256", OptionalString(manifest, L"logical_upload_sha256")));
        auto graphBoundInputSha256 = NormalizeOptionalSha256(OptionalString(request, L"graph_bound_input_sha256"));
        auto graphBoundInputMix64 = OptionalString(request, L"graph_bound_input_mix64");
        if (!graphBoundInputMix64.empty() && graphBoundInputMix64.size() != 16)
        {
            throw WorkerArtifactManifestComputeError("artifact_manifest_compute.bound_input_mix_invalid", "graph_bound_input_mix64 must be a 16-character hex string");
        }

        auto started = std::chrono::steady_clock::now();
        Sha256 logicalSha;
        uint64_t logicalBytesRead = 0;
        uint64_t verifiedPartCount = 0;
        uint64_t readOperationCount = 0;
        uint64_t byteIterations = 0;
        uint64_t byteSum64 = 0;
        uint64_t byteXor64 = 0;
        uint64_t zeroCount = 0;
        uint64_t nonzeroCount = 0;
        uint64_t evenByteSum64 = 0;
        uint64_t oddByteSum64 = 0;
        uint64_t computeMix64 = 0x6a09e667f3bcc909ull;
        if (kernelBinding.computeKind == L"logical_sha256_reduce_v1")
        {
            computeMix64 = 0x243f6a8885a308d3ull;
        }
        else if (kernelBinding.computeKind == L"tiled_scan_u8_v1")
        {
            computeMix64 = 0x13198a2e03707344ull;
        }
        std::array<uint64_t, 16> laneSums{};
        bool verified = declaredPartCount == parts.Size();
        std::vector<std::wstring> partResults;
        std::vector<uint8_t> buffer(static_cast<size_t>(input.readBufferBytes));

        for (uint32_t i = 0; i < parts.Size(); ++i)
        {
            auto part = parts.GetObjectAt(i);
            auto partIndex = OptionalUInt64(part, L"index", i);
            auto partArtifactId = NormalizeArtifactId(OptionalString(part, L"artifact_id"));
            auto expectedPartSha256 = NormalizeOptionalSha256(OptionalString(part, L"sha256"));
            auto expectedPartBytes = OptionalUInt64(part, L"part_bytes", 0);
            if (expectedPartSha256.empty())
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_sha256_missing", "manifest part is missing sha256");
            }
            if (expectedPartBytes == 0 || expectedPartBytes > input.maxArtifactExpectedBytes)
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_bytes_invalid", "manifest part_bytes is outside the supported range");
            }

            auto partTarget = resolver(partArtifactId);
            if (!partTarget.committed)
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_not_committed", "manifest part artifact must be committed");
            }

            std::ifstream stream(partTarget.path, std::ios::binary);
            if (!stream)
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_read_failed", "could not open manifest part artifact");
            }

            Sha256 partSha;
            uint64_t partBytesRead = 0;
            uint64_t partReadOperations = 0;
            uint64_t partMix64 = 0xcbf29ce484222325ull ^ static_cast<uint64_t>(i);
            while (stream)
            {
                stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                auto read = stream.gcount();
                if (read <= 0)
                {
                    break;
                }

                auto readSize = static_cast<size_t>(read);
                auto baseOffset = logicalBytesRead;
                partSha.Update(buffer.data(), readSize);
                logicalSha.Update(buffer.data(), readSize);
                partBytesRead += static_cast<uint64_t>(readSize);
                logicalBytesRead += static_cast<uint64_t>(readSize);
                partReadOperations += 1;
                readOperationCount += 1;

                for (size_t j = 0; j < readSize; ++j)
                {
                    auto value = static_cast<uint64_t>(buffer[j]);
                    auto logicalIndex = baseOffset + static_cast<uint64_t>(j);
                    byteIterations += 1;
                    byteSum64 += value;
                    byteXor64 ^= (value << ((logicalIndex & 7) * 8));
                    if (value == 0)
                    {
                        zeroCount += 1;
                    }
                    else
                    {
                        nonzeroCount += 1;
                    }
                    if ((logicalIndex & 1) == 0)
                    {
                        evenByteSum64 += value;
                    }
                    else
                    {
                        oddByteSum64 += value;
                    }

                    auto lane = static_cast<size_t>(logicalIndex & 15);
                    laneSums[lane] += value;
                    auto mixed = value | ((logicalIndex & 0xffffull) << 8);
                    if (kernelBinding.computeKind == L"logical_sha256_reduce_v1")
                    {
                        auto hashMixed = value + ((logicalIndex & 0xffull) << 16) + (static_cast<uint64_t>(i) << 48);
                        computeMix64 ^= hashMixed + 0x100000001b3ull + (computeMix64 << 7) + (computeMix64 >> 3);
                        computeMix64 = RotateLeft64Local(computeMix64, 5) * 0x9e3779b97f4a7c15ull + 0x243f6a8885a308d3ull;
                    }
                    else if (kernelBinding.computeKind == L"tiled_scan_u8_v1")
                    {
                        auto tileBytes = kernelBinding.tileBytes == 0 ? input.readBufferBytes : kernelBinding.tileBytes;
                        auto tileIndex = logicalIndex / tileBytes;
                        auto tileOffset = logicalIndex % tileBytes;
                        auto tileMixed = (value << ((tileOffset & 7ull) * 8)) ^ (tileIndex * 0x9e3779b97f4a7c15ull) ^ (tileOffset << 1);
                        computeMix64 += tileMixed + 0x94d049bb133111ebull + (computeMix64 << 3);
                        computeMix64 = RotateLeft64Local(computeMix64, 17) ^ 0xbf58476d1ce4e5b9ull;
                    }
                    else
                    {
                        computeMix64 ^= mixed + 0x9e3779b97f4a7c15ull + (computeMix64 << 6) + (computeMix64 >> 2);
                        computeMix64 = RotateLeft64Local(computeMix64, 13) * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
                    }
                    partMix64 ^= mixed + 0x9e3779b97f4a7c15ull + (partMix64 << 6) + (partMix64 >> 2);
                    partMix64 = RotateLeft64Local(partMix64, 11) * 0x100000001b3ull;
                }
            }
            if (stream.bad())
            {
                throw WorkerArtifactManifestComputeError("artifact_manifest_compute.part_read_failed", "could not read manifest part artifact");
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
                L",\"compute_mix64\":" + JsonString(UInt64HexLocal(partMix64)) +
                L",\"verified\":" + BoolJsonLocal(partVerified) +
                L"}");
        }

        auto logicalSha256 = logicalSha.FinalHex();
        if (!graphBoundInputSha256.empty())
        {
            computeMix64 ^= StableWideStringMix64Local(graphBoundInputSha256, 0x6d2b79f5aa2f8d61ull);
            computeMix64 = RotateLeft64Local(computeMix64, 23) * 0x9e3779b97f4a7c15ull + 0x94d049bb133111ebull;
        }
        if (!graphBoundInputMix64.empty())
        {
            computeMix64 ^= StableWideStringMix64Local(graphBoundInputMix64, 0x510e527fade682d1ull);
            computeMix64 = RotateLeft64Local(computeMix64, 29) * 0xbf58476d1ce4e5b9ull + 0x100000001b3ull;
        }
        auto expectedLogicalKnown = !expectedLogicalSha256.empty();
        if (declaredLogicalBytes != 0 && declaredLogicalBytes != logicalBytesRead)
        {
            verified = false;
        }
        if (expectedLogicalKnown && expectedLogicalSha256 != logicalSha256)
        {
            verified = false;
        }
        auto tileCount = (kernelBinding.tileBytes == 0 || logicalBytesRead == 0)
            ? 0
            : ((logicalBytesRead + kernelBinding.tileBytes - 1) / kernelBinding.tileBytes);

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

        auto elapsedMs = ElapsedMs(started);
        auto verdict = verified ? L"passed" : L"failed";
        auto computeMixHex = UInt64HexLocal(computeMix64);
        auto laneSumsJson = UInt64ArrayJson(laneSums);
        auto resultPayload =
            L"{\"schema_version\":" + JsonString(input.artifactManifestComputeJobSchemaVersion) +
            L",\"protocol_version\":" + JsonString(input.protocolVersion) +
            L",\"kernel_id\":" + JsonString(kernelBinding.kernelId) +
            L",\"job_kind\":\"artifact_manifest_streaming_compute\"" +
            L",\"compute_kind\":" + JsonString(kernelBinding.computeKind) +
            L",\"manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"manifest_schema\":" + JsonString(manifestSchema) +
            L",\"manifest_sha256\":" + JsonString(manifestTarget.sha256) +
            L",\"manifest_bytes\":" + std::to_wstring(manifestTarget.bytes) +
            L",\"declared_part_count\":" + std::to_wstring(declaredPartCount) +
            L",\"part_count\":" + std::to_wstring(parts.Size()) +
            L",\"verified_part_count\":" + std::to_wstring(verifiedPartCount) +
            L",\"declared_logical_bytes\":" + std::to_wstring(declaredLogicalBytes) +
            L",\"logical_bytes_read\":" + std::to_wstring(logicalBytesRead) +
            L",\"expected_logical_sha256_known\":" + BoolJsonLocal(expectedLogicalKnown) +
            L",\"expected_logical_sha256\":" + JsonString(expectedLogicalSha256) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"read_buffer_bytes\":" + std::to_wstring(input.readBufferBytes) +
            L",\"tile_bytes\":" + std::to_wstring(kernelBinding.tileBytes) +
            L",\"tile_count\":" + std::to_wstring(tileCount) +
            L",\"read_operation_count\":" + std::to_wstring(readOperationCount) +
            L",\"byte_iterations\":" + std::to_wstring(byteIterations) +
            L",\"byte_sum64\":" + std::to_wstring(byteSum64) +
            L",\"byte_xor64\":" + JsonString(UInt64HexLocal(byteXor64)) +
            L",\"zero_count\":" + std::to_wstring(zeroCount) +
            L",\"nonzero_count\":" + std::to_wstring(nonzeroCount) +
            L",\"even_byte_sum64\":" + std::to_wstring(evenByteSum64) +
            L",\"odd_byte_sum64\":" + std::to_wstring(oddByteSum64) +
            L",\"lane_sums64\":" + laneSumsJson +
            L",\"compute_mix64\":" + JsonString(computeMixHex) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"verified\":" + BoolJsonLocal(verified) +
            L",\"elapsed_ms\":" + DoubleJsonLocal(static_cast<double>(elapsedMs), 6) +
            L",\"part_results\":" + partResultsJson +
            L"}";

        auto suffix = idGenerator();
        if (suffix.size() > 16)
        {
            suffix.resize(16);
        }
        auto defaultArtifactId = L"manifest-compute-result-" + suffix;
        auto manifestFields =
            L",\"job_output_schema\":" + JsonString(input.artifactManifestComputeJobSchemaVersion) +
            L",\"kernel_id\":" + JsonString(kernelBinding.kernelId) +
            L",\"compute_kind\":" + JsonString(kernelBinding.computeKind) +
            L",\"source_manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"graph_bound_input_sha256\":" + JsonString(graphBoundInputSha256) +
            L",\"graph_bound_input_mix64\":" + JsonString(graphBoundInputMix64) +
            L",\"graph_bound_input_applied\":" + BoolJsonLocal(!graphBoundInputSha256.empty()) +
            L",\"compute_mix64\":" + JsonString(computeMixHex) +
            L",\"verified\":" + BoolJsonLocal(verified);

        auto published = publisher(
            request,
            defaultArtifactId,
            L"artifact-manifest-compute-result",
            WideToUtf8Local(resultPayload),
            manifestFields);

        return L"{\"ok\":true,\"command\":\"run_artifact_manifest_compute_job\"" +
            std::wstring(L",\"protocol_version\":") + JsonString(input.protocolVersion) +
            L",\"schema_version\":" + JsonString(input.artifactManifestComputeJobSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(input.artifactUploadSchemaVersion) +
            L",\"kernel_id\":" + JsonString(kernelBinding.kernelId) +
            L",\"compute_kind\":" + JsonString(kernelBinding.computeKind) +
            L",\"manifest_artifact_id\":" + JsonString(manifestArtifactId) +
            L",\"manifest_sha256\":" + JsonString(manifestTarget.sha256) +
            L",\"result_artifact_id\":" + JsonString(published.artifactId) +
            L",\"result_artifact_kind\":" + JsonString(published.artifactKind) +
            L",\"result_bytes\":" + std::to_wstring(published.bytes) +
            L",\"result_sha256\":" + JsonString(published.sha256) +
            L",\"result_blob_handle\":" + JsonString(published.blobHandle) +
            L",\"result_manifest_path\":" + JsonString(published.manifestPath) +
            L",\"result_deduplicated\":" + BoolJsonLocal(published.deduplicated) +
            L",\"declared_part_count\":" + std::to_wstring(declaredPartCount) +
            L",\"part_count\":" + std::to_wstring(parts.Size()) +
            L",\"verified_part_count\":" + std::to_wstring(verifiedPartCount) +
            L",\"declared_logical_bytes\":" + std::to_wstring(declaredLogicalBytes) +
            L",\"logical_bytes_read\":" + std::to_wstring(logicalBytesRead) +
            L",\"expected_logical_sha256\":" + JsonString(expectedLogicalSha256) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"graph_bound_input_sha256\":" + JsonString(graphBoundInputSha256) +
            L",\"graph_bound_input_mix64\":" + JsonString(graphBoundInputMix64) +
            L",\"graph_bound_input_applied\":" + BoolJsonLocal(!graphBoundInputSha256.empty()) +
            L",\"read_buffer_bytes\":" + std::to_wstring(input.readBufferBytes) +
            L",\"tile_bytes\":" + std::to_wstring(kernelBinding.tileBytes) +
            L",\"tile_count\":" + std::to_wstring(tileCount) +
            L",\"read_operation_count\":" + std::to_wstring(readOperationCount) +
            L",\"byte_iterations\":" + std::to_wstring(byteIterations) +
            L",\"byte_sum64\":" + std::to_wstring(byteSum64) +
            L",\"byte_xor64\":" + JsonString(UInt64HexLocal(byteXor64)) +
            L",\"zero_count\":" + std::to_wstring(zeroCount) +
            L",\"nonzero_count\":" + std::to_wstring(nonzeroCount) +
            L",\"even_byte_sum64\":" + std::to_wstring(evenByteSum64) +
            L",\"odd_byte_sum64\":" + std::to_wstring(oddByteSum64) +
            L",\"lane_sums64\":" + laneSumsJson +
            L",\"compute_mix64\":" + JsonString(computeMixHex) +
            L",\"verdict\":" + JsonString(verdict) +
            L",\"verified\":" + BoolJsonLocal(verified) +
            L",\"workspace_bytes_before\":" + std::to_wstring(published.workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(published.workspaceBytesAfter) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(published.workspaceBudgetBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(published.rollbackHeadroomBytes) +
            L",\"storage_available_known\":" + BoolJsonLocal(published.storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(published.storageAvailableBytes) +
            L",\"elapsed_ms\":" + DoubleJsonLocal(static_cast<double>(elapsedMs), 6) +
            L",\"publish_ms\":" + DoubleJsonLocal(published.publishMs, 6) +
            L"}";
    }
}
