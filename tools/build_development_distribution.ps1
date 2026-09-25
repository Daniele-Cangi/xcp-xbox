[CmdletBinding()]
param(
    [string]$OutputRoot = 'build/development-distribution-run',
    [string]$WorkerVersion = '0.1.182.0',
    [string]$CapsuleVersion = '1.3.0.0',
    [string]$PreviousManifest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repository
$run = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $run) { throw 'Select a fresh development distribution output root' }
$parent = Split-Path -Parent $run
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw 'Output parent missing' }
New-Item -ItemType Directory -Path $run | Out-Null

function Invoke-Checked([scriptblock]$Action) {
    $global:LASTEXITCODE = 0
    & $Action
    if (-not $? -or $LASTEXITCODE -ne 0) { throw "Development build command failed: $LASTEXITCODE" }
}
function Invoke-CheckedScript([scriptblock]$Action) {
    $global:LASTEXITCODE = 0
    & $Action
    if (-not $?) { throw 'Development PowerShell build step failed' }
    # The frozen build can run a benign command that leaves LASTEXITCODE set;
    # its own native build failures are raised explicitly inside the script.
    $global:LASTEXITCODE = 0
}

$hostEnvironment = Join-Path $run 'host-python'
Invoke-Checked { python -m venv $hostEnvironment }
$hostPython = Join-Path $hostEnvironment 'Scripts/python.exe'
Invoke-Checked { & $hostPython -m pip install -e '.[test]' }
$env:Path = "$(Join-Path $hostEnvironment 'Scripts');$env:Path"

$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio installation locator missing' }
$installation = & $vswhere -latest -products '*' -property installationPath
if (-not $installation) { throw 'Visual Studio installation missing' }
$msbuild = Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe'
$toolsets = @(Get-ChildItem -LiteralPath (Join-Path $installation 'MSBuild/Microsoft/VC') -Recurse -Directory -Filter v145 -ErrorAction SilentlyContinue)
if (-not (Test-Path -LiteralPath $msbuild) -or $toolsets.Count -eq 0) { throw 'MSVC v145 missing' }
$sdkBin = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits/10/bin'
$makeappx = Get-ChildItem -LiteralPath $sdkBin -Recurse -File -Filter makeappx.exe | Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
$signtool = Get-ChildItem -LiteralPath $sdkBin -Recurse -File -Filter signtool.exe | Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $makeappx -or -not $signtool) { throw 'Windows packaging tools missing' }

$workerStage = Join-Path $run 'worker-stage'
Invoke-Checked { python tools/check_reference_worker_graph.py }
Invoke-Checked { python tools/prepare_reference_worker.py --output $workerStage }
$identityArgs = @('--stage', $workerStage, '--worker-version', $WorkerVersion, '--capsule-version', $CapsuleVersion)
if ($PreviousManifest) { $identityArgs += @('--previous-manifest', $PreviousManifest) }
Invoke-Checked { python tools/stage_development_package_identities.py @identityArgs }
Invoke-CheckedScript { & (Join-Path $workerStage 'tools/build.ps1') -Configuration Release -ManifestFlavor worker-prototype-0181 }
Invoke-CheckedScript { & tools/package_reference_capsule_unsigned.ps1 -StageRoot $workerStage -MSBuildPath $msbuild -MakeAppxPath $makeappx.FullName -Development }
$workerPackages = @(Get-ChildItem -LiteralPath (Join-Path $workerStage 'src/XComputeProbe/AppPackages') -Recurse -File -Filter '*.msix' | Where-Object { $_.Name.Contains($WorkerVersion) })
if ($workerPackages.Count -ne 1) { throw "Expected one development worker MSIX, found $($workerPackages.Count)" }
$workerPackage = $workerPackages[0].FullName
$capsulePackage = Join-Path $workerStage "artifacts/public-development/XCP.Development.CpuCapsule.Framework_${CapsuleVersion}_x64_unsigned.msix"
Invoke-Checked { python tools/check_development_packages.py --worker-package $workerPackage --capsule-package $capsulePackage --unsigned }
$signed = Join-Path $run 'signed-packages'
Invoke-CheckedScript { & tools/sign_reference_development_packages.ps1 -WorkerPackage $workerPackage -CapsulePackage $capsulePackage -SignToolPath $signtool.FullName -OutputDirectory $signed }
$signedWorker = Join-Path $signed "XCP.Development.Worker_${WorkerVersion}_x64_development.msix"
$signedCapsule = Join-Path $signed "XCP.Development.CpuCapsule.Framework_${CapsuleVersion}_x64_development.msix"
Invoke-Checked { python tools/check_development_packages.py --worker-package $signedWorker --capsule-package $signedCapsule }

$studioStage = Join-Path $run 'studio-stage'
Invoke-Checked { python tools/prepare_reference_studio.py --mode public-studio-1.9.1 --output $studioStage }
$studioTests = Join-Path $studioStage 'tests/winui/XComputeControlCenter.Core.Tests/XComputeControlCenter.Core.Tests.csproj'
$studioSolution = Join-Path $studioStage 'src/XComputeControlCenter/XComputeControlCenter.sln'
$studioProject = Join-Path $studioStage 'src/XComputeControlCenter/XComputeControlCenter/XComputeControlCenter.csproj'
Invoke-Checked { dotnet test $studioTests -c Release }
Invoke-Checked { dotnet build $studioSolution -c Release }
$publish = Join-Path $run 'studio-publish'
Invoke-Checked { dotnet publish $studioProject -c Release -o $publish }
$runtime = Join-Path $run 'studio-python'
$cache = Join-Path $run 'python-input-cache'
Invoke-Checked { python reference/xcompute-probe/tools/build_xcp_studio_python_runtime.py build --profile reference/xcompute-probe/profiles/studio/xcp-studio-python-runtime-v1.json --cache $cache --output $runtime }
Invoke-Checked { python reference/xcompute-probe/tools/build_xcp_studio_python_runtime.py verify --runtime $runtime }
$studio = Join-Path $run 'studio-folder'
Invoke-Checked { python tools/assemble_reference_studio.py --publish $publish --runtime $runtime --output $studio }

$vclibsSdk = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs'
if (-not (Test-Path -LiteralPath $vclibsSdk)) { throw 'Microsoft VCLibs Extension SDK missing' }
$vclibsCandidates = @(Get-ChildItem -LiteralPath $vclibsSdk -Recurse -File -Filter 'Microsoft.VCLibs.x64.14.00.appx' | Where-Object { $_.FullName -match '[\\/]Appx[\\/]Retail[\\/]x64[\\/]' })
$vclibs = $null
foreach ($candidate in $vclibsCandidates) {
    python tools/inspect_reference_vclibs.py --package $candidate.FullName | Out-Null
    if ($LASTEXITCODE -ne 0) { continue }
    $signature = Get-AuthenticodeSignature -LiteralPath $candidate.FullName
    if ($signature.Status -eq 'Valid') { $vclibs = $candidate.FullName; break }
}
if (-not $vclibs) { throw 'No valid signed VCLibs Retail x64 dependency in the SDK' }
$buildTools = Join-Path $run 'build-tools.json'
@{
    host_python = (& python --version 2>&1 | Out-String).Trim()
    dotnet_sdk = (& dotnet --version 2>&1 | Out-String).Trim()
    msbuild = (& $msbuild -version -nologo 2>&1 | Out-String).Trim()
    makeappx_file_version = $makeappx.VersionInfo.FileVersion
    signtool_file_version = $signtool.VersionInfo.FileVersion
    msvc_toolset = 'v145'
    msvc_toolset_path_version = $toolsets[0].Parent.Name
    windows_runner = [Environment]::OSVersion.Version.ToString()
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $buildTools -Encoding utf8
$distribution = Join-Path $run 'distribution'
$archive = Join-Path $run 'xcp-development-distribution.zip'
Invoke-Checked { python tools/assemble_development_distribution.py --studio $studio --signed $signed --vclibs-package $vclibs --build-tools $buildTools --output $distribution --archive $archive }
$extracted = Join-Path $run 'extracted'
Invoke-Checked { python tools/verify_development_distribution.py --archive $archive --extract $extracted }
Write-Output "PC development distribution verified from fresh extraction: $archive"
