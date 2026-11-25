<#
.SYNOPSIS
  Fetch or reuse the official unmodified OptiScaler release package (Mochi-style).
  Outputs a verified directory containing OptiScaler.dll, OptiScaler.ini, OptiScaler/, and OptiScaler_LICENSE.txt.
#>
[CmdletBinding()]
param(
    [string]$OptiArchive = $env:NR_OPTI_ARCHIVE,
    [string]$OptiDir = $env:NR_OPTI_DIR,
    [string]$CacheRoot = '',
    [string]$UpstreamDir = ''
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $CacheRoot)   { $CacheRoot   = Join-Path $repoRoot 'cache' }
if (-not $UpstreamDir) { $UpstreamDir = Join-Path $repoRoot (Join-Path 'dist' 'upstream') }
$downloadsDir = Join-Path $CacheRoot 'downloads'
$optiCacheDir = Join-Path $CacheRoot 'optiscaler'
$releaseDir = Join-Path $optiCacheDir 'release'
$licenseOut = Join-Path $optiCacheDir 'OptiScaler_LICENSE.txt'
New-Item -ItemType Directory -Force -Path $downloadsDir, $optiCacheDir | Out-Null

$DefaultPinnedArchiveSha = 'a0db9c2f17845cef483fb7af6b6013d1bb7441bf115c0a161aebdcd447b5390b'
$PinnedArchiveName = 'OptiScaler_v10.0.0-pre1_20261125_a1e0e193.7z'
$PinnedArchiveUrl  = 'https://github.com/optiscaler/OptiScaler-nightly/releases/download/nightly-20261125/OptiScaler_v10.0.0-pre1_20261125_a1e0e193.7z'
$PinnedArchiveSha  = $DefaultPinnedArchiveSha
$PinnedLicenseUrl  = 'https://raw.githubusercontent.com/optiscaler/OptiScaler/a1e0e19335c1da2366d12ee2fdb9276daec7d080/LICENSE'
$PinnedLicenseSha  = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
$EffectiveManifest = [ordered]@{}
$pinsPath = Join-Path $PSScriptRoot 'upstream-pins.json'
if (Test-Path -LiteralPath $pinsPath -PathType Leaf) {
    $pinsObj = Get-Content -LiteralPath $pinsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($pinsObj.optiscaler) {
        if ($pinsObj.optiscaler.archiveName)   { $PinnedArchiveName = [string]$pinsObj.optiscaler.archiveName }
        if ($pinsObj.optiscaler.archiveUrl)    { $PinnedArchiveUrl  = [string]$pinsObj.optiscaler.archiveUrl }
        if ($pinsObj.optiscaler.archiveSha256) { $PinnedArchiveSha  = ([string]$pinsObj.optiscaler.archiveSha256).ToLowerInvariant() }
        if ($pinsObj.optiscaler.licenseUrl)    { $PinnedLicenseUrl  = [string]$pinsObj.optiscaler.licenseUrl }
        if ($pinsObj.optiscaler.licenseSha256) { $PinnedLicenseSha  = ([string]$pinsObj.optiscaler.licenseSha256).ToLowerInvariant() }
        if ($pinsObj.optiscaler.files) {
            foreach ($prop in $pinsObj.optiscaler.files.PSObject.Properties) {
                $EffectiveManifest[$prop.Name] = ([string]$prop.Value).ToLowerInvariant()
            }
        }
    }
}

$DefaultPinnedReleaseManifest = [ordered]@{
    '!! EXTRACT ALL FILES TO GAME FOLDER !!'             = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
    'Licenses/DirectX_LICENSE.txt'                      = '5239850894610071566f7ecee0b751fde43c862032d92b99d7d0f596b3433ebd'
    'Licenses/FidelityFX_v1_LICENSE.md'                 = 'a335c2bb2bd28184b9a30b6e35b0c7e4aa7826abd1c388fc2aa02405e9d04f03'
    'Licenses/FidelityFX_v2_LICENSE.md'                 = 'faeb8ef30681785235183e6393c920ad070955c2d2cae6c4b3a91b6a712767b7'
    'Licenses/XeSS_LICENSE.txt'                         = '784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df'
    'OptiScaler.dll'                                    = 'c4754484827305ebb66f63eebe5bd27151cb977b306a27293156364d82d8aaef'
    'OptiScaler.ini'                                    = 'bfe51f70670fd158ce3fe69f654be7e9d1590317cf233e28fc50f816786cb981'
    'OptiScaler/amd_fidelityfx_framegeneration_dx12.dll'= 'e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6'
    'OptiScaler/amd_fidelityfx_framegeneration_vk.dll'  = 'd002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4'
    'OptiScaler/amd_fidelityfx_loader_dx12.dll'         = 'e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb'
    'OptiScaler/amd_fidelityfx_loader_vk.dll'           = '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'
    'OptiScaler/amd_fidelityfx_upscaler_dx12.dll'       = 'aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace'
    'OptiScaler/amd_fidelityfx_upscaler_vk.dll'         = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'
    'OptiScaler/D3D12_OptiScaler/D3D12Core.dll'         = '07d286c306f8117321422affd9e6388c12d0fb4be1c7fc689d9e899324feeb24'
    'OptiScaler/dlssg_to_fsr3_amd_is_better.dll'        = '14e83096f4e7648b89a724208c6219508b44563ce8c44967c60a5fa46a9221e2'
    'OptiScaler/libxell.dll'                            = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'
    'OptiScaler/libxess_dx11.dll'                       = 'c7cfe86f0c9d94e4fb3696d3cd5035e2bbb6a8b1b0572f8b7395a4cdfd0c625e'
    'OptiScaler/libxess_fg.dll'                         = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
    'OptiScaler/libxess.dll'                            = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'
    'OptiScaler/Streamlined_fetcher_windows.bat'        = '4052e172f464235c9b33bcefd63965250a592f11129d0dee032942db3453cae3'
    'setup_linux.sh'                                    = '0571b0ae077d985616acf9f4101e8338fc225b1b22aa7b339100c7ea3f3022be'
    'setup_windows.bat'                                 = '8f34ddfdb02119e34d37d196aa7073a29b9e8f8609ed2f8d1377443d12bcc89b'
}
if ($EffectiveManifest.Count -eq 0 -and $PinnedArchiveSha -eq $DefaultPinnedArchiveSha) {
    $EffectiveManifest = $DefaultPinnedReleaseManifest
}

function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Test-OptiReleaseLayout([string]$Dir) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir -PathType Container)) { return $false }
    $dll = Join-Path $Dir 'OptiScaler.dll'
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $Dir 'OptiScaler.ini') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $Dir 'OptiScaler') -PathType Container)) {
        return $false
    }
    try {
        $fs = [IO.File]::OpenRead($dll)
        try {
            if ($fs.Length -lt 65536) { return $false }
            $mz = New-Object byte[] 2
            if ($fs.Read($mz, 0, 2) -ne 2 -or $mz[0] -ne 0x4D -or $mz[1] -ne 0x5A) { return $false }
        } finally { $fs.Dispose() }
    } catch { return $false }
    return $true
}

function Test-PinnedOptiRelease([string]$Dir) {
    if (-not (Test-OptiReleaseLayout $Dir)) { return $false }
    if ($EffectiveManifest.Count -gt 0) {
        foreach ($entry in $EffectiveManifest.GetEnumerator()) {
            $fp = Join-Path $Dir ($entry.Key.Replace('/', [IO.Path]::DirectorySeparatorChar))
            if (-not (Test-Path -LiteralPath $fp -PathType Leaf)) { return $false }
            if ((Get-Sha256Hex $fp) -ne $entry.Value) { return $false }
        }
        $prefixLen = [IO.Path]::GetFullPath($Dir).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Length + 1
        $actualFiles = @(Get-ChildItem -LiteralPath $Dir -Recurse -File | ForEach-Object {
            $_.FullName.Substring($prefixLen).Replace('\', '/')
        } | Where-Object { $_ -ne '.stamp.json' })
        if ($actualFiles.Count -ne $EffectiveManifest.Count) { return $false }
        foreach ($rel in $actualFiles) {
            if (-not $EffectiveManifest.Contains($rel)) { return $false }
        }
        return $true
    }
    $stampPath = Join-Path $Dir '.stamp.json'
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) { return $false }
    try {
        $stamp = Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8 | ConvertFrom-Json
        return ($stamp.schema -eq 1 -and ([string]$stamp.archiveSha256).ToLowerInvariant() -eq $PinnedArchiveSha)
    } catch {
        return $false
    }
}

function Write-PinnedStamp([string]$Dir) {
    $stampJson = (@{ schema = 1; archiveSha256 = $PinnedArchiveSha } | ConvertTo-Json -Depth 2) + "`n"
    [IO.File]::WriteAllText((Join-Path $Dir '.stamp.json'), $stampJson, [Text.UTF8Encoding]::new($false))
}

function Download-VerifiedFile([string]$Url, [string]$Dest, [string]$ExpectedSha) {
    if (Test-Path -LiteralPath $Dest -PathType Leaf) {
        if (-not $ExpectedSha -or (Get-Sha256Hex $Dest) -eq $ExpectedSha) { return }
        Remove-Item -LiteralPath $Dest -Force
    }
    Write-Host "Fetching $Url -> $Dest ..." -ForegroundColor Cyan
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
    if ($ExpectedSha -and (Get-Sha256Hex $part) -ne $ExpectedSha) {
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for downloaded file: $Url"
    }
    Move-Item -LiteralPath $part -Destination $Dest -Force
}

function Expand-ArchiveWith7zOrNet([string]$ArchivePath, [string]$DestDir) {
    if (Test-Path -LiteralPath $DestDir) { Remove-Item -LiteralPath $DestDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    if ([IO.Path]::GetExtension($ArchivePath) -ieq '.zip') {
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $DestDir -Force
        return
    }
    $7zCandidates = New-Object System.Collections.Generic.List[string]
    foreach ($cmdName in @('7z.exe', '7zz', '7z', '7za')) {
        $cmd = Get-Command $cmdName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd -and $cmd.Source) { [void]$7zCandidates.Add($cmd.Source) }
    }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf) {
            [void]$7zCandidates.Add((Join-Path (Join-Path $pf '7-Zip') '7z.exe'))
        }
    }
    $7zExe = $7zCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    if (-not $7zExe) {
        throw "7z/7zz is required to unpack $ArchivePath. Install 7-Zip / p7zip or pass -OptiDir pointing to an unpacked OptiScaler folder."
    }
    & $7zExe x -y ("-o" + $DestDir) -- $ArchivePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z failed unpacking $ArchivePath" }
}

# 1. Ensure LICENSE (cached inside cache/downloads/ so CI cache preserves it too)
$shortLicSha = if ($PinnedLicenseSha.Length -ge 12) { $PinnedLicenseSha.Substring(0, 12) } else { 'pinned' }
$cachedLicense = Join-Path $downloadsDir ("OptiScaler_LICENSE_" + $shortLicSha + ".txt")
$upstreamLic = if ($UpstreamDir) { Join-Path $UpstreamDir 'OptiScaler_LICENSE.txt' } else { '' }
$siblingLic = Join-Path (Split-Path -Parent $repoRoot) (Join-Path 'DLSSNR-AMD-mochizuki' (Join-Path 'artifacts' (Join-Path 'ref' (Join-Path 'optiscaler-nightly-20261107' 'LICENSE'))))

if (-not (Test-Path -LiteralPath $cachedLicense -PathType Leaf) -or (Get-Sha256Hex $cachedLicense) -ne $PinnedLicenseSha) {
    if ((Test-Path -LiteralPath $licenseOut -PathType Leaf) -and (Get-Sha256Hex $licenseOut) -eq $PinnedLicenseSha) {
        Copy-Item -LiteralPath $licenseOut -Destination $cachedLicense -Force
    } elseif ($upstreamLic -and (Test-Path -LiteralPath $upstreamLic -PathType Leaf) -and (Get-Sha256Hex $upstreamLic) -eq $PinnedLicenseSha) {
        Copy-Item -LiteralPath $upstreamLic -Destination $cachedLicense -Force
    } elseif ((Test-Path -LiteralPath $siblingLic -PathType Leaf) -and (Get-Sha256Hex $siblingLic) -eq $PinnedLicenseSha) {
        Copy-Item -LiteralPath $siblingLic -Destination $cachedLicense -Force
    } else {
        Download-VerifiedFile $PinnedLicenseUrl $cachedLicense $PinnedLicenseSha
    }
}
if (-not (Test-Path -LiteralPath $licenseOut -PathType Leaf) -or (Get-Sha256Hex $licenseOut) -ne $PinnedLicenseSha) {
    Copy-Item -LiteralPath $cachedLicense -Destination $licenseOut -Force
}

# 2. Ensure unpacked official OptiScaler release
$customOptiInput = [bool]($OptiDir -or $OptiArchive)
if ($OptiDir) {
    if (-not (Test-OptiReleaseLayout $OptiDir)) {
        throw "Provided -OptiDir is not a valid OptiScaler v10 layout: $OptiDir"
    }
    $sepChars = [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $optiSrcFull = [IO.Path]::GetFullPath($OptiDir).TrimEnd($sepChars)
    $optiDstFull = [IO.Path]::GetFullPath($releaseDir).TrimEnd($sepChars)
    if ($optiSrcFull -ine $optiDstFull) {
        if (Test-Path -LiteralPath $releaseDir) { Remove-Item -LiteralPath $releaseDir -Recurse -Force }
        Copy-Item -LiteralPath $OptiDir -Destination $releaseDir -Recurse -Force
    }
} elseif ($OptiArchive) {
    if (-not (Test-Path -LiteralPath $OptiArchive -PathType Leaf)) {
        throw "Provided -OptiArchive not found: $OptiArchive"
    }
    Expand-ArchiveWith7zOrNet $OptiArchive $releaseDir
} elseif (-not (Test-PinnedOptiRelease $releaseDir)) {
    $pinnedArchivePath = Join-Path $downloadsDir $PinnedArchiveName
    $upstreamPinnedArchive = if ($UpstreamDir) { Join-Path $UpstreamDir $PinnedArchiveName } else { '' }
    $repoPinnedArchive = Join-Path (Join-Path (Join-Path $repoRoot 'cache') 'downloads') $PinnedArchiveName
    $parentDir = Split-Path -Parent $repoRoot
    $parentPinnedArchive = Join-Path $parentDir $PinnedArchiveName
    $parentPinnedDir = Join-Path $parentDir ([IO.Path]::GetFileNameWithoutExtension($PinnedArchiveName))
    $siblingMochiRef = Join-Path (Join-Path (Join-Path $parentDir 'DLSSNR-AMD-mochizuki') 'artifacts') 'ref'
    $siblingMochiDownloads = Join-Path $siblingMochiRef 'downloads'
    $siblingMochiArchive = Join-Path $siblingMochiDownloads $PinnedArchiveName
    $siblingMochiLocal7z = Join-Path $siblingMochiDownloads 'OptiScaler-nightly-local.7z'
    $siblingMochiRelease = Join-Path (Join-Path (Join-Path $siblingMochiRef 'opti-release') 'optiscaler') 'release'

    if ($upstreamPinnedArchive -and (Test-Path -LiteralPath $upstreamPinnedArchive -PathType Leaf)) {
        $actualUpSha = Get-Sha256Hex $upstreamPinnedArchive
        if ($actualUpSha -ne $PinnedArchiveSha) {
            throw "SHA-256 mismatch for local upstream OptiScaler archive '$upstreamPinnedArchive': expected $PinnedArchiveSha, got $actualUpSha"
        }
        Write-Host "Reusing verified OptiScaler archive from $upstreamPinnedArchive" -ForegroundColor Green
        Expand-ArchiveWith7zOrNet $upstreamPinnedArchive $releaseDir
    } elseif ((Test-Path -LiteralPath $pinnedArchivePath -PathType Leaf) -and (Get-Sha256Hex $pinnedArchivePath) -eq $PinnedArchiveSha) {
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ($EffectiveManifest.Count -gt 0 -and (Test-PinnedOptiRelease $parentPinnedDir)) {
        Write-Host "Reusing verified official OptiScaler directory from $parentPinnedDir" -ForegroundColor Green
        if (Test-Path -LiteralPath $releaseDir) { Remove-Item -LiteralPath $releaseDir -Recurse -Force }
        Copy-Item -LiteralPath $parentPinnedDir -Destination $releaseDir -Recurse -Force
    } elseif ((Test-Path -LiteralPath $parentPinnedArchive -PathType Leaf) -and (Get-Sha256Hex $parentPinnedArchive) -eq $PinnedArchiveSha) {
        Copy-Item -LiteralPath $parentPinnedArchive -Destination $pinnedArchivePath -Force
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ((Test-Path -LiteralPath $repoPinnedArchive -PathType Leaf) -and (Get-Sha256Hex $repoPinnedArchive) -eq $PinnedArchiveSha) {
        Copy-Item -LiteralPath $repoPinnedArchive -Destination $pinnedArchivePath -Force
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ((Test-Path -LiteralPath $siblingMochiArchive -PathType Leaf) -and (Get-Sha256Hex $siblingMochiArchive) -eq $PinnedArchiveSha) {
        Copy-Item -LiteralPath $siblingMochiArchive -Destination $pinnedArchivePath -Force
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ($EffectiveManifest.Count -gt 0 -and (Test-PinnedOptiRelease $siblingMochiRelease)) {
        Write-Host "Reusing verified cached official OptiScaler release from $siblingMochiRelease" -ForegroundColor Green
        if (Test-Path -LiteralPath $releaseDir) { Remove-Item -LiteralPath $releaseDir -Recurse -Force }
        Copy-Item -LiteralPath $siblingMochiRelease -Destination $releaseDir -Recurse -Force
    } elseif ((Test-Path -LiteralPath $siblingMochiLocal7z -PathType Leaf) -and (Get-Sha256Hex $siblingMochiLocal7z) -eq $PinnedArchiveSha) {
        Write-Host "Unpacking verified cached official OptiScaler archive from $siblingMochiLocal7z" -ForegroundColor Green
        Expand-ArchiveWith7zOrNet $siblingMochiLocal7z $releaseDir
    } else {
        Download-VerifiedFile $PinnedArchiveUrl $pinnedArchivePath $PinnedArchiveSha
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    }
    Write-PinnedStamp $releaseDir
}

if ($customOptiInput) {
    if (-not (Test-OptiReleaseLayout $releaseDir)) {
        throw "Custom OptiScaler release directory is incomplete: $releaseDir"
    }
} else {
    if (-not (Test-PinnedOptiRelease $releaseDir)) {
        throw "Official OptiScaler release directory failed pinned SHA-256 manifest verification: $releaseDir"
    }
}

return @{
    ReleaseDir  = $releaseDir
    LicensePath = $licenseOut
}
