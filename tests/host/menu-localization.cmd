@echo off
setlocal
cd /d "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\test-run\host\menu"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
call tests\_lib\msvc-env.cmd || exit /b 1
set "INCLUDE=%INCLUDE%;%CD%"
set "IMGUI=%CD%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include\imgui"
set "INCLUDE=%INCLUDE%;%IMGUI%\..\..;%CD%\OptiScaler-DLSSNR-PreSR-Multipass-main\external\freetype"
cl /nologo /std:c++20 /EHsc /O1 /utf-8 /I"%IMGUI%\.." tests\host\menu_localization.cpp "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\imgui_impl_dx11.cpp" "%IMGUI%\misc\freetype\imgui_freetype.cpp" /Fo"%OUT%\\" /Fe"%OUT%\menu_localization.exe" /link "OptiScaler-DLSSNR-PreSR-Multipass-main\external\freetype\freetype.lib" d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib imm32.lib dwmapi.lib || exit /b 1
"%OUT%\menu_localization.exe" "%OUT%" || exit /b 1
