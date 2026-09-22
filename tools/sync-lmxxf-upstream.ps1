<#
.SYNOPSIS
  Synchronize vendored lmxxf source closure from an upstream clone without git cherry-pick.

.DESCRIPTION
  Copies the pinned subset of headers, shaders, and hip sources from the upstream
  repository (by default: ..\dlss5-on-amd-9070xt-porting) into third_party\lmxxf\,
  verifies/applies local compatibility patches, and updates UPSTREAM.md with the commit hash.

.PARAMETER UpstreamPath
  Path to the cloned upstream repository. Default: '..\dlss5-on-amd-9070xt-porting'.

.PARAMETER SkipModules
  If set, do not update third_party\lmxxf\modules\ from upstream build output.

.EXAMPLE
  .\tools\sync-lmxxf-upstream.ps1
  .\tools\sync-lmxxf-upstream.ps1 -UpstreamPath 'D:\repos\dlss5-on-amd-9070xt-porting'
#>
[CmdletBinding()]
param(
    [string]$UpstreamPath = '..\dlss5-on-amd-9070xt-porting',
    [switch]$SkipModules
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$vendorRoot = Join-Path $root 'third_party\lmxxf'

$upstream = Resolve-Path (Join-Path $root $UpstreamPath) -ErrorAction SilentlyContinue
if (-not $upstream -or -not (Test-Path $upstream)) {
    throw "Upstream repository not found at '$UpstreamPath'. Please clone or specify -UpstreamPath."
}

Write-Host "Syncing from upstream: $upstream" -ForegroundColor Cyan

# 1. Query git commit of upstream
$commitHash = ''
try {
    $commitHash = (git -C $upstream rev-parse HEAD 2>$null).Trim()
} catch {}
if (-not $commitHash) { $commitHash = 'unknown' }
Write-Host "Upstream HEAD commit: $commitHash" -ForegroundColor Green

# 2. Synchronize selected headers
$headerFiles = @(
    'Development\HIP\hip_api.h',
    'Development\HIP\hip_d3d12_bridge.h',
    'Development\HIP\hip_device_properties.h',
    'Development\HIP\hip_reference_network.h',
    'Development\HIP\packed_weights.h',
    'src\native_device_identity.h',
    'src\native_game_codec.h',
    'src\native_game_rgb_input.h',
    'src\native_hip_network.h',
    'src\native_input_geometry.h',
    'src\native_lab_paths.h',
    'src\native_network_geometry.h',
    'src\native_pinned_resource.h',
    'src\native_pso.h',
    'src\native_rgb_reflect.h',
    'src\native_rgb_texture.h',
    'src\native_shader_cache.h'
)

foreach ($rel in $headerFiles) {
    $src = Join-Path $upstream $rel
    $dst = Join-Path $vendorRoot $rel
    if (Test-Path -LiteralPath $src) {
        $parent = Split-Path -Parent $dst
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Write-Host "  Updated: $rel"
    } else {
        Write-Warning "  Missing in upstream: $rel"
    }
}

# 3. Synchronize shaders
$shaderDir = Join-Path $upstream 'shaders'
if (Test-Path $shaderDir) {
    $dstShaders = Join-Path $vendorRoot 'shaders'
    robocopy $shaderDir $dstShaders *.hlsl /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    Write-Host "  Synchronized shaders"
}

# 4. Synchronize hip recipes
$hipDir = Join-Path $upstream 'hip'
if (Test-Path $hipDir) {
    $dstHip = Join-Path $vendorRoot 'hip'
    robocopy $hipDir $dstHip *.hip build-modules.ps1 rtc_compile.cpp SHA256SUMS README.md /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    Write-Host "  Synchronized hip recipes"
}

# 5. Check and apply local patches
# Patch A: #include <algorithm> in hip_reference_network.h
$refNet = Join-Path $vendorRoot 'Development\HIP\hip_reference_network.h'
if (Test-Path $refNet) {
    $content = Get-Content -LiteralPath $refNet -Raw
    if ($content -notmatch '#include\s*<algorithm>') {
        $content = $content -replace '(#include\s*<vector>)', "`$1`r`n#include <algorithm>"
        [IO.File]::WriteAllText($refNet, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: #include <algorithm> in hip_reference_network.h" -ForegroundColor Yellow
    }
}

# Patch B: CancelUnsubmitted in hip_d3d12_bridge.h
$bridgeH = Join-Path $vendorRoot 'Development\HIP\hip_d3d12_bridge.h'
if (Test-Path $bridgeH) {
    $content = Get-Content -LiteralPath $bridgeH -Raw
    if ($content -notmatch 'CancelUnsubmitted') {
        # Ensure Phase is public
        $content = $content -replace 'enum class Phase \{ Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded \};',
            "public:`r`n enum class Phase { Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded };`r`n Phase CurrentPhase()const{return phase;}`r`nprivate:"
        # Add CancelUnsubmitted and NotifyOutputSubmittedIfRecorded
        $marker = 'void NotifyOutputSubmitted(ID3D12CommandQueue*consumer){Require(Phase::OutputRecorded);QueueContract(consumer);phase=Phase::Ready;}'
        $replacement = "$marker`r`n void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer)NotifyOutputSubmitted(consumer);}`r`n void CancelUnsubmitted(){if(phase==Phase::InputRecorded||phase==Phase::OutputRecordedPendingHip){phase=Phase::Ready;readable=false;}}"
        $content = $content.Replace($marker, $replacement)
        [IO.File]::WriteAllText($bridgeH, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: CancelUnsubmitted() in hip_d3d12_bridge.h" -ForegroundColor Yellow
    }
}

# Patch C: remove unused native_split.h from native_rgb_reflect.h
$reflectH = Join-Path $vendorRoot 'src\native_rgb_reflect.h'
if (Test-Path $reflectH) {
    $content = Get-Content -LiteralPath $reflectH -Raw
    if ($content -match '#include\s*"native_split\.h"') {
        $content = $content -replace '#include\s*"native_split\.h"\r?\n?', ''
        [IO.File]::WriteAllText($reflectH, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: removed native_split.h in native_rgb_reflect.h" -ForegroundColor Yellow
    }
}

# 6. Update UPSTREAM.md with new commit and timestamp
$upstreamMd = Join-Path $vendorRoot 'UPSTREAM.md'
if (Test-Path $upstreamMd) {
    $md = Get-Content -LiteralPath $upstreamMd -Raw
    $today = (Get-Date).ToString('yyyy-MM-dd')
    $md = $md -replace '(?m)^- Commit: .*', "- Commit: ``$commitHash`` (synced $today)"
    [IO.File]::WriteAllText($upstreamMd, $md, [Text.UTF8Encoding]::new($false))
    Write-Host "  Updated UPSTREAM.md" -ForegroundColor Green
}

Write-Host "Sync complete! Upstream commit: $commitHash" -ForegroundColor Green
