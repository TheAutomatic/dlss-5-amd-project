@echo off
rem MinGW (MSYS2 ucrt64) build of LmxxfNrRuntime.dll. Does not use MSVC.
setlocal
cd /d "%~dp0.."
if not defined LMXXF_GXX set "LMXXF_GXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%LMXXF_GXX%" (
  echo FAIL: MinGW g++ not found at %LMXXF_GXX%
  echo Set LMXXF_GXX to x86_64-w64-mingw32-g++.
  exit /b 1
)
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"
"%LMXXF_GXX%" -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 -DLMXXF_NR_RUNTIME_EXPORTS -I "third_party\lmxxf\include" "third_party\lmxxf\runtime\LmxxfNrRuntime.cpp" -o "%OUT%\LmxxfNrRuntime.dll" -Wl,--out-implib,"%OUT%\LmxxfNrRuntime.dll.a"
if not %errorlevel%==0 exit /b 1
if not exist "%OUT%\LmxxfNrRuntime.dll" (
  echo FAIL: LmxxfNrRuntime.dll not produced
  exit /b 1
)
echo BUILD_OK %OUT%\LmxxfNrRuntime.dll
exit /b 0
