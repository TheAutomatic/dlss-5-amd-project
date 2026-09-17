@echo off
rem Requires an x64 MSVC developer environment. Does not load the machine HIP runtime.
setlocal EnableExtensions
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-hip-runtime-load.ps1" %*
exit /b %errorlevel%
