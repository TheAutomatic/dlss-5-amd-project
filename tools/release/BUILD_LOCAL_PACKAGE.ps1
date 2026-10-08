[CmdletBinding()]
param([string]$Root, [string]$Version, [switch]$PlanOnly)
$ErrorActionPreference = 'Stop'
try {
    if (!$Root) {
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.FolderBrowserDialog
        $picker.Description = 'Select the dlssnr_on_amd_setup SOURCE folder'
        $picker.ShowNewFolderButton = $false
        try {
            if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
            $Root = $picker.SelectedPath
        } finally { $picker.Dispose() }
    }
    $Root = (Resolve-Path -LiteralPath $Root).Path
    if ($Root -match '[%!^"\r\n]') { throw 'Project path contains unsupported cmd expansion characters.' }
    foreach ($file in @('VERSION','tools/build/build-lmxxf-runtime.cmd',
        'tools/release/PACKAGE_RELEASE.ps1','tests/_lib/msvc-env.cmd',
        'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/OptiScaler.vcxproj')) {
        if (!(Test-Path -LiteralPath (Join-Path $Root $file) -PathType Leaf)) { throw "Not a source checkout: missing $file" }
    }
    if (!$Version) { $Version = Read-Host 'Package version (example: 1.9.10.3)' }
    $Version = $Version.Trim()
    if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-(?:alpha|beta|rc)(?:[.-]?[0-9]+)?)?$') { throw 'Use a version such as 1.9.10.3 or 1.10.5-alpha.' }
    $roots = @($Root)
    $common = & git -C $Root rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read checkout Git identity.' }
    $roots += Split-Path -Parent ([string]$common)
    $required = @('amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll',
        'amd_fidelityfx_framegeneration_dx12.dll','amd_fidelityfx_vk.dll',
        'libxess.dll','libxess_fg.dll','libxell.dll',
        'D3D12_OptiScaler/D3D12Core.dll')
    $deps = $null
    foreach ($base in ($roots | Select-Object -Unique)) {
        foreach ($area in @('dist','exports')) {
            $folder = Join-Path $base $area
            if (!(Test-Path -LiteralPath $folder)) { continue }
            $candidates = @(Get-ChildItem -LiteralPath $folder -Directory | Sort-Object LastWriteTime -Descending)
            $candidates += @($candidates | ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory })
            foreach ($candidate in $candidates) {
                $depDir = Join-Path $candidate.FullName 'OptiScaler'
                $missing = @($required | Where-Object { !(Test-Path -LiteralPath (Join-Path $depDir $_) -PathType Leaf) })
                if (!$missing.Count) { $deps = $candidate.FullName; break }
            }
            if ($deps) { break }
        }
        if ($deps) { break }
    }
    if (!$deps) { throw 'Extract an existing complete package into this checkout or its main checkout dist/exports, then retry.' }
    $mochi = Test-Path -LiteralPath (Join-Path $Root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp')
    if ($mochi -and !(Test-Path -LiteralPath (Join-Path $Root 'tools/build/build-mochizuki-runtime.cmd'))) { throw 'Mochizuki build script missing.' }
    $branch = & git -C $Root branch --show-current
    Write-Host "Project: $Root`nBranch: $branch`nVersion: $Version`nDependencies: $deps`nMochizuki: $mochi"
    Write-Host 'Full host rebuild and runtime builds. Local test package; no full release test suite.'
    if ($PlanOnly) { exit 0 }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $name = "OptScaler-NR-$Version-local-$stamp"
    $scratch = Join-Path $Root "work/scratch/local-package-$stamp"
    $logs = Join-Path $Root "exports/local-build-$stamp"
    New-Item -ItemType Directory -Path $scratch,$logs -Force | Out-Null
    $buildLock = [IO.File]::Open((Join-Path $Root 'exports/local-package.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try {
        $versionPath = Join-Path $Root 'VERSION'
        $oldVersion = [IO.File]::ReadAllBytes($versionPath)
        $batch = @'
@echo off
setlocal
call tests\_lib\msvc-env.cmd || exit /b 1
call tools\build\build-lmxxf-runtime.cmd exports\lmxxf-runtime || exit /b 1
'@
        if ($mochi) { $batch += "`r`ncall tools\build\build-mochizuki-runtime.cmd || exit /b 1" }
        $batch += @'

if not exist exports\release-local mkdir exports\release-local
"%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo
if errorlevel 1 exit /b 1
if not exist exports\release-local\OptiScaler.dll exit /b 1
exit /b 0
'@
        $batchPath = Join-Path $scratch 'build.cmd'
        [IO.File]::WriteAllText($batchPath,($batch -replace "`r?`n","`r`n"),[Text.Encoding]::ASCII)
        Push-Location $Root
        try {
            [IO.File]::WriteAllText($versionPath,"$Version`r`n",[Text.Encoding]::ASCII)
            & $env:ComSpec /d /c "`"$batchPath`"" 2>&1 | Tee-Object -FilePath (Join-Path $logs 'build.log')
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed; see $logs\build.log" }
            . (Join-Path $Root 'tools/release/package-signing.ps1')
            $signingCertificate = Get-PackageSigningCertificate
            $ps = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
            & $ps -NoProfile -ExecutionPolicy Bypass -File tools/release/PACKAGE_RELEASE.ps1 `
                -LocalTest -Version $Version -Name $name -OutDir dist `
                -SigningThumbprint $signingCertificate.Thumbprint `
                -OptiDll exports/release-local/OptiScaler.dll -DepsRoot $deps 2>&1 |
                Tee-Object -FilePath (Join-Path $logs 'package.log')
            if ($LASTEXITCODE -ne 0) { throw "Packaging failed; see $logs\package.log" }
            $zip = Join-Path $Root "dist/$name.zip"
            if (!(Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'ZIP not produced.' }
            Write-Host "`nSUCCESS: $zip" -ForegroundColor Green
            Write-Host "VERSION is now $Version. No commit, tag or upload was made."
        } catch {
            [IO.File]::WriteAllBytes($versionPath,$oldVersion)
            throw
        } finally { Pop-Location }
    } finally { $buildLock.Dispose() }
} catch {
    Write-Host "`nFAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
