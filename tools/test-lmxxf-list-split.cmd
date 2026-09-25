@echo off
rem P1 no-NR split Execute equivalence. Requires x64 MSVC cl and a D3D12 device.
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_list_split.cpp /Fe"%OUT%\lmxxf_list_split.exe" /Fo"%OUT%\lmxxf_list_split.obj" /link d3d12.lib dxgi.lib dxguid.lib uuid.lib
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_list_split.exe"
if not %errorlevel%==0 exit /b 1
echo lmxxf_list_split: PASS
exit /b 0
