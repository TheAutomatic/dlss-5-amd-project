<#
.SYNOPSIS
  Stage and zip a complete r17 user package (no NVIDIA / author proprietary files).
.EXAMPLE
  .\PACKAGE_R17.ps1 -Version r17 -OutDir dist
#>
[CmdletBinding()]
param(
    [string]$Version = 'r17',
    [string]$OutDir = 'dist',
    [string]$Name = "OptiScaler-AMD-PreSR-$Version",
    [string]$OptiDll = '',
    # Optional folder that already contains OptiScaler\ (FFX/XeSS/Agility) to copy from.
    # Useful on CI when those signed bins are not in git (see .gitignore *.dll).
    [string]$DepsRoot = '',
    # If set, missing FFX/XeSS bins become warnings instead of a hard fail (core-only zip).
    [switch]$AllowMissingDeps
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$source = Join-Path $root 'OptiScaler-DLSSNR-PreSR-Multipass-main'
$stage = Join-Path $root (Join-Path $OutDir $Name)
$zip = Join-Path $root (Join-Path $OutDir ($Name + '.zip'))

if (-not $OptiDll) {
    foreach ($c in @(
        (Join-Path $root "exports/release-$Version/OptiScaler.dll"),
        (Join-Path $source 'x64/Release/OptiScaler.dll')
    )) {
        if (Test-Path -LiteralPath $c) { $OptiDll = $c; break }
    }
}
if (!(Test-Path -LiteralPath $OptiDll)) { throw "OptiScaler.dll not found. Build r17 first." }

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

# Preferred layout: -DepsRoot\OptiScaler\*.dll or -DepsRoot\*.dll
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

$vk = Find-Dep @(
    (Join-Path $source 'external/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_vk.dll'),
    (Join-Path $depSearch[0] 'amd_fidelityfx_vk.dll')
)
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
$agilityCands = @(
    (Join-Path $source 'external/directx_agility_sdk/lib'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler/D3D12_OptiScaler')
)
if ($DepsRoot) {
    $agilityCands = @((Join-Path $DepsRoot 'OptiScaler/D3D12_OptiScaler'), $DepsRoot) + $agilityCands
}
foreach ($d in $agilityCands) {
    if (!(Test-Path $d)) { continue }
    Get-ChildItem -LiteralPath $d -Filter '*.dll' -ErrorAction SilentlyContinue |
        Copy-Item -Destination (Join-Path $deps 'D3D12_OptiScaler') -Force
    break
}

if ($missing.Count -gt 0) {
    $msg = "Missing upscaler dependency binaries:`n  " + ($missing -join "`n  ") + "`n" +
           "These are NOT in git (OptiScaler .gitignore *.dll / [Bb]in/).`n" +
           "Provide -DepsRoot with an OptiScaler\ folder (or R1/v2.25 package), or use -AllowMissingDeps for a core-only zip."
    if ($AllowMissingDeps) {
        Write-Warning $msg
        Add-Content -LiteralPath (Join-Path $stage 'README.md') -Value "`n`nNOTE: This zip was built without FFX/XeSS binaries. Copy them into OptiScaler\ next to OptiScaler.dll before installing."
    } else {
        throw $msg
    }
}

# Licenses (upstream OptiScaler + deps)
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

# INI: FFX upscaler, every-frame default (r17)
$iniSrc = Join-Path $source 'OptiScaler.ini'
if (!(Test-Path $iniSrc)) { throw "Missing $iniSrc" }
$ini = Get-Content -LiteralPath $iniSrc -Raw
$ini = $ini -replace '(?m)^Dx12Upscaler=.*$', 'Dx12Upscaler=ffx'
$ini = $ini -replace '(?m)^LogToFile=.*$', 'LogToFile=true'
$ini = $ini -replace '(?m)^LogLevel=.*$', 'LogLevel=2'
$ini = [regex]::Replace($ini, '(?ms)(\[FrameGen\].*?^Enabled=)[^\r\n]*', '$1false')
$ini = [regex]::Replace($ini, '(?ms)^\[DlssNr\].*?(?=^\[|\z)', @'
[DlssNr]
; AMD NR via author 0.3.0 pass + FFX SR. Every-frame multi-slot is default in r17.
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

'@)
$ini += @"

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
[IO.File]::WriteAllText((Join-Path $stage 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))

# Optional RTGI cso from local package if present
$rtgiSrc = Join-Path $root 'package-amd-presr/experimental_lighting'
if (Test-Path $rtgiSrc) {
    $rtgiDst = Join-Path $stage 'experimental_lighting'
    New-Item -ItemType Directory -Path $rtgiDst -Force | Out-Null
    Get-ChildItem -LiteralPath $rtgiSrc -File | Copy-Item -Destination $rtgiDst -Force
}

# Installer + docs
Copy-Item (Join-Path $root 'tools/install-amd-presr-r17.ps1') (Join-Path $stage 'Setup.ps1') -Force
Copy-Item (Join-Path $root 'tools/install-amd-presr-r17.bat') (Join-Path $stage 'Setup.bat') -Force
# bat wrapper expects install-amd-presr-r17.ps1 next to it — rewrite to Setup.ps1
@'
@echo off
if "%~1"=="" (
  echo Usage: %~nx0 "C:\Path\To\Game\Content" [dxgi.dll]
  exit /b 1
)
set "PROXY=%~2"
if "%PROXY%"=="" set "PROXY=dxgi.dll"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup.ps1" -GameDir "%~1" -Proxy %PROXY%
exit /b %ERRORLEVEL%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Setup.bat') -Encoding ASCII

Copy-Item (Join-Path $root 'tools/README-r17.md') (Join-Path $stage 'README.md') -Force
Copy-Item (Join-Path $root 'tools/README-r17.md') (Join-Path $stage '使用说明.txt') -Force

# Refuse to ship proprietary user files if someone left them in stage
foreach ($bad in @('nvngx_dlss.dll','dlssnr_on_amd_weights.bin','version.dll','dlssnr_on_amd_setup.exe')) {
    if (Test-Path (Join-Path $stage $bad)) {
        throw "Refusing to package proprietary file: $bad"
    }
}

$hashes = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName).Hash, $_.FullName.Substring($stage.Length + 1)
    }
$hashes | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt')

New-Item -ItemType Directory -Force -Path (Join-Path $root $OutDir) | Out-Null
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
Write-Host "Staged: $stage"
Write-Host "Zip:    $zip"
Write-Host "Opti:   $OptiDll"
