[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$PackagePath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw 'VCLibs package is missing'
}
Import-Module -Name (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
$signature = Get-AuthenticodeSignature -LiteralPath $PackagePath
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "VCLibs Authenticode signature is not Valid: $($signature.Status)"
}
Write-Output 'Valid'
