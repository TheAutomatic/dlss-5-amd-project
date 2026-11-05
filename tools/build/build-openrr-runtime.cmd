@echo off
setlocal
cd /d "%~dp0..\.."
call tests\_lib\msvc-env.cmd || exit /b 1
python tools\build\build-openrr-runtime.py %*
exit /b %errorlevel%
