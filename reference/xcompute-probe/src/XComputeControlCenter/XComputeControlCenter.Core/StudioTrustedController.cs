using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace XComputeControlCenter.Core;

public sealed record StudioTrustEnrollment(
    string ControllerId,
    string PublicKeySha256,
    int TrustedControllerCount,
    string AuthenticationMethod);

public sealed record StudioTrustStatus(
    bool Provisioned,
    string ControllerId,
    string PublicKeySha256);

public sealed class StudioTrustException : InvalidOperationException
{
    public StudioTrustException(
        string code,
        string message,
        string field,
        string expected,
        string actual,
        string correction,
        bool retryable = false)
        : base(message)
    {
        Code = code;
        Details = new StudioTrustErrorDetails(
            "xcp-studio-trust-error-details-v1",
            "trusted_controller",
            field,
            expected,
            actual,
            correction,
            retryable);
    }

    public string Code { get; }

    public StudioTrustErrorDetails Details { get; }
}

public sealed record StudioTrustErrorDetails(
    string SchemaVersion,
    string Stage,
    string Field,
    string Expected,
    string Actual,
    string Correction,
    bool Retryable);

public interface IStudioWorkerSessionLease : IAsyncDisposable
{
    string SessionId { get; }
}

public interface IStudioWorkerSessionProvider
{
    Task<IStudioWorkerSessionLease> OpenAsync(
        string deviceAddress,
        int port,
        double timeoutSeconds,
        CancellationToken cancellationToken = default);
}

public interface IStudioWorkerTrustBroker : IStudioWorkerSessionProvider
{
    StudioTrustStatus Status { get; }

    Task<StudioTrustEnrollment> EnrollAsync(
        string deviceAddress,
        string pairingCode,
        int port = 8787,
        double timeoutSeconds = 15,
        CancellationToken cancellationToken = default);
}

public sealed class StudioTrustedControllerBroker : IStudioWorkerTrustBroker
{
    public const string MetadataFileName = "trusted-controller.json";
    public const string MetadataSchema = "xcp-studio-trusted-controller-v1";
    public const string KeyName = "XCP-Studio-Trusted-Controller-v1";
    private const int KeySizeBits = 3072;
    private const int SessionTtlSeconds = 3600;
    private const int MaxResponseBytes = 1024 * 1024;
    private static readonly Regex PairingCodePattern = new(
        "^[0-9]{6}$",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);

    private readonly string _metadataPath;

    public StudioTrustedControllerBroker(string metadataRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(metadataRoot);
        _metadataPath = Path.Combine(
            Path.GetFullPath(metadataRoot),
            MetadataFileName);
    }

    public static StudioTrustedControllerBroker CreateDefault()
    {
        var localApplicationData = Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData);
        return new StudioTrustedControllerBroker(
            Path.Combine(localApplicationData, "XCP", "Studio", "trust"));
    }

    public StudioTrustStatus Status
    {
        get
        {
            if (!File.Exists(_metadataPath))
            {
                return new StudioTrustStatus(false, string.Empty, string.Empty);
            }

            try
            {
                using var identity = LoadIdentity();
                var metadata = identity.Metadata;
                return new StudioTrustStatus(
                    true,
                    metadata.ControllerId,
                    metadata.PublicKeySha256);
            }
            catch (StudioTrustException)
            {
                return new StudioTrustStatus(false, string.Empty, string.Empty);
            }
        }
    }

    public async Task<StudioTrustEnrollment> EnrollAsync(
        string deviceAddress,
        string pairingCode,
        int port = 8787,
        double timeoutSeconds = 15,
        CancellationToken cancellationToken = default)
    {
        ValidateEndpoint(deviceAddress, port, timeoutSeconds);
        if (!PairingCodePattern.IsMatch(pairingCode ?? string.Empty))
        {
            throw Invalid(
                "xcp.studio.pairing_code_invalid",
                "The first-trust pairing code is invalid.",
                "pairing_code",
                "exactly six decimal digits",
                string.IsNullOrEmpty(pairingCode) ? "empty" : "invalid",
                "read the current six-digit code from the Xbox worker and retry");
        }

        using var identity = LoadOrCreateIdentity();
        var response = await SendAsync(
            deviceAddress,
            port,
            timeoutSeconds,
            new Dictionary<string, object?>
            {
                ["command"] = "register_trusted_controller",
                ["pairing_code"] = pairingCode,
                ["controller_id"] = identity.Metadata.ControllerId,
                ["controller_label"] = identity.Metadata.ControllerLabel,
                ["public_key_spki_base64"] = identity.Metadata.PublicKeySpkiBase64,
            },
            cancellationToken).ConfigureAwait(false);
        RequireOk(response, "register_trusted_controller");
        var root = response.RootElement;
        var returnedHash = RequireString(root, "public_key_sha256");
        if (!string.Equals(
                returnedHash,
                identity.Metadata.PublicKeySha256,
                StringComparison.Ordinal))
        {
            throw Invalid(
                "xcp.studio.trusted_controller_identity_mismatch",
                "The Xbox registered a different controller identity.",
                "public_key_sha256",
                identity.Metadata.PublicKeySha256,
                returnedHash,
                "remove the unexpected controller registration and retry");
        }
        RequireFalse(root, "xbox_stores_private_key");
        RequireFalse(root, "xbox_stores_pairing_code");
        var count = root.TryGetProperty("trusted_controller_count", out var value) &&
                    value.TryGetInt32(out var parsed)
            ? parsed
            : 0;
        return new StudioTrustEnrollment(
            identity.Metadata.ControllerId,
            returnedHash,
            count,
            "pairing_code_first_trust");
    }

    public async Task<IStudioWorkerSessionLease> OpenAsync(
        string deviceAddress,
        int port,
        double timeoutSeconds,
        CancellationToken cancellationToken = default)
    {
        ValidateEndpoint(deviceAddress, port, timeoutSeconds);
        using var identity = LoadIdentity();
        var nonceBytes = RandomNumberGenerator.GetBytes(24);
        var nonce = Convert.ToBase64String(nonceBytes)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');
        var timestamp = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        var canonical =
            $"xcompute-worker-trust-v1\n{identity.Metadata.ControllerId}\n" +
            $"{nonce}\n{timestamp}\n{SessionTtlSeconds}";
        var signature = identity.Rsa.SignData(
            Encoding.UTF8.GetBytes(canonical),
            HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1);
        var response = await SendAsync(
            deviceAddress,
            port,
            timeoutSeconds,
            new Dictionary<string, object?>
            {
                ["command"] = "open_session_with_trust",
                ["controller_id"] = identity.Metadata.ControllerId,
                ["nonce"] = nonce,
                ["timestamp_unix_seconds"] = timestamp,
                ["ttl_seconds"] = SessionTtlSeconds,
                ["signature_base64"] = Convert.ToBase64String(signature),
            },
            cancellationToken).ConfigureAwait(false);
        RequireOk(response, "open_session_with_trust");
        var root = response.RootElement;
        var sessionId = RequireString(root, "session_id");
        var authMethod = RequireString(root, "auth_method");
        var returnedHash = RequireString(root, "public_key_sha256");
        if (!string.Equals(
                authMethod,
                "trusted_controller_signature",
                StringComparison.Ordinal) ||
            !ReadBoolean(root, "signature_verified") ||
            !string.Equals(
                returnedHash,
                identity.Metadata.PublicKeySha256,
                StringComparison.Ordinal))
        {
            throw Invalid(
                "xcp.studio.trusted_session_invalid",
                "The Xbox did not verify the trusted controller session.",
                "trusted_session",
                "trusted_controller_signature with exact public key",
                $"{authMethod}/{returnedHash}",
                "repeat first trust with the current Xbox worker");
        }
        RequireFalse(root, "xbox_stores_private_key");
        RequireFalse(root, "xbox_stores_pairing_code");
        return new StudioWorkerSessionLease(
            deviceAddress,
            port,
            timeoutSeconds,
            sessionId);
    }

    private TrustedIdentity LoadOrCreateIdentity()
    {
        if (!OperatingSystem.IsWindows())
        {
            throw PlatformUnsupported();
        }
        var hasMetadata = File.Exists(_metadataPath);
        var hasKey = CngKey.Exists(
            KeyName,
            CngProvider.MicrosoftSoftwareKeyStorageProvider,
            CngKeyOpenOptions.UserKey);
        if (hasMetadata != hasKey)
        {
            throw Invalid(
                "xcp.studio.trusted_controller_store_incomplete",
                "The local trusted-controller store is incomplete.",
                "trust_store",
                "metadata and Windows user key both present or both absent",
                $"metadata={hasMetadata},key={hasKey}",
                "reset the local Studio trust store before enrolling again");
        }
        if (hasMetadata)
        {
            return LoadIdentity();
        }

        var parent = Path.GetDirectoryName(_metadataPath)!;
        Directory.CreateDirectory(parent);
        if ((File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0)
        {
            throw Invalid(
                "xcp.studio.trusted_controller_store_invalid",
                "The trusted-controller directory is a reparse point.",
                "trust_store",
                "ordinary local directory",
                parent,
                "select a normal local user profile location");
        }
        var parameters = new CngKeyCreationParameters
        {
            Provider = CngProvider.MicrosoftSoftwareKeyStorageProvider,
            ExportPolicy = CngExportPolicies.None,
            KeyUsage = CngKeyUsages.Signing,
            KeyCreationOptions = CngKeyCreationOptions.None,
        };
        parameters.Parameters.Add(
            new CngProperty(
                "Length",
                BitConverter.GetBytes(KeySizeBits),
                CngPropertyOptions.None));
        using var key = CngKey.Create(CngAlgorithm.Rsa, KeyName, parameters);
        using var rsa = new RSACng(key);
        var publicKey = rsa.ExportSubjectPublicKeyInfo();
        var publicHash = Convert.ToHexString(SHA256.HashData(publicKey))
            .ToLowerInvariant();
        var controllerId = $"studio-{publicHash[..24]}";
        var label = $"{Environment.MachineName} XCP Studio";
        var metadata = new TrustedControllerMetadata(
            MetadataSchema,
            controllerId,
            label,
            KeyName,
            "RSASIGN_PKCS1_SHA256",
            KeySizeBits,
            Convert.ToBase64String(publicKey),
            publicHash);
        var temporary = _metadataPath + $".tmp-{Environment.ProcessId}";
        File.WriteAllBytes(
            temporary,
            JsonSerializer.SerializeToUtf8Bytes(
                metadata,
                new JsonSerializerOptions
                {
                    PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
                    WriteIndented = true,
                }));
        File.Move(temporary, _metadataPath, false);
        return LoadIdentity();
    }

    private TrustedIdentity LoadIdentity()
    {
        if (!OperatingSystem.IsWindows())
        {
            throw PlatformUnsupported();
        }
        var metadata = ReadMetadata();
        if (!CngKey.Exists(
                metadata.KeyName,
                CngProvider.MicrosoftSoftwareKeyStorageProvider,
                CngKeyOpenOptions.UserKey))
        {
            throw Invalid(
                "xcp.studio.trusted_controller_key_missing",
                "The Windows user key for XCP Studio is missing.",
                "key_name",
                metadata.KeyName,
                "missing",
                "reset local trust and enroll this PC again");
        }
        var key = CngKey.Open(
            metadata.KeyName,
            CngProvider.MicrosoftSoftwareKeyStorageProvider,
            CngKeyOpenOptions.UserKey);
        var rsa = new RSACng(key);
        var publicKey = rsa.ExportSubjectPublicKeyInfo();
        var actualHash = Convert.ToHexString(SHA256.HashData(publicKey))
            .ToLowerInvariant();
        var actualSpki = Convert.ToBase64String(publicKey);
        if (!string.Equals(
                actualHash,
                metadata.PublicKeySha256,
                StringComparison.Ordinal) ||
            !string.Equals(
                actualSpki,
                metadata.PublicKeySpkiBase64,
                StringComparison.Ordinal))
        {
            rsa.Dispose();
            throw Invalid(
                "xcp.studio.trusted_controller_key_mismatch",
                "The Windows user key differs from Studio trust metadata.",
                "public_key_sha256",
                metadata.PublicKeySha256,
                actualHash,
                "reset local trust and enroll this PC again");
        }
        return new TrustedIdentity(metadata, rsa);
    }

    private TrustedControllerMetadata ReadMetadata()
    {
        JsonDocument document;
        try
        {
            if ((File.GetAttributes(_metadataPath) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException("reparse point");
            }
            document = JsonDocument.Parse(File.ReadAllBytes(_metadataPath));
        }
        catch (Exception exception)
            when (exception is IOException or UnauthorizedAccessException or
                  JsonException)
        {
            throw Invalid(
                "xcp.studio.trusted_controller_metadata_invalid",
                "The local trusted-controller metadata is missing or invalid.",
                "metadata",
                "strict local xcp-studio-trusted-controller-v1 JSON",
                exception.GetType().Name,
                "reset local trust and enroll this PC again");
        }
        using (document)
        {
            var root = document.RootElement;
            var schema = RequireString(root, "schema_version");
            var keyName = RequireString(root, "key_name");
            var algorithm = RequireString(root, "algorithm");
            var keySize = root.TryGetProperty("key_size_bits", out var size) &&
                          size.TryGetInt32(out var parsed)
                ? parsed
                : 0;
            var metadata = new TrustedControllerMetadata(
                schema,
                RequireString(root, "controller_id"),
                RequireString(root, "controller_label"),
                keyName,
                algorithm,
                keySize,
                RequireString(root, "public_key_spki_base64"),
                RequireString(root, "public_key_sha256"));
            if (schema != MetadataSchema || keyName != KeyName ||
                algorithm != "RSASIGN_PKCS1_SHA256" || keySize != KeySizeBits ||
                !Regex.IsMatch(
                    metadata.ControllerId,
                    "^[A-Za-z0-9_-]{1,64}$",
                    RegexOptions.CultureInvariant) ||
                !Regex.IsMatch(
                    metadata.PublicKeySha256,
                    "^[0-9a-f]{64}$",
                    RegexOptions.CultureInvariant))
            {
                throw Invalid(
                    "xcp.studio.trusted_controller_metadata_invalid",
                    "The local trusted-controller metadata identity is invalid.",
                    "metadata",
                    $"{MetadataSchema}/{KeyName}/{KeySizeBits}",
                    $"{schema}/{keyName}/{keySize}",
                    "reset local trust and enroll this PC again");
            }
            return metadata;
        }
    }

    private sealed record TrustedControllerMetadata(
        string SchemaVersion,
        string ControllerId,
        string ControllerLabel,
        string KeyName,
        string Algorithm,
        int KeySizeBits,
        string PublicKeySpkiBase64,
        string PublicKeySha256);

    private sealed class TrustedIdentity(
        TrustedControllerMetadata metadata,
        RSA rsa) : IDisposable
    {
        public TrustedControllerMetadata Metadata { get; } = metadata;
        public RSA Rsa { get; } = rsa;
        public void Dispose() => Rsa.Dispose();
    }

    private sealed class StudioWorkerSessionLease(
        string deviceAddress,
        int port,
        double timeoutSeconds,
        string sessionId) : IStudioWorkerSessionLease
    {
        private string _sessionId = sessionId;
        public string SessionId => _sessionId;

        public async ValueTask DisposeAsync()
        {
            var current = Interlocked.Exchange(ref _sessionId, string.Empty);
            if (string.IsNullOrEmpty(current))
            {
                return;
            }
            try
            {
                using var response = await SendAsync(
                    deviceAddress,
                    port,
                    Math.Min(timeoutSeconds, 15),
                    new Dictionary<string, object?>
                    {
                        ["command"] = "close_session",
                        ["session_id"] = current,
                    },
                    CancellationToken.None).ConfigureAwait(false);
            }
            catch
            {
                // Session expiry is bounded to one hour; never retain the id.
            }
        }
    }

    private static async Task<JsonDocument> SendAsync(
        string deviceAddress,
        int port,
        double timeoutSeconds,
        IReadOnlyDictionary<string, object?> request,
        CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(timeoutSeconds));
        try
        {
            using var client = new TcpClient();
            await client.ConnectAsync(
                deviceAddress,
                port,
                timeout.Token).ConfigureAwait(false);
            await using var stream = client.GetStream();
            var encoded = JsonSerializer.SerializeToUtf8Bytes(request);
            await stream.WriteAsync(encoded, timeout.Token).ConfigureAwait(false);
            await stream.WriteAsync("\n"u8.ToArray(), timeout.Token)
                .ConfigureAwait(false);
            await stream.FlushAsync(timeout.Token).ConfigureAwait(false);
            using var memory = new MemoryStream();
            var buffer = new byte[8192];
            while (true)
            {
                var read = await stream.ReadAsync(buffer, timeout.Token)
                    .ConfigureAwait(false);
                if (read == 0)
                {
                    break;
                }
                var newline = Array.IndexOf(buffer, (byte)'\n', 0, read);
                var count = newline >= 0 ? newline : read;
                memory.Write(buffer, 0, count);
                if (memory.Length > MaxResponseBytes)
                {
                    throw Invalid(
                        "xcp.studio.worker_response_too_large",
                        "The trust-bootstrap response exceeded its limit.",
                        "response_bytes",
                        $"at most {MaxResponseBytes}",
                        memory.Length.ToString(),
                        "verify the exact worker package");
                }
                if (newline >= 0)
                {
                    break;
                }
            }
            if (memory.Length == 0)
            {
                throw Invalid(
                    "xcp.studio.worker_response_empty",
                    "The worker returned no trust-bootstrap response.",
                    "response",
                    "one JSON line",
                    "empty",
                    "launch the exact worker and retry",
                    true);
            }
            return JsonDocument.Parse(memory.ToArray());
        }
        catch (StudioTrustException)
        {
            throw;
        }
        catch (Exception exception)
            when (exception is SocketException or IOException or
                  OperationCanceledException or JsonException)
        {
            throw Invalid(
                "xcp.studio.worker_trust_transport_failed",
                "The worker trust endpoint could not be reached or parsed.",
                "endpoint",
                "reachable exact worker returning one JSON line",
                exception.GetType().Name,
                "launch the worker, verify its address, then retry",
                true);
        }
    }

    private static void RequireOk(JsonDocument response, string command)
    {
        var root = response.RootElement;
        if (root.TryGetProperty("ok", out var ok) &&
            ok.ValueKind == JsonValueKind.True)
        {
            return;
        }
        var code = "worker.unknown";
        var message = "The worker rejected the trusted-controller operation.";
        var correction = "inspect the worker and retry the exact operation";
        if (root.TryGetProperty("error", out var error) &&
            error.ValueKind == JsonValueKind.Object)
        {
            code = OptionalString(error, "code") ?? code;
            message = OptionalString(error, "message") ?? message;
            if (error.TryGetProperty("details", out var details) &&
                details.ValueKind == JsonValueKind.Object)
            {
                correction = OptionalString(details, "correction") ?? correction;
            }
        }
        throw Invalid(
            code,
            message,
            command,
            "ok=true",
            "rejected",
            correction);
    }

    private static void RequireFalse(JsonElement value, string name)
    {
        if (!value.TryGetProperty(name, out var property) ||
            property.ValueKind != JsonValueKind.False)
        {
            throw Invalid(
                "xcp.studio.trusted_session_boundary_invalid",
                "The worker trust response changed its secret-storage boundary.",
                name,
                "false",
                property.ValueKind.ToString(),
                "reject the worker package");
        }
    }

    private static bool ReadBoolean(JsonElement value, string name) =>
        value.TryGetProperty(name, out var property) &&
        property.ValueKind == JsonValueKind.True;

    private static string RequireString(JsonElement value, string name)
    {
        var result = OptionalString(value, name);
        if (string.IsNullOrWhiteSpace(result))
        {
            throw Invalid(
                "xcp.studio.worker_trust_response_invalid",
                "The worker trust response is missing a required field.",
                name,
                "non-empty string",
                "missing",
                "verify the exact worker package");
        }
        return result;
    }

    private static string? OptionalString(JsonElement value, string name) =>
        value.TryGetProperty(name, out var property) &&
        property.ValueKind == JsonValueKind.String
            ? property.GetString()
            : null;

    private static void ValidateEndpoint(
        string deviceAddress,
        int port,
        double timeoutSeconds)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(deviceAddress);
        if (port is < 1 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(port));
        }
        if (timeoutSeconds is <= 0 or > 300)
        {
            throw new ArgumentOutOfRangeException(nameof(timeoutSeconds));
        }
    }

    private static StudioTrustException PlatformUnsupported() =>
        Invalid(
            "xcp.studio.trusted_controller_platform_unsupported",
            "The Studio trusted-controller store requires Windows.",
            "platform",
            "Windows user CNG key store",
            Environment.OSVersion.Platform.ToString(),
            "run the Windows x64 Technical Preview");

    private static StudioTrustException Invalid(
        string code,
        string message,
        string field,
        string expected,
        string actual,
        string correction,
        bool retryable = false) =>
        new(code, message, field, expected, actual, correction, retryable);
}
