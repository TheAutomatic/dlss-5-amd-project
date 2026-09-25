<#
.SYNOPSIS
  Stage a pinned lmxxf source closure, require integration review, then verify the build.
.DESCRIPTION
  Reads one source manifest and applies independent unified patches in a temporary git
  archive. The three local headers are preserved unless their Update switch is supplied.
  Missing required files or patch conflicts fail before vendor files are touched.
  Ordinary headers, hip/*.hip and top-level shaders/*.hlsl are mirrored within their owners.

  Source copying alone never completes a sync. The audit writes a report and a review
  template to exports/lmxxf-upstream; review decisions must match the exact commit and
  local integration inputs. See tools/lmxxf-sync/README.md for the staged workflow.
.PARAMETER UpstreamPath
  Upstream clone. By default locate the clone beside the primary checkout (also in worktrees).
.PARAMETER UpstreamRef
  Ref to resolve once to an immutable commit. Default: origin/main.
.PARAMETER SkipUpstreamFetch
  Use an already fetched ref or offline commit.
.PARAMETER AllowOfflineUpstream
  Allow a failed fetch to use the local ref, with an explicit warning.
.PARAMETER UpdateBridge
  Refresh hip_d3d12_bridge.h and apply patches/bridge.patch; conflicts fail closed.
.PARAMETER UpdateReflect
  Refresh native_rgb_reflect.h and apply patches/reflect.patch.
.PARAMETER UpdateInputGeometry
  Refresh native_input_geometry.h and apply patches/input-geometry.patch.
.PARAMETER ReviewFile
  Reviewed JSON decisions. Default: third_party/lmxxf/upstream-review.json.
.PARAMETER SkipEnablementAudit
  Source staging ONLY, for inspection on a machine without Python. Leaves sync-state.json
  pending, does not advance UPSTREAM.md or print completion. Rerun without this switch
  before considering upstream integrated. No module/runtime builds are attempted in this mode.
.PARAMETER PythonPath
  Optional explicit Python 3.9+ executable for the mandatory audit.
.PARAMETER SkipModules
  Keep existing modules; changed recipes require AllowStaleModules.
.PARAMETER AllowStaleModules
  Explicitly allow stale modules for staged integration; the review must acknowledge this.
.PARAMETER ModulesPath
  Flat gfx1201 module directory built separately. Validate its origin manually in the review.
.PARAMETER NoBuildModules
  Require ModulesPath unless SkipModules is set.
.PARAMETER SkipBuild
  Skip runtime compilation; the review must record why and the remaining validation.
.EXAMPLE
  .\tools\sync-lmxxf-upstream.ps1 -SkipUpstreamFetch
.EXAMPLE
  .\tools\sync-lmxxf-upstream.ps1 -UpdateBridge -ReviewFile third_party/lmxxf/upstream-review.json
#>
[CmdletBinding()]
param(
    [string]$UpstreamPath = '',
    [string]$UpstreamRef = 'origin/main',
    [switch]$SkipUpstreamFetch,
    [switch]$AllowOfflineUpstream,
    [switch]$SkipModules,
    [string]$ModulesPath = '',
    [switch]$NoBuildModules,
    [switch]$AllowStaleModules,
    [switch]$UpdateBridge,
    [switch]$UpdateReflect,
    [switch]$UpdateInputGeometry,
    [switch]$SkipBuild,
    [string]$ReviewFile = '',
    [switch]$SkipEnablementAudit,
    [string]$PythonPath = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vendorRoot = Join-Path $root 'third_party\lmxxf'
$configRoot = Join-Path $PSScriptRoot 'lmxxf-sync'
. (Join-Path $configRoot 'Files.ps1')
. (Join-Path $configRoot 'Modules.ps1')
$manifest = Read-SyncJson (Join-Path $configRoot 'manifest.json')
$updates = @{ UpdateBridge = [bool]$UpdateBridge; UpdateReflect = [bool]$UpdateReflect; UpdateInputGeometry = [bool]$UpdateInputGeometry }
$expectedPinned = @('Development/HIP/hip_d3d12_bridge.h', 'src/native_rgb_reflect.h', 'src/native_input_geometry.h')
if ($manifest.schema -ne 1 -or @($manifest.headers | Select-Object -Unique).Count -ne $manifest.headers.Count) {
    throw 'Invalid or duplicate header entries in lmxxf-sync/manifest.json'
}
if (@($manifest.pinned).Count -ne 3 -or @(Compare-Object $expectedPinned @($manifest.pinned.path)).Count) {
    throw 'Only the three documented local headers may be pinned.'
}
foreach ($path in $manifest.headers) {
    if ($path -notmatch '^(src|Development/HIP)/[^/\\]+\.h$') { throw "Header outside owned closure: $path" }
}
$pinned = @{}
foreach ($spec in $manifest.pinned) {
    if ($spec.path -notin $manifest.headers -or -not $updates.ContainsKey($spec.switch)) { throw 'Invalid pinned manifest entry' }
    $pinned[$spec.path] = $spec
}
if (-not $UpstreamPath) {
    $common = & git -C $root rev-parse --path-format=absolute --git-common-dir
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate primary checkout; specify -UpstreamPath.' }
    $primaryRoot = Split-Path -Parent $common
    $UpstreamPath = Join-Path (Split-Path -Parent $primaryRoot) 'dlss5-on-amd-9070xt-porting'
}
if (-not [IO.Path]::IsPathRooted($UpstreamPath)) { $UpstreamPath = Join-Path $root $UpstreamPath }
$upstream = (Resolve-Path -LiteralPath $UpstreamPath).Path
$gitSafe = @('-c', ('safe.directory=' + $upstream.Replace('\', '/')))
if (-not $ReviewFile) { $ReviewFile = Join-Path $vendorRoot 'upstream-review.json' }
if (-not [IO.Path]::IsPathRooted($ReviewFile)) { $ReviewFile = Join-Path $root $ReviewFile }
$reportDir = Join-Path $root 'exports\lmxxf-upstream'
$auditPy = Join-Path $PSScriptRoot 'audit-lmxxf-enablements.py'
$python = $null
if (-not $SkipEnablementAudit) {
    if (-not (Test-Path -LiteralPath $auditPy -PathType Leaf)) { throw "Required audit is missing: $auditPy" }
    $python = Resolve-AuditPython $PythonPath
}
$buildCmd = Join-Path $PSScriptRoot 'build-lmxxf-runtime.cmd'
if (-not $SkipBuild -and -not $SkipEnablementAudit -and -not (Test-Path -LiteralPath $buildCmd -PathType Leaf)) {
    throw "Required build verifier is missing: $buildCmd (use -SkipBuild only for staged validation)"
}
$upstreamMd = Join-Path $vendorRoot 'UPSTREAM.md'
$md = Get-Content -LiteralPath $upstreamMd -Encoding UTF8 -Raw
$pin = [regex]::Matches($md, '(?m)^- Commit: `([0-9a-f]{40})`[^\r\n]*')
if ($pin.Count -ne 1) { throw 'UPSTREAM.md must contain exactly one pinned Commit row.' }
$previousCommit = $pin[0].Groups[1].Value
$archiveRoot = $null
try {
    if (-not $SkipUpstreamFetch) {
        $remotes = @(& git @gitSafe -C $upstream remote)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot list upstream remotes.' }
        $remote = 'origin'
        if ($UpstreamRef.Contains('/') -and $UpstreamRef.Split('/')[0] -in $remotes) { $remote = $UpstreamRef.Split('/')[0] }
        & git @gitSafe -C $upstream fetch $remote
        if ($LASTEXITCODE -ne 0) {
            if (-not $AllowOfflineUpstream) { throw 'Upstream fetch failed. Use -SkipUpstreamFetch or -AllowOfflineUpstream intentionally.' }
            Write-Warning 'Fetch failed; using the locally available upstream ref (-AllowOfflineUpstream).'
        }
    }
    $commitHash = & git @gitSafe -C $upstream rev-parse --verify --end-of-options "$UpstreamRef^{commit}"
    if ($LASTEXITCODE -ne 0 -or $commitHash -notmatch '^[0-9a-f]{40}$') { throw "Cannot pin upstream ref: $UpstreamRef" }
    Write-Host "Staging upstream $commitHash from $upstream" -ForegroundColor Cyan

    $statePath = Join-Path $vendorRoot 'sync-state.json'
    $oldState = if (Test-Path -LiteralPath $statePath) { Read-SyncJson $statePath } else { $null }
    if ($oldState -and ($oldState.schema -ne 1 -or $oldState.status -notin @('pending', 'reviewed') -or
        $oldState.from_commit -notmatch '^[0-9a-f]{40}$' -or $oldState.to_commit -notmatch '^[0-9a-f]{40}$' -or
        -not $oldState.recipes_verified -or -not $oldState.modules_verified)) {
        throw 'Invalid sync-state.json; restore the last recorded baseline before retrying.'
    }
    # Keep the old comparison base across an interrupted/pending run, and replay the same
    # reviewed range when re-verifying an already accepted commit.
    if ($oldState -and $oldState.to_commit -eq $commitHash -and
        ($oldState.status -eq 'pending' -or $previousCommit -eq $commitHash)) {
        $previousCommit = $oldState.from_commit
    }
    $archiveRoot = Join-Path ([IO.Path]::GetTempPath()) ('lmxxf-sync-' + [guid]::NewGuid().ToString('N'))
    $tree = Join-Path $archiveRoot 'tree'
    New-Item -ItemType Directory -Force -Path $tree | Out-Null
    $archiveTar = Join-Path $archiveRoot 'upstream.tar'
    # The same manifest drives extraction and copying. Do not archive all the upstream
    # experiment trees just to get five headers. Missing pinned paths are allowed only
    # when we keep their local versions.
    $requiredHeaders = @($manifest.headers | Where-Object { -not $pinned.ContainsKey($_) -or $updates[$pinned[$_].switch] })
    $archiveHeaders = @(& git @gitSafe -C $upstream ls-tree -r --name-only $commitHash -- @($manifest.headers))
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate the selected upstream headers.' }
    $missingHeaders = @($requiredHeaders | Where-Object { $_ -notin $archiveHeaders })
    if ($missingHeaders.Count) { throw ('Required upstream paths disappeared; update the closure/integration first: ' + ($missingHeaders -join ', ')) }
    $archivePaths = $archiveHeaders + @('shaders', 'hip')
    & git @gitSafe -C $upstream archive --format=tar -o $archiveTar $commitHash -- @archivePaths
    if ($LASTEXITCODE -ne 0) { throw 'git archive failed; no vendor files changed.' }
    Push-Location $tree
    try {
        & tar -xf (Resolve-Path -Relative -LiteralPath $archiveTar)
        if ($LASTEXITCODE -ne 0) { throw 'Upstream archive extraction failed.' }
    } finally { Pop-Location }
    foreach ($file in Get-ChildItem -LiteralPath $tree -Recurse -Force) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Link in source archive: $($file.FullName)" }
    }

    # Validate the entire selected closure before the first destination copy/delete.
    $required = @($manifest.headers | Where-Object { -not $pinned.ContainsKey($_) -or $updates[$pinned[$_].switch] })
    $required += @($manifest.hip_files | ForEach-Object { 'hip/' + $_ })
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $tree $_) -PathType Leaf) })
    if ($missing.Count) { throw ('Required upstream paths disappeared; update the closure/integration first: ' + ($missing -join ', ')) }
    foreach ($pair in @(@('hip', '*.hip'), @('shaders', '*.hlsl'))) {
        if (@(Get-ChildItem -LiteralPath (Join-Path $tree $pair[0]) -File -Filter $pair[1]).Count -eq 0) { throw "Empty upstream $($pair[0]) source set" }
    }
    foreach ($spec in $manifest.pinned) {
        if ($updates[$spec.switch]) {
            Invoke-LocalHeaderPatch $tree (Join-Path $configRoot ('patches/' + $spec.patch))
            Assert-LocalHeader $spec $tree
        } else { Assert-LocalHeader $spec $vendorRoot }
    }
    $reference = Join-Path $tree 'Development/HIP/hip_reference_network.h'
    if ((Get-Content -LiteralPath $reference -Encoding UTF8 -Raw) -notmatch 'PreflightPdl') {
        Invoke-LocalHeaderPatch $tree (Join-Path $configRoot 'patches/reference-network.patch')
    }
    foreach ($dir in @('src', 'Development/HIP', 'hip', 'shaders', 'modules')) {
        $owned = Assert-SyncPath (Join-Path $vendorRoot $dir) $vendorRoot
        if (Test-Path -LiteralPath $owned) {
            foreach ($entry in Get-ChildItem -LiteralPath $owned -Recurse -Force) {
                if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Link in destination owner: $($entry.FullName)" }
            }
        }
    }
    $dstHip = Join-Path $vendorRoot 'hip'
    $dstModules = Join-Path $vendorRoot 'modules'
    $hipFilters = @('*.hip', 'build-modules.ps1', 'rtc_compile.cpp')
    function Get-RecipeFingerprint {
        (Get-TreeFingerprint $dstHip $hipFilters) + ':' + (Get-FileSha256Hex (Join-Path $configRoot 'module-defines.json'))
    }
    # Without a recorded baseline, existing binaries have no verified recipe provenance.
    $hipFpBefore = 'unverified'
    $modulesFpBefore = Get-TreeFingerprint $dstModules @('*.hsaco', 'SHA256SUMS')
    if ($oldState) {
        # A failed run may already have copied new recipes; comparing only this run's
        # before/after would let a retry incorrectly accept stale binaries.
        $hipFpBefore = $oldState.recipes_verified
        $modulesFpBefore = $oldState.modules_verified
    }
    $resolvedModules = $null
    if (-not $SkipModules -and -not $SkipEnablementAudit) {
        $resolvedModules = Resolve-ModulesPath $ModulesPath
        if (-not $resolvedModules -and $NoBuildModules) { throw '-NoBuildModules requires -ModulesPath or -SkipModules.' }
    }
    $state = [ordered]@{ schema = 1; status = 'pending'; from_commit = $previousCommit; to_commit = $commitHash;
        recipes_verified = $hipFpBefore; modules_verified = $modulesFpBefore;
        skipped_checks = @(); updated_utc = [DateTime]::UtcNow.ToString('o') }
    foreach ($name in @('SkipBuild', 'SkipModules', 'AllowStaleModules', 'SkipEnablementAudit')) {
        if ((Get-Variable -Name $name -ValueOnly)) { $state.skipped_checks += $name }
    }
    Write-SyncJson $statePath $state

    foreach ($rel in $manifest.headers) {
        if ($pinned.ContainsKey($rel) -and -not $updates[$pinned[$rel].switch]) { continue }
        $dst = Assert-SyncPath (Join-Path $vendorRoot $rel) $vendorRoot
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath (Join-Path $tree $rel) -Destination $dst -Force
    }
    foreach ($dir in @('src', 'Development/HIP')) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $vendorRoot $dir) -File -Filter '*.h') {
            $rel = $dir + '/' + $file.Name
            if ($rel -notin $manifest.headers) {
                $retired = Assert-SyncPath $file.FullName $vendorRoot
                Remove-Item -LiteralPath $retired -Force
                Write-Host "  Removed retired header: $rel"
            }
        }
    }
    Sync-FlatFiles (Join-Path $tree 'shaders') (Join-Path $vendorRoot 'shaders') '*.hlsl'
    Sync-FlatFiles (Join-Path $tree 'hip') $dstHip '*.hip'
    foreach ($name in $manifest.hip_files) {
        if ($name -eq 'SHA256SUMS') { continue }
        Copy-Item -LiteralPath (Join-Path $tree ('hip/' + $name)) -Destination (Join-Path $dstHip $name) -Force
    }
    $upstreamSums = Join-Path $tree 'hip/SHA256SUMS'
    Merge-HipSums $upstreamSums (Join-Path $dstHip 'SHA256SUMS') $null
    if ($SkipEnablementAudit) {
        Write-Warning "SOURCE STAGING ONLY: audit skipped explicitly. Sync remains pending at $statePath. Rerun without -SkipEnablementAudit to review and verify."
        return
    }

    # Review uses the resolved SHA, never a moving branch name. Nonzero means incomplete.
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $auditArgs = @($auditPy, $upstream, $commitHash, '--base', $previousCommit, '--output-dir', $reportDir, '--review', $ReviewFile)
    foreach ($check in $state.skipped_checks) { $auditArgs += @('--skipped-check', $check) }
    if ($resolvedModules) { $auditArgs += @('--modules-path', $resolvedModules) }
    & $python @auditArgs
    if ($LASTEXITCODE -ne 0) { throw "Enablement/integration review incomplete (exit $LASTEXITCODE). Read $reportDir\report.md and complete the review before rerunning. Sources remain staged; sync is not complete." }

    $builtHere = $false
    if (-not $SkipModules) {
        if (-not $resolvedModules) {
            $resolvedModules = Invoke-BuildGfx1201Modules $dstHip (Join-Path $dstHip '_build_gfx1201')
            $builtHere = $true
        }
        Sync-LmxxfModules $resolvedModules $dstModules $commitHash
        Merge-HipSums $upstreamSums (Join-Path $dstHip 'SHA256SUMS') $dstModules
    }
    $modulesValidated = Assert-ModulesMatchHipSums $dstModules (Join-Path $dstHip 'SHA256SUMS') -allowStale:$AllowStaleModules
    $hipFpAfter = Get-RecipeFingerprint
    $modulesFpAfter = Get-TreeFingerprint $dstModules @('*.hsaco', 'SHA256SUMS')
    if ($hipFpBefore -ne $hipFpAfter -and -not $builtHere -and ($SkipModules -or $modulesFpBefore -eq $modulesFpAfter)) {
        if (-not $AllowStaleModules) { throw 'HIP recipes/defines changed but shipping modules were not refreshed. Rebuild, supply -ModulesPath, or explicitly use -AllowStaleModules for staged integration.' }
        Write-Warning 'Shipping modules may be stale (-AllowStaleModules); follow the deferred validation plan.'
    }
    if (-not $SkipBuild) {
        & cmd.exe /c "`"$buildCmd`""
        if ($LASTEXITCODE -ne 0) { throw "Runtime build verification failed (exit $LASTEXITCODE)." }
    }
    # Advance the completed pin only after review and all requested verification succeed.
    $today = (Get-Date).ToString('yyyy-MM-dd')
    $md = [regex]::Replace($md, '(?m)^- Commit: `([0-9a-f]{40})`[^\r\n]*', "- Commit: ``$commitHash`` (synced $today)")
    [IO.File]::WriteAllText($upstreamMd, $md, [Text.UTF8Encoding]::new($false))
    $state.status = 'reviewed'
    # An explicit stale-module waiver does not turn old binaries into a verified build.
    if ($modulesValidated -and ($builtHere -or (-not $SkipModules -and $modulesFpBefore -ne $modulesFpAfter))) {
        $state.recipes_verified = $hipFpAfter
        $state.modules_verified = $modulesFpAfter
    }
    Write-SyncJson $statePath $state
    Write-Host "Source sync and integration review complete: $commitHash" -ForegroundColor Green
    if ($state.skipped_checks.Count) { Write-Warning ('Explicit verification exceptions: ' + ($state.skipped_checks -join ', ') + '. See the reviewed follow-up plan.') }
} finally {
    if ($archiveRoot) { Remove-SyncTree $archiveRoot ([IO.Path]::GetTempPath()) }
}
