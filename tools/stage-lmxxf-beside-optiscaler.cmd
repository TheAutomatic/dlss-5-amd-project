@echo off
rem Validate and stage runtime + dual-architecture modules + shaders beside OptiScaler.
setlocal
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stage-lmxxf-beside-optiscaler.ps1" -Dest "%~1"
exit /b %ERRORLEVEL%
