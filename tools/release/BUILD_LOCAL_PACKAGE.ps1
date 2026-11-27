[CmdletBinding()]
param(
    [string]$Root = '',
    [string]$Version = '',
    [string]$OptiArchive = '',
    [string]$OptiDir = '',
    [string]$MochiArchive = '',
    [string]$MochiLangArchive = '',
    [string]$MochiDir = '',
    [string]$OpenRrArchive = '',
    [string]$OpenRrDir = '',
    [string]$OptiPatcherFile = '',
    [string]$UpstreamDir = '',
    [switch]$RefreshOptiPatcher
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
if (-not $Version) {
    $vf = Join-Path $Root 'VERSION'
    $Version = if (Test-Path -LiteralPath $vf) { ([IO.File]::ReadAllText($vf)).Trim() } else { '2.0.0' }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = "OptiScaler-NR-Windows-$Version-local-$stamp"
& (Join-Path $PSScriptRoot 'PACKAGE_RELEASE.ps1') `
    -LocalTest `
    -Version $Version `
    -Name $name `
    -OutDir 'dist' `
    -OptiArchive $OptiArchive `
    -OptiDir $OptiDir `
    -MochiArchive $MochiArchive `
    -MochiLangArchive $MochiLangArchive `
    -MochiDir $MochiDir `
    -OpenRrArchive $OpenRrArchive `
    -OpenRrDir $OpenRrDir `
    -OptiPatcherFile $OptiPatcherFile `
    -UpstreamDir $UpstreamDir `
    -RefreshOptiPatcher:$RefreshOptiPatcher
