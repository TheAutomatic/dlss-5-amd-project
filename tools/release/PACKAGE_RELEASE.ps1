<#
.SYNOPSIS
  Stage and zip the standalone OptiScaler ASI Launcher package from pinned upstream releases
  (defined in tools/build/upstream-pins.json; reuses matching archives in dist/upstream/ or cache/downloads/):
    - Official unmodified OptiScaler release (fetched/verified by tools/build/fetch-optiscaler.ps1)
    - DLSSNR-AMD + CJK language pack (fetched/verified by tools/build/fetch-upstream.ps1)
    - OpenRR-ASI (fetched/verified by tools/build/fetch-upstream.ps1)
    - OptiPatcher.asi (resolved/refreshed by tools/build/fetch-upstream.ps1)
    - OptiScalerNRLauncher.exe + Setup.ps1
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$OutDir = 'dist',
    [string]$Name = '',
    [string]$OptiArchive = '',
    [string]$OptiDir = '',
    [string]$MochiArchive = '',
    [string]$MochiLangArchive = '',
    [string]$MochiDir = '',
    [string]$OpenRrArchive = '',
    [string]$OpenRrDir = '',
    [string]$OptiPatcherFile = '',
    [string]$UpstreamDir = '',
    [switch]$RefreshOptiPatcher,
    [string]$SigningThumbprint = '',
    [switch]$LocalTest,
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'

if ($Jobs -le 0) {
    $envJobs = 0
    if ($env:JOBS -and [int]::TryParse($env:JOBS, [ref]$envJobs) -and $envJobs -gt 0) {
        $Jobs = $envJobs
    } else {
        $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
    }
}
if ($Jobs -gt 16) { $Jobs = 16 }

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Find-SevenZip {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($cmdName in @('7z.exe', '7zz', '7z', '7za')) {
        $cmd = Get-Command $cmdName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd -and $cmd.Source) { [void]$candidates.Add($cmd.Source) }
    }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf) { [void]$candidates.Add((Join-Path (Join-Path $pf '7-Zip') '7z.exe')) }
    }
    return $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
}

function Initialize-ParallelPackageTool {
    if ('OptiScaler.ParallelPackageTool' -as [type]) { return }
    Add-Type -ReferencedAssemblies @('System.IO.Compression', 'System.IO.Compression.FileSystem') -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace OptiScaler {
    public static class ParallelPackageTool {
        public static void CopyDirectoryTree(string srcDir, string dstDir) {
            Directory.CreateDirectory(dstDir);
            string[] dirs = Directory.GetDirectories(srcDir, "*", SearchOption.AllDirectories);
            int prefixLen = srcDir.TrimEnd('\\', '/').Length + 1;
            for (int i = 0; i < dirs.Length; i++) {
                Directory.CreateDirectory(Path.Combine(dstDir, dirs[i].Substring(prefixLen)));
            }
            string[] files = Directory.GetFiles(srcDir, "*", SearchOption.AllDirectories);
            for (int i = 0; i < files.Length; i++) {
                string dest = Path.Combine(dstDir, files[i].Substring(prefixLen));
                File.Copy(files[i], dest, true);
            }
        }

        public static KeyValuePair<string, string>[] HashFiles(string stageDir, string[] fullPaths, int maxParallel) {
            int prefixLen = stageDir.TrimEnd('\\', '/').Length + 1;
            var results = new KeyValuePair<string, string>[fullPaths.Length];
            var opts = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, maxParallel) };
            Parallel.For(0, fullPaths.Length, opts,
                () => SHA256.Create(),
                (i, state, sha) => {
                    string full = fullPaths[i];
                    string rel = full.Substring(prefixLen).Replace('\\', '/');
                    using (var fs = File.OpenRead(full)) {
                        string digest = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "");
                        results[i] = new KeyValuePair<string, string>(rel, digest);
                    }
                    return sha;
                },
                sha => { if (sha != null) sha.Dispose(); }
            );
            return results;
        }

        public static void VerifyZipEntries(string zipPath, Dictionary<string, string> expectedEntries, string forbiddenPattern, string[] requiredEntries, int maxParallel) {
            var forbidden = new Regex(forbiddenPattern, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var toVerify = new List<Tuple<int, string, long>>();
            using (var archive = ZipFile.OpenRead(zipPath)) {
                var entries = archive.Entries;
                for (int i = 0; i < entries.Count; i++) {
                    var entry = entries[i];
                    string relative = entry.FullName.Replace('\\', '/');
                    if (string.IsNullOrEmpty(entry.Name)) continue;
                    if (relative.StartsWith("./", StringComparison.Ordinal) || relative.StartsWith("/", StringComparison.Ordinal)) {
                        throw new InvalidDataException("Invalid archive entry path prefix: " + relative);
                    }
                    if (forbidden.IsMatch(entry.Name) || !expectedEntries.ContainsKey(relative) || !seen.Add(relative)) {
                        throw new InvalidDataException("Unexpected, forbidden or duplicate archive entry: " + relative);
                    }
                    toVerify.Add(Tuple.Create(i, relative, entry.Length));
                }
            }
            for (int i = 0; i < requiredEntries.Length; i++) {
                if (!seen.Contains(requiredEntries[i])) {
                    throw new InvalidDataException("Package archive missing required component: " + requiredEntries[i]);
                }
            }
            foreach (string key in expectedEntries.Keys) {
                if (!seen.Contains(key)) {
                    throw new InvalidDataException("Package archive missing expected file: " + key);
                }
            }
            // Schedule largest entries first to avoid single-core tail at the end of verification.
            toVerify.Sort((a, b) => b.Item3.CompareTo(a.Item3));
            var opts = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, maxParallel) };
            try {
                Parallel.ForEach(
                    Partitioner.Create(toVerify, EnumerablePartitionerOptions.NoBuffering),
                    opts,
                    () => Tuple.Create(ZipFile.OpenRead(zipPath), SHA256.Create()),
                    (item, state, local) => {
                        var entry = local.Item1.Entries[item.Item1];
                        using (var stream = entry.Open()) {
                            string actual = BitConverter.ToString(local.Item2.ComputeHash(stream)).Replace("-", "");
                            if (!string.Equals(actual, expectedEntries[item.Item2], StringComparison.OrdinalIgnoreCase)) {
                                throw new InvalidDataException("Archive checksum mismatch: " + item.Item2);
                            }
                        }
                        return local;
                    },
                    local => {
                        if (local != null) {
                            if (local.Item2 != null) local.Item2.Dispose();
                            if (local.Item1 != null) local.Item1.Dispose();
                        }
                    }
                );
            } catch (AggregateException aex) {
                throw aex.Flatten().InnerException ?? aex;
            }
        }
    }
}
'@
}

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Version) {
    $vf = Join-Path $root 'VERSION'
    if (Test-Path -LiteralPath $vf -PathType Leaf) {
        $Version = (Get-Content -LiteralPath $vf -Encoding UTF8 -TotalCount 1).Trim()
    }
    if (-not $Version) { $Version = '2.0.0' }
}
if (-not $Name) { $Name = "OptiScaler-NR-Win-$Version" }

$stage = Join-Path $root (Join-Path $OutDir $Name)
$zip   = Join-Path $root (Join-Path $OutDir ($Name + '.zip'))

# 1. Fetch / verify official OptiScaler + upstream ASI payloads
$fetchScript = Join-Path $root 'tools/build/fetch-optiscaler.ps1'
$optiInfo = & $fetchScript -OptiArchive $OptiArchive -OptiDir $OptiDir -UpstreamDir $UpstreamDir
$optiReleaseDir = $optiInfo.ReleaseDir
$optiLicense    = $optiInfo.LicensePath

$fetchUpstreamScript = Join-Path $root 'tools/build/fetch-upstream.ps1'
$upstreamInfo = & $fetchUpstreamScript `
    -UpstreamDir $UpstreamDir `
    -MochiArchive $MochiArchive `
    -MochiLangArchive $MochiLangArchive `
    -MochiDir $MochiDir `
    -OpenRrArchive $OpenRrArchive `
    -OpenRrDir $OpenRrDir `
    -OptiPatcherFile $OptiPatcherFile `
    -RefreshOptiPatcher:$RefreshOptiPatcher

# 2. Verify ASI payloads exist
$asiPayload = $upstreamInfo.DlssnrDir
$reShade32Entries = @(
    'reshade/ReShade32.dll',
    'reshade/dlssnr_amd.addon32',
    'reshade/vk-override/implicit_layer/ReShade32.json',
    'dxvk/x86/d3d9.dll',
    'dxvk/x86/dxgi.dll',
    'vulkan/x86/vulkan-1.dll'
)
foreach ($required in @(
    'OptiScaler/plugins/dlssnr-amd.asi',
    'nr_present_probe.exe',
    'dlssnr-amd/aco/d3d12_outer.nra',
    'dlssnr-amd/aco/rdna3/d3d12_outer.nra',
    'ini/dlssnr-amd-optiscaler.ini',
    'ini/dlssnr-amd-optiscaler-renodx.ini',
    'ini/dlssnr-amd-standalone.ini',
    'ini/dlssnr-amd-addon.ini',
    'standalone/dxgi.dll',
    'standalone/vk-override/implicit_layer/dlssnr_standalone.json',
    'reshade/ReShade64.dll',
    'reshade/dlssnr_amd.addon64',
    'reshade/ReShade.ini',
    'reshade/ReShadePreset.ini',
    'reshade/ReShadePreset-d3d9.ini',
    'reshade/vk-override/implicit_layer/ReShade64.json',
    'reshade/reshade-shaders/Shaders/DLSS5_Feed.fx',
    'dxvk/d3d9.dll',
    'dxvk/dxgi.dll',
    'dxvk/LICENSE.txt',
    'dxvk/version.txt',
    'vulkan/vulkan-1.dll',
    'vulkan/Vulkan-Loader-LICENSE.txt',
    'model-tools/dlssnr_extract_model.exe',
    'model-tools/dlssnr-int4-weights.exe',
    'model-tools/dlssnr-int4.sha256',
    'experimental/dlssnr-small-experimental.delta',
    'experimental/CC_SMALL_FP8_EXPERIMENTAL.bin.sha256',
    'optimized/dlssnr-optimized.delta',
    'optimized/CC_SILVER_AARDWOLD_OPTIMIZED.bin.sha256'
) + $reShade32Entries) {
    if (-not (Test-Path -LiteralPath (Join-Path $asiPayload $required) -PathType Leaf)) {
        throw "dlssnr-amd payload incomplete: $required"
    }
}
if (Test-Path -LiteralPath (Join-Path $asiPayload 'dlssnr-amd/shaders/d3d12')) {
    throw 'dlssnr-amd payload still contains retired dlssnr-amd/shaders/d3d12 directory.'
}
$retiredHlsl = Get-ChildItem -LiteralPath (Join-Path $asiPayload 'dlssnr-amd/shaders') -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.hlsl', '.hlsli' } | Select-Object -First 1
if ($retiredHlsl) {
    throw "dlssnr-amd payload still contains retired HLSL shader: $($retiredHlsl.FullName)"
}
$openRrPayload = $upstreamInfo.OpenRrDir
$openRrAsiSrc = $null
foreach ($cand in @('OptiScaler/plugins/openrr-amd.asi', 'openrr-amd.asi', 'OptiScaler/plugins/openrr.asi', 'plugins/openrr-amd.asi', 'plugins/openrr.asi')) {
    $p = Join-Path $openRrPayload $cand
    if (Test-Path -LiteralPath $p -PathType Leaf) { $openRrAsiSrc = $p; break }
}
if (-not $openRrAsiSrc) {
    throw 'openrr payload missing OptiScaler/plugins/openrr-amd.asi (or openrr.asi).'
}
foreach ($reqOpenRrDir in @(
    'openrr-amd/shaders',
    'openrr-amd/shaders-gfx11',
    'openrr-amd/aco/gfx1201/openrr-d3d12-v1',
    'openrr-amd/aco/gfx1200/openrr-d3d12-v1',
    'openrr-amd/lang/fonts'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $openRrPayload $reqOpenRrDir) -PathType Container)) {
        throw "openrr payload missing $reqOpenRrDir."
    }
}
foreach ($reqOpenRrFile in @(
    'OptiScaler/plugins/openrr-ngx-host.dll',
    'openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.msgpack',
    'openrr-amd/aco/gfx1200/openrr-d3d12-v1/manifest.msgpack',
    'openrr-amd/lang/zh-CN.txt',
    'openrr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $openRrPayload $reqOpenRrFile) -PathType Leaf)) {
        throw "openrr payload missing $reqOpenRrFile."
    }
}
$optiPatcherAsiSrc = $upstreamInfo.OptiPatcherAsi
if (-not $optiPatcherAsiSrc -or -not (Test-Path -LiteralPath $optiPatcherAsiSrc -PathType Leaf)) {
    throw 'optipatcher payload missing OptiScaler/plugins/OptiPatcher.asi.'
}
$rtxRemixMcPayload = $upstreamInfo.RtxRemixMcDir
if (-not $rtxRemixMcPayload -or -not (Test-Path -LiteralPath (Join-Path $rtxRemixMcPayload 'rtx-remix-mc/remixbridge.dll') -PathType Leaf)) {
    throw 'rtx-remix-mc payload missing rtx-remix-mc/remixbridge.dll.'
}

# 3. Prepare clean staging directory
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'OptiScaler'), (Join-Path $stage 'Licenses') | Out-Null

# 4. Copy unmodified official OptiScaler release files
Copy-Item -LiteralPath (Join-Path $optiReleaseDir 'OptiScaler.dll') -Destination (Join-Path $stage 'OptiScaler.dll') -Force
Copy-Item -LiteralPath (Join-Path $optiReleaseDir 'OptiScaler.ini') -Destination (Join-Path $stage 'OptiScaler.ini') -Force
Get-ChildItem -LiteralPath (Join-Path $optiReleaseDir 'OptiScaler') | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'OptiScaler') -Recurse -Force
}
if (Test-Path -LiteralPath (Join-Path $optiReleaseDir 'Licenses') -PathType Container) {
    Get-ChildItem -LiteralPath (Join-Path $optiReleaseDir 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force
}
Copy-Item -LiteralPath $optiLicense -Destination (Join-Path $stage 'Licenses/OptiScaler_LICENSE.txt') -Force

$pinsManifest = Get-Content -LiteralPath (Join-Path $root 'tools/build/upstream-pins.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$optiCommit = if ([string]$pinsManifest.optiscaler.licenseUrl -match 'OptiScaler/([0-9a-fA-F]{7,40})/LICENSE') { $Matches[1] } else { '' }
$optiSourceText = @(
    'Component: OptiScaler'
    'License: GNU General Public License v3.0 (see Licenses/OptiScaler_LICENSE.txt)'
    'Upstream Repository: https://github.com/optiscaler/OptiScaler'
    "Bundled Pinned Version: $($pinsManifest.optiscaler.version)"
    $(if ($optiCommit) { "Pinned Commit: $optiCommit" } else { 'Pinned Commit: (see tools/build/upstream-pins.json)' })
    "Archive URL: $($pinsManifest.optiscaler.archiveUrl)"
    "Archive SHA-256: $($pinsManifest.optiscaler.archiveSha256)"
    ''
    'Notice: Unmodified official OptiScaler binaries are distributed as an'
    'independent aggregate strictly under GPL-3.0 with no additional restrictions'
    'imposed by this project. Default OptiScaler.ini keys (LoadAsiPlugins=true,'
    'LogToFile=true, LogLevel=2, FGShortcutKey=-1) are set during packaging.'
) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'Licenses/OptiScaler_SOURCE.txt'), ($optiSourceText + "`r`n"), [Text.UTF8Encoding]::new($false))

# Update official OptiScaler.ini in-place (LoadAsiPlugins=true, LogToFile=true, LogLevel=2, FGShortcutKey=-1)
$iniPath = Join-Path $stage 'OptiScaler.ini'
$wanted = [ordered]@{
    'Plugins|LoadAsiPlugins' = 'true'
    'Log|LogToFile'          = 'true'
    'Log|LogLevel'           = '2'
    'Menu|FGShortcutKey'     = '-1'
}
$raw = [IO.File]::ReadAllText($iniPath)
$newline = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
$lines = $raw -split [regex]::Escape($newline)
$section = $null; $seen = @{}
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^\s*\[([^\]]+)\]') { $section = $Matches[1]; continue }
    if ($section -and $lines[$i] -match '^\s*([A-Za-z0-9_]+)\s*=') {
        $key = "$section|$($Matches[1])"
        if ($wanted.Contains($key) -and -not $seen.ContainsKey($key)) {
            $lines[$i] = "$($Matches[1])=$($wanted[$key])"
            $seen[$key] = 1
        }
    }
}
[IO.File]::WriteAllText($iniPath, ($lines -join $newline), [Text.UTF8Encoding]::new($false))

# 5. Stage OptiScaler/plugins/ and OptiPatcher.asi
$optiPluginDst = Join-Path $stage 'OptiScaler/plugins'
New-Item -ItemType Directory -Path $optiPluginDst -Force | Out-Null
$pluginReadme = @(
    'OptiScaler(NR) - ASI Plugins Directory'
    '====================================='
    'Place custom .asi plugins (for example MFGUnlocker.asi or XeFGUnlock.asi) in this folder.'
) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $optiPluginDst 'OptScaler-NR-plugins-README.txt'), ($pluginReadme + "`r`n"), [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $optiPatcherAsiSrc -Destination (Join-Path $optiPluginDst 'OptiPatcher.asi') -Force
$optiPatcherSha = Get-Sha256 (Join-Path $optiPluginDst 'OptiPatcher.asi')
$optiPatcherSourceText = @(
    'Component: OptiPatcher (OptiPatcher.asi)'
    'License: GNU General Public License v3.0 (full text in Licenses/OptiScaler_LICENSE.txt)'
    'Upstream Repository / Corresponding Source: https://github.com/optiscaler/OptiPatcher'
    "Download URL: $($pinsManifest.optipatcher.url)"
    "Staged File SHA-256: $($optiPatcherSha.ToLowerInvariant())"
    ''
    'Notice: Unmodified OptiPatcher.asi is distributed as an independent aggregate'
    'strictly under GPL-3.0 with no additional restrictions imposed by this project.'
) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'Licenses/OptiPatcher_SOURCE.txt'), ($optiPatcherSourceText + "`r`n"), [Text.UTF8Encoding]::new($false))

# 6. Stage ASI 1: dlssnr-amd.asi + resources
Initialize-ParallelPackageTool
$mzAssets = Join-Path $stage 'dlssnr-amd'
New-Item -ItemType Directory -Force $mzAssets, (Join-Path $stage 'model-tools'), (Join-Path $stage 'experimental') | Out-Null
[OptiScaler.ParallelPackageTool]::CopyDirectoryTree((Join-Path $asiPayload 'dlssnr-amd'), $mzAssets)
Copy-Item -LiteralPath (Join-Path $asiPayload 'OptiScaler/plugins/dlssnr-amd.asi') -Destination (Join-Path $optiPluginDst 'dlssnr-amd.asi') -Force
Copy-Item -LiteralPath (Join-Path $asiPayload 'nr_present_probe.exe') -Destination $mzAssets -Force
Copy-Item -LiteralPath (Join-Path $asiPayload 'ini/dlssnr-amd-optiscaler.ini') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $asiPayload 'ini/dlssnr-amd-optiscaler-renodx.ini') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $asiPayload 'ini/dlssnr-amd-standalone.ini') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $asiPayload 'ini/dlssnr-amd-addon.ini') -Destination $stage -Force
foreach ($routeDirName in @('standalone', 'reshade', 'dxvk', 'vulkan')) {
    $routeSrc = Join-Path $asiPayload $routeDirName
    if (Test-Path -LiteralPath $routeSrc -PathType Container) {
        $routeDst = Join-Path $stage $routeDirName
        New-Item -ItemType Directory -Force $routeDst | Out-Null
        Get-ChildItem -LiteralPath $routeSrc | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $routeDst -Recurse -Force
        }
    }
}
Get-ChildItem -LiteralPath (Join-Path $asiPayload 'experimental') -File | Copy-Item -Destination (Join-Path $stage 'experimental') -Force
if (Test-Path -LiteralPath (Join-Path $asiPayload 'optimized') -PathType Container) {
    New-Item -ItemType Directory -Force (Join-Path $stage 'optimized') | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $asiPayload 'optimized') -File | Copy-Item -Destination (Join-Path $stage 'optimized') -Force
}
Get-ChildItem -LiteralPath (Join-Path $asiPayload 'model-tools') -File | Copy-Item -Destination (Join-Path $stage 'model-tools') -Force
Get-ChildItem -LiteralPath (Join-Path $asiPayload 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force
$ownedSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$ownedPinSrc = Join-Path $root 'tools/install/mochizuki-owned.txt'
if (Test-Path -LiteralPath $ownedPinSrc -PathType Leaf) {
    foreach ($line in [IO.File]::ReadAllLines($ownedPinSrc)) {
        $rel = $line.Trim().Replace('\', '/')
        if ($rel.Length -eq 0 -or $rel.StartsWith('#')) { continue }
        [void]$ownedSet.Add($rel)
    }
}
$dlssnrResSrc = Join-Path $asiPayload 'dlssnr-amd'
$dlssnrPrefixLen = [IO.Path]::GetFullPath($dlssnrResSrc).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Length + 1
Get-ChildItem -LiteralPath $dlssnrResSrc -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($dlssnrPrefixLen).Replace('\', '/')
    [void]$ownedSet.Add($rel)
}
$ownedSorted = [string[]]@($ownedSet)
[Array]::Sort($ownedSorted, [StringComparer]::Ordinal)
[IO.File]::WriteAllText((Join-Path $stage 'mochizuki-owned.txt'), (($ownedSorted -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))

@'
@echo off
setlocal
set "SOURCE=%~1"
if not defined SOURCE set "SOURCE=%~dp0nvngx_dlssnr.dll"
if not exist "%SOURCE%" (
  echo Put nvngx_dlssnr.dll 310.9.16 or 310.8.0 beside this file, or pass its path.
  pause
  exit /b 1
)
"%~dp0model-tools\dlssnr_extract_model.exe" "%SOURCE%" "%~dp0dlssnr-amd"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" echo Model extraction failed ^(exit code %RC%^).
echo Base models only: Setup generates Experimental from Small and Optimized from Original.
pause
exit /b %RC%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Mochizuki-Model.bat') -Encoding ASCII

# 7. Stage ASI 2: openrr-amd.asi + openrr-ngx-host.dll + openrr-amd/{shaders,shaders-gfx11,aco,lang}
Copy-Item -LiteralPath $openRrAsiSrc -Destination (Join-Path $optiPluginDst 'openrr-amd.asi') -Force
$openRrHostSrc = Join-Path $openRrPayload 'OptiScaler/plugins/openrr-ngx-host.dll'
if (Test-Path -LiteralPath $openRrHostSrc -PathType Leaf) {
    Copy-Item -LiteralPath $openRrHostSrc -Destination (Join-Path $optiPluginDst 'openrr-ngx-host.dll') -Force
}
$openRrIniSrc = $null
foreach ($cand in @('openrr-amd.ini', 'OptiScaler/openrr-amd.ini')) {
    $p = Join-Path $openRrPayload $cand
    if (Test-Path -LiteralPath $p -PathType Leaf) { $openRrIniSrc = $p; break }
}
if ($openRrIniSrc) { Copy-Item -LiteralPath $openRrIniSrc -Destination (Join-Path $stage 'openrr-amd.ini') -Force }
$openRrAssetsDst = Join-Path $stage 'openrr-amd'
$openRrAssetsSrc = Join-Path $openRrPayload 'openrr-amd'
[OptiScaler.ParallelPackageTool]::CopyDirectoryTree($openRrAssetsSrc, $openRrAssetsDst)
Get-ChildItem -LiteralPath (Join-Path $openRrPayload 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force

# 7b. Stage RTX Remix for Minecraft AMD patch (remixbridge.dll + licenses; no OptiScaler.ini)
$rtxMcStageDir = Join-Path $stage 'rtx-remix-mc'
New-Item -ItemType Directory -Force $rtxMcStageDir | Out-Null
Copy-Item -LiteralPath (Join-Path $rtxRemixMcPayload 'rtx-remix-mc/remixbridge.dll') -Destination (Join-Path $rtxMcStageDir 'remixbridge.dll') -Force
Get-ChildItem -LiteralPath (Join-Path $rtxRemixMcPayload 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force

# 8. Build & stage Launcher + Installer scripts
& (Join-Path $root 'tools/build/build-launcher.ps1') -OutDir (Join-Path $root 'exports/launcher')
$launcherExe = Join-Path $root 'exports/launcher/OptiScalerNRLauncher.exe'
if (-not (Test-Path -LiteralPath $launcherExe -PathType Leaf)) { throw 'Launcher EXE was not produced' }
Copy-Item -LiteralPath $launcherExe -Destination (Join-Path $stage 'OptiScalerNRLauncher.exe') -Force
foreach ($pair in @(
    @('tools/install/install.ps1', 'Setup.ps1'),
    @('tools/install/uninstall.ps1', 'Uninstall_OptiScaler_NR.ps1'),
    @('tools/install/launcher-worker.ps1', 'launcher-worker.ps1'),
    @('tools/install/install-capabilities.ps1', 'install-capabilities.ps1'),
    @('tools/install/library-scan.ps1', 'library-scan.ps1'),
    @('tools/install/proxy-probe.ps1', 'proxy-probe.ps1'),
    @('tools/install/xess-compat.ps1', 'xess-compat.ps1')
)) {
    $srcPs1 = Join-Path $root $pair[0]
    $dstPs1 = Join-Path $stage $pair[1]
    $psContent = [IO.File]::ReadAllText($srcPs1, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText($dstPs1, $psContent, [Text.UTF8Encoding]::new($true))
}
$installShSrc = Join-Path $root 'tools/install/install.sh'
$installShContent = [IO.File]::ReadAllText($installShSrc, [Text.Encoding]::UTF8).Replace("`r`n", "`n")
[IO.File]::WriteAllText((Join-Path $stage 'install.sh'), $installShContent, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $root 'assets/compatibility/proxy-profiles.json') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/compatibility/README.md') -Destination (Join-Path $stage 'Licenses/OptiScaler-wiki-profiles.md') -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE'), (Join-Path $root 'NOTICE') -Destination $stage -Force
foreach ($doc in @('README.md', 'README.en.md', 'README.es.md')) {
    $p = Join-Path $root $doc
    if (Test-Path -LiteralPath $p -PathType Leaf) { Copy-Item -LiteralPath $p -Destination $stage -Force }
}
if (Test-Path -LiteralPath (Join-Path $root 'docs/legacy-readme') -PathType Container) {
    $legacyReadmeDst = Join-Path $stage 'docs/legacy-readme'
    New-Item -ItemType Directory -Force -Path $legacyReadmeDst | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'docs/legacy-readme') -File | Copy-Item -Destination $legacyReadmeDst -Force
}
Set-Content -LiteralPath (Join-Path $stage 'VERSION') -Value ($Version + "`n") -Encoding UTF8

(@'
@echo off
setlocal
title OptiScaler(NR) @VERSION@ - Uninstall
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -GameDir "%~1" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Uninstall failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@).Replace('@VERSION@', $Version) | Set-Content -LiteralPath (Join-Path $stage 'Uninstall_OptiScaler_NR.bat') -Encoding ASCII

if ($LocalTest) {
    [IO.File]::WriteAllText((Join-Path $stage 'LOCAL-TEST.txt'),
        "Local test package $Version.`n",
        [Text.UTF8Encoding]::new($false))
}

# 9. Forbidden proprietary/model file gate
$forbidden = '(?i)^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|dlssnr(?:-int4|-small)?\.bin|CC_(?:SMALL_FP8|SMALL_FP8_EXPERIMENTAL|SILVER_AARDWOLD|SILVER_AARDWOLD_OPTIMIZED|SILVER_AARDWOLD_OPTIMIZED_INT4)\.bin|version\.dll|dlssnr_on_amd_setup\.exe)$'
$badAll = Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $forbidden }
if ($badAll) {
    throw "Refusing to package proprietary/user-supplied file: $(($badAll | ForEach-Object { $_.FullName.Substring($stage.Length+1) }) -join ', ')"
}

# 10. Sign unstamped PE files (skips 0-byte placeholder openrr.asi automatically)
. (Join-Path $PSScriptRoot 'package-signing.ps1')
Invoke-PackageSigning -Stage $stage -Thumbprint $SigningThumbprint

# 11. Generate SHA256SUMS.txt & ZIP (parallel hashing + multi-threaded 7z Deflate with Compress-Archive fallback)
Initialize-ParallelPackageTool
$stageFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object { $_.FullName })
$hashedPairs = [OptiScaler.ParallelPackageTool]::HashFiles($stage, [string[]]$stageFiles, $Jobs)
$expectedEntries = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
$hashes = New-Object 'System.Collections.Generic.List[string]' ($hashedPairs.Length)
foreach ($pair in $hashedPairs) {
    $expectedEntries[$pair.Key] = $pair.Value
    [void]$hashes.Add(('{0} *{1}' -f $pair.Value, $pair.Key))
}
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'), (($hashes -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
$expectedEntries['SHA256SUMS.txt'] = Get-Sha256 (Join-Path $stage 'SHA256SUMS.txt')

New-Item -ItemType Directory -Force -Path (Join-Path $root $OutDir) | Out-Null
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
$requiredZipEntries = [string[]]@('OptiScaler.dll', 'OptiScaler.ini', 'Setup.ps1', 'install.sh', 'OptiScalerNRLauncher.exe', 'launcher-worker.ps1', 'proxy-probe.ps1', 'library-scan.ps1', 'install-capabilities.ps1', 'xess-compat.ps1', 'proxy-profiles.json', 'OptiScaler/plugins/dlssnr-amd.asi', 'OptiScaler/plugins/openrr-amd.asi', 'OptiScaler/plugins/openrr-ngx-host.dll', 'rtx-remix-mc/remixbridge.dll', 'Licenses/OptiScaler_LICENSE.txt', 'Licenses/OptiScaler_SOURCE.txt', 'Licenses/OptiPatcher_SOURCE.txt', 'Licenses/OpenRR_LICENSE.txt', 'Licenses/OpenRR_THIRD_PARTY.md', 'Licenses/RTX-Remix-Minecraft-AMD-Patch_LICENSE.txt', 'Licenses/RTX-Remix-Minecraft-AMD-Patch_NOTICE.txt', 'Licenses/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt', 'openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.msgpack', 'openrr-amd/aco/gfx1200/openrr-d3d12-v1/manifest.msgpack', 'openrr-amd/lang/zh-CN.txt', 'openrr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf', 'dlssnr-amd/nr_present_probe.exe', 'dlssnr-amd/aco/d3d12_outer.nra', 'dlssnr-amd/aco/rdna3/d3d12_outer.nra', 'dlssnr-amd/lang/zh-CN.txt', 'dlssnr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf', 'model-tools/dlssnr_extract_model.exe', 'experimental/dlssnr-small-experimental.delta', 'optimized/dlssnr-optimized.delta', 'dlssnr-amd-optiscaler.ini', 'dlssnr-amd-optiscaler-renodx.ini', 'dlssnr-amd-standalone.ini', 'dlssnr-amd-addon.ini', 'standalone/dxgi.dll', 'standalone/vk-override/implicit_layer/dlssnr_standalone.json', 'reshade/ReShade64.dll', 'reshade/dlssnr_amd.addon64', 'reshade/ReShade.ini', 'reshade/ReShadePreset.ini', 'reshade/ReShadePreset-d3d9.ini', 'reshade/vk-override/implicit_layer/ReShade64.json', 'reshade/reshade-shaders/Shaders/DLSS5_Feed.fx', 'dxvk/d3d9.dll', 'dxvk/dxgi.dll', 'vulkan/vulkan-1.dll', 'mochizuki-owned.txt')
try {
    $sevenZip = Find-SevenZip
    if ($sevenZip) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $sevenZip
        $psi.WorkingDirectory = $stage
        $psi.Arguments = ('a -tzip -mm=Deflate -mx=6 -mmt={0} -bd -y -- "{1}" *' -f $Jobs, $zip)
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        try {
            $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
            $stderrTask = $proc.StandardError.ReadToEndAsync()
            $proc.WaitForExit()
            $zErr = $stderrTask.Result
            $zOut = $stdoutTask.Result
            if ($proc.ExitCode -ne 0) {
                throw ("7z zip creation failed (exit {0}): {1} {2}" -f $proc.ExitCode, $zErr, $zOut)
            }
        } finally {
            $proc.Dispose()
        }
    } else {
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
    }
    [OptiScaler.ParallelPackageTool]::VerifyZipEntries($zip, $expectedEntries, $forbidden, $requiredZipEntries, $Jobs)
} catch {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    throw "Package archive validation failed; zip removed: $($_.Exception.Message)"
}

Write-Host "Product: $Version" -ForegroundColor Green
Write-Host "Staged:  $stage" -ForegroundColor Green
Write-Host "Zip:     $zip" -ForegroundColor Green
