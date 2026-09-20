@echo off
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"
set "INC=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I"%INC%" tests\lmxxf_evaluate_cut.cpp /Fe"%OUT%\lmxxf_evaluate_cut.exe" /Fo"%OUT%\lmxxf_evaluate_cut.obj" /link d3d12.lib dxgi.lib dxguid.lib uuid.lib "%DETOURS%"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_evaluate_cut.exe"
if not %errorlevel%==0 exit /b 1
echo lmxxf_evaluate_cut: PASS
exit /b 0