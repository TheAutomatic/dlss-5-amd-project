<#
.SYNOPSIS
  Remove this project from the folder this script sits in (the game folder after Setup).
  Double-click Uninstall_OptiScaler_NR.bat there. Tests may pass -GameDir.

.DESCRIPTION
  Removes identified OptiScaler proxies, named passes/config/logs, listed dependencies,
  and this uninstaller. Asks whether to keep backup-amd-presr-* folders, then lists
  planned deletions, then asks Y/N. Does NOT delete nvngx_dlssnr.dll, weights,
  original-author setup/log, other proxies, or user-added plugins and unknown files.

.EXAMPLE
  .\Uninstall_OptiScaler_NR.bat
  .\uninstall-amd-presr.ps1 -GameDir 'D:\Games\Foo'
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',
    [switch]$NonInteractive,
    [switch]$NoPause,
    [switch]$RemoveBackups
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

function Read-YesNo([string]$prompt) {
    while ($true) {
        $ans = Read-Host $prompt
        if ($ans -match '^(?i)y(es)?$') { return $true }
        if ($ans -match '^(?i)n(o)?$') { return $false }
        Write-Host 'Please type Y or N (not case sensitive).'
    }
}

function Test-OptiProxy([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        return ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler')
    } catch { return $false }
}

# The launcher owns the final pause when -NoPause is supplied. Direct script
# invocation must also keep unexpected errors visible and return failure.
trap {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Uninstall FAILED.' -ForegroundColor Red
    Pause-Exit 1
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = $PSScriptRoot
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path

# File names this project installs. Proxy names are deleted only when the file is OptiScaler.
$proxyNames = @('dxgi.dll','winmm.dll','d3d12.dll','version.dll','winhttp.dll','wininet.dll','dbghelp.dll')
$projectLeafNames = @(
    'dlssnr_amd_pass1.dll','dlssnr_amd_pass2.dll','dlssnr_amd_pass3.dll',
    'OptiScaler.ini','amd-presr-install.txt',
    'LmxxfNrRuntime.dll',
    'Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1',
    'Uninstall.bat','Uninstall.ps1'
)
$selfLeafNames = @('Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1','Uninstall.bat','Uninstall.ps1')
$projectLogPatterns = @('OptiScaler.log*','amd_bridge.log*','amd_presr.log*')
$dependencyPaths = @(
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_vk.dll',
    'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
    'D3D12_OptiScaler\D3D12Core.dll',
    'D3D12_OptiScaler\d3d12SDKLayers.dll'
)
$protectedNames = @(
    'nvngx_dlssnr.dll',
    'dlssnr_on_amd_weights.bin',
    'dlssnr_on_amd_setup.exe',
    'dlssnr_on_amd.log',
    'version.dll'
)

$planned = New-Object System.Collections.Generic.List[string]
$kept = New-Object System.Collections.Generic.List[string]
$deleted = New-Object System.Collections.Generic.List[string]
$errors = New-Object System.Collections.Generic.List[string]

function Test-UninstallPath([string]$path) {
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

function Test-TreeReparse([string]$path) {
    $stack = New-Object System.Collections.Stack
    $stack.Push($path)
    while ($stack.Count -gt 0) {
        $current = [string]$stack.Pop()
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -eq $item) { continue }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $true }
        if ($item.PSIsContainer) {
            Get-ChildItem -LiteralPath $current -Force -ErrorAction SilentlyContinue |
                ForEach-Object { $stack.Push($_.FullName) }
        }
    }
    return $false
}

function Add-PlannedFile([string]$path, [string]$why) {
    if (!(Test-UninstallPath $path)) { return }
    $leaf = Split-Path -Leaf $path
    if ($leaf -match '^(?i)backup-amd-presr') {
        return
    }
    if ($protectedNames -contains $leaf -and $why -ne 'opti-proxy') {
        $kept.Add("protected: $path")
        return
    }
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $planned.Add("$path  ($why)")
    }
}

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
    $proxyNames = @($recordedProxy) + @($proxyNames | Where-Object { $_ -ine $recordedProxy })
}

$roots = @($game)
$storage = Join-Path $game '_storage_'
if ((Test-UninstallPath $storage) -and (Test-Path -LiteralPath $storage -PathType Container)) {
    $roots += $storage
}

foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    foreach ($name in $proxyNames) {
        $p = Join-Path $root $name
        if (!(Test-UninstallPath $p)) { continue }
        if (!(Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        if (Test-OptiProxy $p) {
            Add-PlannedFile $p 'opti-proxy'
        } elseif ($name -ieq 'version.dll') {
            $kept.Add("left in place (not OptiScaler): $p")
        }
    }
    foreach ($name in $projectLeafNames) {
        Add-PlannedFile (Join-Path $root $name) 'project-file'
    }
    foreach ($pat in $projectLogPatterns) {
        Get-ChildItem -LiteralPath $root -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object {
            Add-PlannedFile $_.FullName 'project-log'
        }
    }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Add-PlannedFile (Join-Path $deps $relative) 'project-dependency'
        }
    }
    $lmxxfMods = Join-Path $root 'lmxxf-modules'
    if ((Test-UninstallPath $lmxxfMods) -and (Test-Path -LiteralPath $lmxxfMods -PathType Container)) {
        $planned.Add("$lmxxfMods  (lmxxf modules tree)")
    }
}

foreach ($name in @('nvngx_dlssnr.dll','dlssnr_on_amd_weights.bin','dlssnr_on_amd_setup.exe','dlssnr_on_amd.log')) {
    foreach ($root in $roots) {
        if (!(Test-UninstallPath $root)) { continue }
        $p = Join-Path $root $name
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            $kept.Add("kept on purpose: $p")
        }
    }
}
$backupDirs = New-Object System.Collections.Generic.List[string]
foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    Get-ChildItem -LiteralPath $root -Directory -Filter 'backup-amd-presr*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            if (Test-UninstallPath $_.FullName) { $backupDirs.Add($_.FullName) }
        }
}

$keepBackups = $true
Write-Host ''
Write-Host 'Uninstall this project from:' -ForegroundColor Yellow
Write-Host "  $game"
Write-Host ''
Write-Host 'This uninstall script is still being tested.' -ForegroundColor Yellow
Write-Host 'It cannot guarantee it will never remove a game file or another mod.' -ForegroundColor Yellow
Write-Host 'It only deletes files that look like THIS project (OptiScaler / pass / project logs).' -ForegroundColor Yellow
Write-Host 'It will NOT delete: nvngx_dlssnr.dll, weights.bin, original-author setup.' -ForegroundColor Yellow
Write-Host ''

if ($backupDirs.Count -gt 0) {
    Write-Host 'Old backup folder(s) from previous installs:' -ForegroundColor Yellow
    foreach ($b in $backupDirs) { Write-Host "  - $b" }
    if ($NonInteractive) {
        $keepBackups = -not $RemoveBackups
    } else {
        Write-Host ''
        $keepBackups = Read-YesNo 'Keep these backup folders? Y = keep, N = delete them too'
    }
    foreach ($b in $backupDirs) {
        if ($keepBackups) { $kept.Add("kept backup: $b") }
        else { $planned.Add("$b  (backup folder)") }
    }
}

if ($planned.Count -gt 0) {
    Write-Host 'Planned deletions (files/folders):' -ForegroundColor Yellow
    foreach ($d in $planned) { Write-Host "  - $d" }
    Write-Host 'Empty OptiScaler dependency folders will be removed if they become empty.'
} else {
    Write-Host 'Nothing matching this project is planned for deletion.' -ForegroundColor Yellow
}
if ($kept.Count -gt 0) {
    Write-Host 'Will keep:' -ForegroundColor Cyan
    foreach ($k in $kept) { Write-Host "  - $k" }
}

if (-not $NonInteractive) {
    Write-Host ''
    if (-not (Read-YesNo 'Type Y to delete the planned files/folders, N to cancel')) {
        Write-Host 'Cancelled.'
        Pause-Exit 0
    }
}

function Remove-SafeFile([string]$path, [string]$why) {
    if (!(Test-UninstallPath $path)) { return }
    $leaf = Split-Path -Leaf $path
    if ($leaf -match '^(?i)backup-amd-presr') {
        return
    }
    if ($protectedNames -contains $leaf -and $why -ne 'opti-proxy') {
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
            [IO.Directory]::Delete($path, $false)
            $deleted.Add("$path  (empty dependency folder)")
        }
    } catch {
        $errors.Add("$path : $($_.Exception.Message)")
    }
}

foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    foreach ($name in $proxyNames) {
        $p = Join-Path $root $name
        if (!(Test-UninstallPath $p)) { continue }
        if (!(Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        if (Test-OptiProxy $p) { Remove-SafeFile $p 'opti-proxy' }
    }
    foreach ($name in $projectLeafNames) {
        if ($selfLeafNames -contains $name) { continue }
        Remove-SafeFile (Join-Path $root $name) 'project-file'
    }
    foreach ($pat in $projectLogPatterns) {
        Get-ChildItem -LiteralPath $root -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-SafeFile $_.FullName 'project-log'
        }
    }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Remove-SafeFile (Join-Path $deps $relative) 'project-dependency'
        }
        Remove-EmptyDirectory (Join-Path $deps 'D3D12_OptiScaler')
        Remove-EmptyDirectory $deps
    }

    $lmxxfMods = Join-Path $root 'lmxxf-modules'
    if ((Test-UninstallPath $lmxxfMods) -and (Test-Path -LiteralPath $lmxxfMods -PathType Container)) {
        if (Test-TreeReparse $lmxxfMods) {
            $kept.Add("linked path: $lmxxfMods")
            $errors.Add("$lmxxfMods : contains a linked path, not deleted")
        } else {
            # Delete only files listed in SHA256SUMS (installer set). Keep user weights/extras.
            $sums = Join-Path $lmxxfMods 'SHA256SUMS'
            $manifestNames = @()
            if (Test-Path -LiteralPath $sums -PathType Leaf) {
                Get-Content -LiteralPath $sums -ErrorAction SilentlyContinue | ForEach-Object {
                    $line = $_.Trim()
                    if (-not $line) { return }
                    $parts = $line -split '\s+', 2
                    if ($parts.Count -ge 2) { $manifestNames += $parts[1].Trim() }
                }
            }
            $manifestNames += @('SHA256SUMS','modules.json','runtime-manifest.json')
            foreach ($name in ($manifestNames | Select-Object -Unique)) {
                if (-not $name) { continue }
                $fp = Join-Path $lmxxfMods $name
                if ((Test-UninstallPath $fp) -and (Test-Path -LiteralPath $fp -PathType Leaf)) {
                    Remove-SafeFile $fp 'lmxxf-module'
                }
            }
            # Remove empty subdirs then the folder if empty (user files keep it alive).
            Get-ChildItem -LiteralPath $lmxxfMods -Directory -Recurse -ErrorAction SilentlyContinue |
                Sort-Object { $_.FullName.Length } -Descending |
                ForEach-Object { Remove-EmptyDirectory $_.FullName }
            Remove-EmptyDirectory $lmxxfMods
        }
    }
}
foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    foreach ($name in $selfLeafNames) {
        Remove-SafeFile (Join-Path $root $name) 'project-file'
    }
}

if (-not $keepBackups) {
    foreach ($b in $backupDirs) {
        if (!(Test-UninstallPath $b)) { continue }
        $leaf = Split-Path -Leaf $b
        if ($leaf -notmatch '^(?i)backup-amd-presr') { continue }
        if (!(Test-Path -LiteralPath $b -PathType Container)) { continue }
        if (Test-TreeReparse $b) {
            $kept.Add("linked path: $b")
            $errors.Add("$b : contains a linked path, not deleted")
            continue
        }
        try {
            Remove-Item -LiteralPath $b -Recurse -Force
            $deleted.Add("$b  (backup folder)")
        } catch {
            $errors.Add("$b : $($_.Exception.Message)")
        }
    }
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
