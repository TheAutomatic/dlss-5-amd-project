@echo off
rem Copy LmxxfNrRuntime.dll + lmxxf-modules + shaders beside OptiScaler (release-local or -Dest).
setlocal
cd /d "%~dp0.."
set "DEST=%~1"
if not defined DEST set "DEST=exports\release-local"
if not exist "%DEST%" mkdir "%DEST%"
if not exist "exports\lmxxf-runtime\LmxxfNrRuntime.dll" (
  echo Building runtime...
  call tools\build-lmxxf-runtime.cmd
  if errorlevel 1 exit /b 1
)
copy /Y "exports\lmxxf-runtime\LmxxfNrRuntime.dll" "%DEST%\LmxxfNrRuntime.dll" >nul
if errorlevel 1 exit /b 1
if not exist "third_party\lmxxf\modules\SHA256SUMS" (
  echo FAIL: missing third_party\lmxxf\modules
  exit /b 1
)
rem Purge stale flat modules and old manifests before staging dual-arch tree
if exist "%DEST%\lmxxf-modules\*.hsaco" del /f /q "%DEST%\lmxxf-modules\*.hsaco"
if exist "%DEST%\lmxxf-modules\modules.json" del /f /q "%DEST%\lmxxf-modules\modules.json"
robocopy "third_party\lmxxf\modules" "%DEST%\lmxxf-modules" /E /NFL /NDL /NJH /NJS /nc /ns /np >nul
if errorlevel 8 exit /b 1
if exist "%DEST%\lmxxf-modules\*.hsaco" (
  echo FAIL: stale flat modules remain in %DEST%\lmxxf-modules
  exit /b 1
)
if not exist "%DEST%\lmxxf-modules\gfx1200\SHA256SUMS" (
  echo FAIL: missing gfx1200 in %DEST%\lmxxf-modules
  exit /b 1
)
if not exist "%DEST%\lmxxf-modules\gfx1201\SHA256SUMS" (
  echo FAIL: missing gfx1201 in %DEST%\lmxxf-modules
  exit /b 1
)
if not exist "third_party\lmxxf\shaders\native_codec_encode.hlsl" (
  echo FAIL: missing third_party\lmxxf\shaders
  exit /b 1
)
robocopy "third_party\lmxxf\shaders" "%DEST%\shaders" /E /NFL /NDL /NJH /NJS /nc /ns /np >nul
if errorlevel 8 exit /b 1
echo staged LmxxfNrRuntime.dll + lmxxf-modules + shaders -^> %DEST%
echo NOTE: weights via LMXXF_WEIGHTS_DIR=native-game-tiled-assets ^(not HIP/^)
exit /b 0
