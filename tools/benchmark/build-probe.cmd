@echo off
setlocal
cd /d "%~dp0..\.."
if "%~1"=="" (
  echo Usage: tools\benchmark\build-probe.cmd VULKAN_HEADERS_INCLUDE [OUTDIR]
  exit /b 1
)
set "BENCH_OUT=%~2"
if not defined BENCH_OUT set "BENCH_OUT=exports\benchmark-bin"
if not exist "%BENCH_OUT%" mkdir "%BENCH_OUT%"
call tests\_lib\msvc-env.cmd || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /I "%~1" tools\benchmark\gpu_probe.cpp /Fo"%BENCH_OUT%\gpu_probe.obj" /Fe"%BENCH_OUT%\gpu_probe.exe" /link OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\vulkan\vulkan-1.lib
exit /b %errorlevel%
