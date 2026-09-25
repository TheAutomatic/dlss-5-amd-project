<#
.SYNOPSIS
  Stage and zip a complete user package (no NVIDIA / danielblnc proprietary files).
  Default product: OptiScaler-AMD-PreSR-1.9.0
    1.9.0  = this fork's product version
    0.3.2  = supported danielblnc runtime (0.3.1 / 0.3.0 also accepted)
    lmxxf  = supported lmxxf HIP neural rendering runtime

.EXAMPLE
  .\PACKAGE_RELEASE.ps1
  .\PACKAGE_RELEASE.ps1 -Version 1.9.0 -DepsRoot 'C:\path\with\OptiScaler'
#>
[CmdletBinding()]
param(
    [string]$Version = '1.9.0.3',
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
; Product $Version - Dual-backend AMD Neural Rendering (DLSS 5 on AMD) Pre-SR pipeline.
; Synthesizes detail and denoises ray-traced inputs before upscaling (FSR/XeSS).

; Enables DLSS-NR Pre-SR pipeline
; true or false - Program default is false. Setup turns this on.
Enabled=false

; Controls whether neural rendering executes before the upscaler
; When true, runs on the pre-upscale colour texture before FSR/XeSS
; true or false - This package sets true. If the key is absent, the program uses false.
RunBeforeSR=true

; Selects the neural rendering backend
; lmxxf  - Open-source AMD HIP neural rendering pipeline (using native-game-tiled-assets)
; daniel - danielblnc 0.3.0 / 0.3.1 / 0.3.2 runtime (using dlssnr_amd_pass*.dll + weights.bin)
; lmxxf or daniel only. Turn the pass off with Enabled=false, not with NrBackend.
; If the chosen host is missing its files, the other installed host runs instead.
NrBackend=lmxxf

; Diagnostic mode for lmxxf backend (NO NR)
; off              - Normal neural rendering operation
; original         - Passthrough original colour
; copy-current     - Diagnostic copy of current frame colour
; staging-current  - Staging isolation test with current frame
; staging-previous - Staging isolation test with previous frame
; off, original, copy-current, staging-current, staging-previous - Default is off
LmxxfDiagnostic=off

; Fit Color inputs above 1920x1080 onto the 1080 network (upstream DLSS5_FIT_LARGE)
; Live from the Ins menu (runtime re-reads the env whenever it checks FitLarge). Installer also writes DLSS5-AMD\native-game-flags.txt. true/false - Default is false
LmxxfFitLarge=false

; PDL chained launch. true by default. false sets DLSS5_HIP_PDL=0 so a driver
; without hipExtModuleLaunchKernel can still start the network. Restart after changing.
LmxxfPdl=true

; Codec paper white for lmxxf encode and decode. Finite and in (0, 64]. Default is 1.
; Not the HDR Paper White anchor.
LmxxfPaperWhite=1

; Resolution scale factor for neural rendering model input
; 1.0 = native render resolution (e.g. 720p for 4K Super Performance)
; float value - Default is 1
AmdModelScale=1

; Encoding format mode for model inputs/outputs
; 0 = FP16 (standard), 1 = FP8 / compressed
; Integer value - Default is 0
AmdEncoding=0

; Schedule neural rendering execution on every frame
; When enabled, avoids skipping frames; multi-slot pipeline skips post-execute wait
; true or false - Default is true
AmdEveryFrame=true

; Number of pipeline slots for asynchronous GPU execution (danielblnc backend)
; Higher values reduce wait time at the cost of VRAM (each slot holds intermediate buffers)
; Recommended: 3 slots (tested 0 wait on yysls)
; 1 to 5 - Default is 3
AmdSlots=3

; GPU synchronization / wait mode between NR and game command queue (danielblnc backend)
; 0 = Original wait (compute dispatch spin)
; 1 = New wait mode (1-pixel graphics draw spin; prevents GPU watchdog resets)
; 0 or 1 - Default is 1
AmdGraphicsWait=1

; Allow unsafe dirty command insertion when graphics state admission fails
; 0 = Safe mode (fallback to original color on state conflict)
; 1 = Unsafe mode (force injection; higher risk of visual glitch)
; 0 or 1 - Default is 0
AmdGraphicsUnsafe=0

; Experimental neural lighting pass (Gather/Resolve shaders)
; true or false - Default is true
AmdNeuralLighting=true

; Intensity of the neural lighting effect
; float value (0.0 to 1.0) - Default is 0.5
AmdNeuralLightingStrength=0.5

; Number of neural rendering passes
; 1 to 3 - Default is 1
Passes=1

; Weight for preserving local tone mapping
; float value - Default is 1
LocalTone=1

; Weight for preserving fine local structure and edges
; float value - Default is 1
LocalStructure=1

; Weight for preserving skin structure and texture
; -1 follows local structure (the model's own default), not a strength of zero
; float value - Default is -1
SkinStructure=-1

; Apply neural rendering adjustments after Ray Reconstruction
; true or false - Default is false
ApplyAfterRR=false

"@)
# These sections are only appended if they do not already exist in the source ini.
# Unconditional appending would duplicate sections if source ini ever includes [AmdLook]/[AmdRtgi].
# The appended block has Enabled=false to avoid overriding user values.
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

# Bundled open-source lmxxf runtime binaries, modules, and shaders
$lmxxfDllSrc = Join-Path $root 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
if (!(Test-Path -LiteralPath $lmxxfDllSrc -PathType Leaf)) {
    throw "Required LmxxfNrRuntime.dll not found at $lmxxfDllSrc! Build it first with tools\build-lmxxf-runtime.cmd."
}
Copy-Item -LiteralPath $lmxxfDllSrc -Destination (Join-Path $stage 'LmxxfNrRuntime.dll') -Force

$lmxxfModSrc = Join-Path $root 'third_party/lmxxf/modules'
if (Test-Path -LiteralPath $lmxxfModSrc -PathType Container) {
    $lmxxfModDst = Join-Path $stage 'lmxxf-modules'
    New-Item -ItemType Directory -Path $lmxxfModDst -Force | Out-Null
    Copy-Item -Path (Join-Path $lmxxfModSrc '*') -Destination $lmxxfModDst -Recurse -Force
}

$lmxxfShaderSrc = Join-Path $root 'third_party/lmxxf/shaders'
if (Test-Path -LiteralPath $lmxxfShaderSrc -PathType Container) {
    $lmxxfShaderDst = Join-Path $stage 'shaders'
    New-Item -ItemType Directory -Path $lmxxfShaderDst -Force | Out-Null
    # Live glue: top-level *.hlsl only. shader-cache/*.dxbc is a local compile cache and is not shipped.
    Get-ChildItem -LiteralPath $lmxxfShaderSrc -Filter '*.hlsl' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $lmxxfShaderDst $_.Name) -Force
    }
}

# Installer + docs (CN + EN + ES). No duplicate 使用说明.txt.
$readmeZh = Join-Path $root 'README.md'
$readmeEn = Join-Path $root 'README.en.md'
$readmeEs = Join-Path $root 'README.es.md'
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
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1" -Proxy "%~2" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Setup failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Setup.bat') -Encoding ASCII

$uninstallSrc = Join-Path $root 'tools/uninstall-amd-presr.ps1'
if (!(Test-Path $uninstallSrc)) { throw "Missing $uninstallSrc" }
Copy-Item $uninstallSrc (Join-Path $stage 'Uninstall_OptiScaler_NR.ps1') -Force
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
'@ | Set-Content -LiteralPath (Join-Path $stage 'Uninstall_OptiScaler_NR.bat') -Encoding ASCII
Copy-Item $readmeZh (Join-Path $stage 'README.md') -Force
Copy-Item $readmeEn (Join-Path $stage 'README.en.md') -Force
if (Test-Path $readmeEs) {
    Copy-Item $readmeEs (Join-Path $stage 'README.es.md') -Force
}

# 绊线：这些文件名一旦出现在 stage 里就拒绝打包（含子目录，例如 Agility 误扫入 version.dll）。
# danielblnc pass（dlssnr_amd_pass*.dll）必须不在包内 —— README 明写「包里没有 danielblnc pass」。
# Keep this filename-only and case-insensitive: the same expression validates the
# staged tree and every entry in the finished archive.
$forbidden = '(?i)^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|version\.dll|dlssnr_on_amd_setup\.exe|.*\.generated\.hip|.*\.hsaco\.s)$'
$badAll = Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match $forbidden }
if ($badAll) {
    throw "Refusing to package proprietary/user-supplied file: $(($badAll | ForEach-Object { $_.FullName.Substring($stage.Length+1) }) -join ', ')"
}

# Validate dual-arch modules structure if staged
$stagedMods = Join-Path $stage 'lmxxf-modules'
if (Test-Path -LiteralPath $stagedMods -PathType Container) {
    $stagedRootSums = Join-Path $stagedMods 'SHA256SUMS'
    if (!(Test-Path -LiteralPath $stagedRootSums -PathType Leaf)) {
        throw "Staged lmxxf-modules is missing root SHA256SUMS manifest"
    }
    $stagedManifest = Join-Path $stagedMods 'runtime-manifest.json'
    if (!(Test-Path -LiteralPath $stagedManifest -PathType Leaf)) {
        throw "Staged lmxxf-modules is missing runtime-manifest.json"
    }
    $stagedRootHsaco = @(Get-ChildItem -LiteralPath $stagedMods -Filter '*.hsaco' -File)
    if ($stagedRootHsaco.Count -gt 0) {
        throw "Staged lmxxf-modules contains legacy flat .hsaco modules in root: $(($stagedRootHsaco | ForEach-Object { $_.Name }) -join ', ')"
    }
    if (Test-Path -LiteralPath (Join-Path $stagedMods 'modules.json') -PathType Leaf) {
        throw "Staged lmxxf-modules contains legacy modules.json in root (must be per-arch only)"
    }
    foreach ($arch in @('gfx1200', 'gfx1201')) {
        $archDir = Join-Path $stagedMods $arch
        if (!(Test-Path -LiteralPath $archDir -PathType Container)) {
            throw "Staged lmxxf-modules is missing required architecture directory: $arch"
        }
        if (!(Test-Path -LiteralPath (Join-Path $archDir 'SHA256SUMS') -PathType Leaf)) {
            throw "Staged lmxxf-modules/$arch is missing leaf SHA256SUMS"
        }
        if (!(Test-Path -LiteralPath (Join-Path $archDir 'modules.json') -PathType Leaf)) {
            throw "Staged lmxxf-modules/$arch is missing leaf modules.json"
        }
        $archHsaco = @(Get-ChildItem -LiteralPath $archDir -Filter '*.hsaco' -File)
        if ($archHsaco.Count -ne 24) {
            throw "Staged lmxxf-modules/$arch must contain exactly 24 .hsaco modules, found $($archHsaco.Count)"
        }
    }
    $totalHsaco = @(Get-ChildItem -LiteralPath $stagedMods -Filter '*.hsaco' -Recurse -File)
    if ($totalHsaco.Count -ne 48) {
        throw "Staged lmxxf-modules must contain exactly 48 .hsaco modules in total, found $($totalHsaco.Count)"
    }
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

# Validate the actual artifact, not only the staging tree. This catches a changed
# archive command, a stale/wrapped staging directory, or anything injected between
# the preflight above and Compress-Archive. An unreadable archive also fails closed.
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $badZip = @($archive.Entries |
            Where-Object { -not [string]::IsNullOrEmpty($_.Name) -and $_.Name -match $forbidden } |
            ForEach-Object { $_.FullName })
        $entryNames = @($archive.Entries | ForEach-Object { $_.Name })
    } finally {
        $archive.Dispose()
    }
    if ($badZip.Count -gt 0) {
        throw "Forbidden proprietary/user-supplied file in zip: $($badZip -join ', ')"
    }
    $requiredNames = @('OptiScaler.dll', 'LmxxfNrRuntime.dll', 'OptiScaler.ini', 'Setup.ps1', 'Setup.bat')
    foreach ($req in $requiredNames) {
        if ($entryNames -notcontains $req) {
            throw "Package archive missing required component: $req"
        }
    }
    # Validate dual-arch modules in zip archive
    $zipHsaco = @($archive.Entries | Where-Object { $_.FullName -like 'lmxxf-modules/*.hsaco' -or $_.FullName -like 'lmxxf-modules/*/*.hsaco' })
    if ($zipHsaco.Count -gt 0) {
        $rootZipHsaco = @($archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -match '^lmxxf-modules/[^/]+\.hsaco$' })
        if ($rootZipHsaco.Count -gt 0) {
            throw "Package archive contains flat .hsaco modules in lmxxf-modules root"
        }
        $g1200Hsaco = @($archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -match '^lmxxf-modules/gfx1200/[^/]+\.hsaco$' })
        if ($g1200Hsaco.Count -ne 24) {
            throw "Package archive gfx1200 must have 24 .hsaco entries, found $($g1200Hsaco.Count)"
        }
        $g1201Hsaco = @($archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -match '^lmxxf-modules/gfx1201/[^/]+\.hsaco$' })
        if ($g1201Hsaco.Count -ne 24) {
            throw "Package archive gfx1201 must have 24 .hsaco entries, found $($g1201Hsaco.Count)"
        }
        $requiredModEntries = @(
            'lmxxf-modules/SHA256SUMS',
            'lmxxf-modules/runtime-manifest.json',
            'lmxxf-modules/gfx1200/SHA256SUMS',
            'lmxxf-modules/gfx1200/modules.json',
            'lmxxf-modules/gfx1201/SHA256SUMS',
            'lmxxf-modules/gfx1201/modules.json'
        )
        $allEntryFullNames = @($archive.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        foreach ($rme in $requiredModEntries) {
            if ($allEntryFullNames -notcontains $rme) {
                throw "Package archive missing required module entry: $rme"
            }
        }
    }
} catch {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    throw "Package archive validation failed; zip removed: $($_.Exception.Message)"
}
Write-Host "Product: $Version"
Write-Host "Staged:  $stage"
Write-Host "Zip:     $zip"
Write-Host "Opti:    $OptiDll"
