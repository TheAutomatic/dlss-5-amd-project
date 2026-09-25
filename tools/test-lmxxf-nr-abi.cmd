@echo off
rem Builds the MinGW runtime then proves the C ABI from an MSVC loader.
rem Optional arg2: modules directory. Defaults to third_party\lmxxf\modules.
rem Requires x64 MSVC cl in PATH (same env as test-amd-host-contracts.cmd).
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
set "MODS=%~2"
if not defined MODS set "MODS=third_party\lmxxf\modules"
call "%~dp0build-lmxxf-runtime.cmd" "%OUT%"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_nr_abi.cpp /I OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime /Fe"%OUT%\lmxxf_nr_abi.exe" /Fo"%OUT%\lmxxf_nr_abi.obj"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_nr_abi.exe" "%OUT%\LmxxfNrRuntime.dll" "%MODS%"
if not %errorlevel%==0 exit /b 1
echo lmxxf_nr_abi: PASS (modules=%MODS%)
rem C host smoke test: export table, capabilities, create-flag accept/reject (no GPU).
cl /nologo /TC /W4 tests\lmxxf_zero_fallback_abi.c /I OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime /Fe"%OUT%\lmxxf_zero_fallback_abi.exe" /Fo"%OUT%\lmxxf_zero_fallback_abi.obj"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_zero_fallback_abi.exe"
if not %errorlevel%==0 exit /b 1
exit /b 0
