[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet("Debug", "Release")][string]$Configuration = "Debug",
    [ValidateSet("x64")][string]$Platform = "x64",
    [ValidateSet("baseline", "codegen-candidate", "expanded-candidate", "gamebucket-candidate", "gamebucket-minimal", "gamebucket-native", "gamebucket-d3d11compute", "gamebucket-shaderruntime", "worker-prototype", "worker-prototype-0178", "worker-prototype-0179", "worker-prototype-0180", "worker-prototype-0181")][string]$ManifestFlavor = "baseline"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "assert-non-onedrive-workspace.ps1") -RepositoryRoot $root | Out-Null
$projectDir = Join-Path $root "src\XComputeProbe"
$project = Join-Path $projectDir "XComputeProbe.vcxproj"
$nativeModuleProject = Join-Path $root "src\XComputeProbeNativeModule\XComputeProbeNativeModule.vcxproj"
$logs = Join-Path $root "artifacts\logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null

function Find-MSBuild {
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    foreach ($base in @($programFiles, $programFilesX86)) {
        $rootPath = Join-Path $base "Microsoft Visual Studio"
        if (Test-Path $rootPath) {
            $candidate = Get-ChildItem $rootPath -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending |
                Select-Object -First 1
            if ($candidate) { return $candidate.FullName }
        }
    }
    return $null
}

function Invoke-MSBuildCleanEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$MSBuildPath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $MSBuildPath
    $startInfo.UseShellExecute = $false
    foreach ($argument in $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $startInfo.EnvironmentVariables.Clear()
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($key in [Environment]::GetEnvironmentVariables("Process").Keys) {
        $name = [string]$key
        if ($seen.Add($name)) {
            $value = [Environment]::GetEnvironmentVariable($name, "Process")
            if ($null -ne $value) {
                $startInfo.EnvironmentVariables[$name] = $value
            }
        }
    }
    if ($env:Path) {
        $startInfo.EnvironmentVariables["Path"] = $env:Path
    }
    if ($startInfo.EnvironmentVariables.ContainsKey("PATH")) {
        $startInfo.EnvironmentVariables.Remove("PATH")
    }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    return $process.ExitCode
}

$manifestMap = @{
    "baseline" = "Package.baseline.appxmanifest"
    "codegen-candidate" = "Package.codegen-candidate.appxmanifest"
    "expanded-candidate" = "Package.expanded-candidate.appxmanifest"
    "gamebucket-candidate" = "Package.gamebucket-candidate.appxmanifest"
    "gamebucket-minimal" = "Package.gamebucket-minimal.appxmanifest"
    "gamebucket-native" = "Package.gamebucket-native.appxmanifest"
    "gamebucket-d3d11compute" = "Package.gamebucket-d3d11compute.appxmanifest"
    "gamebucket-shaderruntime" = "Package.gamebucket-shaderruntime.appxmanifest"
    "worker-prototype" = "Package.worker-prototype.appxmanifest"
    "worker-prototype-0178" = "Package.worker-prototype-0178.appxmanifest"
    "worker-prototype-0179" = "Package.worker-prototype-0179.appxmanifest"
    "worker-prototype-0180" = "Package.worker-prototype-0180.appxmanifest"
    "worker-prototype-0181" = "Package.worker-prototype-0181.appxmanifest"
}
$selectedManifest = Join-Path $projectDir $manifestMap[$ManifestFlavor]
$compiledManifestFlavor = if ($ManifestFlavor -in @("worker-prototype-0178", "worker-prototype-0179", "worker-prototype-0180", "worker-prototype-0181")) {
    "worker-prototype"
} else {
    $ManifestFlavor
}
$activeManifest = Join-Path $projectDir "Package.appxmanifest"
if (-not (Test-Path $selectedManifest)) {
    throw "Manifest not found: $selectedManifest"
}

$msbuild = Find-MSBuild
if (-not $msbuild) {
    Write-Error "MSBuild not found. Install Visual Studio 2022 with UWP C++ workload and MSVC x64 tools."
    exit 2
}

if ($PSCmdlet.ShouldProcess($activeManifest, "copy $ManifestFlavor manifest")) {
    Copy-Item -LiteralPath $selectedManifest -Destination $activeManifest -Force
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$nativeLogPath = Join-Path $logs "build-native-module-$Configuration-$Platform-$stamp.log"
$logPath = Join-Path $logs "build-$ManifestFlavor-$Configuration-$Platform-$stamp.log"
$nativeArgs = @(
    $nativeModuleProject,
    "/m",
    "/restore",
    "/t:Rebuild",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/flp:logfile=$nativeLogPath;verbosity=normal"
)
$args = @(
    $project,
    "/m",
    "/restore",
    "/t:Rebuild",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:XComputeManifestFlavor=$compiledManifestFlavor",
    "/p:AppxBundle=Never",
    "/p:UapAppxPackageBuildMode=SideloadOnly",
    "/flp:logfile=$logPath;verbosity=normal"
)

Write-Host "MSBuild: $msbuild"
Write-Host "Native module log: $nativeLogPath"
Write-Host "Log: $logPath"
if (Test-Path $nativeModuleProject) {
    if ($PSCmdlet.ShouldProcess($nativeModuleProject, "build native module $Configuration|$Platform")) {
        $nativeExit = Invoke-MSBuildCleanEnvironment -MSBuildPath $msbuild -Arguments $nativeArgs
        if ($nativeExit -ne 0) {
            Write-Error "Native module build failed with exit code $nativeExit. See $nativeLogPath"
            exit $nativeExit
        }
    }
}
if ($PSCmdlet.ShouldProcess($project, "build $Configuration|$Platform using $ManifestFlavor")) {
    $exit = Invoke-MSBuildCleanEnvironment -MSBuildPath $msbuild -Arguments $args
    if ($exit -ne 0) {
        Write-Error "Build failed with exit code $exit. See $logPath"
        exit $exit
    }
}

Write-Host "Build command completed."
