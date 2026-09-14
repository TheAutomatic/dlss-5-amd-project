<#
.SYNOPSIS
  Install this project's OptiScaler into a game folder.
  Copies the author's 0.3.0 runtime (version.dll) to dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs THIS project (B path). Does not leave author version.dll in the game.

  Put these in the SAME folder as Setup.ps1 (the package root):
    OptiScaler.dll              this fork
    OptiScaler.ini              optional
    OptiScaler\                 FFX / XeSS / Agility deps
    version.dll                 author AMD NR 0.3.0 (copied to pass1-3)
    nvngx_dlss.dll              optional, only from YOUR game (for weights)
    dlssnr_on_amd_setup.exe     optional, author's local setup
    dlssnr_on_amd_weights.bin   optional if you already have it

.EXAMPLE
  .\install-amd-presr.ps1 -GameDir 'D:\Games\Foo' -Proxy dxgi.dll
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    # dinput8 is NOT a valid proxy for this OptiScaler build (no DirectInput8Create export).
    [ValidateSet('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll')]
    [string]$Proxy = 'dxgi.dll',
    [string]$Root,
    [string]$AuthorDll,
    [switch]$NonInteractive
)
$ErrorActionPreference = 'Stop'

# $Root 绝不能写成 param 默认值 $PSScriptRoot：用 powershell -File 调用时，
# 参数绑定阶段 $PSScriptRoot 还是空的（脚本体内才被赋值）。
# Setup.bat 走的正是 -File。
if (-not $Root) { $Root = $PSScriptRoot }

# 包布局：一切都在包根（与上游 OptiScaler 包一致）。
# 早期包把 DLL 放在 release\ 下，再兜底一次。
$release = $Root
if (!(Test-Path -LiteralPath (Join-Path $release 'OptiScaler.dll')) -and
    (Test-Path -LiteralPath (Join-Path $Root 'release\OptiScaler.dll'))) {
    $release = Join-Path $Root 'release'
}

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Test-OptiProxy([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        return ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler')
    } catch { return $false }
}

function Ask-Choice([string]$title, [string[]]$options) {
    Write-Host ''
    Write-Host $title -ForegroundColor Yellow
    for ($i = 0; $i -lt $options.Count; $i++) {
        Write-Host ("  [{0}] {1}" -f ($i + 1), $options[$i])
    }
    do {
        $ans = Read-Host 'Enter number'
        $n = 0
        if ([int]::TryParse($ans, [ref]$n) -and $n -ge 1 -and $n -le $options.Count) { return $n }
        Write-Host 'Invalid choice.'
    } while ($true)
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path

# Xbox / MS Store: do not write into WindowsApps (store ACL / TrustedInstaller).
# Use the writable tree, e.g. C:\XboxGames\<Game>\Content
if ($game -match '(?i)\\WindowsApps\\') {
    Fail @"
Refusing to install into WindowsApps:
  $game

That path is not a reliable write target.
For Xbox/MS Store games use the writable folder, for example:
  C:\XboxGames\<GameName>\Content
Do not select the .exe under Program Files\WindowsApps.
"@
}
if ($game -match '(?i)^[A-Za-z]:\\XboxGames\\' -and (Split-Path -Leaf $game) -ne 'Content') {
    Write-Host 'NOTE: Xbox game root detected. If a write fails, use the Content subfolder.' -ForegroundColor Yellow
}

$probe = Join-Path $game ('.write-probe-' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText($probe, 'ok')
    Remove-Item -LiteralPath $probe -Force
} catch {
    Fail "Cannot write to $game ($($_.Exception.Message)). For XGP use ...\Content, not the package/exe folder."
}

if (!(Test-Path -LiteralPath (Join-Path $release 'OptiScaler.dll'))) {
    Fail @"
Missing OptiScaler.dll.
Looked in:
  $release\OptiScaler.dll
  $Root\release\OptiScaler.dll

Run Setup.bat from the folder you unzipped the package into, so that
OptiScaler.dll sits next to Setup.ps1.
"@
}

# --- author 0.3.0 runtime: version.dll next to Setup.ps1 ---
$srcA = $AuthorDll
if (-not $srcA) {
    foreach ($cand in @(
        (Join-Path $Root 'version.dll'),
        (Join-Path $Root 'dlssnr_amd_pass1.dll')
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $srcA = $cand; break }
    }
}
if (-not $srcA -or !(Test-Path -LiteralPath $srcA -PathType Leaf)) {
    Fail @"
Missing DLSS-NR-on-AMD 0.3.0 runtime.
Put version.dll from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
in the same folder as Setup.ps1 (or pass -AuthorDll 'D:\path\version.dll').
This tool does not bundle it. pass1-3.dll are copies of that same file.
"@
}

# Hash: only 0.3.0 is supported. Mismatch = ask before continuing.
$expectedA03 = '8321CA728D28CB7632D0D58D3D913E91132BF7645C126505698FBE4CD5A0138'
$knownA0217  = 'BC97F3B06718E19042ACAF227BFE15D1E43D4977F9DC2E39994FCC511445FF4E'
$hashA = (Get-FileHash -LiteralPath $srcA -Algorithm SHA256).Hash
Write-Host ("Author runtime SHA256: {0}" -f $hashA)
if ($hashA -ne $expectedA03) {
    $what = 'unknown build'
    if ($hashA -eq $knownA0217) { $what = 'looks like 0.2.17 (this package requires 0.3.0)' }
    Write-Host ''
    Write-Host ("WARNING: {0} may not be DLSS-NR-on-AMD 0.3.0 ({1})." -f (Split-Path -Leaf $srcA), $what) -ForegroundColor Yellow
    Write-Host ("  expected 0.3.0 SHA256: {0}" -f $expectedA03)
    Write-Host "  source: https://github.com/danielblnc/DLSS-NR-on-AMD/releases"
    if ($NonInteractive) {
        Fail 'Refusing to install a non-0.3.0 runtime in -NonInteractive. Re-run without -NonInteractive to force, or replace version.dll.'
    }
    $choice = Ask-Choice 'Continue anyway with this file?' @(
        'Cancel and exit'
        'Continue anyway (unsupported runtime)'
    )
    if ($choice -eq 1) { Write-Host 'Cancelled.'; exit 0 }
    Write-Host 'Continuing with a non-matching runtime.' -ForegroundColor Yellow
}

# --- weights (same folder as Setup) ---
$weights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
if (!(Test-Path -LiteralPath $weights -PathType Leaf)) {
    $setup = Join-Path $Root 'dlssnr_on_amd_setup.exe'
    $nv    = Join-Path $Root 'nvngx_dlss.dll'
    if ((Test-Path $setup) -and (Test-Path $nv)) {
        Write-Host 'weights.bin missing — running author setup locally with your nvngx_dlss.dll...'
        Push-Location $Root
        try { & $setup | Out-Host } finally { Pop-Location }
    }
    if (!(Test-Path -LiteralPath $weights -PathType Leaf)) {
        Fail 'Missing dlssnr_on_amd_weights.bin next to Setup.ps1. Generate it on this PC with the author setup from your own NV DLL.'
    }
}
# Weights are generated on the user's PC from their NV DLL — SHA256 is not fixed.
# Only sanity-check size so an empty/truncated file is obvious.
$wsize = (Get-Item -LiteralPath $weights).Length
Write-Host ("weights.bin size: {0} bytes" -f $wsize)
if ($wsize -lt 1MB) {
    Write-Host ("WARNING: {0} is only {1} bytes — looks truncated or wrong." -f (Split-Path -Leaf $weights), $wsize) -ForegroundColor Yellow
    if ($NonInteractive) {
        Fail 'Refusing tiny weights.bin in -NonInteractive.'
    }
    $choice = Ask-Choice 'Continue anyway with this weights file?' @(
        'Cancel and exit'
        'Continue anyway'
    )
    if ($choice -eq 1) { Write-Host 'Cancelled.'; exit 0 }
}

# --- inspect common injection DLLs ---
$proxies = @(
    'dxgi.dll','winmm.dll','d3d12.dll','version.dll',
    'winhttp.dll','wininet.dll','dbghelp.dll','dinput8.dll'
)
$found = @()
foreach ($name in $proxies) {
    $p = Join-Path $game $name
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $item = Get-Item -LiteralPath $p
        $found += [pscustomobject]@{
            Name = $name
            Path = $p
            Size = $item.Length
            IsOptiScaler = (Test-OptiProxy $p)
        }
    }
}

Write-Host ''
Write-Host "Game folder: $game"
Write-Host "Proxy:       $Proxy   (OptiScaler.dll installed under this name)"
Write-Host "Author A:    $srcA  -> will be copied as dlssnr_amd_pass1/2/3.dll"
Write-Host 'NOTE: author version.dll is NOT installed here (B path only).'
if ($found.Count -eq 0) {
    Write-Host 'No common injection DLLs found in the game folder.' -ForegroundColor Green
} else {
    Write-Host 'Existing injection-related DLLs:' -ForegroundColor Yellow
    foreach ($f in $found) {
        $tag = if ($f.IsOptiScaler) { ' [OptiScaler]' } else { '' }
        Write-Host ("  - {0}  ({1} bytes){2}" -f $f.Name, $f.Size, $tag)
    }
    Write-Host 'For ReShade or another mod: choose Ignore only if you know they can coexist.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $game "backup-amd-presr-$stamp"
$toMove = @()
# Files the user chose to keep. The install step must not overwrite these.
$keep = @{}

foreach ($f in $found) {
    $isTarget = ($f.Name -ieq $Proxy)
    if ($f.IsOptiScaler) {
        if ($NonInteractive) { $toMove += $f; continue }
        $choice = Ask-Choice ("{0} is already an OptiScaler install. How to continue?" -f $f.Name) @(
            'Cancel install'
            'Backup and move aside, then install'
            'Ignore (overwrite only if it is the chosen proxy; leave others)'
        )
        switch ($choice) {
            1 { Write-Host 'Cancelled.'; exit 0 }
            2 { $toMove += $f }
            3 {
                if ($isTarget) { $toMove += $f }
                else {
                    $keep[$f.Name] = $true
                    Write-Host ("Leaving {0}." -f $f.Name)
                }
            }
        }
    } else {
        if ($NonInteractive) {
            if ($isTarget) {
                Fail ("{0} exists and is not OptiScaler. Refusing to overwrite in -NonInteractive. Backup/remove it or pick another -Proxy." -f $f.Name)
            }
            Write-Host ("WARNING: {0} exists (not OptiScaler); leaving it." -f $f.Name)
            $keep[$f.Name] = $true
            continue
        }
        $choice = Ask-Choice ("{0} exists and is not OptiScaler. How to continue?" -f $f.Name) @(
            'Cancel install'
            'Backup and move aside, then install'
            'Ignore and continue (keep this file)'
        )
        switch ($choice) {
            1 { Write-Host 'Cancelled.'; exit 0 }
            2 { $toMove += $f }
            3 {
                if ($isTarget) {
                    Fail ("You chose to keep {0}, but that is also the proxy name we would install as. Pick a different -Proxy (e.g. winmm.dll) or choose Backup and move aside." -f $f.Name)
                }
                $keep[$f.Name] = $true
                Write-Host ("Keeping {0}." -f $f.Name)
            }
        }
    }
}

New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($f in $toMove) {
    Copy-Item -LiteralPath $f.Path -Destination (Join-Path $backup $f.Name) -Force
    Move-Item -LiteralPath $f.Path -Destination (Join-Path $backup ($f.Name + '.moved')) -Force
    Write-Host ("Moved {0} -> backup" -f $f.Name)
}

function Install-One([string]$src, [string]$rel) {
    $leaf = Split-Path -Leaf $rel
    if ($keep.ContainsKey($leaf)) {
        Write-Host ("Skip (user kept existing file): {0}" -f $rel)
        return
    }
    $dest = Join-Path $game $rel
    if (Test-Path -LiteralPath $dest) {
        $save = Join-Path $backup $rel
        $sdir = Split-Path -Parent $save
        if ($sdir) { New-Item -ItemType Directory -Path $sdir -Force | Out-Null }
        Copy-Item -LiteralPath $dest -Destination $save -Force
    }
    $ddir = Split-Path -Parent $dest
    if ($ddir) { New-Item -ItemType Directory -Path $ddir -Force | Out-Null }
    Copy-Item -LiteralPath $src -Destination $dest -Force
}

Write-Host ''
Write-Host "Installing as $Proxy + pass copies from author 0.3.0 ..."
Install-One (Join-Path $release 'OptiScaler.dll') $Proxy

# Same runtime bytes as native version.dll — three filenames so multi-pass can load independent instances.
foreach ($p in 1..3) {
    Install-One $srcA ("dlssnr_amd_pass$p.dll")
}
Install-One $weights 'dlssnr_on_amd_weights.bin'

$ini = Join-Path $release 'OptiScaler.ini'
if ((Test-Path $ini) -and !(Test-Path (Join-Path $game 'OptiScaler.ini'))) {
    Install-One $ini 'OptiScaler.ini'
}
$deps = Join-Path $release 'OptiScaler'
if (Test-Path $deps) {
    Get-ChildItem -LiteralPath $deps -Recurse -File | ForEach-Object {
        $rel = Join-Path 'OptiScaler' $_.FullName.Substring($deps.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  Game:   $game"
Write-Host "  Proxy:  $Proxy"
Write-Host "  Backup: $backup"
Write-Host '  Installed: OptiScaler (this project) + dlssnr_amd_pass1-3.dll (copies of author 0.3.0) + weights'
Write-Host '  Not installed: author version.dll (native mode) — do that separately if you want it.'
Write-Host '  Enable NR in menu/INI. Every-frame multi-slot is the default.'
exit 0
