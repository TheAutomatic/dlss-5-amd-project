@echo off
rem FSR-RR offline performance and correctness tests.
rem Usage: tests\fsr-rr\run.cmd [cpu^|gpu^|perf] [out-dir]
rem   cpu  : ABI/contract only, no GPU required (default)
rem   gpu  : correctness chain with real denoiser DLL and RX 9070 XT
rem   perf : performance baseline matrix (P4); requires gpu prerequisites
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "TIER=%~1"
if not defined TIER set "TIER=cpu"
set "OUT=%~2"
if not defined OUT set "OUT=exports\test-run\fsr-rr"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"

call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1

set "CXX=cl /nologo /std:c++20 /EHsc /W4 /utf-8"
set "EXT=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\external"
set "INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
set "INCLUDE=%INCLUDE%;%REPO%;%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler;%EXT%\nvngx_dlss_sdk;%EXT%\xess\inc\xess;%EXT%\xess\inc\xell;%EXT%\xess\inc\xess_fg;%EXT%\FidelityFX-SDK\ffx-api\include\ffx_api;%EXT%\simpleini;%EXT%\unordered_dense\include;%EXT%\spdlog\include;%EXT%\freetype;%EXT%\streamline;%EXT%\streamline1;%EXT%\nvapi;%EXT%\nlohmann;%EXT%\fakenvapi;%EXT%\magic_enum\include\magic_enum;%EXT%\AntiLag2-SDK;%EXT%\latencyflex;%EXT%\vulkan\include"

if /i "%TIER%"=="cpu" goto Cpu
if /i "%TIER%"=="gpu" goto Gpu
if /i "%TIER%"=="perf" goto Perf
echo usage: tests\fsr-rr\run.cmd [cpu^|gpu^|perf] [out-dir]
exit /b 2

:Cpu
echo === fsr-rr cpu: ABI + contract ===
%CXX% /DUNICODE /D_UNICODE /I"%INC%" tests\fsr-rr\abi_contract.cpp OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiTypes.cpp /Fe"%OUT%\abi_contract.exe" /Fo"%OUT%/" /link "%DETOURS%" dxgi.lib || goto fail
"%OUT%\abi_contract.exe" || goto fail
echo fsr-rr cpu: PASS
exit /b 0

:Gpu
echo === fsr-rr gpu: offline chain correctness ===
%CXX% /DUNICODE /D_UNICODE /I"%INC%" tests\fsr-rr\offline_chain.cpp /Fe"%OUT%\offline_chain.exe" /Fo"%OUT%/" /link d3d12.lib dxgi.lib dxguid.lib || goto fail
"%OUT%\offline_chain.exe" || goto fail
echo fsr-rr gpu: PASS
exit /b 0

:Perf
echo === fsr-rr perf: baseline matrix (P4) ===
%CXX% /O2 /DUNICODE /D_UNICODE /I"%INC%" tests\fsr-rr\offline_chain.cpp /DPERF_MODE /Fe"%OUT%\perf_chain.exe" /Fo"%OUT%/" /link d3d12.lib dxgi.lib dxguid.lib || goto fail
"%OUT%\perf_chain.exe" --perf --outdir "%OUT%" || goto fail
echo fsr-rr perf: PASS
exit /b 0

:fail
echo fsr-rr %TIER%: FAIL
exit /b 1
