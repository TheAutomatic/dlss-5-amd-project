<#
.SYNOPSIS
  Install this project's OptiScaler into a game folder.
  Double-click Setup.bat (no args) to pick the game folder, or pass -GameDir.
  Copies the original author's 0.3.0 runtime (version.dll) to dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs THIS project (B path). Does not leave original-author version.dll in the game.

  Put these in the SAME folder as Setup.ps1 (the package root):
    OptiScaler.dll              this fork
    OptiScaler.ini              optional
    OptiScaler\                 FFX / XeSS / Agility deps
    version.dll                 author AMD NR 0.3.0 (copied to pass1-3)
    nvngx_dlssnr.dll            optional, to generate weights with original-author setup
    dlssnr_on_amd_setup.exe     optional, original author's 0.3.0 setup
    dlssnr_on_amd_weights.bin   optional if you already have it

.EXAMPLE
  .\Setup.bat
  .\Setup.bat "D:\Games\Foo\Content"
  .\install-amd-presr.ps1 -GameDir 'D:\Games\Foo' -Proxy dxgi.dll
#>
[CmdletBinding()]
param(
    # Empty = open a folder picker (double-click Setup.bat).
    [string]$GameDir = '',
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

# 不要用 Get-FileHash：它属于 Microsoft.PowerShell.Utility，靠模块自动加载。
# 当环境里的 PSModulePath 指向 PowerShell 7 的模块目录时（从 pwsh 终端启动、
# 或 CI 里在 shell: pwsh 步骤里调 powershell -File），5.1 子进程加载不到它，
# 会直接报 CommandNotFoundException —— Setup.bat 经 cmd 走的正是这条路。
# 用 .NET 自己算，不依赖任何模块。
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
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

function Ask-GameFolder {
    # IFileDialog with FOS_PICKFOLDERS: has an address bar (unlike FolderBrowserDialog).
    try {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class AmdFolderPick {
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialogRCW { }
    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileDialog {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(uint count, IntPtr filters);
        void SetFileTypeIndex(uint index);
        void GetFileTypeIndex(out uint index);
        void Advise(IntPtr sink, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint options);
        void GetOptions(out uint options);
        void SetDefaultFolder(IntPtr folder);
        void SetFolder(IntPtr folder);
        void GetFolder(out IntPtr folder);
        void GetCurrentSelection(out IntPtr item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName(out IntPtr name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void GetResult(out IntPtr item);
        void AddPlace(IntPtr item, int order);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string ext);
        void Close(int hr);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr filter);
        void GetResults(out IntPtr items);
        void GetSelectedItems(out IntPtr items);
    }
    [ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem {
        void BindToHandler(IntPtr bc, ref Guid bh, ref Guid riid, out IntPtr ppv);
        void GetParent(out IntPtr ppsi);
        void GetDisplayName(uint sigdnName, out IntPtr ppszName);
        void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        void Compare(IntPtr psi, uint hint, out int piOrder);
    }
    const uint FOS_PICKFOLDERS = 0x20;
    const uint FOS_FORCEFILESYSTEM = 0x40000;
    const uint SIGDN_FILESYSPATH = 0x80058000;
    public static string PickFolder(string title) {
        var dlg = (IFileDialog)new FileOpenDialogRCW();
        uint opts;
        dlg.GetOptions(out opts);
        dlg.SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (!string.IsNullOrEmpty(title)) dlg.SetTitle(title);
        if (dlg.Show(IntPtr.Zero) != 0) return null;
        IntPtr item;
        dlg.GetResult(out item);
        var isi = (IShellItem)Marshal.GetObjectForIUnknown(item);
        IntPtr pathPtr;
        isi.GetDisplayName(SIGDN_FILESYSPATH, out pathPtr);
        string path = Marshal.PtrToStringUni(pathPtr);
        Marshal.FreeCoTaskMem(pathPtr);
        Marshal.Release(item);
        return path;
    }
}
"@ -ErrorAction Stop
        $picked = [AmdFolderPick]::PickFolder('Select the game folder that contains the game .exe')
        if ($picked) { return $picked }
        return $null
    } catch {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Select the game folder that contains the game .exe'
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        return $null
    }
}

# Double-click Setup.bat: no path argument → open a folder picker.
if ([string]::IsNullOrWhiteSpace($GameDir)) {
    if ($NonInteractive) { Fail 'GameDir is required in -NonInteractive mode.' }
    Write-Host 'Pick the game folder (the one with the game .exe)…' -ForegroundColor Yellow
    $GameDir = Ask-GameFolder
    if ([string]::IsNullOrWhiteSpace($GameDir)) {
        Write-Host 'Cancelled — no folder selected.'
        exit 0
    }
}

# Interactive proxy pick (like older 1.7.x installers). dinput8 is invalid for this build.
if (-not $NonInteractive -and -not $PSBoundParameters.ContainsKey('Proxy')) {
    $proxyOptions = @(
        'dxgi.dll      (recommended)',
        'winmm.dll',
        'd3d12.dll',
        'winhttp.dll',
        'wininet.dll',
        'dbghelp.dll'
    )
    $pi = Ask-Choice 'Which proxy DLL should OptiScaler install as?' $proxyOptions
    $Proxy = ($proxyOptions[$pi - 1] -split '\s+')[0]
    Write-Host "Selected proxy: $Proxy"
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path

# Store packages: WindowsApps is not a writable install target (ACL / TrustedInstaller).
if ($game -match '(?i)\\WindowsApps\\') {
    Fail @"
Refusing to install into WindowsApps:
  $game

That path is not a reliable write target.
Pick the writable game folder (the one that contains the game .exe and accepts file copies).
"@
}

$probe = Join-Path $game ('.write-probe-' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText($probe, 'ok')
    Remove-Item -LiteralPath $probe -Force
} catch {
    Fail "Cannot write to $game ($($_.Exception.Message)). Pick a writable folder next to the game .exe."
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

function Confirm-Continue([string]$title) {
    if ($NonInteractive) { Fail $title }
    $choice = Ask-Choice $title @('Cancel and exit', 'Continue anyway')
    if ($choice -eq 1) { Write-Host 'Cancelled.'; exit 0 }
}

function Find-FirstFile([string[]]$paths) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return $p }
    }
    return $null
}

# Author setup (dlssnr_on_amd_setup.exe) is what produces version.dll and
# dlssnr_on_amd_weights.bin. Look in the package folder AND the game folder.
$setup   = Join-Path $Root 'dlssnr_on_amd_setup.exe'
$nv      = Find-FirstFile @(
    (Join-Path $Root 'nvngx_dlssnr.dll'),
    (Join-Path $Root 'nvngx_dlss.dll'),
    (Join-Path $game 'nvngx_dlssnr.dll'),
    (Join-Path $game 'nvngx_dlss.dll')
)
$weights = Find-FirstFile @(
    (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
    (Join-Path $game 'dlssnr_on_amd_weights.bin')
)
$srcA = $AuthorDll
if (-not $srcA) {
    $srcA = Find-FirstFile @(
        (Join-Path $Root 'version.dll'),
        (Join-Path $Root 'dlssnr_amd_pass1.dll'),
        (Join-Path $game 'version.dll'),
        (Join-Path $game 'dlssnr_amd_pass1.dll')
    )
}

# Missing runtime and/or weights → run the original-author setup first (it writes both).
if ((-not $srcA -or -not $weights) -and (Test-Path -LiteralPath $setup -PathType Leaf)) {
    Write-Host ''
    Write-Host 'version.dll and/or weights.bin not found yet.' -ForegroundColor Yellow
    Write-Host 'Launching original-author setup (dlssnr_on_amd_setup.exe) to create them…' -ForegroundColor Yellow
    if ($nv) { Write-Host "  nvngx found: $nv" } else {
        Write-Host '  NOTE: no nvngx_dlssnr.dll next to Setup.bat or in the game folder.' -ForegroundColor Yellow
        Write-Host '  The original-author setup will ask you to locate it if it needs one for weights.' -ForegroundColor Yellow
    }
    Write-Host '  In the author UI: pick the GAME folder if asked, finish install/close when done.'
    Push-Location $Root
    try {
        $p = Start-Process -FilePath $setup -WorkingDirectory $Root -Wait -PassThru
        Write-Host "  original-author setup exit code: {0}" -f $p.ExitCode
    } finally { Pop-Location }

    # Re-scan: setup may drop files in the package dir or install into the game.
    $weights = Find-FirstFile @(
        (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
        (Join-Path $game 'dlssnr_on_amd_weights.bin')
    )
    if (-not $srcA) {
        $srcA = Find-FirstFile @(
            (Join-Path $Root 'version.dll'),
            (Join-Path $Root 'dlssnr_amd_pass1.dll'),
            (Join-Path $game 'version.dll'),
            (Join-Path $game 'dlssnr_amd_pass1.dll')
        )
    }
}

# --- author 0.3.0 runtime ---
if (-not $srcA -or !(Test-Path -LiteralPath $srcA -PathType Leaf)) {
    Fail @"
Still missing DLSS-NR-on-AMD 0.3.0 runtime (version.dll) after original-author setup.
1. Run dlssnr_on_amd_setup.exe yourself and finish its install
2. Put the version.dll it produces next to Setup.bat (or leave it in the game folder)
Download 0.3.0 from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
}

# Hash: only 0.3.0 is supported. The RVA layout is pinned to that binary —
# a different build will not run correctly. Fail closed; do not offer "continue".
$expectedA03 = '8321CAE728D28CB7632D0D58D3D913E91132BF7645C126505698FBE4CD5A0138'
$knownA0217  = 'BC97F3B06718E19042ACAF227BFE15D1E43D4977F9DC2E39994FCC511445FF4E'
$hashA = Get-Sha256 $srcA
Write-Host ("Author runtime SHA256: {0}" -f $hashA)
if ($hashA -ne $expectedA03) {
    $what = 'unknown build'
    if ($hashA -eq $knownA0217) { $what = 'this is 0.2.17, not 0.3.0' }
    Fail @"
$srcA is not DLSS-NR-on-AMD 0.3.0 ($what).
  file:     $srcA
  got:      $hashA
  expected: $expectedA03
Download 0.3.0 from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
}

# Stage the install source OUTSIDE the game folder. Author setup may have written
# version.dll into the game dir; backup/move must not steal the file we still need.
$stagedA = $null
try {
    $srcAFull = [IO.Path]::GetFullPath($srcA)
    $gameFull = [IO.Path]::GetFullPath($game)
    # Require a directory boundary so C:\Games\MyGame does not match C:\Games\MyGame-pkg.
    $gamePrefix = $gameFull.TrimEnd('\') + '\'
    if ($srcAFull.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $stagedA = Join-Path $Root ('._staged_' + [guid]::NewGuid().ToString('N') + '.dll')
        Copy-Item -LiteralPath $srcAFull -Destination $stagedA -Force
        Write-Host "Staged install source outside game folder: $stagedA"
        $srcA = $stagedA
    }
} catch {
    Fail "Could not stage author runtime from $srcA : $($_.Exception.Message)"
}

# --- weights ---
if (-not $weights) {
    $weights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
}
if (-not (Test-Path -LiteralPath $weights -PathType Leaf)) {
    if ((Test-Path $setup) -and $nv) {
        Write-Host 'weights.bin still missing — running original-author setup again with nvngx…'
        Push-Location $Root
        try { Start-Process -FilePath $setup -WorkingDirectory $Root -Wait | Out-Null } finally { Pop-Location }
        $weights = Find-FirstFile @(
            (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
            (Join-Path $game 'dlssnr_on_amd_weights.bin')
        )
    }
}
if (-not $weights -or !(Test-Path -LiteralPath $weights -PathType Leaf)) {
    Fail @"
Missing dlssnr_on_amd_weights.bin.
Run dlssnr_on_amd_setup.exe (with nvngx_dlssnr.dll available), then retry.
"@
}

if ($nv) {
    $nvSize = (Get-Item -LiteralPath $nv).Length
    Write-Host ("nvngx size: {0} bytes  ({1})" -f $nvSize, $nv)
    if ($nvSize -lt 1MB) {
        Confirm-Continue ("nvngx is only {0} bytes — may be the wrong file. Continue?" -f $nvSize)
    }
}

$wsize = (Get-Item -LiteralPath $weights).Length
$whash = Get-Sha256 $weights
Write-Host ("weights.bin size={0}  SHA256={1}" -f $wsize, $whash)
Write-Host '  (weights SHA256 is per-machine; not compared to a fixed value)'
if ($wsize -lt 1MB) {
    Confirm-Continue ("weights.bin is only {0} bytes — looks truncated or wrong. Continue?" -f $wsize)
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
Write-Host "Original author 0.3.0: $srcA  -> will be copied as dlssnr_amd_pass1/2/3.dll"
Write-Host 'NOTE: original-author version.dll is NOT installed here (B path only).'
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
    # Author-native version.dll (hash already known) must not stay next to B.
    $isAuthorNative = $false
    if ($f.Name -ieq 'version.dll' -and -not $f.IsOptiScaler) {
        try {
            $h = Get-Sha256 $f.Path
            if ($h -eq $expectedA03 -or $h -eq $knownA0217) { $isAuthorNative = $true }
        } catch { }
    }
    if ($isAuthorNative) {
        Write-Host ("{0} is the original-author NR runtime — moving aside (cannot coexist with B)." -f $f.Name) -ForegroundColor Yellow
        $toMove += $f
        continue
    }
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
    # Reuse a verified file that is already at the destination (e.g. weights/pass in game dir).
    try {
        $srcFull = [IO.Path]::GetFullPath($src)
        $destFull = [IO.Path]::GetFullPath($dest)
        if ($srcFull -ieq $destFull) {
            Write-Host ("Skip (already in place): {0}" -f $rel)
            return
        }
    } catch { }
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
# Drop the temp staged copy (package root only).
if ($stagedA -and (Test-Path -LiteralPath $stagedA)) {
    try { Remove-Item -LiteralPath $stagedA -Force } catch { }
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
Write-Host '  Installed: OptiScaler (this project) + dlssnr_amd_pass1-3.dll (copies of original-author 0.3.0) + weights'
Write-Host ''
Write-Host 'Next (in game):' -ForegroundColor Yellow
Write-Host '  1. Launch the game'
Write-Host '  2. Press Insert (Ins) to open the OptiScaler menu'
Write-Host '  3. Enable DLSSNR'
exit 0
