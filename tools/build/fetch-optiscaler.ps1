<#
.SYNOPSIS
  Fetch or reuse the official unmodified OptiScaler release package (Mochi-style).
  Outputs a verified directory containing OptiScaler.dll, OptiScaler.ini, OptiScaler/, and OptiScaler_LICENSE.txt.
#>
[CmdletBinding()]
param(
    [string]$OptiArchive = $env:NR_OPTI_ARCHIVE,
    [string]$OptiDir = $env:NR_OPTI_DIR,
    [string]$CacheRoot = ''
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $CacheRoot) { $CacheRoot = Join-Path $repoRoot 'cache' }
$downloadsDir = Join-Path $CacheRoot 'downloads'
$releaseDir = Join-Path $CacheRoot 'optiscaler\release'
$licenseOut = Join-Path $CacheRoot 'optiscaler\OptiScaler_LICENSE.txt'
New-Item -ItemType Directory -Force -Path $downloadsDir, (Split-Path -Parent $releaseDir) | Out-Null

$PinnedArchiveName = 'OptiScaler_v10.0.0-pre1_20261107.7z'
$PinnedArchiveUrl  = 'https://github.com/optiscaler/OptiScaler-nightly/releases/download/nightly-20261107/OptiScaler_v10.0.0-pre1_20261107.7z'
$PinnedArchiveSha  = '7d185f8eb707fdb4ae22304ce4c1c8db6600b079df6e9c04e17d37d7a6727b85'
$PinnedLicenseUrl  = 'https://raw.githubusercontent.com/optiscaler/OptiScaler/e7e3872d410f704479d94d318d68d163b6b58947/LICENSE'
$PinnedLicenseSha  = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'

$PinnedReleaseManifest = [ordered]@{
    'Licenses/DirectX_LICENSE.txt'                      = '5239850894610071566f7ecee0b751fde43c862032d92b99d7d0f596b3433ebd'
    'Licenses/FidelityFX_v2_LICENSE.md'                 = 'faeb8ef30681785235183e6393c920ad070955c2d2cae6c4b3a91b6a712767b7'
    'Licenses/XeSS_LICENSE.txt'                         = '784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df'
    'OptiScaler.dll'                                    = 'e4f8773e05ba754e1e8ce8e67b459bf414ed0726fb68784a0fa8ce435ea1f7bd'
    'OptiScaler.ini'                                    = '2020ab3e14fcc77ec8e32b396c00eafd2a55b0d8f7336e6e1eb884292f322a62'
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
    foreach ($entry in $PinnedReleaseManifest.GetEnumerator()) {
        $fp = Join-Path $Dir ($entry.Key.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $fp -PathType Leaf)) { return $false }
        if ((Get-Sha256Hex $fp) -ne $entry.Value) { return $false }
    }
    $actualFiles = @(Get-ChildItem -LiteralPath $Dir -Recurse -File | ForEach-Object {
        $_.FullName.Substring([IO.Path]::GetFullPath($Dir).TrimEnd('\', '/').Length + 1).Replace('\', '/')
    })
    if ($actualFiles.Count -ne $PinnedReleaseManifest.Count) { return $false }
    foreach ($rel in $actualFiles) {
        if (-not $PinnedReleaseManifest.Contains($rel)) { return $false }
    }
    return $true
}

function Download-VerifiedFile([string]$Url, [string]$Dest, [string]$ExpectedSha) {
    if (Test-Path -LiteralPath $Dest -PathType Leaf) {
        if (-not $ExpectedSha -or (Get-Sha256Hex $Dest) -eq $ExpectedSha) { return }
        Remove-Item -LiteralPath $Dest -Force
    }
    Write-Host "Fetching $Url -> $Dest ..." -ForegroundColor Cyan
    $part = "$Dest.part"
    $curl = Get-Command 'curl.exe' -ErrorAction SilentlyContinue
    if ($curl) {
        & $curl.Source -fsSL --retry 3 -o $part $Url
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading $Url" }
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
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
    $7zCandidates = @( @(
        (Get-Command '7z.exe' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
        "$env:ProgramFiles\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } )
    if ($7zCandidates.Count -eq 0) {
        throw "7z.exe is required to unpack $ArchivePath. Install 7-Zip or pass -OptiDir pointing to an unpacked OptiScaler folder."
    }
    & $7zCandidates[0] x -y ("-o" + $DestDir) -- $ArchivePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z failed unpacking $ArchivePath" }
}

# 1. Ensure LICENSE
if (-not (Test-Path -LiteralPath $licenseOut -PathType Leaf) -or (Get-Sha256Hex $licenseOut) -ne $PinnedLicenseSha) {
    $siblingLic = Join-Path (Split-Path -Parent $repoRoot) 'DLSSNR-AMD-mochizuki\artifacts\ref\optiscaler-nightly-20261107\LICENSE'
    if ((Test-Path -LiteralPath $siblingLic -PathType Leaf) -and (Get-Sha256Hex $siblingLic) -eq $PinnedLicenseSha) {
        Copy-Item -LiteralPath $siblingLic -Destination $licenseOut -Force
    } else {
        Download-VerifiedFile $PinnedLicenseUrl $licenseOut $PinnedLicenseSha
    }
}

# 2. Ensure unpacked official OptiScaler release
$customOptiInput = [bool]($OptiDir -or $OptiArchive)
if ($OptiDir) {
    if (-not (Test-OptiReleaseLayout $OptiDir)) {
        throw "Provided -OptiDir is not a valid OptiScaler v10 layout: $OptiDir"
    }
    if (Test-Path -LiteralPath $releaseDir) { Remove-Item -LiteralPath $releaseDir -Recurse -Force }
    Copy-Item -LiteralPath $OptiDir -Destination $releaseDir -Recurse -Force
} elseif ($OptiArchive) {
    if (-not (Test-Path -LiteralPath $OptiArchive -PathType Leaf)) {
        throw "Provided -OptiArchive not found: $OptiArchive"
    }
    Expand-ArchiveWith7zOrNet $OptiArchive $releaseDir
} elseif (-not (Test-PinnedOptiRelease $releaseDir)) {
    $pinnedArchivePath = Join-Path $downloadsDir $PinnedArchiveName
    $repoPinnedArchive = Join-Path $repoRoot "cache\downloads\$PinnedArchiveName"
    $siblingMochiArchive = Join-Path (Split-Path -Parent $repoRoot) "DLSSNR-AMD-mochizuki\artifacts\ref\downloads\$PinnedArchiveName"
    $siblingMochiLocal7z = Join-Path (Split-Path -Parent $repoRoot) 'DLSSNR-AMD-mochizuki\artifacts\ref\downloads\OptiScaler-nightly-local.7z'
    $siblingMochiRelease = Join-Path (Split-Path -Parent $repoRoot) 'DLSSNR-AMD-mochizuki\artifacts\ref\opti-release\optiscaler\release'

    if ((Test-Path -LiteralPath $pinnedArchivePath -PathType Leaf) -and (Get-Sha256Hex $pinnedArchivePath) -eq $PinnedArchiveSha) {
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ((Test-Path -LiteralPath $repoPinnedArchive -PathType Leaf) -and (Get-Sha256Hex $repoPinnedArchive) -eq $PinnedArchiveSha) {
        Copy-Item -LiteralPath $repoPinnedArchive -Destination $pinnedArchivePath -Force
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif ((Test-Path -LiteralPath $siblingMochiArchive -PathType Leaf) -and (Get-Sha256Hex $siblingMochiArchive) -eq $PinnedArchiveSha) {
        Copy-Item -LiteralPath $siblingMochiArchive -Destination $pinnedArchivePath -Force
        Expand-ArchiveWith7zOrNet $pinnedArchivePath $releaseDir
    } elseif (Test-PinnedOptiRelease $siblingMochiRelease) {
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
