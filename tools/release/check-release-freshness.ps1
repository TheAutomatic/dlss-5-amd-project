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

# OptiScaler.dll vs main host sources (best-effort: vcxproj tree timestamps)
$optiDll = if ($OptiDll) { $OptiDll } else { Join-Path $Root 'exports/release-local/OptiScaler.dll' }
$optiSrcRoot = Join-Path $Root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
if (Test-Path -LiteralPath $optiDll -PathType Leaf) {
    $optiSources = Get-ChildItem -LiteralPath $optiSrcRoot -Recurse -Include '*.cpp', '*.h', '*.hpp', '*.inl', '*.rc', '*.ttf', '*.vcxproj' -File -ErrorAction SilentlyContinue |
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