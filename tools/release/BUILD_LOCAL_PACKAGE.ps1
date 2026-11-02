[CmdletBinding()]
param([string]$Root, [string]$Version, [switch]$PlanOnly, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
function Invoke-LoggedNative {
    param([string]$Executable, [string[]]$Arguments, [string]$Log, [string]$Failure)
    # Windows PowerShell 5.1 wraps native stderr in ErrorRecord, even for an exit-0 warning.
    # Keep it in the log and use the process exit code to decide success. Script errors still stop.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $Log -ErrorAction Stop
    $nativeExit = $LASTEXITCODE
    if ($nativeExit -ne 0) { throw "$Failure (exit $nativeExit); see $Log" }
}
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
    if (!$Version) {
        $defaultVersion = ([IO.File]::ReadAllText((Join-Path $Root 'VERSION'))).Trim()
        $entered = Read-Host "版本 [$defaultVersion] 回车沿用，或输入新版本"
        if ([string]::IsNullOrWhiteSpace($entered)) { $Version = $defaultVersion } else { $Version = $entered }
    }
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
    if (!$deps) { Write-Host 'No previous package found; using dependency binaries from the source checkout (git submodule update --init --recursive if missing).' }
    # Neural Rendering ships the official ASI payload; the retired built-in runtime
    # source is not a packaging dependency.
    $branch = & git -C $Root branch --show-current
    Write-Host "Project: $Root`nBranch: $branch`nVersion: $Version`nDependencies: $deps`nMochizuki: $mochi"
    Write-Host "Build mode: $(if ($Rebuild) { 'full rebuild' } else { 'incremental host; verified Mochizuki/Worker reuse; lmxxf rebuilt' }). Local test package; no full release test suite."
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
powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\prepare-person-assets.ps1
if errorlevel 1 exit /b 1
call tools\build\build-lmxxf-runtime.cmd exports\lmxxf-runtime || exit /b 1
'@
        $batch += @'

if not exist exports\release-local mkdir exports\release-local
"%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:@HOST_TARGET@ /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo
if errorlevel 1 exit /b 1
if not exist exports\release-local\OptiScaler.dll exit /b 1
if "@HOST_TARGET@"=="Build" if exist exports\release-local\person-model\person-worker.exe (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\check-person-worker.ps1 -WorkerExe exports\release-local\person-model\person-worker.exe
  if not errorlevel 1 goto workerReady
)
call tools\build\build-person-worker.cmd exports\release-local\person-model
if errorlevel 1 exit /b 1
:workerReady
if not exist exports\release-local\person-model\person-worker.exe exit /b 1
exit /b 0
'@
        $batch = $batch.Replace('@HOST_TARGET@', $(if ($Rebuild) { 'Rebuild' } else { 'Build' }))
        $batchPath = Join-Path $scratch 'build.cmd'
        [IO.File]::WriteAllText($batchPath,($batch -replace "`r?`n","`r`n"),[Text.Encoding]::ASCII)
        Push-Location $Root
        try {
            [IO.File]::WriteAllText($versionPath,"$Version`r`n",[Text.Encoding]::ASCII)
            Invoke-LoggedNative -Executable $env:ComSpec -Arguments @('/d', '/c', "`"$batchPath`"") `
                -Log (Join-Path $logs 'build.log') -Failure 'Compilation failed'
            . (Join-Path $Root 'tools/release/package-signing.ps1')
            $signingCertificate = Get-PackageSigningCertificate
            $ps = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
            $packageArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', 'tools/release/PACKAGE_RELEASE.ps1', '-LocalTest', '-Version', $Version,
                '-Name', $name, '-OutDir', 'dist', '-SigningThumbprint', $signingCertificate.Thumbprint,
                '-OptiDll', 'exports/release-local/OptiScaler.dll')
            if ($deps) { $packageArgs += @('-DepsRoot', $deps) }
            Invoke-LoggedNative -Executable $ps -Arguments $packageArgs `
                -Log (Join-Path $logs 'package.log') -Failure 'Packaging failed'
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
