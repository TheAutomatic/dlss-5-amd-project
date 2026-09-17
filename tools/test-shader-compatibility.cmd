@echo off
rem Requires an x64 MSVC developer environment. DX11 uses WARP; SRV capture is CPU-only.
setlocal
cd /d "%~dp0.."
set "SHADER_TEST_OUT=%~1"
if not defined SHADER_TEST_OUT set "SHADER_TEST_OUT=exports\shader-compatibility"
for %%I in ("%SHADER_TEST_OUT%") do set "SHADER_TEST_OUT=%%~fI"
if not exist "%SHADER_TEST_OUT%" mkdir "%SHADER_TEST_OUT%"
set "SHADER_PROJECT=OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler"
set "SHADER_EXTERNAL=OptiScaler-DLSSNR-PreSR-Multipass-main\external"
call :BuildAndRun shader_dx12_srv
if not "%errorlevel%"=="0" exit /b 1
call :BuildAndRun shader_dx11_ownership
if not "%errorlevel%"=="0" exit /b 1
exit /b 0

:BuildAndRun
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /MD /O2 /Gy ^
 /I"%SHADER_PROJECT%" /I"%SHADER_PROJECT%\include" ^
 /I"%SHADER_EXTERNAL%\vulkan\include" /I"%SHADER_EXTERNAL%\nvngx_dlss_sdk" ^
 /I"%SHADER_EXTERNAL%\xess\inc\xess" /I"%SHADER_EXTERNAL%\xess\inc\xell" ^
 /I"%SHADER_EXTERNAL%\xess\inc\xess_fg" /I"%SHADER_EXTERNAL%\FidelityFX-SDK\ffx-api\include\ffx_api" ^
 /I"%SHADER_EXTERNAL%\simpleini" /I"%SHADER_EXTERNAL%\unordered_dense\include" ^
 /I"%SHADER_EXTERNAL%\spdlog\include" /I"%SHADER_EXTERNAL%\freetype" ^
 /I"%SHADER_EXTERNAL%\streamline" /I"%SHADER_EXTERNAL%\streamline1" ^
 /I"%SHADER_EXTERNAL%\nvapi" /I"%SHADER_EXTERNAL%\nlohmann" ^
 /I"%SHADER_EXTERNAL%\fakenvapi" /I"%SHADER_EXTERNAL%\magic_enum\include\magic_enum" ^
 /I"%SHADER_EXTERNAL%\AntiLag2-SDK" /I"%SHADER_EXTERNAL%\latencyflex" ^
 "tests\%~1.cpp" /Fe"%SHADER_TEST_OUT%\%~1.exe" /Fo"%SHADER_TEST_OUT%\%~1.obj" ^
 /link /OPT:REF d3d11.lib d3d12.lib d3dcompiler.lib
if not "%errorlevel%"=="0" exit /b 1
"%SHADER_TEST_OUT%\%~1.exe"
exit /b %errorlevel%
