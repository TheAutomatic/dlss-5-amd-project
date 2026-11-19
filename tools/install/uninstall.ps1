<#
.SYNOPSIS
  Remove this project from the folder this script sits in (the game folder after Setup).
  Double-click Uninstall_OptiScaler_NR.bat there. Tests may pass -GameDir.

.DESCRIPTION
  Removes identified OptiScaler proxies, named passes/config/logs, listed dependencies,
  and this uninstaller. Asks whether to keep backup-amd-presr-* folders, then lists
  planned deletions, then asks Y/N. Removes the dedicated dlssnr-amd, openrr-amd
  and legacy opennr-rr resource directories, including their models and caches.
  Original game files, root-level model sources and unrelated plugins are kept.

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
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
# Purge legacy model backups from %LocalAppData% (models belong only in the launcher/game folders).
try {
    $localApp = [Environment]::GetFolderPath('LocalApplicationData')
    if ($localApp) {
        foreach ($legacyDir in @(
            (Join-Path $localApp 'OptScalerNR\ModelStore'),
            (Join-Path $localApp 'OptScalerNR\Launcher\models'),
            (Join-Path $localApp 'OpenNR')
        )) {
            if (Test-Path -LiteralPath $legacyDir -PathType Container) {
                Remove-Item -LiteralPath $legacyDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
} catch { }

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

function Get-CrtHash([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($path)
        try { return [BitConverter]::ToString($sha.ComputeHash($stream)) }
        finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
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

function Assert-LmxxfUnlinkedPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -gt [IO.Path]::GetPathRoot($full).Length) { $full = $full.TrimEnd('\', '/') }
    $cursor = $full
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing linked module path (symlink/junction/reparse point): $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\', '/'))
    }
    return $full
}

function Get-LmxxfUnlinkedFiles([string]$Directory) {
    $full = Assert-LmxxfUnlinkedPath $Directory
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Missing module directory: $full" }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($full)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing linked module path (symlink/junction/reparse point): $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $item }
        }
    }
}

$controlledModules = @(
    'deep_fast-packed-fast.hsaco',
    'vit-stream-fast.hsaco',
    'multi-pass-skin.hsaco',
    'multi-pass-predict.hsaco',
    'boundary-fast.hsaco', 'boundary_reference.hsaco', 'c32_fast.hsaco',
    'c32_fast_attention.hsaco', 'c32_fused_attention.hsaco',
    'c32_fused_ffn_attention-packed.hsaco', 'c32_fused_ffn_attention.hsaco',
    'c32_prefix_reference.hsaco', 'c32_tiled.hsaco', 'c32_wmma.hsaco',
    'c32-wave1-rtz.hsaco', 'c32-wave1-fast.hsaco', 'c64-wave2-fast.hsaco',
    'c32-wave1.hsaco', 'c64-wave2.hsaco', 'c512-m32-mh.hsaco', 'c512-m32-deep.hsaco',
    'deep_fast-packed.hsaco', 'deep_fast.hsaco', 'deep_reference.hsaco', 'deep_wmma.hsaco',
    'multihead-fast-packed.hsaco', 'multihead-fast-padded-wave-packed.hsaco',
    'multihead-fast-padded-wave.hsaco', 'multihead-fast.hsaco', 'multihead-reference.hsaco',
    'multihead-tiled.hsaco', 'multihead-wmma.hsaco', 'multihead_fused_attention.hsaco',
    'prefix_fast.hsaco', 'vit-wide-deep.hsaco', 'wave-pointwise.hsaco', 'vit-stream.hsaco',
    'swin-persistent.hsaco', 'swin-persistent-fast.hsaco', 'c32-wave1-fast-norm900.hsaco'
)

# File names this project installs. Proxy names are deleted only when the file is OptiScaler.
$proxyNames = @('dxgi.dll','winmm.dll','d3d12.dll','version.dll','winhttp.dll','wininet.dll','dbghelp.dll')
$projectLeafNames = @(
    'dlssnr_amd_pass1.dll','dlssnr_amd_pass2.dll','dlssnr_amd_pass3.dll',
    'dlssnr_core.dll', # Legacy standalone NR core; integrated runtimes do not use it.
    'OptiScaler.ini',
    'dlssnr_on_amd.ini', 'dlssnr-amd-crash.dmp',
    'LmxxfNrRuntime.dll', 'MochizukiNrRuntime.dll', 'OpenNrRrRuntime.dll', 'opennr_rr.log', 'openrr-amd.log', 'openrr-amd.ini', 'openrr-ngx-host.dll', 'openrr-control.bin', 'openrr-amd-crash.dmp', 'lmxxf-module-package.ps1',

    'dlssnr-amd.ini', 'dlssnr-amd.default.ini',
    'Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1',
    'OptiScaler/dlssnr/README.md',
    'OptiScaler/dlssnr/design/frame-hold.md',
    'OptiScaler/dlssnr/design/multi-point-anchoring.md',
    'OptiScaler/dlssnr/design/pre-sr-multipass.md',
    'person-model/onnxruntime.dll',
    'person-model/yolo11n-seg.onnx',
    'person-model/pphumanseg.onnx',
    'person-model/yunet.onnx',
    'person-model/person-worker.exe', 'person-model/person-worker.log', 'person-model/person-worker.1.log',
    'person-model/msvcp140.dll', 'person-model/msvcp140_1.dll', 'person-model/msvcp140_2.dll',
    'person-model/msvcp140_atomic_wait.dll', 'person-model/msvcp140_codecvt_ids.dll',
    'person-model/vcruntime140.dll', 'person-model/vcruntime140_1.dll', 'person-model/vcruntime140_threads.dll',
    'person-model/concrt140.dll', 'person-model/vccorlib140.dll',
    'Uninstall.bat','Uninstall.ps1','Remove_OptiScaler.bat','Remove OptiScaler.bat','remove_optiscaler.sh','xess-compat.ps1'
)
$selfLeafNames = @('Uninstall_OptiScaler_NR.bat','Uninstall_OptiScaler_NR.ps1','Uninstall.bat','Uninstall.ps1','mochizuki-owned.txt','xess-compat.ps1')
$pluginResourceDirs = @('dlssnr-amd', 'openrr-amd', 'opennr-rr')
$installRecords = @('dlssnr-amd-install.txt', 'openrr-shaders-install.txt', 'amd-presr-install.txt')
# spdlog rotates OptiScaler.log into OptiScaler.1.log; older sinks used .log.1.
# Match numeric rotations only, not user notes such as OptiScaler.notes.log.
$projectLogNamePattern = '^(OptiScaler|amd_bridge|amd_presr|mochizuki_nr|dlssnr_on_amd|dlssnr-amd|openrr-amd|openrr|opennr_rr|dlss-enabler|dlssg_to_fsr3|fakenvapi)(\.[0-9]+)?\.log(\.[0-9]+)?$'
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
    'amd_fidelityfx_dx12.dll',
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_denoiser_dx12.dll',
    'amd_fidelityfx_vk.dll',
    'amd_fidelityfx_loader_vk.dll',
    'amd_fidelityfx_upscaler_vk.dll',
    'amd_fidelityfx_framegeneration_vk.dll',
    'dlssg_to_fsr3_amd_is_better.dll',
    'Streamlined_fetcher_windows.bat',
    'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
    'D3D12_OptiScaler\D3D12Core.dll',
    'D3D12_OptiScaler\d3d12SDKLayers.dll',
    'plugins\OptiPatcher.asi',
    'plugins\dlssnr-amd.asi',
    'plugins\openrr.asi', 'plugins\openrr-amd.asi', 'plugins\openrr-ngx-host.dll', 'openrr-ngx-host.dll',
    'plugins\OptScaler-NR-plugins-README.txt'
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
$recordedPluginDir = $null
$recordedExternalPlugins = @{}
$installMark = Join-Path $game 'amd-presr-install.txt'
if ((Test-UninstallPath $installMark) -and (Test-Path -LiteralPath $installMark -PathType Leaf)) {
    try {
        foreach ($line in [IO.File]::ReadAllLines($installMark, [Text.Encoding]::UTF8)) {
            $cleanLine = $line.TrimStart([char]0xFEFF).Trim()
            if ($cleanLine -match '^(?i)proxy=(.+)$') { $recordedProxy = $Matches[1].Trim() }
            elseif ($cleanLine -match '^(?i)pluginDir=(.+)$') { $recordedPluginDir = $Matches[1].Trim() }
            elseif ($cleanLine -match '^(?i)extPlugin=([a-f0-9]{64})\|(.+)$') {
                $extSha = $Matches[1].ToLowerInvariant()
                $extPath = [IO.Path]::GetFullPath($Matches[2].Trim())
                $baseRootForExt = [IO.Path]::GetFullPath($game).TrimEnd('\')
                if ([IO.Path]::GetFileName($extPath) -in @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll') -and
                    $extPath -ine $baseRootForExt -and
                    -not $extPath.StartsWith($baseRootForExt + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    $recordedExternalPlugins[$extPath] = $extSha
                } else {
                    $kept.Add("ignored invalid extPlugin receipt entry: $extPath")
                }
            }
        }
    } catch { $recordedProxy = $null; $recordedPluginDir = $null; $recordedExternalPlugins = @{} }
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
    foreach ($name in $projectLeafNames + $installRecords + $selfLeafNames) {
        Add-PlannedFile (Join-Path $root $name) 'project-file'
    }
    foreach ($name in $pluginResourceDirs) {
        $path = Join-Path $root $name
        if ((Test-Path -LiteralPath $path -PathType Container) -and (Test-UninstallPath $path)) {
            $planned.Add("$path  (plugin resources, including models and caches)")
        }
    }
    Get-ChildItem -LiteralPath $root -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $projectLogNamePattern } |
        ForEach-Object { Add-PlannedFile $_.FullName 'project-log' }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Add-PlannedFile (Join-Path $deps $relative) 'project-dependency'
        }
        foreach ($sub in @('Licenses', 'Streamline', 'streamline')) {
            $subDir = Join-Path $deps $sub
            if ((Test-UninstallPath $subDir) -and (Test-Path -LiteralPath $subDir -PathType Container)) {
                Get-ChildItem -LiteralPath $subDir -File -ErrorAction SilentlyContinue | ForEach-Object {
                    Add-PlannedFile $_.FullName 'project-dependency'
                }
            }
        }
    }
    # Resolve once while the INI still exists; the deletion phase removes the INI first.
    $gameIni = Join-Path $root 'OptiScaler.ini'
    try {
        $candidatePlugins = New-Object System.Collections.Generic.List[string]
        $pluginDirsToCheck = New-Object System.Collections.Generic.List[string]
        $pluginDir = Get-PluginsTargetDirectory $root $gameIni
        if ($pluginDir) { $pluginDirsToCheck.Add($pluginDir) }
        if ($recordedPluginDir -and ($pluginDirsToCheck -notcontains $recordedPluginDir)) {
            $pluginDirsToCheck.Add($recordedPluginDir)
        }
        $fallbackRootPlugins = Join-Path $root 'plugins'
        if ($pluginDirsToCheck -notcontains $fallbackRootPlugins) {
            $pluginDirsToCheck.Add($fallbackRootPlugins)
        }
        $defaultOptiPlugins = [IO.Path]::GetFullPath((Join-Path $root 'OptiScaler\plugins'))
        foreach ($pDir in $pluginDirsToCheck) {
            if ([IO.Path]::GetFullPath($pDir) -ieq $defaultOptiPlugins) { continue }
            foreach ($pluginOwned in @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll')) {
                $cand = Join-Path $pDir $pluginOwned
                if ($candidatePlugins -notcontains $cand) { $candidatePlugins.Add($cand) }
            }
        }
        foreach ($recExt in $recordedExternalPlugins.Keys) {
            if ($candidatePlugins -notcontains $recExt) { $candidatePlugins.Add($recExt) }
        }
        foreach ($customPluginFile in $candidatePlugins) {
            if (-not (Test-Path -LiteralPath $customPluginFile -PathType Leaf)) { continue }
            $fullPlugin = [IO.Path]::GetFullPath($customPluginFile)
            if ([IO.Path]::GetFileName($fullPlugin) -notin @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll')) { continue }
            $baseRoot = [IO.Path]::GetFullPath($game).TrimEnd('\')
            if ($fullPlugin -ieq $baseRoot -or $fullPlugin.StartsWith($baseRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
                if (Test-UninstallPath $customPluginFile) {
                    Add-PlannedFile $customPluginFile 'project-dependency'
                    $customPluginFiles.Add($customPluginFile)
                }
            } else {
                try {
                    $safeExtPlugin = Assert-LmxxfUnlinkedPath $customPluginFile
                    if ([IO.Path]::GetFileName($safeExtPlugin) -in @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll')) {
                        $expectedExtSha = $null
                        foreach ($k in $recordedExternalPlugins.Keys) {
                            if ($k -ieq $safeExtPlugin) { $expectedExtSha = $recordedExternalPlugins[$k]; break }
                        }
                        $actualExtSha = (Get-CrtHash $safeExtPlugin).Replace('-', '').ToLowerInvariant()
                        if ($expectedExtSha -and $actualExtSha -eq $expectedExtSha) {
                            $planned.Add("$safeExtPlugin  (project-dependency)")
                            $customPluginFiles.Add($safeExtPlugin)
                        } else {
                            $kept.Add("external plugin without matching receipt kept: $safeExtPlugin")
                        }
                    }
                } catch {
                    $kept.Add("linked external plugin path: $customPluginFile")
                }
            }
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
        Where-Object { $_.Name -like 'backup-amd-presr*' -or $_.Name -ieq 'dlssnr-amd-backup' -or $_.Name -ieq 'OptiScaler-compat-backup' } |
        ForEach-Object {
            if (Test-UninstallPath $_.FullName) { $backupDirs.Add($_.FullName) }
        }
}

$keepBackups = $true
Write-Host ''
Write-Host 'Uninstall this project from:' -ForegroundColor Yellow
Write-Host "  $game"
Write-Host ''
Write-Host 'The dlssnr-amd, openrr-amd and opennr-rr folders will be removed with their models and caches.' -ForegroundColor Yellow
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
    Write-Host 'Other dependency folders are removed when empty.'
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

Write-Host 'Removing installed files...' -ForegroundColor Cyan

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
    foreach ($name in $pluginResourceDirs) {
        $path = [IO.Path]::GetFullPath((Join-Path $root $name))
        if (-not (Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue)) { continue }
        try {
            if (-not (Test-UninstallPath $path)) { throw 'Unsafe plugin resource path.' }
            # Check the whole tree before deleting it. Never follow a junction
            # or symlink into a directory outside the game.
            $longPath = if ($path.StartsWith('\\')) { '\\?\UNC\' + $path.Substring(2) } else { '\\?\' + $path }
            $readOnlyFiles = [Collections.Generic.List[IO.FileInfo]]::new()
            $dirs = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
            $dirs.Push([IO.DirectoryInfo]::new($longPath))
            while ($dirs.Count) {
                foreach ($entry in $dirs.Pop().EnumerateFileSystemInfos()) {
                    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                        throw "Linked path inside plugin resources: $($entry.FullName)"
                    }
                    if ($entry -is [IO.DirectoryInfo]) {
                        $dirs.Push($entry)
                    } elseif ($entry.IsReadOnly) {
                        $readOnlyFiles.Add($entry)
                    }
                }
            }
            foreach ($file in $readOnlyFiles) { $file.IsReadOnly = $false }
            [IO.Directory]::Delete($longPath, $true)
            $deleted.Add("$path  (plugin resources)")
        } catch {
            $errors.Add("$path : $($_.Exception.Message)")
        }
    }
    Get-ChildItem -LiteralPath $root -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $projectLogNamePattern } |
        ForEach-Object { Remove-SafeFile $_.FullName 'project-log' }
    $deps = Join-Path $root 'OptiScaler'
    if ((Test-UninstallPath $deps) -and (Test-Path -LiteralPath $deps -PathType Container)) {
        foreach ($relative in $dependencyPaths) {
            Remove-SafeFile (Join-Path $deps $relative) 'project-dependency'
        }
        foreach ($sub in @('Licenses', 'Streamline', 'streamline')) {
            $subDir = Join-Path $deps $sub
            if ((Test-UninstallPath $subDir) -and (Test-Path -LiteralPath $subDir -PathType Container)) {
                Get-ChildItem -LiteralPath $subDir -File -ErrorAction SilentlyContinue | ForEach-Object {
                    Remove-SafeFile $_.FullName 'project-dependency'
                }
                Remove-EmptyDirectory $subDir
            }
        }
        Remove-EmptyDirectory (Join-Path $deps 'dlssnr/design')
        Remove-EmptyDirectory (Join-Path $deps 'dlssnr')
        Remove-EmptyDirectory (Join-Path $deps 'D3D12_OptiScaler')
        Remove-EmptyDirectory (Join-Path $deps 'plugins')
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
    Remove-EmptyDirectory (Join-Path $root 'person-model')

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

    # Restore active OptiScaler-compat-backup (XeSS / FFX compatibility repair) before cleanup
    $compatActive = Join-Path $root 'OptiScaler-compat-backup\active.json'
    if ((Test-UninstallPath $compatActive) -and (Test-Path -LiteralPath $compatActive -PathType Leaf)) {
        $compatRestoreOk = $false
        $allowedCompatNames = @(
            'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
            'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
            'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_vk.dll',
            'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll'
        )
        try {
            $xessCompatScript = Join-Path $PSScriptRoot 'xess-compat.ps1'
            if (-not (Test-Path -LiteralPath $xessCompatScript -PathType Leaf)) {
                $xessCompatScript = Join-Path $PSScriptRoot 'install\xess-compat.ps1'
            }
            if (Test-Path -LiteralPath $xessCompatScript -PathType Leaf) {
                . $xessCompatScript
            }
            if ((Get-Command Get-XessJournal -ErrorAction SilentlyContinue) -and (Get-Command Restore-XessCompatibility -ErrorAction SilentlyContinue)) {
                $validatedJournal = Get-XessJournal $root
                if ($null -eq $validatedJournal) {
                    throw 'Missing or unreadable XeSS repair journal.'
                }
                if ($validatedJournal.state -in @('pending', 'applied')) {
                    Restore-XessCompatibility $root
                    foreach ($cf in @($validatedJournal.files)) {
                        if ($cf.originalPresent) {
                            $restored.Add("$(Join-Path $root $cf.name)  (restored original game $($cf.name) from OptiScaler-compat-backup)")
                        }
                    }
                } elseif ($validatedJournal.state -notin @('restored', 'archived')) {
                    throw "Invalid XeSS repair journal state: $($validatedJournal.state)"
                }
                $compatRestoreOk = $true
            } else {
                $cj = [IO.File]::ReadAllText($compatActive, [Text.Encoding]::UTF8) | ConvertFrom-Json
                if ($cj.schema -ne 1 -or $cj.gameDir -ne [IO.Path]::GetFullPath($root) -or
                    $cj.transaction -notmatch '^[a-f0-9]{32}$' -or $cj.state -notin @('pending', 'applied', 'restored', 'archived')) {
                    throw 'Invalid XeSS repair record. Preserving current game files and backup.'
                }
                if ($cj.state -in @('pending', 'applied')) {
                    $txDir = Join-Path $root ("OptiScaler-compat-backup\" + $cj.transaction)
                    if (-not (Test-UninstallPath $txDir)) { throw "Unsafe transaction backup directory: $txDir" }
                    $seenCompat = @{}
                    if (@($cj.files).Count -lt 1 -or @($cj.files).Count -gt $allowedCompatNames.Count) {
                        throw 'Invalid XeSS repair file list.'
                    }
                    foreach ($cf in @($cj.files)) {
                        $fName = [string]$cf.name
                        if ($allowedCompatNames -notcontains $fName -or $seenCompat.ContainsKey($fName) -or
                            $cf.appliedHash -cnotmatch '^[a-f0-9]{64}$' -or $cf.originalPresent -isnot [bool] -or
                            ($cf.originalPresent -and $cf.originalHash -cnotmatch '^[a-f0-9]{64}$')) {
                            throw "Invalid XeSS repair file entry: $fName"
                        }
                        $seenCompat[$fName] = $true
                        $cTarget = Join-Path $root $fName
                        $cBackup = Join-Path $txDir $fName
                        if (-not (Test-UninstallPath $cTarget) -or -not (Test-UninstallPath $cBackup)) {
                            throw "Unsafe compatibility target/backup path for $fName"
                        }
                        if ($cf.originalPresent) {
                            if (-not (Test-Path -LiteralPath $cBackup -PathType Leaf)) {
                                throw "Original backup missing: $fName"
                            }
                            $bakHash = (Get-CrtHash $cBackup).Replace('-', '').ToLowerInvariant()
                            if ($bakHash -ne $cf.originalHash) {
                                throw "Original backup is damaged: $fName"
                            }
                        }
                        if (Test-Path -LiteralPath $cTarget -PathType Leaf) {
                            $curHash = (Get-CrtHash $cTarget).Replace('-', '').ToLowerInvariant()
                            if ($curHash -ne $cf.appliedHash -and (-not $cf.originalPresent -or $curHash -ne $cf.originalHash)) {
                                throw "File changed after repair; preserving it: $fName. Game updates must not be overwritten."
                            }
                        } elseif ($cf.originalPresent) {
                            $inFlight = Join-Path $txDir ($fName + '.in-flight')
                            if (-not (Test-Path -LiteralPath $inFlight -PathType Leaf)) {
                                throw "File removed after repair; preserving current state: $fName"
                            }
                        }
                    }
                    if (-not ('OptScalerNR.UninstallAtomicCommit' -as [type])) {
                        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace OptScalerNR {
    public static class UninstallAtomicCommit {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
                                                uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool SetFileInformationByHandle(SafeFileHandle handle, int kind, IntPtr data, uint size);
        public static void RetireChecked(string target, string expectedHash, string saved) {
            // DELETE + GENERIC_READ, exclusive sharing (0), OPEN_EXISTING (3), OPEN_REPARSE_POINT (0x00200000).
            // The same locked handle is hashed and renamed; no pathname check/replace gap.
            using (var handle = CreateFileW(target, 0x80010000, 0, IntPtr.Zero, 3, 0x00200000, IntPtr.Zero)) {
                if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
                using (var stream = new FileStream(handle, FileAccess.Read))
                using (var sha = SHA256.Create()) {
                    string actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                    if (!string.Equals(actual, expectedHash, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("File changed during restore; preserving it: " + target);
                    byte[] name = Encoding.Unicode.GetBytes(Path.GetFullPath(saved));
                    int rootOffset = IntPtr.Size == 8 ? 8 : 4;
                    int lengthOffset = rootOffset + IntPtr.Size;
                    int nameOffset = lengthOffset + 4;
                    IntPtr data = Marshal.AllocHGlobal(nameOffset + name.Length + 2);
                    try {
                        Marshal.WriteInt32(data, 0, 0); // Do not replace an existing saved file.
                        Marshal.WriteIntPtr(data, rootOffset, IntPtr.Zero);
                        Marshal.WriteInt32(data, lengthOffset, name.Length);
                        Marshal.Copy(name, 0, IntPtr.Add(data, nameOffset), name.Length);
                        Marshal.WriteInt16(data, nameOffset + name.Length, 0);
                        if (!SetFileInformationByHandle(handle, 3, data, (uint)(nameOffset + name.Length + 2)))
                            throw new Win32Exception(Marshal.GetLastWin32Error());
                    } finally { Marshal.FreeHGlobal(data); }
                }
            }
        }
    }
}
'@
                    }
                    foreach ($cf in @($cj.files)) {
                        $fName = [string]$cf.name
                        $cTarget = Join-Path $root $fName
                        $cBackup = Join-Path $txDir $fName
                        $inFlight = Join-Path $txDir ($fName + '.in-flight')
                        if (-not (Test-Path -LiteralPath $cTarget -PathType Leaf) -and $cf.originalPresent -and (Test-Path -LiteralPath $inFlight -PathType Leaf)) {
                            $ifHash = (Get-CrtHash $inFlight).Replace('-', '').ToLowerInvariant()
                            if ($ifHash -notin @($cf.originalHash, $cf.appliedHash)) { throw "Interrupted file move is damaged: $fName" }
                            [IO.File]::Move($inFlight, $cTarget)
                        }
                        if ($cf.originalPresent) {
                            if (Test-Path -LiteralPath $cTarget -PathType Leaf) {
                                $curHash = (Get-CrtHash $cTarget).Replace('-', '').ToLowerInvariant()
                                if ($curHash -eq $cf.originalHash) { continue }
                                if ($curHash -ne $cf.appliedHash) { throw "File changed during restore: $fName" }
                            } else {
                                throw "File removed during restore: $fName"
                            }
                            if (Test-Path -LiteralPath $inFlight -PathType Leaf) { [IO.File]::Delete($inFlight) }
                            $tempRestore = $cTarget + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
                            $movedCurrent = $false
                            try {
                                [IO.File]::Copy($cBackup, $tempRestore, $false)
                                if ((Get-CrtHash $tempRestore).Replace('-', '').ToLowerInvariant() -ne $cf.originalHash) {
                                    throw "Backup changed while copying: $cBackup"
                                }
                                [OptScalerNR.UninstallAtomicCommit]::RetireChecked($cTarget, $cf.appliedHash, $inFlight)
                                $movedCurrent = $true
                                [IO.File]::Move($tempRestore, $cTarget)
                                [IO.File]::Delete($inFlight)
                                $restored.Add("$cTarget  (restored original game $fName from OptiScaler-compat-backup)")
                            } catch {
                                if ($movedCurrent -and -not (Test-Path -LiteralPath $cTarget -PathType Leaf)) {
                                    try { [IO.File]::Move($inFlight, $cTarget) } catch { }
                                }
                                throw
                            } finally {
                                if (Test-Path -LiteralPath $tempRestore -PathType Leaf) {
                                    Remove-Item -LiteralPath $tempRestore -Force -ErrorAction SilentlyContinue
                                }
                            }
                        } elseif (Test-Path -LiteralPath $cTarget -PathType Leaf) {
                            $retiredAdded = Join-Path $txDir ($fName + '.' + [Guid]::NewGuid().ToString('N') + '.removed')
                            [OptScalerNR.UninstallAtomicCommit]::RetireChecked($cTarget, $cf.appliedHash, $retiredAdded)
                            [IO.File]::Delete($retiredAdded)
                            $deleted.Add("$cTarget  (compat-repair-added)")
                        }
                    }
                    $cj.state = 'restored'
                    [IO.File]::WriteAllText($compatActive, ($cj | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
                }
                $compatRestoreOk = $true
            }
        } catch {
            $compatRestoreOk = $false
            $kept.Add("$(Join-Path $root 'OptiScaler-compat-backup') (compatibility restore skipped/failed: $($_.Exception.Message))")
            $errors.Add("$compatActive : $($_.Exception.Message)")
        }
        if (-not $compatRestoreOk) {
            $compatDirFull = [IO.Path]::GetFullPath((Join-Path $root 'OptiScaler-compat-backup'))
            for ($bi = $backupDirs.Count - 1; $bi -ge 0; $bi--) {
                if ([IO.Path]::GetFullPath($backupDirs[$bi]) -ieq $compatDirFull) {
                    $backupDirs.RemoveAt($bi)
                }
            }
        }
    }

    # Restore foreign proxy / original game DLLs moved aside during installation (*.moved in backup-amd-presr-*)
    $presrBackups = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'backup-amd-presr*' -and (Test-UninstallPath $_.FullName) } |
        Sort-Object Name)
    foreach ($pb in $presrBackups) {
        if (Test-TreeReparse $pb.FullName) { continue }
        $movedFiles = @(Get-ChildItem -LiteralPath $pb.FullName -Filter '*.moved' -File -Force -ErrorAction SilentlyContinue)
        foreach ($mf in $movedFiles) {
            $origLeaf = $mf.Name.Substring(0, $mf.Name.Length - 6)
            if ($proxyNames -notcontains $origLeaf) { continue }
            $origTarget = Join-Path $root $origLeaf
            if (-not (Test-UninstallPath $origTarget)) { continue }
            try {
                if (Test-Path -LiteralPath $origTarget -PathType Leaf) {
                    $curHash = Get-CrtHash $origTarget
                    $bakHash = Get-CrtHash $mf.FullName
                    if ($curHash -eq $bakHash) {
                        Remove-SafeFile $mf.FullName 'restored-foreign-backup-duplicate'
                        continue
                    }
                    if (-not (Test-OptiProxy $origTarget)) {
                        $kept.Add("$($mf.FullName) (target $origTarget already exists and is not OptiScaler; preserving both)")
                        continue
                    }
                    Remove-SafeFile $origTarget 'opti-proxy'
                }
                Move-Item -LiteralPath $mf.FullName -Destination $origTarget -Force -ErrorAction Stop
                $restored.Add("$origTarget  (restored original $origLeaf from $($pb.Name))")
            } catch {
                $kept.Add("$($mf.FullName) (restore failed; preserving backup)")
                $errors.Add("$($mf.FullName) : $($_.Exception.Message)")
            }
        }
    }
    # Restore original game CRT if upgraded during installation
    foreach ($crtDll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $orig = Join-Path $root ($crtDll + '.orig')
        $target = Join-Path $root $crtDll
        if ((Test-UninstallPath $orig) -and (Test-UninstallPath $target) -and (Test-Path -LiteralPath $orig -PathType Leaf)) {
            try {
                # Old installers copied System32 CRTs but left no ownership receipt.
                # Only restore an old backup over that same system payload; a
                # launcher/user may since have installed a different game version.
                $oldVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($orig)
                $systemCrt = Join-Path ([Environment]::GetFolderPath('System')) $crtDll
                $mayRestore = $oldVersion.FileMajorPart -eq 14 -and $oldVersion.FileMinorPart -lt 30 -and
                    (Test-Path -LiteralPath $target -PathType Leaf) -and (Test-Path -LiteralPath $systemCrt -PathType Leaf) -and
                    ((Get-CrtHash $target) -eq (Get-CrtHash $systemCrt))
                if (!$mayRestore) {
                    $kept.Add("$orig (legacy CRT backup ownership uncertain; game runtime left unchanged)")
                    continue
                }
                Move-Item -LiteralPath $orig -Destination $target -Force
                $restored.Add("$target  (restored original game $crtDll from .orig backup)")
            } catch {
                $errors.Add("$orig : $($_.Exception.Message)")
            }
        }
    }
}
foreach ($path in ($customPluginFiles | Select-Object -Unique)) {
    $fullPlugin = [IO.Path]::GetFullPath($path)
    if ([IO.Path]::GetFileName($fullPlugin) -notin @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll')) { continue }
    $baseRoot = [IO.Path]::GetFullPath($game).TrimEnd('\')
    if ($fullPlugin -ieq $baseRoot -or $fullPlugin.StartsWith($baseRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-SafeFile $path 'project-dependency'
    } else {
        try {
            $safeExtPlugin = Assert-LmxxfUnlinkedPath $path
            if ((Test-Path -LiteralPath $safeExtPlugin -PathType Leaf) -and
                ([IO.Path]::GetFileName($safeExtPlugin) -in @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll'))) {
                $expectedExtSha = $null
                foreach ($k in $recordedExternalPlugins.Keys) {
                    if ($k -ieq $safeExtPlugin) { $expectedExtSha = $recordedExternalPlugins[$k]; break }
                }
                $actualExtSha = (Get-CrtHash $safeExtPlugin).Replace('-', '').ToLowerInvariant()
                if ($expectedExtSha -and $actualExtSha -eq $expectedExtSha) {
                    Remove-Item -LiteralPath $safeExtPlugin -Force -ErrorAction Stop
                    $deleted.Add("$safeExtPlugin  (project-dependency)")
                } else {
                    $kept.Add("external plugin modified or unowned during uninstall: $safeExtPlugin")
                }
            }
        } catch {
            $errors.Add("$path : $($_.Exception.Message)")
        }
    }
}
foreach ($path in $legacyLogPaths) { Remove-SafeFile $path 'legacy-backend-log' }


if (-not $keepBackups) {
    foreach ($b in $backupDirs) {
        if (!(Test-UninstallPath $b)) { continue }
        $leaf = Split-Path -Leaf $b
        if ($leaf -notmatch '^(?i)backup-amd-presr' -and $leaf -ine 'dlssnr-amd-backup' -and $leaf -ine 'OptiScaler-compat-backup') { continue }
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
                if ($file.Name -like '*.moved') {
                    $kept.Add("unrestored foreign backup inside backup folder: $($file.FullName)")
                    continue
                }
                if ($file.Name -in @('CC_SILVER_AARDWOLD.bin', 'CC_SMALL_FP8.bin', 'CC_SMALL_FP8_EXPERIMENTAL.bin', 'CC_SILVER_AARDWOLD_OPTIMIZED.bin', 'CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin', 'dlssnr.bin', 'dlssnr-small.bin', 'dlssnr_on_amd_weights.bin', 'nvngx_dlssnr.dll') -or
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
# Keep the receipts and the game-local uninstaller when cleanup needs a retry.
foreach ($root in $roots) {
    if (!(Test-UninstallPath $root)) { continue }
    foreach ($name in $installRecords + $selfLeafNames) {
        Remove-SafeFile (Join-Path $root $name) 'project-file'
        if ($errors.Count -gt 0) { break }
    }
    if ($errors.Count -gt 0) { break }
}
if ($errors.Count -gt 0) {
    foreach ($e in $errors) { Write-Host "  - $e" -ForegroundColor Red }
    Pause-Exit 1
}
Write-Host 'Uninstall SUCCEEDED.' -ForegroundColor Green
Pause-Exit 0
