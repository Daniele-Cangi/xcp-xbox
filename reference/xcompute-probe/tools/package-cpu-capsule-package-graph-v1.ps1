[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$CapsuleVersion = "1.0.0.0",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [ValidateSet("x64")][string]$Platform = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
& (Join-Path $PSScriptRoot "assert-non-onedrive-workspace.ps1") -RepositoryRoot $root | Out-Null
$projectDir = Join-Path $root "src\XComputeCpuCapsule"
$project = Join-Path $projectDir "XComputeCpuCapsule.vcxproj"
$manifestSource = Join-Path $projectDir "Package.appxmanifest"
$artifacts = Join-Path $root "artifacts"
$releaseDir = Join-Path $artifacts "releases"
$resultDir = Join-Path $artifacts "results"
$layout = Join-Path $artifacts "cpu-capsule-package-graph-v1\$CapsuleVersion\layout"

function Find-Tool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $roots = @(
        (Join-Path $programFilesX86 "Windows Kits\10\bin"),
        (Join-Path $programFiles "Microsoft Visual Studio"),
        (Join-Path $programFilesX86 "Microsoft Visual Studio")
    )
    foreach ($searchRoot in $roots) {
        if (-not (Test-Path -LiteralPath $searchRoot)) { continue }
        $candidate = Get-ChildItem -LiteralPath $searchRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
            Sort-Object @{ Expression = { if ($_.FullName -match '\\x64\\') { 0 } else { 1 } } }, FullName -Descending |
            Select-Object -First 1
        if ($candidate) { return $candidate.FullName }
    }
    return $null
}

$parts = @($CapsuleVersion.Split('.') | ForEach-Object { [UInt16]::Parse($_) })
if ($parts.Count -ne 4 -or $parts[0] -eq 0) { throw "CapsuleVersion must be a nonzero four-part MSIX version." }
$msbuild = Find-Tool "MSBuild.exe"
$makeappx = Find-Tool "makeappx.exe"
if (-not $msbuild) { throw "MSBuild.exe not found." }
if (-not $makeappx) { throw "makeappx.exe not found." }

$arguments = @(
    $project, "/m", "/restore", "/t:Rebuild",
    "/p:Configuration=$Configuration", "/p:Platform=$Platform",
    "/p:CpuCapsuleVersionMajor=$($parts[0])", "/p:CpuCapsuleVersionMinor=$($parts[1])",
    "/p:CpuCapsuleVersionBuild=$($parts[2])", "/p:CpuCapsuleVersionRevision=$($parts[3])"
)
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) { throw "CPU capsule build failed with exit code $LASTEXITCODE." }

$dll = Join-Path $projectDir "$Platform\$Configuration\XComputeCpuCapsuleV1.dll"
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "CPU capsule DLL not found: $dll" }
$artifactsFull = [IO.Path]::GetFullPath($artifacts).TrimEnd([IO.Path]::DirectorySeparatorChar)
$layoutFull = [IO.Path]::GetFullPath($layout)
if (-not $layoutFull.StartsWith($artifactsFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe capsule layout path." }
if (Test-Path -LiteralPath $layout) { Remove-Item -LiteralPath $layout -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $layout "Assets"), $releaseDir, $resultDir | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $layout "XComputeCpuCapsuleV1.dll")
Copy-Item -LiteralPath (Join-Path $root "src\XComputeProbe\Assets\StoreLogo.png") -Destination (Join-Path $layout "Assets\StoreLogo.png")
[xml]$manifest = Get-Content -LiteralPath $manifestSource
$manifest.Package.Identity.Version = $CapsuleVersion
if ([string]$manifest.Package.Properties.Framework -ne "true") { throw "CPU capsule manifest must declare Framework=true." }
if ($manifest.GetElementsByTagName("Capability").Count -ne 0) { throw "CPU capsule framework must not declare capabilities." }
$manifestPath = Join-Path $layout "AppxManifest.xml"
$manifest.Save($manifestPath)

$packagePath = Join-Path $releaseDir "XComputeCpuCapsule.Framework_${CapsuleVersion}_${Platform}.msix"
if (Test-Path -LiteralPath $packagePath) { Remove-Item -LiteralPath $packagePath -Force }
& $makeappx pack /o /d $layout /p $packagePath
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }
& (Join-Path $PSScriptRoot "sign-package.ps1") -PackagePath $packagePath -Publisher "CN=LocalDev"

$report = [pscustomobject][ordered]@{
    schema_version = "cpu-capsule-package-graph-local-package-v1"
    gate_id = "CPU_CAPSULE_PACKAGE_GRAPH_V1"
    capsule_version = $CapsuleVersion
    package_path = $packagePath
    package_bytes = [UInt64](Get-Item -LiteralPath $packagePath).Length
    package_sha256 = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    module_name = "XComputeCpuCapsuleV1.dll"
    module_bytes = [UInt64](Get-Item -LiteralPath $dll).Length
    module_sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    framework = $true
    capabilities = @()
    publisher = "CN=LocalDev"
    target_device_family = "Windows.Xbox"
    built_utc = [DateTime]::UtcNow.ToString("o")
}
$reportPath = Join-Path $resultDir "xcompute-cpu-capsule-package-graph-v1-$CapsuleVersion-local.json"
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
$report | ConvertTo-Json -Depth 10