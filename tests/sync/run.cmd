@echo off
rem lmxxf upstream-sync tooling (tools\sync-lmxxf-upstream.ps1, tools\audit-lmxxf-enablements.py,
rem tools\lmxxf-sync\*). Temporary Git repos and fake modules: no network, no GPU. 1.5-3 minutes.
rem Usage: tests\sync\run.cmd
setlocal EnableExtensions
cd /d "%~dp0..\.."
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
"%AMD_TEST_PYTHON%" -B tests\sync\test_upstream_sync.py --failfast || goto fail
echo sync: PASS
exit /b 0

:fail
echo sync: FAIL
exit /b 1
