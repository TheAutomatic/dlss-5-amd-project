@echo off
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-color-probe"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_color_probe.cpp /Fe"%OUT%\lmxxf_color_probe.exe" /Fo"%OUT%\lmxxf_color_probe.obj" /link d3d12.lib dxgi.lib dxguid.lib
if errorlevel 1 exit /b 1
"%OUT%\lmxxf_color_probe.exe" %2
exit /b %errorlevel%
