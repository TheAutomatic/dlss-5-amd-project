# Build + STA smoke test of the WPF launcher with a fake library; writes PNG previews.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\build\launcher-smoke.ps1 [-OutDir exports\launcher-smoke]
[CmdletBinding()]
param(
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutDir) { $OutDir = Join-Path $root 'exports/launcher-smoke' }
$launcherDir = Join-Path $root 'exports/launcher'
$exe = Join-Path $launcherDir 'OptiScalerNRLauncher.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    & (Join-Path $PSScriptRoot 'build-launcher.ps1')
}
$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$wpf = Join-Path $fw 'WPF'
$smoke = Join-Path $launcherDir 'launcher-smoke.exe'
$cscArgs = @('/nologo', '/noconfig', '/nostdlib+', '/target:exe', '/platform:x64', '/warnaserror+', "/out:$smoke")
foreach ($r in @((Join-Path $fw 'mscorlib.dll'), (Join-Path $fw 'System.dll'), (Join-Path $fw 'System.Core.dll'), (Join-Path $fw 'System.Xaml.dll'),
                 (Join-Path $wpf 'WindowsBase.dll'), (Join-Path $wpf 'PresentationCore.dll'), (Join-Path $wpf 'PresentationFramework.dll'))) {
    $cscArgs += "/r:$r"
}
$cscArgs += (Join-Path $PSScriptRoot 'launcher-smoke.cs')
& (Join-Path $fw 'csc.exe') @cscArgs
if ($LASTEXITCODE -ne 0) { throw "smoke harness compile failed ($LASTEXITCODE)" }

# Throw-away settings + fake library (real files so HasExecutable is true; nothing is installed).
$work = Join-Path ([IO.Path]::GetTempPath()) ('osnr-smoke-' + [Guid]::NewGuid().ToString('N'))
$settings = Join-Path $work 'settings'
New-Item -ItemType Directory -Force -Path $settings | Out-Null
$donor = Join-Path $env:WINDIR 'System32\where.exe'
function New-FakeGame([string]$folder, [string]$exeName) {
    $dir = Join-Path $work ('games\' + $folder)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir $exeName
    Copy-Item -LiteralPath $donor -Destination $path -Force
    return $path
}
$games = @(
    @{ id = 'manual:smoke1'; name = 'Cyberpunk 2077'; exe = (New-FakeGame 'Cyberpunk 2077\bin\x64' 'Cyberpunk2077.exe'); source = 'Manual' },
    @{ id = 'manual:smoke2'; name = 'NVIDIA RTX Remix Minecraft (Long Instance Name For Truncation Test)'; exe = (New-FakeGame 'PrismLauncher\instances\RTX' 'javaw.exe'); source = 'Manual'; targetKind = 'rtx-remix-mc' },
    @{ id = 'manual:smoke3'; name = 'Black Myth: Wukong'; exe = (New-FakeGame 'BlackMythWukong\b1\Binaries\Win64' 'b1-Win64-Shipping.exe'); source = 'Manual' },
    @{ id = 'manual:smoke4'; name = 'Alan Wake 2'; exe = (New-FakeGame 'AlanWake2' 'AlanWake2.exe'); source = 'Manual' }
)
foreach ($g in $games) { $g.root = Split-Path -Parent $g.exe; $g.installed = $false }
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $settings 'library.json'), (@{ Games = $games } | ConvertTo-Json -Depth 5), $utf8)
[IO.File]::WriteAllText((Join-Path $settings 'settings.json'), (@{ Language = 'zh'; Theme = 'light'; ScanOnStartup = $false; RecentGameId = 'manual:smoke1' } | ConvertTo-Json), $utf8)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
& $smoke $exe $settings $OutDir
$code = $LASTEXITCODE
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$size = (Get-Item -LiteralPath $exe).Length
Write-Host ("Launcher size: {0:N0} bytes" -f $size)
if ($size -gt 1600000) { throw "Launcher grew beyond 1.6 MB ($size bytes)" }
if ($code -ne 0) { throw "Launcher smoke test failed ($code)" }
