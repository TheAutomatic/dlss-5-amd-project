@echo off
setlocal
cd /d "%~dp0..\.."
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\benchmark\test-runner.ps1
exit /b %errorlevel%
