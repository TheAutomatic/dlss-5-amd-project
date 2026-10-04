# Fail the release if bundled binaries look stale vs the sources they were built from.
# Run from the repo root, or via tools\release\PACKAGE_RELEASE.ps1 (it calls this automatically).
param(
    [string]$Root = '',
    # The OptiScaler.dll that will actually be packaged. PACKAGE_RELEASE passes its -OptiDll so
    # the stale-DLL rule checks that file rather than a fixed local build path.
    [string]$OptiDll = '',
    [switch]$WarnOnly
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
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
# Include local runtime helpers as well as the entrypoint. A hand-maintained
# list missed the exposure and recording-lease headers when they were extracted.
$rtSources = @(Get-ChildItem -LiteralPath $rtDir -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.hpp', '.inl') } |
    ForEach-Object { $_.FullName })
$rtSources += Join-Path $rtDir '../../../ConfigKeys.h'
Test-NotStale $dll $rtSources 'LmxxfNrRuntime.dll'
# The sync manifest owns the complete runtime header closure, including compiler,
# codec and geometry helpers. Do not maintain a smaller hand-picked release list.
$manifest = Get-Content -LiteralPath (Join-Path $Root 'tools/lmxxf-sync/manifest.json') -Raw | ConvertFrom-Json
$runtimeHeaders = @($manifest.headers | ForEach-Object { Join-Path $Root ("third_party/lmxxf/" + $_) })
Test-NotStale $dll $runtimeHeaders 'LmxxfNrRuntime.dll (vendor header)'

# 2) Dual-arch modules vs hip recipe/sources (modules must not be older than .hip/.inc)
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
        if ($hs.Count -ne 38) { $failures.Add("$arch has $($hs.Count) hsaco, expected 38") }
    }
    $rootSums = Join-Path $modRoot 'SHA256SUMS'
    if (!(Test-Path -LiteralPath $rootSums -PathType Leaf)) {
        $failures.Add('Missing parent SHA256SUMS')
    }
    # Oldest hsaco vs newest .hip/.inc source under third_party/lmxxf/hip
    $hipSrc = Get-ChildItem -LiteralPath (Join-Path $Root 'third_party/lmxxf/hip') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.hip', '.inc') }
    $headerPaths = @($manifest.module_headers | ForEach-Object { 'third_party/lmxxf/' + $_ })
    foreach ($path in $headerPaths) { $hipSrc = @($hipSrc) + (Get-Item -LiteralPath (Join-Path $Root $path)) }
    $hsaco = Get-ChildItem -LiteralPath $modRoot -Recurse -Filter '*.hsaco' -File -ErrorAction SilentlyContinue
    # Committed .hip and .hsaco carry checkout-order timestamps, not build times: on a fresh CI
    # clone a .hip written a few ms after a .hsaco looked "newer" and failed the release. Sync
    # already refuses recipe changes without rebuilt modules, so the timestamp rule only guards
    # local, uncommitted edits.
    $gitClean = $false
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $dirty = & git -C $Root status --porcelain -- 'third_party/lmxxf/hip' 'third_party/lmxxf/modules' @headerPaths 2>$null
        $gitClean = ($LASTEXITCODE -eq 0) -and -not $dirty
    }
    if ($hipSrc -and $hsaco -and -not $gitClean) {
        $newestHip = ($hipSrc | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
        $oldestMod = ($hsaco | Sort-Object LastWriteTimeUtc | Select-Object -First 1).LastWriteTimeUtc
        if ($newestHip -gt $oldestMod) {
            $failures.Add(("STALE modules: hip/*.hip or *.inc newer than oldest hsaco ({0} > {1}) — rebuild with build-modules / sync" -f $newestHip, $oldestMod))
        }
    }
}

# 3) OptiScaler.dll vs main host sources (best-effort: vcxproj tree timestamps)
$optiDll = if ($OptiDll) { $OptiDll } else { Join-Path $Root 'exports/release-local/OptiScaler.dll' }
$optiSrcRoot = Join-Path $Root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
if (Test-Path -LiteralPath $optiDll -PathType Leaf) {
    $optiSources = Get-ChildItem -LiteralPath $optiSrcRoot -Recurse -Include '*.cpp', '*.h' -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FullName -notmatch '\\(external|include\\imgui|dlssnr\\backend\\lmxxf_runtime)\\'
        } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 20
    if ($optiSources) {
        $newest = $optiSources[0].LastWriteTimeUtc
        $a = Get-Stamp $optiDll
        if ($newest -gt $a) {
            $failures.Add(("STALE OptiScaler.dll: source {0} is newer than {1} — run tools\build\build-release-local.cmd" -f $optiSources[0].Name, $optiDll))
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
