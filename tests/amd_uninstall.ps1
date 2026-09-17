[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# Always exercise the same Windows PowerShell 5.1 runtime as Uninstall_OptiScaler_NR.bat.
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$repo = Split-Path -Parent $PSScriptRoot
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
function Run-Uninstall([string]$dir) {
    $output = & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $uninstall -GameDir $dir -NonInteractive -NoPause 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Uninstall exit $LASTEXITCODE : $($output -join [Environment]::NewLine)" }
    if (($output -join '') -notmatch 'Uninstall SUCCEEDED') { throw 'Missing uninstall success result.' }
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
        'dlssnr_on_amd_weights.bin', 'dlssnr_on_amd_setup.exe', 'dlssnr_on_amd.log',
        'OptiScaler\plugins\XeFGUnlock.asi', 'OptiScaler\plugins\XeFGUnlock.ini',
        'OptiScaler\unknown.dll', 'OptiScaler\libxess_custom.dll', 'OptiScaler\user.ini',
        'OptiScaler\nvngx_dlssnr.dll', 'OptiScaler\dlssnr_on_amd_weights.bin',
        'OptiScaler\D3D12_OptiScaler\other-mod.dll',
        'backup-amd-presr-fixture\OptiScaler.ini',
        'backup-amd-presr-fixture\OptiScaler\libxess.dll'
    )
    $removed = @('dxgi.dll', 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll',
        'dlssnr_amd_pass3.dll', 'OptiScaler.ini', 'amd-presr-install.txt',
        'OptiScaler.log.1', 'amd_presr.log', 'amd_bridge.log.2',
        'Uninstall_OptiScaler_NR.bat', 'Uninstall_OptiScaler_NR.ps1')
    foreach ($root in $roots) {
        foreach ($relative in $preserved) { Put-File (Join-Path $root $relative) }
        foreach ($relative in $removed) { Put-File (Join-Path $root $relative) }
        foreach ($relative in $deps) { Put-File (Join-Path $root ('OptiScaler\' + $relative)) }
        Copy-Item -LiteralPath $proxy -Destination (Join-Path $root 'dxgi.dll')
    }
    Put-File (Join-Path $game 'amd-presr-install.txt') 'proxy=dxgi.dll'
    Run-Uninstall $game
    foreach ($root in $roots) {
        foreach ($relative in $preserved) { Assert-Exists (Join-Path $root $relative) }
        foreach ($relative in $removed) { Assert-Removed (Join-Path $root $relative) }
        foreach ($relative in $deps) { Assert-Removed (Join-Path $root ('OptiScaler\' + $relative)) }
    }
    Copy-Item -LiteralPath $uninstall -Destination (Join-Path $game 'Uninstall_OptiScaler_NR.ps1')
    $inPlace = & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $game 'Uninstall_OptiScaler_NR.ps1') -NonInteractive -NoPause 2>&1
    if ($LASTEXITCODE -ne 0) { throw "In-place uninstall exit $LASTEXITCODE : $($inPlace -join [Environment]::NewLine)" }
    if (($inPlace -join '') -notmatch 'Uninstall SUCCEEDED') { throw 'Missing in-place uninstall success result.' }
    if (($inPlace -join '') -notmatch 'Planned deletions:') { throw 'In-place uninstall did not list planned deletions.' }
    Assert-Removed (Join-Path $game 'Uninstall_OptiScaler_NR.ps1')
    Run-Uninstall $game # Repeated/manual uninstall remains supported.
    Write-Host 'PASS project files removed; plugins, author files, unknown files and backups preserved'

    $cleanGame = Join-Path $testRoot 'deps-only-game'
    foreach ($relative in $deps) { Put-File (Join-Path $cleanGame ('OptiScaler\' + $relative)) }
    Run-Uninstall $cleanGame
    Assert-Removed (Join-Path $cleanGame 'OptiScaler')
    Write-Host 'PASS empty dependency directories removed without recursion'

    $escapeGame = Join-Path $testRoot 'escape-game'
    $outsideProxy = Join-Path $testRoot 'outside\escape.dll'
    Put-File $outsideProxy
    Copy-Item -LiteralPath $proxy -Destination $outsideProxy -Force
    foreach ($record in @('proxy=..\outside\escape.dll', ('proxy=' + $outsideProxy))) {
        Put-File (Join-Path $escapeGame 'amd-presr-install.txt') $record
        Run-Uninstall $escapeGame
        Assert-Exists $outsideProxy
    }
    Write-Host 'PASS relative and absolute proxy paths in install records are rejected'

    foreach ($linkedRelative in @('_storage_', 'OptiScaler', 'OptiScaler\D3D12_OptiScaler')) {
        $id = [guid]::NewGuid().ToString('N')
        $linkGame = Join-Path $testRoot ('linked-game-' + $id)
        $target = Join-Path $testRoot ('external-' + $id)
        foreach ($relative in @('OptiScaler.ini', 'amd_presr.log', 'libxess.dll',
                'D3D12Core.dll', 'OptiScaler\libxess.dll')) {
            Put-File (Join-Path $target $relative)
        }
        $link = Join-Path $linkGame $linkedRelative
        Make-Junction $link $target
        Run-Uninstall $linkGame
        Assert-Exists $link
        foreach ($relative in @('OptiScaler.ini', 'amd_presr.log', 'libxess.dll',
                'D3D12Core.dll', 'OptiScaler\libxess.dll')) {
            Assert-Exists (Join-Path $target $relative)
        }
    }
    Write-Host 'PASS storage, dependency and Agility junction targets preserved'
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
