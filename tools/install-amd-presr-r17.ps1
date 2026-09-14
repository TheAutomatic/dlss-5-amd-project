<#
.SYNOPSIS
  Install this project's OptiScaler (r17) into a game folder.
  Takes the author's 0.3.0 version.dll, copies it as dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs THIS project (B path). Does not drop author version.dll into the game.
  Native 0.3 (version.dll alone) is a different mode — install that yourself if wanted.

  vendor\
    version.dll                   author AMD NR 0.3.0 (same binary used as pass)
    dlssnr_on_amd_setup.exe       optional, author's local setup
    nvngx_dlss.dll                optional, only from YOUR game (for weights)
    dlssnr_on_amd_weights.bin     optional if you already have it

  release\
    OptiScaler.dll                this fork (r17)
    OptiScaler.ini                optional
    OptiScaler\                   optional FFX/XeSS deps

.EXAMPLE
  .\install-amd-presr-r17.ps1 -GameDir 'D:\Games\Foo' -Proxy dxgi.dll
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [ValidateSet('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll','dinput8.dll')]
    [string]$Proxy = 'dxgi.dll',
    [string]$Root = $PSScriptRoot,
    [string]$AuthorDll,
    [switch]$NonInteractive
)
$ErrorActionPreference = 'Stop'
$vendor  = Join-Path $Root 'vendor'
$release = Join-Path $Root 'release'

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
    Fail "Missing release\OptiScaler.dll (this project's r17)."
}

# --- author 0.3.0 runtime: version.dll from their package ---
$srcA = $AuthorDll
if (-not $srcA) {
    foreach ($cand in @(
        (Join-Path $vendor 'version.dll'),
        (Join-Path $vendor 'dlssnr_amd_pass1.dll'),
        (Join-Path $vendor '0.3.0\version.dll'),
        (Join-Path $vendor 'dlssnr-on-amd-0.3.0\version.dll')
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $srcA = $cand; break }
    }
}
if (-not $srcA -or !(Test-Path -LiteralPath $srcA -PathType Leaf)) {
    Fail @"
Missing author AMD NR 0.3.0 binary.
Put the author's version.dll under vendor\ (or pass -AuthorDll 'D:\path\version.dll').
This tool does not bundle or download it. pass1-3.dll are copies of that same file.
"@
}

# --- weights ---
$weights = Join-Path $vendor 'dlssnr_on_amd_weights.bin'
if (!(Test-Path -LiteralPath $weights -PathType Leaf)) {
    $setup = Join-Path $vendor 'dlssnr_on_amd_setup.exe'
    $nv    = Join-Path $vendor 'nvngx_dlss.dll'
    if ((Test-Path $setup) -and (Test-Path $nv)) {
        Write-Host 'weights.bin missing — running author setup locally with your nvngx_dlss.dll...'
        Push-Location $vendor
        try { & $setup | Out-Host } finally { Pop-Location }
    }
    if (!(Test-Path -LiteralPath $weights -PathType Leaf)) {
        Fail 'Missing vendor\dlssnr_on_amd_weights.bin. Generate it on this PC with the author setup from your own NV DLL.'
    }
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
            3 { if ($isTarget) { $toMove += $f } else { Write-Host ("Leaving {0}." -f $f.Name) } }
        }
    } else {
        if ($NonInteractive) {
            Write-Host ("WARNING: {0} exists (not OptiScaler)." -f $f.Name)
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
            3 { Write-Host ("Keeping {0}." -f $f.Name) }
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
Write-Host '  Enable NR in menu/INI. Every-frame multi-slot is default in r17.'
exit 0
