@echo off
rem Installer, uninstaller and packaging entrypoints. No GPU; isolated temp folders only.
rem Usage: tests\install\run.cmd
rem   test_installer_exit.py   Setup/Uninstall launchers under Windows PowerShell 5.1
rem   test_module_packages.py  PACKAGE_RELEASE / installer / stage on synthetic module bundles
rem   amd_uninstall.ps1        uninstaller regression (Windows PowerShell 5.1)
setlocal EnableExtensions
cd /d "%~dp0..\.."
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
set "PS51=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\package-signing.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\plugin-paths.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\folder-picker-dpi.ps1 || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_module_packages.py --failfast || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\amd_uninstall.ps1 || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_release_upload.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_runtime_ci_proof.py || goto fail

rem Local build launcher tests run with the cached tooling suite (tests\sync).

echo install: PASS
exit /b 0

:fail
echo install: FAIL
exit /b 1
