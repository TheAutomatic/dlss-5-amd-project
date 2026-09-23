<#
.SYNOPSIS
  Synchronize vendored lmxxf source closure from an upstream clone without git cherry-pick.

.DESCRIPTION
  Copies the pinned subset of headers, shaders, and hip *sources* from the upstream
  git ref (default -UpstreamRef origin/main; extracted via git archive so the working
  tree branch cannot poison the copy) into third_party\lmxxf\,
  verifies/applies local compatibility patches, and updates UPSTREAM.md with the commit hash.
  Live shaders/ are mirror-cleaned to top-level *.hlsl only (retired dx12-network is not vendored).
  Upstream does not publish .hsaco on git (release/ is ignored); shipping modules are built
  locally with hip/build-modules.ps1 (default) or supplied via -ModulesPath.
  By default, Development\HIP\hip_d3d12_bridge.h is preserved (pinned & patched).
  Pass -UpdateBridge to overwrite and re-patch it. Fails closed when hip recipes change but
  modules do not, or modules disagree with hip/SHA256SUMS gfx1201, unless -AllowStaleModules.

.PARAMETER UpstreamPath
  Path to the cloned upstream repository. Default: '..\dlss5-on-amd-9070xt-porting'.

.PARAMETER UpstreamRef
  Git ref inside the upstream clone to sync from (default: origin/main).
  File contents are taken via `git archive` of this ref into a temp tree, so a dirty or
  wrong-branch working tree cannot poison the vendor copy.

.PARAMETER SkipUpstreamFetch
  Skip `git fetch` before resolving UpstreamRef (use when already fetched / offline SHA).

.PARAMETER AllowOfflineUpstream
  If fetch fails (proxy/network), warn and continue with the local UpstreamRef instead of failing.

.PARAMETER SkipModules
  If set, do not refresh third_party\lmxxf\modules\.
  Refused when hip recipes changed unless -AllowStaleModules is also set.

.PARAMETER ModulesPath
  Optional directory of flat gfx1201 .hsaco (already built). Upstream git never ships these;
  use this when you built elsewhere or unpacked a non-git author package.

.PARAMETER NoBuildModules
  Do not run hip/build-modules.ps1. Requires -ModulesPath (or -SkipModules).

.PARAMETER AllowStaleModules
  Permit finishing when hip recipes changed but modules were skipped/unchanged, or when
  modules do not match hip/SHA256SUMS gfx1201 entries. Default is fail-closed.

.PARAMETER UpdateBridge
  If set, overwrites Development\HIP\hip_d3d12_bridge.h from upstream and re-applies
  all local patches (zero fallback, drain check, clear resource management).

.PARAMETER SkipBuild
  If set, skips the post-sync compilation verification of LmxxfNrRuntime.dll.

.EXAMPLE
  .\tools\sync-lmxxf-upstream.ps1
  .\tools\sync-lmxxf-upstream.ps1 -UpstreamRef origin/main
  .\tools\sync-lmxxf-upstream.ps1 -UpstreamRef 7ef24e7c1498bce59738277e174249866608c4ed -SkipUpstreamFetch
  .\tools\sync-lmxxf-upstream.ps1 -ModulesPath 'D:\built\gfx1201'
  .\tools\sync-lmxxf-upstream.ps1 -SkipModules -AllowStaleModules
  .\tools\sync-lmxxf-upstream.ps1 -UpdateBridge
  .\tools\sync-lmxxf-upstream.ps1 -AllowOfflineUpstream
#>
[CmdletBinding()]
param(
    [string]$UpstreamPath = '..\dlss5-on-amd-9070xt-porting',
    [string]$UpstreamRef = 'origin/main',
    [switch]$SkipUpstreamFetch,
    [switch]$AllowOfflineUpstream,
    [switch]$SkipModules,
    [string]$ModulesPath = '',
    [switch]$NoBuildModules,
    [switch]$AllowStaleModules,
    [switch]$UpdateBridge,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$vendorRoot = Join-Path $root 'third_party\lmxxf'

$upstream = Resolve-Path (Join-Path $root $UpstreamPath) -ErrorAction SilentlyContinue
if (-not $upstream -or -not (Test-Path $upstream)) {
    throw "Upstream repository not found at '$UpstreamPath'. Please clone or specify -UpstreamPath."
}

Write-Host "Syncing from upstream: $upstream (ref: $UpstreamRef)" -ForegroundColor Cyan

# Temp extract of UpstreamRef (git archive). Cleared in finally; never checks out the clone.
$script:UpstreamArchiveRoot = $null
$script:UpstreamTree = $null

function Get-FileSha256Hex([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try {
            return (-join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }))
        } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Get-TreeFingerprint([string]$dir, [string[]]$filters) {
    if (-not (Test-Path -LiteralPath $dir)) { return 'missing' }
    $files = @()
    foreach ($f in $filters) {
        $files += @(Get-ChildItem -LiteralPath $dir -File -Filter $f -ErrorAction SilentlyContinue)
    }
    $files = @($files | Sort-Object { $_.Name.ToLowerInvariant() } -Unique)
    if ($files.Count -lt 1) { return 'empty' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $ms = New-Object IO.MemoryStream
    try {
        foreach ($file in $files) {
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($file.Name + ':' + $file.Length + ':')
            $ms.Write($nameBytes, 0, $nameBytes.Length)
            $payload = [IO.File]::ReadAllBytes($file.FullName)
            $ms.Write($payload, 0, $payload.Length)
        }
        return (-join ($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString('x2') }))
    } finally {
        $ms.Dispose()
        $sha.Dispose()
    }
}

function Resolve-ModulesPath([string]$override) {
    # Upstream git never publishes .hsaco (release/ is gitignored). Only an explicit path counts.
    if (-not $override) { return $null }
    if (-not (Test-Path -LiteralPath $override -PathType Container)) {
        throw ("ModulesPath not found: " + $override)
    }
    return (Resolve-Path -LiteralPath $override).Path
}

function Invoke-BuildGfx1201Modules([string]$hipDir, [string]$outDir) {
    $buildPs1 = Join-Path $hipDir 'build-modules.ps1'
    if (-not (Test-Path -LiteralPath $buildPs1 -PathType Leaf)) {
        throw ("Missing " + $buildPs1 + "; cannot build shipping .hsaco")
    }
    $compiler = Join-Path $hipDir 'rtc_compile.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        $rtcCpp = Join-Path $hipDir 'rtc_compile.cpp'
        if (-not (Test-Path -LiteralPath $rtcCpp -PathType Leaf)) {
            throw ("Missing rtc_compile.exe / rtc_compile.cpp under " + $hipDir)
        }
        Write-Host "  Building rtc_compile.exe from rtc_compile.cpp..." -ForegroundColor Cyan
        & cl.exe /nologo /O2 /EHsc /Fe:$compiler $rtcCpp
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
            throw "Failed to build rtc_compile.exe (need MSVC cl in PATH)"
        }
    }
    if (Test-Path -LiteralPath $outDir) {
        Remove-Item -LiteralPath $outDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    Write-Host ("  Building gfx1201 modules via build-modules.ps1 -> " + $outDir) -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildPs1 -OutputDir $outDir -Compiler $compiler -SourceDir $hipDir -Targets gfx1201
    if ($LASTEXITCODE -ne 0) {
        throw ("build-modules.ps1 failed with ExitCode " + $LASTEXITCODE)
    }
    $built = @(Get-ChildItem -LiteralPath $outDir -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
    if ($built.Count -lt 1) {
        throw ("build-modules.ps1 produced no .hsaco under " + $outDir)
    }
    return $outDir
}

function Assert-ModulesMatchHipSums([string]$modulesDir, [string]$hipSums, [switch]$allowStale) {
    if (-not (Test-Path -LiteralPath $hipSums -PathType Leaf)) {
        Write-Warning '  hip SHA256SUMS missing; skipped modules-vs-recipe check.'
        return
    }
    $bad = @()
    foreach ($line in Get-Content -LiteralPath $hipSums) {
        if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+gfx1201/(?<n>.+\.hsaco)$') { continue }
        $name = $Matches['n']
        $want = $Matches['h'].ToLowerInvariant()
        $fp = Join-Path $modulesDir $name
        if (-not (Test-Path -LiteralPath $fp -PathType Leaf)) {
            $bad += ('missing ' + $name)
            continue
        }
        $got = Get-FileSha256Hex $fp
        if ($got -ne $want) {
            $bad += ($name + ' (want ' + $want + ' got ' + $got + ')')
        }
    }
    if ($bad.Count -eq 0) {
        Write-Host '  modules match hip/SHA256SUMS gfx1201 entries' -ForegroundColor Green
        return
    }
    $msg = "Shipping modules do not match hip/SHA256SUMS gfx1201 recipes:`n  - " + ($bad -join "`n  - ")
    if ($allowStale) {
        Write-Warning $msg
        return
    }
    throw ($msg + "`nRebuild with hip/build-modules.ps1, pass a matching -ModulesPath, or use -AllowStaleModules.")
}

function Sync-LmxxfModules([string]$srcDir, [string]$dstDir, [string]$commitHash) {
    if (-not (Test-Path -LiteralPath $dstDir)) {
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    }
    $hsacos = @(Get-ChildItem -LiteralPath $srcDir -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
    if ($hsacos.Count -lt 1) {
        throw ('No .hsaco files found in modules source: ' + $srcDir)
    }
    foreach ($f in $hsacos) {
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dstDir $f.Name) -Force
    }
    foreach ($extra in @('modules.json', 'runtime-manifest.json')) {
        $srcExtra = Join-Path $srcDir $extra
        if (Test-Path -LiteralPath $srcExtra -PathType Leaf) {
            Copy-Item -LiteralPath $srcExtra -Destination (Join-Path $dstDir $extra) -Force
        }
    }
    $sumsDst = Join-Path $dstDir 'SHA256SUMS'
    $srcSums = Join-Path $srcDir 'SHA256SUMS'
    $wroteSums = $false
    if (Test-Path -LiteralPath $srcSums -PathType Leaf) {
        $lines = @(Get-Content -LiteralPath $srcSums | Where-Object {
            ($_ -match '^[0-9a-fA-F]{64}\s+\*?([^\\/]+)$') -and ($_ -match '\.hsaco\s*$')
        })
        if ($lines.Count -gt 0) {
            [IO.File]::WriteAllText($sumsDst, (($lines -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
            $wroteSums = $true
        }
    }
    if (-not $wroteSums) {
        $parentSums = Join-Path (Split-Path -Parent $srcDir) 'SHA256SUMS'
        if (Test-Path -LiteralPath $parentSums -PathType Leaf) {
            $leaf = Split-Path -Leaf $srcDir
            $mapped = @()
            foreach ($line in Get-Content -LiteralPath $parentSums) {
                if ($line -match '^(?<h>[0-9a-fA-F]{64})\s+\*?(?<p>.+)$') {
                    $p = ($Matches['p'] -replace '\\', '/')
                    $prefix = $leaf + '/'
                    if ($p.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and $p.EndsWith('.hsaco', [StringComparison]::OrdinalIgnoreCase)) {
                        $name = $p.Substring($prefix.Length)
                        if ($name -notmatch '[/\\]') {
                            $mapped += ($Matches['h'].ToLowerInvariant() + '  ' + $name)
                        }
                    }
                }
            }
            if ($mapped.Count -gt 0) {
                [IO.File]::WriteAllText($sumsDst, (($mapped -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
                $wroteSums = $true
            }
        }
    }
    if (-not $wroteSums) {
        $mapped = @()
        foreach ($f in ($hsacos | Sort-Object Name)) {
            $mapped += ((Get-FileSha256Hex $f.FullName) + '  ' + $f.Name)
        }
        [IO.File]::WriteAllText($sumsDst, (($mapped -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
    }
    $readme = Join-Path $dstDir 'README.md'
    if (Test-Path -LiteralPath $readme -PathType Leaf) {
        $md = Get-Content -LiteralPath $readme -Raw
        $md2 = [regex]::Replace($md, '(?m)^(- \*\*Commit Base\*\*: `)[^`]+(`)', '${1}' + $commitHash + '${2}')
        if ($md2 -eq $md) {
            $md2 = [regex]::Replace($md, '(?m)^(- \*\*Commit Base\*\*: ).*$', '${1}`' + $commitHash + '`')
        }
        if ($md2 -ne $md) {
            [IO.File]::WriteAllText($readme, $md2, [Text.UTF8Encoding]::new($false))
        }
    }
    Write-Host ('  Synchronized modules (' + $hsacos.Count + ' .hsaco) from ' + $srcDir) -ForegroundColor Green
}


$dstHip = Join-Path $vendorRoot 'hip'
$dstModules = Join-Path $vendorRoot 'modules'
$hipFpBefore = Get-TreeFingerprint $dstHip @('*.hip', 'SHA256SUMS')
$modulesFpBefore = Get-TreeFingerprint $dstModules @('*.hsaco', 'SHA256SUMS')

try {
# 1. Resolve UpstreamRef (fail-closed) and extract via git archive (never checks out the clone)
$upstreamGitPath = $upstream.Path.Replace('\', '/')
$gitSafe = @('-c', "safe.directory=$upstreamGitPath")

if (-not $SkipUpstreamFetch) {
    $fetchRemote = 'origin'
    if ($UpstreamRef -match '^([^/]+)/.+') {
        $fetchRemote = $Matches[1]
    }
    Write-Host ("Fetching {0} (for {1})..." -f $fetchRemote, $UpstreamRef) -ForegroundColor Cyan
    & git @gitSafe -C $upstream.Path fetch $fetchRemote 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        $fetchMsg = "git fetch $fetchRemote failed (proxy/network?). Pass -AllowOfflineUpstream to use local ref '$UpstreamRef', or -SkipUpstreamFetch if you already fetched."
        if ($AllowOfflineUpstream) {
            Write-Warning $fetchMsg
        } else {
            throw $fetchMsg
        }
    }
} else {
    Write-Host '  Skipped upstream fetch (-SkipUpstreamFetch)' -ForegroundColor DarkYellow
}

$commitResult = & git @gitSafe -C $upstream.Path rev-parse "$UpstreamRef^{commit}" 2>$null
if ($LASTEXITCODE -ne 0 -or -not $commitResult) {
    throw "Could not resolve upstream ref '$UpstreamRef'; refusing to record an unpinned vendor snapshot."
}
$commitHash = $commitResult.Trim()

$headResult = & git @gitSafe -C $upstream.Path rev-parse HEAD 2>$null
$headHash = if ($LASTEXITCODE -eq 0 -and $headResult) { $headResult.Trim() } else { '(unknown)' }
$headBranch = (& git @gitSafe -C $upstream.Path branch --show-current 2>$null)
if (-not $headBranch) { $headBranch = '(detached)' }
if ($headHash -ne $commitHash) {
    Write-Host ("  Upstream worktree HEAD is {0} ({1}); syncing pinned ref {2} ({3}) via git archive (worktree not checked out)." -f $headBranch, $headHash, $UpstreamRef, $commitHash) -ForegroundColor DarkYellow
} else {
    Write-Host ("Upstream ref {0} => {1}" -f $UpstreamRef, $commitHash) -ForegroundColor Green
}

$script:UpstreamArchiveRoot = Join-Path ([IO.Path]::GetTempPath()) ('lmxxf-sync-' + [guid]::NewGuid().ToString('N'))
$script:UpstreamTree = Join-Path $script:UpstreamArchiveRoot 'tree'
New-Item -ItemType Directory -Force -Path $script:UpstreamTree | Out-Null
$archiveTar = Join-Path $script:UpstreamArchiveRoot 'upstream.tar'
$archivePaths = @(
    'Development/HIP/hip_api.h',
    'Development/HIP/hip_d3d12_bridge.h',
    'Development/HIP/hip_device_properties.h',
    'Development/HIP/hip_reference_network.h',
    'Development/HIP/packed_weights.h',
    'src/native_device_identity.h',
    'src/native_game_codec.h',
    'src/native_game_rgb_input.h',
    'src/native_hip_network.h',
    'src/native_input_geometry.h',
    'src/native_lab_paths.h',
    'src/native_network_geometry.h',
    'src/native_pinned_resource.h',
    'src/native_pso.h',
    'src/native_rgb_reflect.h',
    'src/native_rgb_texture.h',
    'src/native_shader_cache.h',
    'shaders',
    'hip'
)
& git @gitSafe -C $upstream.Path archive --format=tar -o $archiveTar $commitHash -- @archivePaths
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archiveTar)) {
    throw "git archive of '$UpstreamRef' ($commitHash) failed; cannot sync."
}
Push-Location $script:UpstreamTree
try {
    & tar -xf $archiveTar
    if ($LASTEXITCODE -ne 0) {
        throw "tar extract of upstream archive failed (exit $LASTEXITCODE)."
    }
} finally {
    Pop-Location
}
Write-Host ("  Extracted pinned tree to temp ({0})" -f $script:UpstreamTree) -ForegroundColor Cyan

# 1b. Decide how shipping modules will be refreshed (upstream git has no .hsaco)
$script:ResolvedModulesSrc = $null
if (-not $SkipModules) {
    $script:ResolvedModulesSrc = Resolve-ModulesPath -override $ModulesPath
    if ($script:ResolvedModulesSrc) {
        $probe = @(Get-ChildItem -LiteralPath $script:ResolvedModulesSrc -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
        if ($probe.Count -lt 1) {
            throw ('No .hsaco files found in ModulesPath: ' + $script:ResolvedModulesSrc)
        }
        Write-Host ('  ModulesPath: ' + $script:ResolvedModulesSrc + ' (' + $probe.Count + ' .hsaco)') -ForegroundColor Cyan
    } elseif ($NoBuildModules) {
        throw 'No ModulesPath and -NoBuildModules set. Pass -ModulesPath, omit -NoBuildModules to build, or use -SkipModules -AllowStaleModules.'
    } else {
        Write-Host '  Modules: will build gfx1201 locally after hip recipes sync (upstream git has no release hsaco).' -ForegroundColor Cyan
    }
}

# 2. Synchronize selected headers (from archived UpstreamRef tree)
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
    if ($rel -eq 'Development\HIP\hip_d3d12_bridge.h' -and -not $UpdateBridge) {
        Write-Host "  Preserved (pinned & patched): $rel (pass -UpdateBridge to overwrite and re-patch)" -ForegroundColor DarkYellow
        continue
    }
    $src = Join-Path $script:UpstreamTree $rel
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

# 3. Synchronize shaders — live D3D12 glue only (top-level *.hlsl).
# Upstream keeps retired network-body hlsl under shaders/dx12-network/; do NOT vendor that tree.
# Safer than robocopy /PURGE: copy the live set, then delete any dst *.hlsl not in that set
# (preserves non-hlsl such as shader-cache/*.dxbc). No local-only hlsl is expected.
$shaderDir = Join-Path $script:UpstreamTree 'shaders'
$dstShaders = Join-Path $vendorRoot 'shaders'
if (-not (Test-Path -LiteralPath $dstShaders)) {
    New-Item -ItemType Directory -Force -Path $dstShaders | Out-Null
}
$liveShaderFiles = @()
if (Test-Path -LiteralPath $shaderDir) {
    $liveShaderFiles = @(Get-ChildItem -LiteralPath $shaderDir -Filter '*.hlsl' -File -ErrorAction SilentlyContinue)
}
$liveNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($sf in $liveShaderFiles) {
    [void]$liveNames.Add($sf.Name)
    Copy-Item -LiteralPath $sf.FullName -Destination (Join-Path $dstShaders $sf.Name) -Force
}
$removedShaders = 0
if (Test-Path -LiteralPath $dstShaders) {
    Get-ChildItem -LiteralPath $dstShaders -Filter '*.hlsl' -File -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not $liveNames.Contains($_.Name)) {
            Remove-Item -LiteralPath $_.FullName -Force
            $removedShaders++
        }
    }
}
Write-Host ("  Synchronized {0} live shaders (removed {1} retired *.hlsl; dx12-network not copied)" -f $liveNames.Count, $removedShaders)

# 4. Synchronize hip recipes (from archived UpstreamRef tree)
$hipDir = Join-Path $script:UpstreamTree 'hip'
if (Test-Path $hipDir) {
    $dstHip = Join-Path $vendorRoot 'hip'
    robocopy $hipDir $dstHip *.hip build-modules.ps1 rtc_compile.cpp SHA256SUMS README.md /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    Write-Host "  Synchronized hip recipes"
}

# 4b. Refresh shipping gfx1201 modules (build locally or -ModulesPath; never from upstream git release/)
if ($SkipModules) {
    Write-Host "  Skipped modules sync (-SkipModules)" -ForegroundColor Yellow
} else {
    $modulesSrc = $script:ResolvedModulesSrc
    if (-not $modulesSrc) {
        $buildOut = Join-Path $dstHip '_build_gfx1201'
        $modulesSrc = Invoke-BuildGfx1201Modules -hipDir $dstHip -outDir $buildOut
    }
    Sync-LmxxfModules -srcDir $modulesSrc -dstDir $dstModules -commitHash $commitHash
    Assert-ModulesMatchHipSums -modulesDir $dstModules -hipSums (Join-Path $dstHip 'SHA256SUMS') -allowStale:$AllowStaleModules
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

# Patch B: hip_d3d12_bridge.h (ClearOutput, WaitForSubmittedWork completion check, zero upload, and CancelUnsubmitted)
$bridgeH = Join-Path $vendorRoot 'Development\HIP\hip_d3d12_bridge.h'
if (Test-Path $bridgeH) {
    if (-not $UpdateBridge) {
        Write-Host "  Preserved Patch B: hip_d3d12_bridge.h is pinned (pass -UpdateBridge to re-patch)" -ForegroundColor DarkYellow
    } else {
        $content = Get-Content -LiteralPath $bridgeH -Raw

        # B.0: Ensure #include <algorithm>
        if ($content -notmatch '#include\s*<algorithm>') {
            if ($content -notmatch '(?m)^#include\s*<chrono>') {
                throw "Patch B failed: cannot find anchor '#include <chrono>' in hip_d3d12_bridge.h"
            }
            $content = $content -replace '(?m)^(#include\s*<chrono>)', "#include <algorithm>`r`n`$1"
        }

        # B.1: Member variables for clear resources
        if ($content -notmatch 'ID3D12Resource\*\s*zero_upload') {
            $varAnchor = 'HANDLE fence_handle{},event{};Handle semaphore{};Shared input,history,output;UINT64 value{};size_t pixels{};bool readable{},pending{},failed{};'
            if (-not $content.Contains($varAnchor)) {
                throw "Patch B failed: cannot find member variables anchor in hip_d3d12_bridge.h"
            }
            $content = $content.Replace($varAnchor, "$varAnchor`r`n ID3D12Resource* zero_upload{};ID3D12CommandAllocator* clear_alloc{};ID3D12GraphicsCommandList* clear_cmd{};`r`n size_t zero_upload_bytes{};bool clear_submission_unconfirmed{};")
        }

        # B.2: WaitForSubmittedWork fence completed value check
        if ($content -notmatch 'fence->GetCompletedValue\(\)<target') {
            $waitAnchor = 'if(pending&&queue&&fence){auto target=++value;if(FAILED(queue->Signal(fence,target))||FAILED(fence->SetEventOnCompletion(target,event))||WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return false;}'
            if (-not $content.Contains($waitAnchor)) {
                throw "Patch B failed: cannot find WaitForSubmittedWork anchor in hip_d3d12_bridge.h"
            }
            $waitReplacement = 'if(pending&&queue&&fence){auto target=++value;if(FAILED(queue->Signal(fence,target))||FAILED(fence->SetEventOnCompletion(target,event))||WaitForSingleObject(event,30000)!=WAIT_OBJECT_0||fence->GetCompletedValue()<target)return false;}'
            $content = $content.Replace($waitAnchor, $waitReplacement)
        }

        # B.3: Retain GPU-live resources after an unconfirmed clear submission
        $phaseAnchor = 'if(phase!=Phase::Ready)return false;'
        if ($content.Contains($phaseAnchor)) {
            $content = $content.Replace($phaseAnchor, 'if(phase!=Phase::Ready||clear_submission_unconfirmed)return false;')
        }

        # B.4: Destructor cleanup of clear resources
        if ($content -notmatch 'clear_cmd->Release') {
            $dtorAnchor = "~D3D12Bridge(){`r`n  if(!WaitForSubmittedWork())return;"
            if (-not $content.Contains($dtorAnchor)) {
                $dtorAnchorLf = "~D3D12Bridge(){\n  if(!WaitForSubmittedWork())return;"
                if (-not $content.Contains($dtorAnchorLf)) {
                    throw "Patch B failed: cannot find destructor anchor in hip_d3d12_bridge.h"
                }
                $dtorAnchor = $dtorAnchorLf
            }
            $dtorReplacement = "$dtorAnchor`r`n  if(clear_cmd)clear_cmd->Release();if(clear_alloc)clear_alloc->Release();if(zero_upload)zero_upload->Release();"
            $content = $content.Replace($dtorAnchor, $dtorReplacement)
        }

        # B.5: Lazy zero upload and clear cmd/alloc initialization
        if ($content -notmatch 'bool EnsureZeroClearResources\(\) noexcept') {
            $inputAnchor = 'void InputContract(ID3D12Resource*r)'
            if (-not $content.Contains($inputAnchor)) {
                throw "Patch B failed: cannot find InputContract anchor in hip_d3d12_bridge.h"
            }
            $initCode = @'
bool EnsureZeroClearResources() noexcept {
  if(zero_upload&&clear_alloc&&clear_cmd)return true;
  if(!device||!pixels)return false;
  try{
   zero_upload_bytes=std::min<size_t>(pixels*12,65536);
   D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
   D3D12_RESOURCE_DESC ud{};ud.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;ud.Width=zero_upload_bytes;ud.Height=1;ud.DepthOrArraySize=ud.MipLevels=1;ud.SampleDesc.Count=1;ud.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ud.Flags=D3D12_RESOURCE_FLAG_NONE;
   Check(device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&ud,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&zero_upload)),"zero upload buffer");
   void*mappedZero=nullptr;D3D12_RANGE r{0,0};Check(zero_upload->Map(0,&r,&mappedZero),"map zero upload buffer");
   if(!mappedZero){zero_upload->Unmap(0,nullptr);throw std::runtime_error("map zero upload buffer returned null");}
   std::memset(mappedZero,0,zero_upload_bytes);zero_upload->Unmap(0,nullptr);
   Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&clear_alloc)),"clear allocator");
   Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,clear_alloc,nullptr,IID_PPV_ARGS(&clear_cmd)),"clear command list");Check(clear_cmd->Close(),"close clear command list");
   return true;
  }catch(...){
   if(clear_cmd){clear_cmd->Release();clear_cmd=nullptr;}
   if(clear_alloc){clear_alloc->Release();clear_alloc=nullptr;}
   if(zero_upload){zero_upload->Release();zero_upload=nullptr;}
   zero_upload_bytes=0;
   return false;
  }
 }
'@
            $content = $content.Replace($inputAnchor, "$initCode`r`n $inputAnchor")
        }

        # B.6: ClearOutputAsync / ClearOutputD3D12 / ClearOutput methods
        if ($content -notmatch 'ClearOutputAsync') {
            $notifyCommentAnchor = '// Acknowledges submission, not GPU completion.'
            $notifyAnchor = 'void NotifyOutputSubmitted(ID3D12CommandQueue*consumer)'
            if ($content.Contains($notifyCommentAnchor)) {
                $targetAnchor = $notifyCommentAnchor
            } elseif ($content.Contains($notifyAnchor)) {
                $targetAnchor = $notifyAnchor
            } else {
                throw "Patch B failed: cannot find 'void NotifyOutputSubmitted' anchor in hip_d3d12_bridge.h"
            }
            $clearMethods = @'
private:
 bool ClearOutputAsync() noexcept {
  if(!network||failed||!output.mapped)return false;
  auto&api=network->Runtime();
  try{
   api.Check(api.hipMemsetAsync(output.mapped,0,pixels*12,network->Stream()),"clear output");
   network->Synchronize();
   return true;
  }catch(...){
   failed=true;
   return false;
  }
 }
 bool ClearOutputD3D12(ID3D12CommandQueue* targetQueue) noexcept {
  if(!network||!device||!targetQueue||!output.resource||clear_submission_unconfirmed)return false;
  // A failed HIP call can leave earlier work queued. Do not race that work with
  // a D3D12 write to the same shared buffer.
  if(network->Runtime().hipStreamSynchronize(network->Stream())!=0)return false;
  ID3D12Device* owner=nullptr;
  if(FAILED(targetQueue->GetDevice(IID_PPV_ARGS(&owner)))||!owner)return false;
  const bool sameDevice=NativeSameDevice(owner,device);owner->Release();
  if(!sameDevice)return false;
  if(!EnsureZeroClearResources())return false;
  ID3D12CommandAllocator* alloc=clear_alloc;
  ID3D12GraphicsCommandList* cmd=clear_cmd;
  ID3D12Fence* completion=nullptr;
  HANDLE completedEvent=nullptr;
  bool temp=false;
  bool submitted=false;
  const auto qType=targetQueue->GetDesc().Type;
  if(qType!=D3D12_COMMAND_LIST_TYPE_DIRECT||!alloc||!cmd){
   if(qType!=D3D12_COMMAND_LIST_TYPE_DIRECT&&qType!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return false;
   if(FAILED(device->CreateCommandAllocator(qType,IID_PPV_ARGS(&alloc))))return false;
   if(FAILED(device->CreateCommandList(0,qType,alloc,nullptr,IID_PPV_ARGS(&cmd)))){alloc->Release();return false;}
   temp=true;
  }else if(FAILED(alloc->Reset())||FAILED(cmd->Reset(alloc,nullptr))){return false;}
  bool ok=false;
  try{
   if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion))))throw std::runtime_error("clear fence");
   completedEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
   if(!completedEvent)throw std::runtime_error("clear event");
   Barrier(cmd,output.resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
   const UINT64 total=UINT64(pixels)*12;
   for(UINT64 offset=0;offset<total;offset+=zero_upload_bytes)
    cmd->CopyBufferRegion(output.resource,offset,zero_upload,0,std::min<UINT64>(zero_upload_bytes,total-offset));
   Barrier(cmd,output.resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
   Check(cmd->Close(),"close clear command list");
   ID3D12CommandList* lists[]={cmd};
   targetQueue->ExecuteCommandLists(1,lists);
   submitted=true;clear_submission_unconfirmed=true;
   Check(targetQueue->Signal(completion,1),"signal clear completion");
   Check(completion->SetEventOnCompletion(1,completedEvent),"wait for clear completion");
   ok=WaitForSingleObject(completedEvent,30000)==WAIT_OBJECT_0&&completion->GetCompletedValue()>=1&&SUCCEEDED(device->GetDeviceRemovedReason());
   if(ok)clear_submission_unconfirmed=false;
  }catch(...){ok=false;}
  // SetEventOnCompletion can still signal after a timeout. Keep its fence and
  // event alive whenever the submitted work has not been confirmed complete.
  if(!submitted||ok){if(completedEvent)CloseHandle(completedEvent);if(completion)completion->Release();}
  // If submission completion is unknown, retain command storage until the
  // session's fail-closed teardown instead of freeing a GPU-live allocator.
  if(temp&&(!submitted||ok)){cmd->Release();alloc->Release();}
  return ok;
 }
 public:
 // After producer submission and before consumer submission, clear the private
 // neural output so the caller can decode original Color. The caller must drain
 // any other queue that used Output() before calling this method, and drain a
 // different consumer queue before reusing or destroying the bridge. On false,
 // do not submit the consumer or reuse the bridge.
 bool ClearOutput(ID3D12CommandQueue* targetQueue) noexcept {
  if(phase!=Phase::InputRecorded&&phase!=Phase::OutputRecordedPendingHip)return false;
  if(!targetQueue||!device)return false;
  const auto qType=targetQueue->GetDesc().Type;
  if(qType!=D3D12_COMMAND_LIST_TYPE_DIRECT&&qType!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return false;
  ID3D12Device* owner=nullptr;
  if(FAILED(targetQueue->GetDevice(IID_PPV_ARGS(&owner)))||!owner)return false;
  const bool sameDevice=NativeSameDevice(owner,device);owner->Release();
  if(!sameDevice)return false;
  const bool consumer_recorded=phase==Phase::OutputRecordedPendingHip;
  if(!ClearOutputAsync()&&!ClearOutputD3D12(targetQueue))return false;
  // The stream and clear queue are confirmed complete. A pre-recorded consumer
  // can now submit; otherwise RecordOutputReadable may still be called.
  failed=false;
  phase=consumer_recorded?Phase::OutputRecorded:Phase::HipQueued;
  return true;
 }
'@
            $content = $content.Replace($targetAnchor, "$clearMethods`r`n $targetAnchor")
        }

        # B.7: NotifyOutputSubmittedIfRecorded failure-safe reset
        $notifyIfAnchor = 'void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer)NotifyOutputSubmitted(consumer);}'
        if ($content.Contains($notifyIfAnchor)) {
            $notifyIfReplacement = 'void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer){if(failed){phase=Phase::Ready;return;}NotifyOutputSubmitted(consumer);}}'
            $content = $content.Replace($notifyIfAnchor, $notifyIfReplacement)
        }

        # B.8: CancelUnsubmitted and CurrentPhase (for older upstream commits if missing)
        if ($content -notmatch 'CancelUnsubmitted') {
            if ($content -match 'enum class Phase \{ Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded \};') {
                $content = $content -replace 'enum class Phase \{ Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded \};',
                    "public:`r`n enum class Phase { Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded };`r`n Phase CurrentPhase()const{return phase;}`r`nprivate:"
            }
            if ($content -notmatch 'void CancelUnsubmitted') {
                $marker = 'void NotifyOutputSubmitted(ID3D12CommandQueue*consumer){Require(Phase::OutputRecorded);QueueContract(consumer);phase=Phase::Ready;}'
                $replacement = "$marker`r`n void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer){if(failed){phase=Phase::Ready;return;}NotifyOutputSubmitted(consumer);}}`r`n void CancelUnsubmitted(){if(phase==Phase::InputRecorded||phase==Phase::OutputRecordedPendingHip){phase=Phase::Ready;readable=false;}}"
                $content = $content.Replace($marker, $replacement)
            }
        }

        [IO.File]::WriteAllText($bridgeH, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: local extensions to hip_d3d12_bridge.h" -ForegroundColor Yellow
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

# 7. Post-sync build verification
if (-not $SkipBuild) {
    Write-Host "Verifying runtime build: tools\build-lmxxf-runtime.cmd..." -ForegroundColor Cyan
    $buildCmd = Join-Path $root 'tools\build-lmxxf-runtime.cmd'
    if (Test-Path -LiteralPath $buildCmd) {
        & cmd.exe /c "`"$buildCmd`""
        if ($LASTEXITCODE -ne 0) {
            throw "Post-sync build verification FAILED! LmxxfNrRuntime.dll failed to compile (ExitCode $LASTEXITCODE)."
        }
        Write-Host "  Runtime build verified successfully (ExitCode 0)." -ForegroundColor Green
    } else {
        Write-Warning "  build-lmxxf-runtime.cmd not found, skipping build verification."
    }
} else {
    Write-Host "Skipping runtime build verification (-SkipBuild specified)." -ForegroundColor Yellow
}

# 8. Fail closed if hip recipes moved but shipping modules did not
$hipFpAfter = Get-TreeFingerprint $dstHip @('*.hip', 'SHA256SUMS')
$modulesFpAfter = Get-TreeFingerprint $dstModules @('*.hsaco', 'SHA256SUMS')
$hipChanged = ($hipFpBefore -ne $hipFpAfter)
$modulesChanged = ($modulesFpBefore -ne $modulesFpAfter)
if ($hipChanged -and -not $modulesChanged) {
    if ($AllowStaleModules) {
        Write-Warning 'hip recipes changed but modules fingerprint is unchanged (-AllowStaleModules).'
    } else {
        throw @"
hip recipes changed but third_party\lmxxf\modules did not.
Refusing to finish sync so release packaging cannot ship stale .hsaco with new .hip sources.
Refresh modules (default path), pass -ModulesPath to a rebuilt gfx1201 dir, or use -AllowStaleModules.
"@
    }
}
if ($hipChanged -and $SkipModules -and -not $AllowStaleModules) {
    throw 'hip recipes changed; -SkipModules requires -AllowStaleModules.'
}
if ((-not $hipChanged) -and $SkipModules) {
    Write-Host '  hip unchanged; -SkipModules accepted.' -ForegroundColor DarkYellow
}

Write-Host "Sync complete! Upstream commit: $commitHash" -ForegroundColor Green
}
finally {
    if ($script:UpstreamArchiveRoot -and (Test-Path -LiteralPath $script:UpstreamArchiveRoot)) {
        Remove-Item -LiteralPath $script:UpstreamArchiveRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
