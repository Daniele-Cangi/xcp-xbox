[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageRoot,
    [Parameter(Mandatory = $true)][string]$MSBuildPath,
    [Parameter(Mandatory = $true)][string]$MakeAppxPath,
    [switch]$Development
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $StageRoot).Path
$project = Join-Path $root 'src/XComputeCpuCapsule/XComputeCpuCapsule.vcxproj'
$sourceManifest = Join-Path $root 'src/XComputeCpuCapsule/Package.appxmanifest'
$logo = Join-Path $root 'src/XComputeProbe/Assets/StoreLogo.png'
foreach ($inputPath in @($project, $sourceManifest, $logo, $MSBuildPath, $MakeAppxPath)) {
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "Missing input: $inputPath" }
}
[xml]$manifest = Get-Content -LiteralPath $sourceManifest
$name = if ($Development) { 'XCP.Development.CpuCapsule.Framework' } else { 'XComputeProbe.CpuCapsule.Framework' }
if ([string]$manifest.Package.Identity.Name -ne $name -or
    [string]$manifest.Package.Identity.Publisher -ne 'CN=LocalDev' -or
    [string]$manifest.Package.Properties.Framework -ne 'true') {
    throw 'CPU Capsule staged manifest identity or framework contract changed.'
}
$capsuleVersion = if ($Development) { [string]$manifest.Package.Identity.Version } else { '1.3.0.0' }
if ($capsuleVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Invalid Capsule version' }
$parts = @($capsuleVersion.Split('.') | ForEach-Object { [int]$_ })
$layout = Join-Path $root "artifacts/public-development/capsule-layout-$capsuleVersion"
$package = Join-Path $root "artifacts/public-development/${name}_${capsuleVersion}_x64_unsigned.msix"
if ((Test-Path -LiteralPath $layout) -or (Test-Path -LiteralPath $package)) {
    throw 'Select a fresh worker staging directory; unsigned package output already exists.'
}

& $MSBuildPath $project /m /restore /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:CpuCapsuleVersionMajor=$($parts[0])" "/p:CpuCapsuleVersionMinor=$($parts[1])" "/p:CpuCapsuleVersionBuild=$($parts[2])" "/p:CpuCapsuleVersionRevision=$($parts[3])" /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw "CPU Capsule build failed: $LASTEXITCODE" }
$module = Join-Path $root 'src/XComputeCpuCapsule/x64/Release/XComputeCpuCapsuleV1.dll'
if (-not (Test-Path -LiteralPath $module -PathType Leaf)) { throw 'CPU Capsule module missing after build' }

New-Item -ItemType Directory -Path (Join-Path $layout 'Assets') -Force | Out-Null
Copy-Item -LiteralPath $module -Destination (Join-Path $layout 'XComputeCpuCapsuleV1.dll')
Copy-Item -LiteralPath $logo -Destination (Join-Path $layout 'Assets/StoreLogo.png')
$manifest.Package.Identity.Version = $capsuleVersion
$manifest.Save((Join-Path $layout 'AppxManifest.xml'))
& $MakeAppxPath pack /o /d $layout /p $package
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed: $LASTEXITCODE" }

[pscustomobject]@{
    schema_version = 'xcp-unsigned-capsule-development-build-v1'
    package_path = $package
    package_sha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    package_name = $name
    package_version = $capsuleVersion
    signing = 'UNSIGNED'
    hardware_validation = 'NOT_TESTED_ON_XBOX'
} | ConvertTo-Json -Compress
