@echo off
setlocal
cd /d "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\test-run\shader-compiler"
for %%I in ("%OUT%") do set "OUT=%%~fI"
call tests\_lib\msvc-env.cmd || exit /b 1
set "INCLUDE=%INCLUDE%;%CD%"
if not exist "%OUT%\fixture\game" mkdir "%OUT%\fixture\game"
cl /nologo /std:c++17 /EHsc /LD tests\lmxxf\lmxxf_old_compiler.cpp /Fe"%OUT%\fixture\game\d3dcompiler_47.dll" /Fo"%OUT%\old-compiler.obj" /link /export:D3DCompile=OldCompile || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /utf-8 tests\lmxxf\lmxxf_shader_compiler.cpp /Fe"%OUT%\lmxxf_shader_compiler.exe" /Fo"%OUT%\shader-compiler.obj" /link d3d12.lib dxgi.lib dxguid.lib || exit /b 1
rem A new fixture subdirectory isolates each cold/warm pair without deleting user caches.
set "FIXTURE=%OUT%\fixture\run-%RANDOM%-%RANDOM%"
mkdir "%FIXTURE%\game"
copy /Y "%OUT%\fixture\game\d3dcompiler_47.dll" "%FIXTURE%\game\d3dcompiler_47.dll" >nul || exit /b 1
"%OUT%\lmxxf_shader_compiler.exe" "%FIXTURE%" cold "%CD%\third_party\lmxxf\shaders" || exit /b 1
"%OUT%\lmxxf_shader_compiler.exe" "%FIXTURE%" warm "%CD%\third_party\lmxxf\shaders" || exit /b 1
exit /b 0
