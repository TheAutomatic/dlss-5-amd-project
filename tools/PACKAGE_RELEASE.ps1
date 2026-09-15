<#
.SYNOPSIS
  Stage and zip a complete user package (no NVIDIA / author proprietary files).
  Default product: OptiScaler-AMD-PreSR-1.8.2-0.3.0
    1.8.2  = this fork's product version
    0.3.0  = required author AMD NR runtime version

.EXAMPLE
  .\PACKAGE_RELEASE.ps1
  .\PACKAGE_RELEASE.ps1 -Version 1.8.2-0.3.0 -DepsRoot 'C:\path\with\OptiScaler'
#>
[CmdletBinding()]
param(
    [string]$Version = '1.8.2-0.3.0',
    [string]$OutDir = 'dist',
    [string]$Name = '',
    [string]$OptiDll = '',
    [string]$DepsRoot = '',
    [switch]$AllowMissingDeps
)
$ErrorActionPreference = 'Stop'

# 不要用 Get-FileHash：它属于 Microsoft.PowerShell.Utility，靠模块自动加载。
# 当环境里的 PSModulePath 指向 PowerShell 7 的模块目录时（CI 里在 shell: pwsh
# 步骤里调 powershell -File 正是这种情况），5.1 子进程加载不到它，会直接报
# CommandNotFoundException，整个打包步骤失败。用 .NET 自己算，不依赖任何模块。
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$source = Join-Path $root 'OptiScaler-DLSSNR-PreSR-Multipass-main'
if (-not $Name) { $Name = "OptiScaler-AMD-PreSR-$Version" }
$stage = Join-Path $root (Join-Path $OutDir $Name)
$zip = Join-Path $root (Join-Path $OutDir ($Name + '.zip'))

if (-not $OptiDll) {
    foreach ($c in @(
        (Join-Path $root 'exports/release-local/OptiScaler.dll'),
        (Join-Path $root 'exports/build/OptiScaler.dll'),
        (Join-Path $source 'x64/Release/OptiScaler.dll'),
        (Join-Path $root 'exports/release-r17/OptiScaler.dll')
    )) {
        if (Test-Path -LiteralPath $c) { $OptiDll = $c; break }
    }
}
if (!(Test-Path -LiteralPath $OptiDll)) {
    throw 'OptiScaler.dll not found. Build Release first (r18: ordinary Release, multi-slot default).'
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'OptiScaler'), (Join-Path $stage 'Licenses') | Out-Null
Copy-Item -LiteralPath $OptiDll -Destination (Join-Path $stage 'OptiScaler.dll') -Force

$deps = Join-Path $stage 'OptiScaler'
$missing = [System.Collections.Generic.List[string]]::new()

function Find-Dep([string[]]$candidates) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return $c }
    }
    return $null
}

$depSearch = @()
if ($DepsRoot) {
    $depSearch += (Join-Path $DepsRoot 'OptiScaler')
    $depSearch += $DepsRoot
}
$depSearch += (Join-Path $source 'external/FidelityFX-SDK-v2/Kits/FidelityFX/signedbin')
$depSearch += (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler')
$depSearch += (Join-Path $root 'OptiScaler-AMD-PreSR-Multipass-v2.25/OptiScaler')

foreach ($name in @(
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll'
)) {
    $cands = @()
    foreach ($d in $depSearch) { $cands += (Join-Path $d $name) }
    $hit = Find-Dep $cands
    if ($hit) { Copy-Item -LiteralPath $hit -Destination $deps -Force }
    else { $missing.Add($name) }
}

$vkCands = @(Join-Path $source 'external/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_vk.dll')
if ($depSearch.Count) { $vkCands += (Join-Path $depSearch[0] 'amd_fidelityfx_vk.dll') }
$vk = Find-Dep $vkCands
if ($vk) { Copy-Item -LiteralPath $vk -Destination $deps -Force }

$xessDirs = @(
    (Join-Path $source 'external/xess/bin'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-Multipass-v2.25/OptiScaler')
)
if ($DepsRoot) {
    $xessDirs = @((Join-Path $DepsRoot 'OptiScaler'), $DepsRoot) + $xessDirs
}
$xessCopied = 0
foreach ($d in $xessDirs) {
    if (!(Test-Path $d)) { continue }
    Get-ChildItem -LiteralPath $d -Filter 'libxess*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName -Destination $deps -Force; $xessCopied++ }
    Get-ChildItem -LiteralPath $d -Filter 'libxell*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName -Destination $deps -Force; $xessCopied++ }
    if ($xessCopied) { break }
}
if ($xessCopied -eq 0) { $missing.Add('libxess*.dll (optional but recommended)') }

New-Item -ItemType Directory -Path (Join-Path $deps 'D3D12_OptiScaler') -Force | Out-Null
# Only Agility D3D12Core (and optional Agility companions). Never sweep a
# user-supplied DepsRoot for every *.dll — that could pick up version.dll.
$agilityNames = @('D3D12Core.dll', 'd3d12SDKLayers.dll')
$agilityCands = @(
    (Join-Path $source 'external/directx_agility_sdk/lib'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler/D3D12_OptiScaler')
)
if ($DepsRoot) {
    $agilityCands = @((Join-Path $DepsRoot 'OptiScaler/D3D12_OptiScaler')) + $agilityCands
}
foreach ($d in $agilityCands) {
    if (!(Test-Path $d)) { continue }
    foreach ($n in $agilityNames) {
        $p = Join-Path $d $n
        if (Test-Path -LiteralPath $p) {
            Copy-Item -LiteralPath $p -Destination (Join-Path $deps 'D3D12_OptiScaler') -Force
        }
    }
    break
}

if ($missing.Count -gt 0) {
    $msg = "Missing upscaler dependency binaries:`n  " + ($missing -join "`n  ") + "`n" +
           "These live in the git submodules (external/xess, external/FidelityFX-SDK*,`n" +
           "and they are *.dll so they are not committed directly).`n" +
           "Fix: git submodule update --init --recursive`n" +
           "Or pass -DepsRoot pointing at a folder that has an OptiScaler\ subfolder."
    if ($AllowMissingDeps) {
        Write-Warning $msg
    } else {
        throw $msg
    }
}

# Licenses
$optiLic = Join-Path $source 'LICENSE'
if (Test-Path $optiLic) {
    Copy-Item $optiLic (Join-Path $stage 'Licenses/OptiScaler_LICENSE.txt') -Force
}
if (Test-Path (Join-Path $source 'Licenses')) {
    Get-ChildItem (Join-Path $source 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force
}
foreach ($pair in @(
    @('external/xess/LICENSE.txt', 'XeSS_LICENSE.txt'),
    @('external/FidelityFX-SDK/docs/license.md', 'FidelityFX_v1_LICENSE.md'),
    @('external/FidelityFX-SDK-v2/docs/license.md', 'FidelityFX_v2_LICENSE.md'),
    @('external/directx_agility_sdk/LICENSE.txt', 'DirectX_LICENSE.txt')
)) {
    $p = Join-Path $source $pair[0]
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage ('Licenses/' + $pair[1])) -Force }
}

# INI
$iniSrc = Join-Path $source 'OptiScaler.ini'
if (!(Test-Path $iniSrc)) { throw "Missing $iniSrc" }
$ini = Get-Content -LiteralPath $iniSrc -Raw
$ini = $ini -replace '(?m)^Dx12Upscaler=.*$', 'Dx12Upscaler=ffx'
$ini = $ini -replace '(?m)^LogToFile=.*$', 'LogToFile=true'
$ini = $ini -replace '(?m)^LogLevel=.*$', 'LogLevel=2'
$ini = [regex]::Replace($ini, '(?ms)(\[FrameGen\].*?^Enabled=)[^\r\n]*', '$1false')
$ini = [regex]::Replace($ini, '(?ms)^\[DlssNr\].*?(?=^\[|\z)', @"
[DlssNr]
; Product $Version — every-frame multi-slot is the source default.
; Requires DLSS-NR-on-AMD 0.3.0 (https://github.com/danielblnc/DLSS-NR-on-AMD)
; as dlssnr_amd_pass1-3.dll (Setup copies version.dll from the package folder).
Enabled=false
RunBeforeSR=true
AmdModelScale=1
AmdEncoding=0
AmdEveryFrame=true
AmdNeuralLighting=true
AmdNeuralLightingStrength=0.5
Passes=1
LocalTone=0
LocalStructure=1
SkinStructure=1
ApplyAfterRR=false

"@)
# 这两段只在源 ini 里还没有的时候才追加。
# 无条件追加的写法在源 ini 哪天自带 [AmdLook]/[AmdRtgi] 时会写出重复段 ——
# 而追加的那份是 Enabled=false，可能把用户调好的值顶掉。
$amdLookBlock = @"

[AmdLook]
Enabled=false
Appearance=2
Mix=1
MaterialDetail=1.15
ShapeDefinition=1.2
LocalLighting=1.15
SkinDetail=1.1
SkinSoftness=0.486
DetectSkin=true
SpecularControl=0.58
HighlightRollOff=0.9
ColourSeparation=0
ShadowDepth=0.2
AntiHalo=0.901
FlatAreaProtection=0
Inspect=0
Tone=0
ExposureEV=1
Contrast=1
Saturation=1
HighlightCompression=0
"@

$amdRtgiBlock = @"

[AmdRtgi]
Enabled=false
Quality=2
Denoiser=1
Inspect=0
Mix=1
Lighting=5
Occlusion=1
Ambient=1
Thickness=0.1
Smoothness=0.5
Fade=0.3
Fov=60
FarPlane=600
Contact=0
Saturation=1
Radius=1
"@

if ($ini -notmatch '(?m)^\[AmdLook\]') { $ini += $amdLookBlock }
if ($ini -notmatch '(?m)^\[AmdRtgi\]') { $ini += $amdRtgiBlock }
[IO.File]::WriteAllText((Join-Path $stage 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))

$rtgiSrc = Join-Path $root 'package-amd-presr/experimental_lighting'
if (Test-Path $rtgiSrc) {
    $rtgiDst = Join-Path $stage 'experimental_lighting'
    New-Item -ItemType Directory -Path $rtgiDst -Force | Out-Null
    Get-ChildItem -LiteralPath $rtgiSrc -File | Copy-Item -Destination $rtgiDst -Force
}

# Installer + docs (CN + EN). No duplicate 使用说明.txt.
$readmeZh = Join-Path $root 'tools/README-release.md'
$readmeEn = Join-Path $root 'README.en.md'
if (!(Test-Path $readmeZh)) { throw "Missing $readmeZh" }
if (!(Test-Path $readmeEn)) { throw "Missing $readmeEn" }
$installerSrc = Join-Path $root 'tools/install-amd-presr.ps1'
if (!(Test-Path $installerSrc)) { throw "Missing $installerSrc" }
Copy-Item $installerSrc (Join-Path $stage 'Setup.ps1') -Force
@'
@echo off
setlocal
title OptiScaler AMD pre-SR Setup
rem No args: Setup.ps1 opens a folder picker and proxy menu.
rem Optional: Setup.bat "D:\GameFolder" [dxgi.dll]
if "%~2"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1" -Proxy "%~2"
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" pause
exit /b %EC%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Setup.bat') -Encoding ASCII
Copy-Item $readmeZh (Join-Path $stage 'README.md') -Force
Copy-Item $readmeEn (Join-Path $stage 'README.en.md') -Force

# 绊线：这些文件名一旦出现在 stage 里就拒绝打包（含子目录，例如 Agility 误扫入 version.dll）。
# 原作者 pass（dlssnr_amd_pass*.dll）必须不在包内 —— README 明写「包里没有原作者 pass」。
$forbidden = '^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|version\.dll|dlssnr_on_amd_setup\.exe)$'
$badAll = Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match $forbidden }
if ($badAll) {
    throw "Refusing to package proprietary/user-supplied file: $(($badAll | ForEach-Object { $_.FullName.Substring($stage.Length+1) }) -join ', ')"
}

$hashes = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        # 正斜杠：清单是 coreutils 格式（`sha256sum -c` 用），反斜杠分隔符在 git-bash /
        # Linux 上认不出来。Windows 侧 PowerShell 用正斜杠访问文件同样正常。
        '{0} *{1}' -f (Get-Sha256 $_.FullName), ($_.FullName.Substring($stage.Length + 1) -replace '\\', '/')
    }
# 不要用 Set-Content -Encoding UTF8：Windows PowerShell 5.1 的 -Encoding UTF8 会写 BOM，
# BOM 直接粘在第一个哈希前面，用户跑 `sha256sum -c SHA256SUMS.txt` 会看到
# "1 line is improperly formatted"，第一项永远验不过。走 .NET 的无 BOM UTF-8。
# 行尾用 LF 而不是 CRLF：CRLF 会在文件名后留下 \r，`sha256sum -c` 把它当成文件名的一部分，
# 23 项全部 "No such file or directory"。LF + 正斜杠才能让标准工具真的验得了。
# 记事本（Win10 1809+）与 PowerShell 读 LF 都正常。
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'),
    (($hashes -join "`n") + "`n"),
    [Text.UTF8Encoding]::new($false))

New-Item -ItemType Directory -Force -Path (Join-Path $root $OutDir) | Out-Null
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
Write-Host "Product: $Version"
Write-Host "Staged:  $stage"
Write-Host "Zip:     $zip"
Write-Host "Opti:    $OptiDll"
