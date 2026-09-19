@echo off
rem Builds the MinGW runtime then proves the C ABI from an MSVC loader.
rem Requires x64 MSVC cl in PATH (same env as test-amd-host-contracts.cmd).
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
call "%~dp0build-lmxxf-runtime.cmd" "%OUT%"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_nr_abi.cpp /I third_party\lmxxf\include /Fe"%OUT%\lmxxf_nr_abi.exe" /Fo"%OUT%\lmxxf_nr_abi.obj"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_nr_abi.exe" "%OUT%\LmxxfNrRuntime.dll"
if not %errorlevel%==0 exit /b 1
echo lmxxf_nr_abi: PASS
exit /b 0
