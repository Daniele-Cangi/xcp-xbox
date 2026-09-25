[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [string]$Publisher = "CN=LocalDev"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$certArtifacts = Join-Path $root "artifacts\certs"
New-Item -ItemType Directory -Force -Path $certArtifacts | Out-Null

function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $kitBin = Join-Path $programFilesX86 "Windows Kits\10\bin"
    if (Test-Path $kitBin) {
        $candidate = Get-ChildItem $kitBin -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Sort-Object @{ Expression = { if ($_.FullName -match '\\x64\\') { 0 } elseif ($_.FullName -match '\\x86\\') { 1 } else { 2 } } }, FullName |
            Select-Object -First 1
        if ($candidate) { return $candidate.FullName }
    }
    return $null
}

function Find-OrCreate-CodeSigningCert {
    param([string]$Subject)

    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object {
            $_.Subject -eq $Subject -and
            $_.HasPrivateKey -and
            $_.NotAfter -gt (Get-Date)
        } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if ($cert) { return $cert }

    return New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -FriendlyName "XCompute Probe LocalDev Test Signing" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -HashAlgorithm SHA256 `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -NotAfter (Get-Date).AddYears(5)
}

if (-not (Test-Path -LiteralPath $PackagePath)) {
    throw "Package not found: $PackagePath"
}

$signTool = Find-SignTool
if (-not $signTool) {
    throw "signtool.exe not found. Install Windows SDK signing tools."
}

$resolvedPackage = (Resolve-Path -LiteralPath $PackagePath).Path
$cert = Find-OrCreate-CodeSigningCert -Subject $Publisher
$certPath = Join-Path $certArtifacts "LocalDev.cer"
Export-Certificate -Cert $cert -FilePath $certPath -Force | Out-Null

Write-Host "SignTool: $signTool"
Write-Host "Signing certificate: $($cert.Subject) $($cert.Thumbprint)"
Write-Host "Exported public certificate: $certPath"

& $signTool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $resolvedPackage
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE"
}

$signature = Get-AuthenticodeSignature -LiteralPath $resolvedPackage
if (-not $signature.SignerCertificate) {
    throw "Package signing did not produce a signer certificate."
}

Write-Host "Signed package: $resolvedPackage"
Write-Host "Signature status: $($signature.Status)"
