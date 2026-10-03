@echo off
rem lmxxf runtime and submission tests. Every test in tests\lmxxf belongs to exactly one tier below.
rem Usage: tests\lmxxf\run.cmd abi^|warp^|device^|gpu [out-dir]
rem   abi    : lmxxf_nr_abi, lmxxf_zero_fallback_abi.c, test_runtime_validation.py (no GPU)
rem   warp   : lmxxf_same_frame_boundary, lmxxf_color_probe (D3D12 WARP; no GPU)
rem   device : lmxxf_state_object, lmxxf_list_split, lmxxf_list1_wrap, lmxxf_create_execute,
rem            lmxxf_evaluate_cut (hardware D3D12; state_object requires SDK DXC and DXR)
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
%AMD_TEST_PYTHON% tests\lmxxf\test_fullframes.py || goto fail
%AMD_TEST_PYTHON% tests\lmxxf\test_capture19.py || goto fail
call :Runtime || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 tests\lmxxf\lmxxf_nr_abi.cpp /Fe"%OUT%\lmxxf_nr_abi.exe" /Fo"%OUT%\lmxxf_nr_abi.obj" || goto fail
"%OUT%\lmxxf_nr_abi.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
rem C host smoke test: export table, capabilities, create-flag accept/reject (no GPU).
cl /nologo /TC /W4 /I"%RT_INC%" tests\lmxxf\lmxxf_zero_fallback_abi.c /Fe"%OUT%\lmxxf_zero_fallback_abi.exe" /Fo"%OUT%\lmxxf_zero_fallback_abi.obj" || goto fail
rem It loads LmxxfNrRuntime.dll by bare name, i.e. from beside the exe: stage the DLL under test there.
copy /Y "%LMXXF_TEST_RUNTIME%" "%OUT%\LmxxfNrRuntime.dll" >nul || goto fail
"%OUT%\lmxxf_zero_fallback_abi.exe" || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_runtime_validation.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_highlight_capture.py || goto fail
goto pass

:warp
%CXX% tests\lmxxf\lmxxf_temporal.cpp /Fe"%OUT%\lmxxf_temporal.exe" /Fo"%OUT%\lmxxf_temporal.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_temporal.exe" || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_same_frame_boundary.cpp /Fe"%OUT%\lmxxf_same_frame_boundary.exe" /Fo"%OUT%\lmxxf_same_frame_boundary.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || goto fail
"%OUT%\lmxxf_same_frame_boundary.exe" || goto fail
%CXX% tests\lmxxf\lmxxf_color_probe.cpp /Fe"%OUT%\lmxxf_color_probe.exe" /Fo"%OUT%\lmxxf_color_probe.obj" /link d3d12.lib dxgi.lib dxguid.lib || goto fail
"%OUT%\lmxxf_color_probe.exe" || goto fail
%CXX% /DNOMINMAX /D_WIN32_WINNT=0x0A00 tests\lmxxf\lmxxf_flicker.cpp /Fe"%OUT%\lmxxf_flicker.exe" /Fo"%OUT%\lmxxf_flicker.obj" /link d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib bcrypt.lib user32.lib || goto fail
"%OUT%\lmxxf_flicker.exe" || goto fail
%CXX% /DNOMINMAX /D_WIN32_WINNT=0x0A00 tests\lmxxf\lmxxf_flicker18.cpp /Fe"%OUT%\lmxxf_flicker18.exe" /Fo"%OUT%\lmxxf_flicker18.obj" /link d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib bcrypt.lib user32.lib || goto fail
"%OUT%\lmxxf_flicker18.exe" || goto fail
%CXX% /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I"third_party\lmxxf\src" tests\lmxxf\lmxxf_flicker19.cpp /Fe"%OUT%\lmxxf_flicker19.exe" /Fo"%OUT%\lmxxf_flicker19.obj" /link d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib bcrypt.lib user32.lib || goto fail
"%OUT%\lmxxf_flicker19.exe" || goto fail
%CXX% /DNOMINMAX /I"OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler" /I"OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include" /I"OptiScaler-DLSSNR-PreSR-Multipass-main\external\freetype" tests\lmxxf\nr_overlay19.cpp "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\misc\freetype\imgui_freetype.cpp" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\imgui.cpp" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\imgui_draw.cpp" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\imgui_tables.cpp" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\imgui_widgets.cpp" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui\imgui_impl_dx11.cpp" /Fe"%OUT%\nr_overlay19.exe" /Fo"%OUT%\\" /link "OptiScaler-DLSSNR-PreSR-Multipass-main\external\freetype\freetype.lib" d3d11.lib d3dcompiler.lib dxgi.lib user32.lib || goto fail
"%OUT%\nr_overlay19.exe" || goto fail
goto pass

:device
set "DXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\dxc.exe"
if not exist "%DXC%" (
  echo FAIL: Windows SDK dxc.exe required for DXR continuation test.
  goto fail
)
"%DXC%" -T lib_6_3 tests\lmxxf\lmxxf_state_object.hlsl -Fo "%OUT%\lmxxf_state_object.dxil" || goto fail
%CXX% tests\lmxxf\lmxxf_state_object.cpp /Fe"%OUT%\lmxxf_state_object.exe" /Fo"%OUT%\lmxxf_state_object.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_state_object.exe" "%OUT%\lmxxf_state_object.dxil" || goto fail
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
for %%M in ("" "--resize" "--queue-mismatch" "--rgb9e5" "--reject-formats" "--exposure" "--exposure-bad" "--ultrawide" "--subrect" "--temporal" "--temporal --subrect" "--temporal --ultrawide" "--temporal-guides") do (
  echo --- lmxxf_nr_gpu %%~M
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" %%~M || goto fail
)
rem Output-hash baselines: fixed pattern, 1920x1080, seed 1 (RX 9070 XT).
if defined LMXXF_TEST17_CHECKS (
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --test17-soft || goto fail
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --test17-identity || goto fail
)
rem Rebuilt main 9a721fc control, vendor pin 54e14de5; the previous DLL baseline predated
rem the current source. Completion-event candidate independently matched every control hash.
call :Hash d3e681a3fdee46d8 --output-hash || goto fail
call :Hash bb3b572fa319bd6d --auto-exposure || goto fail
call :Hash bb3b572fa319bd6d --auto-exposure --scale16 || goto fail
call :Hash 9372fe6db7976169 --r10g10b10a2 || goto fail
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
