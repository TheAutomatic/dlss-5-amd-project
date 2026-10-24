@echo off
rem Installer, uninstaller and packaging entrypoints. No GPU; isolated temp folders only.
rem Usage: tests\install\run.cmd [release]  (default: full regression; release: focused smoke cases)
rem   test_installer_exit.py   Setup/Uninstall launchers under Windows PowerShell 5.1
rem   test_module_packages.py  PACKAGE_RELEASE / installer / stage on synthetic module bundles
rem   amd_uninstall.ps1        uninstaller regression (Windows PowerShell 5.1)
setlocal EnableExtensions
cd /d "%~dp0..\.."
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
set "PS51=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if /i "%~1"=="release" goto release
if /i "%~1"=="artwork" goto artwork
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\xess-compat.ps1 || goto fail
"%AMD_TEST_PYTHON%" -m pip install --disable-pip-version-check -r tests\install\requirements.txt || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\package-signing.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\person-worker-package.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\plugin-paths.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\folder-picker-dpi.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\library-artwork.ps1 || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B -m unittest discover -s tests\install -p test_proxy_probe.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B -m unittest discover -s tests\install -p test_launcher_worker.py --failfast || goto fail
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\launcher-localization.ps1 || goto fail
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\launcher-state.ps1 || goto fail
"%AMD_TEST_PYTHON%" -B -m unittest discover -s tests\install -p test_launcher_package.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B -m unittest discover -s tests\install -p test_launcher_startup.py --failfast || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_lmxxf_small.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_module_packages.py --failfast || goto fail
"%PS51%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\install\amd_uninstall.ps1 || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_release_upload.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_release_notes.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_runtime_ci_proof.py || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_ci_workers.py || goto fail

rem Local build launcher tests run with the cached tooling suite (tests\sync).

echo install: PASS
exit /b 0

:fail
echo install: FAIL
exit /b 1

:release
rem Archive install/uninstall, overwrite/preferences, and explicit multi-backend selection.
rem The production packager separately checks signatures, all files and hashes on the actual ZIP.
"%AMD_TEST_PYTHON%" -B tests\install\test_module_packages.py ModulePackageTests.test_valid_actual_archive_can_install || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py InstallerExitTests.test_kept_ini_preserves_canonical_preferences_over_legacy || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py InstallerExitTests.test_explicit_active_backend_survives_install_update_and_reinstall || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py InstallerExitTests.test_launcher_update_reuses_game_models_after_package_cache_deleted || goto fail
"%AMD_TEST_PYTHON%" -B tests\install\test_installer_exit.py InstallerExitTests.test_custom_plugins_keep_configs_readme_and_explicit_loader_setting InstallerExitTests.test_uninstall_preserves_unowned_readme_without_install_record || goto fail
echo install release: PASS
exit /b 0

:artwork
"%PS51%" -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File tests\install\library-artwork.ps1 || goto fail
echo install artwork: PASS
exit /b 0
