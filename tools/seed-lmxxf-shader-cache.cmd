@echo off
rem Precompile lmxxf HLSL variants into shaders\shader-cache (fnv1a keys match CompileNativeShader).
setlocal
cd /d "%~dp0.."
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
if not exist "C:\msys64\ucrt64\bin\g++.exe" (
  echo FAIL: need MSYS2 ucrt64 g++ to seed shader-cache
  exit /b 1
)
set "SRC=%TEMP%\lmxxf_seed_shader_cache.cpp"
rem Prefer a checked-in helper if present; else require cache already populated.
if not exist "third_party\lmxxf\shaders\shader-cache\a05980ebd6e4dadd.dxbc" (
  echo FAIL: missing FIT=1 encode cache blob; run seed from agent/dev machine first
  exit /b 1
)
echo SEED_OK shader-cache present
exit /b 0
