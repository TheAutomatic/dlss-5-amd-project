[CmdletBinding()]
param([string]$Dest = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lmxxf-module-package.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Dest) { $Dest = Join-Path $repo 'exports/release-local' }
$Dest = [IO.Path]::GetFullPath($Dest)
$source = Join-Path $repo 'third_party/lmxxf/modules'
$target = Join-Path $Dest 'lmxxf-modules'
$runtime = Join-Path $repo 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
$shaders = Join-Path $repo 'third_party/lmxxf/shaders'
$stage = $null
try {
    Assert-LmxxfModulePackage $source
    Assert-LmxxfModuleDestination $target
    if (-not (Test-Path -LiteralPath (Join-Path $shaders 'native_codec_encode.hlsl') -PathType Leaf)) {
        throw 'Missing third_party/lmxxf/shaders'
    }
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        Push-Location $repo
        try {
            & cmd.exe /d /c tools\build-lmxxf-runtime.cmd
            if ($LASTEXITCODE -ne 0) { throw 'Runtime build failed.' }
        } finally { Pop-Location }
    }
    $stage = New-LmxxfModuleStage $source $target
    Publish-LmxxfModuleStage $stage $target
    $stage = $null
    Copy-Item -LiteralPath $runtime -Destination (Join-Path $Dest 'LmxxfNrRuntime.dll') -Force
    $shaderDest = Join-Path $Dest 'shaders'
    [void][IO.Directory]::CreateDirectory($shaderDest)
    Get-ChildItem -LiteralPath $shaders -Filter '*.hlsl' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $shaderDest -Force
    }
    Write-Host "Staged runtime, verified dual-architecture modules and shaders -> $Dest"
    Write-Host 'NOTE: weights via LMXXF_WEIGHTS_DIR=native-game-tiled-assets (not HIP/)'
} catch {
    Write-Host ("FAIL: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
} finally {
    if ($stage) { Remove-LmxxfTemporaryTree $stage ([IO.Path]::GetDirectoryName($stage)) }
}
