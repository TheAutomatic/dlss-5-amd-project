@echo off
rem P2 no-game harness: MSVC loader + MinGW runtime + third_party\lmxxf\modules path.
rem Still expects Record/Enqueue/Complete = NOT_IMPLEMENTED until HIP is wired.
rem Does not start a game. Does not change NrBackend default (daniel).
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
set "MODS=%~2"
if not defined MODS set "MODS=third_party\lmxxf\modules"
call "%~dp0test-lmxxf-nr-abi.cmd" "%OUT%" "%MODS%"
if not %errorlevel%==0 exit /b 1
if not exist "%MODS%\SHA256SUMS" (
  echo FAIL: missing %MODS%\SHA256SUMS
  exit /b 1
)
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_nr_gpu.cpp /I OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime /Fe"%OUT%\lmxxf_nr_gpu.exe" /Fo"%OUT%\lmxxf_nr_gpu.obj" /link d3d12.lib dxgi.lib
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_nr_gpu.exe" "%OUT%\LmxxfNrRuntime.dll" "%MODS%"
if not %errorlevel%==0 exit /b 1
echo lmxxf_nr_harness: PASS
exit /b 0
