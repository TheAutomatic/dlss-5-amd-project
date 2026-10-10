@echo off
rem Production shader helpers on CPU/WARP (no GPU). Every *.cpp in tests\shader runs here.
rem Usage: tests\shader\run.cmd [out-dir]
rem Needs the xess and FidelityFX submodules (git submodule update --init --recursive).
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "SHADER_TEST_OUT=%~1"
if not defined SHADER_TEST_OUT set "SHADER_TEST_OUT=exports\test-run\shader"
for %%I in ("%SHADER_TEST_OUT%") do set "SHADER_TEST_OUT=%%~fI"
if not exist "%SHADER_TEST_OUT%" mkdir "%SHADER_TEST_OUT%"
call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1
rem Repo-rooted includes (#include "OptiScaler-..." / "third_party/..."). The repo root goes at the END of
rem INCLUDE, not in /I: /I is searched before the SDK/STL dirs, and on a case-insensitive disk
rem <version> would then resolve to the repo's VERSION file.
set "INCLUDE=%INCLUDE%;%REPO%"
set "SHADER_PROJECT=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler"
set "SHADER_EXTERNAL=OptiScaler-DLSSNR-PreSR-Multipass-main\external"
if not exist "%SHADER_TEST_OUT%\person-capture-worker" mkdir "%SHADER_TEST_OUT%\person-capture-worker"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX tests\host\person_worker_fixture.cpp /Fo"%SHADER_TEST_OUT%\person-capture-worker\fixture.obj" /Fe"%SHADER_TEST_OUT%\person-capture-worker\person-worker.exe" || goto fail
call :BuildAndRun shader_dx12_srv
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun shader_dx11_ownership
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_person_partition
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_output_effects
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_residual_shaping
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_stabilizer
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_stabilizer_lifecycle
if not "%errorlevel%"=="0" goto fail
call :BuildAndRun nr_post_sr
if not "%errorlevel%"=="0" goto fail
echo shader: PASS
exit /b 0

:fail
echo shader: FAIL
exit /b 1

:BuildAndRun
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /MD /O2 /Gy ^
 /I"%SHADER_PROJECT%" /I"%SHADER_PROJECT%\include" ^
 /I"%SHADER_EXTERNAL%\vulkan\include" /I"%SHADER_EXTERNAL%\nvngx_dlss_sdk" ^
 /I"%SHADER_EXTERNAL%\xess\inc\xess" /I"%SHADER_EXTERNAL%\xess\inc\xell" ^
 /I"%SHADER_EXTERNAL%\xess\inc\xess_fg" /I"%SHADER_EXTERNAL%\FidelityFX-SDK\ffx-api\include\ffx_api" ^
 /I"%SHADER_EXTERNAL%\simpleini" /I"%SHADER_EXTERNAL%\unordered_dense\include" ^
 /I"%SHADER_EXTERNAL%\spdlog\include" /I"%SHADER_EXTERNAL%\freetype" ^
 /I"%SHADER_EXTERNAL%\streamline" /I"%SHADER_EXTERNAL%\streamline1" ^
 /I"%SHADER_EXTERNAL%\nvapi" /I"%SHADER_EXTERNAL%\nlohmann" ^
 /I"%SHADER_EXTERNAL%\fakenvapi" /I"%SHADER_EXTERNAL%\magic_enum\include\magic_enum" ^
 /I"%SHADER_EXTERNAL%\AntiLag2-SDK" /I"%SHADER_EXTERNAL%\latencyflex" ^
 "tests\shader\%~1.cpp" /Fe"%SHADER_TEST_OUT%\%~1.exe" /Fo"%SHADER_TEST_OUT%\%~1.obj" ^
 /link /OPT:REF d3d11.lib d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib user32.lib "%SHADER_PROJECT%\library\detours\detours.lib"
if not "%errorlevel%"=="0" exit /b 1
"%SHADER_TEST_OUT%\%~1.exe"
exit /b %errorlevel%
