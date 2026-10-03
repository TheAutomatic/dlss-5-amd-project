@echo off
setlocal
cd /d "%~dp0..\.."
call tests\_lib\msvc-env.cmd || exit /b 1
if not exist exports\mochizuki-tests mkdir exports\mochizuki-tests
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/runtime.cpp /Foexports/mochizuki-tests/runtime.obj /Feexports/mochizuki-tests/runtime.exe /link d3d12.lib dxgi.lib || exit /b 1
if /i "%~1"=="startup" goto startup
if /i "%~1"=="pass-switch" goto passswitch
if /i "%~1"=="gpu" (
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime"
) else (
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
)
if errorlevel 1 exit /b 1
python -X utf8 -c "from pathlib import Path; import hashlib; p=Path('exports/mochizuki-runtime'); (p/'abi-ci.sha256').write_text(hashlib.sha256((p/'MochizukiNrRuntime.dll').read_bytes()).hexdigest()+'\n')"
exit /b %errorlevel%

:passswitch
set "MZ_TEST_UPGRADE_REFUSALS="
set "MZ_TEST_UPGRADE_OOM_ONCE="
set "MZ_TEST_FRAME_BUDGET_ONCE="
set "MOCHI_PASS_MODE=normal"
if not "%~2"=="" set "MOCHI_PASS_MODE=%~2"
if /i "%~2"=="budget" set "MZ_TEST_UPGRADE_REFUSALS=1"
if /i "%~2"=="oom" set "MZ_TEST_UPGRADE_OOM_ONCE=1"
if /i "%~2"=="frame-budget" set "MZ_TEST_FRAME_BUDGET_ONCE=1"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --pass-switch "%MOCHI_PASS_MODE%"
exit /b %errorlevel%

:startup
set "ASSETS=%CD%\exports\mochizuki-runtime"
if not "%~2"=="" set "ASSETS=%~f2"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%ASSETS%" --startup "%~3"
exit /b %errorlevel%
