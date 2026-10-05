[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# Always exercise the same Windows PowerShell 5.1 runtime as Uninstall_OptiScaler_NR.bat.
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$uninstall = Join-Path $repo 'tools\uninstall-amd-presr.ps1'
$testRoot = Join-Path $repo ('exports\uninstall-tests-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$links = New-Object System.Collections.Generic.List[string]

function Put-File([string]$path, [string]$content = 'fixture') {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $content)
}
function Assert-Exists([string]$path) {
    if (!(Test-Path -LiteralPath $path)) { throw "Expected preserved path: $path" }
}
function Assert-Removed([string]$path) {
    if (Test-Path -LiteralPath $path) { throw "Expected removed path: $path" }
}
function Run-Uninstall {
    param(
        [string]$Dir,
        [switch]$RemoveBackups
    )
    $extra = @()
    if ($RemoveBackups) { $extra += '-RemoveBackups' }
    $output = & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $uninstall -GameDir $Dir -NonInteractive -NoPause @extra 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Uninstall exit $LASTEXITCODE : $($output -join [Environment]::NewLine)" }
    if (($output -join '') -notmatch 'Uninstall SUCCEEDED') { throw 'Missing uninstall success result.' }
    ,$output
}
function Make-Junction([string]$path, [string]$target) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [void](New-Item -ItemType Junction -Path $path -Target $target)
    $links.Add($path)
}

try {
    $proxy = Join-Path $testRoot 'fixture-opti.dll'
    Add-Type -TypeDefinition @'
using System.Reflection;
[assembly: AssemblyProduct("OptiScaler")]
public class UninstallProxyFixture { }
'@ -OutputAssembly $proxy -OutputType Library
    if ((Get-Item -LiteralPath $proxy).VersionInfo.ProductName -ne 'OptiScaler') {
        throw 'Synthetic proxy did not receive OptiScaler version metadata.'
    }

    $deps = @(
        'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
        'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_vk.dll',
        'libxess.dll', 'libxess_dx11.dll', 'libxess_fg.dll', 'libxell.dll',
        'D3D12_OptiScaler\D3D12Core.dll', 'D3D12_OptiScaler\d3d12SDKLayers.dll'
    )
    $game = Join-Path $testRoot 'normal-game'
    $roots = @($game, (Join-Path $game '_storage_'))
    $preserved = @(
        'version.dll', 'unknown-game.dll', 'nvngx_dlssnr.dll',
        'dlssnr_on_amd_weights.bin', 'dlssnr_on_amd_setup.exe',
        'OptiScaler\plugins\XeFGUnlock.asi', 'OptiScaler\plugins\XeFGUnlock.ini',
        'OptiScaler\unknown.dll', 'OptiScaler\libxess_custom.dll', 'OptiScaler\user.ini',
        'OptiScaler\nvngx_dlssnr.dll', 'OptiScaler\dlssnr_on_amd_weights.bin',
        'OptiScaler\D3D12_OptiScaler\other-mod.dll',
        'OptiScaler.notes.log', 'OptiScaler.log.notes', 'OptiScaler.1.log.bak',
        'other-mod.1.log', 'logs\dxgi.log', 'yysls_d3d11.log',
        'OptiScaler\dlssnr\user-notes.md',
        'lmxxf-modules\user.generated.hip', 'lmxxf-modules\user.hsaco.s',
        'lmxxf-modules\gfx9999\c32_fast.generated.hip',
        'dlssnr-amd\dlssnr.bin', 'native-game-tiled-assets\model.bin',
        'dlssnr-amd\shaders\runtime\user.spv', 'dlssnr-amd\prewarm\user.txt',
        'backup-amd-presr-fixture\OptiScaler.ini',
        'backup-amd-presr-fixture\OptiScaler\libxess.dll'
    )
    $removed = @('dlssnr_on_amd.log', 'dlssnr_on_amd.1.log', 'dlssnr_on_amd.ini',
        'dlssnr-amd.log', 'dlssnr-amd.log.1', 'dlssnr-amd-crash.dmp', 'dlssnr-amd.ini', 'dlssnr-amd-install.txt',
        'dxgi.dll', 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll',
        'dlssnr_amd_pass3.dll', 'OptiScaler.ini', 'amd-presr-install.txt',
        'OptiScaler.log', 'OptiScaler.1.log', 'OptiScaler.2.log', 'OptiScaler.10.log',
        'OptiScaler.log.1', 'amd_presr.log', 'amd_bridge.log.2', 'mochizuki_nr.1.log',
        'OptiScaler\dlssnr\README.md', 'OptiScaler\dlssnr\design\frame-hold.md',
        'OptiScaler\dlssnr\design\multi-point-anchoring.md', 'OptiScaler\dlssnr\design\pre-sr-multipass.md',
        'dlssnr-amd\pipeline.cache', 'dlssnr-amd\prewarm\manifest.txt',
        'dlssnr-amd\shaders\runtime\runtime_encode.spv',
        'dlssnr-amd\shaders\temporal\motion_estimate.spv',
        'lmxxf-modules\c32_fast.generated.hip', 'lmxxf-modules\c32_fast.hsaco.s',
        'lmxxf-modules\gfx1200\boundary-fast.generated.hip', 'lmxxf-modules\gfx1200\boundary-fast.hsaco.s',
        'lmxxf-modules\gfx1201\deep_fast.generated.hip', 'lmxxf-modules\gfx1201\deep_fast.hsaco.s',
        'Uninstall_OptiScaler_NR.bat', 'Uninstall_OptiScaler_NR.ps1')
    foreach ($root in $roots) {
        foreach ($relative in $preserved) { Put-File (Join-Path $root $relative) }
        foreach ($relative in $removed) { Put-File (Join-Path $root $relative) }
        foreach ($relative in $deps) { Put-File (Join-Path $root ('OptiScaler\' + $relative)) }
        Copy-Item -LiteralPath $proxy -Destination (Join-Path $root 'dxgi.dll')
    }
    Put-File (Join-Path $game 'amd-presr-install.txt') 'proxy=dxgi.dll'
    $normalOut = Run-Uninstall $game
    $plan = ($normalOut -join "`n") -split 'Deleted:', 2 | Select-Object -First 1
    foreach ($relative in @('OptiScaler.1.log', 'dlssnr-amd/pipeline.cache', 'lmxxf-modules/c32_fast.generated.hip')) {
        if (-not $plan.Contains((Join-Path $game $relative).Replace('/', '\'))) {
            throw "Cleanup omitted from preview: $relative"
        }
    }
    foreach ($root in $roots) {
        foreach ($relative in $preserved) { Assert-Exists (Join-Path $root $relative) }
        foreach ($relative in $removed) { Assert-Removed (Join-Path $root $relative) }
        foreach ($relative in $deps) { Assert-Removed (Join-Path $root ('OptiScaler\' + $relative)) }
    }
    Copy-Item -LiteralPath $uninstall -Destination (Join-Path $game 'Uninstall_OptiScaler_NR.ps1')
    $inPlace = & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $game 'Uninstall_OptiScaler_NR.ps1') -NonInteractive -NoPause 2>&1
    if ($LASTEXITCODE -ne 0) { throw "In-place uninstall exit $LASTEXITCODE : $($inPlace -join [Environment]::NewLine)" }
    if (($inPlace -join '') -notmatch 'Uninstall SUCCEEDED') { throw 'Missing in-place uninstall success result.' }
    if (($inPlace -join '') -notmatch 'Planned deletions') { throw 'In-place uninstall did not list planned deletions.' }
    Assert-Removed (Join-Path $game 'Uninstall_OptiScaler_NR.ps1')
    $null = Run-Uninstall $game # Repeated/manual uninstall remains supported.
    Write-Host 'PASS project files removed; plugins, author files, unknown files and backups preserved'

    $backupGame = Join-Path $testRoot 'backup-remove-game'
    Put-File (Join-Path $backupGame 'dlssnr_amd_pass1.dll')
    Put-File (Join-Path $backupGame 'backup-amd-presr-fixture\OptiScaler.ini')
    $removeOut = Run-Uninstall -Dir $backupGame -RemoveBackups
    Assert-Removed (Join-Path $backupGame 'dlssnr_amd_pass1.dll')
    Assert-Removed (Join-Path $backupGame 'backup-amd-presr-fixture')
    if (($removeOut -join '') -notmatch 'backup folder') { throw 'RemoveBackups did not list the backup folder.' }
    Write-Host 'PASS RemoveBackups deletes backup-amd-presr-* folders'

    $cleanGame = Join-Path $testRoot 'deps-only-game'
    foreach ($relative in $deps) { Put-File (Join-Path $cleanGame ('OptiScaler\' + $relative)) }
    foreach ($relative in $removed | Where-Object { $_ -match '^(OptiScaler\\|dlssnr-amd\\|lmxxf-modules\\)' }) {
        Put-File (Join-Path $cleanGame $relative)
    }
    $null = Run-Uninstall $cleanGame
    Assert-Removed (Join-Path $cleanGame 'OptiScaler')
    Assert-Removed (Join-Path $cleanGame 'dlssnr-amd')
    Assert-Removed (Join-Path $cleanGame 'lmxxf-modules')
    Write-Host 'PASS empty dependency directories removed without recursion'

    $escapeGame = Join-Path $testRoot 'escape-game'
    $outsideProxy = Join-Path $testRoot 'outside\escape.dll'
    Put-File $outsideProxy
    Copy-Item -LiteralPath $proxy -Destination $outsideProxy -Force
    foreach ($record in @('proxy=..\outside\escape.dll', ('proxy=' + $outsideProxy))) {
        Put-File (Join-Path $escapeGame 'amd-presr-install.txt') $record
        $null = Run-Uninstall $escapeGame
        Assert-Exists $outsideProxy
    }
    Write-Host 'PASS relative and absolute proxy paths in install records are rejected'

    foreach ($linkedRelative in @('_storage_', 'OptiScaler', 'OptiScaler\D3D12_OptiScaler', 'OptiScaler\dlssnr',
            'dlssnr-amd', 'dlssnr-amd\prewarm', 'lmxxf-modules')) {
        $id = [guid]::NewGuid().ToString('N')
        $linkGame = Join-Path $testRoot ('linked-game-' + $id)
        $target = Join-Path $testRoot ('external-' + $id)
        foreach ($relative in @('OptiScaler.ini', 'amd_presr.log', 'libxess.dll',
                'D3D12Core.dll', 'OptiScaler\libxess.dll', 'OptiScaler.1.log',
                'pipeline.cache', 'manifest.txt', 'prewarm\manifest.txt',
                'c32_fast.generated.hip', 'README.md', 'design\frame-hold.md')) {
            Put-File (Join-Path $target $relative)
        }
        $link = Join-Path $linkGame $linkedRelative
        Make-Junction $link $target
        $null = Run-Uninstall $linkGame
        Assert-Exists $link
        foreach ($relative in @('OptiScaler.ini', 'amd_presr.log', 'libxess.dll',
                'D3D12Core.dll', 'OptiScaler\libxess.dll', 'OptiScaler.1.log',
                'pipeline.cache', 'manifest.txt', 'prewarm\manifest.txt',
                'c32_fast.generated.hip', 'README.md', 'design\frame-hold.md')) {
            Assert-Exists (Join-Path $target $relative)
        }
    }
    Write-Host 'PASS storage, dependency and Agility junction targets preserved'

    $flagsGame = Join-Path $testRoot 'flags-game'
    Put-File (Join-Path $flagsGame 'DLSS5-AMD\native-game-flags.txt') "DLSS5_FIT_LARGE=1`r`n"
    $null = Run-Uninstall $flagsGame
    Assert-Removed (Join-Path $flagsGame 'DLSS5-AMD')
    $userFlags = Join-Path $testRoot 'user-flags-game\DLSS5-AMD\native-game-flags.txt'
    Put-File $userFlags "DLSS5_NETWORK_HEIGHT=900`r`nDLSS5_FIT_LARGE=0`r`nDLSS5_STRENGTH=1,1`r`n"
    $null = Run-Uninstall (Join-Path $testRoot 'user-flags-game')
    $left = [IO.File]::ReadAllText($userFlags)
    if ($left -cne "DLSS5_NETWORK_HEIGHT=900`r`nDLSS5_STRENGTH=1,1`r`n") { throw "User flags not preserved: [$left]" }
    Write-Host 'PASS flags file: installer-owned FIT_LARGE line removed, user flags kept'
    $seed = @(
        '# Optional lmxxf upstream keys (DLSS5_*).',
        '# OptiScaler.ini / Ins menu win on conflict; this file only fills gaps.',
        '# Example: DLSS5_HIP_WAVE_OWNED=1'
    ) -join "`r`n"
    $seedGame = Join-Path $testRoot 'seed-only-game'
    Put-File (Join-Path $seedGame 'DLSS5-AMD\native-game-flags.txt') ($seed + "`r`n")
    $null = Run-Uninstall $seedGame
    Assert-Removed (Join-Path $seedGame 'DLSS5-AMD')
    foreach ($custom in @('# My saved experiment', 'DLSS5_HIP_WAVE_OWNED=1')) {
        Put-File $userFlags ($seed + "`r`n" + $custom + "`r`n")
        $savedTime = [datetime]::new(2020, 1, 2, 3, 4, 5, [DateTimeKind]::Utc)
        [IO.File]::SetLastWriteTimeUtc($userFlags, $savedTime)
        $customOut = Run-Uninstall (Split-Path -Parent (Split-Path -Parent $userFlags))
        if ([IO.File]::GetLastWriteTimeUtc($userFlags) -ne $savedTime -or
            ($customOut -join '') -match 'removed the DLSS5_FIT_LARGE line') {
            throw 'Unchanged user flags were rewritten or reported as deleted.'
        }
        if ([IO.File]::ReadAllText($userFlags) -cne ($seed + "`r`n" + $custom + "`r`n")) {
            throw 'Custom flags/comments changed during uninstall.'
        }
    }
    Write-Host 'PASS untouched seed removed; custom flags and comments preserved byte-for-byte'

    $standalone = Join-Path $testRoot 'standalone-game'
    Put-File (Join-Path $standalone 'lmxxf-modules\c32_fast.generated.hip')
    Copy-Item -LiteralPath $uninstall -Destination (Join-Path $standalone 'Uninstall_OptiScaler_NR.ps1')
    Copy-Item -LiteralPath (Join-Path $repo 'tools\lmxxf-module-package.ps1') -Destination $standalone
    $out = & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $standalone 'Uninstall_OptiScaler_NR.ps1') -NonInteractive -NoPause 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Standalone cleanup failed: $out" }
    Assert-Removed (Join-Path $standalone 'lmxxf-modules')
    Assert-Removed (Join-Path $standalone 'lmxxf-module-package.ps1')
    Assert-Removed (Join-Path $standalone 'Uninstall_OptiScaler_NR.ps1')
    Write-Host 'PASS installed standalone uninstaller loads module names before removing its helper'
    $legacyGame = Join-Path $testRoot 'legacy-backend-game'
    $legacyRemove = @('game_dxgi.log', 'game_d3d11.log', 'game_d3d9.log', 'vkd3d-proton.cache', 'vkd3d-proton.cache.write')
    $legacyKeep = @('game.dll', 'game-notes.log', 'logs\game_dxgi.log', 'game-assets\save.dat')
    foreach ($file in $legacyRemove + $legacyKeep) { Put-File (Join-Path $legacyGame $file) }
    $outsideLog = Join-Path $testRoot 'outside_dxgi.log'
    Put-File $outsideLog
    $record = ($legacyRemove | ForEach-Object { 'L ' + $_ }) -join "`r`n"
    $record += "`r`nL ..\outside_dxgi.log`r`nL game.dll`r`nL game-notes.log`r`nL logs\game_dxgi.log`r`nF game.dll`r`nD game-assets`r`n"
    Put-File (Join-Path $legacyGame 'dlssnr-amd-install.txt') $record
    $null = Run-Uninstall $legacyGame
    foreach ($file in $legacyRemove) { Assert-Removed (Join-Path $legacyGame $file) }
    foreach ($file in $legacyKeep) { Assert-Exists (Join-Path $legacyGame $file) }
    Assert-Exists $outsideLog
    Assert-Removed (Join-Path $legacyGame 'dlssnr-amd-install.txt')
    Write-Host 'PASS legacy record cleans only allowed logs/cache; no DLL, directory or outside-path operations'

    $legacyBackup = Join-Path $legacyGame 'dlssnr-amd-backup'
    $models = @('dlssnr-amd\dlssnr.bin', 'dlssnr_on_amd_weights.bin',
        'native-game-tiled-assets\tensor.bin', 'nvngx_dlssnr.dll')
    foreach ($file in $models) { Put-File (Join-Path $legacyBackup $file) 'only model copy' }
    foreach ($file in @('OptiScaler.ini', 'dlssnr-amd\pipeline.cache', 'dlssnr-amd\shaders\old.spv')) {
        Put-File (Join-Path $legacyBackup $file)
    }
    $null = Run-Uninstall $legacyGame
    Assert-Exists (Join-Path $legacyBackup 'OptiScaler.ini')
    $null = Run-Uninstall -Dir $legacyGame -RemoveBackups
    foreach ($file in $models) {
        if ([IO.File]::ReadAllText((Join-Path $legacyBackup $file)) -cne 'only model copy') {
            throw 'Backup cleanup changed a model.'
        }
    }
    Assert-Removed (Join-Path $legacyBackup 'OptiScaler.ini')
    Assert-Removed (Join-Path $legacyBackup 'dlssnr-amd\pipeline.cache')
    Assert-Removed (Join-Path $legacyBackup 'dlssnr-amd\shaders')
    $legacyEmpty = Join-Path $testRoot 'legacy-no-model'
    Put-File (Join-Path $legacyEmpty 'dlssnr-amd-backup\old.dll')
    $null = Run-Uninstall -Dir $legacyEmpty -RemoveBackups
    Assert-Removed (Join-Path $legacyEmpty 'dlssnr-amd-backup')
    Write-Host 'PASS legacy backup is optional; deleting it preserves every model and prunes empty folders'
    Write-Host 'All uninstall regression checks passed.'
} finally {
    # Remove junctions themselves before fixture cleanup. Never recursively
    # remove a tree while it still contains a link to another directory.
    foreach ($link in $links) {
        if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link, $false) }
    }
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $expectedPrefix = [IO.Path]::GetFullPath((Join-Path $repo 'exports')) + '\uninstall-tests-'
    if (!$resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing fixture cleanup outside expected directory: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
