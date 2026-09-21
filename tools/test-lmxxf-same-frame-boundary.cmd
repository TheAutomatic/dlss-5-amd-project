@echo off
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-same-frame"
if not exist "%OUT%" mkdir "%OUT%"
set "INC=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I"%INC%" tests\lmxxf_same_frame_boundary.cpp /Fe"%OUT%\lmxxf_same_frame_boundary.exe" /Fo"%OUT%\lmxxf_same_frame_boundary.obj" /link d3d12.lib dxgi.lib dxguid.lib uuid.lib d3dcompiler.lib "%DETOURS%"
if errorlevel 1 exit /b 1
"%OUT%\lmxxf_same_frame_boundary.exe"
exit /b %errorlevel%
