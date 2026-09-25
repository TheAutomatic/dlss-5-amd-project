# Fail the release if bundled binaries look stale vs the sources they were built from.
# Run from the repo root, or via PACKAGE_RELEASE.ps1 (it calls this automatically).
param(
    [string]$Root = '',
    [switch]$WarnOnly
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$Root = (Resolve-Path -LiteralPath $Root).Path

function Write-Issue([string]$msg, [bool]$fatal) {
    $color = if ($fatal) { 'Red' } else { 'Yellow' }
    Write-Host $msg -ForegroundColor $color
}

$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Get-Stamp([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return (Get-Item -LiteralPath $path).LastWriteTimeUtc
}

function Test-NotStale([string]$artifact, [string[]]$sources, [string]$label) {
    $a = Get-Stamp $artifact
    if ($null -eq $a) {
        $script:failures.Add("Missing artifact: $artifact")
        return
    }
    foreach ($s in $sources) {
        $t = Get-Stamp $s
        if ($null -eq $t) {
            $script:warnings.Add("Source missing for $label : $s")
            continue
        }
        if ($t -gt $a) {
            $script:failures.Add(("STALE {0}: {1} is older than source {2}" -f $label, $artifact, $s))
        }
    }
}

# 1) LmxxfNrRuntime.dll vs C++ sources
$dll = Join-Path $Root 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
$rtDir = Join-Path $Root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime'
$rtSources = @(
    (Join-Path $rtDir 'LmxxfNrRuntime.cpp'),
    (Join-Path $rtDir 'LmxxfNrApi.h'),
    (Join-Path $rtDir 'LmxxfProductionOptions.h')
)
Test-NotStale $dll $rtSources 'LmxxfNrRuntime.dll'
# Pinned headers used by the runtime
foreach ($h in @('hip_d3d12_bridge.h', 'hip_reference_network.h')) {
    $p = Join-Path $Root "third_party/lmxxf/Development/HIP/$h"
    if (Test-Path -LiteralPath $p -PathType Leaf) { Test-NotStale $dll @($p) "LmxxfNrRuntime.dll ($h)" }
}

# 2) Dual-arch modules vs hip recipe/sources (modules must not be older than .hip)
$modRoot = Join-Path $Root 'third_party/lmxxf/modules'
if (!(Test-Path -LiteralPath $modRoot -PathType Container)) {
    $failures.Add("Missing modules dir: $modRoot")
} else {
    foreach ($arch in @('gfx1200', 'gfx1201')) {
        $sums = Join-Path $modRoot "$arch/SHA256SUMS"
        if (!(Test-Path -LiteralPath $sums -PathType Leaf)) {
            $failures.Add("Missing $arch leaf SHA256SUMS")
            continue
        }
        $hs = @(Get-ChildItem -LiteralPath (Join-Path $modRoot $arch) -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
        if ($hs.Count -ne 24) { $failures.Add("$arch has $($hs.Count) hsaco, expected 24") }
    }
    $rootSums = Join-Path $modRoot 'SHA256SUMS'
    if (!(Test-Path -LiteralPath $rootSums -PathType Leaf)) {
        $failures.Add('Missing parent SHA256SUMS')
    }
    # Oldest hsaco vs newest .hip source under third_party/lmxxf/hip
    $hipSrc = Get-ChildItem -LiteralPath (Join-Path $Root 'third_party/lmxxf/hip') -Filter '*.hip' -File -ErrorAction SilentlyContinue
    $hsaco = Get-ChildItem -LiteralPath $modRoot -Recurse -Filter '*.hsaco' -File -ErrorAction SilentlyContinue
    if ($hipSrc -and $hsaco) {
        $newestHip = ($hipSrc | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
        $oldestMod = ($hsaco | Sort-Object LastWriteTimeUtc | Select-Object -First 1).LastWriteTimeUtc
        if ($newestHip -gt $oldestMod) {
            $failures.Add(("STALE modules: hip/*.hip newer than oldest hsaco ({0} > {1}) — rebuild with build-modules / sync" -f $newestHip, $oldestMod))
        }
    }
}

# 3) OptiScaler.dll vs main host sources (best-effort: vcxproj tree timestamps)
$optiDll = Join-Path $Root 'exports/release-local/OptiScaler.dll'
$optiSrcRoot = Join-Path $Root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
if (Test-Path -LiteralPath $optiDll -PathType Leaf) {
    $optiSources = Get-ChildItem -LiteralPath $optiSrcRoot -Recurse -Include '*.cpp', '*.h' -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FullName -notmatch '\\(external|include\\imgui|dlssnr\\backend\\lmxxf_runtime)\\' -and
            $_.Name -notlike 'Lmxxf*'
        } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 20
    if ($optiSources) {
        $newest = $optiSources[0].LastWriteTimeUtc
        $a = Get-Stamp $optiDll
        if ($newest -gt $a) {
            $failures.Add(("STALE OptiScaler.dll: source {0} is newer than {1} — run tools\build-release-local.cmd" -f $optiSources[0].Name, $optiDll))
        }
    }
} else {
    $warnings.Add("OptiScaler.dll not found at $optiDll (PACKAGE will use -DepsRoot or release-local)")
}

foreach ($w in $warnings) { Write-Issue $w $false }
foreach ($f in $failures) { Write-Issue $f $true }

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("Release freshness check FAILED ({0} problem(s))." -f $failures.Count) -ForegroundColor Red
    if ($WarnOnly) { exit 0 }
    exit 1
}
Write-Host "Release freshness check OK ($($warnings.Count) warning(s))." -ForegroundColor Green
exit 0
