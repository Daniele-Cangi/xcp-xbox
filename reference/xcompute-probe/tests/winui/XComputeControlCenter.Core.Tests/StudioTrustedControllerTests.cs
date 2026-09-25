using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class StudioTrustedControllerTests
{
    [Fact]
    public void EmptyStoreReportsNoProvisionedController()
    {
        var root = NewTemporaryRoot();
        try
        {
            var broker = new StudioTrustedControllerBroker(root);

            var status = broker.Status;

            Assert.False(status.Provisioned);
            Assert.Empty(status.ControllerId);
            Assert.Empty(status.PublicKeySha256);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("12345")]
    [InlineData("1234567")]
    [InlineData("12A456")]
    public async Task InvalidFirstTrustCodeFailsBeforeCreatingLocalKey(
        string pairingCode)
    {
        var root = NewTemporaryRoot();
        try
        {
            var broker = new StudioTrustedControllerBroker(root);

            var exception = await Assert.ThrowsAsync<StudioTrustException>(
                () => broker.EnrollAsync(
                    "127.0.0.1",
                    pairingCode,
                    timeoutSeconds: 1));

            Assert.Equal("xcp.studio.pairing_code_invalid", exception.Code);
            Assert.False(File.Exists(
                Path.Combine(
                    root,
                    StudioTrustedControllerBroker.MetadataFileName)));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void PublicTrustSurfaceContainsNoPairingOrSessionProperty()
    {
        var names = typeof(StudioTrustStatus)
            .GetProperties()
            .Concat(typeof(StudioTrustEnrollment).GetProperties())
            .Select(property => property.Name)
            .ToArray();

        Assert.DoesNotContain(
            names,
            name => name.Contains("Pairing", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(
            names,
            name => name.Contains("Session", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(
            names,
            name => name.Contains("Private", StringComparison.OrdinalIgnoreCase));
    }

    private static string NewTemporaryRoot()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-trust-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }
}
