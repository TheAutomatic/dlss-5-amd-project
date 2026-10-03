@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0BUILD_LOCAL_PACKAGE.ps1" %*
set "BUILD_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %BUILD_RESULT%
