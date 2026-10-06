@echo off
rem lmxxf upstream-sync tooling (tools\sync-lmxxf-upstream.ps1, tools\audit-lmxxf-enablements.py,
rem tools\lmxxf-sync\*). Temporary Git repos and fake modules: no network, no GPU. 1.5-3 minutes.
rem Internal full suite; use run.cmd [--force] to run and record validation.
setlocal EnableExtensions
cd /d "%~dp0..\.."
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
"%AMD_TEST_PYTHON%" -B tests\sync\test_git_attributes.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B tests\sync\test_module_compiler_provenance.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B tests\sync\test_upstream_sync.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_local_package.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\sync\test_suite_cache.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\sync\test_local_release_exit.py || goto fail
echo sync: PASS
exit /b 0

:fail
echo sync: FAIL
exit /b 1
