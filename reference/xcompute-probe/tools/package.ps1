[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet("baseline", "codegen-candidate", "expanded-candidate", "gamebucket-candidate", "gamebucket-minimal", "gamebucket-native", "gamebucket-d3d11compute", "gamebucket-shaderruntime", "worker-prototype", "worker-prototype-0178", "worker-prototype-0179", "worker-prototype-0180", "worker-prototype-0181")][string]$ManifestFlavor = "baseline",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [ValidateSet("x64")][string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "assert-non-onedrive-workspace.ps1") -RepositoryRoot $root | Out-Null
$projectDir = Join-Path $root "src\XComputeProbe"
$artifacts = Join-Path $root "artifacts"
$logs = Join-Path $artifacts "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-PackageEntryBytes {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath,
        [Parameter(Mandatory = $true)][string]$EntryName
    )
    $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        $entry = $archive.GetEntry($EntryName)
        if ($null -eq $entry) { throw "Package entry '$EntryName' not found in $PackagePath" }
        $stream = $entry.Open()
        try {
            $memory = [IO.MemoryStream]::new()
            try {
                $stream.CopyTo($memory)
                return ,$memory.ToArray()
            }
            finally { $memory.Dispose() }
        }
        finally { $stream.Dispose() }
    }
    finally { $archive.Dispose() }
}

function Get-WinMdContract {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $stream = [IO.MemoryStream]::new($Bytes, $false)
    try {
        $peReader = [System.Reflection.PortableExecutable.PEReader]::new($stream)
        try {
            if (-not $peReader.HasMetadata) {
                throw "Packaged WinMD does not contain ECMA-335 metadata."
            }
            $metadataReader = [System.Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($peReader)
            if (-not $metadataReader.IsAssembly) {
                throw "Packaged WinMD metadata is not an assembly."
            }
            $assembly = $metadataReader.GetAssemblyDefinition()
            $module = $metadataReader.GetModuleDefinition()
            $typeNames = [Collections.Generic.List[string]]::new()
            foreach ($typeHandle in $metadataReader.TypeDefinitions) {
                $typeDefinition = $metadataReader.GetTypeDefinition($typeHandle)
                $typeNamespace = $metadataReader.GetString($typeDefinition.Namespace)
                $typeName = $metadataReader.GetString($typeDefinition.Name)
                if (-not [string]::IsNullOrWhiteSpace($typeNamespace)) {
                    [void]$typeNames.Add($typeNamespace + "." + $typeName)
                }
            }
            return [pscustomobject]@{
                assembly_name = $metadataReader.GetString($assembly.Name)
                module_name = $metadataReader.GetString($module.Name)
                windows_runtime = (([int]$assembly.Flags -band 0x0200) -eq 0x0200)
                type_names = @($typeNames)
            }
        }
        finally {
            $peReader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-PackagedFlavor {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath,
        [Parameter(Mandatory = $true)][string]$ExpectedIdentityName,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedManifestFlavor
    )
    $packagedManifestText = [Text.Encoding]::UTF8.GetString(
        (Get-PackageEntryBytes -PackagePath $PackagePath -EntryName "AppxManifest.xml"))
    [xml]$packagedManifest = $packagedManifestText.TrimStart([char]0xFEFF)
    $actualIdentityName = [string]$packagedManifest.Package.Identity.Name
    $actualVersion = [string]$packagedManifest.Package.Identity.Version
    if ($actualIdentityName -ne $ExpectedIdentityName -or $actualVersion -ne $ExpectedVersion) {
        throw "Package identity mismatch in $PackagePath expected $ExpectedIdentityName/$ExpectedVersion, found $actualIdentityName/$actualVersion"
    }
    if ($ExpectedManifestFlavor -eq "worker-prototype") {
        $packageDependencies = @($packagedManifest.GetElementsByTagName("PackageDependency"))
        $capsuleDependencies = @($packageDependencies | Where-Object {
            $_.GetAttribute("Name") -eq "XComputeProbe.CpuCapsule.Framework"
        })
        if ($capsuleDependencies.Count -ne 1) {
            throw "Worker-prototype package must contain exactly one XComputeProbe.CpuCapsule.Framework dependency, found $($capsuleDependencies.Count)."
        }
        $allowedVclibsNames = @("Microsoft.VCLibs.140.00", "Microsoft.VCLibs.140.00.Debug")
        $microsoftPublisher = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
        $vclibsDependencies = @($packageDependencies | Where-Object {
            ($allowedVclibsNames -contains $_.GetAttribute("Name")) -and
            $_.GetAttribute("Publisher") -eq $microsoftPublisher
        })
        if ($packageDependencies.Count -ne (1 + $vclibsDependencies.Count) -or
            $vclibsDependencies.Count -gt 1) {
            throw "Worker-prototype package contains an unexpected PackageDependency."
        }
        $capsuleDependency = $capsuleDependencies[0]
        if ($capsuleDependency.GetAttribute("Name") -ne "XComputeProbe.CpuCapsule.Framework" -or
            $capsuleDependency.GetAttribute("Publisher") -ne "CN=LocalDev" -or
            $capsuleDependency.GetAttribute("MinVersion") -ne "1.3.0.0" -or
            $capsuleDependency.GetAttribute("MaxMajorVersionTested") -ne "1") {
            throw "Worker-prototype package must bind XComputeProbe.CpuCapsule.Framework/CN=LocalDev with MinVersion=1.3.0.0 and MaxMajorVersionTested=1."
        }
        $appServices = @($packagedManifest.SelectNodes("//*[local-name()='AppService']"))
        $appServiceExtensions = @($packagedManifest.SelectNodes("//*[local-name()='Extension' and @Category='windows.appService']"))
        if ($appServices.Count -ne 1 -or $appServiceExtensions.Count -ne 1) {
            throw "Worker-prototype package must contain exactly one App Service extension."
        }
        if ($appServices[0].GetAttribute("Name") -ne "xcp.process.topology.probe.v1" -or
            $appServiceExtensions[0].GetAttribute("EntryPoint") -ne "XComputeTopologyBroker.BrokerTask" -or
            $appServiceExtensions[0].GetAttribute("ResourceGroup") -ne "xcp.process.topology.probe.v1") {
            throw "Worker-prototype App Service contract mismatch."
        }
        foreach ($forbiddenCapability in @("expandedResources", "codeGeneration", "runFullTrust", "broadFileSystemAccess")) {
            if (@($packagedManifest.SelectNodes("//*[local-name()='Capability' and @Name='$forbiddenCapability']")).Count -ne 0) {
                throw "Worker-prototype package contains forbidden capability $forbiddenCapability."
            }
        }
        $inProcessExtensions = @($packagedManifest.SelectNodes("//*[local-name()='Extension' and @Category='windows.activatableClass.inProcessServer']"))
        if ($inProcessExtensions.Count -ne 1) {
            throw "Worker-prototype package must contain exactly one in-process activatable-class extension."
        }
        $inProcessServers = @($inProcessExtensions[0].SelectNodes("./*[local-name()='InProcessServer']"))
        if ($inProcessServers.Count -ne 1) {
            throw "Worker-prototype package must contain exactly one in-process server registration."
        }
        $serverPaths = @($inProcessServers[0].SelectNodes("./*[local-name()='Path']"))
        $brokerClasses = @($inProcessServers[0].SelectNodes("./*[local-name()='ActivatableClass']"))
        if ($serverPaths.Count -ne 1 -or $serverPaths[0].InnerText.Trim() -ne "XComputeTopologyBroker.dll" -or
            $brokerClasses.Count -ne 1 -or
            $brokerClasses[0].GetAttribute("ActivatableClassId") -ne "XComputeTopologyBroker.BrokerTask" -or
            $brokerClasses[0].GetAttribute("ThreadingModel") -ne "both") {
            throw "Worker-prototype activatable-class registration mismatch."
        }

        [void](Get-PackageEntryBytes -PackagePath $PackagePath -EntryName "XComputeTopologyBroker.dll")
        $brokerWinMd = Get-WinMdContract -Bytes (Get-PackageEntryBytes -PackagePath $PackagePath -EntryName "XComputeTopologyBroker.winmd")
        $foreignBrokerTypes = @($brokerWinMd.type_names | Where-Object { $_ -notlike "XComputeTopologyBroker.*" })
        if ($brokerWinMd.assembly_name -ne "XComputeTopologyBroker" -or
            $brokerWinMd.module_name -ne "XComputeTopologyBroker.winmd" -or
            -not $brokerWinMd.windows_runtime -or
            $brokerWinMd.type_names -notcontains "XComputeTopologyBroker.BrokerTask" -or
            $foreignBrokerTypes.Count -ne 0) {
            throw "Worker-prototype WinMD identity, module, namespace, or runtime metadata mismatch."
        }
    }
    $expectedCompiledFlavorMarker = "XCOMPUTE_COMPILED_MANIFEST_FLAVOR=$ExpectedManifestFlavor"
    $packagedExecutableText = [Text.Encoding]::Unicode.GetString(
        (Get-PackageEntryBytes -PackagePath $PackagePath -EntryName "XComputeProbe.exe"))
    if ($packagedExecutableText.IndexOf($expectedCompiledFlavorMarker, [StringComparison]::Ordinal) -lt 0) {
        throw "Compiled manifest flavor marker mismatch in $PackagePath expected '$expectedCompiledFlavorMarker'"
    }
    Write-Host "Verified package identity and compiled flavor: $ExpectedIdentityName $ExpectedVersion $ExpectedManifestFlavor"
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

$restricted = @("expandedResources", "codeGeneration", "runFullTrust", "broadFileSystemAccess")
$manifest = Join-Path $projectDir $manifestMap[$ManifestFlavor]
$compiledManifestFlavor = if ($ManifestFlavor -in @("worker-prototype-0178", "worker-prototype-0179", "worker-prototype-0180", "worker-prototype-0181")) {
    "worker-prototype"
} else {
    $ManifestFlavor
}
if (-not (Test-Path $manifest)) {
    throw "Manifest not found: $manifest"
}

[xml]$manifestXml = Get-Content -LiteralPath $manifest
$capabilityNames = @($manifestXml.GetElementsByTagName("Capability") | ForEach-Object { $_.GetAttribute("Name") })
if ($ManifestFlavor -eq "baseline") {
    $leaks = @($capabilityNames | Where-Object { $_ -in $restricted })
    if ($leaks.Count -gt 0) {
        throw "Baseline manifest contains restricted capabilities: $($leaks -join ', ')"
    }
}

$buildScript = Join-Path $PSScriptRoot "build.ps1"
if ($PSCmdlet.ShouldProcess($buildScript, "build package flavor $ManifestFlavor")) {
    & $buildScript -Configuration $Configuration -Platform $Platform -ManifestFlavor $ManifestFlavor
    if (-not $?) {
        Write-Error "Build script failed."
        exit 1
    }
}

$expectedIdentityName = [string]$manifestXml.Package.Identity.Name
$expectedVersion = [string]$manifestXml.Package.Identity.Version
$expectedLayoutPrefix = "XComputeProbe_$($expectedVersion)_$($Platform)"
$expectedPackageBaseName = "XComputeProbe_$($expectedVersion)_$($Platform)"
$packageRoots = @(
    (Join-Path $projectDir "AppPackages"),
    (Join-Path (Join-Path (Join-Path $projectDir $Platform) $Configuration) "AppPackages")
) | Where-Object { Test-Path $_ }

$layoutPattern = if ($Configuration -eq "Debug") {
    "*_$($Platform)_Debug_Test"
} else {
    "*_$($Platform)_Test"
}

$layoutRoots = @()
foreach ($packageRoot in $packageRoots) {
    $layoutRoots += Get-ChildItem $packageRoot -Recurse -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like $layoutPattern -and $_.Name -like "$expectedLayoutPrefix*" }
}

$packages = @()
foreach ($layoutRoot in $layoutRoots) {
    $packages += Get-ChildItem $layoutRoot.FullName -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".appx", ".msix", ".appxbundle", ".msixbundle") }
}

if ($packages.Count -eq 0) {
    Write-Error "No package artifacts found for $expectedIdentityName $expectedVersion after build. Check MSBuild output under artifacts/logs."
    exit 3
}

$signScript = Join-Path $PSScriptRoot "sign-package.ps1"
$ownPackages = @($packages | Where-Object {
    $_.FullName -notmatch '\\Dependencies\\' -and
    $_.BaseName -eq $expectedPackageBaseName
})
if ($ownPackages.Count -ne 1) {
    throw "Expected exactly one current package for $expectedIdentityName $expectedVersion, found $($ownPackages.Count)"
}
foreach ($ownPackage in $ownPackages) {
    Assert-PackagedFlavor -PackagePath $ownPackage.FullName -ExpectedIdentityName $expectedIdentityName -ExpectedVersion $expectedVersion -ExpectedManifestFlavor $compiledManifestFlavor
    & $signScript -PackagePath $ownPackage.FullName -Publisher $manifestXml.Package.Identity.Publisher
}

$inventory = foreach ($package in $packages) {
    $currentPackage = Get-Item -LiteralPath $package.FullName
    [ordered]@{
        path = $currentPackage.FullName
        length = $currentPackage.Length
        sha256 = (Get-FileHash -LiteralPath $currentPackage.FullName -Algorithm SHA256).Hash
    }
}

$inventoryPath = Join-Path $artifacts "package-inventory-$ManifestFlavor.json"
$inventory | ConvertTo-Json -Depth 5 | Set-Content -Path $inventoryPath -Encoding UTF8
Write-Host "Wrote $inventoryPath"
