<#
.SYNOPSIS
  Remove this project's install from a game folder.
  Double-click Uninstall.bat, or pass -GameDir.

.DESCRIPTION
  Deletes only files this project installs (OptiScaler proxy, pass1-3, OptiScaler\ deps, project logs).
  Does NOT delete: install backups, nvngx_dlssnr.dll, dlssnr_on_amd_weights.bin,
  original-author setup/log, or any DLL that is not identified as OptiScaler.

.EXAMPLE
  .\Uninstall.bat
  .\uninstall-amd-presr.ps1 -GameDir 'D:\Games\Foo'
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',
    [switch]$NonInteractive
)
$ErrorActionPreference = 'Stop'

function Pause-Exit([int]$code) {
    if (-not $NonInteractive) {
        Write-Host ''
        Write-Host 'Press any key to exit...'
        [void][Console]::ReadKey($true)
    }
    exit $code
}

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    Write-Host ''
    Write-Host 'Uninstall FAILED.' -ForegroundColor Red
    Pause-Exit 1
}

function Test-OptiProxy([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        return ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler')
    } catch { return $false }
}

function Ask-GameFolder {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Select the game folder that contains the game .exe'
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        return $null
    } catch { return $null }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    if ($NonInteractive) { Fail 'GameDir is required in -NonInteractive mode.' }
    Write-Host 'Pick the game folder (the one with the game .exe)…' -ForegroundColor Yellow
    $GameDir = Ask-GameFolder
    if ([string]::IsNullOrWhiteSpace($GameDir)) {
        Write-Host 'Cancelled — no folder selected.'
        Pause-Exit 0
    }
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path

Write-Host ''
Write-Host 'Uninstall this project from:' -ForegroundColor Yellow
Write-Host "  $game"
Write-Host ''
Write-Host 'This uninstall script is still being tested.' -ForegroundColor Yellow
Write-Host 'It cannot guarantee it will never remove a game file or another mod.' -ForegroundColor Yellow
Write-Host 'It only deletes files that look like THIS project (OptiScaler / pass / project logs).' -ForegroundColor Yellow
Write-Host 'It will NOT delete: backups, nvngx_dlssnr.dll, weights.bin, original-author setup.' -ForegroundColor Yellow

if (-not $NonInteractive) {
    $ans = Read-Host 'Type Y to continue, anything else to cancel'
    if ($ans -notmatch '^(?i)y(es)?$') {
        Write-Host 'Cancelled.'
        Pause-Exit 0
    }
}

# File names this project installs. Proxy names are deleted only when the file is OptiScaler.
$proxyNames = @('dxgi.dll','winmm.dll','d3d12.dll','version.dll','winhttp.dll','wininet.dll','dbghelp.dll')
$projectLeafNames = @('dlssnr_amd_pass1.dll','dlssnr_amd_pass2.dll','dlssnr_amd_pass3.dll','OptiScaler.ini','amd-presr-install.txt')
$projectLogPatterns = @('OptiScaler.log*','amd_bridge.log*','amd_presr.log*')

# Never delete, even if the names overlap.
$protectedNames = @(
    'nvngx_dlssnr.dll',
    'dlssnr_on_amd_weights.bin',
    'dlssnr_on_amd_setup.exe',
    'dlssnr_on_amd.log',
    'version.dll'   # only removed later if it is identified as OptiScaler
)

$deleted = New-Object System.Collections.Generic.List[string]
$kept = New-Object System.Collections.Generic.List[string]
$errors = New-Object System.Collections.Generic.List[string]

# Prefer the proxy name the installer recorded, if present.
$recordedProxy = $null
$installMark = Join-Path $game 'amd-presr-install.txt'
if (Test-Path -LiteralPath $installMark -PathType Leaf) {
    try {
        foreach ($line in Get-Content -LiteralPath $installMark) {
            if ($line -match '^(?i)proxy=(.+)$') { $recordedProxy = $Matches[1].Trim(); break }
        }
    } catch { $recordedProxy = $null }
}
if ($recordedProxy) {
    Write-Host "Install record says proxy was: $recordedProxy"
    $proxyNames = @($recordedProxy) + @($proxyNames | Where-Object { $_ -ine $recordedProxy })
} else {
    Write-Host 'No install record; scanning common proxy names.'
}

function Remove-SafeFile([string]$path, [string]$why) {
    $leaf = Split-Path -Leaf $path
    if ($leaf -match '^(?i)backup-amd-presr') {
        $kept.Add("backup folder/file: $path")
        return
    }
    if ($protectedNames -contains $leaf -and $why -ne 'opti-proxy') {
        $kept.Add("protected: $path")
        return
    }
    try {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            $deleted.Add("$path  ($why)")
        }
    } catch {
        $errors.Add("$path : $($_.Exception.Message)")
    }
}

# XBOX / store builds may write under _storage_ next to the exe.
$roots = @($game)
$storage = Join-Path $game '_storage_'
if (Test-Path -LiteralPath $storage -PathType Container) {
    $roots += $storage
    Write-Host "Also checking: $storage"
}

foreach ($root in $roots) {
    # Proxies: only when VersionInfo says OptiScaler (never a random game/mod DLL).
    foreach ($name in $proxyNames) {
        $p = Join-Path $root $name
        if (!(Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        if (Test-OptiProxy $p) {
            Remove-SafeFile $p 'opti-proxy'
        } else {
            if ($name -ieq 'version.dll') {
                $kept.Add("left in place (not OptiScaler): $p")
            }
        }
    }

    foreach ($name in $projectLeafNames) {
        Remove-SafeFile (Join-Path $root $name) 'project-file'
    }

    foreach ($pat in $projectLogPatterns) {
        Get-ChildItem -LiteralPath $root -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-SafeFile $_.FullName 'project-log'
        }
    }

    # OptiScaler\ dependency tree shipped by this package.
    $deps = Join-Path $root 'OptiScaler'
    if (Test-Path -LiteralPath $deps -PathType Container) {
        try {
            Remove-Item -LiteralPath $deps -Recurse -Force
            $deleted.Add("$deps  (OptiScaler deps folder)")
        } catch {
            $errors.Add("$deps : $($_.Exception.Message)")
        }
    }
}

# Always note intentional keeps that may sit in the game folder.
foreach ($name in @('nvngx_dlssnr.dll','dlssnr_on_amd_weights.bin','dlssnr_on_amd_setup.exe','dlssnr_on_amd.log')) {
    foreach ($root in $roots) {
        $p = Join-Path $root $name
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            $kept.Add("kept on purpose: $p")
        }
    }
}
foreach ($root in $roots) {
    Get-ChildItem -LiteralPath $root -Directory -Filter 'backup-amd-presr*' -ErrorAction SilentlyContinue |
        ForEach-Object { $kept.Add("kept backup: $($_.FullName)") }
}

Write-Host ''
if ($deleted.Count -gt 0) {
    Write-Host 'Deleted:' -ForegroundColor Green
    foreach ($d in $deleted) { Write-Host "  - $d" }
} else {
    Write-Host 'Nothing to delete (this project does not look installed there).' -ForegroundColor Yellow
}
if ($kept.Count -gt 0) {
    Write-Host 'Kept on purpose:' -ForegroundColor Cyan
    foreach ($k in $kept) { Write-Host "  - $k" }
}
if ($errors.Count -gt 0) {
    Write-Host 'Errors:' -ForegroundColor Red
    foreach ($e in $errors) { Write-Host "  - $e" }
    Write-Host ''
    Write-Host 'Uninstall FAILED (some files could not be removed).' -ForegroundColor Red
    Pause-Exit 1
}

Write-Host ''
Write-Host 'Uninstall SUCCEEDED.' -ForegroundColor Green
Pause-Exit 0
