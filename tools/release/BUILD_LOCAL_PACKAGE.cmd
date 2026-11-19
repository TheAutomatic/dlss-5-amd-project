@echo off
setlocal
rem MSVC/MSBuild emit UTF-8 zh-CN text; show it in a UTF-8 console instead of GBK mojibake.
chcp 65001 >nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0BUILD_LOCAL_PACKAGE.ps1" %*
set "BUILD_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %BUILD_RESULT%
