# XeSS compatibility repair. Inspection reads PE metadata only; never executes DLLs.
# All writes are restricted to four known names beside the selected game EXE.
$script:XessGroups = [ordered]@{
    sr = @('libxess.dll')
    dx11 = @('libxess_dx11.dll')
    fg = @('libxess_fg.dll', 'libxell.dll')
    fsr_fg = @('amd_fidelityfx_framegeneration_dx12.dll')
    fsr_upscaler = @('amd_fidelityfx_upscaler_dx12.dll')
    fsr_loader = @('amd_fidelityfx_loader_dx12.dll')
    fsr_vk = @('amd_fidelityfx_vk.dll')
}

$script:XessFileCache = $null
$script:XessFileCacheDirty = $false
$script:XessFileCachePath = Join-Path ([IO.Path]::GetTempPath()) 'OptScalerNR-launcher\xess-file-cache.json'

function Ensure-XessFileCache {
    if ($null -ne $script:XessFileCache) { return }
    $script:XessFileCache = @{}
    try {
        if ([IO.File]::Exists($script:XessFileCachePath)) {
            $raw = [IO.File]::ReadAllText($script:XessFileCachePath, [Text.Encoding]::UTF8) | ConvertFrom-Json
            foreach ($p in $raw.PSObject.Properties) {
                $script:XessFileCache[$p.Name] = $p.Value
            }
        }
    } catch { $script:XessFileCache = @{} }
}

function Save-XessFileCache {
    if (-not $script:XessFileCacheDirty -or $null -eq $script:XessFileCache) { return }
    $script:XessFileCacheDirty = $false
    try {
        $dir = [IO.Path]::GetDirectoryName($script:XessFileCachePath)
        if (-not [IO.Directory]::Exists($dir)) { [void][IO.Directory]::CreateDirectory($dir) }
        $tmp = $script:XessFileCachePath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        [IO.File]::WriteAllText($tmp, ($script:XessFileCache | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
        if ([IO.File]::Exists($script:XessFileCachePath)) { [IO.File]::Delete($script:XessFileCachePath) }
        [IO.File]::Move($tmp, $script:XessFileCachePath)
    } catch { }
}

function Assert-XessPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if ([IO.File]::Exists($current) -or [IO.Directory]::Exists($current)) {
            try {
                $attr = [IO.File]::GetAttributes($current)
                if ($attr -band [IO.FileAttributes]::ReparsePoint) { throw "Linked paths are not supported: $current" }
            } catch {
                if ($_.Exception.Message -like 'Linked paths*') { throw }
            }
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Get-XessHash([string]$Path) {
    Assert-XessPath $Path
    $sha = [Security.Cryptography.SHA256]::Create()
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite,Delete')
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLowerInvariant()
    } finally {
        $fs.Dispose()
        $sha.Dispose()
    }
}
function Get-XessFile([string]$Path) {
    Assert-XessPath $Path
    $info = @{ present = $false; hash = ''; version = ''; valid = $false }
    if ([IO.Directory]::Exists($Path)) { throw "Not a regular file: $Path" }
    if (-not [IO.File]::Exists($Path)) { return $info }
    $info.present = $true
    $fi = New-Object IO.FileInfo($Path)
    $cacheKey = $null
    if ($fi.Length -gt 65536) {
        Ensure-XessFileCache
        $cacheKey = "$($fi.FullName.ToLowerInvariant())|$($fi.Length)|$($fi.LastWriteTimeUtc.Ticks)"
        if ($script:XessFileCache.ContainsKey($cacheKey)) {
            $c = $script:XessFileCache[$cacheKey]
            return @{ present = $true; hash = [string]$c.hash; version = [string]$c.version; valid = [bool]$c.valid }
        }
    }
    $info.hash = Get-XessHash $Path
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite,Delete')
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($stream.Length -ge 64 -and $reader.ReadUInt16() -eq 0x5a4d) {
            $stream.Position = 60
            $offset = $reader.ReadUInt32()
            if ($offset -ge 64 -and $offset -le $stream.Length - 26) {
                $stream.Position = $offset
                $signature = $reader.ReadUInt32()
                $machine = $reader.ReadUInt16()
                $stream.Position = $offset + 24
                $info.valid = $signature -eq 0x4550 -and $machine -eq 0x8664 -and $reader.ReadUInt16() -eq 0x20b
            }
        }
    } finally { $reader.Dispose() }
    $info.version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion
    if ($cacheKey) {
        if ($script:XessFileCache.Count -gt 256) { $script:XessFileCache.Clear() }
        $script:XessFileCache[$cacheKey] = @{ hash = $info.hash; version = $info.version; valid = $info.valid }
        $script:XessFileCacheDirty = $true
    }
    return $info
}
function Get-XessJournal([string]$GameDir) {
    $path = Join-Path $GameDir 'OptiScaler-compat-backup/active.json'
    Assert-XessPath $path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $data = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($data.schema -ne 1 -or $data.gameDir -ne [IO.Path]::GetFullPath($GameDir) -or
        $data.transaction -notmatch '^[a-f0-9]{32}$' -or $data.state -notin @('pending','applied','restored','archived')) {
        throw 'Invalid XeSS repair record. No game files were changed.'
    }
    $seen = @{}
    $allowed = @($script:XessGroups.Values | ForEach-Object { $_ })
    if (@($data.files).Count -lt 1 -or @($data.files).Count -gt $allowed.Count) { throw 'Invalid XeSS repair file list.' }
    foreach ($file in $data.files) {
        if ($file.name -notin $allowed -or $seen.ContainsKey([string]$file.name) -or
            $file.appliedHash -cnotmatch '^[a-f0-9]{64}$' -or $file.originalPresent -isnot [bool] -or
            ($file.originalPresent -and $file.originalHash -cnotmatch '^[a-f0-9]{64}$')) { throw 'Invalid XeSS repair file entry.' }
        $seen[[string]$file.name] = $true
    }
    return $data
}
function Write-XessJournal([string]$GameDir, $Data) {
    $path = Join-Path $GameDir 'OptiScaler-compat-backup/active.json'
    Assert-XessPath $path
    $temp = $path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temp, ($Data | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $path) { [IO.File]::Replace($temp, $path, [NullString]::Value) }
        else { [IO.File]::Move($temp, $path) }
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force } }
}
function Get-XessCompatibility([string]$GameDir, [string]$PackageRoot) {
    Assert-XessPath $GameDir
    $journal = Get-XessJournal $GameDir
    $groups = @()
    $identity = New-Object System.Collections.Generic.List[string]
    foreach ($id in $script:XessGroups.Keys) {
        $files = @()
        $ready = [bool]$PackageRoot
        $different = $false
        foreach ($name in $script:XessGroups[$id]) {
            $game = Get-XessFile (Join-Path $GameDir $name)
            $package = @{ present = $false; hash = ''; version = ''; valid = $false }
            if ($PackageRoot) { $package = Get-XessFile (Join-Path $PackageRoot "OptiScaler/$name") }
            $ready = $ready -and $package.valid
            $state = if (-not $game.present) { 'missing' } elseif ($game.hash -eq $package.hash) { 'same' } else { 'different' }
            if ($state -ne 'same') { $different = $true }
            $files += @{ name = $name; gameVersion = $game.version; packageVersion = $package.version; state = $state }
            $identity.Add("$name|$($game.hash)|$($package.hash)")
        }
        $groups += @{ id = $id; ready = [bool]$ready; different = $different; files = $files }
    }
    $active = $null -ne $journal -and $journal.state -in @('pending','applied')
    $journalPath = Join-Path $GameDir 'OptiScaler-compat-backup/active.json'
    if (Test-Path -LiteralPath $journalPath) { $identity.Add((Get-XessHash $journalPath)) }
    $identity.Add([IO.Path]::GetFullPath($GameDir))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $token = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($identity -join "`n"))))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
    $evidence = @()
    # Historical evidence is displayed as such, never treated as proof of current failure.
    $log = @('OptiScaler.log','OptiScaler.1.log' | ForEach-Object { Join-Path $GameDir $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending | Select-Object -First 1)
    if ($log.Count) {
        Assert-XessPath $log[0]
        $evidence = @(Get-Content -LiteralPath $log[0] -Tail 1200 -Encoding UTF8 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '(?i)(libxess|libxell|XeSSProxy|XeFGProxy|XeLLProxy)' -and $_ -match '(?i)(loaded|load|error|fail|version)' } |
            Select-Object -Last 8 | ForEach-Object {
                # WinPS 5.1 Get-Content strings carry provider metadata. A new string instance
                # prevents ConvertTo-Json from traversing PSDrive/Provider/Assembly objects.
                $line = [string]$_
                [string]::new($line.Substring(0, [Math]::Min(2048, $line.Length)).ToCharArray())
            })
    }
    $restoreFiles = @()
    if ($active) { $restoreFiles = @($journal.files | ForEach-Object { $_.name }) }
    $transaction = if ($active) { $journal.transaction } else { '' }
    Save-XessFileCache
    return @{ groups = $groups; active = $active; fingerprint = $token; evidence = $evidence; restoreFiles = $restoreFiles; transaction = $transaction }
}
function Get-XessRecoveryInfo([string]$GameDir) {
    $journal = Get-XessJournal $GameDir
    $active = $null -ne $journal -and $journal.state -in @('pending','applied')
    $fingerprint = if ($null -ne $journal) { Get-XessHash (Join-Path $GameDir 'OptiScaler-compat-backup/active.json') } else { '' }
    return @{ active = $active; fingerprint = $fingerprint; transaction = $(if ($active) { $journal.transaction } else { '' });
        restoreFiles = @($(if ($active) { $journal.files | ForEach-Object { $_.name } })) }
}
function Get-XessPreviewJournal([string]$GameDir, [string]$Transaction) {
    $journal = Get-XessJournal $GameDir
    if (-not $Transaction -or $null -eq $journal -or $journal.transaction -cne $Transaction -or
        $journal.state -notin @('pending','applied')) { throw 'The repair record changed. Reopen Compatibility repair and review the file list again.' }
    return $journal
}
function Complete-XessCompatibility([string]$GameDir, [string]$Transaction) {
    $journal = Get-XessPreviewJournal $GameDir $Transaction
    # Explicitly keep current game files and all old backups; only close the old operation.
    $journal.state = 'archived'
    $archive = Join-Path $GameDir ('OptiScaler-compat-backup/' + $journal.transaction + '/record.json')
    Assert-XessPath $archive
    [IO.File]::WriteAllText($archive, ($journal | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    Write-XessJournal $GameDir $journal
}
function Assert-XessGameStopped([string]$GameDir) {
    if ($GameDir -match '(?i)\\WindowsApps(\\|$)') { throw 'WindowsApps is not writable. Select the actual game EXE in a writable folder.' }
    foreach ($process in @(Get-Process)) {
        $path = $null
        try { $path = $process.Path } catch { }
        if ($path -and $process.ProcessName -ne 'OptScalerNR.Launcher' -and [IO.Path]::GetDirectoryName($path) -eq $GameDir) { throw 'Close the game before repairing or restoring XeSS.' }
    }
}
function Assert-XessUnlocked([string[]]$Paths) {
    $handles = New-Object System.Collections.Generic.List[IO.FileStream]
    try {
        foreach ($path in $Paths) {
            Assert-XessPath $path
            if (Test-Path -LiteralPath $path) { $handles.Add([IO.File]::Open($path, 'Open', 'ReadWrite', 'None')) }
        }
    } finally { foreach ($handle in $handles) { $handle.Dispose() } }
}
function Initialize-XessFileCommit {
    if ('OptScalerNR.XessFileCommit' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace OptScalerNR {
    public static class XessFileCommit {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
                                                uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool SetFileInformationByHandle(SafeFileHandle handle, int kind, IntPtr data, uint size);
        public static void Retire(string target, string expectedHash, string saved) {
            // DELETE + GENERIC_READ, exclusive sharing, OPEN_EXISTING, OPEN_REPARSE_POINT.
            // The same locked handle is hashed and renamed; no pathname check/replace gap.
            using (var handle = CreateFileW(target, 0x80010000, 0, IntPtr.Zero, 3, 0x00200000, IntPtr.Zero)) {
                if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
                using (var stream = new FileStream(handle, FileAccess.Read))
                using (var sha = SHA256.Create()) {
                    string actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                    if (!string.Equals(actual, expectedHash, StringComparison.Ordinal))
                        throw new IOException("File changed during operation; preserving it: " + target);
                    byte[] name = Encoding.Unicode.GetBytes(Path.GetFullPath(saved));
                    int rootOffset = IntPtr.Size == 8 ? 8 : 4;
                    int lengthOffset = rootOffset + IntPtr.Size;
                    int nameOffset = lengthOffset + 4;
                    IntPtr data = Marshal.AllocHGlobal(nameOffset + name.Length + 2);
                    try {
                        Marshal.WriteInt32(data, 0, 0); // Do not replace an existing saved file.
                        Marshal.WriteIntPtr(data, rootOffset, IntPtr.Zero);
                        Marshal.WriteInt32(data, lengthOffset, name.Length);
                        Marshal.Copy(name, 0, IntPtr.Add(data, nameOffset), name.Length);
                        Marshal.WriteInt16(data, nameOffset + name.Length, 0);
                        if (!SetFileInformationByHandle(handle, 3, data, (uint)(nameOffset + name.Length + 2)))
                            throw new Win32Exception(Marshal.GetLastWin32Error());
                    } finally { Marshal.FreeHGlobal(data); }
                }
            }
        }
    }
}
'@
}
function Move-XessFileChecked([string]$Target, [string]$ExpectedHash, [string]$Saved) {
    Assert-XessPath $Target
    Assert-XessPath $Saved
    Initialize-XessFileCommit
    [OptScalerNR.XessFileCommit]::Retire($Target, $ExpectedHash, $Saved)
}
function Get-XessPendingMove([string]$Backup, $Entry) {
    $path = Join-Path $Backup ($Entry.name + '.in-flight')
    Assert-XessPath $path
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    if ((Get-XessHash $path) -notin @($Entry.originalHash, $Entry.appliedHash)) { throw "Interrupted file move is damaged: $($Entry.name)" }
    return $path
}
function Set-XessFileAtomic([string]$Source, [string]$Target, [string]$ExpectedHash) {
    Assert-XessPath $Source
    Assert-XessPath $Target
    $gameDir = [IO.Path]::GetDirectoryName($Target)
    $journal = Get-XessJournal $gameDir
    $entry = @($journal.files | Where-Object { $_.name -eq [IO.Path]::GetFileName($Target) })
    if ($null -eq $journal -or $journal.state -notin @('pending','applied') -or $entry.Count -ne 1 -or
        $ExpectedHash -notin @($entry[0].originalHash, $entry[0].appliedHash)) { throw 'File replacement does not match the active repair record.' }
    $CurrentHash = if ($entry[0].originalPresent -and $ExpectedHash -eq $entry[0].originalHash) { $entry[0].appliedHash } else { $entry[0].originalHash }
    $RetiredDir = Join-Path $gameDir ('OptiScaler-compat-backup/' + $journal.transaction)
    $temp = $Target + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $retired = Join-Path $RetiredDir ($entry[0].name + '.in-flight')
    $pendingMove = Get-XessPendingMove $RetiredDir $entry[0]
    if ($pendingMove) {
        # The original backup is separate. A completed move may leave this marker after a crash.
        if (-not (Test-Path -LiteralPath $Target)) { throw 'An interrupted file move needs to be restored first.' }
        [IO.File]::Delete($pendingMove)
    }
    $moved = $false
    try {
        [IO.File]::Copy($Source, $temp, $false)
        if ((Get-XessHash $temp) -ne $ExpectedHash) { throw "Source changed while copying: $Source" }
        if ($CurrentHash) {
            Move-XessFileChecked $Target $CurrentHash $retired
            $moved = $true
        }
        # Move never replaces a file. An updater creating the target now wins.
        [IO.File]::Move($temp, $Target)
        if ($moved) { [IO.File]::Delete($retired) }
    } catch {
        if ($moved -and -not (Test-Path -LiteralPath $Target)) {
            try { [IO.File]::Move($retired, $Target) } catch { } # Never overwrite a concurrently created file.
        }
        throw
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force } }
}
function Restore-XessCompatibility([string]$GameDir, [string]$Transaction = '') {
    Assert-XessPath $GameDir
    Assert-XessGameStopped $GameDir
    $journal = if ($Transaction) { Get-XessPreviewJournal $GameDir $Transaction } else { Get-XessJournal $GameDir }
    if ($null -eq $journal -or $journal.state -notin @('pending','applied')) { throw 'No active XeSS repair to restore.' }
    $backup = Join-Path $GameDir ("OptiScaler-compat-backup/" + $journal.transaction)
    # Validate every backup and current target before touching any file. A game update wins.
    foreach ($file in $journal.files) {
        $target = Join-Path $GameDir $file.name
        Assert-XessPath $target
        if ($file.originalPresent -and (Get-XessHash (Join-Path $backup $file.name)) -ne $file.originalHash) { throw "Original backup is damaged: $($file.name)" }
        if (Test-Path -LiteralPath $target) {
            $hash = Get-XessHash $target
            if ($hash -ne $file.appliedHash -and (-not $file.originalPresent -or $hash -ne $file.originalHash)) {
                throw "File changed after repair; preserving it: $($file.name). Game updates must not be overwritten."
            }
        } elseif ($file.originalPresent -and -not (Get-XessPendingMove $backup $file)) {
            throw "File removed after repair; preserving the current state: $($file.name)"
        }
    }
    Assert-XessUnlocked @($journal.files | ForEach-Object { Join-Path $GameDir $_.name })
    foreach ($file in $journal.files) {
        $target = Join-Path $GameDir $file.name
        if (-not (Test-Path -LiteralPath $target) -and $file.originalPresent) {
            $pendingMove = Get-XessPendingMove $backup $file
            if (-not $pendingMove) { throw "File removed during restore: $($file.name)" }
            [IO.File]::Move($pendingMove, $target) # No replacement if an updater creates the target now.
        }
        # Re-check immediately before each write as well as during the complete preflight.
        if (Test-Path -LiteralPath $target) {
            $hash = Get-XessHash $target
            if ($file.originalPresent -and $hash -eq $file.originalHash) { continue }
            if ($hash -ne $file.appliedHash) { throw "File changed during restore: $($file.name)" }
        } elseif (-not $file.originalPresent) { continue }
        else { throw "File removed during restore: $($file.name)" }
        if ($file.originalPresent) {
            Set-XessFileAtomic (Join-Path $backup $file.name) $target $file.originalHash
        } else {
            $retired = Join-Path $backup ($file.name + '.' + [Guid]::NewGuid().ToString('N') + '.removed')
            Move-XessFileChecked $target $file.appliedHash $retired
            [IO.File]::Delete($retired)
        }
    }
    $journal.state = 'restored'
    Write-XessJournal $GameDir $journal
}
function Repair-XessCompatibility([string]$GameDir, [string]$PackageRoot, [string[]]$Groups, [string]$Fingerprint) {
    Assert-XessPath $GameDir
    Assert-XessGameStopped $GameDir
    if (-not $PackageRoot) { throw 'Complete package required for XeSS repair.' }
    $report = Get-XessCompatibility $GameDir $PackageRoot
    if (-not $Fingerprint -or $Fingerprint -ne $report.fingerprint) { throw 'Files changed. Reopen Compatibility repair and review the new file list.' }
    if ($report.active) { throw 'Restore the previous XeSS repair before applying another repair.' }
    if (-not $Groups -or @($Groups | Select-Object -Unique).Count -ne $Groups.Count) { throw 'Select XeSS components to repair.' }
    $files = @()
    foreach ($group in $Groups) {
        if ($group -cnotin @($script:XessGroups.Keys)) { throw "Unknown XeSS component: $group" }
        $capability = @($report.groups | Where-Object { $_.id -eq $group })[0]
        if (-not $capability.ready) { throw "Complete package required: missing or invalid XeSS component $group" }
        foreach ($name in $script:XessGroups[$group]) {
            $original = Get-XessFile (Join-Path $GameDir $name)
            $source = Get-XessFile (Join-Path $PackageRoot "OptiScaler/$name")
            $files += @{ name = $name; originalPresent = $original.present; originalHash = $original.hash; appliedHash = $source.hash }
        }
    }
    Assert-XessUnlocked @($files | ForEach-Object { Join-Path $GameDir $_.name })
    if ((Get-XessCompatibility $GameDir $PackageRoot).fingerprint -ne $Fingerprint) { throw 'Files changed before backup. Reopen Compatibility repair.' }
    $transaction = [Guid]::NewGuid().ToString('N')
    $backup = Join-Path $GameDir ("OptiScaler-compat-backup/" + $transaction)
    Assert-XessPath $backup
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    # Persist all originals before replacing the first DLL. Interrupted repairs remain restorable.
    foreach ($file in $files) {
        if ($file.originalPresent) {
            [IO.File]::Copy((Join-Path $GameDir $file.name), (Join-Path $backup $file.name), $false)
            if ((Get-XessHash (Join-Path $backup $file.name)) -ne $file.originalHash) { throw "Game file changed before backup: $($file.name)" }
        }
    }
    $journal = @{ schema = 1; gameDir = [IO.Path]::GetFullPath($GameDir); transaction = $transaction; state = 'pending'; files = $files }
    Write-XessJournal $GameDir $journal
    try {
        foreach ($file in $files) {
            $target = Join-Path $GameDir $file.name
            if ($file.originalPresent) {
                if ((Get-XessHash $target) -ne $file.originalHash) { throw "Game file changed during repair: $($file.name)" }
            } elseif (Test-Path -LiteralPath $target) { throw "Game file appeared during repair: $($file.name)" }
            Set-XessFileAtomic (Join-Path $PackageRoot "OptiScaler/$($file.name)") $target $file.appliedHash
        }
        $journal.state = 'applied'
        Write-XessJournal $GameDir $journal
    } catch {
        $failure = $_.Exception.Message
        try { Restore-XessCompatibility $GameDir $journal.transaction }
        catch { throw "$failure Restore also stopped: $($_.Exception.Message). Backups retained; use Restore game files." }
        throw "$failure Original game files restored."
    }
}
