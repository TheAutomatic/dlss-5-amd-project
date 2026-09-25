@echo off
rem Build LmxxfNrRuntime.dll via MSVC (primary) or MinGW (fallback).
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"

rem Check if MSVC cl.exe is available in PATH
where cl.exe >nul 2>&1
if %errorlevel%==0 (
  echo Building LmxxfNrRuntime.dll with MSVC...
  cl /nologo /std:c++17 /O2 /LD /EHsc /utf-8 /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DLMXXF_NR_RUNTIME_EXPORTS ^
    /I "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime" ^
    /I "third_party\lmxxf\src" ^
    /I "third_party\lmxxf\Development\HIP" ^
    "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime\LmxxfNrRuntime.cpp" ^
    /Fe:"%OUT%\LmxxfNrRuntime.dll" ^
    /Fo:"%OUT%\LmxxfNrRuntime.obj" ^
    d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib user32.lib
  if not %errorlevel%==0 (
    echo FAIL: MSVC compilation of LmxxfNrRuntime.dll failed
    exit /b 1
  )
  goto verify
)

rem MinGW fallback
if not defined LMXXF_GXX set "LMXXF_GXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%LMXXF_GXX%" (
  echo FAIL: Neither MSVC cl.exe nor MinGW g++ found at %LMXXF_GXX%
  echo Set LMXXF_GXX to x86_64-w64-mingw32-g++ or run from an MSVC developer prompt.
  exit /b 1
)
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
"%LMXXF_GXX%" -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 -DLMXXF_NR_RUNTIME_EXPORTS -I "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime" -I "third_party\lmxxf\src" -I "third_party\lmxxf\Development\HIP" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime\LmxxfNrRuntime.cpp" -o "%OUT%\LmxxfNrRuntime.dll" -Wl,--out-implib,"%OUT%\LmxxfNrRuntime.dll.a" -ld3d12 -ldxgi -ld3dcompiler -ldxguid

:verify
rem The exit code must be checked separately from "is the DLL there": a failed compile leaves a
rem stale DLL in place and used to sail through to BUILD_OK, so tests ran against yesterday's
rem binary while the log said the build was fine.
if not %errorlevel%==0 (
  echo FAIL: compilation of LmxxfNrRuntime.dll failed
  exit /b 1
)
if not exist "%OUT%\LmxxfNrRuntime.dll" (
  echo FAIL: LmxxfNrRuntime.dll not produced
  exit /b 1
)
echo BUILD_OK %OUT%\LmxxfNrRuntime.dll
exit /b 0
