@echo off
rem lmxxf runtime and submission tests. Every test in tests\lmxxf belongs to exactly one tier below.
rem Usage: tests\lmxxf\run.cmd abi^|warp^|device^|gpu [out-dir]
rem   abi    : lmxxf_nr_abi, lmxxf_zero_fallback_abi.c, test_runtime_validation.py (no GPU)
rem   warp   : lmxxf_same_frame_boundary, lmxxf_color_probe (D3D12 WARP; no GPU)
rem   device : lmxxf_list_split, lmxxf_list1_wrap, lmxxf_create_execute, lmxxf_evaluate_cut
rem            (hardware D3D12 adapter)
rem   gpu    : lmxxf_nr_gpu (13 modes, 3 output hashes checked), lmxxf_bridge_zero_gpu (/std:c++17;
rem            fails under C++20 on hip_d3d12_bridge.h char8_t). Needs an AMD GPU and LMXXF_ASSETS =
rem            the weights folder (native-game-tiled-assets). Modules come from third_party\lmxxf\modules.
rem abi and gpu use LMXXF_TEST_RUNTIME when it names a built LmxxfNrRuntime.dll; otherwise they build
rem one into <out-dir>\runtime with tools\build\build-lmxxf-runtime.cmd.
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "TIER=%~1"
set "OUT=%~2"
if not defined OUT set "OUT=exports\test-run\lmxxf"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1
rem Repo-rooted includes (#include "OptiScaler-..." / "third_party/..."). The repo root goes at the END of
rem INCLUDE, not in /I: /I is searched before the SDK/STL dirs, and on a case-insensitive disk
rem <version> would then resolve to the repo's VERSION file.
set "INCLUDE=%INCLUDE%;%REPO%"
set "RT_INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime"
set "INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
set "MODS=%REPO%\third_party\lmxxf\modules"
set "CXX=cl /nologo /std:c++20 /EHsc /W4 /utf-8"
set "D3D=d3d12.lib dxgi.lib dxguid.lib uuid.lib"
if /i "%TIER%"=="abi" goto abi
if /i "%TIER%"=="warp" goto warp
if /i "%TIER%"=="device" goto device
if /i "%TIER%"=="gpu" goto gpu
echo usage: tests\lmxxf\run.cmd abi^|warp^|device^|gpu [out-dir]
exit /b 2

:abi
call :Runtime || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 tests\lmxxf\lmxxf_nr_abi.cpp /Fe"%OUT%\lmxxf_nr_abi.exe" /Fo"%OUT%\lmxxf_nr_abi.obj" || goto fail
"%OUT%\lmxxf_nr_abi.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
rem C host smoke test: export table, capabilities, create-flag accept/reject (no GPU).
cl /nologo /TC /W4 /I"%RT_INC%" tests\lmxxf\lmxxf_zero_fallback_abi.c /Fe"%OUT%\lmxxf_zero_fallback_abi.exe" /Fo"%OUT%\lmxxf_zero_fallback_abi.obj" || goto fail
rem It loads LmxxfNrRuntime.dll by bare name, i.e. from beside the exe: stage the DLL under test there.
copy /Y "%LMXXF_TEST_RUNTIME%" "%OUT%\LmxxfNrRuntime.dll" >nul || goto fail
"%OUT%\lmxxf_zero_fallback_abi.exe" || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_runtime_validation.py || goto fail
goto pass

:warp
%CXX% /I"%REPO%\third_party\lmxxf\src" tests\lmxxf\lmxxf_exposure_recording.cpp /Fe"%OUT%\lmxxf_exposure_recording.exe" /Fo"%OUT%\lmxxf_exposure_recording.obj" /link %D3D% || goto fail
"%OUT%\lmxxf_exposure_recording.exe" || goto fail
call tests\lmxxf\shader-compiler.cmd "%OUT%\shader-compiler" || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_recording_lifecycle.cpp /Fe"%OUT%\lmxxf_recording_lifecycle.exe" /Fo"%OUT%\lmxxf_recording_lifecycle.obj" /link %D3D% "%DETOURS%" || goto fail
"%OUT%\lmxxf_recording_lifecycle.exe" || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_same_frame_boundary.cpp /Fe"%OUT%\lmxxf_same_frame_boundary.exe" /Fo"%OUT%\lmxxf_same_frame_boundary.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || goto fail
"%OUT%\lmxxf_same_frame_boundary.exe" || goto fail
%CXX% tests\lmxxf\lmxxf_color_probe.cpp /Fe"%OUT%\lmxxf_color_probe.exe" /Fo"%OUT%\lmxxf_color_probe.obj" /link d3d12.lib dxgi.lib dxguid.lib || goto fail
"%OUT%\lmxxf_color_probe.exe" || goto fail
goto pass

:device
%CXX% tests\lmxxf\lmxxf_list_split.cpp /Fe"%OUT%\lmxxf_list_split.exe" /Fo"%OUT%\lmxxf_list_split.obj" /link %D3D% || goto fail
"%OUT%\lmxxf_list_split.exe" || goto fail
for %%T in (lmxxf_list1_wrap lmxxf_create_execute lmxxf_evaluate_cut) do (
  %CXX% /I"%INC%" tests\lmxxf\%%T.cpp /Fe"%OUT%\%%T.exe" /Fo"%OUT%\%%T.obj" /link %D3D% "%DETOURS%" || goto fail
  "%OUT%\%%T.exe" || goto fail
)
goto pass

:gpu
if not defined LMXXF_ASSETS (
  echo FAIL: set LMXXF_ASSETS to the lmxxf weights folder ^(native-game-tiled-assets^).
  goto fail
)
if not exist "%LMXXF_ASSETS%\block0-ffn.f16" if not exist "%LMXXF_ASSETS%\block0-ffn.f32" (
  echo FAIL: LMXXF_ASSETS=%LMXXF_ASSETS% has no block0-ffn weights.
  goto fail
)
call :Runtime || goto fail
rem The runtime resolves modules from its assets argument and weights from LMXXF_WEIGHTS_DIR.
rem Passing the weights folder itself would make it prefer the (possibly stale) HIP\ copy beside
rem the weights over this checkout's modules.
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
%CXX% /I"%RT_INC%" tests\lmxxf\lmxxf_nr_gpu.cpp /Fe"%OUT%\lmxxf_nr_gpu.exe" /Fo"%OUT%\lmxxf_nr_gpu.obj" /link d3d12.lib dxgi.lib || goto fail
for %%M in ("" "--resize" "--queue-mismatch" "--rgb9e5" "--reject-formats" "--exposure" "--exposure-bad" "--ultrawide" "--subrect") do (
  echo --- lmxxf_nr_gpu %%~M
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" %%~M || goto fail
)
rem Output-hash baselines: fixed pattern, 1920x1080, seed 1 (RX 9070 XT, this weights set).
rem Re-baselined 2026-09-29 for c0a6196 FMA/fusion stack (see work/notes/2026-09-29-fma-baseline-ab.md).
call :Hash fe40c904da05472e --output-hash || goto fail
call :Hash 79233836b6257864 --auto-exposure || goto fail
call :Hash 79233836b6257864 --auto-exposure --scale16 || goto fail
call :Hash 8ba14ef2db0dddfe --r10g10b10a2 || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DNOMINMAX /D_WIN32_WINNT=0x0A00 tests\lmxxf\lmxxf_bridge_recording_gpu.cpp /Fe"%OUT%\lmxxf_bridge_recording_gpu.exe" /Fo"%OUT%\lmxxf_bridge_recording_gpu.obj" /link d3d12.lib dxgi.lib user32.lib || goto fail
"%OUT%\lmxxf_bridge_recording_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DNOMINMAX /D_WIN32_WINNT=0x0A00 tests\lmxxf\lmxxf_bridge_zero_gpu.cpp /Fe"%OUT%\lmxxf_bridge_zero_gpu.exe" /Fo"%OUT%\lmxxf_bridge_zero_gpu.obj" /link d3d12.lib dxgi.lib || goto fail
"%OUT%\lmxxf_bridge_zero_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" || goto fail
"%OUT%\lmxxf_bridge_zero_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" --probe-drain || goto fail
goto pass

:pass
echo lmxxf %TIER%: PASS
exit /b 0

:fail
echo lmxxf %TIER%: FAIL
exit /b 1

:Runtime
if defined LMXXF_TEST_RUNTIME if exist "%LMXXF_TEST_RUNTIME%" exit /b 0
call "%REPO%\tools\build\build-lmxxf-runtime.cmd" "%OUT%\runtime" || exit /b 1
set "LMXXF_TEST_RUNTIME=%OUT%\runtime\LmxxfNrRuntime.dll"
exit /b 0

:Hash
set "EXPECT=%~1"
shift
set "MODE="
:HashArgs
if "%~1"=="" goto HashRun
set "MODE=%MODE% %~1"
shift
goto HashArgs
:HashRun
echo --- lmxxf_nr_gpu%MODE% (expect output_hash=%EXPECT%)
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%"%MODE% > "%OUT%\nr_gpu_hash.txt" 2>&1
set "RC=%errorlevel%"
type "%OUT%\nr_gpu_hash.txt"
if not "%RC%"=="0" exit /b 1
findstr /c:"output_hash=%EXPECT% " "%OUT%\nr_gpu_hash.txt" >nul || (
  echo FAIL: output hash differs from baseline %EXPECT%
  exit /b 1
)
exit /b 0
