@echo off
rem Requires an x64 MSVC developer environment and Python 3 (standard library only).
setlocal
cd /d "%~dp0.."
set "AMD_TEST_OUT=%~1"
if not defined AMD_TEST_OUT set "AMD_TEST_OUT=exports\amd-host-tests"
for %%I in ("%AMD_TEST_OUT%") do set "AMD_TEST_OUT=%%~fI"
if not exist "%AMD_TEST_OUT%" mkdir "%AMD_TEST_OUT%"
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
cl /nologo /std:c++20 /EHsc /W4 tests\amd_submission_state.cpp /Fe"%AMD_TEST_OUT%\amd_submission_state.exe" /Fo"%AMD_TEST_OUT%\amd_submission_state.obj"
if errorlevel 1 exit /b 1
"%AMD_TEST_OUT%\amd_submission_state.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_snapshot.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_snapshot.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_snapshot.obj"
if errorlevel 1 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_snapshot.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /IOptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include tests\amd_runtime_host_load.cpp /Fe"%AMD_TEST_OUT%\amd_runtime_host_load.exe" /Fo"%AMD_TEST_OUT%\amd_runtime_host_load.obj" /link OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib
if errorlevel 1 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture.dll"
if errorlevel 1 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture.dll"
exit /b %errorlevel%
