<#
.SYNOPSIS
  Resolve, verify by SHA-256, and extract pinned upstream ASI payloads
  (DLSSNR-AMD + CJK lang pack, OpenRR-ASI, OptiPatcher) defined in tools/build/upstream-pins.json.
  Reuses matching verified archives in dist/upstream/ or cache/downloads/ before downloading from GitHub,
  and supports local developer overrides via -MochiDir/-MochiArchive/-MochiLangArchive and -OpenRrDir/-OpenRrArchive.
#>
[CmdletBinding()]
param(
    [string]$UpstreamDir = '',
    [string]$CacheRoot = '',
    [string]$MochiArchive = $env:NR_MOCHI_ARCHIVE,
    [string]$MochiArchive32 = $env:NR_MOCHI_ARCHIVE_32,
    [string]$MochiLangArchive = $env:NR_MOCHI_LANG_ARCHIVE,
    [string]$MochiDir = $env:NR_MOCHI_DIR,
    [string]$OpenRrArchive = $env:NR_OPENRR_ARCHIVE,
    [string]$OpenRrDir = $env:NR_OPENRR_DIR,
    [string]$OptiPatcherFile = $env:NR_OPTIPATCHER_FILE,
    [string]$RtxRemixMcArchive = $env:NR_RTX_REMIX_MC_ARCHIVE,
    [switch]$RefreshOptiPatcher
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pinsPath = Join-Path $PSScriptRoot 'upstream-pins.json'
if (-not (Test-Path -LiteralPath $pinsPath -PathType Leaf)) {
    throw "Missing upstream pin manifest: $pinsPath"
}
$pins = Get-Content -LiteralPath $pinsPath -Raw -Encoding UTF8 | ConvertFrom-Json

if (-not $UpstreamDir) { $UpstreamDir = Join-Path $repoRoot (Join-Path 'dist' 'upstream') }
if (-not $CacheRoot)   { $CacheRoot   = Join-Path $repoRoot 'cache' }
$downloadsDir = Join-Path $CacheRoot 'downloads'
$upstreamCache = Join-Path $CacheRoot 'upstream'
New-Item -ItemType Directory -Force -Path $downloadsDir, $upstreamCache | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem

$ForbiddenNamePattern = '(?i)^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|dlssnr(?:-int4|-small)?\.bin|CC_(?:SMALL_FP8|SMALL_FP8_EXPERIMENTAL|SMALL_FP8_OPTIMIZED|SMALL_FP8_OPTIMIZED_INT4|SMALL_FP8_OPTIMIZED_INT4_SINK|SILVER_AARDWOLD|SILVER_AARDWOLD_OPTIMIZED|SILVER_AARDWOLD_OPTIMIZED_INT4)\.bin|pipeline\.cache|version\.dll|dlssnr_on_amd_setup\.exe)$'

function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Assert-PeBinary([string]$Path, [int64]$MinBytes = 4096, [int64]$MaxBytes = 52428800, [int]$ExpectedMachine = 0x8664) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing PE binary: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt $MinBytes -or $bytes.Length -gt $MaxBytes) {
        throw "Implausible PE size ($($bytes.Length) bytes): $Path"
    }
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Missing MZ header: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw "Invalid e_lfanew ($peOffset): $Path"
    }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "Missing PE signature: $Path"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne $ExpectedMachine) {
        throw ("Expected PE machine 0x{0:X4}, found 0x{1:X4}: {2}" -f $ExpectedMachine, $machine, $Path)
    }
}

function Test-Pe64Amd64([string]$Path, [int64]$MinBytes = 4096, [int64]$MaxBytes = 52428800) {
    try {
        Assert-PeBinary -Path $Path -MinBytes $MinBytes -MaxBytes $MaxBytes
        return $true
    } catch {
        return $false
    }
}

function Download-FileWithFallback([string]$Url, [string]$Dest) {
    $part = "$Dest.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
    $curl = Get-Command 'curl.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $curl) {
        $curl = Get-Command 'curl' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    $downloaded = $false
    if ($curl) {
        & $curl.Source -fsSL --retry 3 --connect-timeout 15 --max-time 600 -o $part $Url
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $part -PathType Leaf)) {
            $downloaded = $true
        } else {
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
            Write-Warning "curl failed ($LASTEXITCODE) downloading $Url; falling back to Invoke-WebRequest."
        }
    }
    if (-not $downloaded) {
        try {
            if ([Net.SecurityProtocolType].GetEnumNames() -contains 'Tls12') {
                [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            }
        } catch { }
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $part
    }
    Move-Item -LiteralPath $part -Destination $Dest -Force
}

function Resolve-PinnedArchive([string]$Name, [string]$Url, [string]$ExpectedSha) {
    $expected = $ExpectedSha.ToLowerInvariant()
    if ($UpstreamDir) {
        $localCand = Join-Path $UpstreamDir $Name
        if (Test-Path -LiteralPath $localCand -PathType Leaf) {
            $actual = Get-Sha256Hex $localCand
            if ($actual -ne $expected) {
                throw "SHA-256 mismatch for local upstream archive '$localCand': expected $expected, got $actual"
            }
            Write-Host "Reusing verified upstream archive: $localCand" -ForegroundColor Green
            return $localCand
        }
    }

    $cached = Join-Path $downloadsDir $Name
    if (Test-Path -LiteralPath $cached -PathType Leaf) {
        $actual = Get-Sha256Hex $cached
        if ($actual -eq $expected) {
            Write-Host "Reusing cached verified archive: $cached" -ForegroundColor Green
            return $cached
        }
        Write-Warning "Cached archive SHA-256 mismatch, re-downloading: $cached"
        Remove-Item -LiteralPath $cached -Force
    }

    Write-Host "Downloading $Url -> $cached ..." -ForegroundColor Cyan
    $part = "$cached.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
    Download-FileWithFallback -Url $Url -Dest $cached
    $actual = Get-Sha256Hex $cached
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $cached -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for downloaded archive '$Url': expected $expected, got $actual"
    }
    return $cached
}

function Prefetch-PinnedArchives([object[]]$Specs) {
    if (-not $Specs -or $Specs.Count -le 1) { return }
    $curl = Get-Command 'curl.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $curl) { $curl = Get-Command 'curl' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if (-not $curl) { return }
    $maxJobs = [Math]::Max(1, [Environment]::ProcessorCount)
    $envJobs = 0
    if ($env:JOBS -and [int]::TryParse($env:JOBS, [ref]$envJobs) -and $envJobs -gt 0) { $maxJobs = $envJobs }
    if ($maxJobs -le 1) { return }

    $pending = New-Object System.Collections.Generic.List[hashtable]
    foreach ($spec in $Specs) {
        $name = [string]$spec.Name
        $url = [string]$spec.Url
        $expected = ([string]$spec.ExpectedSha).ToLowerInvariant()
        if ($UpstreamDir -and (Test-Path -LiteralPath (Join-Path $UpstreamDir $name) -PathType Leaf)) { continue }
        $cached = Join-Path $downloadsDir $name
        if ((Test-Path -LiteralPath $cached -PathType Leaf) -and (Get-Sha256Hex $cached) -eq $expected) { continue }
        [void]$pending.Add(@{ Name = $name; Url = $url; Expected = $expected; Cached = $cached; Part = "$cached.part.$([guid]::NewGuid().ToString('N'))" })
    }
    if ($pending.Count -le 1) { return }

    $running = New-Object System.Collections.Generic.List[hashtable]
    $idx = 0
    try {
        while ($idx -lt $pending.Count -or $running.Count -gt 0) {
            while ($idx -lt $pending.Count -and $running.Count -lt $maxJobs) {
                $item = $pending[$idx]
                $idx++
                Write-Host "Downloading $($item.Url) -> $($item.Cached) ..." -ForegroundColor Cyan
                $psi = New-Object System.Diagnostics.ProcessStartInfo
                $psi.FileName = $curl.Source
                $psi.Arguments = ('-fsSL --retry 3 --connect-timeout 15 --max-time 600 -o "{0}" "{1}"' -f $item.Part, $item.Url)
                $psi.UseShellExecute = $false
                $psi.CreateNoWindow = $true
                $proc = [System.Diagnostics.Process]::Start($psi)
                [void]$running.Add(@{ Item = $item; Proc = $proc })
            }
            $finished = $null
            for ($i = 0; $i -lt $running.Count; $i++) {
                if ($running[$i].Proc.HasExited) { $finished = $i; break }
            }
            if ($null -eq $finished) { Start-Sleep -Milliseconds 100; continue }
            $slot = $running[$finished]
            $running.RemoveAt($finished)
            $code = $slot.Proc.ExitCode
            $slot.Proc.Dispose()
            $item = $slot.Item
            if ($code -ne 0 -or -not (Test-Path -LiteralPath $item.Part -PathType Leaf)) {
                if (Test-Path -LiteralPath $item.Part) { Remove-Item -LiteralPath $item.Part -Force -ErrorAction SilentlyContinue }
                throw "Parallel curl download failed (exit $code) for $($item.Url)"
            }
            $actual = Get-Sha256Hex $item.Part
            if ($actual -ne $item.Expected) {
                Remove-Item -LiteralPath $item.Part -Force -ErrorAction SilentlyContinue
                throw "SHA-256 mismatch for downloaded archive '$($item.Url)': expected $($item.Expected), got $actual"
            }
            Move-Item -LiteralPath $item.Part -Destination $item.Cached -Force
        }
    } catch {
        foreach ($slot in $running) {
            try { if (-not $slot.Proc.HasExited) { $slot.Proc.Kill() } } catch { }
            try { $slot.Proc.Dispose() } catch { }
            if (Test-Path -LiteralPath $slot.Item.Part) { Remove-Item -LiteralPath $slot.Item.Part -Force -ErrorAction SilentlyContinue }
        }
        throw
    }
}

$script:CreatedDirs = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)

function Join-RelPath([string]$BaseDir, [string]$RelPath) {
    return Join-Path $BaseDir ($RelPath.Replace('/', [IO.Path]::DirectorySeparatorChar))
}

function Write-ZipEntryToFile($Entry, [string]$DestPath) {
    $dir = [IO.Path]::GetDirectoryName($DestPath)
    if ($dir -and $script:CreatedDirs.Add($dir)) {
        [void][IO.Directory]::CreateDirectory($dir)
    }
    $inStream = $Entry.Open()
    try {
        $outStream = [IO.File]::Create($DestPath)
        try {
            $inStream.CopyTo($outStream)
        } finally {
            $outStream.Dispose()
        }
    } finally {
        $inStream.Dispose()
    }
}

function Copy-FileToRel([string]$SrcPath, [string]$BaseDir, [string]$DestRel) {
    $destPath = Join-RelPath $BaseDir $DestRel
    $dir = [IO.Path]::GetDirectoryName($destPath)
    if ($dir -and $script:CreatedDirs.Add($dir)) {
        [void][IO.Directory]::CreateDirectory($dir)
    }
    [IO.File]::Copy($SrcPath, $destPath, $true)
}

function Normalize-ZipEntryRel([string]$FullName) {
    $rel = $FullName.Replace('\', '/')
    if ($rel.StartsWith('/') -or $rel -match '(?:^|/)\.\.(?:$|/)' -or $rel -match '^[A-Za-z]:') {
        throw "Unsafe zip entry path: $FullName"
    }
    return $rel
}

function Test-ForbiddenPayloadRel([string]$Rel) {
    $norm = $Rel.Replace('\', '/')
    $leaf = [IO.Path]::GetFileName($norm)
    if ($leaf -match $ForbiddenNamePattern) { return $true }
    if ($norm.StartsWith('dlssnr-amd/aco-cache/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($norm.StartsWith('openrr-amd/cache/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    return $false
}

function Get-DlssnrDestRel([string]$Rel, [switch]$AllowNormalizedFallback, [string]$SourceRoot = '') {
    $norm = $Rel.Replace('\', '/')
    if ($norm -eq 'LICENSE') { return 'Licenses/DLSSNR-AMD_LICENSE.txt' }
    if ($norm -eq 'NOTICE') { return 'Licenses/DLSSNR-AMD_NOTICE.txt' }
    if ($norm -eq 'optiscaler/asi/dlssnr-amd.asi') { return 'OptiScaler/plugins/dlssnr-amd.asi' }
    if ($norm -eq 'optiscaler/asi/nr_present_probe.exe') { return 'nr_present_probe.exe' }
    if ($norm -eq 'optiscaler/asi/DearImGui-LICENSE.txt') { return 'Licenses/DearImGui-LICENSE.txt' }
    if ($norm -eq 'optiscaler/asi/Inter-LICENSE.txt') { return 'Licenses/Inter-LICENSE.txt' }
    if ($norm -in @('ini/dlssnr-amd-optiscaler.ini', 'ini/dlssnr-amd-optiscaler-renodx.ini', 'ini/dlssnr-amd-standalone.ini', 'ini/dlssnr-amd-addon.ini')) { return $norm }
    if ($norm -eq 'standalone/dxgi.dll' -or $norm.StartsWith('standalone/vk-override/')) { return $norm }
    if ($norm.StartsWith('reshade/') -or $norm.StartsWith('dxvk/') -or $norm.StartsWith('vulkan/')) { return $norm }
    if ($AllowNormalizedFallback) {
        if ($norm -in @('OptiScaler/plugins/dlssnr-amd.asi', 'nr_present_probe.exe', 'Licenses/DearImGui-LICENSE.txt', 'Licenses/Inter-LICENSE.txt')) {
            return $norm
        }
        if ($norm -in @('dlssnr-amd-optiscaler.ini', 'dlssnr-amd-optiscaler-renodx.ini', 'dlssnr-amd-standalone.ini', 'dlssnr-amd-addon.ini')) {
            return 'ini/' + $norm
        }
        if ($norm -eq 'standalone/dlssnr-amd.asi' -and $SourceRoot -and -not (Test-Path -LiteralPath (Join-RelPath $SourceRoot 'optiscaler/asi/dlssnr-amd.asi') -PathType Leaf)) {
            return 'OptiScaler/plugins/dlssnr-amd.asi'
        }
        if ($norm -eq 'standalone/nr_present_probe.exe' -and $SourceRoot -and -not (Test-Path -LiteralPath (Join-RelPath $SourceRoot 'optiscaler/asi/nr_present_probe.exe') -PathType Leaf)) {
            return 'nr_present_probe.exe'
        }
        if ($norm -eq 'dlssnr-amd/nr_present_probe.exe' -and $SourceRoot -and
            -not (Test-Path -LiteralPath (Join-RelPath $SourceRoot 'optiscaler/asi/nr_present_probe.exe') -PathType Leaf) -and
            -not (Test-Path -LiteralPath (Join-RelPath $SourceRoot 'nr_present_probe.exe') -PathType Leaf)) {
            return 'nr_present_probe.exe'
        }
    }
    if ($norm.StartsWith('model-tools/') -or $norm.StartsWith('experimental/') -or
        $norm.StartsWith('optimized/') -or $norm.StartsWith('CC_SMALL_FP8_OPTIMIZED/') -or
        $norm.StartsWith('CC_SILVER_AARDWOLD_OPTIMIZED/') -or $norm.StartsWith('dlssnr-amd/')) {
        return $norm
    }
    return $null
}

function Get-Dlssnr32DestRel([string]$Rel) {
    $norm = $Rel.Replace('\', '/')
    if ($norm -in @(
        'reshade/ReShade32.dll',
        'reshade/dlssnr_amd.addon32',
        'reshade/vk-override/implicit_layer/ReShade32.json'
    )) { return $norm }
    if ($norm -eq 'dxvk/d3d9.dll') { return 'dxvk/x86/d3d9.dll' }
    if ($norm -eq 'dxvk/dxgi.dll') { return 'dxvk/x86/dxgi.dll' }
    if ($norm -eq 'vulkan/vulkan-1.dll') { return 'vulkan/x86/vulkan-1.dll' }
    if ($norm -eq 'vulkan/Vulkan-Loader-LICENSE.txt') { return 'vulkan/x86/Vulkan-Loader-LICENSE.txt' }
    if ($norm -eq 'vulkan/Vulkan-Loader-PATCHES.diff') { return 'vulkan/x86/Vulkan-Loader-PATCHES.diff' }
    return $null
}

function Get-OpenRrDestRel([string]$Rel, [switch]$AllowNormalizedFallback, [string]$SourceRoot = '') {
    $norm = $Rel.Replace('\', '/')
    if ($norm -eq 'OptiScaler/plugins/openrr-amd.asi') { return 'OptiScaler/plugins/openrr-amd.asi' }
    if ($norm -eq 'openrr-amd.ini') { return 'openrr-amd.ini' }
    if ($AllowNormalizedFallback) {
        if ($norm -in @('openrr-amd.asi', 'plugins/openrr-amd.asi', 'OptiScaler/plugins/openrr.asi', 'plugins/openrr.asi')) {
            return 'OptiScaler/plugins/openrr-amd.asi'
        }
        if ($norm -eq 'OptiScaler/openrr-amd.ini') { return 'openrr-amd.ini' }
        if ($norm.StartsWith('Licenses/')) { return $norm }
    }
    if ($norm.StartsWith('openrr-amd/')) { return $norm }
    return $null
}

# --- 1. Prepare DLSSNR-AMD payload (dlssnr-amd x86_64 + i686 32-bit runtime overlay + CJK lang pack + static license notices) ---
$isCustomDlssnr = [bool]($MochiDir -or $MochiArchive -or $MochiArchive32 -or $MochiLangArchive)
$dlssnrDir = Join-Path $upstreamCache $(if ($isCustomDlssnr) { 'dlssnr-amd-custom' } else { 'dlssnr-amd' })
$dlssnrStampPath = Join-Path $dlssnrDir '.stamp.json'
$staticLicenseSrcDir = Join-Path (Join-Path $repoRoot 'assets') 'licenses'
$staticLicenseFiles = @('Mochizuki_MIT.txt', 'Mochizuki_ASI_SOURCES.md', 'RenoDX_MIT.txt')
$hasPinnedArchive32 = [bool]($pins.dlssnr.PSObject.Properties['archive32Url'] -and $pins.dlssnr.archive32Url)
$pinnedArchive32Sha = if ($hasPinnedArchive32) { ([string]$pins.dlssnr.archive32Sha256).ToLowerInvariant() } else { '' }

$staticLicenseDigestBuilder = New-Object System.Text.StringBuilder
$resolvedStaticLicenses = @()
foreach ($licName in $staticLicenseFiles) {
    $licPath = Join-Path $staticLicenseSrcDir $licName
    if (-not (Test-Path -LiteralPath $licPath -PathType Leaf)) {
        throw "Missing required static Mochizuki attribution notice: $licPath"
    }
    [void]$staticLicenseDigestBuilder.Append("$licName=$(Get-Sha256Hex $licPath);")
    $resolvedStaticLicenses += @{ Name = $licName; Path = $licPath }
}
$shaTmp = [System.Security.Cryptography.SHA256]::Create()
try {
    $staticLicHash = ([BitConverter]::ToString($shaTmp.ComputeHash([Text.Encoding]::UTF8.GetBytes($staticLicenseDigestBuilder.ToString())))).Replace('-', '').ToLowerInvariant()
} finally { $shaTmp.Dispose() }

function Extract-DlssnrLangZip([string]$LangZipPath, [string]$DestStage, [switch]$AllowSkipForbidden) {
    $langArchive = [IO.Compression.ZipFile]::OpenRead($LangZipPath)
    try {
        foreach ($entry in $langArchive.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) { continue }
            $rel = Normalize-ZipEntryRel $entry.FullName
            if (-not $rel.StartsWith('lang/')) { continue }
            $destRel = 'dlssnr-amd/' + $rel
            if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                if ($AllowSkipForbidden) {
                    Write-Host "  Skipping excluded weight/cache file in custom lang pack: $rel" -ForegroundColor DarkYellow
                    continue
                }
                throw "Refusing forbidden or cache entry in pinned upstream archive: $rel"
            }
            Write-ZipEntryToFile -Entry $entry -DestPath (Join-RelPath $DestStage $destRel)
        }
    } finally {
        $langArchive.Dispose()
    }
}

function Extract-Dlssnr32Overlay([string]$Zip32Path, [string]$DestStage, [switch]$AllowSkipForbidden) {
    $archive32 = [IO.Compression.ZipFile]::OpenRead($Zip32Path)
    try {
        foreach ($entry in $archive32.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) { continue }
            $rel = Normalize-ZipEntryRel $entry.FullName
            $destRel = Get-Dlssnr32DestRel $rel
            if (-not $destRel) { continue }
            if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                if ($AllowSkipForbidden) {
                    Write-Host "  Skipping excluded weight/cache file in 32-bit archive: $rel" -ForegroundColor DarkYellow
                    continue
                }
                throw "Refusing forbidden or cache entry in pinned 32-bit upstream archive: $rel"
            }
            Write-ZipEntryToFile -Entry $entry -DestPath (Join-RelPath $DestStage $destRel)
        }
    } finally {
        $archive32.Dispose()
    }
}

function Stage-DlssnrPayload([string]$DestDir, [bool]$CustomMode) {
    $stageTmp = "$DestDir.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Force -Path $stageTmp | Out-Null
        if ($CustomMode -and $MochiDir) {
            if (-not (Test-Path -LiteralPath $MochiDir -PathType Container)) {
                throw "Provided -MochiDir not found: $MochiDir"
            }
            Write-Host "Staging custom DLSSNR-AMD directory: $MochiDir" -ForegroundColor Cyan
            $srcPrefixLen = [IO.Path]::GetFullPath($MochiDir).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Length + 1
            foreach ($file in Get-ChildItem -LiteralPath $MochiDir -Recurse -File) {
                $rel = Normalize-ZipEntryRel ($file.FullName.Substring($srcPrefixLen))
                $destRel = Get-DlssnrDestRel -Rel $rel -AllowNormalizedFallback -SourceRoot $MochiDir
                if (-not $destRel) { continue }
                if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                    Write-Host "  Skipping excluded weight/cache file from -MochiDir: $rel" -ForegroundColor DarkYellow
                    continue
                }
                Copy-FileToRel -SrcPath $file.FullName -BaseDir $stageTmp -DestRel $destRel
            }
        } else {
            if ($CustomMode -and $MochiArchive) {
                if (-not (Test-Path -LiteralPath $MochiArchive -PathType Leaf)) {
                    throw "Provided -MochiArchive not found: $MochiArchive"
                }
                Write-Host "Staging custom DLSSNR-AMD archive: $MochiArchive" -ForegroundColor Cyan
                $dlssnrZip = $MochiArchive
            } else {
                $dlssnrZip = Resolve-PinnedArchive -Name $pins.dlssnr.archiveName -Url $pins.dlssnr.archiveUrl -ExpectedSha $pins.dlssnr.archiveSha256
            }
            $archive = [IO.Compression.ZipFile]::OpenRead($dlssnrZip)
            try {
                foreach ($entry in $archive.Entries) {
                    if ([string]::IsNullOrEmpty($entry.Name)) { continue }
                    $rel = Normalize-ZipEntryRel $entry.FullName
                    $destRel = if ($CustomMode) {
                        Get-DlssnrDestRel -Rel $rel -AllowNormalizedFallback
                    } else {
                        Get-DlssnrDestRel -Rel $rel
                    }
                    if (-not $destRel) { continue }
                    if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                        if ($CustomMode) {
                            Write-Host "  Skipping excluded weight/cache file from -MochiArchive: $rel" -ForegroundColor DarkYellow
                            continue
                        }
                        throw "Refusing forbidden or cache entry in pinned upstream archive: $rel"
                    }
                    Write-ZipEntryToFile -Entry $entry -DestPath (Join-RelPath $stageTmp $destRel)
                }
            } finally {
                $archive.Dispose()
            }
        }

        if ($MochiArchive32) {
            if (-not (Test-Path -LiteralPath $MochiArchive32 -PathType Leaf)) {
                throw "Provided -MochiArchive32 not found: $MochiArchive32"
            }
            Write-Host "Overlaying custom 32-bit DLSSNR-AMD runtime archive: $MochiArchive32" -ForegroundColor Cyan
            Extract-Dlssnr32Overlay -Zip32Path $MochiArchive32 -DestStage $stageTmp -AllowSkipForbidden
        } elseif ($hasPinnedArchive32 -and (-not (Test-Path -LiteralPath (Join-RelPath $stageTmp 'reshade/dlssnr_amd.addon32') -PathType Leaf))) {
            $dlssnrZip32 = Resolve-PinnedArchive -Name $pins.dlssnr.archive32Name -Url $pins.dlssnr.archive32Url -ExpectedSha $pins.dlssnr.archive32Sha256
            Extract-Dlssnr32Overlay -Zip32Path $dlssnrZip32 -DestStage $stageTmp
        }

        if ($MochiLangArchive) {
            if (-not (Test-Path -LiteralPath $MochiLangArchive -PathType Leaf)) {
                throw "Provided -MochiLangArchive not found: $MochiLangArchive"
            }
            Write-Host "Overlaying custom DLSSNR-AMD CJK language archive: $MochiLangArchive" -ForegroundColor Cyan
            Extract-DlssnrLangZip -LangZipPath $MochiLangArchive -DestStage $stageTmp -AllowSkipForbidden
        } elseif (-not $CustomMode -or -not (Test-Path -LiteralPath (Join-RelPath $stageTmp 'dlssnr-amd/lang/zh-CN.txt') -PathType Leaf)) {
            $dlssnrLangZip = Resolve-PinnedArchive -Name $pins.dlssnr.langArchiveName -Url $pins.dlssnr.langArchiveUrl -ExpectedSha $pins.dlssnr.langArchiveSha256
            Extract-DlssnrLangZip -LangZipPath $dlssnrLangZip -DestStage $stageTmp
        }

        $licDestDir = Join-Path $stageTmp 'Licenses'
        New-Item -ItemType Directory -Force -Path $licDestDir | Out-Null
        foreach ($lic in $resolvedStaticLicenses) {
            Copy-Item -LiteralPath $lic.Path -Destination (Join-Path $licDestDir $lic.Name) -Force
        }
        foreach ($routeLic in @(
            @('reshade/ReShade-LICENSE.md', 'ReShade-LICENSE.md'),
            @('dxvk/LICENSE.txt', 'DXVK-LICENSE.txt'),
            @('vulkan/Vulkan-Loader-LICENSE.txt', 'Vulkan-Loader-LICENSE.txt'),
            @('reshade/reshade-shaders/Shaders/vort-LICENSE.txt', 'vort-LICENSE.txt'),
            @('reshade/reshade-shaders/Shaders/DLSS5_Feed-LICENSE.txt', 'DLSS5_Feed-LICENSE.txt')
        )) {
            $rlSrc = Join-RelPath $stageTmp $routeLic[0]
            if (Test-Path -LiteralPath $rlSrc -PathType Leaf) {
                Copy-Item -LiteralPath $rlSrc -Destination (Join-Path $licDestDir $routeLic[1]) -Force
            }
        }

        Assert-PeBinary -Path (Join-RelPath $stageTmp 'OptiScaler/plugins/dlssnr-amd.asi')
        Assert-PeBinary -Path (Join-RelPath $stageTmp 'nr_present_probe.exe')
        Assert-PeBinary -Path (Join-RelPath $stageTmp 'model-tools/dlssnr_extract_model.exe')
        Assert-PeBinary -Path (Join-RelPath $stageTmp 'model-tools/dlssnr-int4-weights.exe')
        foreach ($routePe in @(
            'standalone/dxgi.dll',
            'reshade/ReShade64.dll',
            'reshade/dlssnr_amd.addon64',
            'dxvk/d3d9.dll',
            'dxvk/dxgi.dll',
            'vulkan/vulkan-1.dll'
        )) {
            Assert-PeBinary -Path (Join-RelPath $stageTmp $routePe)
        }
        foreach ($routePe in @('reshade/ReShade32.dll', 'reshade/dlssnr_amd.addon32',
                'dxvk/x86/d3d9.dll', 'dxvk/x86/dxgi.dll', 'vulkan/x86/vulkan-1.dll')) {
            Assert-PeBinary -Path (Join-RelPath $stageTmp $routePe) -ExpectedMachine 0x014C
        }

        if (-not $CustomMode) {
            $wantDlssnrStamp = @{
                schema            = 4
                archiveSha256     = $pins.dlssnr.archiveSha256.ToLowerInvariant()
                archive32Sha256   = $pinnedArchive32Sha
                langArchiveSha256 = $pins.dlssnr.langArchiveSha256.ToLowerInvariant()
                staticLicenseSha  = $staticLicHash
            }
            [IO.File]::WriteAllText((Join-Path $stageTmp '.stamp.json'), (($wantDlssnrStamp | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
        }
        if (Test-Path -LiteralPath $DestDir) { Remove-Item -LiteralPath $DestDir -Recurse -Force }
        Move-Item -LiteralPath $stageTmp -Destination $DestDir -Force
    } catch {
        if (Test-Path -LiteralPath $stageTmp) { Remove-Item -LiteralPath $stageTmp -Recurse -Force -ErrorAction SilentlyContinue }
        throw
    }
}

# Pre-download any missing pinned archives concurrently (largest first) before staging.
$prefetchSpecs = New-Object System.Collections.Generic.List[hashtable]
if (-not $isCustomDlssnr) {
    [void]$prefetchSpecs.Add(@{ Name = $pins.dlssnr.archiveName; Url = $pins.dlssnr.archiveUrl; ExpectedSha = $pins.dlssnr.archiveSha256 })
    if ($hasPinnedArchive32) {
        [void]$prefetchSpecs.Add(@{ Name = $pins.dlssnr.archive32Name; Url = $pins.dlssnr.archive32Url; ExpectedSha = $pins.dlssnr.archive32Sha256 })
    }
    [void]$prefetchSpecs.Add(@{ Name = $pins.dlssnr.langArchiveName; Url = $pins.dlssnr.langArchiveUrl; ExpectedSha = $pins.dlssnr.langArchiveSha256 })
}
if (-not ($OpenRrDir -or $OpenRrArchive)) {
    [void]$prefetchSpecs.Add(@{ Name = $pins.openrr.archiveName; Url = $pins.openrr.archiveUrl; ExpectedSha = $pins.openrr.archiveSha256 })
}
if (-not $RtxRemixMcArchive) {
    [void]$prefetchSpecs.Add(@{ Name = $pins.rtxRemixMc.archiveName; Url = $pins.rtxRemixMc.archiveUrl; ExpectedSha = $pins.rtxRemixMc.archiveSha256 })
}
Prefetch-PinnedArchives -Specs $prefetchSpecs.ToArray()

if ($isCustomDlssnr) {
    Stage-DlssnrPayload -DestDir $dlssnrDir -CustomMode $true
    Write-Host "Staged custom DLSSNR-AMD payload -> $dlssnrDir" -ForegroundColor Green
} else {
    $wantDlssnrStamp = @{
        schema            = 4
        archiveSha256     = $pins.dlssnr.archiveSha256.ToLowerInvariant()
        archive32Sha256   = $pinnedArchive32Sha
        langArchiveSha256 = $pins.dlssnr.langArchiveSha256.ToLowerInvariant()
        staticLicenseSha  = $staticLicHash
    }
    $dlssnrCachedOk = $false
    if ((Test-Path -LiteralPath $dlssnrStampPath -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $dlssnrDir 'OptiScaler/plugins/dlssnr-amd.asi') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $dlssnrDir 'dlssnr-amd/lang/zh-CN.txt') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $dlssnrDir 'standalone/dxgi.dll') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $dlssnrDir 'reshade/dlssnr_amd.addon64') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $dlssnrDir 'reshade/dlssnr_amd.addon32') -PathType Leaf)) {
        try {
            $cur = Get-Content -LiteralPath $dlssnrStampPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($cur.schema -eq $wantDlssnrStamp.schema -and
                $cur.archiveSha256 -eq $wantDlssnrStamp.archiveSha256 -and
                $cur.archive32Sha256 -eq $wantDlssnrStamp.archive32Sha256 -and
                $cur.langArchiveSha256 -eq $wantDlssnrStamp.langArchiveSha256 -and
                $cur.staticLicenseSha -eq $wantDlssnrStamp.staticLicenseSha) {
                $dlssnrCachedOk = $true
            }
        } catch { $dlssnrCachedOk = $false }
    }
    if (-not $dlssnrCachedOk) {
        Stage-DlssnrPayload -DestDir $dlssnrDir -CustomMode $false
        Write-Host "Extracted DLSSNR-AMD $($pins.dlssnr.version) + 32-bit runtime + CJK lang pack -> $dlssnrDir" -ForegroundColor Green
    }
}

# --- 2. Prepare OpenRR-ASI payload ---
$isCustomOpenRr = [bool]($OpenRrDir -or $OpenRrArchive)
$openRrDir = Join-Path $upstreamCache $(if ($isCustomOpenRr) { 'openrr-custom' } else { 'openrr' })
$openRrStampPath = Join-Path $openRrDir '.stamp.json'

function Stage-OpenRrPayload([string]$DestDir, [bool]$CustomMode) {
    $stageTmp = "$DestDir.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Force -Path $stageTmp | Out-Null
        if ($CustomMode -and $OpenRrDir) {
            if (-not (Test-Path -LiteralPath $OpenRrDir -PathType Container)) {
                throw "Provided -OpenRrDir not found: $OpenRrDir"
            }
            Write-Host "Staging custom OpenRR-ASI directory: $OpenRrDir" -ForegroundColor Cyan
            $srcPrefixLen = [IO.Path]::GetFullPath($OpenRrDir).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Length + 1
            foreach ($file in Get-ChildItem -LiteralPath $OpenRrDir -Recurse -File) {
                $rel = Normalize-ZipEntryRel ($file.FullName.Substring($srcPrefixLen))
                $destRel = Get-OpenRrDestRel -Rel $rel -AllowNormalizedFallback -SourceRoot $OpenRrDir
                if (-not $destRel) { continue }
                if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                    Write-Host "  Skipping excluded weight/cache file from -OpenRrDir: $rel" -ForegroundColor DarkYellow
                    continue
                }
                Copy-FileToRel -SrcPath $file.FullName -BaseDir $stageTmp -DestRel $destRel
            }
        } else {
            if ($CustomMode -and $OpenRrArchive) {
                if (-not (Test-Path -LiteralPath $OpenRrArchive -PathType Leaf)) {
                    throw "Provided -OpenRrArchive not found: $OpenRrArchive"
                }
                Write-Host "Staging custom OpenRR-ASI archive: $OpenRrArchive" -ForegroundColor Cyan
                $openRrZip = $OpenRrArchive
            } else {
                $openRrZip = Resolve-PinnedArchive -Name $pins.openrr.archiveName -Url $pins.openrr.archiveUrl -ExpectedSha $pins.openrr.archiveSha256
            }
            $archive = [IO.Compression.ZipFile]::OpenRead($openRrZip)
            try {
                foreach ($entry in $archive.Entries) {
                    if ([string]::IsNullOrEmpty($entry.Name)) { continue }
                    $rel = Normalize-ZipEntryRel $entry.FullName
                    $destRel = if ($CustomMode) {
                        Get-OpenRrDestRel -Rel $rel -AllowNormalizedFallback
                    } else {
                        Get-OpenRrDestRel -Rel $rel
                    }
                    if (-not $destRel) { continue }
                    if ((Test-ForbiddenPayloadRel $rel) -or (Test-ForbiddenPayloadRel $destRel)) {
                        if ($CustomMode) {
                            Write-Host "  Skipping excluded weight/cache file from -OpenRrArchive: $rel" -ForegroundColor DarkYellow
                            continue
                        }
                        throw "Refusing forbidden or cache entry in pinned upstream archive: $rel"
                    }
                    Write-ZipEntryToFile -Entry $entry -DestPath (Join-RelPath $stageTmp $destRel)
                }
            } finally {
                $archive.Dispose()
            }
        }

        $rrLicDir = Join-Path $stageTmp 'Licenses'
        New-Item -ItemType Directory -Force -Path $rrLicDir | Out-Null
        $rrLegacyMit = Join-Path $rrLicDir 'OpenRR_MIT.txt'
        if (Test-Path -LiteralPath $rrLegacyMit -PathType Leaf) {
            Remove-Item -LiteralPath $rrLegacyMit -Force -ErrorAction SilentlyContinue
        }
        $rrLic = Join-RelPath $stageTmp 'openrr-amd/OpenRR_LICENSE.txt'
        $rrNotice = Join-RelPath $stageTmp 'openrr-amd/OpenRR_NOTICE.txt'
        $rrThird = Join-RelPath $stageTmp 'openrr-amd/OpenRR_THIRD_PARTY.md'
        $rrLicDest = Join-Path $rrLicDir 'OpenRR_LICENSE.txt'
        $rrNoticeDest = Join-Path $rrLicDir 'OpenRR_NOTICE.txt'
        $rrThirdDest = Join-Path $rrLicDir 'OpenRR_THIRD_PARTY.md'
        if ((Test-Path -LiteralPath $rrLic -PathType Leaf) -and -not (Test-Path -LiteralPath $rrLicDest -PathType Leaf)) {
            Copy-Item -LiteralPath $rrLic -Destination $rrLicDest -Force
        }
        if ((Test-Path -LiteralPath $rrNotice -PathType Leaf) -and -not (Test-Path -LiteralPath $rrNoticeDest -PathType Leaf)) {
            Copy-Item -LiteralPath $rrNotice -Destination $rrNoticeDest -Force
        }
        if ((Test-Path -LiteralPath $rrThird -PathType Leaf) -and -not (Test-Path -LiteralPath $rrThirdDest -PathType Leaf)) {
            Copy-Item -LiteralPath $rrThird -Destination $rrThirdDest -Force
        }
        if (-not (Test-Path -LiteralPath $rrLicDest -PathType Leaf)) { throw 'Missing OpenRR_LICENSE.txt / Licenses/OpenRR_LICENSE.txt in OpenRR payload' }
        if (-not (Test-Path -LiteralPath $rrThirdDest -PathType Leaf)) { throw 'Missing OpenRR_THIRD_PARTY.md / Licenses/OpenRR_THIRD_PARTY.md in OpenRR payload' }

        Assert-PeBinary -Path (Join-RelPath $stageTmp 'OptiScaler/plugins/openrr-amd.asi')

        if (-not $CustomMode) {
            $wantOpenRrStamp = @{
                schema        = 2
                archiveSha256 = $pins.openrr.archiveSha256.ToLowerInvariant()
            }
            [IO.File]::WriteAllText((Join-Path $stageTmp '.stamp.json'), (($wantOpenRrStamp | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
        }
        if (Test-Path -LiteralPath $DestDir) { Remove-Item -LiteralPath $DestDir -Recurse -Force }
        Move-Item -LiteralPath $stageTmp -Destination $DestDir -Force
    } catch {
        if (Test-Path -LiteralPath $stageTmp) { Remove-Item -LiteralPath $stageTmp -Recurse -Force -ErrorAction SilentlyContinue }
        throw
    }
}

if ($isCustomOpenRr) {
    Stage-OpenRrPayload -DestDir $openRrDir -CustomMode $true
    Write-Host "Staged custom OpenRR-ASI payload -> $openRrDir" -ForegroundColor Green
} else {
    $wantOpenRrStamp = @{
        schema        = 2
        archiveSha256 = $pins.openrr.archiveSha256.ToLowerInvariant()
    }
    $openRrCachedOk = $false
    if ((Test-Path -LiteralPath $openRrStampPath -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $openRrDir 'OptiScaler/plugins/openrr-amd.asi') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $openRrDir 'openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.msgpack') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $openRrDir 'Licenses/OpenRR_LICENSE.txt') -PathType Leaf)) {
        try {
            $cur = Get-Content -LiteralPath $openRrStampPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($cur.schema -eq $wantOpenRrStamp.schema -and $cur.archiveSha256 -eq $wantOpenRrStamp.archiveSha256) {
                $openRrCachedOk = $true
            }
        } catch { $openRrCachedOk = $false }
    }
    if (-not $openRrCachedOk) {
        Stage-OpenRrPayload -DestDir $openRrDir -CustomMode $false
        Write-Host "Extracted OpenRR-ASI $($pins.openrr.version) -> $openRrDir" -ForegroundColor Green
    }
}

# --- 3. Resolve OptiPatcher.asi (best-effort rolling refresh in CI, local/cached fallbacks) ---
$optiPatcherOut = Join-RelPath $upstreamCache 'optipatcher/OptiScaler/plugins/OptiPatcher.asi'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $optiPatcherOut) | Out-Null
$optiPatcherCache = Join-Path $downloadsDir $pins.optipatcher.fileName
$optiPatcherResolved = $null

function Try-DownloadOptiPatcher {
    $tmp = "$optiPatcherCache.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        Download-FileWithFallback -Url $pins.optipatcher.url -Dest $tmp
        Assert-PeBinary -Path $tmp -MinBytes 4096 -MaxBytes 20971520
        Move-Item -LiteralPath $tmp -Destination $optiPatcherCache -Force
        return $optiPatcherCache
    } catch {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
        Write-Warning "OptiPatcher rolling download failed ($($_.Exception.Message)); trying fallback copies."
        return $null
    }
}

if ($OptiPatcherFile) {
    if (-not (Test-Path -LiteralPath $OptiPatcherFile -PathType Leaf)) {
        throw "Provided -OptiPatcherFile not found: $OptiPatcherFile"
    }
    Assert-PeBinary -Path $OptiPatcherFile -MinBytes 4096 -MaxBytes 20971520
    $optiPatcherResolved = $OptiPatcherFile
    Write-Host "Using provided OptiPatcher.asi: $OptiPatcherFile" -ForegroundColor Green
} elseif ($RefreshOptiPatcher -and $pins.optipatcher.allowRollingRefresh) {
    $optiPatcherResolved = Try-DownloadOptiPatcher
    if ($optiPatcherResolved) {
        Write-Host "Refreshed OptiPatcher.asi from rolling release -> $optiPatcherCache" -ForegroundColor Green
    }
}
if (-not $optiPatcherResolved) {
    foreach ($cand in @(
        $(if ($UpstreamDir) { Join-Path $UpstreamDir $pins.optipatcher.fileName } else { '' }),
        $optiPatcherCache
    )) {
        if ($cand -and (Test-Pe64Amd64 -Path $cand -MinBytes 4096 -MaxBytes 20971520)) {
            $optiPatcherResolved = $cand
            Write-Host "Reusing verified OptiPatcher.asi: $cand" -ForegroundColor Green
            break
        }
    }
}
if (-not $optiPatcherResolved) {
    $optiPatcherResolved = Try-DownloadOptiPatcher
}
if (-not $optiPatcherResolved) {
    throw "Failed to resolve valid AMD64 OptiPatcher.asi from -OptiPatcherFile, dist/upstream, cache/downloads, or $($pins.optipatcher.url)."
}
Copy-Item -LiteralPath $optiPatcherResolved -Destination $optiPatcherOut -Force

# --- 4. Prepare RTX Remix for Minecraft AMD patch payload (skipping bundled OptiScaler.ini) ---
$isCustomRtxRemixMc = [bool]$RtxRemixMcArchive
$rtxRemixMcDir = Join-Path $upstreamCache $(if ($isCustomRtxRemixMc) { 'rtx-remix-mc-custom' } else { 'rtx-remix-mc' })
$rtxRemixMcStampPath = Join-Path $rtxRemixMcDir '.stamp.json'

function Stage-RtxRemixMcPayload([string]$DestDir, [bool]$CustomMode) {
    $stageTmp = "$DestDir.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Force -Path $stageTmp | Out-Null
        if ($CustomMode) {
            if (-not (Test-Path -LiteralPath $RtxRemixMcArchive -PathType Leaf)) {
                throw "Provided -RtxRemixMcArchive not found: $RtxRemixMcArchive"
            }
            Write-Host "Staging custom RTX Remix Minecraft patch archive: $RtxRemixMcArchive" -ForegroundColor Cyan
            $mcZip = $RtxRemixMcArchive
        } else {
            $mcZip = Resolve-PinnedArchive -Name $pins.rtxRemixMc.archiveName -Url $pins.rtxRemixMc.archiveUrl -ExpectedSha $pins.rtxRemixMc.archiveSha256
        }

        $entryMap = @{
            'remixbridge.dll' = 'rtx-remix-mc/remixbridge.dll'
            'LICENSE' = 'Licenses/RTX-Remix-Minecraft-AMD-Patch_LICENSE.txt'
            'NOTICE' = 'Licenses/RTX-Remix-Minecraft-AMD-Patch_NOTICE.txt'
            'THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt' = 'Licenses/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt'
        }
        $optionalEntryMap = @{
            'THIRD_PARTY_LICENSES/NVIDIA-remix-c-api-MIT.txt' = 'Licenses/NVIDIA-remix-c-api-MIT.txt'
        }
        $seenEntries = @{}
        $archive = [IO.Compression.ZipFile]::OpenRead($mcZip)
        try {
            foreach ($entry in $archive.Entries) {
                if ([string]::IsNullOrEmpty($entry.Name)) { continue }
                $rel = Normalize-ZipEntryRel $entry.FullName
                $destRel = $null
                if ($entryMap.ContainsKey($rel)) {
                    $destRel = $entryMap[$rel]
                } elseif ($optionalEntryMap.ContainsKey($rel)) {
                    $destRel = $optionalEntryMap[$rel]
                } else {
                    # Intentionally skip OptiScaler.ini (patched dynamically from official OptiScaler.ini at install time)
                    continue
                }
                if ((Test-ForbiddenPayloadRel $rel)) {
                    throw "Refusing forbidden entry in RTX Remix Minecraft archive: $rel"
                }
                $destPath = Join-RelPath $stageTmp $destRel
                Write-ZipEntryToFile -Entry $entry -DestPath $destPath
                if (-not $CustomMode -and $pins.rtxRemixMc.files.PSObject.Properties[$rel]) {
                    $expectedFileSha = [string]$pins.rtxRemixMc.files.$rel
                    $actualFileSha = Get-Sha256Hex $destPath
                    if ($actualFileSha -ne $expectedFileSha.ToLowerInvariant()) {
                        throw "SHA-256 mismatch for RTX Remix Minecraft entry '$rel': expected $expectedFileSha, got $actualFileSha"
                    }
                }
                $seenEntries[$rel] = $true
            }
        } finally {
            $archive.Dispose()
        }

        foreach ($reqEntry in $entryMap.Keys) {
            if (-not $seenEntries.ContainsKey($reqEntry)) {
                throw "RTX Remix Minecraft archive missing required entry: $reqEntry"
            }
        }
        Assert-PeBinary -Path (Join-RelPath $stageTmp 'rtx-remix-mc/remixbridge.dll')

        if (-not $CustomMode) {
            $wantMcStamp = @{
                schema        = 2
                archiveSha256 = $pins.rtxRemixMc.archiveSha256.ToLowerInvariant()
            }
            [IO.File]::WriteAllText((Join-Path $stageTmp '.stamp.json'), (($wantMcStamp | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
        }
        if (Test-Path -LiteralPath $DestDir) { Remove-Item -LiteralPath $DestDir -Recurse -Force }
        Move-Item -LiteralPath $stageTmp -Destination $DestDir -Force
    } catch {
        if (Test-Path -LiteralPath $stageTmp) { Remove-Item -LiteralPath $stageTmp -Recurse -Force -ErrorAction SilentlyContinue }
        throw
    }
}

if ($isCustomRtxRemixMc) {
    Stage-RtxRemixMcPayload -DestDir $rtxRemixMcDir -CustomMode $true
    Write-Host "Staged custom RTX Remix Minecraft payload -> $rtxRemixMcDir" -ForegroundColor Green
} else {
    $wantMcStamp = @{
        schema        = 2
        archiveSha256 = $pins.rtxRemixMc.archiveSha256.ToLowerInvariant()
    }
    $mcCachedOk = $false
    if ((Test-Path -LiteralPath $rtxRemixMcStampPath -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'rtx-remix-mc/remixbridge.dll') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'Licenses/RTX-Remix-Minecraft-AMD-Patch_LICENSE.txt') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'Licenses/RTX-Remix-Minecraft-AMD-Patch_NOTICE.txt') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'Licenses/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt') -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'rtx-remix-mc/OptiScaler.ini') -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-RelPath $rtxRemixMcDir 'OptiScaler.ini') -PathType Leaf)) {
        try {
            $cur = Get-Content -LiteralPath $rtxRemixMcStampPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($cur.schema -eq $wantMcStamp.schema -and
                $cur.archiveSha256 -eq $wantMcStamp.archiveSha256) {
                $mcCachedOk = $true
            }
        } catch { $mcCachedOk = $false }
    }
    if (-not $mcCachedOk) {
        Stage-RtxRemixMcPayload -DestDir $rtxRemixMcDir -CustomMode $false
        Write-Host "Extracted RTX Remix Minecraft $($pins.rtxRemixMc.version) -> $rtxRemixMcDir" -ForegroundColor Green
    }
}

return @{
    DlssnrDir      = $dlssnrDir
    OpenRrDir      = $openRrDir
    OptiPatcherAsi = $optiPatcherOut
    RtxRemixMcDir  = $rtxRemixMcDir
}
