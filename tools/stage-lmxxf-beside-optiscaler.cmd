@echo off
rem Copy LmxxfNrRuntime.dll + lmxxf-modules-68dc099 beside OptiScaler (release-local or -Dest).
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
if not exist "exports\lmxxf-modules-68dc099\SHA256SUMS" (
  echo FAIL: missing exports\lmxxf-modules-68dc099
  exit /b 1
)
robocopy "exports\lmxxf-modules-68dc099" "%DEST%\lmxxf-modules" /E /NFL /NDL /NJH /NJS /nc /ns /np >nul
if errorlevel 8 exit /b 1
echo staged LmxxfNrRuntime.dll + lmxxf-modules -^> %DEST%
echo NOTE: weights via LMXXF_WEIGHTS_DIR=native-game-tiled-assets ^(not HIP/^)
exit /b 0