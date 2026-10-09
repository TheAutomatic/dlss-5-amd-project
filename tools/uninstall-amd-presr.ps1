<#
.SYNOPSIS
  Remove this project from the folder this script sits in (the game folder after Setup).
  Double-click Uninstall_OptiScaler_NR.bat there. Tests may pass -GameDir.

.DESCRIPTION
  Removes identified OptiScaler proxies, named passes/config/logs, listed dependencies,
  and this uninstaller. Asks whether to keep backup-amd-presr-* folders, then lists
  planned deletions, then asks Y/N. Does NOT delete nvngx_dlssnr.dll, weights,
  danielblnc setup, other proxies, or user-added plugins and unknown files.

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
# Resolve-Path keeps 8.3 names such as RUNNER~1. GetFullPath expands them.
# Later checks use GetFullPath, so the game root must be that same form.
$game = (Resolve-Path -LiteralPath $GameDir).Path
$game = [IO.Path]::GetFullPath($game)

# New packages share the controlled module names. A standalone copied uninstaller
# can still use the installed manifests; it never claims ownership by extension.
$moduleHelper = Join-Path $PSScriptRoot 'lmxxf-module-package.ps1'
$controlledModules = @()
if (Test-Path -LiteralPath $moduleHelper -PathType Leaf) {
    . $moduleHelper
    $controlledModules = @(Get-LmxxfModuleNames)
}

# File names this project installs. Proxy names are deleted only when the file is OptiScaler.
$proxyNames = @('dxgi.dll','winmm.dll','d3d12.dll','version.dll','winhttp.dll','wininet.dll','dbghelp.dll')
$projectLeafNames = @(
    'dlssnr_amd_pass1.dll','dlssnr_amd_pass2.dll','dlssnr_amd_pass3.dll',
    'dlssnr_core.dll', # Legacy standalone NR core; integrated runtimes do not use it.
    'OptiScaler.ini','amd-presr-install.txt',
    'dlssnr_on_amd.ini', 'dlssnr-amd.ini', 'dlssnr-amd-crash.dmp', 'dlssnr-amd-install.txt',
    'LmxxfNrRuntime.dll', 'MochizukiNrRuntime.dll', 'lmxxf-module-package.ps1',
    'Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1',
    'OptiScaler/dlssnr/README.md',
    'OptiScaler/dlssnr/design/frame-hold.md',
    'OptiScaler/dlssnr/design/multi-point-anchoring.md',
    'OptiScaler/dlssnr/design/pre-sr-multipass.md',
    'dlssnr-amd/pipeline.cache', 'dlssnr-amd/prewarm/manifest.txt',
    'dlssnr-amd/shaders/accumulation.txt',
    'dlssnr-amd/shaders/coherent-act.txt',
    'dlssnr-amd/shaders/g_attn.spv',
    'dlssnr-amd/shaders/g_decups.spv',
    'dlssnr-amd/shaders/g_ffwd3.spv',
    'dlssnr-amd/shaders/g_ffwd3w.spv',
    'dlssnr-amd/shaders/g_fswin128.spv',
    'dlssnr-amd/shaders/g_fswin256.spv',
    'dlssnr-amd/shaders/g_fswin32.spv',
    'dlssnr-amd/shaders/g_fswin64.spv',
    'dlssnr-amd/shaders/g_fswinds128.spv',
    'dlssnr-amd/shaders/g_fswinds256.spv',
    'dlssnr-amd/shaders/g_fswinds32.spv',
    'dlssnr-amd/shaders/g_fswinds64.spv',
    'dlssnr-amd/shaders/g_fswindsp128.spv',
    'dlssnr-amd/shaders/g_fswindsp256.spv',
    'dlssnr-amd/shaders/g_fswindsp32.spv',
    'dlssnr-amd/shaders/g_fswindsp64.spv',
    'dlssnr-amd/shaders/g_fswinfusedup128.spv',
    'dlssnr-amd/shaders/g_fswinfusedup256.spv',
    'dlssnr-amd/shaders/g_fswinfusedup32.spv',
    'dlssnr-amd/shaders/g_fswinfusedup64.spv',
    'dlssnr-amd/shaders/g_fswinimagepost32.spv',
    'dlssnr-amd/shaders/g_fswinimagepreds32.spv',
    'dlssnr-amd/shaders/g_fswinp128.spv',
    'dlssnr-amd/shaders/g_fswinp256.spv',
    'dlssnr-amd/shaders/g_fswinp64.spv',
    'dlssnr-amd/shaders/g_fswinpds128.spv',
    'dlssnr-amd/shaders/g_fswinpds256.spv',
    'dlssnr-amd/shaders/g_fswinpds64.spv',
    'dlssnr-amd/shaders/g_fswinpup128.spv',
    'dlssnr-amd/shaders/g_fswinpup256.spv',
    'dlssnr-amd/shaders/g_fswinpup64.spv',
    'dlssnr-amd/shaders/g_gemmnores.spv',
    'dlssnr-amd/shaders/g_gemmpool.spv',
    'dlssnr-amd/shaders/g_gemmproj.spv',
    'dlssnr-amd/shaders/g_gemmprojc.spv',
    'dlssnr-amd/shaders/g_gemmprojt.spv',
    'dlssnr-amd/shaders/g_gemmprojw.spv',
    'dlssnr-amd/shaders/g_gemmvact.spv',
    'dlssnr-amd/shaders/g_gemmvnores.spv',
    'dlssnr-amd/shaders/g_gemmvproj.spv',
    'dlssnr-amd/shaders/g_gemmvqkv.spv',
    'dlssnr-amd/shaders/g_gemmvqkvnorm.spv',
    'dlssnr-amd/shaders/g_gemmvqkvs.spv',
    'dlssnr-amd/shaders/g_noisefield.spv',
    'dlssnr-amd/shaders/g_repack.spv',
    'dlssnr-amd/shaders/g_upsview.spv',
    'dlssnr-amd/shaders/g_vitattn.spv',
    'dlssnr-amd/shaders/g_vitattnnobda.spv',
    'dlssnr-amd/shaders/runtime/cascade_blur.spv',
    'dlssnr-amd/shaders/runtime/cascade_feed.spv',
    'dlssnr-amd/shaders/runtime/cascade_lograt.spv',
    'dlssnr-amd/shaders/runtime/runtime_alpha.spv',
    'dlssnr-amd/shaders/runtime/runtime_depth.spv',
    'dlssnr-amd/shaders/runtime/runtime_encode.spv',
    'dlssnr-amd/shaders/runtime/runtime_prep.spv',
    'dlssnr-amd/shaders/runtime/runtime_transfer.spv',
    'dlssnr-amd/shaders/shader-constants.txt',
    'dlssnr-amd/shaders/swin-bias-storage.txt',
    'dlssnr-amd/shaders/temporal/motion_estimate.spv',
    'dlssnr-amd/shaders/temporal/motion_luma.spv',
    'dlssnr-amd/shaders/temporal/shader-constants.txt',
    'dlssnr-amd/shaders/temporal/temporal_post_fp32.spv',
    'dlssnr-amd/shaders/temporal/temporal_pre_fp32.spv',
    'person-model/onnxruntime.dll',
    'person-model/yolo11n-seg.onnx',
    'Uninstall.bat','Uninstall.ps1'
)
$selfLeafNames = @('Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1','Uninstall.bat','Uninstall.ps1')
# spdlog rotates OptiScaler.log into OptiScaler.1.log; older sinks used .log.1.
# Match numeric rotations only, not user notes such as OptiScaler.notes.log.
$projectLogNamePattern = '^(OptiScaler|amd_bridge|amd_presr|mochizuki_nr|dlssnr_on_amd|dlssnr-amd|dlss-enabler)(\.[0-9]+)?\.log(\.[0-9]+)?$'
# Old module bundles also shipped generated sources and disassembly. Limit cleanup
# to the same controlled module stems and the two supported architecture folders.
foreach ($arch in @('', 'gfx1200/', 'gfx1201/')) {
    foreach ($module in $controlledModules) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($module)
        $projectLeafNames += 'lmxxf-modules/' + $arch + $stem + '.generated.hip'
        $projectLeafNames += 'lmxxf-modules/' + $arch + $module + '.s'
    }
}
$dependencyPaths = @(
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_vk.dll',
    'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
    'D3D12_OptiScaler\D3D12Core.dll',
    'D3D12_OptiScaler\d3d12SDKLayers.dll',
    'plugins\OptiPatcher.asi'
)
$protectedNames = @(
    'nvngx_dlssnr.dll',
    'dlssnr_on_amd_weights.bin',
    'dlssnr_on_amd_setup.exe',
    'version.dll',
    'native-game-tiled-assets'
)
# Live glue set. Also purge retired native_/preblock_ files in both historical locations.
$lmxxfShaderFiles = @(
    'native_black_probe.hlsl',
    'native_codec_decode.hlsl',
    'native_codec_encode.hlsl',
    'native_format_convert.hlsl',
    'native_game_rgb_input.hlsl',
    'native_history_guard.hlsl',
    'native_output_smooth.hlsl',
    'native_rgb_reflect.hlsl',
    'native_rgb_texture.hlsl',
    'native_temporal_coordinates.hlsl',
    'native_temporal_feed.hlsl',
    'native_temporal_sample.hlsl',
    'native_text_overlay.hlsl'
)

$planned = New-Object System.Collections.Generic.List[string]
$kept = New-Object System.Collections.Generic.List[string]
$deleted = New-Object System.Collections.Generic.List[string]
$restored = New-Object System.Collections.Generic.List[string]
$errors = New-Object System.Collections.Generic.List[string]

function Test-UninstallPath([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    $base = [IO.Path]::GetFullPath($game).TrimEnd('\')
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

function Get-IniSetting([string]$iniPath, [string]$sectionName, [string]$key) {
    $inSection = $false
    foreach ($line in [System.IO.File]::ReadAllLines($iniPath)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            if ($inSection) { break }
            $inSection = ($Matches[1] -ieq $sectionName)
        } elseif ($inSection -and $line -match ('^\s*' + [regex]::Escape($key) + '\s*=(.*)$')) {
            return $Matches[1].Trim()
        }
    }
    return $null
}

# Keep this standalone helper aligned with uninstall and PluginPath.h.
# Relative paths are anchored to the game executable directory, never the launcher's CWD.
function Get-PluginsTargetDirectory([string]$gameDir, [string]$iniPath) {
    $gameDir = [IO.Path]::GetFullPath($gameDir)
    $cfgPath = $null
    $mainPath = $null
    if (Test-Path -LiteralPath $iniPath -PathType Leaf) {
        $cfgPath = Get-IniSetting $iniPath 'Plugins' 'Path'
        $mainPath = Get-IniSetting $iniPath 'Libraries' 'OptiDllPath'
    }
    if ($cfgPath -and $cfgPath -ine 'auto') {
        # Setup creates an explicit plugin directory before the host checks it.
        $target = if ([IO.Path]::IsPathRooted($cfgPath)) { $cfgPath } else { Join-Path $gameDir $cfgPath }
        return [IO.Path]::GetFullPath($target)
    }
    if (-not $mainPath -or $mainPath -ieq 'auto') { $mainPath = 'OptiScaler' }
    if (-not [IO.Path]::IsPathRooted($mainPath)) { $mainPath = Join-Path $gameDir $mainPath }
    $mainPath = [IO.Path]::GetFullPath($mainPath)
    if (-not (Test-Path -LiteralPath $mainPath -PathType Container)) { $mainPath = $gameDir }
    return Join-Path $mainPath 'plugins'
}

# Setup only upserts one DLSS5_FIT_LARGE line; the file may also hold the user's own
# upstream lmxxf flags. Remove our old line or an otherwise untouched seed template.
function Get-FlagsRemainder([string]$path) {
    $text = [IO.File]::ReadAllText($path)
    $remainder = [regex]::Replace($text, '(?m)^DLSS5_FIT_LARGE=.*(\r?\n|$)', '')
    $seedLines = @(
        '# Optional lmxxf upstream keys (DLSS5_*).',
        '# OptiScaler.ini / Ins menu win on conflict; this file only fills gaps.',
        '# Example: DLSS5_HIP_WAVE_OWNED=1'
    )
    $userLines = @($remainder -split '\r?\n' | Where-Object {
        $_.Trim().Length -gt 0 -and $seedLines -cnotcontains $_.Trim()
    })
    if ($userLines.Count -eq 0) { return '' }
    return $remainder
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

$customPluginFiles = New-Object System.Collections.Generic.List[string]
$legacyLogPaths = New-Object System.Collections.Generic.List[string]
foreach ($root in $roots) {
    $legacyMark = Join-Path $root 'dlssnr-amd-install.txt'
    if ((Test-UninstallPath $legacyMark) -and (Test-Path -LiteralPath $legacyMark -PathType Leaf)) {
        foreach ($line in [IO.File]::ReadAllLines($legacyMark)) {
            if ($line -match '^L ([^/\\:*?"<>|]+_(?:dxgi|d3d11|d3d9)\.log|vkd3d-proton\.cache(?:\.write)?)$') {
                $legacyLogPaths.Add((Join-Path $root $Matches[1]))
            }
        }
    }
}
foreach ($path in $legacyLogPaths) { Add-PlannedFile $path 'legacy-backend-log' }

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
    Get-ChildItem -LiteralPath $root -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $projectLogNamePattern } |
        ForEach-Object { Add-PlannedFile $_.FullName 'project-log' }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Add-PlannedFile (Join-Path $deps $relative) 'project-dependency'
        }
    }
    # Resolve once while the INI still exists; the deletion phase removes the INI first.
    $gameIni = Join-Path $root 'OptiScaler.ini'
    try {
        $pluginDir = Get-PluginsTargetDirectory $root $gameIni
        $customPluginAsi = Join-Path $pluginDir 'OptiPatcher.asi'
        if ((Test-Path -LiteralPath $customPluginAsi -PathType Leaf) -and
            (Test-UninstallPath $customPluginAsi)) {
            Add-PlannedFile $customPluginAsi 'project-dependency'
            $customPluginFiles.Add($customPluginAsi)
        }
    } catch {
        $kept.Add("unresolved plugin path in ${gameIni}: $($_.Exception.Message)")
    }
    $lmxxfMods = Join-Path $root 'lmxxf-modules'
    if ((Test-UninstallPath $lmxxfMods) -and (Test-Path -LiteralPath $lmxxfMods -PathType Container)) {
        $planned.Add("$lmxxfMods  (lmxxf modules tree)")
    }
    foreach ($shadersDir in @((Join-Path $root 'shaders'), (Join-Path $root 'lmxxf-modules/shaders'))) {
        if ((Test-UninstallPath $shadersDir) -and (Test-Path -LiteralPath $shadersDir -PathType Container)) {
            foreach ($sf in $lmxxfShaderFiles) {
                Add-PlannedFile (Join-Path $shadersDir $sf) 'lmxxf-shader'
            }
            # Also plan retired wave/vit/preblock hlsl left by older installs (install used to upsert-only).
            Get-ChildItem -LiteralPath $shadersDir -Filter '*.hlsl' -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^(native_|preblock_)' -and ($lmxxfShaderFiles -notcontains $_.Name) } |
                ForEach-Object { Add-PlannedFile $_.FullName 'lmxxf-shader-stale' }
            $cacheDir = Join-Path $shadersDir 'shader-cache'
            if ((Test-UninstallPath $cacheDir) -and (Test-Path -LiteralPath $cacheDir -PathType Container)) {
                Get-ChildItem -LiteralPath $cacheDir -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^[0-9a-f]{16}(\.v2)?\.dxbc(\.[0-9]+\.[0-9]+\.tmp)?$' } | ForEach-Object {
                        Add-PlannedFile $_.FullName 'lmxxf-shader-cache'
                    }
            }
        }
    }

    $flagsFile = Join-Path $root 'DLSS5-AMD\native-game-flags.txt'
    if ((Test-UninstallPath $flagsFile) -and (Test-Path -LiteralPath $flagsFile -PathType Leaf)) {
        $flagsRest = Get-FlagsRemainder $flagsFile
        if ($flagsRest.Trim().Length -eq 0) {
            Add-PlannedFile $flagsFile 'lmxxf-flags'
        } elseif ($flagsRest -cne [IO.File]::ReadAllText($flagsFile)) {
            $planned.Add("$flagsFile  (lmxxf-flags: remove the DLSS5_FIT_LARGE line; other flags kept)")
        } else {
            $kept.Add("user flags/comments: $flagsFile")
        }
    }
}

foreach ($name in @('nvngx_dlssnr.dll','dlssnr_on_amd_weights.bin','dlssnr_on_amd_setup.exe')) {
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
    $weightsDir = Join-Path $root 'native-game-tiled-assets'
    if (Test-Path -LiteralPath $weightsDir -PathType Container) {
        $kept.Add("kept on purpose (weights directory): $weightsDir")
    }
}
$backupDirs = New-Object System.Collections.Generic.List[string]
foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'backup-amd-presr*' -or $_.Name -ieq 'dlssnr-amd-backup' } |
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
Write-Host 'It will NOT delete: nvngx_dlssnr.dll, weights.bin, native-game-tiled-assets, danielblnc setup.' -ForegroundColor Yellow
Write-Host ''

if ($backupDirs.Count -gt 0) {
    Write-Host 'Old backup folder(s) from previous installs:' -ForegroundColor Yellow
    foreach ($b in $backupDirs) { Write-Host "  - $b" }
    if ($NonInteractive) {
        $keepBackups = -not $RemoveBackups
    } else {
        Write-Host ''
        $keepBackups = Read-YesNo 'Keep these backup folders? Y = keep, N = delete them too (model files are kept)'
    }
    foreach ($b in $backupDirs) {
        if ($keepBackups) { $kept.Add("kept backup: $b") }
        else { $planned.Add("$b  (backup folder)") }
    }
}

if ($planned.Count -gt 0) {
    Write-Host 'Planned deletions (files/folders):' -ForegroundColor Yellow
    foreach ($d in $planned) { Write-Host "  - $d" }
    Write-Host 'Known dependency and shader folders will be removed only if empty. Mochizuki caches will be rebuilt on next use.'
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
    Get-ChildItem -LiteralPath $root -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $projectLogNamePattern } |
        ForEach-Object { Remove-SafeFile $_.FullName 'project-log' }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Remove-SafeFile (Join-Path $deps $relative) 'project-dependency'
        }
        Remove-EmptyDirectory (Join-Path $deps 'dlssnr/design')
        Remove-EmptyDirectory (Join-Path $deps 'dlssnr')
        Remove-EmptyDirectory (Join-Path $deps 'D3D12_OptiScaler')
        Remove-EmptyDirectory $deps
    }

    $lmxxfMods = Join-Path $root 'lmxxf-modules'
    if ((Test-UninstallPath $lmxxfMods) -and (Test-Path -LiteralPath $lmxxfMods -PathType Container)) {
        if (Test-TreeReparse $lmxxfMods) {
            $kept.Add("linked path: $lmxxfMods")
            $errors.Add("$lmxxfMods : contains a linked path, not deleted")
        } else {
            # Delete only files listed in SHA256SUMS (installer set) or standard module files. Keep user weights/extras.
            $sums = Join-Path $lmxxfMods 'SHA256SUMS'
            $manifestNames = [System.Collections.Generic.List[string]]::new()
            if (Test-Path -LiteralPath $sums -PathType Leaf) {
                Get-Content -LiteralPath $sums -ErrorAction SilentlyContinue | ForEach-Object {
                    $line = $_.Trim()
                    if (-not $line) { return }
                    $parts = $line -split '\s+', 2
                    if ($parts.Count -ge 2 -and $parts[0] -match '^[0-9a-fA-F]{64}$') {
                        $raw = $parts[1].Trim().TrimStart('*')
                        # Accept root or single-arch relative paths: e.g. foo.hsaco or gfx1201/foo.hsaco
                        if ($raw -match '(?i)^(((?:gfx1200|gfx1201)[/\\])?[^/\\:*?"<>|]+\.hsaco)$' -and $raw -notmatch '\.\.') {
                            $manifestNames.Add($raw.Replace('/', '\'))
                        }
                    }
                }
            }
            # Only the two supported architecture directories are project-owned.
            $archDirs = @(Get-ChildItem -LiteralPath $lmxxfMods -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -in @('gfx1200', 'gfx1201') })
            foreach ($arch in $archDirs) {
                if (Test-TreeReparse $arch.FullName) {
                    $kept.Add("linked path: $($arch.FullName)")
                    $errors.Add("$($arch.FullName) : contains a linked path, not deleted")
                    continue
                }
                $leafSums = Join-Path $arch.FullName 'SHA256SUMS'
                if (Test-Path -LiteralPath $leafSums -PathType Leaf) {
                    Get-Content -LiteralPath $leafSums -ErrorAction SilentlyContinue | ForEach-Object {
                        $line = $_.Trim()
                        if (-not $line) { return }
                        $parts = $line -split '\s+', 2
                        if ($parts.Count -ge 2 -and $parts[0] -match '^[0-9a-fA-F]{64}$') {
                            $raw = $parts[1].Trim().TrimStart('*')
                            if ($raw -match '(?i)^[^/\\:*?"<>|]+\.hsaco$' -and $raw -notmatch '\.\.') {
                                $manifestNames.Add($arch.Name + '\' + $raw)
                            }
                        }
                    }
                }
                foreach ($name in $controlledModules) { $manifestNames.Add($arch.Name + '\' + $name) }
                foreach ($extra in @('SHA256SUMS', 'modules.json', 'runtime-manifest.json', 'README.md')) {
                    $manifestNames.Add($arch.Name + '\' + $extra)
                }
            }
            # Legacy flat installs own only recorded or controlled module names.
            foreach ($name in $controlledModules) { $manifestNames.Add($name) }
            foreach ($extra in @('SHA256SUMS','modules.json','runtime-manifest.json','README.md')) {
                $manifestNames.Add($extra)
            }
            $lmxxfModsFull = [IO.Path]::GetFullPath($lmxxfMods).TrimEnd('\') + '\'
            foreach ($name in ($manifestNames | Select-Object -Unique)) {
                if (-not $name) { continue }
                $fp = Join-Path $lmxxfMods $name
                $fpFull = [IO.Path]::GetFullPath($fp)
                if ($fpFull.StartsWith($lmxxfModsFull, [StringComparison]::OrdinalIgnoreCase) -and
                    (Test-UninstallPath $fp) -and (Test-Path -LiteralPath $fp -PathType Leaf)) {
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
    foreach ($shadersDir in @((Join-Path $root 'shaders'), (Join-Path $root 'lmxxf-modules/shaders'))) {
        if ((Test-UninstallPath $shadersDir) -and (Test-Path -LiteralPath $shadersDir -PathType Container)) {
            foreach ($sf in $lmxxfShaderFiles) {
                Remove-SafeFile (Join-Path $shadersDir $sf) 'lmxxf-shader'
            }
            Get-ChildItem -LiteralPath $shadersDir -Filter '*.hlsl' -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^(native_|preblock_)' } |
                ForEach-Object { Remove-SafeFile $_.FullName 'lmxxf-shader-stale' }
            $cacheDir = Join-Path $shadersDir 'shader-cache'
            if ((Test-UninstallPath $cacheDir) -and (Test-Path -LiteralPath $cacheDir -PathType Container)) {
                Get-ChildItem -LiteralPath $cacheDir -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^[0-9a-f]{16}(\.v2)?\.dxbc(\.[0-9]+\.[0-9]+\.tmp)?$' } | ForEach-Object {
                        Remove-SafeFile $_.FullName 'lmxxf-shader-cache'
                    }
                Remove-EmptyDirectory $cacheDir
            }
            Remove-EmptyDirectory $shadersDir
        }
    }
    Remove-EmptyDirectory (Join-Path $root 'lmxxf-modules')
    foreach ($relative in @('dlssnr-amd/shaders/runtime', 'dlssnr-amd/shaders/temporal',
            'dlssnr-amd/shaders', 'dlssnr-amd/prewarm', 'dlssnr-amd', 'person-model')) {
        Remove-EmptyDirectory (Join-Path $root $relative)
    }

    $flagsFile = Join-Path $root 'DLSS5-AMD\native-game-flags.txt'
    if ((Test-UninstallPath $flagsFile) -and (Test-Path -LiteralPath $flagsFile -PathType Leaf)) {
        $flagsRest = Get-FlagsRemainder $flagsFile
        if ($flagsRest.Trim().Length -eq 0) {
            Remove-SafeFile $flagsFile 'lmxxf-flags'
            Remove-EmptyDirectory (Join-Path $root 'DLSS5-AMD')
        } elseif ($flagsRest -cne [IO.File]::ReadAllText($flagsFile)) {
            try {
                [IO.File]::WriteAllText($flagsFile, $flagsRest)
                $deleted.Add("$flagsFile  (lmxxf-flags: removed the DLSS5_FIT_LARGE line)")
            } catch {
                $errors.Add("$flagsFile : $($_.Exception.Message)")
            }
        }
    }

    # Restore original game CRT if upgraded during installation
    foreach ($crtDll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $orig = Join-Path $root ($crtDll + '.orig')
        $target = Join-Path $root $crtDll
        if ((Test-UninstallPath $orig) -and (Test-Path -LiteralPath $orig -PathType Leaf)) {
            try {
                Move-Item -LiteralPath $orig -Destination $target -Force
                $restored.Add("$target  (restored original game $crtDll from .orig backup)")
            } catch {
                $errors.Add("$orig : $($_.Exception.Message)")
            }
        }
    }
}
foreach ($path in ($customPluginFiles | Select-Object -Unique)) { Remove-SafeFile $path 'project-dependency' }
foreach ($path in $legacyLogPaths) { Remove-SafeFile $path 'legacy-backend-log' }
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
        if ($leaf -notmatch '^(?i)backup-amd-presr' -and $leaf -ine 'dlssnr-amd-backup') { continue }
        if (!(Test-Path -LiteralPath $b -PathType Container)) { continue }
        if (Test-TreeReparse $b) {
            $kept.Add("linked path: $b")
            $errors.Add("$b : contains a linked path, not deleted")
            continue
        }
        try {
            # Some original-backend backups include the only remaining model copy.
            # Delete backup contents individually so choosing N cannot destroy it.
            $backupPrefix = [IO.Path]::GetFullPath($b).TrimEnd('\') + '\'
            foreach ($file in Get-ChildItem -LiteralPath $b -Recurse -File -Force -ErrorAction Stop) {
                $relative = $file.FullName.Substring($backupPrefix.Length)
                if ($file.Name -in @('dlssnr.bin', 'dlssnr_on_amd_weights.bin', 'nvngx_dlssnr.dll') -or
                    @($relative.Split('\')) -contains 'native-game-tiled-assets') {
                    $kept.Add("model inside backup: $($file.FullName)")
                    continue
                }
                Remove-SafeFile $file.FullName 'backup-file'
            }
            Get-ChildItem -LiteralPath $b -Recurse -Directory -Force -ErrorAction Stop |
                Sort-Object { $_.FullName.Length } -Descending |
                ForEach-Object { Remove-EmptyDirectory $_.FullName }
            Remove-EmptyDirectory $b
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
if ($restored.Count -gt 0) {
    Write-Host 'Restored:' -ForegroundColor Green
    foreach ($r in $restored) { Write-Host "  - $r" }
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
