<#
.SYNOPSIS
  Install official OptiScaler together with dlssnr-amd.asi and openrr-amd.asi into a game folder.
  Run OptiScalerNRLauncher.exe, or pass -GameDir to Setup.ps1.
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',
    [ValidateSet('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll','version.dll')]
    [string]$Proxy = 'dxgi.dll',
    [string]$Root,
    [ValidateSet("auto", "mochizuki", "all", "none")]
    [string]$Backend = "auto",
    [ValidateSet('both', 'all', 'small', 'experimental', 'original', 'int4', 'optimized')]
    [string]$MochizukiModels,
    [string[]]$MochizukiNetworks,
    [ValidateSet('small', 'experimental', 'original', 'int4', 'optimized')]
    [string]$MochizukiActiveModel,
    [string[]]$Backends,
    [string]$ActiveBackend,
    [switch]$NonInteractive,
    [switch]$UninstallExisting,
    [switch]$NoPause,
    [ValidateSet('full', 'auto')]
    [string]$OpenRrSpoof = 'full',
    [string]$DecisionJson
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding

# Purge legacy model backups from %LocalAppData% (models belong only in the launcher/game folders).
try {
    $localApp = [Environment]::GetFolderPath('LocalApplicationData')
    if ($localApp) {
        foreach ($legacyDir in @(
            (Join-Path $localApp 'OptScalerNR\ModelStore'),
            (Join-Path $localApp 'OptScalerNR\Launcher\models')
        )) {
            if (Test-Path -LiteralPath $legacyDir -PathType Container) {
                Remove-Item -LiteralPath $legacyDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
} catch { }
$setupSourceStage = $null
$uninstallStarted = $false
$mochizukiModelStage = $null

function Pause-Exit([int]$code) {
    if ($mochizukiModelStage -and (Test-Path -LiteralPath $mochizukiModelStage)) {
        try { Remove-Item -LiteralPath $mochizukiModelStage -Recurse -Force -ErrorAction Stop }
        catch { Write-Warning "Mochizuki model staging cleanup failed: $($_.Exception.Message)" }
    }
    if ($setupSourceStage -and $uninstallStarted -and $code -ne 0) {
        Write-Warning "Reinstall inputs kept at $setupSourceStage."
    } elseif ($setupSourceStage) {
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

trap { Fail ("Unexpected install error: " + $_.Exception.Message) }

$launcherDecisions = $null
if ($DecisionJson) {
    if (-not [IO.Path]::IsPathRooted($DecisionJson)) { Fail 'DecisionJson must be an absolute path.' }
    $launcherDecisions = Get-Content -LiteralPath $DecisionJson -Raw -Encoding UTF8 | ConvertFrom-Json
}
$hasBackendSelection = $PSBoundParameters.ContainsKey('Backends')
$hasOpenRrSpoofSelection = $PSBoundParameters.ContainsKey('OpenRrSpoof')
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
        if ($MochizukiActiveModel -notin @('small','experimental','original','int4','optimized')) {
            Fail 'DecisionJson mochizukiActiveModel must be small, experimental, original, int4, or optimized.'
        }
    }
    if (-not $PSBoundParameters.ContainsKey('MochizukiModels') -and $launcherDecisions.mochizukiModels) {
        $MochizukiModels = [string]$launcherDecisions.mochizukiModels
        if ($MochizukiModels -notin @('both','all','small','experimental','original','int4','optimized')) {
            Fail 'DecisionJson mochizukiModels must be both, all, small, experimental, original, int4, or optimized.'
        }
    }
    if (-not $PSBoundParameters.ContainsKey('OpenRrSpoof') -and
        $launcherDecisions.PSObject.Properties['openRrSpoof'] -and $launcherDecisions.openRrSpoof) {
        $OpenRrSpoof = ([string]$launcherDecisions.openRrSpoof).Trim().ToLowerInvariant()
        $hasOpenRrSpoofSelection = $true
        if ($OpenRrSpoof -notin @('full', 'auto')) {
            Fail 'DecisionJson openRrSpoof must be full or auto.'
        }
    }
}

if ($PSBoundParameters.ContainsKey('ActiveBackend')) {
    if (-not $hasBackendSelection) { Fail 'ActiveBackend requires an explicit -Backends list (or DecisionJson backends).' }
    if ($ActiveBackend -notin @('mochizuki','none')) { Fail 'ActiveBackend must be mochizuki or none.' }
}

function Assert-LmxxfUnlinkedPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -gt [IO.Path]::GetPathRoot($full).Length) { $full = $full.TrimEnd('\', '/') }
    $cursor = $full
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing linked module path (symlink/junction/reparse point): $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\', '/'))
    }
    return $full
}

function Get-LmxxfUnlinkedFiles([string]$Directory) {
    $full = Assert-LmxxfUnlinkedPath $Directory
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Missing module directory: $full" }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($full)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing linked module path (symlink/junction/reparse point): $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $item }
        }
    }
}
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

$productTitle = 'OptiScaler(NR)'
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
# The install record remembers the last OpenRR spoof policy (written only while OpenRR is installed).
$priorOpenRrSpoof = $null
$priorOpenRrInstalled = $false
$priorOpenRrPluginDirs = @((Join-Path $game 'OptiScaler\plugins'), (Join-Path $game 'plugins'))
$priorRecordPath = Join-Path $game 'amd-presr-install.txt'
if (Test-Path -LiteralPath $priorRecordPath -PathType Leaf) {
    foreach ($line in [IO.File]::ReadAllLines($priorRecordPath, [Text.Encoding]::UTF8)) {
        if ($line.TrimStart([char]0xFEFF).Trim() -match '^(?i)openRrSpoof=(full|auto)$') {
            $priorOpenRrSpoof = $Matches[1].ToLowerInvariant()
        }
        if ($line.TrimStart([char]0xFEFF).Trim() -match '^(?i)pluginDir=(.+)$') {
            $priorDir = $Matches[1]
            if (-not [IO.Path]::IsPathRooted($priorDir)) { $priorDir = Join-Path $game $priorDir }
            $priorOpenRrPluginDirs += $priorDir
        }
    }
}
# Capture legacy installation evidence before an uninstall/update removes it.
$priorOpenRrInstalled = [bool]$priorOpenRrSpoof -or (Test-Path -LiteralPath (Join-Path $game 'openrr-amd.ini') -PathType Leaf)
foreach ($priorDir in $priorOpenRrPluginDirs) {
    foreach ($priorName in @('openrr-amd.asi', 'openrr.asi')) {
        if (Test-Path -LiteralPath (Join-Path $priorDir $priorName) -PathType Leaf) { $priorOpenRrInstalled = $true }
    }
}
if (-not $hasOpenRrSpoofSelection -and $priorOpenRrSpoof) { $OpenRrSpoof = $priorOpenRrSpoof }
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
foreach ($name in ($proxies + @('LmxxfNrRuntime.dll', 'OpenNrRrRuntime.dll', 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll', 'dlssnr_amd_pass3.dll', 'dlssnr_on_amd_weights.bin'))) {
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
        $_.Name -notin @('OptiScalerNRLauncher.exe', 'OptScalerNR.Launcher.exe', 'dlssnr_on_amd_setup.exe', 'person-worker.exe') -and
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
    (Join-Path $PSScriptRoot 'uninstall.ps1'),
    (Join-Path $PSScriptRoot 'uninstall-amd-presr.ps1')
)
if (-not $ps1Src) { Fail 'Missing Uninstall_OptiScaler_NR.ps1 next to Setup.ps1.' }
$batSrc = Find-FirstFile @(
    (Join-Path $release 'Uninstall_OptiScaler_NR.bat'),
    (Join-Path $Root 'Uninstall_OptiScaler_NR.bat')
)
$xessCompatSrc = Find-FirstFile @(
    (Join-Path $release 'xess-compat.ps1'),
    (Join-Path $Root 'xess-compat.ps1'),
    (Join-Path $PSScriptRoot 'xess-compat.ps1'),
    (Join-Path $PSScriptRoot 'install/xess-compat.ps1')
)

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
    if ($mochizukiAssets) {
        foreach ($outerNra in @('aco/d3d12_outer.nra', 'aco/rdna3/d3d12_outer.nra')) {
            if (-not (Test-Path -LiteralPath (Join-Path $mochizukiAssets $outerNra) -PathType Leaf)) {
                Fail "Incomplete Mochizuki ASI package: missing dlssnr-amd/$outerNra. Re-extract the complete update."
            }
        }
    }
}
$installMochizuki = $false
$selectedActiveBackend = 'mochizuki'
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
$modelCandidates = @(
    (Join-Path $Root 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'),
    (Join-Path $Root 'CC_SILVER_AARDWOLD.bin'),
    (Join-Path $Root 'dlssnr-amd/dlssnr.bin'),
    (Join-Path $Root 'dlssnr.bin')
)
if ($mochizukiAssets) {
    $modelCandidates += (Join-Path $mochizukiAssets 'CC_SILVER_AARDWOLD.bin')
    $modelCandidates += (Join-Path $mochizukiAssets 'dlssnr.bin')
}
$modelCandidates += (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin')
$modelCandidates += (Join-Path $game 'dlssnr-amd/dlssnr.bin')
foreach ($candidate in $modelCandidates) {
    # Presence only: the ASI owns the model format.
    if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Item -LiteralPath $candidate).Length -gt 0) {
        $mochizukiModel = $candidate; break
    }
}

$mochizukiSmallModel = $null
$smallModelCandidates = @(
    (Join-Path $Root 'dlssnr-amd/CC_SMALL_FP8.bin'),
    (Join-Path $Root 'CC_SMALL_FP8.bin'),
    (Join-Path $Root 'dlssnr-amd/dlssnr-small.bin'),
    (Join-Path $Root 'dlssnr-small.bin')
)
if ($mochizukiAssets) {
    $smallModelCandidates += (Join-Path $mochizukiAssets 'CC_SMALL_FP8.bin')
    $smallModelCandidates += (Join-Path $mochizukiAssets 'dlssnr-small.bin')
}
$smallModelCandidates += (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin')
$smallModelCandidates += (Join-Path $game 'dlssnr-amd/dlssnr-small.bin')
foreach ($candidate in $smallModelCandidates) {
    if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Item -LiteralPath $candidate).Length -gt 0) {
        $mochizukiSmallModel = $candidate; break
    }
}
$mochizukiNewNvSha = '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f'
$mochizukiNvSha = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
Write-Host ''
Write-Host ("Detected Mochizuki NR ASI: ASI/resources={0}; model={1}" -f $canMochizuki, $(if ($mochizukiModel -or $mochizukiSmallModel) { 'present' } else { 'not extracted yet' })) -ForegroundColor Cyan

if ($hasBackendSelection) {
    $seenBackend = @{}
    foreach ($id in @($Backends)) { $seenBackend[$id] = $true }
    if ($seenBackend.ContainsKey('mochizuki') -and -not $canMochizuki) {
        Fail 'Mochizuki ASI payload is missing from the package.'
    }
    $installMochizuki = [bool]$seenBackend.ContainsKey('mochizuki')
    $selectedActiveBackend = if ($installMochizuki) { 'mochizuki' } else { 'none' }
} elseif ($Backend -ieq 'none') {
    $installMochizuki = $false
    $selectedActiveBackend = 'none'
} else {
    if (-not $canMochizuki) { Fail 'Mochizuki ASI payload is missing from the package.' }
    $installMochizuki = $true
    $selectedActiveBackend = 'mochizuki'
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
        $badNetworks = @($MochizukiNetworks | Where-Object { $_ -notin @('small', 'experimental', 'original', 'int4', 'optimized') })
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
                Write-Host '  5. CC_SILVER_AARDWOLD_OPTIMIZED (made from CC_SILVER_AARDWOLD)'
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

    # Experimental is built from the small network; Optimized / INT4 from the original.
    if ($networksExplicit) {
        $wantSmall = @($MochizukiNetworks | Where-Object { $_ -in @('small', 'experimental') }).Count -gt 0
        $wantOriginal = @($MochizukiNetworks | Where-Object { $_ -in @('original', 'int4', 'optimized') }).Count -gt 0
        $generateExperimental = $MochizukiNetworks -contains 'experimental'
        $generateInt4 = ($MochizukiNetworks -contains 'int4') -or ($MochizukiNetworks -contains 'optimized')
        if ($MochizukiActiveModel -and $MochizukiNetworks -notcontains $MochizukiActiveModel) {
            Fail "Conflict: -MochizukiActiveModel '$MochizukiActiveModel' is not in the selected install range."
        }
    } else {
        $wantSmall = $MochizukiModels -in @('both', 'all', 'small', 'experimental')
        $wantOriginal = $MochizukiModels -in @('both', 'all', 'original', 'int4', 'optimized')
        $generateExperimental = $MochizukiModels -in @('both', 'all', 'experimental')
        $generateInt4 = $MochizukiModels -in @('both', 'all', 'int4', 'optimized')
        if ($MochizukiActiveModel) {
            $activeNeedsSmall = $MochizukiActiveModel -in @('small', 'experimental')
            $activeNeedsOriginal = $MochizukiActiveModel -in @('original', 'int4', 'optimized')
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
        $extractedOriginal = Join-Path $modelOutput 'CC_SILVER_AARDWOLD.bin'
        $extractedSmall = Join-Path $modelOutput 'CC_SMALL_FP8.bin'
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
                Fail 'Model preparation failed: the extractor did not produce CC_SILVER_AARDWOLD.bin. The existing installation was preserved.'
            }
            $mochizukiModel = $extractedOriginal
        }
        if ($needExtractSmall) {
            if (-not $gotSmall) {
                Fail 'Model preparation failed: this DLL has no small model (CC_SMALL_FP8.bin); use the 310.9.16 DLL for both models, or install the original model only.'
            }
            $mochizukiSmallModel = $extractedSmall
        }
        # Keep the models in the package for later installs when that folder is writable.
        # A failed cache copy must not delete the staged file the install still needs.
        foreach ($pair in @(@($extractedOriginal, 'CC_SILVER_AARDWOLD.bin'), @($extractedSmall, 'CC_SMALL_FP8.bin'))) {
            if (-not (Test-Path -LiteralPath $pair[0] -PathType Leaf)) { continue }
            $target = Join-Path $mochizukiAssets $pair[1]
            try {
                Copy-Item -LiteralPath $pair[0] -Destination $target -Force -ErrorAction Stop
                if ($pair[1] -eq 'CC_SILVER_AARDWOLD.bin') { $mochizukiModel = $target } else { $mochizukiSmallModel = $target }
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
    if ($MochizukiActiveModel -in @('original', 'int4', 'optimized') -and -not $mochizukiModel) {
        Fail 'Conflict: the selected ASI network needs the original model, which this source does not provide.'
    }
}
if ($installMochizuki) {
    $selectedModelNames = @()
    if ($wantOriginal) { $selectedModelNames += 'CC_SILVER_AARDWOLD.bin' }
    if ($wantSmall) { $selectedModelNames += 'CC_SMALL_FP8.bin' }
    foreach ($modelName in $selectedModelNames) {
        $modelTarget = Join-Path $game (Join-Path 'dlssnr-amd' $modelName)
        if (Test-FileLocked $modelTarget) {
            Fail "Model destination is locked: $modelTarget. Close the game completely before running Setup again."
        }
    }
}
$wantOpenRr = (-not $launcherDecisions) -or (-not $launcherDecisions.PSObject.Properties['openRr']) -or ([string]$launcherDecisions.openRr -ne 'skip')
$openRrExplicit = $launcherDecisions -and $launcherDecisions.PSObject.Properties['openRr'] -and ([string]$launcherDecisions.openRr -eq 'install')
$userDlssd = $null
if ($wantOpenRr) {
    foreach ($cand in @(
        (Join-Path $Root 'nvngx_dlssd.dll'),
        (Join-Path $release 'nvngx_dlssd.dll'),
        (Join-Path $Root 'openrr-amd\nvngx_dlssd.dll'),
        (Join-Path $Root 'opennr-rr\nvngx_dlssd.dll'),
        (Join-Path $game 'openrr-amd\nvngx_dlssd.dll'),
        (Join-Path $game 'opennr-rr\nvngx_dlssd.dll'),
        (Join-Path $game 'nvngx_dlssd.dll')
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            try {
                $v = (Get-Item -LiteralPath $cand).VersionInfo
                if ($v.FileMajorPart -eq 310 -and $v.FileMinorPart -eq 9 -and $v.FileBuildPart -eq 1) {
                    $userDlssd = $cand
                    break
                }
            } catch { }
        }
    }
    if (-not $userDlssd) {
        if ($openRrExplicit) {
            Fail 'OpenRR model preparation failed: put nvngx_dlssd.dll (310.9.1.0) beside Setup.bat (or import it in the Launcher), or uncheck OpenRR.'
        } elseif ($Backend -ieq 'none' -or (-not $installMochizuki)) {
            $wantOpenRr = $false
            Write-Host 'Skipping OpenRR ASI plugin (nvngx_dlssd.dll 310.9.1.0 is not present).' -ForegroundColor Cyan
        } else {
            $wantOpenRr = $false
            Write-Host 'Skipping OpenRR ASI plugin (nvngx_dlssd.dll 310.9.1.0 is not present; put nvngx_dlssd.dll 310.9.1.0 beside Setup.bat to enable OpenRR).' -ForegroundColor Cyan
        }
    }
}
$openRrShadersSrc = $null
$openRrShadersGfx11Src = $null
$openRrAssetsSrc = $null
if ($wantOpenRr) {
    $openRrAssetsSrc = @((Join-Path $release 'openrr-amd'), (Join-Path $Root 'openrr-amd')) |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'shaders') -PathType Container } |
        Select-Object -First 1
    if ($openRrAssetsSrc) {
        foreach ($reqRr in @(
            'aco\gfx1201\openrr-d3d12-v1\manifest.msgpack',
            'aco\gfx1200\openrr-d3d12-v1\manifest.msgpack'
        )) {
            if (-not (Test-Path -LiteralPath (Join-Path $openRrAssetsSrc $reqRr) -PathType Leaf)) {
                Fail "Incomplete OpenRR ASI package: missing openrr-amd\$reqRr. Re-extract the complete update."
            }
        }
        $openRrShadersSrc = Join-Path $openRrAssetsSrc 'shaders'
        $gfx11Cand = Join-Path $openRrAssetsSrc 'shaders-gfx11'
        if (Test-Path -LiteralPath $gfx11Cand -PathType Container) { $openRrShadersGfx11Src = $gfx11Cand }
    } else {
        $openRrShadersSrc = @(
            (Join-Path $release 'openrr-amd\shaders'), (Join-Path $Root 'openrr-amd\shaders'),
            (Join-Path $release 'opennr-rr\shaders'), (Join-Path $Root 'opennr-rr\shaders')
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -First 1
        $openRrShadersGfx11Src = @(
            (Join-Path $release 'openrr-amd\shaders-gfx11'), (Join-Path $Root 'openrr-amd\shaders-gfx11'),
            (Join-Path $release 'opennr-rr\shaders-gfx11'), (Join-Path $Root 'opennr-rr\shaders-gfx11')
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -First 1
    }
}
if ($uninstallFirst) {
    try {
        $optiSource = Save-ReinstallSource (Join-Path $release 'OptiScaler.dll')
        foreach ($name in @('OptiScaler.ini', 'OptiScaler', 'dlssnr-amd-optiscaler-renodx.ini', 'dlssnr-amd-optiscaler.ini', 'openrr-amd.ini')) {
            $path = Join-Path $release $name
            if (Test-Path -LiteralPath $path) { $null = Save-ReinstallSource $path }
        }
        if ($openRrAssetsSrc) {
            $openRrAssetsSrc = Save-ReinstallSource $openRrAssetsSrc
            $openRrShadersSrc = Join-Path $openRrAssetsSrc 'shaders'
            $gfx11Saved = Join-Path $openRrAssetsSrc 'shaders-gfx11'
            $openRrShadersGfx11Src = if (Test-Path -LiteralPath $gfx11Saved -PathType Container) { $gfx11Saved } else { $null }
        } else {
            $openRrShadersSrc = Save-ReinstallSource $openRrShadersSrc
            $openRrShadersGfx11Src = Save-ReinstallSource $openRrShadersGfx11Src
        }
        $release = [IO.Path]::GetDirectoryName($optiSource)
        if ($installMochizuki) {
            if ($wantOriginal) { $mochizukiModel = Save-ReinstallSource $mochizukiModel }
            if ($wantSmall) { $mochizukiSmallModel = Save-ReinstallSource $mochizukiSmallModel }
            $mochizukiAsi = Save-ReinstallSource $mochizukiAsi
            $mochizukiAssets = Save-ReinstallSource $mochizukiAssets
            $mochizukiTools = Save-ReinstallSource $mochizukiTools
            if ($mochizukiInt4Tool) { $mochizukiInt4Tool = Save-ReinstallSource $mochizukiInt4Tool }
            if ($mochizukiProbe) { $mochizukiProbe = Save-ReinstallSource $mochizukiProbe }
        }
        if ($wantOpenRr -and $userDlssd) { $userDlssd = Save-ReinstallSource $userDlssd }
        $ps1Src = Save-ReinstallSource $ps1Src
        $batSrc = Save-ReinstallSource $batSrc
        if ($xessCompatSrc) { $xessCompatSrc = Save-ReinstallSource $xessCompatSrc }
    } catch { Fail ("Could not preserve installation sources before uninstall: " + $_.Exception.Message) }

    Write-Host ''
    Write-Host 'Uninstalling the existing OptiScaler installation...' -ForegroundColor Cyan
    Write-Host ("Uninstall script: {0}" -f $ps1Src)
    # A child process isolates the uninstaller's exit statement. Y already grants
    # consent: no second confirmation, backup deletion, or intermediate pause.
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $uninstallStarted = $true
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $ps1Src -GameDir $game -NonInteractive -NoPause
    if ($LASTEXITCODE -ne 0) { Fail "Uninstall failed (exit code $LASTEXITCODE). Installation stopped; fix the reported error and run Setup again." }
    Write-Host 'Uninstall completed. Continuing installation...' -ForegroundColor Green
}

# --- inspect common injection DLLs after the optional uninstall ---
$priorExternalPlugins = @{}
$priorInstallMark = Join-Path $game 'amd-presr-install.txt'
$gameRootPrefixForExt = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
if (Test-Path -LiteralPath $priorInstallMark -PathType Leaf) {
    try {
        foreach ($line in [IO.File]::ReadAllLines($priorInstallMark, [Text.Encoding]::UTF8)) {
            $cleanLine = $line.TrimStart([char]0xFEFF).Trim()
            if ($cleanLine -match '^(?i)extPlugin=([a-f0-9]{64})\|(.+)$') {
                $extSha = $Matches[1].ToLowerInvariant()
                $extPath = [IO.Path]::GetFullPath($Matches[2].Trim())
                if ([IO.Path]::GetFileName($extPath) -in @('OptiPatcher.asi', 'dlssnr-amd.asi', 'openrr-amd.asi', 'openrr.asi', 'openrr-ngx-host.dll') -and
                    -not $extPath.StartsWith($gameRootPrefixForExt, [StringComparison]::OrdinalIgnoreCase)) {
                    $priorExternalPlugins[$extPath] = $extSha
                }
            }
        }
    } catch { $priorExternalPlugins = @{} }
}
function Remove-PluginFileChecked([string]$pluginPath) {
    if (-not $pluginPath -or -not (Test-Path -LiteralPath $pluginPath -PathType Leaf)) { return $false }
    $full = [IO.Path]::GetFullPath($pluginPath)
    if ($full.StartsWith($gameRootPrefixForExt, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
        return $true
    }
    try {
        $safeFull = Assert-LmxxfUnlinkedPath $full
        $expectedSha = $null
        foreach ($k in $priorExternalPlugins.Keys) {
            if ($k -ieq $safeFull) { $expectedSha = $priorExternalPlugins[$k]; break }
        }
        $actualSha = (Get-Sha256 $safeFull).ToLowerInvariant()
        if ($expectedSha -and $actualSha -eq $expectedSha) {
            Remove-Item -LiteralPath $safeFull -Force -ErrorAction SilentlyContinue
            [void]$priorExternalPlugins.Remove($safeFull)
            return $true
        } else {
            Write-Host "Kept unowned or modified external plugin: $safeFull" -ForegroundColor Cyan
            return $false
        }
    } catch {
        return $false
    }
}

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
    $isMochizukiModel = ($rel -in @('dlssnr-amd/CC_SILVER_AARDWOLD.bin', 'dlssnr-amd/CC_SMALL_FP8.bin'))
    $isWeight = ($rel -ieq 'dlssnr_on_amd_weights.bin') -or
                ($rel -ilike 'native-game-tiled-assets\*') -or
                ($rel -ilike 'native-game-tiled-assets/*') -or $isMochizukiModel

    $aside = $null
    try {
        if (Test-Path -LiteralPath $dest) {
            if ($isMochizukiModel) {
                $expectedEntries = if ($rel -eq 'dlssnr-amd/CC_SMALL_FP8.bin') { 255 } else { 599 }
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
    $rawBytes = [System.IO.File]::ReadAllBytes($iniPath)
    $hasBom = ($rawBytes.Length -ge 3 -and $rawBytes[0] -eq 0xEF -and $rawBytes[1] -eq 0xBB -and $rawBytes[2] -eq 0xBF)
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
    [System.IO.File]::WriteAllLines($iniPath, $lines, [System.Text.UTF8Encoding]::new($hasBom))
}

# OpenRR's policy choice is the user's authorization for these keys, including in a kept INI.
# full forces spoofing on; auto (and disabling OpenRR) leaves the decision to OptiScaler/OptiPatcher.
function Set-OpenRrSpoof([string]$iniPath, [string]$value) {
    if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) { return }
    Set-IniSettings $iniPath 'Spoofing' ([ordered]@{
        'Dxgi'                    = $value
        'Vulkan'                  = $value
        'VulkanExtensionSpoofing' = $value
    })
    Write-Host "Set [Spoofing] Dxgi, Vulkan, VulkanExtensionSpoofing = $value in OptiScaler.ini (OpenRR)." -ForegroundColor Green
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
$anyExistingIni = (Test-Path -LiteralPath $gameIni -PathType Leaf) -or
    (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd.ini') -PathType Leaf) -or
    (Test-Path -LiteralPath (Join-Path $game 'openrr-amd.ini') -PathType Leaf)
$overwriteIni = $false
if ($lmxxfIniAction) {
    $overwriteIni = ($lmxxfIniAction -eq 'overwrite')
} elseif ($launcherDecisions -and $launcherDecisions.existingIni) {
    if ($launcherDecisions.existingIni -eq 'overwrite') { $overwriteIni = $true }
    elseif ($launcherDecisions.existingIni -eq 'keep') { $overwriteIni = $false }
    else { Fail 'DecisionJson existingIni must be overwrite or keep.' }
} elseif ($anyExistingIni) {
    # Recommended on a large version jump. -NonInteractive takes that option.
    if ($NonInteractive) {
        $overwriteIni = $true
    } else {
        $iniChoice = Ask-Choice 'Configuration files (.ini) already exist in the game folder.' @(
            'Overwrite existing .ini files (Recommended: major version update!)'
            'Keep the existing .ini files'
        )
        $overwriteIni = ($iniChoice -eq 1)
    }
}
if (Test-Path -LiteralPath $ini -PathType Leaf) {
    if (-not (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
        Install-One $ini 'OptiScaler.ini'
        if (-not $keep.ContainsKey('OptiScaler.ini')) { $iniFromTemplate = $true }
    } elseif ($overwriteIni) {
        Install-One $ini 'OptiScaler.ini'
        if (-not $keep.ContainsKey('OptiScaler.ini')) { $iniFromTemplate = $true }
        Write-Host 'Replaced OptiScaler.ini with the package file.' -ForegroundColor Cyan
    } else {
        Write-Host 'Keeping the existing OptiScaler.ini.' -ForegroundColor Cyan
    }
}
# Apply the launcher's UI language only to a template that this run created or
# overwrote. A kept user ini keeps its own [Menu] Language.
$uiLanguage = $null
$asiMenuLanguage = $null
if ($launcherDecisions -and $launcherDecisions.PSObject.Properties['language']) {
    if ($launcherDecisions.language -eq 'zh') { $uiLanguage = 'zh-CN'; $asiMenuLanguage = 'zh-CN' }
    elseif ($launcherDecisions.language -eq 'en') { $uiLanguage = 'en'; $asiMenuLanguage = 'English' }
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

# Purge retired lmxxf / danielblnc runtime files from the game folder during install/upgrade
foreach ($retiredFile in @('LmxxfNrRuntime.dll', 'MochizukiNrRuntime.dll', 'OpenNrRrRuntime.dll', 'opennr_rr.dll', 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll', 'dlssnr_amd_pass3.dll', 'lmxxf-module-package.ps1')) {
    $rp = Join-Path $game $retiredFile
    if (Test-Path -LiteralPath $rp -PathType Leaf) {
        Remove-Item -LiteralPath $rp -Force -ErrorAction SilentlyContinue
        Write-Host "Removed retired runtime file: $retiredFile" -ForegroundColor DarkGray
    }
}
foreach ($retiredDir in @('lmxxf-modules', 'DLSS5-AMD')) {
    $rd = Join-Path $game $retiredDir
    if ((Test-Path -LiteralPath $rd -PathType Container) -and -not ((Get-Item -LiteralPath $rd -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $rd -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Removed retired directory: $retiredDir" -ForegroundColor DarkGray
    }
}
# Build CC_SMALL_FP8_EXPERIMENTAL or CC_SILVER_AARDWOLD_OPTIMIZED from its base pack and the vendored delta.
# Supports added entries ("performance/<name>") for CC_SILVER_AARDWOLD_OPTIMIZED's levels in one pack.
function New-MochizukiExperimentalWeights([string]$Small, [string]$Delta, [string]$Output) {
    $pack = [IO.File]::ReadAllBytes($Small)
    if ($pack.Length -lt 16 -or [Text.Encoding]::ASCII.GetString($pack, 0, 8) -ne 'NRMODEL1') {
        throw "$Small is not an NRMODEL1 model pack"
    }
    $countPack = [BitConverter]::ToUInt32($pack, 8)
    $index = @{}; $at = 16
    for ($k = 0; $k -lt $countPack; $k++) {
        if ($at + 4 -gt $pack.Length) { throw "${Small}: truncated entry" }
        $n = [BitConverter]::ToUInt32($pack, $at)
        if ($at + 4 + $n + 16 -gt $pack.Length) { throw "${Small}: truncated entry name" }
        $name = [Text.Encoding]::UTF8.GetString($pack, $at + 4, $n)
        $index[$name] = @([BitConverter]::ToUInt64($pack, $at + 4 + $n), [BitConverter]::ToUInt64($pack, $at + 12 + $n))
        $at += 4 + $n + 16
    }
    $indexEnd = $at
    $d = [IO.File]::ReadAllBytes($Delta)
    if ($d.Length -lt 12 -or [Text.Encoding]::ASCII.GetString($d, 0, 8) -ne 'NRXDIFF1') {
        throw "$Delta is not an NRXDIFF1 weights difference"
    }
    $count = [BitConverter]::ToUInt32($d, 8)
    $orig = $null; $at = 12
    for ($k = 0; $k -lt $count; $k++) {
        if ($at + 4 -gt $d.Length) { throw "${Delta}: truncated entry" }
        $n = [BitConverter]::ToUInt32($d, $at)
        if ($at + 4 + $n + 8 -gt $d.Length) { throw "${Delta}: truncated entry name" }
        if (-not $index.ContainsKey([Text.Encoding]::UTF8.GetString($d, $at + 4, $n))) { $orig = [byte[]]$pack.Clone(); break }
        $at += 4 + $n + 8 + [int64][BitConverter]::ToUInt64($d, $at + 4 + $n)
    }
    $added = New-Object System.Collections.Generic.List[object]
    $at = 12
    for ($k = 0; $k -lt $count; $k++) {
        if ($at + 4 -gt $d.Length) { throw "${Delta}: truncated entry" }
        $n = [BitConverter]::ToUInt32($d, $at)
        if ($at + 4 + $n + 8 -gt $d.Length) { throw "${Delta}: truncated entry name" }
        $name = [Text.Encoding]::UTF8.GetString($d, $at + 4, $n)
        $size = [BitConverter]::ToUInt64($d, $at + 4 + $n)
        $at += 4 + $n + 8
        if (-not $index.ContainsKey($name)) {
            $slash = $name.IndexOf('/')
            $base = if ($slash -ge 0) { $name.Substring($slash + 1) } else { '' }
            if (-not $index.ContainsKey($base) -or $index[$base][1] -ne $size) {
                throw "${name}: an added entry without $base of $size bytes in $Small"
            }
            if ($at + $size -gt $d.Length) { throw "${Delta}: truncated data for $name" }
            $boff = [int64]$index[$base][0]
            if ($boff + $size -gt $orig.Length) { throw "${Small}: entry $base exceeds the pack" }
            $bytes = New-Object byte[] $size
            for ([int64]$i = 0; $i -lt $size; $i++) { $bytes[$i] = $orig[$boff + $i] -bxor $d[$at + $i] }
            $added.Add(@($name, $bytes))
        } else {
            if ($index[$name][1] -ne $size) {
                throw "$name is not in $Small with $size bytes"
            }
            if ($at + $size -gt $d.Length) { throw "${Delta}: truncated data for $name" }
            $off = [int64]$index[$name][0]
            if ($off + $size -gt $pack.Length) { throw "${Small}: entry $name exceeds the pack" }
            for ([int64]$i = 0; $i -lt $size; $i++) {
                $pack[$off + $i] = $pack[$off + $i] -bxor $d[$at + $i]
            }
        }
        $at += [int64]$size
    }
    if ($at -ne $d.Length) { throw "$Delta has $($d.Length - $at) bytes left over" }
    if ($added.Count -gt 0) {
        $grow = 0; foreach ($e in $added) { $grow += 4 + [Text.Encoding]::UTF8.GetByteCount($e[0]) + 16 }
        $ms = New-Object System.IO.MemoryStream; $bw = New-Object System.IO.BinaryWriter($ms)
        $bw.Write($pack, 0, 8); $bw.Write([uint32]($countPack + $added.Count)); $bw.Write($pack, 12, 4)
        $at = 16
        for ($k = 0; $k -lt $countPack; $k++) {
            $n = [BitConverter]::ToUInt32($pack, $at)
            $bw.Write($pack, $at, 4 + $n)
            $bw.Write([uint64]([BitConverter]::ToUInt64($pack, $at + 4 + $n) + $grow))
            $bw.Write([uint64]([BitConverter]::ToUInt64($pack, $at + 12 + $n)))
            $at += 4 + $n + 16
        }
        $end = [uint64]($pack.Length + $grow)
        foreach ($e in $added) {
            $nb = [Text.Encoding]::UTF8.GetBytes($e[0])
            $bw.Write([uint32]$nb.Length); $bw.Write($nb); $bw.Write([uint64]$end); $bw.Write([uint64]$e[1].Length); $end += [uint64]$e[1].Length
        }
        $bw.Write($pack, $indexEnd, $pack.Length - $indexEnd)
        foreach ($e in $added) { $bw.Write([byte[]]$e[1]) }
        $bw.Flush(); $pack = $ms.ToArray()
    }
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
    $modelNames = @(
        'CC_SILVER_AARDWOLD.bin', 'CC_SMALL_FP8.bin',
        'CC_SMALL_FP8_EXPERIMENTAL.bin', 'CC_SILVER_AARDWOLD_OPTIMIZED.bin',
        'CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin',
        'dlssnr.bin', 'dlssnr-small.bin', 'dlssnr-int4.bin', 'dlssnr-small-experimental.bin'
    )
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
    $acoCacheDir = Join-Path $game 'dlssnr-amd/aco-cache'
    if ((Test-Path -LiteralPath $acoCacheDir -PathType Container) -and
        -not ((Get-Item -LiteralPath $acoCacheDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        foreach ($staleDxbc in @(Get-ChildItem -LiteralPath $acoCacheDir -Filter '*.dxbc' -File -ErrorAction SilentlyContinue)) {
            try { Remove-Item -LiteralPath $staleDxbc.FullName -Force -ErrorAction Stop } catch { }
        }
    }
    $legacyPipeBinDir = Join-Path $game 'dlssnr-amd/int4-ultra/pipeline-binaries'
    if ((Test-Path -LiteralPath $legacyPipeBinDir -PathType Container) -and
        -not ((Get-Item -LiteralPath $legacyPipeBinDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        try {
            if (@(Get-ChildItem -LiteralPath $legacyPipeBinDir -Force -ErrorAction Stop).Count -eq 0) {
                [IO.Directory]::Delete($legacyPipeBinDir, $false)
            }
        } catch { }
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
    $asiTargetFull = [IO.Path]::GetFullPath($asiTarget)
    if (-not $asiTargetFull.StartsWith($gameFullPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        if (-not $script:installedExternalPlugins) { $script:installedExternalPlugins = New-Object System.Collections.Generic.List[string] }
        $script:installedExternalPlugins.Add(("{0}|{1}" -f (Get-Sha256 $asiTargetFull).ToLowerInvariant(), $asiTargetFull))
    }
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
        $mochizukiModelsToInstall += @{ Src = $mochizukiModel; Rel = 'dlssnr-amd/CC_SILVER_AARDWOLD.bin' }
    }
    if ($wantSmall -and $mochizukiSmallModel) {
        $mochizukiModelsToInstall += @{ Src = $mochizukiSmallModel; Rel = 'dlssnr-amd/CC_SMALL_FP8.bin' }
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

    function Get-ExpectedSha256FromFile([string]$ShaFile) {
        if (-not $ShaFile -or -not (Test-Path -LiteralPath $ShaFile -PathType Leaf)) { return $null }
        $raw = Get-Content -LiteralPath $ShaFile -Raw -ErrorAction SilentlyContinue
        if (-not $raw) { return $null }
        $tok = (($raw.Trim() -split '\s+')[0]).Trim().ToUpperInvariant()
        if ($tok -match '^[0-9A-F]{64}$') { return $tok }
        return $null
    }
    function Test-MatchesSha256File([string]$BinPath, [string]$ShaFile) {
        $expected = Get-ExpectedSha256FromFile $ShaFile
        if (-not $expected) { return $true }
        if (-not (Test-Path -LiteralPath $BinPath -PathType Leaf)) { return $false }
        try { return ((Get-Sha256 $BinPath).ToUpperInvariant() -eq $expected) } catch { return $false }
    }
    $expShaFile = Find-FirstFile @(
        (Join-Path $Root 'experimental/CC_SMALL_FP8_EXPERIMENTAL.bin.sha256'),
        (Join-Path $release 'experimental/CC_SMALL_FP8_EXPERIMENTAL.bin.sha256')
    )
    $optShaFile = Find-FirstFile @(
        (Join-Path $Root 'optimized/CC_SILVER_AARDWOLD_OPTIMIZED.bin.sha256'),
        (Join-Path $release 'optimized/CC_SILVER_AARDWOLD_OPTIMIZED.bin.sha256')
    )
    $int4ShaFile = Find-FirstFile @(
        (Join-Path $Root 'model-tools/dlssnr-int4.sha256'),
        (Join-Path $release 'model-tools/dlssnr-int4.sha256')
    )

    # A previous game's derived files were cached in the package by
    # Save-GameModelForNextInstall. A new game gets that real cache instead of
    # regenerating (and failing when the package tools are unavailable),
    # provided the cached weights still match the packaged .sha256 pin.
    $cachedExperimental = Find-FirstFile @(
        (Join-Path $mochizukiAssets 'CC_SMALL_FP8_EXPERIMENTAL.bin'),
        (Join-Path $Root 'dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin')
    )
    $cachedExperimentalTarget = Join-Path $game 'dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin'
    if ($generateExperimental -and $cachedExperimental -and
        [IO.Path]::GetFullPath($cachedExperimental) -ne [IO.Path]::GetFullPath($cachedExperimentalTarget) -and
        -not (Test-Path -LiteralPath $cachedExperimentalTarget -PathType Leaf) -and
        (Test-MatchesSha256File $cachedExperimental $expShaFile)) {
        Copy-Item -LiteralPath $cachedExperimental -Destination $cachedExperimentalTarget -Force
        Write-Host 'Copied cached CC_SMALL_FP8_EXPERIMENTAL weights.' -ForegroundColor Green
    }
    $cachedOptimized = Find-FirstFile @(
        (Join-Path $mochizukiAssets 'CC_SILVER_AARDWOLD_OPTIMIZED.bin'),
        (Join-Path $Root 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED.bin')
    )
    $cachedOptimizedTarget = Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED.bin'
    if ($generateInt4 -and $cachedOptimized -and
        [IO.Path]::GetFullPath($cachedOptimized) -ne [IO.Path]::GetFullPath($cachedOptimizedTarget) -and
        -not (Test-Path -LiteralPath $cachedOptimizedTarget -PathType Leaf) -and
        (Test-MatchesSha256File $cachedOptimized $optShaFile)) {
        Copy-Item -LiteralPath $cachedOptimized -Destination $cachedOptimizedTarget -Force
        Write-Host 'Copied cached CC_SILVER_AARDWOLD_OPTIMIZED weights.' -ForegroundColor Green
    }
    $cachedInt4 = Find-FirstFile @(
        (Join-Path $mochizukiAssets 'CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin'),
        (Join-Path $Root 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin')
    )
    $cachedInt4Target = Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin'
    if ($generateInt4 -and $cachedInt4 -and
        [IO.Path]::GetFullPath($cachedInt4) -ne [IO.Path]::GetFullPath($cachedInt4Target) -and
        -not (Test-Path -LiteralPath $cachedInt4Target -PathType Leaf) -and
        (Test-MatchesSha256File $cachedInt4 $int4ShaFile)) {
        Copy-Item -LiteralPath $cachedInt4 -Destination $cachedInt4Target -Force
        Write-Host 'Copied cached CC_SILVER_AARDWOLD_OPTIMIZED_INT4 weights.' -ForegroundColor Green
    }

    # CC_SILVER_AARDWOLD_OPTIMIZED is generated from CC_SILVER_AARDWOLD.bin and optimized/dlssnr-optimized.delta,
    # then its INT4 layers (CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin) are generated from it using dlssnr-int4-weights.exe.
    $optOutFile = Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED.bin'
    $int4Data = Join-Path $game 'dlssnr-amd/int4-ultra'
    if (-not (Test-Path -LiteralPath $int4Data -PathType Container)) {
        $int4Data = Join-Path $game 'dlssnr-amd/int4'
    }
    $int4OutFile = Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin'
    $int4Reusable = $false
    if ($generateInt4 -and (Test-Path -LiteralPath $optOutFile -PathType Leaf) -and (Test-Path -LiteralPath $int4OutFile -PathType Leaf)) {
        $originalPath = Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'
        if (Test-Path -LiteralPath $originalPath -PathType Leaf) {
            $optInfo = Get-Item -LiteralPath $optOutFile
            $int4Info = Get-Item -LiteralPath $int4OutFile
            $originalInfo = Get-Item -LiteralPath $originalPath
            $int4Reusable = $optInfo.Length -gt 0 -and $int4Info.Length -gt 0 -and
                $optInfo.LastWriteTimeUtc -ge $originalInfo.LastWriteTimeUtc -and
                $int4Info.LastWriteTimeUtc -ge $optInfo.LastWriteTimeUtc -and
                (Test-MatchesSha256File $optOutFile $optShaFile) -and
                (Test-MatchesSha256File $int4OutFile $int4ShaFile)
        }
    }
    if ($generateInt4 -and $int4Reusable) {
        $int4Ready = $true
        Write-Host 'Reused existing CC_SILVER_AARDWOLD_OPTIMIZED weights.' -ForegroundColor Green
    } elseif ($generateInt4) {
        $optimizedDelta = Find-FirstFile @(
            (Join-Path $Root 'optimized/dlssnr-optimized.delta'),
            (Join-Path $release 'optimized/dlssnr-optimized.delta')
        )
        if (-not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin') -PathType Leaf)) {
            Fail 'CC_SILVER_AARDWOLD_OPTIMIZED needs the original model, which is not installed.'
        }
        if ($optimizedDelta) {
            $optTemp = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-optimized-' + [guid]::NewGuid().ToString('N') + '.bin')
            try {
                New-MochizukiExperimentalWeights (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin') $optimizedDelta $optTemp
                if (-not (Test-Path -LiteralPath $optTemp -PathType Leaf) -or (Get-Item -LiteralPath $optTemp).Length -le 0) {
                    Fail 'CC_SILVER_AARDWOLD_OPTIMIZED generation produced no weights.'
                }
                if (-not (Test-MatchesSha256File $optTemp $optShaFile)) {
                    Fail 'CC_SILVER_AARDWOLD_OPTIMIZED weights checksum mismatch (model or package incomplete?).'
                }
                Copy-Item -LiteralPath $optTemp -Destination $optOutFile -Force
                Write-Host 'CC_SILVER_AARDWOLD_OPTIMIZED weights generated.' -ForegroundColor Green
            } catch {
                if ($_.Exception.Message -like 'CC_SILVER_AARDWOLD_OPTIMIZED*') { throw }
                Fail "CC_SILVER_AARDWOLD_OPTIMIZED generation failed: $($_.Exception.Message)"
            } finally {
                Remove-Item -LiteralPath $optTemp -Force -ErrorAction SilentlyContinue
            }
        } else {
            Copy-Item -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin') -Destination $optOutFile -Force
        }
        if (-not $mochizukiInt4Tool) {
            Fail 'CC_SILVER_AARDWOLD_OPTIMIZED was requested, but model-tools/dlssnr-int4-weights.exe is missing. Re-extract the complete package.'
        }
        if (-not (Test-Path -LiteralPath $int4Data -PathType Container)) {
            Fail 'CC_SILVER_AARDWOLD_OPTIMIZED was requested, but dlssnr-amd/int4-ultra is missing. Re-extract the complete package.'
        }
        $int4Out = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-int4-' + [guid]::NewGuid().ToString('N') + '.bin')
        try {
            & $mochizukiInt4Tool $optOutFile $int4Data $int4Out
            if ($LASTEXITCODE -ne 0) {
                Fail "CC_SILVER_AARDWOLD_OPTIMIZED_INT4 generation failed (exit code $LASTEXITCODE)."
            }
            if (-not (Test-Path -LiteralPath $int4Out -PathType Leaf) -or (Get-Item -LiteralPath $int4Out).Length -le 0) {
                Fail 'CC_SILVER_AARDWOLD_OPTIMIZED_INT4 generation produced no weights.'
            }
            if (-not (Test-MatchesSha256File $int4Out $int4ShaFile)) {
                Fail 'CC_SILVER_AARDWOLD_OPTIMIZED_INT4 weights checksum mismatch (model or package incomplete?).'
            }
            Copy-Item -LiteralPath $int4Out -Destination $int4OutFile -Force
            Write-Host 'CC_SILVER_AARDWOLD_OPTIMIZED_INT4 weights generated.' -ForegroundColor Green
        } catch {
            if ($_.Exception.Message -like 'CC_SILVER_AARDWOLD_OPTIMIZED*') { throw }
            Fail "CC_SILVER_AARDWOLD_OPTIMIZED_INT4 generation failed: $($_.Exception.Message)"
        } finally {
            Remove-Item -LiteralPath $int4Out -Force -ErrorAction SilentlyContinue
        }
        $int4Ready = $true
    } else {
        $int4Ready = (Test-Path -LiteralPath $optOutFile -PathType Leaf) -and
            (Get-Item -LiteralPath $optOutFile).Length -gt 0
    }

    # CC_SMALL_FP8_EXPERIMENTAL is made from the installed small model and the
    # packaged delta. A requested network whose generation fails is an install
    # failure, never a quiet fallback or a stale file.
    $experimentalOut = Join-Path $game 'dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin'
    $experimentalReusable = $false
    if ($generateExperimental -and (Test-Path -LiteralPath $experimentalOut -PathType Leaf)) {
        $smallPathForTime = Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin'
        if (Test-Path -LiteralPath $smallPathForTime -PathType Leaf) {
            $expInfo = Get-Item -LiteralPath $experimentalOut
            $smallInfo = Get-Item -LiteralPath $smallPathForTime
            if ($expInfo.Length -gt 0 -and $expInfo.LastWriteTimeUtc -ge $smallInfo.LastWriteTimeUtc -and
                (Test-MatchesSha256File $experimentalOut $expShaFile)) {
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
        if (-not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin') -PathType Leaf)) {
            Fail 'CC_SMALL_FP8_EXPERIMENTAL needs the small model, which is not installed.'
        }
        $expOut = Join-Path ([IO.Path]::GetTempPath()) ('dlssnr-small-experimental-' + [guid]::NewGuid().ToString('N') + '.bin')
        try {
            New-MochizukiExperimentalWeights (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin') $experimentalDelta $expOut
            if (-not (Test-Path -LiteralPath $expOut -PathType Leaf) -or (Get-Item -LiteralPath $expOut).Length -le 0) {
                Fail 'CC_SMALL_FP8_EXPERIMENTAL generation produced no weights.'
            }
            if (-not (Test-MatchesSha256File $expOut $expShaFile)) {
                Fail 'CC_SMALL_FP8_EXPERIMENTAL weights checksum mismatch (model or package incomplete?).'
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

    # Migrate or remove legacy 0.0.8.1 model filenames once current CC_*.bin names are in place.
    foreach ($pair in @(
        @('dlssnr-amd/dlssnr.bin', 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'),
        @('dlssnr-amd/dlssnr-small.bin', 'dlssnr-amd/CC_SMALL_FP8.bin'),
        @('dlssnr-amd/dlssnr-small-experimental.bin', 'dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin')
    )) {
        foreach ($baseDir in @($game, $Root)) {
            if (-not $baseDir) { continue }
            $oldBin = Join-Path $baseDir $pair[0]
            $newBin = Join-Path $baseDir $pair[1]
            if (Test-Path -LiteralPath $oldBin -PathType Leaf) {
                try {
                    if (-not (Test-Path -LiteralPath $newBin -PathType Leaf) -and (Get-Item -LiteralPath $oldBin).Length -gt 0) {
                        Move-Item -LiteralPath $oldBin -Destination $newBin -Force -ErrorAction Stop
                        Write-Host "Migrated legacy model $($pair[0]) -> $($pair[1])" -ForegroundColor Green
                    } else {
                        Remove-Item -LiteralPath $oldBin -Force -ErrorAction Stop
                        Write-Host "Removed retired legacy model $($pair[0])" -ForegroundColor DarkGray
                    }
                } catch { }
            }
        }
    }
    foreach ($baseDir in @($game, $Root)) {
        if (-not $baseDir) { continue }
        $oldInt4 = Join-Path $baseDir 'dlssnr-amd/dlssnr-int4.bin'
        if (Test-Path -LiteralPath $oldInt4 -PathType Leaf) {
            try {
                Remove-Item -LiteralPath $oldInt4 -Force -ErrorAction Stop
                Write-Host 'Removed retired legacy model dlssnr-amd/dlssnr-int4.bin' -ForegroundColor DarkGray
            } catch { }
        }
    }
    $retiredPersonDir = Join-Path $game 'person-model'
    if ((Test-Path -LiteralPath $retiredPersonDir -PathType Container) -and
        -not ((Get-Item -LiteralPath $retiredPersonDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        foreach ($ownedPersonFile in @(
            'onnxruntime.dll', 'yolo11n-seg.onnx', 'pphumanseg.onnx', 'yunet.onnx',
            'person-worker.exe', 'person-worker.log', 'person-worker.1.log',
            'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
            'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
            'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll',
            'concrt140.dll', 'vccorlib140.dll'
        )) {
            $p = Join-Path $retiredPersonDir $ownedPersonFile
            if (Test-Path -LiteralPath $p -PathType Leaf) {
                try { Remove-Item -LiteralPath $p -Force -ErrorAction Stop } catch { }
            }
        }
        try {
            if (@(Get-ChildItem -LiteralPath $retiredPersonDir -Force -ErrorAction Stop).Count -eq 0) {
                [IO.Directory]::Delete($retiredPersonDir, $false)
                Write-Host 'Removed retired person-model directory.' -ForegroundColor DarkGray
            }
        } catch { }
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

    # Prune empty directories left behind under dlssnr-amd\ after retired assets are removed
    # (for example, retired shaders\d3d12\* and shaders\renodx\renodx-prep).
    $asiAssetDir = Join-Path $game 'dlssnr-amd'
    if (Test-Path -LiteralPath $asiAssetDir -PathType Container) {
        try {
            $null = Assert-LmxxfUnlinkedPath $asiAssetDir
            Get-ChildItem -LiteralPath $asiAssetDir -Directory -Recurse -ErrorAction SilentlyContinue |
                Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
                Sort-Object { $_.FullName.Length } -Descending |
                ForEach-Object {
                    try {
                        if (@(Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction Stop).Count -eq 0) {
                            [IO.Directory]::Delete($_.FullName, $false)
                        }
                    } catch { }
                }
        } catch { }
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
    $asiIniFresh = (-not $asiIniExisted) -or $overwriteIni
    if ($asiIniFresh -and $asiTemplate) {
        Copy-Item -LiteralPath $asiTemplate -Destination $asiIni -Force
        if ($asiIniExisted) {
            Write-Host 'Replaced dlssnr-amd.ini with the package file.' -ForegroundColor Cyan
        }
    } elseif ($asiIniExisted) {
        Write-Host 'Keeping the existing dlssnr-amd.ini.' -ForegroundColor Cyan
        # Strip any UTF-8 BOM left by a prior Set-IniSettings call so the native
        # C++ parser in dlssnr-amd.asi can recognize the first [Network] section.
        $asiBytes = [System.IO.File]::ReadAllBytes($asiIni)
        if ($asiBytes.Length -ge 3 -and $asiBytes[0] -eq 0xEF -and $asiBytes[1] -eq 0xBB -and $asiBytes[2] -eq 0xBF) {
            $cleanBytes = New-Object byte[] ($asiBytes.Length - 3)
            [System.Array]::Copy($asiBytes, 3, $cleanBytes, 0, $cleanBytes.Length)
            [System.IO.File]::WriteAllBytes($asiIni, $cleanBytes)
        }
    }
    # [NRControl] Language names a file in dlssnr-amd\lang without .txt:
    # zh-CN.txt and English.txt are the shipped values.
    # - Fresh install or overwrite install: set Language to the launcher language.
    # - Kept user ini: only fill Language if the key is completely missing.
    #   An explicit empty value ("Language =") is Mochizuki's native representation
    #   for English and must be preserved when the user chose to keep their settings.
    if ($asiMenuLanguage -and (Test-Path -LiteralPath $asiIni -PathType Leaf)) {
        if ($asiIniFresh) {
            Set-IniSettings $asiIni 'NRControl' ([ordered]@{ 'Language' = $asiMenuLanguage })
        } else {
            Set-IniSettings $asiIni 'NRControl' ([ordered]@{ 'Language' = $asiMenuLanguage }) -OnlyMissing
        }
    }
    # A fresh or overwritten ini gets this install's startup network. A kept user ini is
    # preserved except when the caller explicitly chose a startup network; that
    # choice must take effect instead of being ignored.
    $writeActiveNetwork = ($asiIniFresh -and $asiTemplate) -or
        ($asiIniExisted -and [bool]$MochizukiActiveModel)
    if ($writeActiveNetwork -and (Test-Path -LiteralPath $asiIni -PathType Leaf)) {
        $smallReady = (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin') -PathType Leaf) -and
            (Get-Item -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin')).Length -gt 0
        $originalReady = (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin') -PathType Leaf) -and
            (Get-Item -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin')).Length -gt 0
        $activeModelName = $MochizukiActiveModel
        if (-not $activeModelName -and $asiIniFresh) {
            if ($networksExplicit) {
                foreach ($candidate in @('small', 'experimental', 'original', 'int4', 'optimized')) {
                    if ($MochizukiNetworks -notcontains $candidate) { continue }
                    $ready = ($candidate -eq 'small' -and $smallReady) -or
                        ($candidate -eq 'experimental' -and $experimentalReady) -or
                        ($candidate -eq 'original' -and $originalReady) -or
                        (($candidate -in @('int4', 'optimized')) -and $int4Ready)
                    if ($ready) { $activeModelName = $candidate; break }
                }
            }
            if (-not $activeModelName -and $MochizukiModels -in @('small', 'experimental', 'original', 'int4', 'optimized')) {
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
        if ($activeModelName -in @('int4', 'optimized') -and -not $int4Ready) {
            Fail 'The selected startup network CC_SILVER_AARDWOLD_OPTIMIZED is not available; generation did not complete.'
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
                'int4' { 'CC_SILVER_AARDWOLD_OPTIMIZED' }
                'optimized' { 'CC_SILVER_AARDWOLD_OPTIMIZED' }
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

    if ($wantSmall -and -not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin'))) {
        Write-Host 'Mochizuki small model not installed. Run Mochizuki-Model.bat beside Setup.bat with your nvngx_dlssnr.dll, then run Setup again.' -ForegroundColor Yellow
    }
    if ($wantOriginal -and -not (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'))) {
        Write-Host 'Mochizuki original model not installed. Run Mochizuki-Model.bat beside Setup.bat with your nvngx_dlssnr.dll, then run Setup again.' -ForegroundColor Yellow
    }
}

# The staged extraction is only needed until the models are installed.
if ($mochizukiModelStage -and (Test-Path -LiteralPath $mochizukiModelStage)) {
    Remove-Item -LiteralPath $mochizukiModelStage -Recurse -Force -ErrorAction SilentlyContinue
    $mochizukiModelStage = $null
}

$targetNetworkModel = if ($MochizukiModels -in @('original', 'int4') -or $MochizukiActiveModel -in @('original', 'int4')) { 0 } else { 1 }
if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
    if (-not $installMochizuki) {
        $asiPluginDirs = @((Join-Path $game 'OptiScaler/plugins'))
        try {
            $customPlugDir = Get-PluginsTargetDirectory $game $gameIni
            if ($customPlugDir -and $asiPluginDirs -notcontains $customPlugDir) { $asiPluginDirs += $customPlugDir }
        } catch { }
        foreach ($pDir in $asiPluginDirs) {
            $staleAsi = Join-Path $pDir 'dlssnr-amd.asi'
            if (Test-Path -LiteralPath $staleAsi -PathType Leaf) {
                if (Remove-PluginFileChecked $staleAsi) {
                    Write-Host 'Removed dlssnr-amd.asi because Neural Rendering was unchecked.' -ForegroundColor DarkGray
                }
            }
        }
    }
    Set-IniSettings $gameIni 'Plugins' ([ordered]@{ 'LoadAsiPlugins' = 'true' })
    Set-IniSettings $gameIni 'Log' ([ordered]@{ 'LogToFile' = 'true'; 'LogLevel' = '2' }) -OnlyMissing
    if ($installMochizuki) {
        $fgKey = Get-IniSetting $gameIni 'Menu' 'FGShortcutKey'
        if ([string]::IsNullOrWhiteSpace($fgKey) -or $fgKey -eq 'auto' -or $fgKey -eq '35' -or $fgKey -eq '0x23') {
            Set-IniSettings $gameIni 'Menu' ([ordered]@{ 'FGShortcutKey' = '-1' })
            Write-Host 'Set [Menu] FGShortcutKey=-1 because NR Control uses End. A custom FG key is preserved.' -ForegroundColor Green
        }
    }
    Write-Host "Updated official OptiScaler.ini ([Plugins] LoadAsiPlugins=true, [Log] LogToFile=true)" -ForegroundColor Green
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
            $extFull = [IO.Path]::GetFullPath($targetPluginFile)
            if (-not $script:installedExternalPlugins) { $script:installedExternalPlugins = New-Object System.Collections.Generic.List[string] }
            $script:installedExternalPlugins.Add(("{0}|{1}" -f (Get-Sha256 $extFull).ToLowerInvariant(), $extFull))
        }
    }

    $installed = $false
    if ($downloadSucceeded) {
        Write-Host 'Downloaded latest OptiPatcher.asi successfully.' -ForegroundColor Green
        # Update cache in package only when PE validation succeeded
        $pkgPluginDirs = @((Join-Path $release 'OptiScaler\plugins'), (Join-Path $release 'plugins'))
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
            (Join-Path $release 'OptiScaler\plugins\OptiPatcher.asi'),
            (Join-Path $release 'plugins\OptiPatcher.asi')
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
    foreach ($srcPluginDir in @((Join-Path $release 'OptiScaler\plugins'), (Join-Path $release 'plugins'))) {
        if (-not (Test-Path -LiteralPath $srcPluginDir -PathType Container)) { continue }
        if ([IO.Path]::GetFullPath($srcPluginDir).TrimEnd('\', '/') -ieq [IO.Path]::GetFullPath($resolvedPluginsDir).TrimEnd('\', '/')) { continue }
        Get-ChildItem -LiteralPath $srcPluginDir -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -notmatch '(?i)^(OptiPatcher.*|dlssnr-amd|openrr|openrr-amd)\.asi$' -and
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

# OpenRR ASI plugin (openrr-amd.asi + openrr-amd.ini + openrr-amd/{shaders,shaders-gfx11,aco,lang} + optional nvngx_dlssd.dll 310.9.1)
$openRrPluginDir = try { Get-PluginsTargetDirectory $game $gameIni } catch { Join-Path $game 'OptiScaler\plugins' }
$gameFullPrefixForRr = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
$openRrShaderManifest = Join-Path $game 'openrr-shaders-install.txt'
function Install-OpenRrShaders([string]$standard, [string]$gfx11, [string]$assetsRoot = '') {
    $manifest = Assert-LmxxfUnlinkedPath $openRrShaderManifest
    $previous = @()
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        $previous = @(Get-Content -LiteralPath $manifest | Where-Object {
            $_ -match '^openrr-amd/(?:shaders(?:-gfx11)?/[^\\/:*?"<>|\x00-\x1F]+\.spv|aco/(?:gfx1200|gfx1201)/openrr-d3d12-v1/[^\\/:*?"<>|\x00-\x1F]+\.(?:nrp|json|msgpack)|lang/(?:fonts/)?[^\\/:*?"<>|\x00-\x1F]+\.(?:txt|ttf)|OpenRR_(?:LICENSE\.txt|THIRD_PARTY\.md))$'
        })
    }
    $installed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if ($assetsRoot -and (Test-Path -LiteralPath $assetsRoot -PathType Container)) {
        $assetsPrefix = [IO.Path]::GetFullPath($assetsRoot).TrimEnd('\', '/')
        foreach ($file in @(Get-LmxxfUnlinkedFiles $assetsRoot)) {
            if ($file.Name -ieq 'nvngx_dlssd.dll') { continue }
            $subRel = $file.FullName.Substring($assetsPrefix.Length).TrimStart('\', '/').Replace('\', '/')
            $rel = 'openrr-amd/' + $subRel
            if ($keep.ContainsKey($file.Name)) { continue }
            [void](Assert-LmxxfUnlinkedPath (Join-Path $game $rel))
            Install-One $file.FullName $rel
            [void]$installed.Add($rel)
        }
    } else {
        foreach ($source in @(@{ Path = $standard; Name = 'shaders' }, @{ Path = $gfx11; Name = 'shaders-gfx11' })) {
            if (-not $source.Path) { continue }
            foreach ($file in Get-ChildItem -LiteralPath $source.Path -Filter '*.spv' -File) {
                $rel = 'openrr-amd/' + $source.Name + '/' + $file.Name
                if ($keep.ContainsKey($file.Name)) { continue }
                [void](Assert-LmxxfUnlinkedPath (Join-Path $game $rel))
                Install-One $file.FullName $rel
                [void]$installed.Add($rel)
            }
        }
    }
    foreach ($rel in $previous) {
        if ($installed.Contains($rel)) { continue }
        if ($keep.ContainsKey((Split-Path -Leaf $rel))) {
            [void]$installed.Add($rel)
            continue
        }
        try {
            $stale = Assert-LmxxfUnlinkedPath (Join-Path $game $rel)
            if (Test-Path -LiteralPath $stale -PathType Leaf) {
                Remove-Item -LiteralPath $stale -Force -ErrorAction Stop
            }
        } catch {
            [void]$installed.Add($rel)
            Write-Host "NOTE: could not remove retired OpenRR asset $rel ($($_.Exception.Message))" -ForegroundColor Yellow
        }
    }
    $rrGameDir = Join-Path $game 'openrr-amd'
    if (Test-Path -LiteralPath $rrGameDir -PathType Container) {
        try {
            $null = Assert-LmxxfUnlinkedPath $rrGameDir
            Get-ChildItem -LiteralPath $rrGameDir -Directory -Recurse -ErrorAction SilentlyContinue |
                Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
                Sort-Object { $_.FullName.Length } -Descending |
                ForEach-Object {
                    try {
                        if (@(Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction Stop).Count -eq 0) {
                            [IO.Directory]::Delete($_.FullName, $false)
                        }
                    } catch { }
                }
        } catch { }
    }
    # An empty inventory still marks a managed installation for future cleanup.
    $lines = @('# OpenRR resources installed by OptiScaler(NR).') + @($installed | Sort-Object)
    Set-Content -LiteralPath $manifest -Value $lines -Encoding UTF8
}

if ($wantOpenRr) {
    $openRrAsiSrc = $null
    foreach ($cand in @(
        (Join-Path $release 'OptiScaler\plugins\openrr-amd.asi'),
        (Join-Path $Root 'OptiScaler\plugins\openrr-amd.asi'),
        (Join-Path $release 'plugins\openrr-amd.asi'),
        (Join-Path $Root 'openrr-amd.asi'),
        (Join-Path $release 'OptiScaler\plugins\openrr.asi'),
        (Join-Path $Root 'OptiScaler\plugins\openrr.asi'),
        (Join-Path $release 'plugins\openrr.asi'),
        (Join-Path $Root 'openrr.asi')
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $openRrAsiSrc = $cand; break }
    }
    if (-not (Test-Path -LiteralPath $openRrPluginDir -PathType Container)) {
        [void][IO.Directory]::CreateDirectory($openRrPluginDir)
    }
    $openRrTargetFull = [IO.Path]::GetFullPath((Join-Path $openRrPluginDir 'openrr-amd.asi'))
    $openRrTargetRel = if ($openRrTargetFull.StartsWith($gameFullPrefixForRr, [StringComparison]::OrdinalIgnoreCase)) {
        $openRrTargetFull.Substring($gameFullPrefixForRr.Length)
    } else { $null }
    foreach ($staleAsiPath in @(
        (Join-Path $openRrPluginDir 'openrr.asi'),
        (Join-Path $game 'OptiScaler\plugins\openrr.asi'),
        (Join-Path $game 'OptiScaler\plugins\openrr-amd.asi'),
        (Join-Path $game 'plugins\openrr.asi'),
        (Join-Path $game 'plugins\openrr-amd.asi')
    )) {
        if ([IO.Path]::GetFullPath($staleAsiPath) -ine $openRrTargetFull -and (Test-Path -LiteralPath $staleAsiPath -PathType Leaf)) {
            [void](Remove-PluginFileChecked $staleAsiPath)
        }
    }
    if ($openRrAsiSrc) {
        if (Test-ValidPePlugin $openRrAsiSrc) {
            if ($openRrTargetRel) {
                Install-One $openRrAsiSrc $openRrTargetRel
                Write-Host "Installed OpenRR ASI plugin ($openRrTargetRel)." -ForegroundColor Green
            } else {
                Copy-Item -LiteralPath $openRrAsiSrc -Destination $openRrTargetFull -Force
                if (-not $script:installedExternalPlugins) { $script:installedExternalPlugins = New-Object System.Collections.Generic.List[string] }
                $script:installedExternalPlugins.Add(("{0}|{1}" -f (Get-Sha256 $openRrTargetFull).ToLowerInvariant(), $openRrTargetFull))
                Write-Host "Installed OpenRR ASI plugin ($openRrTargetFull)." -ForegroundColor Green
            }
        } else {
            if (Test-Path -LiteralPath $openRrTargetFull -PathType Leaf) {
                [void](Remove-PluginFileChecked $openRrTargetFull)
            }
            Write-Host 'NOTE: openrr-amd.asi is currently a 0-byte placeholder; skipped copying empty .asi into plugins directory.' -ForegroundColor Cyan
        }
    }
    # OptiFG DLSSD host: the ASI probes for openrr-ngx-host.dll beside itself.
    $openRrHostSrc = $null
    foreach ($cand in @(
        (Join-Path $release 'OptiScaler\plugins\openrr-ngx-host.dll'),
        (Join-Path $release 'plugins\openrr-ngx-host.dll'),
        (Join-Path $Root 'OptiScaler\plugins\openrr-ngx-host.dll'),
        (Join-Path $Root 'plugins\openrr-ngx-host.dll'),
        (Join-Path $Root 'openrr-ngx-host.dll')
    )) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $openRrHostSrc = $cand; break }
    }
    if ($openRrHostSrc) {
        $openRrHostTargetFull = [IO.Path]::GetFullPath((Join-Path $openRrPluginDir 'openrr-ngx-host.dll'))
        foreach ($staleHostPath in @(
            (Join-Path $game 'OptiScaler\plugins\openrr-ngx-host.dll'),
            (Join-Path $game 'plugins\openrr-ngx-host.dll'),
            (Join-Path $game 'openrr-ngx-host.dll')
        )) {
            if ([IO.Path]::GetFullPath($staleHostPath) -ine $openRrHostTargetFull -and (Test-Path -LiteralPath $staleHostPath -PathType Leaf)) {
                [void](Remove-PluginFileChecked $staleHostPath)
            }
        }
        if ($openRrHostTargetFull.StartsWith($gameFullPrefixForRr, [StringComparison]::OrdinalIgnoreCase)) {
            Install-One $openRrHostSrc $openRrHostTargetFull.Substring($gameFullPrefixForRr.Length)
        } else {
            Copy-Item -LiteralPath $openRrHostSrc -Destination $openRrHostTargetFull -Force
            if (-not $script:installedExternalPlugins) { $script:installedExternalPlugins = New-Object System.Collections.Generic.List[string] }
            $script:installedExternalPlugins.Add(("{0}|{1}" -f (Get-Sha256 $openRrHostTargetFull).ToLowerInvariant(), $openRrHostTargetFull))
        }
        Write-Host 'Installed openrr-ngx-host.dll (OptiFG DLSSD host; enable OptiFGHost in openrr-amd.ini).' -ForegroundColor Green
    }
    $openRrIniSrc = Find-FirstFile @((Join-Path $release 'openrr-amd.ini'), (Join-Path $Root 'openrr-amd.ini'))
    $gameOpenRrIni = Join-Path $game 'openrr-amd.ini'
    $openRrIniExisted = Test-Path -LiteralPath $gameOpenRrIni -PathType Leaf
    $openRrIniFresh = (-not $openRrIniExisted) -or $overwriteIni
    if ($openRrIniFresh -and $openRrIniSrc) {
        Copy-Item -LiteralPath $openRrIniSrc -Destination $gameOpenRrIni -Force
        Write-Host $(if ($openRrIniExisted) { 'Replaced openrr-amd.ini with the package file.' } else { 'Installed openrr-amd.ini.' }) -ForegroundColor Green
    } elseif ($openRrIniExisted) {
        $rrIniBytes = [IO.File]::ReadAllBytes($gameOpenRrIni)
        if ($rrIniBytes.Length -ge 3 -and $rrIniBytes[0] -eq 0xEF -and $rrIniBytes[1] -eq 0xBB -and $rrIniBytes[2] -eq 0xBF) {
            $cleanRrBytes = New-Object byte[] ($rrIniBytes.Length - 3)
            [Array]::Copy($rrIniBytes, 3, $cleanRrBytes, 0, $cleanRrBytes.Length)
            [IO.File]::WriteAllBytes($gameOpenRrIni, $cleanRrBytes)
        }
    }
    if ($asiMenuLanguage -eq 'zh-CN' -and (Test-Path -LiteralPath $gameOpenRrIni -PathType Leaf)) {
        if ($openRrIniFresh) {
            Set-IniSettings $gameOpenRrIni 'OpenRR' ([ordered]@{ 'Language' = 'zh-CN' })
        } else {
            Set-IniSettings $gameOpenRrIni 'OpenRR' ([ordered]@{ 'Language' = 'zh-CN' }) -OnlyMissing
        }
    }
    Set-OpenRrSpoof $gameIni $(if ($OpenRrSpoof -eq 'auto') { 'auto' } else { 'true' })
    foreach ($legacyRrDir in @('opennr-rr\shaders', 'opennr-rr\shaders-gfx11')) {
        $legacyRrPath = Join-Path $game $legacyRrDir
        if (Test-Path -LiteralPath $legacyRrPath -PathType Container) {
            Remove-Item -LiteralPath $legacyRrPath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    if ($openRrAssetsSrc -or $openRrShadersSrc) {
        Install-OpenRrShaders $openRrShadersSrc $openRrShadersGfx11Src $openRrAssetsSrc
        if (-not $userDlssd) {
            foreach ($cand in @((Join-Path $Root 'nvngx_dlssd.dll'), (Join-Path $release 'nvngx_dlssd.dll'), (Join-Path $Root 'openrr-amd\nvngx_dlssd.dll'), (Join-Path $Root 'opennr-rr\nvngx_dlssd.dll'), (Join-Path $game 'openrr-amd\nvngx_dlssd.dll'), (Join-Path $game 'opennr-rr\nvngx_dlssd.dll'), (Join-Path $game 'nvngx_dlssd.dll'))) {
                if (Test-Path -LiteralPath $cand -PathType Leaf) {
                    try {
                        $v = (Get-Item -LiteralPath $cand).VersionInfo
                        if ($v.FileMajorPart -eq 310 -and $v.FileMinorPart -eq 9 -and $v.FileBuildPart -eq 1) {
                            $userDlssd = $cand
                            break
                        }
                    } catch { }
                }
            }
        }
        if ($userDlssd) {
            Install-One $userDlssd 'openrr-amd\nvngx_dlssd.dll'
            Write-Host "Installed user-supplied nvngx_dlssd.dll (310.9.1) into openrr-amd\nvngx_dlssd.dll." -ForegroundColor Green
        }
        Write-Host 'Installed OpenRR resources (openrr-amd\shaders, shaders-gfx11, aco, lang).' -ForegroundColor Green
    }
} else {
    if ($priorOpenRrInstalled) { Set-OpenRrSpoof $gameIni 'auto' } # OpenRR was installed before: hand spoofing back.
    foreach ($stalePluginAsi in @(
        (Join-Path $openRrPluginDir 'openrr-amd.asi'),
        (Join-Path $openRrPluginDir 'openrr.asi'),
        (Join-Path $openRrPluginDir 'openrr-ngx-host.dll')
    )) {
        if (Test-Path -LiteralPath $stalePluginAsi -PathType Leaf) {
            [void](Remove-PluginFileChecked $stalePluginAsi)
        }
    }
    foreach ($staleRr in @('OptiScaler\plugins\openrr.asi', 'OptiScaler\plugins\openrr-amd.asi', 'OptiScaler\plugins\openrr-ngx-host.dll', 'plugins\openrr.asi', 'plugins\openrr-amd.asi', 'plugins\openrr-ngx-host.dll', 'openrr-ngx-host.dll', 'openrr-amd.ini', 'openrr-amd.log', 'openrr-amd-crash.dmp', 'openrr-control.bin', 'opennr_rr.log', 'OpenNrRrRuntime.dll', 'opennr_rr.dll', 'OptiScaler\OpenNrRrRuntime.dll', 'OptiScaler\opennr_rr.dll', 'openrr-amd\OpenRR_LICENSE.txt', 'openrr-amd\OpenRR_THIRD_PARTY.md')) {
        $stalePath = Join-Path $game $staleRr
        if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
            Remove-Item -LiteralPath $stalePath -Force -ErrorAction SilentlyContinue
        }
    }
    $staleRrDirs = @('openrr-amd\cache', 'openrr-amd\aco', 'openrr-amd\lang', 'opennr-rr\shaders', 'opennr-rr\shaders-gfx11')
    if (Test-Path -LiteralPath $openRrShaderManifest -PathType Leaf) {
        Install-OpenRrShaders '' '' ''
        Remove-Item -LiteralPath $openRrShaderManifest -Force -ErrorAction SilentlyContinue
        foreach ($shaderDir in @('openrr-amd\shaders', 'openrr-amd\shaders-gfx11', 'openrr-amd\aco', 'openrr-amd\lang')) {
            $shaderPath = Assert-LmxxfUnlinkedPath (Join-Path $game $shaderDir)
            if ((Test-Path -LiteralPath $shaderPath -PathType Container) -and
                @(Get-ChildItem -LiteralPath $shaderPath -Force).Count -eq 0) {
                [IO.Directory]::Delete($shaderPath, $false)
            }
        }
    } else {
        $staleRrDirs += @('openrr-amd\shaders', 'openrr-amd\shaders-gfx11')
    }
    foreach ($staleDir in $staleRrDirs) {
        $staleRrShaders = Join-Path $game $staleDir
        if (Test-Path -LiteralPath $staleRrShaders -PathType Container) {
            Remove-Item -LiteralPath $staleRrShaders -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($parentRr in @('openrr-amd', 'opennr-rr')) {
        $parentPath = Join-Path $game $parentRr
        if ((Test-Path -LiteralPath $parentPath -PathType Container) -and @(Get-ChildItem -LiteralPath $parentPath -Force -ErrorAction SilentlyContinue).Count -eq 0) {
            [IO.Directory]::Delete($parentPath, $false)
        }
    }

    Write-Host 'Removed/skipped OpenRR ASI plugin (unchecked).' -ForegroundColor Cyan
}
# Uninstaller is copied into the game folder. Double-click it there; it
# targets that directory (no folder picker). Sources were resolved before install
# and preserved if the automatic uninstall could remove them.
Install-One $ps1Src 'Uninstall_OptiScaler_NR.ps1'
if ($xessCompatSrc) {
    Install-One $xessCompatSrc 'xess-compat.ps1'
}
$ownedPin = Join-Path $Root 'mochizuki-owned.txt'
if (Test-Path -LiteralPath $ownedPin -PathType Leaf) {
    Install-One $ownedPin 'mochizuki-owned.txt'
}
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
    $resolvedPluginDirRecord = try { Get-PluginsTargetDirectory $game $gameIni } catch { '' }
    $markLines = @(
        'project=OptScaler(NR)',
        ('proxy=' + $Proxy),
        ('backend=' + $selectedActiveBackend),
        ('pluginDir=' + $resolvedPluginDirRecord),
        ('uninstallFirst=' + $uninstallFirst.ToString().ToLowerInvariant()),
        ('installed=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')),
        ('game=' + $game)
    )
    if ($wantOpenRr) {
        $markLines += ('openRrSpoof=' + $(if ($OpenRrSpoof -eq 'auto') { 'auto' } else { 'full' }))
    }
    $finalExternalPlugins = @{}
    foreach ($priorPath in $priorExternalPlugins.Keys) {
        $priorSha = $priorExternalPlugins[$priorPath]
        if (Test-Path -LiteralPath $priorPath -PathType Leaf) {
            try {
                if ((Get-Sha256 $priorPath).ToLowerInvariant() -eq $priorSha) {
                    $finalExternalPlugins[$priorPath] = $priorSha
                }
            } catch { }
        }
    }
    if ($script:installedExternalPlugins) {
        foreach ($extEntry in $script:installedExternalPlugins) {
            if ($extEntry -match '^([a-f0-9]{64})\|(.+)$') {
                $finalExternalPlugins[[IO.Path]::GetFullPath($Matches[2])] = $Matches[1].ToLowerInvariant()
            }
        }
    }
    foreach ($extPath in ($finalExternalPlugins.Keys | Sort-Object)) {
        $markLines += ("extPlugin={0}|{1}" -f $finalExternalPlugins[$extPath], $extPath)
    }
    [IO.File]::WriteAllText($installMark, (($markLines -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($true))
    Write-Host "Wrote install record: $installMark" -ForegroundColor Green
} catch {
    Write-Host "NOTE: could not write install record: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Cache game models even when their backend was not selected for this install.
# Publish only complete copies; never merge into or replace an existing package model.
function Save-GameModelForNextInstall([ValidateSet('mochizuki', 'mochizuki-small',
        'mochizuki-experimental', 'mochizuki-optimized', 'mochizuki-int4')][string]$Kind) {
    $stage = $null
    try {
        $packageRoot = [IO.Path]::GetFullPath($Root)
        $gameRoot = [IO.Path]::GetFullPath($game)
        if ($packageRoot.TrimEnd('\', '/') -ieq $gameRoot.TrimEnd('\', '/')) { return }        $relative = switch ($Kind) {
            'mochizuki-small' { 'dlssnr-amd/CC_SMALL_FP8.bin' }
            'mochizuki-experimental' { 'dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin' }
            'mochizuki-optimized' { 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED.bin' }
            'mochizuki-int4' { 'dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin' }
            default { 'dlssnr-amd/CC_SILVER_AARDWOLD.bin' }
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
Save-GameModelForNextInstall 'mochizuki'
Save-GameModelForNextInstall 'mochizuki-small'
Save-GameModelForNextInstall 'mochizuki-experimental'
Save-GameModelForNextInstall 'mochizuki-optimized'
Save-GameModelForNextInstall 'mochizuki-int4'

Write-Host ''
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
if ($installMochizuki) {
    Write-Host "  Installed:      OptiScaler + dlssnr-amd.asi (OptiScaler/plugins/dlssnr-amd.asi, resources)" -ForegroundColor Green
    $activeModelStr = if ($targetNetworkModel -eq 1) { 'Small' } else { 'Original' }
    Write-Host "  Mochizuki model: $MochizukiModels (active: $activeModelStr)" -ForegroundColor Green
}
if ($wantOpenRr) {
    Write-Host "  OpenRR ASI:     Installed (OptiScaler/plugins/openrr-amd.asi + openrr-amd/{shaders,aco,lang})" -ForegroundColor Green
}
if ($installedOptiPatcher) {
    Write-Host "  OptiPatcher:    Installed (DLSS spoofing enhancement, LoadAsiPlugins=true)" -ForegroundColor Green
}
Write-Host ''
$activeNeedsSmall = if ($mochizukiActiveNetwork) { $mochizukiActiveNetwork -in @('small', 'experimental') } else { $targetNetworkModel -eq 1 }
$mochizukiActiveValid = if ($activeNeedsSmall) { (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SMALL_FP8.bin') -PathType Leaf) } else { (Test-Path -LiteralPath (Join-Path $game 'dlssnr-amd/CC_SILVER_AARDWOLD.bin') -PathType Leaf) }
$activeModelPending = ($selectedActiveBackend -eq 'mochizuki' -and -not $mochizukiActiveValid)
if ($activeModelPending) {
    Write-Host "Installation completed, but dlssnr-amd.asi cannot run yet: MODEL SETUP REQUIRED. Follow the model instructions above, then restart the game." -ForegroundColor Yellow
} else { Write-Host 'Install SUCCEEDED.' -ForegroundColor Green }Pause-Exit 0
