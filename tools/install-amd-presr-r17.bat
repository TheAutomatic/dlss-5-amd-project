@echo off
rem Wrapper for install-amd-presr-r17.ps1
rem Usage: install-amd-presr-r17.bat "C:\Path\To\Game" [proxy.dll]
setlocal
set "GAME=%~1"
set "PROXY=%~2"
if "%GAME%"=="" (
  echo Usage: %~nx0 "C:\Path\To\Game" [dxgi.dll^|winmm.dll^|...]
  exit /b 1
)
if "%PROXY%"=="" set "PROXY=dxgi.dll"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-amd-presr-r17.ps1" -GameDir "%GAME%" -Proxy %PROXY%
exit /b %ERRORLEVEL%
