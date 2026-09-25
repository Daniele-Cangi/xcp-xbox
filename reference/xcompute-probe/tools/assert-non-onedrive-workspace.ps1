[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot ".."
}

$resolvedRoot = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd("\", "/")
$segments = @($resolvedRoot -split '[\\/]' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$oneDriveRoots = @(
    $env:OneDrive,
    $env:OneDriveCommercial,
    $env:OneDriveConsumer
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object {
    [System.IO.Path]::GetFullPath($_).TrimEnd("\", "/")
}

$isOneDriveSegment = @($segments | Where-Object { $_ -ieq "OneDrive" }).Count -gt 0
$isUnderKnownOneDriveRoot = $false
foreach ($oneDriveRoot in $oneDriveRoots) {
    if ($resolvedRoot.Equals($oneDriveRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedRoot.StartsWith("$oneDriveRoot\", [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedRoot.StartsWith("$oneDriveRoot/", [System.StringComparison]::OrdinalIgnoreCase)) {
        $isUnderKnownOneDriveRoot = $true
        break
    }
}

if ($isOneDriveSegment -or $isUnderKnownOneDriveRoot) {
    throw "Workspace policy violation: build, test, package, deploy, commit, and evidence generation are forbidden under OneDrive. Use a non-OneDrive checkout."
}

[pscustomobject][ordered]@{
    schema_version = "xcompute-local-workspace-policy-0.1"
    repository_root = $resolvedRoot
    onedrive_forbidden = $true
    passed = $true
}
