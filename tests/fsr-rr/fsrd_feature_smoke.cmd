@echo off
rem Build/stage only. Never launches a GPU workload.
rem Usage: tests\fsr-rr\fsrd_feature_smoke.cmd STAGE HOST_DLL PROVIDER_DIRECTORY [auto^|indirect^|split]
rem HOST_DLL must be the freshly built production OptiScaler.dll (not a fixture DLL).
rem PROVIDER_DIRECTORY contains the owned real AMD loader, RR 1.2 and SR providers.
rem Run afterward: STAGE\fsrd_feature_smoke.exe --luid HEX_LUID --guided-mask 12
rem For split use --guided-mask 15. Each route requires a fresh STAGE directory.
setlocal EnableExtensions
if "%~3"=="" (
  echo Usage: %~nx0 STAGE HOST_DLL PROVIDER_DIRECTORY [auto^|indirect^|split]
  exit /b 2
)
set "STAGE=%~f1"
set "HOST=%~f2"
set "PROVIDERS=%~f3"
set "ROUTE=%~4"
if not defined ROUTE set "ROUTE=auto"
set "TESTDIR=%~dp0"
set "REPO=%~dp0..\.."
call "%REPO%\tests\_lib\msvc-env.cmd" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%TESTDIR%fsrd_feature_smoke_stage.ps1" -Stage "%STAGE%" -HostDll "%HOST%" -Providers "%PROVIDERS%" -Route "%ROUTE%" || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\external\nvngx_dlss_sdk" "%TESTDIR%fsrd_feature_smoke.cpp" /Fe"%STAGE%\fsrd_feature_smoke.exe" /Fo"%STAGE%\fsrd_feature_smoke.obj" /link d3d12.lib dxgi.lib dxguid.lib user32.lib || exit /b 1
echo Built native production NGX consumer only; no product code statically linked, no GPU run.
echo Run: "%STAGE%\fsrd_feature_smoke.exe" --luid HEX_ADAPTER_LUID --guided-mask 12
if /i "%ROUTE%"=="split" echo Split route requires --guided-mask 15 instead.
exit /b 0
