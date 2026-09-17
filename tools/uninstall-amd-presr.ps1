<#
.SYNOPSIS
  Remove this project's install from a game folder.
  Double-click Uninstall.bat, or pass -GameDir.

.DESCRIPTION
  Removes identified OptiScaler proxies, named passes/config/logs, and explicitly listed dependencies.
  Does NOT delete: install backups, nvngx_dlssnr.dll, dlssnr_on_amd_weights.bin,
  original-author setup/log, other proxies, or user-added plugins and unknown files.

.EXAMPLE
  .\Uninstall.bat
  .\uninstall-amd-presr.ps1 -GameDir 'D:\Games\Foo'
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',
    [switch]$NonInteractive,
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'

function Pause-Exit([int]$code) {
    if (-not $NonInteractive -and -not $NoPause) {
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

# The launcher owns the final pause when -NoPause is supplied. Direct script
# invocation must also keep unexpected errors visible and return failure.
trap {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Uninstall FAILED.' -ForegroundColor Red
    Pause-Exit 1
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
# Explicit dependency names shipped by PACKAGE_RELEASE.ps1. Never sweep *.dll
# or recurse: users may add plugins, configuration, or other mods here.
$dependencyPaths = @(
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_vk.dll',
    'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
    'D3D12_OptiScaler\D3D12Core.dll',
    'D3D12_OptiScaler\d3d12SDKLayers.dll'
)

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

function Test-UninstallPath([string]$path) {
    # Refuse linked files/directories, including an OptiScaler or _storage_
    # junction. Checking each component prevents following a link to another
    # installation while examining an otherwise allowlisted leaf name.
    $full = [IO.Path]::GetFullPath($path)
    $base = $game.TrimEnd('\')
    if ($full -ine $base -and -not $full.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
        $kept.Add("outside game folder: $path")
        return $false
    }
    $current = $full
    while ($true) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            $kept.Add("linked path: $path")
            return $false
        }
        if ($current.TrimEnd('\') -ieq $base) { return $true }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

# Prefer the proxy name the installer recorded, if present.
$recordedProxy = $null
$installMark = Join-Path $game 'amd-presr-install.txt'
if ((Test-UninstallPath $installMark) -and (Test-Path -LiteralPath $installMark -PathType Leaf)) {
    try {
        foreach ($line in Get-Content -LiteralPath $installMark) {
            if ($line -match '^(?i)proxy=(.+)$') { $recordedProxy = $Matches[1].Trim(); break }
        }
    } catch { $recordedProxy = $null }
}
if ($recordedProxy -and $proxyNames -notcontains $recordedProxy) {
    $kept.Add("ignored invalid proxy name in install record: $recordedProxy")
    $recordedProxy = $null
}
if ($recordedProxy) {
    Write-Host "Install record says proxy was: $recordedProxy"
    $proxyNames = @($recordedProxy) + @($proxyNames | Where-Object { $_ -ine $recordedProxy })
} else {
    Write-Host 'No install record; scanning common proxy names.'
}

function Remove-SafeFile([string]$path, [string]$why) {
    if (!(Test-UninstallPath $path)) { return }
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

function Remove-EmptyDirectory([string]$path) {
    if (!(Test-UninstallPath $path)) { return }
    if (!(Test-Path -LiteralPath $path -PathType Container)) { return }
    try {
        if (@(Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop).Count -eq 0) {
            # The nonrecursive API also refuses deletion if new content appears.
            [IO.Directory]::Delete($path, $false)
            $deleted.Add("$path  (empty dependency folder)")
        }
    } catch {
        $errors.Add("$path : $($_.Exception.Message)")
    }
}

# XBOX / store builds may write under _storage_ next to the exe.
$roots = @($game)
$storage = Join-Path $game '_storage_'
if ((Test-UninstallPath $storage) -and (Test-Path -LiteralPath $storage -PathType Container)) {
    $roots += $storage
    Write-Host "Also checking: $storage"
}

foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    # Proxies: only when VersionInfo says OptiScaler (never a random game/mod DLL).
    foreach ($name in $proxyNames) {
        $p = Join-Path $root $name
        if (!(Test-UninstallPath $p)) { continue }
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

    # Remove only known dependency files, preserving plugins and unknown files.
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Remove-SafeFile (Join-Path $deps $relative) 'project-dependency'
        }
        Remove-EmptyDirectory (Join-Path $deps 'D3D12_OptiScaler')
        Remove-EmptyDirectory $deps
    }
}

# Always note intentional keeps that may sit in the game folder.
foreach ($name in @('nvngx_dlssnr.dll','dlssnr_on_amd_weights.bin','dlssnr_on_amd_setup.exe','dlssnr_on_amd.log')) {
    foreach ($root in $roots) {
        if (!(Test-UninstallPath $root)) { continue }
        $p = Join-Path $root $name
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            $kept.Add("kept on purpose: $p")
        }
    }
}
foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
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
