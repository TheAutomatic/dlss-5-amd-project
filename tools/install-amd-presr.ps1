<#
.SYNOPSIS
  Install this project's OptiScaler into a game folder.
  Double-click Setup.bat (no args) to pick the game folder, or pass -GameDir.
  Copies the original author's 0.3.1 or 0.3.0 runtime (version.dll) to dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs this project. Does not leave original-author version.dll in the game.

  Put these in the SAME folder as Setup.ps1 (the package root):
    OptiScaler.dll              this fork
    OptiScaler.ini              optional
    OptiScaler\                 FFX / XeSS / Agility deps
    version.dll                 author AMD NR 0.3.1 or 0.3.0 (copied to pass1-3)
    nvngx_dlssnr.dll            optional, to generate weights with original-author setup
    dlssnr_on_amd_setup.exe     optional, original author's 0.3.1 / 0.3.0 setup
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
    [switch]$NonInteractive,
    # Setup.bat owns the final pause; keep all folder/proxy/confirmation prompts.
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'

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
    Write-Host 'Install FAILED.' -ForegroundColor Red
    Pause-Exit 1
}

# Catch errors outside the individual file-operation handlers (for example,
# an invalid author setup executable). The batch also catches parser/parameter
# binding failures, which happen before this script body can run.
trap { Fail ("Unexpected install error: " + $_.Exception.Message) }

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
    # IFileDialog + FOS_PICKFOLDERS: address bar works. A parentless console
    # dialog often shows an empty nav pane unless it has an owner window.
    try {
        Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
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
    const uint FOS_NOCHANGEDIR = 0x8;
    const uint SIGDN_FILESYSPATH = 0x80058000;
    const uint SIGDN_NORMALDISPLAY = 0;
    static readonly Guid IID_IShellItem = new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe");
    // This PC / My Computer — populates the left navigation tree.
    const string ThisPcParsingName = "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    static extern int SHCreateItemFromParsingName(
        [MarshalAs(UnmanagedType.LPWStr)] string path,
        IntPtr pbc, ref Guid riid, out IntPtr ppv);
    static IntPtr ItemFromParsingName(string path) {
        Guid iid = IID_IShellItem;
        IntPtr item;
        int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out item);
        return hr == 0 ? item : IntPtr.Zero;
    }
    public static string PickFolder(string title) {
        var dlg = (IFileDialog)new FileOpenDialogRCW();
        uint opts;
        dlg.GetOptions(out opts);
        // Do NOT set FOS_FORCEFILESYSTEM: it can leave the left nav tree empty
        // for a parentless console host. FOS_PICKFOLDERS is enough.
        dlg.SetOptions(opts | FOS_PICKFOLDERS | FOS_NOCHANGEDIR);
        if (!string.IsNullOrEmpty(title)) dlg.SetTitle(title);
        // Parentless IFileDialog often shows a blank nav pane. A tiny hidden
        // owner window gives the shell a host to hang the tree on.
        IntPtr owner = IntPtr.Zero;
        try {
            var form = new Form {
                ShowInTaskbar = false,
                WindowState = FormWindowState.Minimized,
                StartPosition = FormStartPosition.Manual,
                Location = new Point(-32000, -32000),
                Size = new Size(1, 1)
            };
            form.Show();
            form.Hide();
            owner = form.Handle;
        } catch { owner = IntPtr.Zero; }
        IntPtr thisPc = ItemFromParsingName(ThisPcParsingName);
        if (thisPc != IntPtr.Zero) {
            try { dlg.SetDefaultFolder(thisPc); } catch { }
            try { dlg.SetFolder(thisPc); } catch { }
            Marshal.Release(thisPc);
        }
        int hr = dlg.Show(owner);
        if (owner != IntPtr.Zero) {
            try {
                var form = Control.FromHandle(owner) as Form;
                if (form != null) form.Dispose();
            } catch { }
        }
        if (hr != 0) return null;
        IntPtr result;
        dlg.GetResult(out result);
        var isi = (IShellItem)Marshal.GetObjectForIUnknown(result);
        IntPtr pathPtr;
        isi.GetDisplayName(SIGDN_FILESYSPATH, out pathPtr);
        string path = Marshal.PtrToStringUni(pathPtr);
        Marshal.FreeCoTaskMem(pathPtr);
        Marshal.Release(result);
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
        Pause-Exit 0
    }
}

# Interactive proxy pick (like older 1.7.x installers). dinput8 is invalid for this build.
if (-not $NonInteractive -and -not $PSBoundParameters.ContainsKey('Proxy')) {
    $proxyOptions = @(
        'dxgi.dll (default; if the game fails to start, try winmm.dll)',
        'winmm.dll (recommended by some games)',
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

# The game must be closed: it holds dxgi/pass/weights while running.
function Test-FileLocked([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $false
    } catch [System.IO.IOException] { return $true }
    catch { return $false }
}

$locked = @()
foreach ($name in @($Proxy, 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll', 'dlssnr_amd_pass3.dll', 'dlssnr_on_amd_weights.bin')) {
    $p = Join-Path $game $name
    if (Test-FileLocked $p) { $locked += $name }
}
if ($locked.Count -gt 0) {
    Fail @"
The game is still running (file lock detected):
  $($locked -join ', ')

Close the game completely, then run Setup.bat again.
"@
}
# Also flag a live process whose image lives in the game folder.
$gameExes = @(Get-ChildItem -LiteralPath $game -Filter '*.exe' -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
if ($gameExes.Count -gt 0) {
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $n = "$($_.ProcessName).exe"
        ($gameExes -contains $n)
    }
    if ($running) {
        $names = ($running | ForEach-Object { "$($_.ProcessName).exe (PID $($_.Id))" }) -join ', '
        Fail @"
A game process is still running:
  $names

Close the game completely, then run Setup.bat again.
"@
    }
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
    if ($choice -eq 1) { Write-Host 'Cancelled.'; Pause-Exit 0 }
}

function Find-FirstFile([string[]]$paths) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return $p }
    }
    return $null
}

# Hash: only known author runtimes are supported. The RVA layout is pinned to
# each binary — a different build will not run correctly. Fail closed.
# 0.3.0 = AmdLayout.h kAmd03; 0.3.1 = kAmd031 (mapped 2026-09-16).
$expectedA030 = '8321CAE728D28CB7632D0D58D3D913E91132BF7645C126505698FBE4CD5A0138'
$expectedA031 = 'B108D6407EB7F094A4F9111EDD778EEE7B978B648D413A9FC7AEEDFDD914C154'
$knownA0217  = 'BC97F3B06718E19042ACAF227BFE15D1E43D4977F9DC2E39994FCC511445FF4E'
$expectedAuthor = @($expectedA030, $expectedA031)

# Walk every candidate and accept only a file whose SHA256 is a known runtime.
# A game may have B installed as version.dll (README allows that); the first
# same-named file must not block a valid pass1.dll sitting next to it.
function Find-AuthorRuntime {
    $candidates = @()
    if ($AuthorDll) { $candidates += $AuthorDll }
    $candidates += @(
        (Join-Path $Root 'version.dll'),
        (Join-Path $Root 'dlssnr_amd_pass1.dll'),
        (Join-Path $game 'version.dll'),
        (Join-Path $game 'dlssnr_amd_pass1.dll')
    )
    foreach ($c in $candidates) {
        if (-not $c -or !(Test-Path -LiteralPath $c -PathType Leaf)) { continue }
        try {
            $h = Get-Sha256 $c
        } catch { continue }
        if ($expectedAuthor -contains $h) { return $c }
    }
    return $null
}

# --- detect lmxxf components ---
$lmxxfRuntime = $null
foreach ($candidate in @(
        (Join-Path $release 'LmxxfNrRuntime.dll'),
        (Join-Path $Root 'LmxxfNrRuntime.dll'),
        (Join-Path $Root 'exports\lmxxf-runtime\LmxxfNrRuntime.dll'),
        (Join-Path $game 'LmxxfNrRuntime.dll')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $lmxxfRuntime = $candidate; break }
}
$lmxxfMods = $null
foreach ($candidate in @(
        (Join-Path $release 'lmxxf-modules'),
        (Join-Path $Root 'lmxxf-modules'),
        (Join-Path $Root 'third_party\lmxxf\modules'),
        (Join-Path $game 'lmxxf-modules')
    )) {
    if ((Test-Path -LiteralPath $candidate -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'SHA256SUMS') -PathType Leaf)) {
        $lmxxfMods = $candidate
        break
    }
}
$lmxxfShaders = $null
foreach ($candidate in @(
        (Join-Path $release 'shaders'),
        (Join-Path $Root 'shaders'),
        (Join-Path $Root 'third_party\lmxxf\shaders'),
        (Join-Path $game 'shaders')
    )) {
    if ((Test-Path -LiteralPath $candidate -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'native_codec_encode.hlsl') -PathType Leaf)) {
        $lmxxfShaders = $candidate
        break
    }
}
$lmxxfWeights = $null
foreach ($candidate in @(
        (Join-Path $release 'native-game-tiled-assets'),
        (Join-Path $Root 'native-game-tiled-assets'),
        (Join-Path $Root 'DLSS5-AMD\native-game-tiled-assets'),
        (Join-Path $game 'native-game-tiled-assets')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        $lmxxfWeights = $candidate
        break
    }
}

# --- detect danielblnc components ---
$setup   = Join-Path $Root 'dlssnr_on_amd_setup.exe'
$nv      = Find-FirstFile @(
    (Join-Path $Root 'nvngx_dlssnr.dll'),
    (Join-Path $game 'nvngx_dlssnr.dll')
)
$weights = Find-FirstFile @(
    (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
    (Join-Path $game 'dlssnr_on_amd_weights.bin')
)
$srcA = Find-AuthorRuntime

$canLmxxf  = [bool]($lmxxfRuntime -and $lmxxfMods)
$canDaniel = [bool]($srcA -or (Test-Path -LiteralPath $setup -PathType Leaf) -or $weights)

$installLmxxf  = $false
$installDaniel = $false
$activeBackend = 'lmxxf'

if ($canLmxxf -and $canDaniel) {
    if ($NonInteractive) {
        $installLmxxf  = $true
        $installDaniel = $true
        $activeBackend = 'lmxxf'
    } else {
        $choice = Ask-Choice "检测到两种神经渲染后端均有可用文件，请选择安装模式 (Select Installation Mode):" @(
            "安装 lmxxf 后端 (Install lmxxf backend - using native-game-tiled-assets)",
            "安装 danielblnc 后端 (Install danielblnc backend - using dlssnr_amd_pass / weights.bin)",
            "同时安装两个后端 (Install both - coexist in game folder, switch via OptiScaler.ini)"
        )
        switch ($choice) {
            1 { $installLmxxf = $true; $installDaniel = $false; $activeBackend = 'lmxxf' }
            2 { $installLmxxf = $false; $installDaniel = $true; $activeBackend = 'daniel' }
            3 {
                $installLmxxf  = $true
                $installDaniel = $true
                $defChoice = Ask-Choice "请选择默认启用的后端 (Select default active backend in OptiScaler.ini):" @(
                    "默认启用 lmxxf 后端 (Default: lmxxf)",
                    "默认启用 danielblnc 后端 (Default: daniel)"
                )
                $activeBackend = if ($defChoice -eq 1) { 'lmxxf' } else { 'daniel' }
            }
        }
    }
} elseif ($canLmxxf) {
    $installLmxxf  = $true
    $installDaniel = $false
    $activeBackend = 'lmxxf'
    Write-Host ''
    Write-Host "检测到 lmxxf 后端组件齐备，将安装 lmxxf 后端。" -ForegroundColor Cyan
    Write-Host "提示：未检测到 danielblnc 后端文件 (缺少 dlssnr_on_amd_setup.exe 或 version.dll)。" -ForegroundColor DarkYellow
} elseif ($canDaniel) {
    $installLmxxf  = $false
    $installDaniel = $true
    $activeBackend = 'daniel'
    Write-Host ''
    Write-Host "检测到 danielblnc 后端组件齐备，将安装 danielblnc 后端。" -ForegroundColor Cyan
    Write-Host "提示：未检测到 lmxxf 后端组件 (缺少 LmxxfNrRuntime.dll 或 lmxxf-modules)。" -ForegroundColor DarkYellow
} else {
    Fail @"
未检测到任何可用的神经渲染后端文件！
- 若使用 lmxxf 后端：请确保安装包内有 LmxxfNrRuntime.dll 与 lmxxf-modules（以及 native-game-tiled-assets 权重文件夹）。
- 若使用 danielblnc 后端：请将 dlssnr_on_amd_setup.exe + nvngx_dlssnr.dll（或现成的 version.dll + weights.bin）放在 Setup.bat 同目录下。
"@
}

# --- process danielblnc runtime if selected ---
$stagedA = $null
if ($installDaniel) {
    $gameHasNv = Test-Path -LiteralPath (Join-Path $game 'nvngx_dlssnr.dll') -PathType Leaf
    $gameHasW  = Test-Path -LiteralPath (Join-Path $game 'dlssnr_on_amd_weights.bin') -PathType Leaf
    if (-not $gameHasNv -and -not $gameHasW) {
        $srcNv = Join-Path $Root 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $srcNv -PathType Leaf) {
            Copy-Item -LiteralPath $srcNv -Destination (Join-Path $game 'nvngx_dlssnr.dll') -Force
            Write-Host 'Copied nvngx_dlssnr.dll into the game folder (original-author runtime expects it there).' -ForegroundColor Green
            $nv = Join-Path $game 'nvngx_dlssnr.dll'
        }
    }

    if ((-not $srcA -or -not $weights) -and (Test-Path -LiteralPath $setup -PathType Leaf)) {
        Write-Host ''
        Write-Host 'version.dll and/or weights.bin not found yet.' -ForegroundColor Yellow
        Write-Host 'Launching original-author setup (dlssnr_on_amd_setup.exe) to create them…' -ForegroundColor Yellow
        if ($nv) { Write-Host "  nvngx_dlssnr found: $nv" } else {
            Write-Host '  NOTE: no nvngx_dlssnr.dll next to Setup.bat or in the game folder.' -ForegroundColor Yellow
            Write-Host '  The original-author setup will ask you to locate it if it needs one for weights.' -ForegroundColor Yellow
        }
        Write-Host '  In the author UI: pick the GAME folder if asked, finish install/close when done.'
        Push-Location $Root
        try {
            $p = Start-Process -FilePath $setup -WorkingDirectory $Root -Wait -PassThru
            Write-Host "  original-author setup exit code: {0}" -f $p.ExitCode
        } finally { Pop-Location }

        $weights = Find-FirstFile @(
            (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
            (Join-Path $game 'dlssnr_on_amd_weights.bin')
        )
        if (-not $srcA) {
            $srcA = Find-AuthorRuntime
        }
    }

    if (-not $srcA -or !(Test-Path -LiteralPath $srcA -PathType Leaf)) {
        Fail @"
Still missing a known DLSS-NR-on-AMD runtime (version.dll) after original-author setup.
Supported: 0.3.0 or 0.3.1.
1. Run dlssnr_on_amd_setup.exe yourself and finish its install
2. Put the version.dll it produces next to Setup.bat (or leave it in the game folder)
Download from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
    }

    $hashA = Get-Sha256 $srcA
    Write-Host ("Author runtime SHA256: {0}" -f $hashA)
    if ($expectedAuthor -notcontains $hashA) {
        $what = 'unknown build'
        if ($hashA -eq $knownA0217) { $what = 'this is 0.2.17, not 0.3.0/0.3.1' }
        Fail @"
$srcA is not a supported DLSS-NR-on-AMD runtime ($what).
  file:     $srcA
  got:      $hashA
  expected: $expectedA030 (0.3.0) or $expectedA031 (0.3.1)
Download 0.3.0 or 0.3.1 from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
    }

    try {
        $srcAFull = [IO.Path]::GetFullPath($srcA)
        $gameFull = [IO.Path]::GetFullPath($game)
        $rootFull = [IO.Path]::GetFullPath($Root)
        $gamePrefix = $gameFull.TrimEnd('\') + '\'
        $inGame = $srcAFull.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)
        $pkgIsGame = ($rootFull.TrimEnd('\') -ieq $gameFull.TrimEnd('\'))
        if ($inGame) {
            if ($pkgIsGame) {
                $stagedA = Join-Path $env:TEMP ('amd-presr-version-' + [guid]::NewGuid().ToString('N') + '.dll')
            } else {
                $stagedA = Join-Path $Root 'version.dll'
            }
            if ($srcAFull -ieq [IO.Path]::GetFullPath($stagedA)) {
                # Already the staged location.
            } else {
                Copy-Item -LiteralPath $srcAFull -Destination $stagedA -Force
                Write-Host "Copied author 0.3.0 runtime outside game folder: $stagedA"
            }
            $srcA = $stagedA
        }
    } catch {
        Fail "Could not stage author runtime from $srcA : $($_.Exception.Message)"
    }

    if (-not $weights) {
        $weights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
    }
    if (-not (Test-Path -LiteralPath $weights -PathType Leaf)) {
        if ((Test-Path -LiteralPath $setup) -and $nv) {
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
Write-Host "Original author runtime: $srcA  -> will be copied as dlssnr_amd_pass1/2/3.dll"
Write-Host 'NOTE: original-author version.dll is NOT left in the game folder (this package only installs OptiScaler as the proxy).'
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
            if ($expectedAuthor -contains $h -or $h -eq $knownA0217) { $isAuthorNative = $true }
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
            1 { Write-Host 'Cancelled.'; Pause-Exit 0 }
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
            1 { Write-Host 'Cancelled.'; Pause-Exit 0 }
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

# New-Item has no -LiteralPath on Windows PowerShell 5.1; paths may contain [].
# [IO.Directory]::CreateDirectory treats the string as a literal path.
[void][System.IO.Directory]::CreateDirectory($backup)
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
    try {
        if (Test-Path -LiteralPath $dest) {
            $save = Join-Path $backup $rel
            $sdir = Split-Path -Parent $save
            if ($sdir) { [void][System.IO.Directory]::CreateDirectory($sdir) }
            Copy-Item -LiteralPath $dest -Destination $save -Force
        }
        $ddir = Split-Path -Parent $dest
        if ($ddir) { [void][System.IO.Directory]::CreateDirectory($ddir) }
        Copy-Item -LiteralPath $src -Destination $dest -Force
    } catch [System.IO.IOException] {
        Fail @"
Install failed on $rel :
  $($_.Exception.Message)

The file is probably locked by a running game.
Close the game completely, then run Setup.bat again.
Partial files (if any) are under:
  $backup
"@
    }
}

function Set-IniSettings([string]$iniPath, [string]$sectionName, [System.Collections.IDictionary]$settings) {
    if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) { return }
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.AddRange([System.IO.File]::ReadAllLines($iniPath))
    $sectionIdx = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match ('^\s*\[' + [regex]::Escape($sectionName) + '\]\s*$')) {
            $sectionIdx = $i
            break
        }
    }
    if ($sectionIdx -lt 0) {
        $lines.Add("")
        $lines.Add("[$sectionName]")
        foreach ($k in $settings.Keys) {
            $lines.Add("$k = $($settings[$k])")
        }
    } else {
        $endIdx = $sectionIdx + 1
        while ($endIdx -lt $lines.Count -and $lines[$endIdx] -notmatch '^\s*\[') { ++$endIdx }
        foreach ($k in $settings.Keys) {
            $found = $false
            for ($i = $sectionIdx + 1; $i -lt $endIdx; ++$i) {
                if ($lines[$i] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) {
                    $lines[$i] = "$k = $($settings[$k])"
                    $found = $true
                    break
                }
            }
            if (-not $found) {
                $lines.Insert($endIdx, "$k = $($settings[$k])")
                ++$endIdx
            }
        }
    }
    [System.IO.File]::WriteAllLines($iniPath, $lines, [System.Text.UTF8Encoding]::new($true))
}

Write-Host ''
Write-Host "Installing OptiScaler as $Proxy ..." -ForegroundColor Cyan
Install-One (Join-Path $release 'OptiScaler.dll') $Proxy

$ini = Join-Path $release 'OptiScaler.ini'
$gameIni = Join-Path $game 'OptiScaler.ini'
if (Test-Path -LiteralPath $ini -PathType Leaf) {
    if (-not (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
        Install-One $ini 'OptiScaler.ini'
    }
}
$deps = Join-Path $release 'OptiScaler'
if (Test-Path -LiteralPath $deps) {
    Get-ChildItem -LiteralPath $deps -Recurse -File | ForEach-Object {
        $rel = Join-Path 'OptiScaler' $_.FullName.Substring($deps.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
}

# --- Install selected backends ---
if ($installDaniel) {
    Write-Host 'Installing danielblnc runtime (dlssnr_amd_pass1-3.dll) + weights.bin...' -ForegroundColor Cyan
    foreach ($p in 1..3) {
        Install-One $srcA ("dlssnr_amd_pass$p.dll")
    }
    Install-One $weights 'dlssnr_on_amd_weights.bin'
}

if ($installLmxxf) {
    Write-Host 'Installing lmxxf runtime + modules + shaders...' -ForegroundColor Cyan
    Install-One $lmxxfRuntime 'LmxxfNrRuntime.dll'
    Get-ChildItem -LiteralPath $lmxxfMods -Recurse -File | ForEach-Object {
        $rel = Join-Path 'lmxxf-modules' $_.FullName.Substring($lmxxfMods.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
    if ($lmxxfShaders) {
        Get-ChildItem -LiteralPath $lmxxfShaders -Recurse -File | ForEach-Object {
            $rel = Join-Path 'shaders' $_.FullName.Substring($lmxxfShaders.Length).TrimStart('\','/')
            Install-One $_.FullName $rel
        }
        Write-Host "  shaders installed from $lmxxfShaders"
    } else {
        Write-Host 'NOTE: lmxxf shaders not found in package; PrepareFrame may fail until shaders/ is beside OptiScaler.' -ForegroundColor DarkYellow
    }
    if ($lmxxfWeights) {
        $gameWeights = Join-Path $game 'native-game-tiled-assets'
        if ([IO.Path]::GetFullPath($lmxxfWeights) -ine [IO.Path]::GetFullPath($gameWeights)) {
            Write-Host "Installing native-game-tiled-assets from $lmxxfWeights..." -ForegroundColor Cyan
            Get-ChildItem -LiteralPath $lmxxfWeights -Recurse -File | ForEach-Object {
                $rel = Join-Path 'native-game-tiled-assets' $_.FullName.Substring($lmxxfWeights.Length).TrimStart('\','/')
                Install-One $_.FullName $rel
            }
        } else {
            Write-Host 'native-game-tiled-assets already present in game folder.' -ForegroundColor Green
        }
    } else {
        Write-Host 'NOTE: native-game-tiled-assets not found in package. If using lmxxf, place native-game-tiled-assets in the game folder.' -ForegroundColor DarkYellow
    }
}

# --- Configure OptiScaler.ini with chosen backend ---
if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
    Set-IniSettings $gameIni 'DlssNr' ([ordered]@{
        'Enabled' = 'true'
        'RunBeforeSR' = 'true'
        'NrBackend' = $activeBackend
    })
    Write-Host "Configured OptiScaler.ini: [DlssNr] Enabled=true, NrBackend=$activeBackend" -ForegroundColor Green
}

# Uninstaller is copied into the game folder. Double-click it there; it
# targets that directory (no folder picker). Look next to Setup first —
# $release may be a legacy release\ subfolder that does not contain it.
$ps1Src = $null
foreach ($candidate in @(
        (Join-Path $Root 'Uninstall_OptiScaler_NR.ps1'),
        (Join-Path $release 'Uninstall_OptiScaler_NR.ps1'),
        (Join-Path $PSScriptRoot 'uninstall-amd-presr.ps1')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $ps1Src = $candidate
        break
    }
}
if (-not $ps1Src) {
    Fail 'Missing Uninstall_OptiScaler_NR.ps1 next to Setup.ps1. Setup copies it into the game folder.'
}
Install-One $ps1Src 'Uninstall_OptiScaler_NR.ps1'

$batSrc = $null
foreach ($candidate in @(
        (Join-Path $Root 'Uninstall_OptiScaler_NR.bat'),
        (Join-Path $release 'Uninstall_OptiScaler_NR.bat')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $batSrc = $candidate
        break
    }
}
if ($batSrc) {
    Install-One $batSrc 'Uninstall_OptiScaler_NR.bat'
} else {
    $batDest = Join-Path $game 'Uninstall_OptiScaler_NR.bat'
    @'
@echo off
setlocal
title OptiScaler AMD pre-SR Uninstall
rem Double-click in the game folder. Optional: Uninstall_OptiScaler_NR.bat "D:\GameFolder"
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -GameDir "%~1" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Uninstall failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@ | Set-Content -LiteralPath $batDest -Encoding ASCII
    Write-Host "Wrote uninstaller: $batDest" -ForegroundColor Green
}

# Small install record so Uninstall knows which proxy name this install used.
# Uninstall still verifies OptiScaler by VersionInfo; this is a hint, not a trust root.
$installMark = Join-Path $game 'amd-presr-install.txt'
try {
    @(
        'project=OptiScaler AMD pre-SR',
        ('proxy=' + $Proxy),
        ('backend=' + $activeBackend),
        ('installed=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')),
        ('game=' + $game)
    ) -join "`r`n" | Set-Content -LiteralPath $installMark -Encoding ASCII
    Write-Host "Wrote install record: $installMark" -ForegroundColor Green
} catch {
    Write-Host "NOTE: could not write install record: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Keep reusable author files in the package folder for the next game.
# Never write them into the game folder — that would re-inject original A next to B.
# 摘要要报"包目录里真正留下的那份"，不能报 $srcA —— 当包目录就是游戏目录时
# $srcA 指向 TEMP 暂存文件，而下面会把它删掉；当包目录不是游戏目录时，
# 真正留下的是 $Root\version.dll。$keptA/$keptW 只记后者。
$keptA = $null
$keptW = $null
try {
    $rootFull = [IO.Path]::GetFullPath($Root)
    $gameFull = [IO.Path]::GetFullPath($game)
    $pkgIsGame = ($rootFull.TrimEnd('\') -ieq $gameFull.TrimEnd('\'))
    if (-not $pkgIsGame) {
        $pkgVersion = Join-Path $Root 'version.dll'
        if ($srcA -and (Test-Path -LiteralPath $srcA -PathType Leaf) -and -not (Test-Path -LiteralPath $pkgVersion -PathType Leaf)) {
            Copy-Item -LiteralPath $srcA -Destination $pkgVersion -Force
            Write-Host "Saved version.dll next to Setup.bat for the next install." -ForegroundColor Green
        }
        if (Test-Path -LiteralPath $pkgVersion -PathType Leaf) { $keptA = $pkgVersion }
        $pkgWeights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
        if ($weights -and (Test-Path -LiteralPath $weights -PathType Leaf) -and
            -not (Test-Path -LiteralPath $pkgWeights -PathType Leaf)) {
            Copy-Item -LiteralPath $weights -Destination $pkgWeights -Force
            Write-Host "Saved dlssnr_on_amd_weights.bin next to Setup.bat for the next install." -ForegroundColor Green
        }
        if (Test-Path -LiteralPath $pkgWeights -PathType Leaf) { $keptW = $pkgWeights }
    }
    if ($stagedA -and (Test-Path -LiteralPath $stagedA -PathType Leaf)) {
        $stagedFull = [IO.Path]::GetFullPath($stagedA)
        if ($stagedFull.StartsWith($env:TEMP, [StringComparison]::OrdinalIgnoreCase) -or
            $stagedFull -match 'amd-presr-version-') {
            try { Remove-Item -LiteralPath $stagedA -Force } catch { }
        }
    }
} catch {
    Write-Host "NOTE: could not copy author files into the package folder: $($_.Exception.Message)" -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  Game:           $game"
Write-Host "  Proxy:          $Proxy"
Write-Host "  Backup:         $backup"
Write-Host "  Active Backend: $activeBackend" -ForegroundColor Cyan
if ($installDaniel -and $installLmxxf) {
    Write-Host "  Installed:      Both backends (lmxxf + danielblnc) coexisting" -ForegroundColor Green
    Write-Host "  Tip:            To switch backend, edit OptiScaler.ini ([DlssNr] NrBackend=lmxxf or daniel) or re-run Setup.bat." -ForegroundColor Yellow
} elseif ($installLmxxf) {
    Write-Host "  Installed:      OptiScaler + lmxxf runtime (LmxxfNrRuntime.dll, modules, shaders)" -ForegroundColor Green
} else {
    Write-Host "  Installed:      OptiScaler + danielblnc runtime (dlssnr_amd_pass1-3.dll + weights)" -ForegroundColor Green
}
if ($keptA -or $keptW) {
    Write-Host "  Package keeps:  $keptA"
    if ($keptW) { Write-Host "                  $keptW" }
} else {
    Write-Host '  Package keeps:  (nothing — the package folder is the game folder)'
}
Write-Host ''
Write-Host 'Next (in game):' -ForegroundColor Yellow
Write-Host '  1. Launch the game'
Write-Host '  2. Press Insert (Ins) to open the OptiScaler menu'
Write-Host '  3. Ensure DLSSNR is enabled'
if ($installDaniel -and $installLmxxf) {
    Write-Host "  4. Switch backends anytime in OptiScaler.ini ([DlssNr] NrBackend=$activeBackend) or by re-running Setup.bat"
}
Write-Host ''
Write-Host 'Install SUCCEEDED.' -ForegroundColor Green
Pause-Exit 0
