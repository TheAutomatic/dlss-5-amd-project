<#
.SYNOPSIS
  Install this project's OptiScaler into a game folder.
  Double-click Setup.bat (no args) to pick the game folder, or pass -GameDir.
  Copies danielblnc's 0.3.0–0.6.0 runtime (version.dll) to dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs this project. Does not leave danielblnc's version.dll in the game.

  Put these in the SAME folder as Setup.ps1 (the package root):
    OptiScaler.dll              this fork
    OptiScaler.ini              optional
    OptiScaler\                 FFX / XeSS / Agility deps
    version.dll                 danielblnc AMD NR 0.3.0–0.6.0 (copied to pass1-3)
    nvngx_dlssnr.dll            optional, to generate weights with danielblnc setup
    dlssnr_on_amd_setup.exe     optional, danielblnc 0.3.0–0.6.0 setup
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
    [ValidateSet('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll','version.dll')]
    [string]$Proxy = 'dxgi.dll',
    [string]$Root,
    [string]$AuthorDll,
    [ValidateSet("auto", "daniel", "lmxxf", "mochizuki", "all")]
    [string]$Backend = "auto",
    [ValidateSet('both', 'all', 'small', 'experimental', 'original', 'int4')]
    [string]$MochizukiModels,
    [string[]]$MochizukiNetworks,
    [ValidateSet('small', 'experimental', 'original', 'int4')]
    [string]$MochizukiActiveModel,
    [ValidateSet('both', 'small', 'original')]
    [string]$LmxxfModels,
    [string[]]$Backends,
    [string]$ActiveBackend,
    [switch]$NonInteractive,
    # Explicit opt-in for unattended clean reinstall. Otherwise unattended setup overwrites.
    [switch]$UninstallExisting,
    # Setup.bat owns the final pause; keep all folder/proxy/confirmation prompts.
    [switch]$NoPause,
    # Absolute JSON file from the GUI worker. Missing keys still require a prompt
    # (or fail under -NonInteractive). Never treated as blanket consent.
    [string]$DecisionJson
)
$ErrorActionPreference = 'Stop'
# Match redirected child output and the launcher reader, independent of the OS code page.
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$lmxxfStage = $null
$setupSourceStage = $null
$lmxxfSmallStage = $null
$mochizukiModelStage = $null

function Pause-Exit([int]$code) {
    if ($lmxxfSmallStage) {
        try { Remove-LmxxfTemporaryTree $lmxxfSmallStage $Root } catch { Write-Warning $_.Exception.Message }
    }
    if ($mochizukiModelStage -and (Test-Path -LiteralPath $mochizukiModelStage)) {
        try { Remove-Item -LiteralPath $mochizukiModelStage -Recurse -Force -ErrorAction Stop }
        catch { Write-Warning "Mochizuki model staging cleanup failed: $($_.Exception.Message)" }
    }
    if ($lmxxfStage) {
        try { Remove-LmxxfTemporaryTree $lmxxfStage ([IO.Path]::GetDirectoryName($lmxxfStage)) }
        catch { Write-Warning "Module staging cleanup failed: $($_.Exception.Message)" }
    }
    if ($setupSourceStage) {
        try {
            $full = Assert-LmxxfUnlinkedPath $setupSourceStage
            if ([IO.Path]::GetDirectoryName($full) -ine (Assert-LmxxfUnlinkedPath $game) -or
                [IO.Path]::GetFileName($full) -notmatch '^\.amd-presr-source-[0-9a-f]{32}$') {
                throw "Unexpected installer source staging path: $full"
            }
            $null = @(Get-LmxxfUnlinkedFiles $full)
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
        } catch { Write-Warning "Installer source cleanup failed: $($_.Exception.Message)" }
    }
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
# an invalid danielblnc setup executable). The batch also catches parser/parameter
# binding failures, which happen before this script body can run.
trap { Fail ("Unexpected install error: " + $_.Exception.Message) }

$launcherDecisions = $null
if ($DecisionJson) {
    if (-not [IO.Path]::IsPathRooted($DecisionJson)) { Fail 'DecisionJson must be an absolute path.' }
    $launcherDecisions = Get-Content -LiteralPath $DecisionJson -Raw -Encoding UTF8 | ConvertFrom-Json
}
$hasBackendSelection = $PSBoundParameters.ContainsKey('Backends')
$lmxxfModelsExplicit = $PSBoundParameters.ContainsKey('LmxxfModels')
if ($launcherDecisions) {
    if (-not $hasBackendSelection -and $launcherDecisions.PSObject.Properties['backends']) {
        $Backends = @($launcherDecisions.backends | ForEach-Object { [string]$_ })
        $hasBackendSelection = $true
    }
    if (-not $PSBoundParameters.ContainsKey('MochizukiNetworks') -and
        $launcherDecisions.PSObject.Properties['mochizukiNetworks'] -and $launcherDecisions.mochizukiNetworks) {
        $MochizukiNetworks = @($launcherDecisions.mochizukiNetworks | ForEach-Object { [string]$_ })
    }
    if (-not $PSBoundParameters.ContainsKey('MochizukiActiveModel') -and
        $launcherDecisions.PSObject.Properties['mochizukiActiveModel'] -and $launcherDecisions.mochizukiActiveModel) {
        $MochizukiActiveModel = [string]$launcherDecisions.mochizukiActiveModel
        if ($MochizukiActiveModel -notin @('small','experimental','original','int4')) {
            Fail 'DecisionJson mochizukiActiveModel must be small, experimental, original, or int4.'
        }
    }
    if (-not $PSBoundParameters.ContainsKey('MochizukiModels') -and $launcherDecisions.mochizukiModels) {
        $MochizukiModels = [string]$launcherDecisions.mochizukiModels
        if ($MochizukiModels -notin @('both','all','small','experimental','original','int4')) {
            Fail 'DecisionJson mochizukiModels must be both, all, small, experimental, original, or int4.'
        }
    }
    if (-not $lmxxfModelsExplicit -and $launcherDecisions.lmxxfModels) {
        $LmxxfModels = [string]$launcherDecisions.lmxxfModels
        if ($LmxxfModels -notin @('both','small','original')) { Fail 'DecisionJson lmxxfModels must be both, small, or original.' }
        $lmxxfModelsExplicit = $true
    }
}

# Keep the requested parameter separate from the resolved installation state:
# PowerShell variable names are case-insensitive, including parameter names.
if ($PSBoundParameters.ContainsKey('ActiveBackend')) {
    if (-not $hasBackendSelection) { Fail 'ActiveBackend requires an explicit -Backends list (or DecisionJson backends).' }
    if ($ActiveBackend -notin @('lmxxf','daniel','mochizuki')) { Fail 'ActiveBackend must be lmxxf, daniel, or mochizuki.' }
}

. (Join-Path $PSScriptRoot 'lmxxf-module-package.ps1')

# $Root must not be a param default of $PSScriptRoot: when invoked via powershell -File,
# $PSScriptRoot is not yet assigned during parameter binding (assigned inside script body).
# Setup.bat uses -File.
if (-not $Root) { $Root = $PSScriptRoot }

# Package layout: everything is in the package root (matching upstream OptiScaler packages).
# Early packages placed DLLs under release\, check there as a fallback.
$release = $Root
if (!(Test-Path -LiteralPath (Join-Path $release 'OptiScaler.dll')) -and
    (Test-Path -LiteralPath (Join-Path $Root 'release\OptiScaler.dll'))) {
    $release = Join-Path $Root 'release'
}

$productTitle = 'OptScaler(NR)'
$versionFile = Join-Path $release 'VERSION'
if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
    $productVersion = ([IO.File]::ReadAllText($versionFile)).Trim()
    # This title is also embedded into the fallback batch launcher.
    if ($productVersion -match '^[0-9]+(?:\.[0-9]+){2,3}(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?$') {
        $productTitle += ' ' + $productVersion
    }
}
Write-Host $productTitle -ForegroundColor Cyan
Write-Host '==============================================================================' -ForegroundColor Yellow
if (-not $launcherDecisions -or $launcherDecisions.language -ne 'en') {
    Write-Host '  本软件免费，如付费购买请立即退款！' -ForegroundColor Yellow
}
if (-not $launcherDecisions -or $launcherDecisions.language -ne 'zh') {
    Write-Host '  This software is free. If you paid for it, request a refund immediately!' -ForegroundColor Yellow
}
Write-Host '==============================================================================' -ForegroundColor Yellow

# Do not use Get-FileHash: it belongs to Microsoft.PowerShell.Utility and relies on module autoloading.
# When PSModulePath points to PowerShell 7 module directories (e.g. launched from pwsh
# or in CI within shell: pwsh invoking powershell -File), the 5.1 child process cannot load it
# and throws CommandNotFoundException. Setup.bat via cmd takes this exact path.
# Compute SHA256 using .NET directly without module dependencies.
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

# PE-image SHA256 (headers + section raw bytes). 0.5.0+ may append a default ini
# after the PE; game-folder copies drop it, so whole-file hash forks. Pre-0.5.0
# has no overlay and the PE hash equals the whole-file hash.
function Get-PeSha256([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $end = $bytes.Length
    try {
        if ($bytes.Length -ge 0x40 -and $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A) {
            $e = [int64][BitConverter]::ToUInt32($bytes, 0x3C)
            if ($e -gt 0 -and ($e + 24) -lt [int64]$bytes.Length -and $bytes[$e] -eq 0x50 -and $bytes[$e+1] -eq 0x45) {
                $nsec = [BitConverter]::ToUInt16($bytes, $e + 6)
                $optsz = [BitConverter]::ToUInt16($bytes, $e + 20)
                $sec = $e + 24 + $optsz
                $last = [int64]0
                for ($i = 0; $i -lt $nsec; $i++) {
                    $o = $sec + $i * 40
                    if (($o + 24) -gt [int64]$bytes.Length) { break }
                    $rsz = [BitConverter]::ToUInt32($bytes, $o + 16)
                    $raw = [BitConverter]::ToUInt32($bytes, $o + 20)
                    $off = [int64]$raw + [int64]$rsz
                    if ($off -gt $last -and $off -le [int64]$bytes.Length) { $last = $off }
                }
                if ($last -gt 0) { $end = [int]$last }
            }
        }
    } catch { }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes, 0, $end))).Replace('-', '')
    } finally { $sha.Dispose() }
}

function Test-OptiProxy([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        if ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler') { return $true }
    } catch { }
    try {
        $pkgOpti = Join-Path $release 'OptiScaler.dll'
        if (Test-Path -LiteralPath $pkgOpti -PathType Leaf) {
            if ((Get-Sha256 $path) -eq (Get-Sha256 $pkgOpti)) { return $true }
        }
    } catch { }
    return $false
}

function Test-ValidPePlugin([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $fs = [IO.File]::OpenRead($path)
        try {
            if ($fs.Length -lt 2048) { return $false }
            $buf = New-Object byte[] 64
            $read = $fs.Read($buf, 0, 64)
            if ($read -lt 64 -or $buf[0] -ne 0x4D -or $buf[1] -ne 0x5A) { return $false }
            $peOffset = [BitConverter]::ToInt32($buf, 0x3C)
            if ($peOffset -le 0 -or $peOffset -ge ($fs.Length - 4)) { return $false }
            [void]$fs.Seek($peOffset, [IO.SeekOrigin]::Begin)
            $peSig = New-Object byte[] 4
            $readSig = $fs.Read($peSig, 0, 4)
            if ($readSig -lt 4) { return $false }
            return ($peSig[0] -eq 0x50 -and $peSig[1] -eq 0x45 -and $peSig[2] -eq 0 -and $peSig[3] -eq 0)
        } finally {
            $fs.Dispose()
        }
    } catch {
        return $false
    }
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

function Ask-UninstallExisting {
    Write-Host ''
    Write-Host "Existing OptiScaler installation detected in: $game" -ForegroundColor Yellow
    Write-Host 'Recommended: uninstall before installing this version to avoid conflicts with old files and settings.' -ForegroundColor Yellow
    Write-Host '  Y = uninstall automatically, then install (Recommended; resets OptiScaler settings)'
    Write-Host '  N = continue with an overwrite installation'
    Write-Host 'Weights and existing backup folders will be kept.'
    while ($true) {
        $answer = (Read-Host 'Uninstall before installing? [Y/N]').Trim()
        if ($answer -match '^(?i)y(es)?$') { return $true }
        if ($answer -match '^(?i)n(o)?$') { return $false }
        Write-Host 'Please type Y or N (not case sensitive).'
    }
}

# An install can reuse runtime/shader files from the game, or even run from a
# package extracted there. Preserve just the sources we need before uninstalling;
# never recursively copy the entire game folder.
function Save-ReinstallSource([string]$Path) {
    if (-not $Path) { return $null }
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $game.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { return $Path }
    $null = Assert-LmxxfUnlinkedPath $full
    if (-not $script:setupSourceStage) {
        $script:setupSourceStage = Join-Path $game ('.amd-presr-source-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($script:setupSourceStage)
    }
    $dest = Join-Path $script:setupSourceStage $full.Substring($prefix.Length)
    if (Test-Path -LiteralPath $full -PathType Container) {
        $null = @(Get-LmxxfUnlinkedFiles $full)
        [void][IO.Directory]::CreateDirectory($dest)
        Get-ChildItem -LiteralPath $full -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $dest -Recurse -Force -ErrorAction Stop
        }
    } else {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))
        Copy-Item -LiteralPath $full -Destination $dest -Force -ErrorAction Stop
    }
    return $dest
}

function Get-LastGameDir {
    try {
        $p = Get-ItemProperty -Path 'HKCU:\Software\OptiScaler-AMD-PreSR' -Name LastGameDir -ErrorAction Stop
        $v = [string]$p.LastGameDir
        if ($v -and (Test-Path -LiteralPath $v -PathType Container)) { return $v }
    } catch { }
    return ''
}

function Save-LastGameDir([string]$Path) {
    try {
        if ([string]::IsNullOrWhiteSpace($Path)) { return }
        if (!(Test-Path -LiteralPath 'HKCU:\Software\OptiScaler-AMD-PreSR')) {
            New-Item -Path 'HKCU:\Software\OptiScaler-AMD-PreSR' -Force | Out-Null
        }
        Set-ItemProperty -Path 'HKCU:\Software\OptiScaler-AMD-PreSR' -Name LastGameDir -Value $Path
    } catch { }
}

function Ask-GameFolder {
    # IFileDialog + FOS_PICKFOLDERS: address bar works. A parentless console
    # dialog often shows an empty nav pane unless it has an owner window.
    $initial = Get-LastGameDir
    try {
        Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public static class AmdFolderPick {
    // Scope the change to this UI thread; leave the PowerShell host's process DPI alone.
    sealed class DpiScope : IDisposable {
        [DllImport("user32.dll", SetLastError = true)]
        static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
        IntPtr previous;
        public DpiScope() {
            previous = SetThreadDpiAwarenessContext(new IntPtr(-4)); // Per-monitor v2.
            if (previous == IntPtr.Zero)
                previous = SetThreadDpiAwarenessContext(new IntPtr(-3)); // Per-monitor.
        }
        public void Dispose() {
            if (previous != IntPtr.Zero) {
                SetThreadDpiAwarenessContext(previous);
                previous = IntPtr.Zero;
            }
        }
    }
    public static IDisposable BeginDpiScope() { return new DpiScope(); }
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
    public static string PickFolder(string title, string initialPath) {
        using (BeginDpiScope()) { return PickFolderCore(title, initialPath); }
    }
    static string PickFolderCore(string title, string initialPath) {
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
        IntPtr start = IntPtr.Zero;
        if (!string.IsNullOrEmpty(initialPath) && System.IO.Directory.Exists(initialPath)) {
            start = ItemFromParsingName(initialPath);
        }
        if (start == IntPtr.Zero) start = ItemFromParsingName(ThisPcParsingName);
        if (start != IntPtr.Zero) {
            try { dlg.SetDefaultFolder(start); } catch { }
            try { dlg.SetFolder(start); } catch { }
            Marshal.Release(start);
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
        $picked = [AmdFolderPick]::PickFolder('Select the game folder that contains the game .exe', $initial)
        if ($picked) { return $picked }
        return $null
    } catch {
        Add-Type -AssemblyName System.Windows.Forms
        $dpiScope = $null
        $dlg = $null
        try {
            if ('AmdFolderPick' -as [type]) { $dpiScope = [AmdFolderPick]::BeginDpiScope() }
            $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
            $dlg.Description = 'Select the game folder that contains the game .exe'
            $dlg.ShowNewFolderButton = $false
            if ($initial) { $dlg.SelectedPath = $initial }
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
            return $null
        } finally {
            if ($dlg) { $dlg.Dispose() }
            if ($dpiScope) { $dpiScope.Dispose() }
        }
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
    Save-LastGameDir $GameDir
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
# Resolve-Path keeps 8.3 names such as RUNNER~1. GetFullPath expands them.
$game = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $GameDir).Path)
Save-LastGameDir $game
$proxies = @(
    'dxgi.dll','winmm.dll','d3d12.dll','version.dll',
    'winhttp.dll','wininet.dll','dbghelp.dll','dinput8.dll'
)

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

# Legacy standalone NR core is not used by our integrated runtimes. Only this
# exact DLL and DLSS Enabler's disposable logs are retired; keep its DLL/INI.
# Collect and check locks before changing the installation, including store logs.
$legacyCleanupFiles = @()
foreach ($legacyRoot in @($game, (Join-Path $game '_storage_'))) {
    if (-not (Test-Path -LiteralPath $legacyRoot -PathType Container)) { continue }
    try { $null = Assert-LmxxfUnlinkedPath $legacyRoot }
    catch { Write-Warning "Legacy cleanup skipped linked directory: $legacyRoot"; continue }
    foreach ($file in (Get-ChildItem -LiteralPath $legacyRoot -File -Force)) {
        if ($file.Name -ine 'dlssnr_core.dll' -and
            $file.Name -notmatch '^dlss-enabler(\.[0-9]+)?\.log(\.[0-9]+)?$') { continue }
        try { $legacyCleanupFiles += Assert-LmxxfUnlinkedPath $file.FullName }
        catch { Write-Warning "Legacy cleanup skipped linked file: $($file.FullName)" }
    }
}

$locked = @()
foreach ($name in ($proxies + @('LmxxfNrRuntime.dll', 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll', 'dlssnr_amd_pass3.dll', 'dlssnr_on_amd_weights.bin'))) {
    $p = Join-Path $game $name
    if (Test-FileLocked $p) { $locked += $name }
}
foreach ($path in $legacyCleanupFiles) {
    if (Test-FileLocked $path) { $locked += $path }
}
if ($locked.Count -gt 0) {
    Fail @"
The game is still running (file lock detected):
  $($locked -join ', ')

Close the game completely, then run Setup.bat again.
"@
}
# Also flag a live process whose image lives in the game folder.
# Our own running binaries are not the game: users who extract the whole package into the game
# folder would otherwise self-lock (the launcher itself launched this installer).
# Crash reporters are excluded: Crashpad/Unreal/Unity helpers and WER routinely outlive a crashed
# game (or belong to unrelated apps that ship the same helper name), and they never hold the DLLs
# we replace. Real file locks are already caught by the check above.
$crashHelperNames = '^(crashpad_handler|unitycrashhandler(32|64)?|crashreportclient(editor)?|werfault|werfaultsecure)$'
$gameExes = @(Get-ChildItem -LiteralPath $game -Filter '*.exe' -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -notin @('OptScalerNR.Launcher.exe', 'dlssnr_on_amd_setup.exe', 'person-worker.exe') -and
        $_.BaseName -notmatch $crashHelperNames
    } |
    Select-Object -ExpandProperty Name)
if ($gameExes.Count -gt 0) {
    $gamePrefix = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $n = "$($_.ProcessName).exe"
        if ($gameExes -notcontains $n) { return $false }
        # A same-named process elsewhere on the machine (another app's helper) is not the game.
        $image = $null
        try { $image = $_.Path } catch { }
        if ([string]::IsNullOrEmpty($image)) { return $true }
        return $image.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)
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

# Ask once, immediately after selecting and checking the game folder. The actual
# uninstall waits until the new package and any reusable sources are ready.
$existingOpti = @($proxies | Where-Object { Test-OptiProxy (Join-Path $game $_) }).Count -gt 0
$existingRecord = Join-Path $game 'amd-presr-install.txt'
if (-not $existingOpti -and (Test-Path -LiteralPath $existingRecord -PathType Leaf)) {
    $existingOpti = [IO.File]::ReadAllText($existingRecord) -match '(?m)^project=(?:OptScaler\(NR\)|OptiScaler AMD pre-SR)\r?$'
}
$uninstallFirst = $false
if ($existingOpti) {
    if ($UninstallExisting) { $uninstallFirst = $true }
    elseif ($launcherDecisions -and $launcherDecisions.existingInstall) {
        if ($launcherDecisions.existingInstall -eq 'uninstall') { $uninstallFirst = $true }
        elseif ($launcherDecisions.existingInstall -eq 'overwrite') { $uninstallFirst = $false }
        else { Fail 'DecisionJson existingInstall must be uninstall or overwrite.' }
    }
    elseif (-not $NonInteractive) { $uninstallFirst = Ask-UninstallExisting }
    if (-not $uninstallFirst) { Write-Host 'Continuing with an overwrite installation.' -ForegroundColor Cyan }
}

# Interactive proxy pick (like older 1.7.x installers). dinput8 is invalid for this build.
if (-not $NonInteractive -and -not $PSBoundParameters.ContainsKey('Proxy')) {
    $proxyOptions = @(
        'dxgi.dll (default; if the game fails to start, try winmm.dll)',
        'winmm.dll (recommended by some games)',
        'd3d12.dll',
        'winhttp.dll',
        'wininet.dll',
        'dbghelp.dll',
        'version.dll'
    )
    $pi = Ask-Choice 'Which proxy DLL should OptiScaler install as?' $proxyOptions
    $Proxy = ($proxyOptions[$pi - 1] -split '\s+')[0]
    Write-Host "Selected proxy: $Proxy"
}

function Confirm-Continue([string]$title) {
    if ($launcherDecisions) {
        if ([string]$launcherDecisions.shortFileContinue -eq 'continue') { return }
        Fail $title
    }
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

# Always use the new package's uninstaller, and check it before changing the game.
$ps1Src = Find-FirstFile @(
    (Join-Path $release 'Uninstall_OptiScaler_NR.ps1'),
    (Join-Path $Root 'Uninstall_OptiScaler_NR.ps1'),
    (Join-Path $PSScriptRoot 'uninstall-amd-presr.ps1')
)
if (-not $ps1Src) { Fail 'Missing Uninstall_OptiScaler_NR.ps1 next to Setup.ps1.' }
$batSrc = Find-FirstFile @(
    (Join-Path $release 'Uninstall_OptiScaler_NR.bat'),
    (Join-Path $Root 'Uninstall_OptiScaler_NR.bat')
)
$moduleHelperSrc = Join-Path $PSScriptRoot 'lmxxf-module-package.ps1'

# Hash: only known danielblnc runtimes are supported. The RVA layout is pinned to
# each binary — a different build will not run correctly. Fail closed.
# Identity is the PE image (Get-PeSha256); 0.5.0+ may append a default ini overlay.
# 0.3.0 = kAmd03; ... 0.4.3 = kAmd043; 0.5.0 = kAmd050; 0.5.1 = kAmd051.
$expectedA030 = '8321CAE728D28CB7632D0D58D3D913E91132BF7645C126505698FBE4CD5A0138'
$expectedA031 = 'B108D6407EB7F094A4F9111EDD778EEE7B978B648D413A9FC7AEEDFDD914C154'
$expectedA032 = 'B92F7481BC03FA41F443B1E1E54B502789DF2BBBCEFE48C680DF7BB02A33FC1E'
$expectedA033 = '907B30A61644A6D7E43E58A43A9D97A04A24B1A764A88BDEF3954AC807E8D112'
$expectedA040 = 'D62BE3D8B9FBB3C6C81982C4DDB3DFA00EB9662E3206925CBE5B7E1BC6798B80'
$expectedA041 = '823063EB4C76B1334FD1800C41798873AE61D4016AF0406F1F0B9DCE57B1D376'
$expectedA042 = '8AA2DCC5B6596ACA97995DBFD4E0A9790D8C15108495E0ED154DD15DBB5B465A'
$expectedA043 = 'D1E320862A8763AC39E7CE194536D4B6C55BA61BAE9E8A92753CEC32DF67A457'
$expectedA050 = 'CDDFB09E019347957BF7B96C95C0E900E8D3062DFAED697A8A96B0A039AEC31A'
$expectedA051 = '493B4A3B80A21F7255109172AB7BB01BA08D35F2941718F441768F1ABFC48ACD'
$expectedA060 = '195C4A891B6EAC4C1CB7671E10FF62BBBE2B17F1DFAE1344DC5A6714E4775721'
$knownA0217  = 'BC97F3B06718E19042ACAF227BFE15D1E43D4977F9DC2E39994FCC511445FF4E'
$expectedAuthor = @($expectedA030, $expectedA031, $expectedA032, $expectedA033, $expectedA040, $expectedA041, $expectedA042, $expectedA043, $expectedA050, $expectedA051, $expectedA060)

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
            $h = Get-PeSha256 $c
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
        (Join-Path $Root 'exports\lmxxf-runtime\LmxxfNrRuntime.dll')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $lmxxfRuntime = $candidate; break }
}
$lmxxfMods = $null
foreach ($candidate in @(
        (Join-Path $release 'lmxxf-modules'),
        (Join-Path $Root 'lmxxf-modules'),
        (Join-Path $Root 'third_party\lmxxf\modules')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        $lmxxfMods = $candidate
        break
    }
}
$lmxxfShaders = $null
foreach ($candidate in @(
        (Join-Path $release 'shaders'),
        (Join-Path $Root 'shaders'),
        (Join-Path $Root 'third_party\lmxxf\shaders')
    )) {
    if ((Test-Path -LiteralPath $candidate -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'native_codec_encode.hlsl') -PathType Leaf)) {
        $lmxxfShaders = $candidate
        break
    }
}
function Test-LmxxfWeights([string]$Directory) {
    if (-not (Get-Command Test-LmxxfModel -ErrorAction SilentlyContinue)) { return $false }
    return Test-LmxxfModel $Directory original -Quick
}
$lmxxfModelHelper = Find-FirstFile @((Join-Path $Root 'lmxxf-small.ps1'), (Join-Path $PSScriptRoot 'install/lmxxf-small.ps1'))
if ($lmxxfModelHelper) { . $lmxxfModelHelper }
$lmxxfWeights = $null
$lmxxfSmallWeights = $null
$lmxxfCandidates = @(
        (Join-Path $release 'native-game-tiled-assets'),
        (Join-Path $Root 'native-game-tiled-assets'),
        (Join-Path $Root 'DLSS5-AMD\native-game-tiled-assets'),
        (Join-Path $game 'native-game-tiled-assets')
    )
foreach ($candidate in $lmxxfCandidates) {
    if (-not $lmxxfWeights -and (Test-LmxxfWeights $candidate)) { $lmxxfWeights = $candidate }
    if ($lmxxfModelHelper -and -not $lmxxfSmallWeights -and (Test-LmxxfModel (Join-Path $candidate 'small') small)) { $lmxxfSmallWeights = Join-Path $candidate 'small' }
}

# --- detect danielblnc components ---
$setup   = Join-Path $Root 'dlssnr_on_amd_setup.exe'
$modelSourceCandidates = @(
    (Join-Path $Root 'nvngx_dlssnr.dll'),
    (Join-Path $game 'nvngx_dlssnr.dll')
)
if ($launcherDecisions -and $launcherDecisions.modelSourceDll) {
    $chosenModelSource = [string]$launcherDecisions.modelSourceDll
    if (-not [IO.Path]::IsPathRooted($chosenModelSource) -or -not (Test-Path -LiteralPath $chosenModelSource -PathType Leaf)) { Fail 'Selected model source DLL no longer exists. Select nvngx_dlssnr.dll again.' }
    $chosenModelSource = Assert-LmxxfUnlinkedPath $chosenModelSource
    $modelSourceCandidates = @($chosenModelSource) + $modelSourceCandidates
}
$nv = Find-FirstFile $modelSourceCandidates
$weights = Find-FirstFile @(
    (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
    (Join-Path $game 'dlssnr_on_amd_weights.bin')
)
$srcA = Find-AuthorRuntime

# Neural Rendering is the official dlssnr-amd.asi, not the retired built-in
# MochizukiNrRuntime.dll. Installation checks names and layout only.
$mochizukiAsi = Find-FirstFile @(
    (Join-Path $release 'OptiScaler/plugins/dlssnr-amd.asi'),
    (Join-Path $Root 'OptiScaler/plugins/dlssnr-amd.asi'),
    (Join-Path $release 'dlssnr-amd.asi'),
    (Join-Path $Root 'dlssnr-amd.asi')
)
$mochizukiAssetsRoot = $null
foreach ($candidate in @($release, $Root)) {
    if (-not $mochizukiAssetsRoot -and
        (Test-Path -LiteralPath (Join-Path $candidate 'dlssnr-amd/shaders') -PathType Container)) {
        $mochizukiAssetsRoot = $candidate
    }
}
$mochizukiAssets = if ($mochizukiAssetsRoot) { Join-Path $mochizukiAssetsRoot 'dlssnr-amd' } else { $null }
$mochizukiTools = Find-FirstFile @(
    (Join-Path $Root 'model-tools/dlssnr_extract_model.exe'),
    (Join-Path $release 'model-tools/dlssnr_extract_model.exe')
)
$mochizukiInt4Tool = Find-FirstFile @(
    (Join-Path $Root 'model-tools/dlssnr-int4-weights.exe'),
    (Join-Path $release 'model-tools/dlssnr-int4-weights.exe')
)
$mochizukiProbe = $null
if ($mochizukiAssets) { $mochizukiProbe = Find-FirstFile @((Join-Path $mochizukiAssets 'nr_present_probe.exe')) }
if (-not $mochizukiProbe) { $mochizukiProbe = Find-FirstFile @((Join-Path $release 'nr_present_probe.exe'), (Join-Path $Root 'nr_present_probe.exe')) }
$canMochizuki = [bool]($mochizukiAsi -and $mochizukiAssets -and $mochizukiTools)
if ($canMochizuki) {
    foreach ($required in @('dlssnr-amd/shaders', 'dlssnr-amd/aco')) {
        if (-not (Test-Path -LiteralPath (Join-Path $release $required) -PathType Container) -and
            -not (Test-Path -LiteralPath (Join-Path $Root $required) -PathType Container)) {
            Fail "Incomplete Mochizuki ASI package: missing $required. Re-extract the complete update."
        }
    }
    if ($mochizukiAssets -and -not (Test-Path -LiteralPath (Join-Path $mochizukiAssets 'aco') -PathType Container)) {
        # The root probe above finds shaders; ACO must live in the same tree.
        Fail 'Incomplete Mochizuki ASI package: dlssnr-amd/aco missing beside the resource tree.'
    }
}
$installMochizuki = $false
$canLmxxf  = [bool]($lmxxfRuntime -and $lmxxfMods -and $lmxxfShaders)
# Code and shaders must come from this update, even when Y will uninstall first.
# Model reuse from the game is separate and remains supported.
if ($Backend -in @('auto', 'all', 'lmxxf') -and -not $canLmxxf -and
    ($lmxxfRuntime -or $lmxxfMods -or $lmxxfShaders)) {
    Fail 'Incomplete lmxxf package: runtime, modules and shaders must be supplied together. Re-extract the complete update; installed game components are not used to fill missing update files.'
}
$canDaniel = [bool]($srcA -or (Test-Path -LiteralPath $setup -PathType Leaf) -or $weights)

$installLmxxf  = $false
$installDaniel = $false
$selectedActiveBackend = 'lmxxf'

$knownOriginalSha = '2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd'
$knownSmallSha    = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33'

function Test-MochizukiModelFile([string]$Path, [uint32]$Entries = 599) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $flen = $stream.Length
        if ($flen -le 16) { return $false }
        $header = New-Object byte[] 16
        if ($stream.Read($header, 0, 16) -ne 16) { return $false }
        $magic = [Text.Encoding]::ASCII.GetString($header, 0, 8)
        $count = [BitConverter]::ToUInt32($header, 8)
        $reserved = [BitConverter]::ToUInt32($header, 12)
        if ($magic -cne 'NRMODEL1' -or $count -ne $Entries -or $reserved -ne 0) { return $false }

        # Check known fast SHA
        $stream.Position = 0
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $hashStr = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        } finally { $sha.Dispose() }

        if ($Entries -eq 599 -and $hashStr -eq $script:knownOriginalSha) { return $true }
        if ($Entries -eq 255 -and $hashStr -eq $script:knownSmallSha) { return $true }

        $stream.Dispose(); $stream = $null
        if ($flen -ge 65536 -and (Get-Command Ensure-NativeModelExtractor -ErrorAction SilentlyContinue)) {
            Ensure-NativeModelExtractor
            $fam = if ($Entries -eq 255) { 'small' } else { 'original' }
            if ([OptScaler.NativeModelExtractor]::VerifyMochizukiPack($Path, $fam)) { return $true }
        }

        # No Python fallback: the embedded native verifier above is the single
        # validation path for lmxxf model packs. Missing it means not verified.
        return $false
    } catch {
        return $false
    } finally {
        if ($stream) { $stream.Dispose() }
    }
}

function Test-MochizukiModelHeader([string]$Path, [uint32]$Entries = 599) {
    return Test-MochizukiModelFile $Path $Entries
}
$mochizukiModel = $null
$modelCandidates = @((Join-Path $Root 'dlssnr-amd/dlssnr.bin'), (Join-Path $Root 'dlssnr.bin'))
if ($mochizukiAssets) { $modelCandidates += (Join-Path $mochizukiAssets 'dlssnr.bin') }
$modelCandidates += (Join-Path $game 'dlssnr-amd/dlssnr.bin')
foreach ($candidate in $modelCandidates) {
    # Presence only: the ASI owns the model format.
    if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Item -LiteralPath $candidate).Length -gt 0) {
        $mochizukiModel = $candidate; break
    }
}

$mochizukiSmallModel = $null
$smallModelCandidates = @((Join-Path $Root 'dlssnr-amd/dlssnr-small.bin'), (Join-Path $Root 'dlssnr-small.bin'))
if ($mochizukiAssets) { $smallModelCandidates += (Join-Path $mochizukiAssets 'dlssnr-small.bin') }
$smallModelCandidates += (Join-Path $game 'dlssnr-amd/dlssnr-small.bin')
foreach ($candidate in $smallModelCandidates) {
    if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Item -LiteralPath $candidate).Length -gt 0) {
        $mochizukiSmallModel = $candidate; break
    }
}
$mochizukiNewNvSha = '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f'
$mochizukiNvSha = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
# --- Weight-check helpers; preparation runs only after backend selection ---
# Only checks that already exist are used: Mochizuki INT4 hash pin (model-tools/dlssnr-int4.sha256),
# Mochizuki model header/source hashes, lmxxf tensor layouts/Small hashes, and the
# danielblnc weights.bin size floor. A Failed entry removes that backend from the install.
$weightProblems = New-Object System.Collections.Generic.List[object]
$weightFailed = @{}
function Add-WeightProblem([string]$Backend, [bool]$Failed, [string[]]$En, [string[]]$Zh = @()) {
    $script:weightProblems.Add([pscustomobject]@{ Backend = $Backend; Failed = $Failed; En = $En })
    if ($Failed) { $script:weightFailed[$Backend] = $true }
}
function Write-WeightBlock([string[]]$Lines) {
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor Red
    foreach ($line in $Lines) { Write-Host $line -ForegroundColor Red }
    Write-Host ('=' * 78) -ForegroundColor Red
    Write-Host ''
}
function Show-WeightProblems {
    if ($script:weightProblems.Count -eq 0) { return }
    $lines = @('WEIGHTS CHECK: problems found', '')
    foreach ($p in $script:weightProblems) {
        $state = if ($p.Failed) { 'FAILED - this backend will not be installed' } else { 'warning' }
        $lines += "<$($p.Backend): $state>"
        $lines += $p.En
        $lines += ''
    }
    Write-WeightBlock $lines
}

Write-Host ''
Write-Host 'Detected backend files (model presence is not a GPU compatibility test):' -ForegroundColor Cyan
Write-Host ("  lmxxf: runtime/modules={0}; weights={1}" -f $canLmxxf, $(if ($lmxxfWeights) { $lmxxfWeights } else { 'MISSING - supply native-game-tiled-assets' }))
Write-Host ("  daniel: runtime/installer={0}; weights={1}; NVIDIA model={2}" -f $canDaniel, $(if ($weights) { $weights } else { 'MISSING' }), $(if ($nv) { $nv } else { 'MISSING' }))
Write-Host ("  mochizuki: ASI/resources={0}; model={1}" -f $canMochizuki, $(if ($mochizukiModel) { $mochizukiModel } else { 'MISSING - supply dlssnr.bin or nvngx_dlssnr.dll 310.9.16 / 310.8.0; extraction follows backend selection' }))

if ($hasBackendSelection -and $PSBoundParameters.ContainsKey('Backend')) {
    Fail 'Pass either -Backend or -Backends, not both.'
}
if ($hasBackendSelection) {
    $allowedBackendIds = @('lmxxf','daniel','mochizuki')
    if (-not $Backends -or @($Backends).Count -eq 0) { Fail '-Backends requires at least one backend id.' }
    $seenBackend = @{}
    foreach ($id in @($Backends)) {
        if ($allowedBackendIds -notcontains $id) { Fail "Unknown backend id: $id" }
        if ($seenBackend.ContainsKey($id)) { Fail "Duplicate backend id: $id" }
        $seenBackend[$id] = $true
    }
    if (($seenBackend.ContainsKey('lmxxf') -and -not $canLmxxf) -or
        ($seenBackend.ContainsKey('daniel') -and -not $canDaniel) -or
        ($seenBackend.ContainsKey('mochizuki') -and -not $canMochizuki)) {
        Fail 'A requested backend is not present in the package.'
    }
    $installLmxxf = [bool]$seenBackend.ContainsKey('lmxxf')
    $installDaniel = [bool]$seenBackend.ContainsKey('daniel')
    $installMochizuki = [bool]$seenBackend.ContainsKey('mochizuki')
    $selectedActiveBackend = $null
    foreach ($id in @('lmxxf','daniel','mochizuki')) {
        if ($seenBackend.ContainsKey($id)) { $selectedActiveBackend = $id; break }
    }
    if ($PSBoundParameters.ContainsKey('ActiveBackend')) {
        if (-not $seenBackend.ContainsKey($ActiveBackend)) { Fail 'ActiveBackend is not in -Backends.' }
        $selectedActiveBackend = $ActiveBackend
    }
} elseif ($canMochizuki -or $Backend -ne 'auto') {
    if ($Backend -eq 'auto' -and -not $NonInteractive) {
        $choices = @('Install all available backends')
        $kinds = @('all')
        if ($canLmxxf) { $choices += 'Install lmxxf only'; $kinds += 'lmxxf' }
        if ($canDaniel) { $choices += 'Install daniel only'; $kinds += 'daniel' }
        if ($canMochizuki) { $choices += 'Install mochizuki only (Vulkan / RDNA4)'; $kinds += 'mochizuki' }
        $choice = Ask-Choice 'Choose NR backends to install:' $choices
        $Backend = $kinds[$choice - 1]
    }
    if ($Backend -eq 'auto') { $Backend = 'all' }
    if (($Backend -eq 'mochizuki' -and -not $canMochizuki) -or
        ($Backend -eq 'lmxxf' -and -not $canLmxxf) -or ($Backend -eq 'daniel' -and -not $canDaniel)) {
        Fail "Requested backend is not present in the package: $Backend"
    }
    $installMochizuki = $canMochizuki -and $Backend -in @('all','mochizuki')
    $installLmxxf = $canLmxxf -and $Backend -in @('all','lmxxf')
    $installDaniel = $canDaniel -and $Backend -in @('all','daniel')
    if (-not ($installMochizuki -or $installLmxxf -or $installDaniel)) { Fail 'No requested backend is available.' }
    $selectedActiveBackend = if ($installLmxxf) { 'lmxxf' } elseif ($installDaniel) { 'daniel' } else { 'mochizuki' }
    if ($Backend -eq 'all' -and -not $NonInteractive) {
        $defaults = @(); if ($installLmxxf) { $defaults += 'lmxxf' }; if ($installDaniel) { $defaults += 'daniel' }; if ($installMochizuki) { $defaults += 'mochizuki' }
        if ($defaults.Count -gt 1) { $selectedActiveBackend = $defaults[(Ask-Choice 'Choose the active backend:' $defaults) - 1] }
    }
} elseif ($canLmxxf -and $canDaniel) {
    if ($NonInteractive) {
        $installLmxxf  = $true
        $installDaniel = $true
        $selectedActiveBackend = 'lmxxf'
    } else {
        $choice = Ask-Choice "Detected files for both neural rendering backends. Select installation mode:" @(
            "Install lmxxf backend (using native-game-tiled-assets)",
            "Install danielblnc backend (using dlssnr_amd_pass / weights.bin)",
            "Install both backends (coexist in game folder, switch via OptiScaler.ini)"
        )
        switch ($choice) {
            1 { $installLmxxf = $true; $installDaniel = $false; $selectedActiveBackend = 'lmxxf' }
            2 { $installLmxxf = $false; $installDaniel = $true; $selectedActiveBackend = 'daniel' }
            3 {
                $installLmxxf  = $true
                $installDaniel = $true
                $defChoice = Ask-Choice "Select default active backend in OptiScaler.ini:" @(
                    "Default: lmxxf backend",
                    "Default: danielblnc backend"
                )
                $selectedActiveBackend = if ($defChoice -eq 1) { 'lmxxf' } else { 'daniel' }
            }
        }
    }
} elseif ($canLmxxf) {
    $installLmxxf  = $true
    $installDaniel = $false
    $selectedActiveBackend = 'lmxxf'
    Write-Host ''
    Write-Host "Detected lmxxf backend components. Preparing to install lmxxf backend." -ForegroundColor Cyan
    Write-Host "NOTE: danielblnc backend files not detected (missing dlssnr_on_amd_setup.exe or version.dll)." -ForegroundColor DarkYellow
} elseif ($canDaniel) {
    $installLmxxf  = $false
    $installDaniel = $true
    $selectedActiveBackend = 'daniel'
    Write-Host ''
    Write-Host "Detected danielblnc backend components. Preparing to install danielblnc backend." -ForegroundColor Cyan
    Write-Host "NOTE: lmxxf backend components not detected (missing LmxxfNrRuntime.dll or lmxxf-modules)." -ForegroundColor DarkYellow
} else {
    Fail @"
No valid neural rendering backend files detected.
- If using lmxxf backend: ensure LmxxfNrRuntime.dll and lmxxf-modules (and native-game-tiled-assets weights folder) are present.
- If using danielblnc backend: place dlssnr_on_amd_setup.exe + nvngx_dlssnr.dll (or existing version.dll + weights.bin) next to Setup.bat.
"@
}

# Validate and stage the complete package before changing installed files,
# including danielblnc setup, proxy moves, DLL/INI copies and module manifests.
if ($installLmxxf) {
    $gameModsDir = Join-Path $game 'lmxxf-modules'
    try { $lmxxfStage = New-LmxxfModuleStage $lmxxfMods $gameModsDir -Upgrade }
    catch { Fail $_.Exception.Message }
}

if ($installMochizuki) {
    try {
        $null = @(Get-LmxxfUnlinkedFiles $mochizukiAssets)
        $null = Assert-LmxxfUnlinkedPath (Join-Path $game 'dlssnr-amd')
        if (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd')) {
            $null = @(Get-LmxxfUnlinkedFiles (Join-Path $game 'dlssnr-amd'))
        }
    } catch { Fail $_.Exception.Message }
}

if ($installMochizuki) {
    # Models are the 0.0.8.2 ASI's own files. The official extractor writes them;
    # existing files are accepted when present and non-empty. No hash or model
    # header whitelist: a newer official model must remain usable.
    $mochizukiNv = $nv
    $networksExplicit = $MochizukiNetworks -and @($MochizukiNetworks).Count -gt 0
    if ($networksExplicit) {
        $MochizukiNetworks = @($MochizukiNetworks | ForEach-Object { ([string]$_).Split(',') } |
            ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ } | Select-Object -Unique)
        $badNetworks = @($MochizukiNetworks | Where-Object { $_ -notin @('small', 'experimental', 'original', 'int4') })
        if ($badNetworks.Count -gt 0) {
            Fail ("Unknown Mochizuki network(s): " + ($badNetworks -join ', '))
        }
        # The install range is explicit; legacy MochizukiModels only drives old
        # two-base callers and is not used for base selection here.
        $MochizukiModels = 'all'
    }
    $modelsExplicit = $networksExplicit -or (-not [string]::IsNullOrWhiteSpace($MochizukiModels))

    if (-not $modelsExplicit) {
        if ($mochizukiModel -and $mochizukiSmallModel) {
            if ($NonInteractive) {
                # Never block an unattended update on a prompt: both are present.
                $MochizukiModels = 'both'
                Write-Host 'Both Mochizuki ASI models are already present; installing both.' -ForegroundColor Cyan
            } else {
                Write-Host ''
                Write-Host 'Select the Mochizuki network to install and start with:' -ForegroundColor Yellow
                Write-Host '  1. Both base models (recommended)'
                Write-Host '  2. CC_SMALL_FP8'
                Write-Host '  3. CC_SMALL_FP8_EXPERIMENTAL (made from CC_SMALL_FP8)'
                Write-Host '  4. CC_SILVER_AARDWOLD'
                Write-Host '  5. CC_SILVER_AARDWOLD_INT4_MIXED (made from CC_SILVER_AARDWOLD)'
                Write-Host '  0. Cancel'
                while ($true) {
                    $choice = Read-Host 'Enter choice [1/2/3/4/5/0]'
                    if ($null -ne $choice) { $choice = $choice.Trim() }
                    if ($choice -eq '1') { $MochizukiModels = 'both'; break }
                    if ($choice -eq '2') { $MochizukiModels = 'small'; break }
                    if ($choice -eq '3') { $MochizukiModels = 'experimental'; break }
                    if ($choice -eq '4') { $MochizukiModels = 'original'; break }
                    if ($choice -eq '5') { $MochizukiModels = 'int4'; break }
                    if ($choice -eq '0') { Write-Host 'Cancelled.'; Pause-Exit 0 }
                    Write-Host 'Invalid choice. Enter 1, 2, 3, 4, 5, or 0.' -ForegroundColor Yellow
                }
            }
        } elseif ($mochizukiModel) {
            $MochizukiModels = 'original'
            Write-Host 'Only the original ASI model is present; installing it. Provide nvngx_dlssnr.dll 310.9.16 to add the small model.'
        } elseif ($mochizukiSmallModel) {
            $MochizukiModels = 'small'
            Write-Host 'Only the small ASI model is present; installing it. Provide nvngx_dlssnr.dll 310.8.0 to add the original model.'
        } elseif ($mochizukiNv) {
            $MochizukiModels = 'both'
            Write-Host 'No ASI model is installed yet; extracting from nvngx_dlssnr.dll.' -ForegroundColor Cyan
        } else {
            Fail 'No Mochizuki ASI model found. Put nvngx_dlssnr.dll (310.9.16 or 310.8.0) beside Setup.bat and run Setup again.'
        }
    }

    # Experimental is built from the small network; INT4 mixed from the original.
    if ($networksExplicit) {
        $wantSmall = @($MochizukiNetworks | Where-Object { $_ -in @('small', 'experimental') }).Count -gt 0
        $wantOriginal = @($MochizukiNetworks | Where-Object { $_ -in @('original', 'int4') }).Count -gt 0
        $generateExperimental = $MochizukiNetworks -contains 'experimental'
        $generateInt4 = $MochizukiNetworks -contains 'int4'
        if ($MochizukiActiveModel -and $MochizukiNetworks -notcontains $MochizukiActiveModel) {
            Fail "Conflict: -MochizukiActiveModel '$MochizukiActiveModel' is not in the selected install range."
        }
    } else {
        $wantSmall = $MochizukiModels -in @('both', 'all', 'small', 'experimental')
        $wantOriginal = $MochizukiModels -in @('both', 'all', 'original', 'int4')
        $generateExperimental = $MochizukiModels -in @('both', 'all', 'experimental')
        $generateInt4 = $MochizukiModels -in @('both', 'all', 'int4')
        if ($MochizukiActiveModel) {
            $activeNeedsSmall = $MochizukiActiveModel -in @('small', 'experimental')
            $activeNeedsOriginal = $MochizukiActiveModel -in @('original', 'int4')
            if ($activeNeedsSmall -and -not $wantSmall) {
                Fail "Conflict: -MochizukiActiveModel '$MochizukiActiveModel' needs the small network, which -MochizukiModels does not install."
            }
            if ($activeNeedsOriginal -and -not $wantOriginal) {
                Fail "Conflict: -MochizukiActiveModel '$MochizukiActiveModel' needs the original network, which -MochizukiModels does not install."
            }
        }
    }

    $needExtractOriginal = $wantOriginal -and -not $mochizukiModel
    $needExtractSmall    = $wantSmall -and -not $mochizukiSmallModel
    if ($needExtractOriginal -or $needExtractSmall) {
        if (-not $mochizukiNv) {
            Fail 'Model preparation failed: put nvngx_dlssnr.dll 310.9.16 or 310.8.0 beside Setup.bat (or pass -Dll). The existing installation was preserved.'
        }
        if (-not $mochizukiTools) {
            Fail 'Model preparation failed: model-tools/dlssnr_extract_model.exe is missing from this package. Re-extract the complete update.'
        }
        $modelOutput = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-amd-model-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($modelOutput)
        $mochizukiModelStage = $modelOutput
        Write-Host 'Extracting Mochizuki ASI models from nvngx_dlssnr.dll...' -ForegroundColor Cyan
        & $mochizukiTools $mochizukiNv $modelOutput
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Fail "Model preparation failed (extractor exit code $code). The existing installation was preserved."
        }
        $extractedOriginal = Join-Path $modelOutput 'dlssnr.bin'
        $extractedSmall = Join-Path $modelOutput 'dlssnr-small.bin'
        $gotOriginal = (Test-Path -LiteralPath $extractedOriginal -PathType Leaf) -and
            (Get-Item -LiteralPath $extractedOriginal).Length -gt 0
        $gotSmall = (Test-Path -LiteralPath $extractedSmall -PathType Leaf) -and
            (Get-Item -LiteralPath $extractedSmall).Length -gt 0
        if ($needExtractSmall -and -not $gotSmall -and -not $modelsExplicit -and $gotOriginal) {
            # 310.8.0 provides only the original network. An implicit "both" follows
            # what the official tool actually produced instead of failing.
            Write-Host 'Note: this DLL has no small model; installing the original model only.' -ForegroundColor Yellow
            $MochizukiModels = 'original'
            $wantSmall = $false
            $generateExperimental = $false
            $needExtractSmall = $false
        }
        if ($needExtractOriginal) {
            if (-not $gotOriginal) {
                Fail 'Model preparation failed: the extractor did not produce dlssnr.bin. The existing installation was preserved.'
            }
            $mochizukiModel = $extractedOriginal
        }
        if ($needExtractSmall) {
            if (-not $gotSmall) {
                Fail 'Model preparation failed: this DLL has no small model (dlssnr-small.bin); use the 310.9.16 DLL for both models, or install the original model only.'
            }
            $mochizukiSmallModel = $extractedSmall
        }
        # Keep the models in the package for later installs when that folder is writable.
        # A failed cache copy must not delete the staged file the install still needs.
        foreach ($pair in @(@($extractedOriginal, 'dlssnr.bin'), @($extractedSmall, 'dlssnr-small.bin'))) {
            if (-not (Test-Path -LiteralPath $pair[0] -PathType Leaf)) { continue }
            $target = Join-Path $mochizukiAssets $pair[1]
            try {
                Copy-Item -LiteralPath $pair[0] -Destination $target -Force -ErrorAction Stop
                if ($pair[1] -eq 'dlssnr.bin') { $mochizukiModel = $target } else { $mochizukiSmallModel = $target }
            } catch {
                Write-Host "Note: could not keep $($pair[1]) in the package folder; the staged copy is used for this install." -ForegroundColor Yellow
            }
        }
    }

    if ($wantOriginal -and -not $mochizukiModel) {
        Fail 'The original ASI model is unavailable. Provide nvngx_dlssnr.dll 310.8.0 or 310.9.16 and run Setup again.'
    }
    if ($wantSmall -and -not $mochizukiSmallModel) {
        Fail 'The small ASI model is unavailable. Provide nvngx_dlssnr.dll 310.9.16 and run Setup again.'
    }
    if ($MochizukiActiveModel -in @('small', 'experimental') -and -not $mochizukiSmallModel) {
        Fail 'Conflict: the selected ASI network needs the small model, which this source does not provide.'
    }
    if ($MochizukiActiveModel -in @('original', 'int4') -and -not $mochizukiModel) {
        Fail 'Conflict: the selected ASI network needs the original model, which this source does not provide.'
    }
}
$lmxxfSelectedModels = $LmxxfModels
if ($installLmxxf) {
    if (-not $lmxxfModelHelper) { Fail 'Incomplete package: lmxxf-small.ps1 is missing. Re-extract the complete package.' }
    $lmxxfOriginalPack = $null
    foreach ($candidate in $modelCandidates) {
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and ((Get-Item -LiteralPath $candidate).Length -ge 65536) -and (Test-MochizukiModelFile $candidate 599)) {
            $lmxxfOriginalPack = $candidate; break
        }
    }
    $lmxxfOriginalSourceDll = $null
    if (-not $lmxxfWeights -and -not $lmxxfOriginalPack) {
        foreach ($candidate in $modelSourceCandidates) {
            if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and ((Get-Item -LiteralPath $candidate).Length -ge 65536)) {
                $candSha = Get-Sha256 $candidate
                if ($candSha -ieq $mochizukiNewNvSha -or $candSha -ieq $mochizukiNvSha) {
                    $lmxxfOriginalSourceDll = $candidate; break
                }
            }
        }
    }
    $lmxxfSmallPack = $null
    foreach ($candidate in $smallModelCandidates) {
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and ((Get-Item -LiteralPath $candidate).Length -ge 65536) -and (Test-MochizukiModelFile $candidate 255)) {
            $lmxxfSmallPack = $candidate; break
        }
    }
    $lmxxfSourceDll = $null
    if (-not $lmxxfSmallWeights -and -not $lmxxfSmallPack) {
        foreach ($candidate in $modelSourceCandidates) {
            if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and ((Get-Item -LiteralPath $candidate).Length -ge 65536) -and (Get-Sha256 $candidate) -ieq $mochizukiNewNvSha) { $lmxxfSourceDll = $candidate; break }
        }
    }
    $canPrepareLmxxfOriginal = [bool]($lmxxfWeights -or $lmxxfOriginalPack -or $lmxxfOriginalSourceDll)
    $canPrepareLmxxfSmall = [bool]($lmxxfSmallWeights -or $lmxxfSmallPack -or $lmxxfSourceDll)
    if (-not $lmxxfSelectedModels) {
        if (-not $NonInteractive) {
            Write-Host 'Select lmxxf models: 1 = Both, 2 = Small (310.9.16), 3 = Original'
            do { $choice = Read-Host 'Choice'; $lmxxfSelectedModels = switch ($choice) { '1' {'both'} '2' {'small'} '3' {'original'} } } while (-not $lmxxfSelectedModels)
            $lmxxfModelsExplicit = $true
        } elseif ($canPrepareLmxxfOriginal -and $canPrepareLmxxfSmall) { $lmxxfSelectedModels = 'both' }
        elseif ($canPrepareLmxxfSmall) { $lmxxfSelectedModels = 'small' }
        elseif ($canPrepareLmxxfOriginal) { $lmxxfSelectedModels = 'original' }
        else { $lmxxfSelectedModels = 'none'; Write-Host 'lmxxf models missing; installing components only. Supply complete Original or Small weights before enabling NR.' -ForegroundColor Yellow }
    }
    if ($lmxxfSelectedModels -in @('both','original')) {
        if (-not $lmxxfWeights) {
            if (-not $lmxxfOriginalPack -and -not $lmxxfOriginalSourceDll) {
                Fail 'lmxxf Original weights are missing or incomplete. Supply complete native-game-tiled-assets, a verified dlssnr.bin, or nvngx_dlssnr.dll 310.9.16 / 310.8.0 before installation.'
            }
            if (-not $lmxxfSmallStage) {
                $lmxxfSmallStage = Join-Path $Root ('.lmxxf-stage-' + [guid]::NewGuid().ToString('N'))
                [void][IO.Directory]::CreateDirectory($lmxxfSmallStage)
            }
            $lmxxfWeights = Join-Path $lmxxfSmallStage 'original'
            Write-Host 'Preparing lmxxf Original weights from model source...' -ForegroundColor Cyan
            Initialize-LmxxfOriginal -Pack $lmxxfOriginalPack -Dll $lmxxfOriginalSourceDll -Output $lmxxfWeights
            if (-not (Test-LmxxfModel $lmxxfWeights original)) { Fail 'Prepared lmxxf Original weights failed validation.' }
        }
    }
    if ($lmxxfSelectedModels -eq 'none') {
        foreach ($candidate in $lmxxfCandidates) {
            if (Test-Path -LiteralPath $candidate -PathType Container) {
                Add-WeightProblem 'lmxxf' $false @("[lmxxf] Wrong folder: $candidate (Original and Small are missing or incomplete)", '  Supply a complete Original or Small model before enabling lmxxf NR.')
            }
        }
    }
    if ($lmxxfSelectedModels -in @('both','small')) {
        if ($lmxxfSmallWeights -and -not (Test-LmxxfModel $lmxxfSmallWeights small)) { Fail 'lmxxf Small weight validation failed. The game folder was not changed.' }
        if (-not $lmxxfSmallWeights) {
            if (-not $lmxxfSmallPack -and -not $lmxxfSourceDll) { Fail 'lmxxf Small requires native-game-tiled-assets/small, a verified dlssnr-small.bin, or nvngx_dlssnr.dll 310.9.16.' }
            if (-not $lmxxfSmallStage) {
                $lmxxfSmallStage = Join-Path $Root ('.lmxxf-stage-' + [guid]::NewGuid().ToString('N'))
                [void][IO.Directory]::CreateDirectory($lmxxfSmallStage)
            }
            $lmxxfSmallWeights = Join-Path $lmxxfSmallStage 'small'
            Write-Host 'Preparing lmxxf Small weights from model source...' -ForegroundColor Cyan
            Initialize-LmxxfSmall -Pack $lmxxfSmallPack -Dll $lmxxfSourceDll -Output $lmxxfSmallWeights
            if (-not (Test-LmxxfModel $lmxxfSmallWeights small)) { Fail 'Prepared lmxxf Small weights failed validation.' }
        }
    }
    # Keep an explicit retained model preference. Reject an unavailable combination before writing files.
    $lmxxfRetainedModel = $null
    $lmxxfGameIni = Join-Path $game 'OptiScaler.ini'
    $inNrSection = $false
    $lmxxfIniAction = $null
    if (Test-Path -LiteralPath $lmxxfGameIni -PathType Leaf) {
        if ($launcherDecisions -and $launcherDecisions.existingIni) {
            $lmxxfIniAction = [string]$launcherDecisions.existingIni
            if ($lmxxfIniAction -notin @('keep','overwrite')) { Fail 'DecisionJson existingIni must be overwrite or keep.' }
        } elseif ($NonInteractive) { $lmxxfIniAction = 'overwrite' }
        else {
            $iniChoice = Ask-Choice 'OptiScaler.ini already exists in the game folder.' @('Overwrite OptiScaler.ini (Recommended: major version update!)','Keep the existing OptiScaler.ini')
            $lmxxfIniAction = if ($iniChoice -eq 1) { 'overwrite' } else { 'keep' }
        }
    }
    $keepIniChoice = $lmxxfIniAction -eq 'keep'
    if ($keepIniChoice) {
        foreach ($line in [IO.File]::ReadAllLines($lmxxfGameIni)) {
            if ($line -match '^\s*\[([^\]]+)\]') { $inNrSection = $Matches[1] -ieq 'DlssNr' }
            elseif ($inNrSection -and $line -match '^\s*LmxxfNetworkModel\s*=\s*([01])\s*$') { $lmxxfRetainedModel = $Matches[1]; break }
        }
        if ($lmxxfRetainedModel -eq '0' -and $lmxxfSelectedModels -eq 'small' -and -not (Test-LmxxfWeights (Join-Path $game 'native-game-tiled-assets'))) { Fail 'Kept INI selects lmxxf Original, but only Small is available. Choose both models, change LmxxfNetworkModel, or overwrite the INI.' }
        if ($lmxxfRetainedModel -eq '1' -and $lmxxfSelectedModels -eq 'original' -and -not (Test-LmxxfModel (Join-Path $game 'native-game-tiled-assets/small') small)) { Fail 'Kept INI selects lmxxf Small, but only Original is available. Choose both models, change LmxxfNetworkModel, or overwrite the INI.' }
    }
    foreach ($model in @('original','small')) {
        if ($lmxxfSelectedModels -notin @('both',$model)) { continue }
        $source = if ($model -eq 'original') { $lmxxfWeights } else { $lmxxfSmallWeights }
        $target = Join-Path $game 'native-game-tiled-assets'
        if ($model -eq 'small') { $target = Join-Path $target 'small' }
        $null = Assert-LmxxfModelPath $target
        foreach ($file in @(Get-LmxxfModelFiles $source $model)) {
            $path = Assert-LmxxfModelPath (Join-Path $target $file.Name)
            if (Test-FileLocked $path) { Fail "Model destination is locked: $path. Close the game completely." }
            if ([IO.Path]::GetExtension($path) -eq '.f16') {
                $alternate = Assert-LmxxfModelPath ([IO.Path]::ChangeExtension($path,'.f32'))
                if (Test-FileLocked $alternate) { Fail "Model destination is locked: $alternate. Close the game completely." }
            }
        }
    }
}

if ($installDaniel -and $weights -and (Get-Item -LiteralPath $weights).Length -lt 1MB) {
    $size = (Get-Item -LiteralPath $weights).Length
    Add-WeightProblem 'daniel' $true @(
        "[daniel] Wrong file: $weights (only $size bytes: truncated or not a weights file)",
        '  Expected:   dlssnr_on_amd_weights.bin of at least 1 MB (no hash pin exists; the hash is per machine)',
        '  Fix: delete it, run dlssnr_on_amd_setup.exe with nvngx_dlssnr.dll available, then run Setup again.')
}

Show-WeightProblems
if ($weightProblems.Count -gt 0 -and -not $NonInteractive) {
    $null = Read-Host 'Read the red WEIGHTS CHECK block above, then press Enter to continue'
}
$selectedBackends = @(@(@('lmxxf', $installLmxxf), @('daniel', $installDaniel), @('mochizuki', $installMochizuki)) |
    Where-Object { $_[1] } | ForEach-Object { $_[0] })
$droppedBackends = @($selectedBackends | Where-Object { $weightFailed[$_] })
if ($droppedBackends.Count -gt 0) {
    $names = $droppedBackends -join ', '
    if ($droppedBackends.Count -eq $selectedBackends.Count) {
        Show-WeightProblems
        Fail "Weights check failed for: $names. Nothing was installed; the game folder was not changed. See the red WEIGHTS CHECK block above."
    }
    foreach ($kind in $droppedBackends) {
        Write-Host "Skipping ${kind}: weights check failed (see the red WEIGHTS CHECK block)." -ForegroundColor Red
        switch ($kind) { 'lmxxf' { $installLmxxf = $false } 'daniel' { $installDaniel = $false } 'mochizuki' { $installMochizuki = $false } }
    }
    if ($selectedActiveBackend -in $droppedBackends) {
        $selectedActiveBackend = @($selectedBackends | Where-Object { $_ -notin $droppedBackends })[0]
        Write-Host "Active backend changed to $selectedActiveBackend." -ForegroundColor Yellow
    }
}

# --- process danielblnc runtime if selected ---
# Detect existing model locks before updating any game files.
if ($installMochizuki) {
    $selectedModelNames = @()
    if ($wantOriginal) { $selectedModelNames += 'dlssnr.bin' }
    if ($wantSmall) { $selectedModelNames += 'dlssnr-small.bin' }
    foreach ($modelName in $selectedModelNames) {
        $modelTarget = Join-Path $game (Join-Path 'dlssnr-amd' $modelName)
        if (Test-FileLocked $modelTarget) {
            Fail "Model destination is locked: $modelTarget. Close the game completely before running Setup again."
        }
    }
}
$stagedA = $null
if ($installDaniel) {
    $gameHasNv = Test-Path -LiteralPath (Join-Path $game 'nvngx_dlssnr.dll') -PathType Leaf
    $gameHasW  = Test-Path -LiteralPath (Join-Path $game 'dlssnr_on_amd_weights.bin') -PathType Leaf
    if (-not $gameHasNv -and -not $gameHasW) {
        $srcNv = $nv
        if ($srcNv -and (Test-Path -LiteralPath $srcNv -PathType Leaf)) {
            Copy-Item -LiteralPath $srcNv -Destination (Join-Path $game 'nvngx_dlssnr.dll') -Force
            Write-Host 'Copied nvngx_dlssnr.dll into the game folder (danielblnc runtime expects it there).' -ForegroundColor Green
            $nv = Join-Path $game 'nvngx_dlssnr.dll'
        }
    }

    if (-not $srcA -and (Test-Path -LiteralPath $setup -PathType Leaf) -and
        ((Get-Item -LiteralPath $setup).Length -ge 65536) -and
        (Get-Command Ensure-NativeModelExtractor -ErrorAction SilentlyContinue)) {
        try {
            Ensure-NativeModelExtractor
            $extractedRuntime = Join-Path $Root 'version.dll'
            if ([OptScaler.NativeModelExtractor]::ExtractDanielRuntime($setup, $extractedRuntime)) {
                Write-Host "Extracted danielblnc runtime (version.dll) from dlssnr_on_amd_setup.exe." -ForegroundColor Green
                $srcA = Find-AuthorRuntime
            }
        } catch {
            Write-Host "NOTE: native runtime extraction from dlssnr_on_amd_setup.exe skipped: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
    if (-not $weights -and $nv -and (Test-Path -LiteralPath $nv -PathType Leaf) -and
        ((Get-Item -LiteralPath $nv).Length -ge 65536) -and
        (Get-Command Ensure-NativeModelExtractor -ErrorAction SilentlyContinue)) {
        try {
            Ensure-NativeModelExtractor
            $extractedWeights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
            [OptScaler.NativeModelExtractor]::ExtractDanielWeights($nv, $extractedWeights)
            if (Test-Path -LiteralPath $extractedWeights -PathType Leaf) {
                Write-Host "Extracted dlssnr_on_amd_weights.bin directly from nvngx_dlssnr.dll." -ForegroundColor Green
                $weights = $extractedWeights
            }
        } catch {
            Write-Host "NOTE: native weights extraction from nvngx_dlssnr.dll failed: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    if ((-not $srcA -or -not $weights) -and (Test-Path -LiteralPath $setup -PathType Leaf)) {
        Write-Host ''
        Write-Host 'version.dll and/or weights.bin not found yet.' -ForegroundColor Yellow
        Write-Host 'Launching danielblnc setup (dlssnr_on_amd_setup.exe) to create them…' -ForegroundColor Yellow
        if ($nv) { Write-Host "  nvngx_dlssnr found: $nv" } else {
            Write-Host '  NOTE: no nvngx_dlssnr.dll next to Setup.bat or in the game folder.' -ForegroundColor Yellow
            Write-Host '  The danielblnc setup will ask you to locate it if it needs one for weights.' -ForegroundColor Yellow
        }
        Write-Host '  In the danielblnc UI: pick the GAME folder if asked, finish install/close when done.'
        Push-Location $Root
        try {
            $p = Start-Process -FilePath $setup -WorkingDirectory $Root -Wait -PassThru
            Write-Host "  danielblnc setup exit code: {0}" -f $p.ExitCode
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
Still missing a known DLSS-NR-on-AMD runtime (version.dll) after danielblnc setup.
Supported: 0.3.0–0.6.0.
1. Run dlssnr_on_amd_setup.exe yourself and finish its install
2. Put the version.dll it produces next to Setup.bat (or leave it in the game folder)
Download from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
    }

    $hashA = Get-PeSha256 $srcA
    Write-Host ("danielblnc runtime SHA256: {0}" -f $hashA)
    if ($expectedAuthor -notcontains $hashA) {
        $what = 'unknown build'
        if ($hashA -eq $knownA0217) { $what = 'this is 0.2.17, not 0.3.x/0.4.x' }
        Fail @"
$srcA is not a supported DLSS-NR-on-AMD runtime ($what).
  file:     $srcA
  got:      $hashA
  expected: $expectedA030 (0.3.0) .. $expectedA060 (0.6.0)
Download 0.3.0–0.6.0 from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
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
                Write-Host "Copied danielblnc runtime outside game folder: $stagedA"
            }
            $srcA = $stagedA
        }
    } catch {
        Fail "Could not stage danielblnc runtime from $srcA : $($_.Exception.Message)"
    }

    if (-not $weights) {
        $weights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
    }
    if (-not (Test-Path -LiteralPath $weights -PathType Leaf)) {
        if ((Test-Path -LiteralPath $setup) -and $nv) {
            Write-Host 'weights.bin still missing — running danielblnc setup again with nvngx…'
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

if ($uninstallFirst) {
    try {
        $optiSource = Save-ReinstallSource (Join-Path $release 'OptiScaler.dll')
        foreach ($name in @('OptiScaler.ini', 'OptiScaler', 'person-model')) {
            $path = Join-Path $release $name
            if (Test-Path -LiteralPath $path) { $null = Save-ReinstallSource $path }
        }
        $release = [IO.Path]::GetDirectoryName($optiSource)
        if ($installLmxxf) {
            $lmxxfRuntime = Save-ReinstallSource $lmxxfRuntime
            $lmxxfShaders = Save-ReinstallSource $lmxxfShaders
            $lmxxfManifestSource = Join-Path (Split-Path -Parent $lmxxfModelHelper) 'lmxxf-small-manifest.json'
            $lmxxfModelHelper = Save-ReinstallSource $lmxxfModelHelper
            $null = Save-ReinstallSource $lmxxfManifestSource
            . $lmxxfModelHelper
        }
        if ($installMochizuki) {
            $mochizukiAsi = Save-ReinstallSource $mochizukiAsi
            $mochizukiAssets = Save-ReinstallSource $mochizukiAssets
            $mochizukiTools = Save-ReinstallSource $mochizukiTools
            if ($mochizukiInt4Tool) { $mochizukiInt4Tool = Save-ReinstallSource $mochizukiInt4Tool }
            if ($mochizukiProbe) { $mochizukiProbe = Save-ReinstallSource $mochizukiProbe }
        }
        if ($installDaniel) { $srcA = Save-ReinstallSource $srcA }
        $ps1Src = Save-ReinstallSource $ps1Src
        $batSrc = Save-ReinstallSource $batSrc
        $moduleHelperSrc = Save-ReinstallSource $moduleHelperSrc
    } catch { Fail ("Could not preserve installation sources before uninstall: " + $_.Exception.Message) }

    Write-Host ''
    Write-Host 'Uninstalling the existing OptiScaler installation...' -ForegroundColor Cyan
    Write-Host ("Uninstall script: {0}" -f $ps1Src)
    # A child process isolates the uninstaller's exit statement. Y already grants
    # consent: no second confirmation, backup deletion, or intermediate pause.
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $ps1Src -GameDir $game -NonInteractive -NoPause
    if ($LASTEXITCODE -ne 0) { Fail "Uninstall failed (exit code $LASTEXITCODE). Installation stopped; fix the reported error and run Setup again." }
    Write-Host 'Uninstall completed. Continuing installation...' -ForegroundColor Green
}

# --- inspect common injection DLLs after the optional uninstall ---
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
if ($installDaniel) {
    Write-Host "danielblnc runtime:      $srcA  -> will be copied as dlssnr_amd_pass1/2/3.dll"
    Write-Host 'NOTE: danielblnc version.dll is NOT left in the game folder (this package only installs OptiScaler as the proxy).'
}
if ($installLmxxf) {
    Write-Host "lmxxf runtime:           $lmxxfRuntime  -> will be copied to game folder"
}
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

$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$backup = Join-Path $game "backup-amd-presr-$stamp"
$toMove = @()
# Files the user chose to keep. The install step must not overwrite these.
$keep = @{}

foreach ($f in $found) {
    $isTarget = ($f.Name -ieq $Proxy)
    # danielblnc native version.dll (hash already known) must not stay next to OptiScaler.
    $isAuthorNative = $false
    if ($f.Name -ieq 'version.dll' -and -not $f.IsOptiScaler) {
        try {
            $h = Get-PeSha256 $f.Path
            if ($expectedAuthor -contains $h -or $h -eq $knownA0217) { $isAuthorNative = $true }
        } catch { }
    }
    if ($isAuthorNative) {
        Write-Host ("{0} is the danielblnc NR runtime — moving aside (cannot coexist with OptiScaler)." -f $f.Name) -ForegroundColor Yellow
        $toMove += $f
        continue
    }
    if ($f.IsOptiScaler) {
        # Overwrite the target directly; retire other identified OptiScaler proxies
        # to avoid duplicate injection without retaining old package copies.
        $toMove += $f
        if (-not $isTarget) {
            Write-Host ("{0} is a previous OptiScaler proxy — removing it to prevent duplicate injection with {1}." -f $f.Name, $Proxy) -ForegroundColor Yellow
        }
    } else {
        $fp = $null
        if ($launcherDecisions -and $launcherDecisions.foreignProxy) {
            $rawFp = $launcherDecisions.foreignProxy
            $fpRows = @($rawFp)
            if ($fpRows.Count -gt 0 -and $null -ne $fpRows[0].PSObject.Properties['Key']) {
                foreach ($row in $fpRows) {
                    if ([string]$row.Key -ieq $f.Name) { $fp = [string]$row.Value; break }
                }
            } else {
                $prop = $rawFp.PSObject.Properties[$f.Name]
                if (-not $prop) { $prop = $rawFp.PSObject.Properties[$f.Name.ToLowerInvariant()] }
                if ($prop) { $fp = [string]$prop.Value }
            }
        }
        if ($fp) {
            switch ($fp) {
                'cancel' { Write-Host 'Cancelled.'; Pause-Exit 0 }
                'backup' { $toMove += $f }
                'keep' {
                    if ($isTarget) {
                        Fail ("You chose to keep {0}, but that is also the proxy name we would install as. Pick a different -Proxy." -f $f.Name)
                    }
                    $keep[$f.Name] = $true
                }
                default { Fail ("DecisionJson foreignProxy for {0} must be backup, keep, or cancel." -f $f.Name) }
            }
            continue
        }
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

# Publish a fully validated module tree before changing DLLs. A failed directory
# switch restores the previous tree; no per-file live module overwrite is used.
if ($installLmxxf) {
    $moduleBackup = Join-Path $backup 'lmxxf-modules'
    try { Publish-LmxxfModuleStage $lmxxfStage $gameModsDir $moduleBackup -Upgrade }
    catch { Fail ("Could not install lmxxf-modules: " + $_.Exception.Message) }
    $lmxxfStage = $null
    if (Test-Path -LiteralPath $moduleBackup) {
        Write-Host "User-added module files excluded from the active package are preserved in: $moduleBackup" -ForegroundColor Cyan
    }
}

# Old OptiScaler files are ordinary overwrite data. Preserve a single copy only
# for the author runtime or another proxy explicitly selected for moving aside.
foreach ($f in $toMove) {
    if ($f.IsOptiScaler) {
        if ($f.Name -ine $Proxy) { Remove-Item -LiteralPath $f.Path -Force -ErrorAction Stop }
        continue
    }
    if (-not (Test-Path -LiteralPath $backup)) {
        [void][System.IO.Directory]::CreateDirectory($backup)
    }
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
    $isMochizukiModel = ($rel -in @('dlssnr-amd/dlssnr.bin', 'dlssnr-amd/dlssnr-small.bin'))
    $isWeight = ($rel -ieq 'dlssnr_on_amd_weights.bin') -or
                ($rel -ilike 'native-game-tiled-assets\*') -or
                ($rel -ilike 'native-game-tiled-assets/*') -or $isMochizukiModel

    $aside = $null
    try {
        if (Test-Path -LiteralPath $dest) {
            if ($isMochizukiModel) {
                $expectedEntries = if ($rel -eq 'dlssnr-amd/dlssnr-small.bin') { 255 } else { 599 }
                if ((Test-MochizukiModelFile $dest $expectedEntries) -and ((Get-Sha256 $dest) -eq (Get-Sha256 $src))) {
                    Write-Host ("Skip (verified model already in place): {0}" -f $rel)
                    return
                }
                $aside = $dest + '.bak-' + (Get-Date -Format 'yyyyMMddHHmmss')
                Move-Item -LiteralPath $dest -Destination $aside -Force -ErrorAction Stop
                Write-Host "Backed up mismatched or invalid model: $dest -> $aside" -ForegroundColor Yellow
            } elseif ($isWeight) {
                return
            }
        }
        $ddir = Split-Path -Parent $dest
        if ($ddir) { [void][System.IO.Directory]::CreateDirectory($ddir) }
        Copy-Item -LiteralPath $src -Destination $dest -Force
    } catch [System.IO.IOException] {
        if ($aside -and (Test-Path -LiteralPath $aside)) {
            Move-Item -LiteralPath $aside -Destination $dest -Force -ErrorAction SilentlyContinue
        }
        Fail @"
Install failed on $rel :
  $($_.Exception.Message)

The file is probably locked by a running game.
Close the game completely, then run Setup.bat again.
Run Setup again after closing the game to finish the full-package overwrite.
"@
    } catch {
        if ($aside -and (Test-Path -LiteralPath $aside)) {
            Move-Item -LiteralPath $aside -Destination $dest -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}

function Get-IniSetting([string]$iniPath, [string]$sectionName, [string]$key) {
    $inSection = $false
    foreach ($line in [System.IO.File]::ReadAllLines($iniPath)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            if ($inSection) { break }
            $inSection = ($Matches[1] -ieq $sectionName)
        } elseif ($inSection -and $line -match ('^\s*' + [regex]::Escape($key) + '\s*=(.*)$')) {
            return $Matches[1].Trim()
        }
    }
    return $null
}

function Set-IniSettings([string]$iniPath, [string]$sectionName, [System.Collections.IDictionary]$settings, [switch]$OnlyMissing) {
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
                    if (-not $OnlyMissing) { $lines[$i] = "$k = $($settings[$k])" }
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

# Keep this standalone helper aligned with uninstall and PluginPath.h.
# Relative paths are anchored to the game executable directory, never the launcher's CWD.
function Get-PluginsTargetDirectory([string]$gameDir, [string]$iniPath) {
    $gameDir = [IO.Path]::GetFullPath($gameDir)
    $cfgPath = $null
    $mainPath = $null
    if (Test-Path -LiteralPath $iniPath -PathType Leaf) {
        $cfgPath = Get-IniSetting $iniPath 'Plugins' 'Path'
        $mainPath = Get-IniSetting $iniPath 'Libraries' 'OptiDllPath'
    }
    if ($cfgPath -and $cfgPath -ine 'auto') {
        # Setup creates an explicit plugin directory before the host checks it.
        $target = if ([IO.Path]::IsPathRooted($cfgPath)) { $cfgPath } else { Join-Path $gameDir $cfgPath }
        return [IO.Path]::GetFullPath($target)
    }
    if (-not $mainPath -or $mainPath -ieq 'auto') { $mainPath = 'OptiScaler' }
    if (-not [IO.Path]::IsPathRooted($mainPath)) { $mainPath = Join-Path $gameDir $mainPath }
    $mainPath = [IO.Path]::GetFullPath($mainPath)
    if (-not (Test-Path -LiteralPath $mainPath -PathType Container)) { $mainPath = $gameDir }
    return Join-Path $mainPath 'plugins'
}

Write-Host ''
Write-Host "Installing $productTitle as $Proxy ..." -ForegroundColor Cyan
Install-One (Join-Path $release 'OptiScaler.dll') $Proxy

$ini = Join-Path $release 'OptiScaler.ini'
$gameIni = Join-Path $game 'OptiScaler.ini'
$iniFromTemplate = $false
if (Test-Path -LiteralPath $ini -PathType Leaf) {
    if (-not (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
        Install-One $ini 'OptiScaler.ini'
        if (-not $keep.ContainsKey('OptiScaler.ini')) { $iniFromTemplate = $true }
    } else {
        # Recommended on a large version jump. -NonInteractive takes that option.
        $overwriteIni = $NonInteractive
        if ($lmxxfIniAction) { $overwriteIni = $lmxxfIniAction -eq 'overwrite' }
        elseif ($launcherDecisions -and $launcherDecisions.existingIni) {
            if ($launcherDecisions.existingIni -eq 'overwrite') { $overwriteIni = $true }
            elseif ($launcherDecisions.existingIni -eq 'keep') { $overwriteIni = $false }
            else { Fail 'DecisionJson existingIni must be overwrite or keep.' }
        }
        elseif (-not $NonInteractive) {
            $iniChoice = Ask-Choice 'OptiScaler.ini already exists in the game folder.' @(
                'Overwrite OptiScaler.ini (Recommended: major version update!)'
                'Keep the existing OptiScaler.ini'
            )
            $overwriteIni = ($iniChoice -eq 1)
        }
        if ($overwriteIni) {
            Install-One $ini 'OptiScaler.ini'
            if (-not $keep.ContainsKey('OptiScaler.ini')) { $iniFromTemplate = $true }
            Write-Host 'Replaced OptiScaler.ini with the package file.' -ForegroundColor Cyan
        } else {
            Write-Host 'Keeping the existing OptiScaler.ini.' -ForegroundColor Cyan
        }
    }
}
# Apply the launcher's UI language only to a template that this run created or
# overwrote. A kept user ini keeps its own [Menu] Language.
$uiLanguage = $null
if ($launcherDecisions -and $launcherDecisions.PSObject.Properties['language']) {
    if ($launcherDecisions.language -eq 'zh') { $uiLanguage = 'zh-CN' }
    elseif ($launcherDecisions.language -eq 'en') { $uiLanguage = 'en' }
}
if ($iniFromTemplate -and $uiLanguage -and (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
    Set-IniSettings $gameIni 'Menu' ([ordered]@{ 'Language' = $uiLanguage })
}
$deps = Join-Path $release 'OptiScaler'
if (Test-Path -LiteralPath $deps) {
    Get-ChildItem -LiteralPath $deps -Recurse -File | Where-Object {
        # Custom plugins go through the resolved-path/config-preservation flow below.
        $relative = $_.FullName.Substring($deps.Length)
        $relative -notmatch '^[\\/]plugins[\\/]' -or
            $relative -ieq '\plugins\OptScaler-NR-plugins-README.txt'
    } | ForEach-Object {
        $rel = Join-Path 'OptiScaler' $_.FullName.Substring($deps.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
}

$personModelSrc = Join-Path $release 'person-model'
if (Test-Path -LiteralPath $personModelSrc -PathType Container) {
    Write-Host 'Installing person partition model & ONNX Runtime (person-model)...' -ForegroundColor Cyan
    Get-ChildItem -LiteralPath $personModelSrc -File | ForEach-Object {
        Install-One $_.FullName (Join-Path 'person-model' $_.Name)
    }
}

# --- Install selected backends ---
if ($installDaniel) {
    Write-Host 'Installing danielblnc runtime (dlssnr_amd_pass1-3.dll) + weights.bin...' -ForegroundColor Cyan
    foreach ($p in 1..3) {
        Install-One $srcA ("dlssnr_amd_pass$p.dll")
    }
    $gameWeightsBin = Join-Path $game 'dlssnr_on_amd_weights.bin'
    if (Test-Path -LiteralPath $gameWeightsBin -PathType Leaf) {
        Write-Host 'dlssnr_on_amd_weights.bin already present in game folder.' -ForegroundColor Green
    } else {
        Install-One $weights 'dlssnr_on_amd_weights.bin'
    }
}

if ($installLmxxf) {
    Write-Host 'Installing lmxxf runtime + modules + shaders...' -ForegroundColor Cyan
    Install-One $lmxxfRuntime 'LmxxfNrRuntime.dll'

    Write-Host '  installed verified dual-architecture modules (76 modules).'
    if ($lmxxfShaders) {
        $shaderKeep = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        Get-ChildItem -LiteralPath $lmxxfShaders -Recurse -File | ForEach-Object {
            $relLeaf = $_.FullName.Substring($lmxxfShaders.Length).TrimStart('\','/')
            $rel = Join-Path 'shaders' $relLeaf
            # Keep set is package top-level *.hlsl only (relLeaf has no subdir).
            if ($_.Extension -ieq '.hlsl' -and ($relLeaf -notmatch '[\\/]')) {
                [void]$shaderKeep.Add($_.Name)
            }
            Install-One $_.FullName $rel
        }
        # Install is upsert-only for named files; purge retired native_/preblock_ *.hlsl left by older packages.
        $gameShadersDir = Join-Path $game 'shaders'
        if ((Test-Path -LiteralPath $gameShadersDir -PathType Container) -and $shaderKeep.Count -gt 0) {
            $stale = @(Get-ChildItem -LiteralPath $gameShadersDir -Filter '*.hlsl' -File -ErrorAction SilentlyContinue |
                Where-Object { ($_.Name -match '^(native_|preblock_)') -and (-not $shaderKeep.Contains($_.Name)) })
            foreach ($sf in $stale) {
                try {
                    Remove-Item -LiteralPath $sf.FullName -Force
                    Write-Host ("  removed stale shader: {0}" -f $sf.Name) -ForegroundColor DarkYellow
                } catch {
                    Write-Host ("  WARN: could not remove stale shader {0}: {1}" -f $sf.Name, $_.Exception.Message) -ForegroundColor Yellow
                }
            }
            if ($stale.Count -gt 0) {
                Write-Host ("  purged {0} stale shader(s); live set={1}" -f $stale.Count, $shaderKeep.Count) -ForegroundColor Cyan
            }
        }
        Write-Host "  shaders installed from $lmxxfShaders"
    } else {
        Write-Host 'NOTE: lmxxf shaders not found in package; PrepareFrame may fail until shaders/ is beside OptiScaler.' -ForegroundColor DarkYellow
    }
    $gameWeights = Join-Path $game 'native-game-tiled-assets'
    foreach ($model in @('original','small')) {
        if ($lmxxfSelectedModels -notin @('both',$model)) { continue }
        $source = if ($model -eq 'original') { $lmxxfWeights } else { $lmxxfSmallWeights }
        $target = if ($model -eq 'original') { $gameWeights } else { Join-Path $gameWeights 'small' }
        Copy-LmxxfModel -Source $source -Destination $target -Model $model
        Write-Host "lmxxf $model weights verified: $target" -ForegroundColor Green
    }
}

# Build CC_SMALL_FP8_EXPERIMENTAL from CC_SMALL_FP8 and the vendored delta.
# Structural checks only: no fixed SHA gate, so a newer official delta/model works.
function New-MochizukiExperimentalWeights([string]$Small, [string]$Delta, [string]$Output) {
    $pack = [IO.File]::ReadAllBytes($Small)
    if ($pack.Length -lt 16 -or [Text.Encoding]::ASCII.GetString($pack, 0, 8) -ne 'NRMODEL1') {
        throw "$Small is not an NRMODEL1 model pack"
    }
    $count = [BitConverter]::ToUInt32($pack, 8)
    $index = @{}; $at = 16
    for ($k = 0; $k -lt $count; $k++) {
        if ($at + 4 -gt $pack.Length) { throw "${Small}: truncated entry" }
        $n = [BitConverter]::ToUInt32($pack, $at)
        if ($at + 4 + $n + 16 -gt $pack.Length) { throw "${Small}: truncated entry name" }
        $name = [Text.Encoding]::UTF8.GetString($pack, $at + 4, $n)
        $index[$name] = @([BitConverter]::ToUInt64($pack, $at + 4 + $n), [BitConverter]::ToUInt64($pack, $at + 12 + $n))
        $at += 4 + $n + 16
    }
    $d = [IO.File]::ReadAllBytes($Delta)
    if ($d.Length -lt 12 -or [Text.Encoding]::ASCII.GetString($d, 0, 8) -ne 'NRXDIFF1') {
        throw "$Delta is not an NRXDIFF1 weights difference"
    }
    $count = [BitConverter]::ToUInt32($d, 8); $at = 12
    for ($k = 0; $k -lt $count; $k++) {
        if ($at + 4 -gt $d.Length) { throw "${Delta}: truncated entry" }
        $n = [BitConverter]::ToUInt32($d, $at)
        if ($at + 4 + $n + 8 -gt $d.Length) { throw "${Delta}: truncated entry name" }
        $name = [Text.Encoding]::UTF8.GetString($d, $at + 4, $n)
        $size = [BitConverter]::ToUInt64($d, $at + 4 + $n)
        $at += 4 + $n + 8
        if (-not $index.ContainsKey($name) -or $index[$name][1] -ne $size) {
            throw "$name is not in $Small with $size bytes"
        }
        if ($at + $size -gt $d.Length) { throw "${Delta}: truncated data for $name" }
        $off = [int64]$index[$name][0]
        if ($off + $size -gt $pack.Length) { throw "${Small}: entry $name exceeds the pack" }
        for ([int64]$i = 0; $i -lt $size; $i++) {
            $pack[$off + $i] = $pack[$off + $i] -bxor $d[$at + $i]
        }
        $at += [int64]$size
    }
    if ($at -ne $d.Length) { throw "$Delta has $($d.Length - $at) bytes left over" }
    [IO.File]::WriteAllBytes($Output, $pack)
}

# --- Install the Neural Rendering ASI payload ---
if ($installMochizuki) {
    $asiManifest = Join-Path $game 'dlssnr-amd-install.txt'
    $asiFiles = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
    $asiTargetDir = Get-PluginsTargetDirectory $game $gameIni
    if (-not (Test-Path -LiteralPath $asiTargetDir -PathType Container)) {
        [void][IO.Directory]::CreateDirectory($asiTargetDir)
    }
    $asiTarget = Join-Path $asiTargetDir 'dlssnr-amd.asi'
    $gameFullPrefix = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'

    function Add-AsiOwned([string]$fullPath) {
        $full = [IO.Path]::GetFullPath($fullPath)
        if ($full.StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            [void]$asiFiles.Add($full.Substring($gameFullPrefix.Length))
        }
    }

    # The previous install's own inventory decides what this version may remove.
    # Models and user files are never in it; a manifest entry outside the shared
    # resource tree or this install's plugin path is ignored.
    $previousAsi = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
    $asiTargetRel = $null
    if ($asiTarget.StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $asiTargetRel = $asiTarget.Substring($gameFullPrefix.Length)
    }
    if (Test-Path -LiteralPath $asiManifest -PathType Leaf) {
        foreach ($line in [IO.File]::ReadAllLines($asiManifest)) {
            if ($line -notmatch '^(?i)F (.+)$') { continue }
            $oldRel = $Matches[1].Trim()
            if ($oldRel -match '(^|[\\/])\.\.([\\/]|$)' -or [IO.Path]::IsPathRooted($oldRel)) { continue }
            $normalized = $oldRel.Replace('\', '/')
            if ($normalized -match '^(?i)dlssnr-amd/' -or
                ($asiTargetRel -and $oldRel -ieq $asiTargetRel)) {
                [void]$previousAsi.Add($oldRel)
            }
        }
    }

    # Replace our shared resource folder with this ASI version. The retired
    # built-in runtime's files are removed only where the legacy inventory owns
    # them, so user files and other runtimes are untouched.
    $resourceRoot = Join-Path $mochizukiAssets ''
    $modelNames = @('dlssnr.bin', 'dlssnr-small.bin', 'dlssnr-int4.bin', 'dlssnr-small-experimental.bin')
    $legacyPin = Find-FirstFile @((Join-Path $Root 'mochizuki-owned.txt'), (Join-Path $PSScriptRoot 'install/mochizuki-owned.txt'))
    $newRelative = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @(Get-LmxxfUnlinkedFiles $resourceRoot)) {
        if ($modelNames -contains $file.Name) { continue }
        $rel = $file.FullName.Substring($resourceRoot.Length).TrimStart('\', '/')
        [void]$newRelative.Add(('dlssnr-amd/' + $rel.Replace('\', '/')))
    }
    if ($legacyPin -and (Test-Path -LiteralPath $legacyPin -PathType Leaf)) {
        foreach ($line in [IO.File]::ReadAllLines($legacyPin)) {
            $rel = $line.Trim().Replace('\', '/')
            if ($rel.Length -eq 0 -or $rel.StartsWith('#')) { continue }
            if ($rel.IndexOfAny([char[]]@('*', '?')) -ge 0) { continue }
            $ownedRel = 'dlssnr-amd/' + $rel
            if ($newRelative.Contains($ownedRel)) { continue }
            $stale = Join-Path $game $ownedRel
            if (Test-Path -LiteralPath $stale -PathType Leaf) {
                try { Remove-Item -LiteralPath $stale -Force -ErrorAction Stop }
                catch { Write-Host "NOTE: could not remove retired asset $ownedRel (game running?): $($_.Exception.Message)" -ForegroundColor Yellow }
            }
        }
    }
    $legacyRuntime = Join-Path $game 'MochizukiNrRuntime.dll'
    if (Test-Path -LiteralPath $legacyRuntime -PathType Leaf) {
        $movedTo = Join-Path $backup 'MochizukiNrRuntime.dll.retired'
        if (-not (Test-Path -LiteralPath $backup -PathType Container)) { [void][IO.Directory]::CreateDirectory($backup) }
        try {
            Move-Item -LiteralPath $legacyRuntime -Destination $movedTo -Force -ErrorAction Stop
            Write-Host "Moved the retired built-in runtime to $movedTo" -ForegroundColor Yellow
        } catch {
            Write-Host "NOTE: could not move the retired MochizukiNrRuntime.dll aside: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    # Plugin and probe. The shared resource folder may not exist yet on a clean game.
    [void][IO.Directory]::CreateDirectory((Join-Path $game 'dlssnr-amd'))
    Copy-Item -LiteralPath $mochizukiAsi -Destination $asiTarget -Force
    Add-AsiOwned $asiTarget
    if ($mochizukiProbe -and (Test-Path -LiteralPath $mochizukiProbe -PathType Leaf)) {
        $probeTarget = Join-Path $game 'dlssnr-amd/nr_present_probe.exe'
        Copy-Item -LiteralPath $mochizukiProbe -Destination $probeTarget -Force
        Add-AsiOwned $probeTarget
    }

    # Shared resources: this version's files, never a mixture with the old set.
    foreach ($file in @(Get-LmxxfUnlinkedFiles $resourceRoot)) {
        if ($modelNames -contains $file.Name) { continue }
        $rel = $file.FullName.Substring($resourceRoot.Length).TrimStart('\', '/')
        $target = Join-Path $game (Join-Path 'dlssnr-amd' $rel)
        $parent = Split-Path -Parent $target
        if ($parent) { [void][IO.Directory]::CreateDirectory($parent) }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        Add-AsiOwned $target
    }

    # Models: presence-driven, no format or version gate.
    $mochizukiModelsToInstall = @()
    if ($wantOriginal -and $mochizukiModel) {
        $mochizukiModelsToInstall += @{ Src = $mochizukiModel; Rel = 'dlssnr-amd/dlssnr.bin' }
    }
    if ($wantSmall -and $mochizukiSmallModel) {
        $mochizukiModelsToInstall += @{ Src = $mochizukiSmallModel; Rel = 'dlssnr-amd/dlssnr-small.bin' }
    }
    foreach ($m in $mochizukiModelsToInstall) {
        $destPath = Join-Path $game $m.Rel
        $destExisted = Test-Path -LiteralPath $destPath -PathType Leaf
        if ($destExisted) {
            $same = $false
            try { $same = (Get-Sha256 $destPath) -eq (Get-Sha256 $m.Src) } catch { }
            if ($same) {
                Write-Host ("Skip (model already in place): {0}" -f $m.Rel)
                continue
            }
            $backupPath = $destPath + '.bak-' + (Get-Date -Format 'yyyyMMddHHmmss')
            Move-Item -LiteralPath $destPath -Destination $backupPath -Force -ErrorAction Stop
            Write-Host "Backed up existing model: $destPath -> $backupPath" -ForegroundColor Yellow
        }
        $parentDir = Split-Path -Parent $destPath
        if ($parentDir) { [void][IO.Directory]::CreateDirectory($parentDir) }
        Copy-Item -LiteralPath $m.Src -Destination $destPath -Force
        Write-Host "Installed ASI model: $($m.Rel)" -ForegroundColor Green
    }

    # A previous game's derived files were cached in the package by
    # Save-GameModelForNextInstall. A new game gets that real cache instead of
    # regenerating (and failing when the package tools are unavailable).
    $cachedExperimental = Find-FirstFile @(
        (Join-Path $mochizukiAssets 'dlssnr-small-experimental.bin'),
        (Join-Path $Root 'dlssnr-amd/dlssnr-small-experimental.bin')
    )
    $cachedExperimentalTarget = Join-Path $game 'dlssnr-amd/dlssnr-small-experimental.bin'
    if ($generateExperimental -and $cachedExperimental -and
        [IO.Path]::GetFullPath($cachedExperimental) -ne [IO.Path]::GetFullPath($cachedExperimentalTarget) -and
        -not (Test-Path -LiteralPath $cachedExperimentalTarget -PathType Leaf)) {
        Copy-Item -LiteralPath $cachedExperimental -Destination $cachedExperimentalTarget -Force
        Write-Host 'Copied cached CC_SMALL_FP8_EXPERIMENTAL weights.' -ForegroundColor Green
    }
    $cachedInt4 = Find-FirstFile @(
        (Join-Path $mochizukiAssets 'dlssnr-int4.bin'),
        (Join-Path $Root 'dlssnr-amd/dlssnr-int4.bin')
    )
    $cachedInt4Target = Join-Path $game 'dlssnr-amd/dlssnr-int4.bin'
    if ($generateInt4 -and $cachedInt4 -and
        [IO.Path]::GetFullPath($cachedInt4) -ne [IO.Path]::GetFullPath($cachedInt4Target) -and
        -not (Test-Path -LiteralPath $cachedInt4Target -PathType Leaf)) {
        Copy-Item -LiteralPath $cachedInt4 -Destination $cachedInt4Target -Force
        Write-Host 'Copied cached CC_SILVER_AARDWOLD_INT4_MIXED weights.' -ForegroundColor Green
    }

    # INT4 mixed weights are generated by the official tool. When this install
    # requests the network, missing inputs or a failed generation fail the
    # install instead of reporting success or trusting an old file.
    $int4Data = Join-Path $game 'dlssnr-amd/int4'
    $int4OutFile = Join-Path $game 'dlssnr-amd/dlssnr-int4.bin'
    $int4Reusable = $false
    if ($generateInt4 -and (Test-Path -LiteralPath $int4OutFile -PathType Leaf)) {
        $originalPath = Join-Path $game 'dlssnr-amd/dlssnr.bin'
        if (Test-Path -LiteralPath $originalPath -PathType Leaf) {
            $int4Info = Get-Item -LiteralPath $int4OutFile
            $originalInfo = Get-Item -LiteralPath $originalPath
            $int4Reusable = $int4Info.Length -gt 0 -and $int4Info.LastWriteTimeUtc -ge $originalInfo.LastWriteTimeUtc
        }
    }
    if ($generateInt4 -and $int4Reusable) {
        $int4Ready = $true
        Write-Host 'Reused existing CC_SILVER_AARDWOLD_INT4_MIXED weights.' -ForegroundColor Green
    } elseif ($generateInt4) {
        if (-not $mochizukiInt4Tool) {
            Fail 'CC_SILVER_AARDWOLD_INT4_MIXED was requested, but model-tools/dlssnr-int4-weights.exe is missing. Re-extract the complete package.'
        }
        if (-not (Test-Path -LiteralPath $int4Data -PathType Container)) {
            Fail 'CC_SILVER_AARDWOLD_INT4_MIXED was requested, but dlssnr-amd/int4 is missing. Re-extract the complete package.'
        }
        if (-not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr.bin') -PathType Leaf)) {
            Fail 'CC_SILVER_AARDWOLD_INT4_MIXED needs the original model, which is not installed.'
        }
        $int4Out = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-int4-' + [guid]::NewGuid().ToString('N') + '.bin')
        try {
            & $mochizukiInt4Tool (Join-Path $game 'dlssnr-amd/dlssnr.bin') $int4Data $int4Out
            if ($LASTEXITCODE -ne 0) {
                Fail "CC_SILVER_AARDWOLD_INT4_MIXED generation failed (exit code $LASTEXITCODE)."
            }
            if (-not (Test-Path -LiteralPath $int4Out -PathType Leaf) -or (Get-Item -LiteralPath $int4Out).Length -le 0) {
                Fail 'CC_SILVER_AARDWOLD_INT4_MIXED generation produced no weights.'
            }
            Copy-Item -LiteralPath $int4Out -Destination $int4OutFile -Force
            Write-Host 'INT4 mixed weights generated.' -ForegroundColor Green
        } catch {
            if ($_.Exception.Message -like 'CC_SILVER_AARDWOLD_INT4_MIXED*') { throw }
            Fail "CC_SILVER_AARDWOLD_INT4_MIXED generation failed: $($_.Exception.Message)"
        } finally {
            Remove-Item -LiteralPath $int4Out -Force -ErrorAction SilentlyContinue
        }
        $int4Ready = $true
    } else {
        $int4Ready = (Test-Path -LiteralPath $int4OutFile -PathType Leaf) -and
            (Get-Item -LiteralPath $int4OutFile).Length -gt 0
    }

    # CC_SMALL_FP8_EXPERIMENTAL is made from the installed small model and the
    # packaged delta. A requested network whose generation fails is an install
    # failure, never a quiet fallback or a stale file.
    $experimentalOut = Join-Path $game 'dlssnr-amd/dlssnr-small-experimental.bin'
    $experimentalReusable = $false
    if ($generateExperimental -and (Test-Path -LiteralPath $experimentalOut -PathType Leaf)) {
        $smallPathForTime = Join-Path $game 'dlssnr-amd/dlssnr-small.bin'
        if (Test-Path -LiteralPath $smallPathForTime -PathType Leaf) {
            $expInfo = Get-Item -LiteralPath $experimentalOut
            $smallInfo = Get-Item -LiteralPath $smallPathForTime
            if ($expInfo.Length -gt 0 -and $expInfo.LastWriteTimeUtc -ge $smallInfo.LastWriteTimeUtc) {
            try {
                $stream = [IO.File]::OpenRead($experimentalOut)
                try { $magic = New-Object byte[] 8; $read = $stream.Read($magic, 0, 8) } finally { $stream.Dispose() }
                $experimentalReusable = $read -eq 8 -and [Text.Encoding]::ASCII.GetString($magic) -eq 'NRMODEL1'
                } catch { $experimentalReusable = $false }
            }
        }
    }
    if ($generateExperimental -and $experimentalReusable) {
        $experimentalReady = $true
        Write-Host 'Reused existing CC_SMALL_FP8_EXPERIMENTAL weights.' -ForegroundColor Green
    } elseif ($generateExperimental) {
        $experimentalDelta = Find-FirstFile @(
            (Join-Path $Root 'experimental/dlssnr-small-experimental.delta'),
            (Join-Path $release 'experimental/dlssnr-small-experimental.delta')
        )
        if (-not $experimentalDelta) {
            Fail 'CC_SMALL_FP8_EXPERIMENTAL was requested, but experimental/dlssnr-small-experimental.delta is missing. Re-extract the complete package.'
        }
        if (-not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr-small.bin') -PathType Leaf)) {
            Fail 'CC_SMALL_FP8_EXPERIMENTAL needs the small model, which is not installed.'
        }
        $expOut = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-small-experimental-' + [guid]::NewGuid().ToString('N') + '.bin')
        try {
            New-MochizukiExperimentalWeights (Join-Path $game 'dlssnr-amd/dlssnr-small.bin') $experimentalDelta $expOut
            if (-not (Test-Path -LiteralPath $expOut -PathType Leaf) -or (Get-Item -LiteralPath $expOut).Length -le 0) {
                Fail 'CC_SMALL_FP8_EXPERIMENTAL generation produced no weights.'
            }
            Copy-Item -LiteralPath $expOut -Destination $experimentalOut -Force
            $experimentalReady = $true
            Write-Host 'CC_SMALL_FP8_EXPERIMENTAL weights generated.' -ForegroundColor Green
        } catch {
            if ($_.Exception.Message -like 'CC_SMALL_FP8_EXPERIMENTAL*') { throw }
            Fail "CC_SMALL_FP8_EXPERIMENTAL generation failed: $($_.Exception.Message)"
        } finally {
            Remove-Item -LiteralPath $expOut -Force -ErrorAction SilentlyContinue
        }
    } else {
        $experimentalReady = (Test-Path -LiteralPath $experimentalOut -PathType Leaf) -and
            (Get-Item -LiteralPath $experimentalOut).Length -gt 0
    }

    # Assets this version no longer installs, but the previous install owned.
    # Removal happens only after the new payload is in place.
    foreach ($oldRel in $previousAsi) {
        if ($asiFiles.Contains($oldRel)) { continue }
        $stale = Join-Path $game $oldRel
        if (Test-Path -LiteralPath $stale -PathType Leaf) {
            try {
                Remove-Item -LiteralPath $stale -Force -ErrorAction Stop
                Write-Host "Removed asset no longer installed: $oldRel" -ForegroundColor DarkYellow
            } catch {
                Write-Host "NOTE: could not remove $oldRel (game running?): $($_.Exception.Message)" -ForegroundColor Yellow
            }
        }
    }

    # Ownership record: upgrade and uninstall delete exactly these files.
    $manifestLines = @('# Files installed by OptScaler(NR) for dlssnr-amd.asi; models are not listed.')
    foreach ($rel in @($asiFiles | Sort-Object)) { $manifestLines += ('F ' + $rel) }
    Set-Content -LiteralPath $asiManifest -Value ($manifestLines -join "`r`n") -Encoding ASCII

    # The ASI config template is installed once; a user's later edits win.
    $asiIni = Join-Path $game 'dlssnr-amd.ini'
    # The upstream default is the RenoDX style. The plain wilsjo2 template remains
    # a fallback and can be selected later in NR Control.
    $asiTemplate = Find-FirstFile @(
        (Join-Path $Root 'dlssnr-amd-optiscaler-renodx.ini'),
        (Join-Path $release 'dlssnr-amd-optiscaler-renodx.ini'),
        (Join-Path $Root 'dlssnr-amd-optiscaler.ini'),
        (Join-Path $release 'dlssnr-amd-optiscaler.ini')
    )
    $asiIniExisted = Test-Path -LiteralPath $asiIni -PathType Leaf
    if (-not $asiIniExisted -and $asiTemplate) {
        Copy-Item -LiteralPath $asiTemplate -Destination $asiIni -Force
    }
    # A fresh ini gets this install's startup network. An existing user ini is
    # preserved except when the caller explicitly chose a startup network; that
    # choice must take effect instead of being ignored.
    $writeActiveNetwork = (-not $asiIniExisted -and $asiTemplate) -or
        ($asiIniExisted -and [bool]$MochizukiActiveModel)
    if ($writeActiveNetwork -and (Test-Path -LiteralPath $asiIni -PathType Leaf)) {
        $smallReady = (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr-small.bin') -PathType Leaf) -and
            (Get-Item -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr-small.bin')).Length -gt 0
        $originalReady = (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr.bin') -PathType Leaf) -and
            (Get-Item -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr.bin')).Length -gt 0
        $activeModelName = $MochizukiActiveModel
        if (-not $activeModelName -and -not $asiIniExisted) {
            if ($networksExplicit) {
                foreach ($candidate in @('small', 'experimental', 'original', 'int4')) {
                    if ($MochizukiNetworks -notcontains $candidate) { continue }
                    $ready = ($candidate -eq 'small' -and $smallReady) -or
                        ($candidate -eq 'experimental' -and $experimentalReady) -or
                        ($candidate -eq 'original' -and $originalReady) -or
                        ($candidate -eq 'int4' -and $int4Ready)
                    if ($ready) { $activeModelName = $candidate; break }
                }
            }
            if (-not $activeModelName -and $MochizukiModels -in @('small', 'experimental', 'original', 'int4')) {
                $activeModelName = $MochizukiModels
            }
            if (-not $activeModelName) {
                if ($smallReady) { $activeModelName = 'small' }
                elseif ($originalReady) { $activeModelName = 'original' }
            }
        }
        if ($activeModelName -eq 'experimental' -and -not $experimentalReady) {
            Fail 'The selected startup network CC_SMALL_FP8_EXPERIMENTAL is not available; generation did not complete.'
        }
        if ($activeModelName -eq 'int4' -and -not $int4Ready) {
            Fail 'The selected startup network CC_SILVER_AARDWOLD_INT4_MIXED is not available; generation did not complete.'
        }
        if ($activeModelName -eq 'small' -and -not $smallReady) {
            Fail 'The selected startup network CC_SMALL_FP8 is not installed.'
        }
        if ($activeModelName -eq 'original' -and -not $originalReady) {
            Fail 'The selected startup network CC_SILVER_AARDWOLD is not installed.'
        }
        if ($activeModelName) {
            $networkName = switch ($activeModelName) {
                'small' { 'CC_SMALL_FP8' }
                'experimental' { 'CC_SMALL_FP8_EXPERIMENTAL' }
                'original' { 'CC_SILVER_AARDWOLD' }
                'int4' { 'CC_SILVER_AARDWOLD_INT4_MIXED' }
            }
            Set-IniSettings $asiIni 'Network' ([ordered]@{ 'Model' = $networkName })
            # With LayerN sections read_chain uses each layer's Network for the
            # running network; the first layer is the startup layer. Update that
            # layer too and leave the other layers untouched.
            $layerNumbers = @()
            foreach ($line in [IO.File]::ReadAllLines($asiIni)) {
                if ($line -match '^\s*\[Layer(\d+)\]\s*$') { $layerNumbers += [int]$Matches[1] }
            }
            if ($layerNumbers.Count -gt 0) {
                $firstLayer = $layerNumbers | Sort-Object | Select-Object -First 1
                Set-IniSettings $asiIni ("Layer" + $firstLayer) ([ordered]@{ 'Network' = $networkName })
            }
            $mochizukiActiveNetwork = $activeModelName
        }
    }
    # Upstream keeps the resolved config as dlssnr-amd\dlssnr-amd.default.ini;
    # the ASI restores a deleted dlssnr-amd.ini from it. Mirror the current main
    # file so user edits and this install's choices are what gets restored.
    if ($asiTemplate -and (Test-Path -LiteralPath $asiIni -PathType Leaf)) {
        $asiDefault = Join-Path $game 'dlssnr-amd/dlssnr-amd.default.ini'
        try {
            Copy-Item -LiteralPath $asiIni -Destination $asiDefault -Force -ErrorAction Stop
        } catch {
            Write-Host "NOTE: could not write $asiDefault : $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    if ($wantSmall -and -not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr-small.bin'))) {
        Write-Host 'Mochizuki small model not installed. Run Mochizuki-Model.bat beside Setup.bat with your nvngx_dlssnr.dll, then run Setup again.' -ForegroundColor Yellow
    }
    if ($wantOriginal -and -not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr.bin'))) {
        Write-Host 'Mochizuki original model not installed. Run Mochizuki-Model.bat beside Setup.bat with your nvngx_dlssnr.dll, then run Setup again.' -ForegroundColor Yellow
    }
}

# The staged extraction is only needed until the models are installed.
if ($mochizukiModelStage -and (Test-Path -LiteralPath $mochizukiModelStage)) {
    Remove-Item -LiteralPath $mochizukiModelStage -Recurse -Force -ErrorAction SilentlyContinue
    $mochizukiModelStage = $null
}

# Runs after a package overwrite as well. These keys are this install's choices, so the
# copied template cannot leave NrBackend (or Enabled / RunBeforeSR) on the package placeholder.
# Preference keys are only added when missing, so a kept ini retains model scale, every-frame,
# encoding and FitLarge. An overwrite already replaced those with the package defaults.
if ($installLmxxf -and $lmxxfSelectedModels -ne 'none' -and -not (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
    [IO.File]::WriteAllText($gameIni, "[DlssNr]`r`n", [Text.UTF8Encoding]::new($true))
}
if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
    # Fill a missing canonical key from its legacy spelling before using defaults.
    # -OnlyMissing below still gives an existing canonical choice precedence.
    $fitLargeDefault = Get-IniSetting $gameIni 'DlssNr' 'LmxxfFitLarge'
    $pdlDefault = Get-IniSetting $gameIni 'DlssNr' 'LmxxfPdl'
    if ([string]::IsNullOrWhiteSpace($fitLargeDefault)) { $fitLargeDefault = 'true' }
    if ([string]::IsNullOrWhiteSpace($pdlDefault)) { $pdlDefault = 'true' }
    $targetNetworkModel = 1
    if ($MochizukiModels -in @('small', 'experimental')) {
        $targetNetworkModel = 1
    } elseif ($MochizukiModels -in @('original', 'int4')) {
        $targetNetworkModel = 0
    } elseif ($MochizukiModels -in @('both', 'all')) {
        if ($MochizukiActiveModel -in @('original', 'int4')) {
            $targetNetworkModel = 0
        } elseif ($MochizukiActiveModel -in @('small', 'experimental')) {
            $targetNetworkModel = 1
        } else {
            $existingVal = Get-IniSetting $gameIni 'DlssNr' 'MochizukiNetworkModel'
            if ($existingVal -eq '0' -or $existingVal -eq '1') {
                $targetNetworkModel = [int]$existingVal
            } else {
                $targetNetworkModel = 1
            }
        }
    }
    $dlssnrSettings = [ordered]@{
        'RunBeforeSR' = 'true'
        'NrBackend' = $selectedActiveBackend
        'LmxxfDiagnostic' = 'off'
    }
    if ($installLmxxf -and $lmxxfSelectedModels -ne 'none') {
        $existingLmModel = Get-IniSetting $gameIni 'DlssNr' 'LmxxfNetworkModel'
        if ($keepIniChoice -and $lmxxfRetainedModel -in @('0','1')) {
            $dlssnrSettings['LmxxfNetworkModel'] = $lmxxfRetainedModel
        } elseif (-not $keepIniChoice -or $existingLmModel -notin @('0','1')) {
            $dlssnrSettings['LmxxfNetworkModel'] = if ($lmxxfSelectedModels -eq 'small') { '1' } else { '0' }
        }
    }
    Set-IniSettings $gameIni 'DlssNr' $dlssnrSettings
    # A user's explicit Enabled choice is preserved; only a missing key is seeded.
    Set-IniSettings $gameIni 'DlssNr' ([ordered]@{ 'Enabled' = 'true' }) -OnlyMissing
    if ($installMochizuki) {
        # The ASI is loaded by the host, not by a new loader.
        Set-IniSettings $gameIni 'Plugins' ([ordered]@{ 'LoadAsiPlugins' = 'true' })
        # NR Control opens with End. Replace only the default/empty FG key, never
        # a custom chord the user chose.
        $fgKey = Get-IniSetting $gameIni 'Menu' 'FGShortcutKey'
        if ([string]::IsNullOrWhiteSpace($fgKey) -or $fgKey -eq 'auto' -or $fgKey -eq '35' -or $fgKey -eq '0x23') {
            Set-IniSettings $gameIni 'Menu' ([ordered]@{ 'FGShortcutKey' = '-1' })
            Write-Host 'Set [Menu] FGShortcutKey=-1 because NR Control uses End. A custom FG key is preserved.' -ForegroundColor Green
        }
    }
    Set-IniSettings $gameIni 'DlssNr' ([ordered]@{
        'DLSS5_FIT_LARGE' = $fitLargeDefault
        'DLSS5_HIP_PDL' = $pdlDefault
        'LmxxfPaperWhite' = '1'
        'AmdModelScale' = '1'
        'AmdEncoding' = '0'
        'AmdEveryFrame' = 'true'
    }) -OnlyMissing
    Write-Host "Updated OptiScaler.ini [DlssNr] (NrBackend=$selectedActiveBackend; Enabled and other preference defaults only where missing)" -ForegroundColor Green
}

# Product keys live in OptiScaler.ini (menu > ini). native-game-flags.txt is only a
# fallback for DLSS5_* keys the host did not set. Do not rewrite txt from ini here.
if ($installLmxxf) {
    $flagsDir = Join-Path $game 'DLSS5-AMD'
    New-Item -ItemType Directory -Force -Path $flagsDir | Out-Null
    $flagsPath = Join-Path $flagsDir 'native-game-flags.txt'
    if (-not (Test-Path -LiteralPath $flagsPath -PathType Leaf)) {
        $seed = @(
            '# Optional lmxxf upstream keys (DLSS5_*).'
            '# OptiScaler.ini / Ins menu win on conflict; this file only fills gaps.'
            '# Example: DLSS5_HIP_WAVE_OWNED=1'
        ) -join "`r`n"
        [IO.File]::WriteAllText($flagsPath, ($seed + "`r`n"))
        Write-Host "  seeded optional flags fallback: $flagsPath" -ForegroundColor Green
    } else {
        Write-Host "  kept existing $flagsPath (OptiScaler.ini wins on conflict)" -ForegroundColor Green
    }
}

# --- Optional OptiPatcher (DLSS Spoofing Enhancement / OptiScaler native feature) ---
# Shared installer core: fetch the latest rolling release within a deadline, validate the PE and
# install it; fall back to the bundled offline copy when the network is unavailable or the download
# is invalid. Returns $true when an OptiPatcher.asi was installed into the game.
function Install-OptiPatcherPlugin {
    param([int]$TimeoutSec)

    Write-Host "Attempting to download latest OptiPatcher.asi (timeout: ${TimeoutSec}s)..." -ForegroundColor Cyan
    $optiPatcherUrl = 'https://github.com/optiscaler/OptiPatcher/releases/download/rolling/OptiPatcher.asi'
    $tempDownload = [IO.Path]::GetTempFileName()
    $downloadSucceeded = $false
    try {
        $curlCmd = Get-Command 'curl.exe' -ErrorAction SilentlyContinue
        if ($curlCmd) {
            # -f / --fail ensures non-zero exit on HTTP 4xx/5xx; Test-ValidPePlugin ensures valid PE
            & curl.exe -f -s -S -L --connect-timeout 10 --max-time $TimeoutSec -o $tempDownload $optiPatcherUrl 2>$null
            if ($LASTEXITCODE -eq 0 -and (Test-ValidPePlugin $tempDownload)) {
                $downloadSucceeded = $true
            }
        }
    } catch { }
    if (-not $downloadSucceeded) {
        try {
            $req = [System.Net.HttpWebRequest]::Create($optiPatcherUrl)
            $req.Timeout = $TimeoutSec * 1000
            $req.ReadWriteTimeout = $TimeoutSec * 1000
            $req.UserAgent = 'OptScaler-Setup'
            $resp = $req.GetResponse()
            try {
                $stream = $resp.GetResponseStream()
                try {
                    $fs = [IO.File]::Create($tempDownload)
                    try { $stream.CopyTo($fs) }
                    finally { $fs.Dispose() }
                } finally { $stream.Dispose() }
            } finally { $resp.Dispose() }
            if (Test-ValidPePlugin $tempDownload) {
                $downloadSucceeded = $true
            }
        } catch { }
    }

    # Resolve plugins destination following host [Plugins] Path resolution
    $targetPluginsDir = Get-PluginsTargetDirectory $game $gameIni
    $targetPluginFile = Join-Path $targetPluginsDir 'OptiPatcher.asi'
    $gameFullPrefix = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
    $displayDest = if ($targetPluginFile.StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $targetPluginFile.Substring($gameFullPrefix.Length)
    } else {
        $targetPluginFile
    }

    function Install-OptiPatcherFile([string]$sourcePath) {
        if ([IO.Path]::GetFullPath($targetPluginFile).StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            $rel = [IO.Path]::GetFullPath($targetPluginFile).Substring($gameFullPrefix.Length)
            Install-One $sourcePath $rel
        } else {
            if (-not (Test-Path -LiteralPath $targetPluginsDir -PathType Container)) {
                [void][IO.Directory]::CreateDirectory($targetPluginsDir)
            }
            Copy-Item -LiteralPath $sourcePath -Destination $targetPluginFile -Force
        }
    }

    $installed = $false
    if ($downloadSucceeded) {
        Write-Host 'Downloaded latest OptiPatcher.asi successfully.' -ForegroundColor Green
        # Update cache in package only when PE validation succeeded
        $pkgPluginDirs = @((Join-Path $release 'plugins'), (Join-Path $release 'OptiScaler\plugins'))
        foreach ($pd in $pkgPluginDirs) {
            if (Test-Path -LiteralPath $pd -PathType Container) {
                try {
                    Copy-Item -LiteralPath $tempDownload -Destination (Join-Path $pd 'OptiPatcher.asi') -Force
                    Write-Host "Updated package plugin cache: $(Join-Path $pd 'OptiPatcher.asi')" -ForegroundColor Green
                } catch { }
            }
        }
        Install-OptiPatcherFile $tempDownload
        $installed = $true
        Write-Host "Installed latest OptiPatcher.asi to: $displayDest" -ForegroundColor Green
    } else {
        Write-Host 'Network unavailable, timed out, or invalid download; using bundled offline version.' -ForegroundColor Yellow
        Write-Host "If ineffective, download from https://github.com/optiscaler/OptiPatcher/releases and replace $displayDest" -ForegroundColor Yellow
        $cachedPlugin = $null
        foreach ($cand in @(
            (Join-Path $release 'plugins\OptiPatcher.asi'),
            (Join-Path $release 'OptiScaler\plugins\OptiPatcher.asi')
        )) {
            if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Test-ValidPePlugin $cand)) {
                $cachedPlugin = $cand
                break
            }
        }
        if ($cachedPlugin) {
            Install-OptiPatcherFile $cachedPlugin
            $installed = $true
            Write-Host "Installed bundled offline OptiPatcher.asi to: $displayDest" -ForegroundColor Green
        } else {
            Write-Host 'WARN: Bundled offline OptiPatcher.asi not found in package.' -ForegroundColor Yellow
        }
    }

    if (Test-Path -LiteralPath $tempDownload) {
        try { Remove-Item -LiteralPath $tempDownload -Force } catch { }
    }

    return $installed
}

$installedOptiPatcher = $false
$isInteractiveUser = (-not $NonInteractive) -and (-not [Console]::IsInputRedirected -or [Console]::In.Peek() -ge 0)
if ($launcherDecisions) {
    $isInteractiveUser = $false
    if ([string]$launcherDecisions.optiPatcher -eq 'install') {
        Write-Host 'DecisionJson requested OptiPatcher; fetching latest with bundled fallback (30s deadline).' -ForegroundColor Cyan
        $installedOptiPatcher = Install-OptiPatcherPlugin 30
    }
}
if ($isInteractiveUser) {
    Write-Host ''
    Write-Host '===================================================' -ForegroundColor Cyan
    Write-Host 'OptiPatcher (DLSS Spoofing Enhancement / Native ASI Plugin)' -ForegroundColor Yellow
    Write-Host 'Unlocks native DLSS / DLSS-FG options in 270+ supported games without spoofing overhead.' -ForegroundColor Gray
    Write-Host 'More info: https://github.com/optiscaler/OptiPatcher' -ForegroundColor Gray
    Write-Host '---------------------------------------------------' -ForegroundColor Gray
    Write-Host 'Install OptiPatcher (DLSS Spoofing Enhancement)?' -ForegroundColor Yellow
    Write-Host '  [1] Yes (Recommended: fetch latest release or use bundled offline version)'
    Write-Host '  [2] No  (Skip)'

    $optiPatcherChoice = $false
    $maxAttempts = if ([Console]::IsInputRedirected) { 1 } else { 5 }
    $attempts = 0
    while ($attempts -lt $maxAttempts) {
        $attempts++
        $rawAns = Read-Host 'Enter choice [1/2] (Default: 1)'
        $ans = if ($null -ne $rawAns) { $rawAns.Trim() } else { '' }
        if ([string]::IsNullOrWhiteSpace($ans) -or $ans -eq '1' -or $ans -match '^(?i)y(es)?$') {
            $optiPatcherChoice = $true
            break
        } elseif ($ans -eq '2' -or $ans -match '^(?i)n(o)?$') {
            $optiPatcherChoice = $false
            break
        } else {
            Write-Host 'Invalid choice. Enter 1 or 2 (or press Enter for default [1]).' -ForegroundColor Yellow
        }
    }

    if ($optiPatcherChoice) {
        $installedOptiPatcher = Install-OptiPatcherPlugin 60
    }
}

# ASI plugins are on by default: an ASI component is part of this product, so the install
# always writes true, overriding an explicit false in the game ini on purpose. Users who do
# not want it can set false by hand afterwards; the launcher no longer exposes a toggle.
if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
    Set-IniSettings $gameIni 'Plugins' ([ordered]@{ 'LoadAsiPlugins' = 'true' })
    Write-Host 'Enabled LoadAsiPlugins=true in OptiScaler.ini.' -ForegroundColor Green
}

# Ensure the resolved plugins directory always exists in the game folder and copy any
# user-supplied custom .asi / .ini files placed in the package's plugins\ directory before Setup.
try {
    $resolvedPluginsDir = Get-PluginsTargetDirectory $game $gameIni
    if (-not (Test-Path -LiteralPath $resolvedPluginsDir -PathType Container)) {
        [void][IO.Directory]::CreateDirectory($resolvedPluginsDir)
    }
    $gameFullPrefix = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
    foreach ($srcPluginDir in @((Join-Path $release 'plugins'), (Join-Path $release 'OptiScaler\plugins'))) {
        if (-not (Test-Path -LiteralPath $srcPluginDir -PathType Container)) { continue }
        if ([IO.Path]::GetFullPath($srcPluginDir).TrimEnd('\', '/') -ieq [IO.Path]::GetFullPath($resolvedPluginsDir).TrimEnd('\', '/')) { continue }
        Get-ChildItem -LiteralPath $srcPluginDir -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -notmatch '(?i)OptiPatcher.*\.asi$' -and
                $_.Name -notin @('README.txt', 'OptScaler-NR-plugins-README.txt') -and
                $_.Extension -in @('.asi', '.ini', '.txt', '.json')
        } | ForEach-Object {
            $destFile = Join-Path $resolvedPluginsDir $_.Name
            if ($_.Extension -ine '.asi' -and (Test-Path -LiteralPath $destFile)) {
                Write-Host "Keeping existing plugin configuration: $destFile"
                return
            }
            if ([IO.Path]::GetFullPath($destFile).StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                $rel = [IO.Path]::GetFullPath($destFile).Substring($gameFullPrefix.Length)
                Install-One $_.FullName $rel
            } else {
                Copy-Item -LiteralPath $_.FullName -Destination $destFile -Force
            }
        }
    }
} catch {
    Write-Host "NOTE: could not initialize plugins folder: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Uninstaller is copied into the game folder. Double-click it there; it
# targets that directory (no folder picker). Sources were resolved before install
# and preserved if the automatic uninstall could remove them.
Install-One $ps1Src 'Uninstall_OptiScaler_NR.ps1'
$ownedPin = Join-Path $Root 'mochizuki-owned.txt'
if (Test-Path -LiteralPath $ownedPin -PathType Leaf) {
    Install-One $ownedPin 'mochizuki-owned.txt'
}
Install-One $moduleHelperSrc 'lmxxf-module-package.ps1'
if ($batSrc) {
    Install-One $batSrc 'Uninstall_OptiScaler_NR.bat'
} else {
    $batDest = Join-Path $game 'Uninstall_OptiScaler_NR.bat'
    (@'
@echo off
setlocal
title @PRODUCT_TITLE@ - Uninstall
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
'@).Replace('@PRODUCT_TITLE@', $productTitle) | Set-Content -LiteralPath $batDest -Encoding ASCII
    Write-Host "Wrote uninstaller: $batDest" -ForegroundColor Green
}

# Small install record so Uninstall knows which proxy name this install used.
# Uninstall still verifies OptiScaler by VersionInfo; this is a hint, not a trust root.
$installMark = Join-Path $game 'amd-presr-install.txt'
try {
    @(
        'project=OptScaler(NR)',
        ('proxy=' + $Proxy),
        ('backend=' + $selectedActiveBackend),
        ('uninstallFirst=' + $uninstallFirst.ToString().ToLowerInvariant()),
        ('installed=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')),
        ('game=' + $game)
    ) -join "`r`n" | Set-Content -LiteralPath $installMark -Encoding ASCII
    Write-Host "Wrote install record: $installMark" -ForegroundColor Green
} catch {
    Write-Host "NOTE: could not write install record: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Cache game models even when their backend was not selected for this install.
# Publish only complete copies; never merge into or replace an existing package model.
function Save-GameModelForNextInstall([ValidateSet('lmxxf', 'mochizuki', 'mochizuki-small',
        'mochizuki-experimental', 'mochizuki-int4')][string]$Kind) {
    $stage = $null
    try {
        $packageRoot = [IO.Path]::GetFullPath($Root)
        $gameRoot = [IO.Path]::GetFullPath($game)
        if ($packageRoot.TrimEnd('\', '/') -ieq $gameRoot.TrimEnd('\', '/')) { return }
        if ($Kind -eq 'lmxxf') {
            if (-not $lmxxfModelHelper) { return }
            foreach ($model in @('original','small')) {
                $relative = if ($model -eq 'original') { 'native-game-tiled-assets' } else { 'native-game-tiled-assets/small' }
                $source = Join-Path $gameRoot $relative; $destination = Join-Path $packageRoot $relative
                if ((Test-LmxxfModel $source $model) -and -not (Test-LmxxfModel $destination $model)) {
                    Copy-LmxxfModel -Source $source -Destination $destination -Model $model
                    Write-Host "Cached lmxxf $model weights: $destination"
                }
            }
            return
        }
        $relative = switch ($Kind) {
            'mochizuki-small' { 'dlssnr-amd/dlssnr-small.bin' }
            'mochizuki-experimental' { 'dlssnr-amd/dlssnr-small-experimental.bin' }
            'mochizuki-int4' { 'dlssnr-amd/dlssnr-int4.bin' }
            default { 'dlssnr-amd/dlssnr.bin' }
        }
        $needsHeader = $Kind -in @('mochizuki', 'mochizuki-small')
        $entries = if ($Kind -eq 'mochizuki-small') { 255 } else { 599 }
        $destination = Join-Path $packageRoot $relative
        if (Test-Path -LiteralPath $destination) { return }
        $source = Join-Path $gameRoot $relative
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { return }
        $source = Assert-LmxxfUnlinkedPath $source
        $destination = Assert-LmxxfUnlinkedPath $destination
        $packageRoot = Assert-LmxxfUnlinkedPath $packageRoot
        if ($needsHeader -and -not (Test-MochizukiModelHeader $source $entries)) { return }
        $files = @(Get-Item -LiteralPath $source -ErrorAction Stop)
        $stage = Join-Path $packageRoot ('.amd-presr-model-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($stage)
        $payload = Join-Path $stage 'model'
        foreach ($file in $files) {
            $copy = $payload
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copy))
            $hash = Get-Sha256 $file.FullName
            Copy-Item -LiteralPath $file.FullName -Destination $copy -ErrorAction Stop
            if ((Get-Sha256 $copy) -ne $hash) { throw "Model copy checksum mismatch: $($file.FullName)" }
        }
        if ($needsHeader -and -not (Test-MochizukiModelHeader $payload $entries)) {
            throw 'Copied Mochizuki model has an invalid header.'
        }
        if (-not (Get-Item -LiteralPath $payload).Length) { throw 'Copied model is empty.' }
        $destination = Assert-LmxxfUnlinkedPath $destination
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        # The two-argument Move APIs fail if the destination appeared during copying.
        [IO.File]::Move($payload, $destination)
        Write-Host "Saved $relative in the package folder for the next install: $destination" -ForegroundColor Green
    } catch {
        Write-Host "NOTE: could not save $Kind game model for the next install: $($_.Exception.Message)" -ForegroundColor Yellow
    } finally {
        if ($stage -and (Test-Path -LiteralPath $stage)) {
            try {
                $safeStage = Assert-LmxxfUnlinkedPath $stage
                if ([IO.Path]::GetDirectoryName($safeStage) -ine $packageRoot -or
                    [IO.Path]::GetFileName($safeStage) -notmatch '^\.amd-presr-model-[0-9a-f]{32}$') {
                    throw "Unexpected model staging path: $safeStage"
                }
                $null = @(Get-LmxxfUnlinkedFiles $safeStage)
                Remove-Item -LiteralPath $safeStage -Recurse -Force -ErrorAction Stop
            } catch {
                Write-Host "NOTE: model staging cleanup failed: $($_.Exception.Message)" -ForegroundColor Yellow
            }
        }
    }
}
Save-GameModelForNextInstall 'lmxxf'
Save-GameModelForNextInstall 'mochizuki'
Save-GameModelForNextInstall 'mochizuki-small'
Save-GameModelForNextInstall 'mochizuki-experimental'
Save-GameModelForNextInstall 'mochizuki-int4'

# Keep reusable danielblnc files in the package folder for the next game.
# Never write them into the game folder — that would re-inject danielblnc version.dll next to OptiScaler.
# Summary must report what is truly kept in the package directory, not $srcA.
# When package directory is the game directory, $srcA points to a TEMP file deleted below.
# When package directory is not the game directory, the kept file is $Root\version.dll.
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
        if ([IO.Path]::GetDirectoryName($stagedFull) -ieq [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\', '/') -and
            [IO.Path]::GetFileName($stagedFull) -match '^amd-presr-version-[0-9a-f]{32}\.dll$') {
            try { Remove-Item -LiteralPath $stagedA -Force } catch { }
        }
    }
} catch {
    Write-Host "NOTE: could not copy danielblnc files into the package folder: $($_.Exception.Message)" -ForegroundColor Yellow
}

Write-Host ''
# All selected backends have been installed. No permanent backup of these
# retired artifacts; a clean reinstall may already have removed them.
foreach ($path in $legacyCleanupFiles) {
    $safePath = Assert-LmxxfUnlinkedPath $path
    if (Test-Path -LiteralPath $safePath -PathType Leaf) {
        Remove-Item -LiteralPath $safePath -Force -ErrorAction Stop
        Write-Host "Removed legacy NR artifact: $safePath"
    }
}
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  Game:           $game"
Write-Host "  Proxy:          $Proxy"
if (Test-Path -LiteralPath $backup) {
    Write-Host "  Backup:         $backup"
}
Write-Host "  Active Backend: $selectedActiveBackend" -ForegroundColor Cyan
if ($installLmxxf) {
    Write-Host "  Installed:      OptScaler(NR) + lmxxf runtime (LmxxfNrRuntime.dll, modules, shaders)" -ForegroundColor Green
    Write-Host "  lmxxf models:   $lmxxfSelectedModels"
}
if ($installDaniel) {
    Write-Host "  Installed:      OptScaler(NR) + danielblnc runtime (dlssnr_amd_pass1-3.dll + weights)" -ForegroundColor Green
}
if ($installMochizuki) {
    Write-Host "  Installed:      OptScaler(NR) + Mochizuki NR ASI (OptiScaler/plugins/dlssnr-amd.asi, resources)" -ForegroundColor Green
    $activeModelStr = if ($targetNetworkModel -eq 1) { 'Small' } else { 'Original' }
    Write-Host "  Mochizuki model: $MochizukiModels (active: $activeModelStr)" -ForegroundColor Green
}
if ($installedOptiPatcher) {
    Write-Host "  OptiPatcher:    Installed (DLSS spoofing enhancement, LoadAsiPlugins=true)" -ForegroundColor Green
}
if ($installDaniel) {
    if ($keptA -or $keptW) {
        Write-Host "  Package keeps:  $keptA"
        if ($keptW) { Write-Host "                  $keptW" }
    } else {
        Write-Host '  Package keeps:  (nothing — the package folder is the game folder)'
    }
}
Write-Host ''
Write-Host 'Next (in game):' -ForegroundColor Yellow
Write-Host '  1. Launch the game'
Write-Host '  2. Press Insert (Ins) to open the OptiScaler menu'
Write-Host '  3. Ensure DLSSNR is enabled'
if (@($installDaniel, $installLmxxf, $installMochizuki | Where-Object { $_ }).Count -gt 1) {
    Write-Host '  4. Select the backend in Ins; with hot switching disabled, save and restart the game.'
}
Write-Host ''
if ($installMochizuki) { Write-Host "Mochizuki NR is the official ASI plugin: press End in game for NR Control. Built-in NR controls stay disabled while it is loaded." }
if ($weightProblems.Count -gt 0) {
    Write-Host ''
    foreach ($p in $weightProblems) {
        $state = if ($p.Failed) { 'FAILED, not installed' } else { 'warning' }
        Write-Host ("WEIGHTS CHECK {0}: {1} - {2}" -f $p.Backend, $state, ($p.En | Where-Object { $_ -match 'Wrong (file|folder)' } | Select-Object -First 1).Trim()) -ForegroundColor Red
    }
}
$activeNeedsSmall = if ($mochizukiActiveNetwork) { $mochizukiActiveNetwork -in @('small', 'experimental') } else { $targetNetworkModel -eq 1 }
$mochizukiActiveValid = if ($activeNeedsSmall) { (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr-small.bin') -PathType Leaf) } else { (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/dlssnr.bin') -PathType Leaf) }
$activeModelPending = ($selectedActiveBackend -eq 'mochizuki' -and -not $mochizukiActiveValid) -or
    ($selectedActiveBackend -eq 'lmxxf' -and $lmxxfSelectedModels -eq 'none')
if ($activeModelPending) {
    Write-Host "Installation completed, but $selectedActiveBackend cannot run yet: MODEL SETUP REQUIRED. Follow the model instructions above, then restart the game." -ForegroundColor Yellow
} else { Write-Host 'Install SUCCEEDED. Verify backend status in Ins when the game starts.' -ForegroundColor Green }
Pause-Exit 0
