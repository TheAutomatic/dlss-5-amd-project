@echo off
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"
set "INC=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I"%INC%" tests\lmxxf_list1_wrap.cpp /Fe"%OUT%\lmxxf_list1_wrap.exe" /Fo"%OUT%\lmxxf_list1_wrap.obj" /link d3d12.lib dxgi.lib dxguid.lib uuid.lib "%DETOURS%"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_list1_wrap.exe"
if not %errorlevel%==0 exit /b 1
echo lmxxf_list1_wrap: PASS
exit /b 0