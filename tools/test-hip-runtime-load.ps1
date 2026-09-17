# Requires an x64 MSVC developer environment. Does not load the machine HIP runtime.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$out = $args[0]
if ([string]::IsNullOrWhiteSpace($out)) { $out = "exports\hip-fallback-tests" }
$out = [System.IO.Path]::GetFullPath($out)
$build = Join-Path $out "fixture-build"
New-Item -ItemType Directory -Force -Path $out, $build | Out-Null

function Invoke-Cl {
    param([string[]]$ClArgs)
    & cl @ClArgs
    if ($LASTEXITCODE -ne 0) { throw "cl failed: $($ClArgs -join ' ')" }
}

function Invoke-HipTest {
    param([string[]]$TestArgs)
    & (Join-Path $out "amd_hip_load.exe") @TestArgs
    if ($LASTEXITCODE -ne 0) { throw "amd_hip_load failed: $($TestArgs -join ' ')" }
}

function Install-Fixture {
    param([string]$Name)
    $bin = Join-Path $out (Join-Path $Name "bin")
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Copy-Item -Force (Join-Path $build "amd_hip_fixture_dependency.dll") (Join-Path $bin "amd_hip_fixture_dependency.dll")
    Copy-Item -Force (Join-Path $build "fixture.dll") (Join-Path $bin "amdhip64_7.dll")
    $dll = Join-Path $bin "amdhip64_7.dll"
    Invoke-HipTest @("--dependency", $dll)
    Invoke-HipTest @("--loaded", $dll)
}

Invoke-Cl @(
    "/nologo", "/std:c++20", "/EHsc", "/W4", "/utf-8",
    "tests\amd_hip_load.cpp",
    "/Fe$(Join-Path $out 'amd_hip_load.exe')",
    "/Fo$(Join-Path $out 'amd_hip_load.obj')"
)
Invoke-HipTest

Invoke-Cl @(
    "/nologo", "/LD", "/DAMD_HIP_DEPENDENCY_FIXTURE",
    "tests\amd_hip_load_fixture.cpp",
    "/Fe$(Join-Path $build 'amd_hip_fixture_dependency.dll')",
    "/Fo$(Join-Path $build 'fixture_dependency.obj')"
)
Invoke-Cl @(
    "/nologo", "/LD",
    "tests\amd_hip_load_fixture.cpp",
    (Join-Path $build "amd_hip_fixture_dependency.lib"),
    "/Fe$(Join-Path $build 'fixture.dll')",
    "/Fo$(Join-Path $build 'fixture.obj')"
)

Install-Fixture "sdk space"
Install-Fixture "SDK $([char]0x4E2D)$([char]0x6587)"
Write-Host "HIP runtime load tests passed"
