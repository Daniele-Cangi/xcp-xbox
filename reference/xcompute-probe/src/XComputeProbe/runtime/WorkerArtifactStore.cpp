#include "pch.h"
#include "WorkerArtifactStore.h"

#include <set>

#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    WorkerArtifactStoreError::WorkerArtifactStoreError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)), message(std::move(messageValue))
    {
    }

    char const* WorkerArtifactStoreError::what() const noexcept
    {
        return message.c_str();
    }

    namespace
    {
        std::wstring Utf8ToWideLocal(std::string const& value)
        {
            return winrt::to_hstring(value).c_str();
        }

        std::string WideToUtf8Local(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        double ElapsedMillisecondsLocal(std::chrono::steady_clock::time_point const& started)
        {
            auto elapsed = std::chrono::steady_clock::now() - started;
            return std::chrono::duration<double, std::milli>(elapsed).count();
        }

        std::wstring DoubleJsonLocal(double value, int precision = 3)
        {
            std::wostringstream out;
            out.imbue(std::locale::classic());
            out << std::fixed << std::setprecision(precision) << value;
            return out.str();
        }

        std::wstring BoolJsonLocal(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring OkBaseLocal(std::wstring const& command, WorkerArtifactStoreConfig const& config)
        {
            return L"{\"ok\":true,\"protocol_version\":" + JsonString(config.protocolVersion) + L",\"command\":" + JsonString(command);
        }

        std::wstring JsonStringArrayLocal(std::vector<std::wstring> const& values)
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

        std::string ReadTextFileLocal(fs::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerArtifactStoreError("file.read_failed", "could not open file for read");
            }
            return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        }

        void WriteInternalTextFileLocal(fs::path const& path, std::string const& content)
        {
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw WorkerArtifactStoreError("file.write_failed", "could not open file for write");
            }
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.flush();
            if (!output.good())
            {
                throw WorkerArtifactStoreError("file.write_failed", "file write did not complete");
            }
        }

        void WriteBinaryFileLocal(fs::path const& path, std::vector<uint8_t> const& bytes)
        {
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw WorkerArtifactStoreError("file.write_failed", "could not open binary file for write");
            }
            if (!bytes.empty())
            {
                output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            output.flush();
            if (!output.good())
            {
                throw WorkerArtifactStoreError("file.write_failed", "binary file write did not complete");
            }
        }

        bool IsSafeArtifactName(std::wstring const& value)
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

        std::wstring ToLowerAsciiLocal(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    return static_cast<wchar_t>(ch - L'A' + L'a');
                }
                return ch;
            });
            return value;
        }

        bool IsSha256HexLocal(std::wstring const& value)
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

        std::wstring NormalizeOptionalSha256Local(std::wstring const& value)
        {
            if (value.empty())
            {
                return L"";
            }
            if (!IsSha256HexLocal(value))
            {
                throw WorkerArtifactStoreError("artifact.sha256_invalid", "expected_sha256 must be 64 hex characters");
            }
            return ToLowerAsciiLocal(value);
        }

        std::wstring GetOptionalStringLocal(JsonObject const& object, wchar_t const* name, std::wstring const& fallback = L"")
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedString(name, fallback);
            return std::wstring(value.data(), value.size());
        }

        uint64_t GetOptionalUInt64Local(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }

            auto value = object.GetNamedNumber(name);
            if (value < 0)
            {
                throw WorkerArtifactStoreError("argument.invalid_number", "numeric argument cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }

        uint64_t GetBoundedOptionalUInt64Local(JsonObject const& object, wchar_t const* name, uint64_t fallback, uint64_t minimum, uint64_t maximum)
        {
            auto value = GetOptionalUInt64Local(object, name, fallback);
            if (value < minimum || value > maximum)
            {
                throw WorkerArtifactStoreError("argument.out_of_range", "numeric argument is outside the supported range");
            }
            return value;
        }

        double GetOptionalDoubleNoThrowLocal(JsonObject const& object, wchar_t const* name, double fallback = 0.0)
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

        uint64_t GetOptionalUInt64NoThrowLocal(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            auto value = GetOptionalDoubleNoThrowLocal(object, name, static_cast<double>(fallback));
            if (value < 0)
            {
                return fallback;
            }
            return static_cast<uint64_t>(value);
        }

        JsonObject ReadJsonObjectFileLocal(fs::path const& path, std::string const& code)
        {
            try
            {
                return JsonObject::Parse(Utf8ToWideLocal(ReadTextFileLocal(path)));
            }
            catch (WorkerArtifactStoreError const&)
            {
                throw;
            }
            catch (...)
            {
                throw WorkerArtifactStoreError(code, "could not parse stored JSON metadata");
            }
        }

        std::wstring NormalizeArtifactIdLocal(std::wstring const& value, bool required, WorkerArtifactIdGenerator const& idGenerator)
        {
            auto id = value;
            if (id.empty())
            {
                if (required)
                {
                    throw WorkerArtifactStoreError("artifact_id.required", "artifact_id is required");
                }
                auto suffix = idGenerator ? idGenerator() : L"";
                if (suffix.size() > 16)
                {
                    suffix.resize(16);
                }
                id = L"artifact-" + suffix;
            }
            if (!IsSafeArtifactName(id))
            {
                throw WorkerArtifactStoreError("artifact_id.invalid", "artifact_id must be 1..64 chars using letters, digits, underscore, or dash");
            }
            return id;
        }

        uint64_t SaturatingAddLocal(uint64_t left, uint64_t right)
        {
            if (UINT64_MAX - left < right)
            {
                return UINT64_MAX;
            }
            return left + right;
        }

        uint64_t WorkspaceBytesLocal(fs::path const& root)
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

        uint64_t WorkspaceFileCountLocal(fs::path const& root)
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

        uint64_t WorkspaceDirectoryCountLocal(fs::path const& root)
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

        uint64_t DirectoryBytesIfExistsLocal(fs::path const& path)
        {
            if (!fs::exists(path))
            {
                return 0;
            }
            return WorkspaceBytesLocal(path);
        }

        uint64_t FileBytesIfExistsLocal(fs::path const& path)
        {
            try
            {
                if (fs::exists(path) && fs::is_regular_file(path))
                {
                    return static_cast<uint64_t>(fs::file_size(path));
                }
            }
            catch (...)
            {
            }
            return 0;
        }

        uint64_t StorageAvailableBytesLocal(fs::path const& path, bool& known)
        {
            known = false;
            try
            {
                auto info = fs::space(path);
                known = true;
                auto available = info.available;
                if (available > static_cast<uintmax_t>(UINT64_MAX))
                {
                    return UINT64_MAX;
                }
                return static_cast<uint64_t>(available);
            }
            catch (...)
            {
                return 0;
            }
        }

        int Base64ValueLocal(char value)
        {
            if (value >= 'A' && value <= 'Z') return value - 'A';
            if (value >= 'a' && value <= 'z') return value - 'a' + 26;
            if (value >= '0' && value <= '9') return value - '0' + 52;
            if (value == '+') return 62;
            if (value == '/') return 63;
            return -1;
        }

        std::vector<uint8_t> Base64DecodeLocal(std::string const& encoded)
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

                auto value = Base64ValueLocal(c);
                if (value < 0)
                {
                    throw WorkerArtifactStoreError("base64.invalid", "data_base64 contains an invalid character");
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

        struct Sha256Local
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

        std::wstring Sha256FileLocal(fs::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerArtifactStoreError("file.read_failed", "could not open file for hash");
            }

            Sha256Local sha;
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

        void WriteBinaryChunkLocal(
            fs::path const& path,
            uint64_t offset,
            std::vector<uint8_t> const& data,
            bool truncate,
            size_t maxBytes,
            std::string const& tooLargeCode,
            std::string const& tooLargeMessage)
        {
            if (data.size() > maxBytes)
            {
                throw WorkerArtifactStoreError(tooLargeCode, tooLargeMessage);
            }

            fs::create_directories(path.parent_path());
            bool exists = fs::exists(path);
            uint64_t currentSize = exists ? static_cast<uint64_t>(fs::file_size(path)) : 0;
            if (!exists && offset != 0)
            {
                throw WorkerArtifactStoreError("chunk.offset_past_eof", "write_chunk cannot create sparse file");
            }
            if (offset > currentSize)
            {
                throw WorkerArtifactStoreError("chunk.offset_past_eof", "write_chunk offset is past EOF");
            }

            if (!exists)
            {
                std::ofstream create(path, std::ios::binary);
                if (!create)
                {
                    throw WorkerArtifactStoreError("file.write_failed", "could not create file for chunk write");
                }
            }

            std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
            if (!output)
            {
                throw WorkerArtifactStoreError("file.write_failed", "could not open file for chunk write");
            }
            output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!data.empty())
            {
                output.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            output.flush();
            if (!output.good())
            {
                throw WorkerArtifactStoreError("file.write_failed", "chunk write did not complete");
            }

            if (truncate)
            {
                fs::resize_file(path, offset + data.size());
            }
        }

        std::wstring SyncServerTimingJsonLocal(
            JsonObject const& request,
            double authMs,
            double decodeBase64Ms,
            double writeFileMs,
            double responseBuildMs,
            uint64_t payloadBytes,
            uint64_t decodedBytes)
        {
            auto requestBytes = GetOptionalUInt64NoThrowLocal(request, L"_server_request_bytes", 0);
            auto binaryReadMs = GetOptionalDoubleNoThrowLocal(request, L"_server_binary_read_ms", 0.0);
            auto parseJsonMs = GetOptionalDoubleNoThrowLocal(request, L"_server_parse_json_ms", 0.0);
            if (authMs <= 0.0)
            {
                authMs = GetOptionalDoubleNoThrowLocal(request, L"_server_auth_ms", 0.0);
            }

            return L",\"server_timing_schema\":\"worker-sync-command-timing-0.1\""
                L",\"server_timing_ms\":{"
                L"\"parse_json\":" + DoubleJsonLocal(parseJsonMs, 6) +
                L",\"auth\":" + DoubleJsonLocal(authMs, 6) +
                L",\"binary_body_read\":" + DoubleJsonLocal(binaryReadMs, 6) +
                L",\"decode_base64\":" + DoubleJsonLocal(decodeBase64Ms, 6) +
                L",\"write_file\":" + DoubleJsonLocal(writeFileMs, 6) +
                L",\"response_build\":" + DoubleJsonLocal(responseBuildMs, 6) +
                L"},\"server_timing_bytes\":{"
                L"\"request\":" + std::to_wstring(requestBytes) +
                L",\"payload\":" + std::to_wstring(payloadBytes) +
                L",\"decoded\":" + std::to_wstring(decodedBytes) +
                L"}";
        }

        void WriteArtifactMetadataLocal(
            fs::path const& path,
            std::wstring const& artifactId,
            std::wstring const& artifactKind,
            bool expectedBytesKnown,
            uint64_t expectedBytes,
            std::wstring const& expectedSha256,
            WorkerArtifactStoreConfig const& config)
        {
            auto metadata =
                L"{\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
                L",\"protocol_version\":" + JsonString(config.protocolVersion) +
                L",\"status\":\"staging\"" +
                L",\"artifact_id\":" + JsonString(artifactId) +
                L",\"artifact_kind\":" + JsonString(artifactKind) +
                L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
                L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
                L",\"expected_sha256\":" + JsonString(expectedSha256) +
                L"}";
            WriteInternalTextFileLocal(path, WideToUtf8Local(metadata));
        }

        std::wstring NormalizeDatasetPatternLocal(std::wstring const& value)
        {
            auto pattern = value.empty() ? L"xorshift32" : ToLowerAsciiLocal(value);
            if (pattern != L"xorshift32" && pattern != L"zero" && pattern != L"ramp")
            {
                throw WorkerArtifactStoreError("artifact.dataset_pattern_invalid", "dataset pattern must be xorshift32, zero, or ramp");
            }
            return pattern;
        }

        uint32_t NextXorShift32Local(uint32_t& state)
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

        std::wstring WriteDeterministicDatasetFileLocal(
            fs::path const& path,
            uint64_t byteCount,
            uint32_t seed,
            std::wstring const& pattern,
            double& writeMs)
        {
            auto started = std::chrono::steady_clock::now();
            fs::create_directories(path.parent_path());

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw WorkerArtifactStoreError("artifact.dataset_write_failed", "could not create generated dataset file");
            }

            Sha256Local sha;
            std::vector<uint8_t> buffer(1024 * 1024);
            uint64_t remaining = byteCount;
            uint64_t absoluteOffset = 0;
            uint32_t xorshift = seed;

            while (remaining > 0)
            {
                auto chunk = static_cast<size_t>((std::min<uint64_t>)(remaining, buffer.size()));
                if (pattern == L"zero")
                {
                    std::fill(buffer.begin(), buffer.begin() + chunk, uint8_t{ 0 });
                }
                else if (pattern == L"ramp")
                {
                    for (size_t i = 0; i < chunk; ++i)
                    {
                        buffer[i] = static_cast<uint8_t>((absoluteOffset + i + seed) & 0xFFu);
                    }
                }
                else
                {
                    for (size_t i = 0; i < chunk; ++i)
                    {
                        if ((i & 3u) == 0u)
                        {
                            xorshift = NextXorShift32Local(xorshift);
                        }
                        buffer[i] = static_cast<uint8_t>((xorshift >> ((i & 3u) * 8u)) & 0xFFu);
                    }
                }

                output.write(reinterpret_cast<char const*>(buffer.data()), static_cast<std::streamsize>(chunk));
                if (!output)
                {
                    throw WorkerArtifactStoreError("artifact.dataset_write_failed", "could not write generated dataset chunk");
                }
                sha.Update(buffer.data(), chunk);
                remaining -= static_cast<uint64_t>(chunk);
                absoluteOffset += static_cast<uint64_t>(chunk);
            }

            output.close();
            if (!output)
            {
                throw WorkerArtifactStoreError("artifact.dataset_write_failed", "could not close generated dataset file");
            }

            writeMs = ElapsedMillisecondsLocal(started);
            return sha.FinalHex();
        }

        template <typename TReferenceVisitor>
        bool VisitCreativeInstallBlobReferencesLocal(
            fs::path const& root,
            TReferenceVisitor const& visitor)
        {
            auto installRoot =
                root.parent_path() /
                L"xcp-creative-v1" /
                L"installs";
            if (!fs::exists(installRoot))
            {
                return true;
            }

            for (auto const& entry :
                 fs::directory_iterator(installRoot))
            {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != L".json")
                {
                    continue;
                }
                try
                {
                    auto record = ReadJsonObjectFileLocal(
                        entry.path(),
                        "creative.install_record_invalid");
                    if (GetOptionalStringLocal(
                            record,
                            L"schema_version") !=
                            L"xcp-creative-installed-bundle-v1" ||
                        GetOptionalStringLocal(
                            record,
                            L"install_id") !=
                            entry.path().stem().wstring() ||
                        !record.HasKey(L"bundle_sha256") ||
                        !record.HasKey(L"files") ||
                        record.GetNamedValue(
                            L"files").ValueType() !=
                            JsonValueType::Array ||
                        !record.HasKey(L"file_count") ||
                        !record.HasKey(L"cas_references") ||
                        record.GetNamedValue(
                            L"cas_references").ValueType() !=
                            JsonValueType::Array)
                    {
                        return false;
                    }
                    std::set<std::wstring> expectedReferences;
                    auto bundleSha256 =
                        NormalizeOptionalSha256Local(
                            GetOptionalStringLocal(
                                record,
                                L"bundle_sha256"));
                    if (bundleSha256.empty())
                    {
                        return false;
                    }
                    expectedReferences.insert(bundleSha256);
                    auto files = record.GetNamedArray(L"files");
                    if (files.Size() == 0 ||
                        GetOptionalUInt64Local(
                            record,
                            L"file_count",
                            0) != files.Size())
                    {
                        return false;
                    }
                    for (uint32_t index = 0;
                         index < files.Size();
                         ++index)
                    {
                        auto value = files.GetAt(index);
                        if (value.ValueType() !=
                            JsonValueType::Object)
                        {
                            return false;
                        }
                        auto fileSha256 =
                            NormalizeOptionalSha256Local(
                                GetOptionalStringLocal(
                                    value.GetObject(),
                                    L"sha256"));
                        if (fileSha256.empty())
                        {
                            return false;
                        }
                        expectedReferences.insert(
                            std::move(fileSha256));
                    }
                    auto references =
                        record.GetNamedArray(L"cas_references");
                    std::set<std::wstring> declaredReferences;
                    for (uint32_t index = 0;
                         index < references.Size();
                         ++index)
                    {
                        auto value = references.GetAt(index);
                        if (value.ValueType() !=
                            JsonValueType::String)
                        {
                            return false;
                        }
                        auto reference =
                            NormalizeOptionalSha256Local(
                                std::wstring(
                                    value.GetString().c_str()));
                        if (reference.empty() ||
                            !declaredReferences.insert(
                                std::move(reference)).second)
                        {
                            return false;
                        }
                    }
                    if (declaredReferences != expectedReferences)
                    {
                        return false;
                    }
                    for (auto const& reference :
                         declaredReferences)
                    {
                        visitor(reference);
                    }
                }
                catch (...)
                {
                    // Fail-safe: unreadable creative install metadata
                    // protects every candidate CAS blob.
                    return false;
                }
            }
            return true;
        }

        uint64_t CountArtifactBlobReferencesLocal(fs::path const& root, std::wstring const& sha256)
        {
            if (sha256.empty())
            {
                return 0;
            }

            uint64_t count = 0;
            auto manifestRoot = ArtifactRoot(root) / L"manifests";
            if (fs::exists(manifestRoot))
            {
                for (auto const& entry : fs::directory_iterator(manifestRoot))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != L".json")
                    {
                        continue;
                    }

                    try
                    {
                        auto manifest = ReadJsonObjectFileLocal(entry.path(), "artifact.manifest_invalid");
                        auto manifestSha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"sha256"));
                        if (manifestSha256 == sha256)
                        {
                            ++count;
                        }
                    }
                    catch (...)
                    {
                        // A corrupt public manifest may still reference the blob.
                        // Cleanup therefore protects it until metadata is repaired.
                        ++count;
                    }
                }
            }

            auto pageRoot =
                root.parent_path() /
                L"xcompute-storage-v1" /
                L"namespaces";
            if (fs::exists(pageRoot))
            {
                for (auto const& entry : fs::recursive_directory_iterator(pageRoot))
                {
                    if (!entry.is_regular_file() ||
                        entry.path().extension() != L".json" ||
                        entry.path().parent_path().filename() != L"pages")
                    {
                        continue;
                    }

                    try
                    {
                        auto record = ReadJsonObjectFileLocal(
                            entry.path(),
                            "storage.page_record_invalid");
                        auto contentSha256 = NormalizeOptionalSha256Local(
                            GetOptionalStringLocal(record, L"content_sha256"));
                        if (contentSha256 == sha256)
                        {
                            ++count;
                        }
                    }
                    catch (...)
                    {
                        // Fail-safe: unreadable storage metadata blocks CAS reaping.
                        ++count;
                    }
                }
            }

            auto creativeMetadataReadable =
                VisitCreativeInstallBlobReferencesLocal(
                    root,
                    [&](std::wstring const& installedSha256)
                    {
                        if (installedSha256 == sha256)
                        {
                            ++count;
                        }
                    });
            if (!creativeMetadataReadable)
            {
                // One conservative reference blocks deletion when any
                // installed creative metadata cannot be trusted.
                ++count;
            }

            return count;
        }
    }

    std::wstring NormalizeArtifactKind(std::wstring const& value)
    {
        auto kind = value.empty() ? L"generic" : value;
        if (!IsSafeArtifactName(kind))
        {
            throw WorkerArtifactStoreError("artifact_kind.invalid", "artifact_kind must be 1..64 chars using letters, digits, underscore, or dash");
        }
        return kind;
    }

    fs::path ArtifactRoot(fs::path const& root)
    {
        return root / L"artifacts";
    }

    fs::path ArtifactStagingDirectory(fs::path const& root, std::wstring const& artifactId)
    {
        return ArtifactRoot(root) / L"staging" / artifactId;
    }

    fs::path ArtifactManifestPath(fs::path const& root, std::wstring const& artifactId)
    {
        return ArtifactRoot(root) / L"manifests" / (artifactId + L".json");
    }

    fs::path ArtifactPayloadPath(fs::path const& root, std::wstring const& artifactId)
    {
        return ArtifactStagingDirectory(root, artifactId) / L"payload.bin";
    }

    fs::path ArtifactMetadataPath(fs::path const& root, std::wstring const& artifactId)
    {
        return ArtifactStagingDirectory(root, artifactId) / L"metadata.json";
    }

    fs::path ArtifactBlobPath(fs::path const& root, std::wstring const& sha256)
    {
        auto prefix = sha256.substr(0, 2);
        return ArtifactRoot(root) / L"blobs" / L"sha256" / prefix / (sha256 + L".bin");
    }

    std::wstring ArtifactHandleForBlob(fs::path const& root, fs::path const& blobPath)
    {
        return blobPath.lexically_relative(root).wstring();
    }

    std::wstring WorkerContentSha256(std::vector<uint8_t> const& bytes)
    {
        Sha256Local sha;
        if (!bytes.empty())
        {
            sha.Update(bytes.data(), bytes.size());
        }
        return sha.FinalHex();
    }

    std::wstring WorkerContentSha256(std::string const& bytes)
    {
        Sha256Local sha;
        if (!bytes.empty())
        {
            sha.Update(
                reinterpret_cast<uint8_t const*>(bytes.data()),
                bytes.size());
        }
        return sha.FinalHex();
    }

    std::wstring WorkerContentSha256File(fs::path const& path)
    {
        return Sha256FileLocal(path);
    }

    WorkerContentAddressedBlobCommit WorkerCommitContentAddressedBlob(
        fs::path const& root,
        std::wstring const& stagingToken,
        std::vector<uint8_t> const& bytes)
    {
        if (!IsSafeArtifactName(stagingToken))
        {
            throw WorkerArtifactStoreError(
                "content_addressed.staging_token_invalid",
                "content-addressed staging token is invalid");
        }

        WorkerContentAddressedBlobCommit result;
        result.sha256 = WorkerContentSha256(bytes);
        result.bytes = static_cast<uint64_t>(bytes.size());
        result.path = ArtifactBlobPath(root, result.sha256);
        result.deduplicated = fs::exists(result.path);
        if (result.deduplicated)
        {
            if (static_cast<uint64_t>(fs::file_size(result.path)) !=
                    result.bytes ||
                WorkerContentSha256File(result.path) != result.sha256)
            {
                throw WorkerArtifactStoreError(
                    "content_addressed.existing_blob_invalid",
                    "existing content-addressed blob failed length or hash verification");
            }
            return result;
        }

        auto stagingRoot =
            ArtifactRoot(root) /
            L"content-addressed-staging";
        auto tempPath = stagingRoot / (stagingToken + L".tmp");
        fs::create_directories(stagingRoot);
        if (fs::exists(tempPath))
        {
            fs::remove(tempPath);
        }
        WriteBinaryFileLocal(tempPath, bytes);
        if (WorkerContentSha256File(tempPath) != result.sha256)
        {
            fs::remove(tempPath);
            throw WorkerArtifactStoreError(
                "content_addressed.staged_blob_invalid",
                "staged content-addressed blob failed hash verification");
        }

        fs::create_directories(result.path.parent_path());
        std::error_code renameError;
        fs::rename(tempPath, result.path, renameError);
        if (renameError)
        {
            if (!fs::exists(result.path) ||
                static_cast<uint64_t>(fs::file_size(result.path)) !=
                    result.bytes ||
                WorkerContentSha256File(result.path) != result.sha256)
            {
                fs::remove(tempPath);
                throw WorkerArtifactStoreError(
                    "content_addressed.commit_failed",
                    "content-addressed blob commit failed");
            }
            result.deduplicated = true;
            fs::remove(tempPath);
        }
        return result;
    }

    uint64_t WorkerCountContentAddressedBlobReferences(
        fs::path const& root,
        std::wstring const& sha256)
    {
        auto normalized = NormalizeOptionalSha256Local(sha256);
        return CountArtifactBlobReferencesLocal(root, normalized);
    }

    bool WorkerRemoveUnreferencedContentAddressedBlob(
        fs::path const& root,
        std::wstring const& sha256)
    {
        auto normalized = NormalizeOptionalSha256Local(sha256);
        if (CountArtifactBlobReferencesLocal(root, normalized) != 0)
        {
            return false;
        }
        auto path = ArtifactBlobPath(root, normalized);
        if (!fs::exists(path))
        {
            return false;
        }
        auto removed = fs::remove(path);
        if (removed)
        {
            std::error_code error;
            fs::remove(path.parent_path(), error);
        }
        return removed;
    }

    WorkerContentAddressedBlobCleanup
    WorkerRemoveUnreferencedContentAddressedBlobs(
        fs::path const& root,
        std::vector<std::wstring> const& sha256)
    {
        std::map<std::wstring, uint64_t> references;
        for (auto const& candidate : sha256)
        {
            references.emplace(
                NormalizeOptionalSha256Local(candidate),
                0);
        }

        WorkerContentAddressedBlobCleanup result;
        result.candidateCount =
            static_cast<uint64_t>(references.size());
        if (references.empty())
        {
            return result;
        }

        auto protectAll = false;
        auto manifestRoot = ArtifactRoot(root) / L"manifests";
        if (fs::exists(manifestRoot))
        {
            for (auto const& entry :
                 fs::directory_iterator(manifestRoot))
            {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != L".json")
                {
                    continue;
                }
                try
                {
                    auto manifest = ReadJsonObjectFileLocal(
                        entry.path(),
                        "artifact.manifest_invalid");
                    auto manifestSha256 =
                        NormalizeOptionalSha256Local(
                            GetOptionalStringLocal(
                                manifest,
                                L"sha256"));
                    auto found = references.find(manifestSha256);
                    if (found != references.end())
                    {
                        ++found->second;
                    }
                }
                catch (...)
                {
                    protectAll = true;
                }
            }
        }

        auto pageRoot =
            root.parent_path() /
            L"xcompute-storage-v1" /
            L"namespaces";
        if (fs::exists(pageRoot))
        {
            for (auto const& entry :
                 fs::recursive_directory_iterator(pageRoot))
            {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != L".json" ||
                    entry.path().parent_path().filename() != L"pages")
                {
                    continue;
                }
                try
                {
                    auto record = ReadJsonObjectFileLocal(
                        entry.path(),
                        "storage.page_record_invalid");
                    auto contentSha256 =
                        NormalizeOptionalSha256Local(
                            GetOptionalStringLocal(
                                record,
                                L"content_sha256"));
                    auto found = references.find(contentSha256);
                    if (found != references.end())
                    {
                        ++found->second;
                    }
                }
                catch (...)
                {
                    protectAll = true;
                }
            }
        }

        if (!VisitCreativeInstallBlobReferencesLocal(
                root,
                [&](std::wstring const& installedSha256)
                {
                    auto found =
                        references.find(installedSha256);
                    if (found != references.end())
                    {
                        ++found->second;
                    }
                }))
        {
            protectAll = true;
        }

        result.protectedByUnreadableMetadata = protectAll;
        if (protectAll)
        {
            return result;
        }

        for (auto const& [candidate, referenceCount] : references)
        {
            if (referenceCount != 0)
            {
                continue;
            }
            auto path = ArtifactBlobPath(root, candidate);
            if (!fs::exists(path))
            {
                continue;
            }
            auto bytes = static_cast<uint64_t>(fs::file_size(path));
            if (!fs::remove(path))
            {
                continue;
            }
            ++result.removedBlobCount;
            result.removedBytes += bytes;
            std::error_code error;
            fs::remove(path.parent_path(), error);
        }
        return result;
    }

    ArtifactReadTarget ResolveArtifactReadTarget(fs::path const& root, std::wstring const& artifactId)
    {
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        if (fs::exists(manifestPath))
        {
            auto manifest = ReadJsonObjectFileLocal(manifestPath, "artifact.manifest_invalid");
            auto sha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"sha256"));
            if (sha256.empty())
            {
                throw WorkerArtifactStoreError("artifact.sha256_missing", "committed artifact manifest does not contain a SHA-256 blob reference");
            }

            auto blobPath = ArtifactBlobPath(root, sha256);
            if (!fs::exists(blobPath))
            {
                throw WorkerArtifactStoreError("artifact.blob_missing", "committed artifact blob was not found");
            }

            ArtifactReadTarget target;
            target.path = blobPath;
            target.artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(manifest, L"artifact_kind", L"generic"));
            target.status = GetOptionalStringLocal(manifest, L"status", L"committed");
            target.sha256 = sha256;
            target.bytes = GetOptionalUInt64Local(manifest, L"bytes", static_cast<uint64_t>(fs::file_size(blobPath)));
            target.committed = true;
            return target;
        }

        auto metadataPath = ArtifactMetadataPath(root, artifactId);
        if (fs::exists(metadataPath))
        {
            auto payloadPath = ArtifactPayloadPath(root, artifactId);
            if (!fs::exists(payloadPath))
            {
                throw WorkerArtifactStoreError("artifact.payload_missing", "artifact staging payload was not uploaded");
            }

            auto metadata = ReadJsonObjectFileLocal(metadataPath, "artifact.metadata_invalid");
            ArtifactReadTarget target;
            target.path = payloadPath;
            target.artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(metadata, L"artifact_kind", L"generic"));
            target.status = L"staging";
            target.bytes = static_cast<uint64_t>(fs::file_size(payloadPath));
            target.committed = false;
            return target;
        }

        throw WorkerArtifactStoreError("artifact.not_found", "artifact was not found");
    }

    PublishedArtifactResult WorkerPublishUtf8ArtifactPayload(
        JsonObject const& request,
        fs::path const& root,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator)
    {
        auto artifactIdInput = GetOptionalStringLocal(request, L"artifact_id", defaultArtifactId);
        auto artifactId = NormalizeArtifactIdLocal(artifactIdInput, false, idGenerator);
        auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(request, L"artifact_kind", defaultArtifactKind));
        auto payloadBytes = static_cast<uint64_t>(payloadUtf8.size());
        auto rollbackHeadroomBytes = GetOptionalUInt64Local(request, L"rollback_headroom_bytes", config.defaultArtifactRollbackHeadroomBytes);
        auto workspaceBudgetBytes = GetOptionalUInt64Local(request, L"workspace_budget_bytes", config.defaultArtifactWorkspaceBudgetBytes);

        if (payloadBytes > config.maxArtifactExpectedBytes)
        {
            throw WorkerArtifactStoreError("artifact.max_size_exceeded", "job result artifact exceeds the artifact size limit");
        }
        if (workspaceBudgetBytes == 0 || workspaceBudgetBytes > config.maxArtifactWorkspaceBudgetBytes)
        {
            throw WorkerArtifactStoreError("artifact.workspace_budget_invalid", "workspace_budget_bytes is invalid");
        }

        auto stagingDir = ArtifactStagingDirectory(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        if (fs::exists(stagingDir) || fs::exists(manifestPath))
        {
            throw WorkerArtifactStoreError("artifact.exists", "artifact_id already exists");
        }

        auto workspaceBytesBefore = WorkspaceBytesLocal(root);
        auto projectedGrowthWithRollbackBytes = SaturatingAddLocal(payloadBytes, rollbackHeadroomBytes);
        auto projectedWorkspaceWithRollbackBytes = SaturatingAddLocal(workspaceBytesBefore, projectedGrowthWithRollbackBytes);
        if (projectedWorkspaceWithRollbackBytes > workspaceBudgetBytes)
        {
            throw WorkerArtifactStoreError("artifact.rollback_headroom_exceeded", "job result artifact would exceed workspace budget plus rollback headroom");
        }

        bool storageAvailableKnown = false;
        auto storageAvailableBytes = StorageAvailableBytesLocal(root, storageAvailableKnown);
        if (storageAvailableKnown && projectedGrowthWithRollbackBytes > storageAvailableBytes)
        {
            throw WorkerArtifactStoreError("artifact.storage_available_exceeded", "job result artifact would exceed platform-reported available storage plus rollback headroom");
        }

        auto generatedDir = ArtifactRoot(root) / L"generated" / artifactId;
        auto generatedPath = generatedDir / L"payload.json";
        if (fs::exists(generatedDir))
        {
            fs::remove_all(generatedDir);
        }

        std::wstring actualSha256;
        try
        {
            WriteInternalTextFileLocal(generatedPath, payloadUtf8);
            actualSha256 = Sha256FileLocal(generatedPath);
        }
        catch (...)
        {
            fs::remove_all(generatedDir);
            throw;
        }

        auto blobPath = ArtifactBlobPath(root, actualSha256);
        auto deduplicated = fs::exists(blobPath);
        auto publishStarted = std::chrono::steady_clock::now();
        try
        {
            if (!deduplicated)
            {
                fs::create_directories(blobPath.parent_path());
                auto tempBlobPath = blobPath;
                tempBlobPath += L".tmp";
                if (fs::exists(tempBlobPath))
                {
                    fs::remove(tempBlobPath);
                }
                fs::rename(generatedPath, tempBlobPath);
                fs::rename(tempBlobPath, blobPath);
            }
            fs::remove_all(generatedDir);
        }
        catch (...)
        {
            fs::remove_all(generatedDir);
            throw;
        }
        auto publishMs = ElapsedMillisecondsLocal(publishStarted);

        auto blobHandle = ArtifactHandleForBlob(root, blobPath);
        auto manifest =
            L"{\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"protocol_version\":" + JsonString(config.protocolVersion) +
            L",\"status\":\"committed\"" +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"bytes\":" + std::to_wstring(payloadBytes) +
            L",\"expected_bytes_known\":true" +
            L",\"expected_bytes\":" + std::to_wstring(payloadBytes) +
            L",\"sha256\":" + JsonString(actualSha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            extraManifestFields +
            L"}";
        WriteInternalTextFileLocal(manifestPath, WideToUtf8Local(manifest));

        PublishedArtifactResult result;
        result.artifactId = artifactId;
        result.artifactKind = artifactKind;
        result.sha256 = actualSha256;
        result.blobHandle = blobHandle;
        result.manifestPath = manifestPath.lexically_relative(root).wstring();
        result.bytes = payloadBytes;
        result.workspaceBytesBefore = workspaceBytesBefore;
        result.workspaceBytesAfter = WorkspaceBytesLocal(root);
        result.workspaceBudgetBytes = workspaceBudgetBytes;
        result.rollbackHeadroomBytes = rollbackHeadroomBytes;
        result.storageAvailableKnown = storageAvailableKnown;
        result.storageAvailableBytes = storageAvailableBytes;
        result.deduplicated = deduplicated;
        result.publishMs = publishMs;
        return result;
    }

    std::wstring WorkerExecuteArtifactQuotaPreflight(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactIdInput = GetOptionalStringLocal(request, L"artifact_id");
        auto artifactId = artifactIdInput.empty() ? L"" : NormalizeArtifactIdLocal(artifactIdInput, true, nullptr);
        auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(request, L"artifact_kind", L"generic"));
        auto expectedSha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(request, L"expected_sha256"));
        auto expectedBytes = GetOptionalUInt64Local(request, L"expected_bytes", 0);
        auto projectedUploadBytes = request.HasKey(L"projected_upload_bytes") ?
            GetOptionalUInt64Local(request, L"projected_upload_bytes", expectedBytes) :
            expectedBytes;
        auto projectedJobOutputBytes = GetOptionalUInt64Local(request, L"projected_job_output_bytes", 0);
        auto rollbackHeadroomBytes = GetOptionalUInt64Local(request, L"rollback_headroom_bytes", config.defaultArtifactRollbackHeadroomBytes);
        auto workspaceBudgetBytes = GetOptionalUInt64Local(request, L"workspace_budget_bytes", config.defaultArtifactWorkspaceBudgetBytes);

        std::vector<std::wstring> violations;
        if (projectedUploadBytes > config.maxArtifactExpectedBytes)
        {
            violations.push_back(L"artifact.projected_upload_exceeds_max");
        }
        if (expectedBytes > config.maxArtifactExpectedBytes)
        {
            violations.push_back(L"artifact.expected_bytes_exceeds_max");
        }
        if (projectedJobOutputBytes > config.maxArtifactExpectedBytes)
        {
            violations.push_back(L"artifact.projected_job_output_exceeds_max");
        }
        if (workspaceBudgetBytes == 0 || workspaceBudgetBytes > config.maxArtifactWorkspaceBudgetBytes)
        {
            violations.push_back(L"artifact.workspace_budget_invalid");
        }

        auto stagingRoot = ArtifactRoot(root) / L"staging";
        auto blobRoot = ArtifactRoot(root) / L"blobs";
        auto workspaceBytes = WorkspaceBytesLocal(root);
        auto workspaceFileCount = WorkspaceFileCountLocal(root);
        auto workspaceDirectoryCount = WorkspaceDirectoryCountLocal(root);
        auto artifactStagingBytes = DirectoryBytesIfExistsLocal(stagingRoot);
        auto artifactCommittedBlobBytes = DirectoryBytesIfExistsLocal(blobRoot);

        bool knownBlobExists = false;
        if (!expectedSha256.empty())
        {
            knownBlobExists = fs::exists(ArtifactBlobPath(root, expectedSha256));
        }
        auto effectiveProjectedUploadBytes = knownBlobExists ? 0 : projectedUploadBytes;

        bool artifactPathAvailable = true;
        if (!artifactId.empty())
        {
            auto stagingDir = ArtifactStagingDirectory(root, artifactId);
            auto manifestPath = ArtifactManifestPath(root, artifactId);
            artifactPathAvailable = !fs::exists(stagingDir) && !fs::exists(manifestPath);
            if (!artifactPathAvailable)
            {
                violations.push_back(L"artifact.id_not_available");
            }
        }

        auto projectedWorkBytes = SaturatingAddLocal(effectiveProjectedUploadBytes, projectedJobOutputBytes);
        auto projectedGrowthWithRollbackBytes = SaturatingAddLocal(projectedWorkBytes, rollbackHeadroomBytes);
        auto projectedWorkspaceBytes = SaturatingAddLocal(workspaceBytes, projectedWorkBytes);
        auto projectedWorkspaceWithRollbackBytes = SaturatingAddLocal(workspaceBytes, projectedGrowthWithRollbackBytes);
        auto workspaceHeadroomBytes = workspaceBudgetBytes > workspaceBytes ? workspaceBudgetBytes - workspaceBytes : 0;
        auto rollbackSafe = projectedWorkspaceWithRollbackBytes <= workspaceBudgetBytes;
        if (!rollbackSafe)
        {
            violations.push_back(L"artifact.rollback_headroom_exceeded");
        }

        bool storageAvailableKnown = false;
        auto storageAvailableBytes = StorageAvailableBytesLocal(root, storageAvailableKnown);
        if (storageAvailableKnown && projectedGrowthWithRollbackBytes > storageAvailableBytes)
        {
            violations.push_back(L"artifact.storage_available_exceeded");
        }

        return OkBaseLocal(L"artifact_quota_preflight", config) +
            L",\"schema_version\":\"worker-artifact-quota-preflight-0.1\"" +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"can_accept\":" + BoolJsonLocal(violations.empty()) +
            L",\"violation_count\":" + std::to_wstring(violations.size()) +
            L",\"violations\":" + JsonStringArrayLocal(violations) +
            L",\"artifact_path_available\":" + BoolJsonLocal(artifactPathAvailable) +
            L",\"known_blob_exists\":" + BoolJsonLocal(knownBlobExists) +
            L",\"deduplicated_candidate\":" + BoolJsonLocal(knownBlobExists) +
            L",\"expected_sha256\":" + JsonString(expectedSha256) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
            L",\"projected_upload_bytes\":" + std::to_wstring(projectedUploadBytes) +
            L",\"effective_projected_upload_bytes\":" + std::to_wstring(effectiveProjectedUploadBytes) +
            L",\"projected_job_output_bytes\":" + std::to_wstring(projectedJobOutputBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(rollbackHeadroomBytes) +
            L",\"projected_work_bytes\":" + std::to_wstring(projectedWorkBytes) +
            L",\"projected_growth_with_rollback_bytes\":" + std::to_wstring(projectedGrowthWithRollbackBytes) +
            L",\"projected_workspace_bytes\":" + std::to_wstring(projectedWorkspaceBytes) +
            L",\"projected_workspace_with_rollback_bytes\":" + std::to_wstring(projectedWorkspaceWithRollbackBytes) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(workspaceBudgetBytes) +
            L",\"workspace_headroom_bytes\":" + std::to_wstring(workspaceHeadroomBytes) +
            L",\"rollback_safe\":" + BoolJsonLocal(rollbackSafe) +
            L",\"max_artifact_bytes\":" + std::to_wstring(config.maxArtifactExpectedBytes) +
            L",\"max_workspace_budget_bytes\":" + std::to_wstring(config.maxArtifactWorkspaceBudgetBytes) +
            L",\"workspace_path\":" + JsonString(root.wstring()) +
            L",\"workspace_bytes\":" + std::to_wstring(workspaceBytes) +
            L",\"workspace_file_count\":" + std::to_wstring(workspaceFileCount) +
            L",\"workspace_directory_count\":" + std::to_wstring(workspaceDirectoryCount) +
            L",\"artifact_staging_bytes\":" + std::to_wstring(artifactStagingBytes) +
            L",\"artifact_committed_blob_bytes\":" + std::to_wstring(artifactCommittedBlobBytes) +
            L",\"storage_available_known\":" + BoolJsonLocal(storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(storageAvailableBytes) +
            L"}";
    }

    std::wstring WorkerExecuteBeginArtifactUpload(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), false, idGenerator);
        auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(request, L"artifact_kind", L"generic"));
        auto expectedBytesKnown = request.HasKey(L"expected_bytes");
        auto expectedBytes = GetBoundedOptionalUInt64Local(request, L"expected_bytes", 0, 0, config.maxArtifactExpectedBytes);
        auto expectedSha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(request, L"expected_sha256"));
        auto stagingDir = ArtifactStagingDirectory(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);

        if (fs::exists(stagingDir) || fs::exists(manifestPath))
        {
            throw WorkerArtifactStoreError("artifact.exists", "artifact_id already exists");
        }

        fs::create_directories(stagingDir);
        WriteArtifactMetadataLocal(ArtifactMetadataPath(root, artifactId), artifactId, artifactKind, expectedBytesKnown, expectedBytes, expectedSha256, config);

        return OkBaseLocal(L"begin_artifact_upload", config) +
            L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"status\":\"staging\"" +
            L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
            L",\"expected_sha256\":" + JsonString(expectedSha256) +
            L",\"staging_path\":" + JsonString(stagingDir.lexically_relative(root).wstring()) +
            L"}";
    }

    std::wstring WorkerExecuteAppendArtifactChunk(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto metadataPath = ArtifactMetadataPath(root, artifactId);
        if (!fs::exists(metadataPath))
        {
            throw WorkerArtifactStoreError("artifact.not_staging", "artifact staging metadata was not found");
        }

        auto metadata = ReadJsonObjectFileLocal(metadataPath, "artifact.metadata_invalid");
        auto expectedBytesKnown = metadata.GetNamedBoolean(L"expected_bytes_known", metadata.HasKey(L"expected_bytes"));
        auto expectedBytes = GetOptionalUInt64Local(metadata, L"expected_bytes", 0);
        auto offset = GetOptionalUInt64Local(request, L"offset", 0);
        auto dataBase64 = WideToUtf8Local(GetOptionalStringLocal(request, L"data_base64"));
        if (dataBase64.empty())
        {
            throw WorkerArtifactStoreError("artifact.data_required", "append_artifact_chunk requires data_base64");
        }

        auto decodeStarted = std::chrono::steady_clock::now();
        auto data = Base64DecodeLocal(dataBase64);
        auto decodeMs = ElapsedMillisecondsLocal(decodeStarted);
        if (data.size() > config.maxArtifactJsonChunkBytes)
        {
            throw WorkerArtifactStoreError("artifact.chunk_too_large", "append_artifact_chunk data exceeds 64 KiB");
        }
        auto chunkBytes = static_cast<uint64_t>(data.size());
        if (offset > config.maxArtifactExpectedBytes || chunkBytes > config.maxArtifactExpectedBytes - offset)
        {
            throw WorkerArtifactStoreError("artifact.max_size_exceeded", "append_artifact_chunk exceeds the artifact size limit");
        }
        if (expectedBytesKnown && (offset > expectedBytes || chunkBytes > expectedBytes - offset))
        {
            throw WorkerArtifactStoreError("artifact.expected_size_exceeded", "append_artifact_chunk exceeds expected_bytes");
        }

        auto payloadPath = ArtifactPayloadPath(root, artifactId);
        auto writeStarted = std::chrono::steady_clock::now();
        WriteBinaryChunkLocal(
            payloadPath,
            offset,
            data,
            false,
            static_cast<size_t>(config.maxArtifactJsonChunkBytes),
            "artifact.chunk_too_large",
            "append_artifact_chunk data exceeds 64 KiB");
        auto writeMs = ElapsedMillisecondsLocal(writeStarted);
        auto stagedBytes = fs::exists(payloadPath) ? static_cast<uint64_t>(fs::file_size(payloadPath)) : 0;
        auto responseStarted = std::chrono::steady_clock::now();
        auto response = OkBaseLocal(L"append_artifact_chunk", config) +
            L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_written\":" + std::to_wstring(data.size()) +
            L",\"staged_bytes\":" + std::to_wstring(stagedBytes) +
            L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes);
        auto responseMs = ElapsedMillisecondsLocal(responseStarted);
        response += SyncServerTimingJsonLocal(request, 0.0, decodeMs, writeMs, responseMs, dataBase64.size(), data.size());
        response += L"}";
        return response;
    }

    std::wstring WorkerExecuteAppendArtifactChunkBinary(
        JsonObject const& request,
        fs::path const& root,
        std::vector<uint8_t> const& data,
        double authMs,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto expectedPayloadBytes = GetOptionalUInt64Local(request, L"byte_count", data.size());
        if (expectedPayloadBytes != data.size())
        {
            throw WorkerArtifactStoreError("artifact.binary_length_mismatch", "append_artifact_chunk_binary byte_count does not match received payload");
        }

        auto metadataPath = ArtifactMetadataPath(root, artifactId);
        if (!fs::exists(metadataPath))
        {
            throw WorkerArtifactStoreError("artifact.not_staging", "artifact staging metadata was not found");
        }

        auto metadata = ReadJsonObjectFileLocal(metadataPath, "artifact.metadata_invalid");
        auto expectedBytesKnown = metadata.GetNamedBoolean(L"expected_bytes_known", metadata.HasKey(L"expected_bytes"));
        auto expectedBytes = GetOptionalUInt64Local(metadata, L"expected_bytes", 0);
        auto offset = GetOptionalUInt64Local(request, L"offset", 0);
        if (data.size() > config.maxStreamWriteBytes)
        {
            throw WorkerArtifactStoreError("artifact.binary_chunk_too_large", "append_artifact_chunk_binary data exceeds 4 MiB");
        }
        auto chunkBytes = static_cast<uint64_t>(data.size());
        if (offset > config.maxArtifactExpectedBytes || chunkBytes > config.maxArtifactExpectedBytes - offset)
        {
            throw WorkerArtifactStoreError("artifact.max_size_exceeded", "append_artifact_chunk_binary exceeds the artifact size limit");
        }
        if (expectedBytesKnown && (offset > expectedBytes || chunkBytes > expectedBytes - offset))
        {
            throw WorkerArtifactStoreError("artifact.expected_size_exceeded", "append_artifact_chunk_binary exceeds expected_bytes");
        }

        auto payloadPath = ArtifactPayloadPath(root, artifactId);
        auto writeStarted = std::chrono::steady_clock::now();
        WriteBinaryChunkLocal(
            payloadPath,
            offset,
            data,
            false,
            static_cast<size_t>(config.maxStreamWriteBytes),
            "artifact.binary_chunk_too_large",
            "append_artifact_chunk_binary data exceeds 4 MiB");
        auto writeMs = ElapsedMillisecondsLocal(writeStarted);
        auto stagedBytes = fs::exists(payloadPath) ? static_cast<uint64_t>(fs::file_size(payloadPath)) : 0;
        auto responseStarted = std::chrono::steady_clock::now();
        auto response = OkBaseLocal(L"append_artifact_chunk_binary", config) +
            L",\"schema_version\":" + JsonString(config.artifactBinaryFramingSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"offset\":" + std::to_wstring(offset) +
            L",\"bytes_written\":" + std::to_wstring(data.size()) +
            L",\"byte_count\":" + std::to_wstring(data.size()) +
            L",\"staged_bytes\":" + std::to_wstring(stagedBytes) +
            L",\"stream_max_bytes\":" + std::to_wstring(config.maxStreamWriteBytes) +
            L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
            L",\"encoding\":\"binary\"";
        auto responseMs = ElapsedMillisecondsLocal(responseStarted);
        response += SyncServerTimingJsonLocal(request, authMs, 0.0, writeMs, responseMs, data.size(), data.size());
        response += L"}";
        return response;
    }

    std::wstring WorkerExecuteCommitArtifactUpload(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto metadataPath = ArtifactMetadataPath(root, artifactId);
        auto payloadPath = ArtifactPayloadPath(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        if (!fs::exists(metadataPath))
        {
            throw WorkerArtifactStoreError("artifact.not_staging", "artifact staging metadata was not found");
        }
        if (fs::exists(manifestPath))
        {
            throw WorkerArtifactStoreError("artifact.already_committed", "artifact already has a committed manifest");
        }

        auto metadata = ReadJsonObjectFileLocal(metadataPath, "artifact.metadata_invalid");
        auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(metadata, L"artifact_kind", L"generic"));
        auto expectedBytesKnown = metadata.GetNamedBoolean(L"expected_bytes_known", metadata.HasKey(L"expected_bytes"));
        auto expectedBytes = GetOptionalUInt64Local(metadata, L"expected_bytes", 0);
        auto expectedSha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(metadata, L"expected_sha256"));
        if (!fs::exists(payloadPath))
        {
            if (!expectedBytesKnown || expectedBytes == 0)
            {
                WriteBinaryFileLocal(payloadPath, std::vector<uint8_t>{});
            }
            else
            {
                throw WorkerArtifactStoreError("artifact.payload_missing", "artifact payload was not uploaded");
            }
        }

        auto actualBytes = static_cast<uint64_t>(fs::file_size(payloadPath));
        if (actualBytes > config.maxArtifactExpectedBytes)
        {
            throw WorkerArtifactStoreError("artifact.max_size_exceeded", "artifact payload exceeds the artifact size limit");
        }
        if (expectedBytesKnown && actualBytes != expectedBytes)
        {
            throw WorkerArtifactStoreError("artifact.size_mismatch", "artifact payload size does not match expected_bytes");
        }

        auto actualSha256 = Sha256FileLocal(payloadPath);
        if (!expectedSha256.empty() && actualSha256 != expectedSha256)
        {
            throw WorkerArtifactStoreError("artifact.sha256_mismatch", "artifact payload SHA-256 does not match expected_sha256");
        }

        auto blobPath = ArtifactBlobPath(root, actualSha256);
        auto deduplicated = fs::exists(blobPath);
        if (!deduplicated)
        {
            fs::create_directories(blobPath.parent_path());
            auto tempPath = blobPath;
            tempPath += L".tmp";
            if (fs::exists(tempPath))
            {
                fs::remove(tempPath);
            }
            fs::copy_file(payloadPath, tempPath, fs::copy_options::none);
            fs::rename(tempPath, blobPath);
        }

        auto blobHandle = ArtifactHandleForBlob(root, blobPath);
        auto manifest =
            L"{\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"protocol_version\":" + JsonString(config.protocolVersion) +
            L",\"status\":\"committed\"" +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"bytes\":" + std::to_wstring(actualBytes) +
            L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
            L",\"sha256\":" + JsonString(actualSha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            L"}";
        WriteInternalTextFileLocal(manifestPath, WideToUtf8Local(manifest));
        fs::remove_all(ArtifactStagingDirectory(root, artifactId));

        return OkBaseLocal(L"commit_artifact_upload", config) +
            L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"status\":\"committed\"" +
            L",\"bytes\":" + std::to_wstring(actualBytes) +
            L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
            L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
            L",\"sha256\":" + JsonString(actualSha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            L",\"manifest_path\":" + JsonString(manifestPath.lexically_relative(root).wstring()) +
            L",\"deduplicated\":" + BoolJsonLocal(deduplicated) +
            L"}";
    }

    std::wstring WorkerExecuteGenerateArtifactDataset(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), false, idGenerator);
        auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(request, L"artifact_kind", L"dataset"));
        auto byteCount = GetBoundedOptionalUInt64Local(request, L"byte_count", 1024 * 1024, 0, config.maxArtifactExpectedBytes);
        auto seed = static_cast<uint32_t>(GetBoundedOptionalUInt64Local(request, L"seed", 17, 0, UINT32_MAX));
        auto pattern = NormalizeDatasetPatternLocal(GetOptionalStringLocal(request, L"pattern", L"xorshift32"));
        auto rollbackHeadroomBytes = GetOptionalUInt64Local(request, L"rollback_headroom_bytes", config.defaultArtifactRollbackHeadroomBytes);
        auto workspaceBudgetBytes = GetOptionalUInt64Local(request, L"workspace_budget_bytes", config.defaultArtifactWorkspaceBudgetBytes);

        if (workspaceBudgetBytes == 0 || workspaceBudgetBytes > config.maxArtifactWorkspaceBudgetBytes)
        {
            throw WorkerArtifactStoreError("artifact.workspace_budget_invalid", "workspace_budget_bytes is invalid");
        }

        auto stagingDir = ArtifactStagingDirectory(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        if (fs::exists(stagingDir) || fs::exists(manifestPath))
        {
            throw WorkerArtifactStoreError("artifact.exists", "artifact_id already exists");
        }

        auto workspaceBytesBefore = WorkspaceBytesLocal(root);
        auto projectedGrowthWithRollbackBytes = SaturatingAddLocal(byteCount, rollbackHeadroomBytes);
        auto projectedWorkspaceWithRollbackBytes = SaturatingAddLocal(workspaceBytesBefore, projectedGrowthWithRollbackBytes);
        if (projectedWorkspaceWithRollbackBytes > workspaceBudgetBytes)
        {
            throw WorkerArtifactStoreError("artifact.rollback_headroom_exceeded", "generated dataset would exceed workspace budget plus rollback headroom");
        }

        bool storageAvailableKnown = false;
        auto storageAvailableBytes = StorageAvailableBytesLocal(root, storageAvailableKnown);
        if (storageAvailableKnown && projectedGrowthWithRollbackBytes > storageAvailableBytes)
        {
            throw WorkerArtifactStoreError("artifact.storage_available_exceeded", "generated dataset would exceed platform-reported available storage plus rollback headroom");
        }

        auto generatedDir = ArtifactRoot(root) / L"generated" / artifactId;
        auto generatedPath = generatedDir / L"payload.bin";
        if (fs::exists(generatedDir))
        {
            fs::remove_all(generatedDir);
        }

        double generationMs = 0.0;
        std::wstring actualSha256;
        try
        {
            actualSha256 = WriteDeterministicDatasetFileLocal(generatedPath, byteCount, seed, pattern, generationMs);
        }
        catch (...)
        {
            fs::remove_all(generatedDir);
            throw;
        }

        auto blobPath = ArtifactBlobPath(root, actualSha256);
        auto deduplicated = fs::exists(blobPath);
        auto publishStarted = std::chrono::steady_clock::now();
        try
        {
            if (!deduplicated)
            {
                fs::create_directories(blobPath.parent_path());
                auto tempBlobPath = blobPath;
                tempBlobPath += L".tmp";
                if (fs::exists(tempBlobPath))
                {
                    fs::remove(tempBlobPath);
                }
                fs::rename(generatedPath, tempBlobPath);
                fs::rename(tempBlobPath, blobPath);
            }
            fs::remove_all(generatedDir);
        }
        catch (...)
        {
            fs::remove_all(generatedDir);
            throw;
        }
        auto publishMs = ElapsedMillisecondsLocal(publishStarted);

        auto blobHandle = ArtifactHandleForBlob(root, blobPath);
        auto manifest =
            L"{\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"protocol_version\":" + JsonString(config.protocolVersion) +
            L",\"status\":\"committed\"" +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"bytes\":" + std::to_wstring(byteCount) +
            L",\"expected_bytes_known\":true" +
            L",\"expected_bytes\":" + std::to_wstring(byteCount) +
            L",\"sha256\":" + JsonString(actualSha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            L",\"generator_schema\":\"worker-generated-dataset-0.1\"" +
            L",\"generator_pattern\":" + JsonString(pattern) +
            L",\"generator_seed\":" + std::to_wstring(seed) +
            L"}";
        WriteInternalTextFileLocal(manifestPath, WideToUtf8Local(manifest));

        auto workspaceBytesAfter = WorkspaceBytesLocal(root);
        return OkBaseLocal(L"generate_artifact_dataset", config) +
            L",\"schema_version\":\"worker-generated-dataset-0.1\"" +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"status\":\"committed\"" +
            L",\"bytes\":" + std::to_wstring(byteCount) +
            L",\"pattern\":" + JsonString(pattern) +
            L",\"seed\":" + std::to_wstring(seed) +
            L",\"sha256\":" + JsonString(actualSha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            L",\"manifest_path\":" + JsonString(manifestPath.lexically_relative(root).wstring()) +
            L",\"deduplicated\":" + BoolJsonLocal(deduplicated) +
            L",\"workspace_bytes_before\":" + std::to_wstring(workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(workspaceBytesAfter) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(workspaceBudgetBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(rollbackHeadroomBytes) +
            L",\"storage_available_known\":" + BoolJsonLocal(storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(storageAvailableBytes) +
            L",\"generation_ms\":" + DoubleJsonLocal(generationMs, 6) +
            L",\"publish_ms\":" + DoubleJsonLocal(publishMs, 6) +
            L"}";
    }

    std::wstring WorkerExecuteAbortArtifactUpload(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto stagingDir = ArtifactStagingDirectory(root, artifactId);
        auto existed = fs::exists(stagingDir);
        uint64_t removed = 0;
        if (existed)
        {
            removed = static_cast<uint64_t>(fs::remove_all(stagingDir));
        }
        return OkBaseLocal(L"abort_artifact_upload", config) +
            L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"existed\":" + BoolJsonLocal(existed) +
            L",\"removed\":" + std::to_wstring(removed) +
            L"}";
    }

    std::wstring WorkerExecuteDeleteArtifact(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto deleteUnreferencedBlob = request.GetNamedBoolean(L"delete_unreferenced_blob", true);
        auto stagingDir = ArtifactStagingDirectory(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        auto workspaceBytesBefore = WorkspaceBytesLocal(root);

        auto stagingExisted = fs::exists(stagingDir);
        auto manifestExisted = fs::exists(manifestPath);
        auto stagingBytes = DirectoryBytesIfExistsLocal(stagingDir);
        auto manifestBytes = FileBytesIfExistsLocal(manifestPath);
        std::wstring artifactKind;
        std::wstring sha256;
        std::wstring blobHandle;
        uint64_t artifactBytes = 0;
        uint64_t blobBytes = 0;
        bool blobExistedBefore = false;
        uint64_t blobReferenceCountBefore = 0;
        uint64_t blobReferenceCountAfter = 0;
        bool blobRemoved = false;
        bool blobPrefixDirectoryRemoved = false;

        if (manifestExisted)
        {
            auto manifest = ReadJsonObjectFileLocal(manifestPath, "artifact.manifest_invalid");
            artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(manifest, L"artifact_kind", L"generic"));
            sha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"sha256"));
            blobHandle = GetOptionalStringLocal(manifest, L"blob_handle");
            artifactBytes = GetOptionalUInt64Local(manifest, L"bytes", 0);
            if (!sha256.empty())
            {
                auto blobPath = ArtifactBlobPath(root, sha256);
                blobExistedBefore = fs::exists(blobPath);
                blobBytes = FileBytesIfExistsLocal(blobPath);
                blobReferenceCountBefore = CountArtifactBlobReferencesLocal(root, sha256);
            }
        }

        uint64_t stagingRemovedEntries = 0;
        if (stagingExisted)
        {
            stagingRemovedEntries = static_cast<uint64_t>(fs::remove_all(stagingDir));
        }

        bool manifestRemoved = false;
        if (manifestExisted)
        {
            manifestRemoved = fs::remove(manifestPath);
        }

        if (!sha256.empty())
        {
            blobReferenceCountAfter = CountArtifactBlobReferencesLocal(root, sha256);
            auto blobPath = ArtifactBlobPath(root, sha256);
            if (deleteUnreferencedBlob && blobReferenceCountAfter == 0 && fs::exists(blobPath))
            {
                blobRemoved = fs::remove(blobPath);
            }
            if (blobRemoved)
            {
                std::error_code removeError;
                blobPrefixDirectoryRemoved = fs::remove(blobPath.parent_path(), removeError);
            }
        }

        auto workspaceBytesAfter = WorkspaceBytesLocal(root);
        auto removedBytesEstimate = stagingBytes + (manifestRemoved ? manifestBytes : 0) + (blobRemoved ? blobBytes : 0);
        return OkBaseLocal(L"delete_artifact", config) +
            L",\"schema_version\":\"worker-artifact-cleanup-0.1\"" +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"artifact_kind\":" + JsonString(artifactKind) +
            L",\"existed\":" + BoolJsonLocal(stagingExisted || manifestExisted) +
            L",\"staging_existed\":" + BoolJsonLocal(stagingExisted) +
            L",\"staging_removed_entries\":" + std::to_wstring(stagingRemovedEntries) +
            L",\"manifest_existed\":" + BoolJsonLocal(manifestExisted) +
            L",\"manifest_removed\":" + BoolJsonLocal(manifestRemoved) +
            L",\"artifact_bytes\":" + std::to_wstring(artifactBytes) +
            L",\"sha256\":" + JsonString(sha256) +
            L",\"blob_handle\":" + JsonString(blobHandle) +
            L",\"delete_unreferenced_blob\":" + BoolJsonLocal(deleteUnreferencedBlob) +
            L",\"blob_existed_before\":" + BoolJsonLocal(blobExistedBefore) +
            L",\"blob_bytes\":" + std::to_wstring(blobBytes) +
            L",\"blob_reference_count_before\":" + std::to_wstring(blobReferenceCountBefore) +
            L",\"blob_reference_count_after\":" + std::to_wstring(blobReferenceCountAfter) +
            L",\"blob_removed\":" + BoolJsonLocal(blobRemoved) +
            L",\"blob_prefix_directory_removed\":" + BoolJsonLocal(blobPrefixDirectoryRemoved) +
            L",\"removed_bytes_estimate\":" + std::to_wstring(removedBytesEstimate) +
            L",\"workspace_bytes_before\":" + std::to_wstring(workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(workspaceBytesAfter) +
            L"}";
    }

    std::wstring WorkerExecuteReapArtifacts(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto dryRun = request.GetNamedBoolean(L"dry_run", false);
        auto removeUnreferencedBlobs = request.GetNamedBoolean(L"remove_unreferenced_blobs", true);
        auto removeGeneratedTemps = request.GetNamedBoolean(L"remove_generated_temps", true);
        auto workspaceBytesBefore = WorkspaceBytesLocal(root);

        uint64_t unreferencedBlobCandidates = 0;
        uint64_t unreferencedBlobCandidateBytes = 0;
        uint64_t unreferencedBlobRemoved = 0;
        uint64_t unreferencedBlobRemovedBytes = 0;
        uint64_t blobPrefixDirectoryCandidates = 0;
        uint64_t blobPrefixDirectoriesRemoved = 0;
        if (removeUnreferencedBlobs)
        {
            auto blobRoot = ArtifactRoot(root) / L"blobs" / L"sha256";
            if (fs::exists(blobRoot))
            {
                for (auto const& entry : fs::recursive_directory_iterator(blobRoot))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != L".bin")
                    {
                        continue;
                    }

                    auto sha256 = entry.path().stem().wstring();
                    if (!IsSha256HexLocal(sha256))
                    {
                        continue;
                    }
                    sha256 = ToLowerAsciiLocal(sha256);
                    if (CountArtifactBlobReferencesLocal(root, sha256) != 0)
                    {
                        continue;
                    }

                    auto bytes = FileBytesIfExistsLocal(entry.path());
                    ++unreferencedBlobCandidates;
                    unreferencedBlobCandidateBytes += bytes;
                    if (!dryRun && fs::remove(entry.path()))
                    {
                        ++unreferencedBlobRemoved;
                        unreferencedBlobRemovedBytes += bytes;
                    }
                }

                for (auto const& entry : fs::directory_iterator(blobRoot))
                {
                    if (!entry.is_directory())
                    {
                        continue;
                    }

                    bool pruneCandidate = false;
                    if (!dryRun)
                    {
                        std::error_code emptyError;
                        pruneCandidate = fs::is_empty(entry.path(), emptyError) && !emptyError;
                    }
                    else
                    {
                        pruneCandidate = true;
                        for (auto const& child : fs::directory_iterator(entry.path()))
                        {
                            if (!child.is_regular_file() || child.path().extension() != L".bin")
                            {
                                pruneCandidate = false;
                                break;
                            }
                            auto sha256 = child.path().stem().wstring();
                            if (!IsSha256HexLocal(sha256) ||
                                CountArtifactBlobReferencesLocal(root, ToLowerAsciiLocal(sha256)) != 0)
                            {
                                pruneCandidate = false;
                                break;
                            }
                        }
                    }

                    if (!pruneCandidate)
                    {
                        continue;
                    }
                    ++blobPrefixDirectoryCandidates;
                    if (!dryRun)
                    {
                        std::error_code removeError;
                        if (fs::remove(entry.path(), removeError) && !removeError)
                        {
                            ++blobPrefixDirectoriesRemoved;
                        }
                    }
                }
            }
        }

        uint64_t generatedTempCandidates = 0;
        uint64_t generatedTempCandidateBytes = 0;
        uint64_t generatedTempRemovedEntries = 0;
        uint64_t generatedTempRemovedBytes = 0;
        if (removeGeneratedTemps)
        {
            auto generatedRoot = ArtifactRoot(root) / L"generated";
            if (fs::exists(generatedRoot))
            {
                for (auto const& entry : fs::directory_iterator(generatedRoot))
                {
                    auto bytes = entry.is_directory() ? DirectoryBytesIfExistsLocal(entry.path()) : FileBytesIfExistsLocal(entry.path());
                    ++generatedTempCandidates;
                    generatedTempCandidateBytes += bytes;
                    if (!dryRun)
                    {
                        generatedTempRemovedEntries += static_cast<uint64_t>(fs::remove_all(entry.path()));
                        generatedTempRemovedBytes += bytes;
                    }
                }
            }
        }

        auto workspaceBytesAfter = WorkspaceBytesLocal(root);
        return OkBaseLocal(L"reap_artifacts", config) +
            L",\"schema_version\":\"worker-artifact-cleanup-0.1\"" +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"dry_run\":" + BoolJsonLocal(dryRun) +
            L",\"remove_unreferenced_blobs\":" + BoolJsonLocal(removeUnreferencedBlobs) +
            L",\"remove_generated_temps\":" + BoolJsonLocal(removeGeneratedTemps) +
            L",\"unreferenced_blob_candidates\":" + std::to_wstring(unreferencedBlobCandidates) +
            L",\"unreferenced_blob_candidate_bytes\":" + std::to_wstring(unreferencedBlobCandidateBytes) +
            L",\"unreferenced_blob_removed\":" + std::to_wstring(unreferencedBlobRemoved) +
            L",\"unreferenced_blob_removed_bytes\":" + std::to_wstring(unreferencedBlobRemovedBytes) +
            L",\"blob_prefix_directory_candidates\":" + std::to_wstring(blobPrefixDirectoryCandidates) +
            L",\"blob_prefix_directories_removed\":" + std::to_wstring(blobPrefixDirectoriesRemoved) +
            L",\"generated_temp_candidates\":" + std::to_wstring(generatedTempCandidates) +
            L",\"generated_temp_candidate_bytes\":" + std::to_wstring(generatedTempCandidateBytes) +
            L",\"generated_temp_removed_entries\":" + std::to_wstring(generatedTempRemovedEntries) +
            L",\"generated_temp_removed_bytes\":" + std::to_wstring(generatedTempRemovedBytes) +
            L",\"workspace_bytes_before\":" + std::to_wstring(workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(workspaceBytesAfter) +
            L"}";
    }

    std::wstring WorkerExecuteGetArtifactStatus(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactStoreConfig const& config)
    {
        auto artifactId = NormalizeArtifactIdLocal(GetOptionalStringLocal(request, L"artifact_id"), true, nullptr);
        auto metadataPath = ArtifactMetadataPath(root, artifactId);
        auto manifestPath = ArtifactManifestPath(root, artifactId);
        if (fs::exists(manifestPath))
        {
            auto manifest = ReadJsonObjectFileLocal(manifestPath, "artifact.manifest_invalid");
            auto bytes = GetOptionalUInt64Local(manifest, L"bytes", 0);
            auto expectedBytesKnown = manifest.GetNamedBoolean(L"expected_bytes_known", manifest.HasKey(L"expected_bytes"));
            auto expectedBytes = GetOptionalUInt64Local(manifest, L"expected_bytes", 0);
            auto sha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"sha256"));
            auto blobHandle = GetOptionalStringLocal(manifest, L"blob_handle");
            auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(manifest, L"artifact_kind", L"generic"));
            std::wstring workerProvenanceJson;
            if ((artifactKind == L"xvm-state-snapshot-v2" || artifactKind == L"xvm-state-snapshot-v3" ||
                artifactKind == L"xvm-state-snapshot-v4" || artifactKind == L"xvm-state-snapshot-v5") &&
                manifest.HasKey(L"execution_id") && manifest.HasKey(L"checkpoint_sequence") &&
                manifest.HasKey(L"previous_state_sha256") && manifest.HasKey(L"bound_input_sha256") &&
                manifest.HasKey(L"snapshot_seal_schema") &&
                manifest.HasKey(L"snapshot_seal_key_id"))
            {
                workerProvenanceJson = std::wstring(L",\"worker_provenance\":{\"schema_version\":\"xvm-snapshot-artifact-provenance-v1\"") +
                    L",\"execution_id\":" + JsonString(GetOptionalStringLocal(manifest, L"execution_id")) +
                    L",\"checkpoint_sequence\":" + std::to_wstring(GetOptionalUInt64Local(manifest, L"checkpoint_sequence", 0)) +
                    L",\"previous_state_sha256\":" + JsonString(NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"previous_state_sha256"))) +
                    L",\"bound_input_sha256\":" + JsonString(NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"bound_input_sha256"))) +
                    L",\"seal_schema_version\":" + JsonString(GetOptionalStringLocal(manifest, L"snapshot_seal_schema")) +
                    L",\"seal_key_id\":" + JsonString(NormalizeOptionalSha256Local(GetOptionalStringLocal(manifest, L"snapshot_seal_key_id"))) +
                    L"}";
            }
            return OkBaseLocal(L"get_artifact_status", config) +
                L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
                L",\"artifact_id\":" + JsonString(artifactId) +
                L",\"artifact_kind\":" + JsonString(artifactKind) +
                L",\"exists\":true" +
                L",\"status\":\"committed\"" +
                L",\"bytes\":" + std::to_wstring(bytes) +
                L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
                L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
                L",\"sha256\":" + JsonString(sha256) +
                L",\"blob_handle\":" + JsonString(blobHandle) +
                L",\"manifest_path\":" + JsonString(manifestPath.lexically_relative(root).wstring()) +
                workerProvenanceJson +
                L"}";
        }

        if (fs::exists(metadataPath))
        {
            auto metadata = ReadJsonObjectFileLocal(metadataPath, "artifact.metadata_invalid");
            auto payloadPath = ArtifactPayloadPath(root, artifactId);
            auto stagedBytes = fs::exists(payloadPath) ? static_cast<uint64_t>(fs::file_size(payloadPath)) : 0;
            auto expectedBytesKnown = metadata.GetNamedBoolean(L"expected_bytes_known", metadata.HasKey(L"expected_bytes"));
            auto expectedBytes = GetOptionalUInt64Local(metadata, L"expected_bytes", 0);
            auto expectedSha256 = NormalizeOptionalSha256Local(GetOptionalStringLocal(metadata, L"expected_sha256"));
            auto artifactKind = NormalizeArtifactKind(GetOptionalStringLocal(metadata, L"artifact_kind", L"generic"));
            return OkBaseLocal(L"get_artifact_status", config) +
                L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
                L",\"artifact_id\":" + JsonString(artifactId) +
                L",\"artifact_kind\":" + JsonString(artifactKind) +
                L",\"exists\":true" +
                L",\"status\":\"staging\"" +
                L",\"staged_bytes\":" + std::to_wstring(stagedBytes) +
                L",\"expected_bytes_known\":" + BoolJsonLocal(expectedBytesKnown) +
                L",\"expected_bytes\":" + std::to_wstring(expectedBytes) +
                L",\"expected_sha256\":" + JsonString(expectedSha256) +
                L",\"staging_path\":" + JsonString(ArtifactStagingDirectory(root, artifactId).lexically_relative(root).wstring()) +
                L"}";
        }

        return OkBaseLocal(L"get_artifact_status", config) +
            L",\"schema_version\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(artifactId) +
            L",\"exists\":false" +
            L",\"status\":\"missing\"" +
            L"}";
    }
}
