[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$WorkerPackage,
    [Parameter(Mandatory = $true)][string]$CapsulePackage,
    [Parameter(Mandatory = $true)][string]$SignToolPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$worker = (Resolve-Path -LiteralPath $WorkerPackage).Path
$capsule = (Resolve-Path -LiteralPath $CapsulePackage).Path
if (-not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) { throw 'SignTool missing' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Select a fresh output directory for development signing' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $output) -PathType Container)) {
    throw 'Development signing output parent is missing'
}

function Get-PackageIdentity([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        if ($null -ne $archive.GetEntry('AppxSignature.p7x')) {
            throw 'Input package already has a signature; refuse to replace it'
        }
        $entry = $archive.GetEntry('AppxManifest.xml')
        if ($null -eq $entry) { throw 'Package manifest missing' }
        $stream = $entry.Open()
        $reader = [IO.StreamReader]::new($stream)
        try { [xml]$manifest = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
        return $manifest.Package.Identity
    }
    finally { $archive.Dispose() }
}
$workerId = Get-PackageIdentity $worker
$capsuleId = Get-PackageIdentity $capsule
if ([string]$workerId.Name -ne 'XCP.Development.Worker' -or
    [string]$capsuleId.Name -ne 'XCP.Development.CpuCapsule.Framework' -or
    [string]$workerId.Publisher -ne 'CN=LocalDev' -or
    [string]$capsuleId.Publisher -ne 'CN=LocalDev') {
    throw 'Refusing to sign a historical or mismatched package family'
}

# The key exists only for this invocation. The distribution contains its public certificate.
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject 'CN=LocalDev' `
    -FriendlyName 'XCP reconstructed development packages' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -HashAlgorithm SHA256 `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -KeyExportPolicy NonExportable `
    -NotAfter (Get-Date).AddYears(1)
if (-not $certificate -or -not $certificate.HasPrivateKey) {
    throw 'Fresh development signing certificate was not created'
}
try {
    New-Item -ItemType Directory -Path $output | Out-Null
    $workerSigned = Join-Path $output "XCP.Development.Worker_$($workerId.Version)_x64_development.msix"
    $capsuleSigned = Join-Path $output "XCP.Development.CpuCapsule.Framework_$($capsuleId.Version)_x64_development.msix"
    Copy-Item -LiteralPath $worker -Destination $workerSigned
    Copy-Item -LiteralPath $capsule -Destination $capsuleSigned
    $publicCertificate = Join-Path $output 'XCP-Development-Public.cer'
    Export-Certificate -Cert $certificate -FilePath $publicCertificate | Out-Null
    foreach ($package in @($capsuleSigned, $workerSigned)) {
        & $SignToolPath sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $package
        if ($LASTEXITCODE -ne 0) { throw "Development signing failed: $package" }
        $signature = Get-AuthenticodeSignature -LiteralPath $package
        if (-not $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
            throw "Development signature identity mismatch: $package"
        }
    }
    $workerDigest = (Get-FileHash -LiteralPath $workerSigned -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($workerDigest -eq '0b7bf086f2423caefae876239a4ea394afa10346f15b0bf564469aed5d08d06c') {
        throw 'Development package unexpectedly matches the historical .181 worker digest'
    }
    $manifest = [ordered]@{
        schema_version = 'xcp-worker-development-packages-v2'
        hardware_validation = 'NOT_TESTED_ON_XBOX'
        historical_worker_identity = $false
        publisher = 'CN=LocalDev'
        certificate_thumbprint = $certificate.Thumbprint
        public_certificate_sha256 = (Get-FileHash -LiteralPath $publicCertificate -Algorithm SHA256).Hash.ToLowerInvariant()
        capsule = [ordered]@{
            name = [string]$capsuleId.Name
            file = [IO.Path]::GetFileName($capsuleSigned)
            version = [string]$capsuleId.Version
            sha256 = (Get-FileHash -LiteralPath $capsuleSigned -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        worker = [ordered]@{
            name = [string]$workerId.Name
            file = [IO.Path]::GetFileName($workerSigned)
            version = [string]$workerId.Version
            sha256 = $workerDigest
        }
        external_dependency = 'Microsoft.VCLibs.140.00 >=14.0.33519.0 from the Windows SDK'
    }
    $manifestPath = Join-Path $output 'xcp-worker-development-packages.json'
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $manifest | ConvertTo-Json -Depth 8
}
finally {
    $newCertificatePath = "Cert:\CurrentUser\My\$($certificate.Thumbprint)"
    if (Test-Path -LiteralPath $newCertificatePath) { Remove-Item -LiteralPath $newCertificatePath }
}
