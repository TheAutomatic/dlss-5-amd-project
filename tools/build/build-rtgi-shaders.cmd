@echo off
rem Rebuild the experimental-lighting (RTGI) compute shaders from assets\experimental_lighting\Lighting.hlsl.
rem Usage: tools\build\build-rtgi-shaders.cmd [out-dir]
rem Default out-dir is assets\experimental_lighting (the shipped .cso files). fxc 10.0.26100
rem /T cs_5_0 /O1 reproduces the committed GatherCS.cso and ResolveCS.cso byte for byte.
setlocal
cd /d "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=assets\experimental_lighting"
if not exist "%OUT%" mkdir "%OUT%"
where fxc.exe >nul 2>&1 || call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul 2>&1
where fxc.exe >nul 2>&1 || (
  echo FAIL: fxc.exe not found. Run from an MSVC x64 developer prompt with the Windows SDK.
  exit /b 1
)
for %%E in (GatherCS ResolveCS) do (
  fxc /nologo /T cs_5_0 /E %%E /O1 /Fo "%OUT%\%%E.cso" "assets\experimental_lighting\Lighting.hlsl"
  if errorlevel 1 (
    echo FAIL: fxc %%E
    exit /b 1
  )
)
echo BUILD_OK %OUT%\GatherCS.cso %OUT%\ResolveCS.cso
exit /b 0
