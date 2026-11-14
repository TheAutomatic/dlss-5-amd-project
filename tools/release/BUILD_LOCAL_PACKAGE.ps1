[CmdletBinding()]
param([string]$Root = '', [string]$Version = '')
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
if (-not $Version) {
    $vf = Join-Path $Root 'VERSION'
    $Version = if (Test-Path -LiteralPath $vf) { ([IO.File]::ReadAllText($vf)).Trim() } else { '2.0.0' }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = "OptiScaler-NR-$Version-local-$stamp"
& (Join-Path $PSScriptRoot 'PACKAGE_RELEASE.ps1') -LocalTest -Version $Version -Name $name -OutDir 'dist'
