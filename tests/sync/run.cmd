@echo off
setlocal
cd /d "%~dp0..\.."
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
"%AMD_TEST_PYTHON%" -B tools\lmxxf-sync\test-cache.py run %*
exit /b %errorlevel%
