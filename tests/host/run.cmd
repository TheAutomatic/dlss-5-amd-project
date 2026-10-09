@echo off
rem Host-contract tests. Every *.cpp in tests\host belongs to exactly one tier below.
rem Usage: tests\host\run.cmd [ci^|device^|all] [out-dir]
rem   ci     : CPU only, no GPU. nr_backend_selector, amd_submission_state, amd_graphics_{snapshot,
rem            tracker,restore,restore_dx12,invocation,native_hooks}, amd_runtime_host_load (+ its
rem            generated fixture PE), amd_retirement_diagnostics, and hip_load.ps1 (amd_hip_load +
rem            amd_hip_load_fixture in space/unicode dirs; never loads the machine HIP runtime).
rem   device : amd_graphics_d3 (hardware D3D12 adapter; prints SKIP without one).
rem Requires Python 3 (standard library only). Enters MSVC 14.44 itself when cl is missing.
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "TIER=%~1"
if not defined TIER set "TIER=ci"
set "OUT=%~2"
if not defined OUT set "OUT=exports\test-run\host"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1
rem Repo-rooted includes (#include "OptiScaler-..." / "third_party/..."). The repo root goes at the END of
rem INCLUDE, not in /I: /I is searched before the SDK/STL dirs, and on a case-insensitive disk
rem <version> would then resolve to the repo's VERSION file.
set "INCLUDE=%INCLUDE%;%REPO%"
set "INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
set "CXX=cl /nologo /std:c++20 /EHsc /W4"
if /i "%TIER%"=="ci" goto ci
if /i "%TIER%"=="all" goto ci
if /i "%TIER%"=="device" goto device
echo usage: tests\host\run.cmd [ci^|device^|all] [out-dir]
exit /b 2

:ci
%CXX% /utf-8 tests\host\recording_device_identity.cpp /Fe"%OUT%\recording_device_identity.exe" /Fo"%OUT%\recording_device_identity.obj" /link dxguid.lib || goto fail
"%OUT%\recording_device_identity.exe" || goto fail
%CXX% /utf-8 /I"%INC%" tests\host\amdxc64_hook_init.cpp /Fe"%OUT%\amdxc64_hook_init.exe" /Fo"%OUT%\amdxc64_hook_init.obj" /link "%DETOURS%" || goto fail
"%OUT%\amdxc64_hook_init.exe" || goto fail
%CXX% /utf-8 tests\host\plugin_path.cpp /Fe"%OUT%\plugin_path.exe" /Fo"%OUT%\plugin_path.obj" || goto fail
"%OUT%\plugin_path.exe" "%OUT%\plugin-path-fixture" || goto fail
%CXX% /O2 /utf-8 tests\host\descriptor_copy_range.cpp /Fe"%OUT%\descriptor_copy_range.exe" /Fo"%OUT%\descriptor_copy_range.obj" || goto fail
"%OUT%\descriptor_copy_range.exe" || goto fail
%CXX% /utf-8 tests\host\dx11_companion_resize.cpp /Fe"%OUT%\dx11_companion_resize.exe" /Fo"%OUT%\dx11_companion_resize.obj" /link d3d11.lib dxgi.lib user32.lib || goto fail
"%OUT%\dx11_companion_resize.exe" || goto fail
%CXX% /O2 /utf-8 /LD /DFIXTURE_ID=11 tests\host\streamline_plugin_fixture.cpp /Fe"%OUT%\sl_fixture_a.dll" /Fo"%OUT%\sl_fixture_a.obj" /link /IMPLIB:"%OUT%\sl_fixture_a.lib" || goto fail
%CXX% /O2 /utf-8 /LD /DFIXTURE_ID=22 tests\host\streamline_plugin_fixture.cpp /Fe"%OUT%\sl_fixture_b.dll" /Fo"%OUT%\sl_fixture_b.obj" /link /IMPLIB:"%OUT%\sl_fixture_b.lib" || goto fail
%CXX% /O2 /utf-8 /I"%INC%" tests\host\streamline_plugin_slots.cpp /Fe"%OUT%\streamline_plugin_slots.exe" /Fo"%OUT%\streamline_plugin_slots.obj" /link "%DETOURS%" || goto fail
"%OUT%\streamline_plugin_slots.exe" "%OUT%\sl_fixture_a.dll" "%OUT%\sl_fixture_b.dll" || goto fail
"%AMD_TEST_PYTHON%" -B tests\host\test_menu_localization.py || goto fail
call "%REPO%\tests\host\menu-localization.cmd" "%OUT%\menu" || goto fail
%CXX% /utf-8 tests\host\dx12_interop_desc.cpp /Fe"%OUT%\dx12_interop_desc.exe" /Fo"%OUT%\dx12_interop_desc.obj" || goto fail
"%OUT%\dx12_interop_desc.exe" || goto fail
%CXX% /utf-8 tests\host\vulkan_call_scope.cpp /Fe"%OUT%\vulkan_call_scope.exe" /Fo"%OUT%\vulkan_call_scope.obj" || goto fail
"%OUT%\vulkan_call_scope.exe" || goto fail
%CXX% /utf-8 tests\host\nr_sr_placement.cpp /Fe"%OUT%\nr_sr_placement.exe" /Fo"%OUT%\nr_sr_placement.obj" || goto fail
"%OUT%\nr_sr_placement.exe" || goto fail
%CXX% /utf-8 tests\host\nr_install_status.cpp /Fe"%OUT%\nr_install_status.exe" /Fo"%OUT%\nr_install_status.obj" || goto fail
"%OUT%\nr_install_status.exe" || goto fail
%CXX% /utf-8 tests\host\fg_resource_readiness.cpp /Fe"%OUT%\fg_resource_readiness.exe" /Fo"%OUT%\fg_resource_readiness.obj" || goto fail
"%OUT%\fg_resource_readiness.exe" || goto fail
%CXX% /utf-8 tests\host\nr_performance.cpp /Fe"%OUT%\nr_performance.exe" /Fo"%OUT%\nr_performance.obj" || goto fail
"%OUT%\nr_performance.exe" || goto fail
%CXX% /utf-8 tests\host\nr_status_display.cpp /Fe"%OUT%\nr_status_display.exe" /Fo"%OUT%\nr_status_display.obj" || goto fail
"%OUT%\nr_status_display.exe" || goto fail
%CXX% /utf-8 tests\host\upscaler_route_diagnostic.cpp /Fe"%OUT%\upscaler_route_diagnostic.exe" /Fo"%OUT%\upscaler_route_diagnostic.obj" || goto fail
"%OUT%\upscaler_route_diagnostic.exe" || goto fail
%CXX% /utf-8 tests\host\menu_window_layout.cpp /Fe"%OUT%\menu_window_layout.exe" /Fo"%OUT%\menu_window_layout.obj" || goto fail
"%OUT%\menu_window_layout.exe" || goto fail
%CXX% /utf-8 tests\host\nr_diagnostic_log.cpp /Fe"%OUT%\nr_diagnostic_log.exe" /Fo"%OUT%\nr_diagnostic_log.obj" || goto fail
"%OUT%\nr_diagnostic_log.exe" "%OUT%\diagnostic-log" || goto fail
%CXX% tests\host\nr_multiplier.cpp /Fe"%OUT%\nr_multiplier.exe" /Fo"%OUT%\nr_multiplier.obj" || goto fail
"%OUT%\nr_multiplier.exe" || goto fail
%CXX% /utf-8 /I"%INC%" tests\host\nr_activity_recording.cpp /Fe"%OUT%\nr_activity_recording.exe" /Fo"%OUT%\nr_activity_recording.obj" /link "%DETOURS%" dxguid.lib || goto fail
"%OUT%\nr_activity_recording.exe" || goto fail
%CXX% /utf-8 tests\host\amd_model_settings.cpp /Fe"%OUT%\amd_model_settings.exe" /Fo"%OUT%\amd_model_settings.obj" || goto fail
"%OUT%\amd_model_settings.exe" || goto fail
%CXX% tests\host\nr_backend_selector.cpp /Fe"%OUT%\nr_backend_selector.exe" /Fo"%OUT%\nr_backend_selector.obj" || goto fail
"%OUT%\nr_backend_selector.exe" || goto fail
%CXX% /utf-8 /I"%INC%" tests\host\nr_session_lifecycle.cpp /Fe"%OUT%\nr_session_lifecycle.exe" /Fo"%OUT%\nr_session_lifecycle.obj" /link "%DETOURS%" dxguid.lib || goto fail
"%OUT%\nr_session_lifecycle.exe" || goto fail
%CXX% tests\host\amd_submission_state.cpp /Fe"%OUT%\amd_submission_state.exe" /Fo"%OUT%\amd_submission_state.obj" || goto fail
"%OUT%\amd_submission_state.exe" || goto fail
for %%T in (amd_graphics_snapshot amd_graphics_tracker amd_graphics_restore amd_graphics_restore_dx12 amd_graphics_invocation) do (
  %CXX% /utf-8 tests\host\%%T.cpp /Fe"%OUT%\%%T.exe" /Fo"%OUT%\%%T.obj" || goto fail
  "%OUT%\%%T.exe" || goto fail
)
%CXX% /O2 /utf-8 /I"%INC%" tests\host\amd_graphics_native_hooks.cpp /Fe"%OUT%\amd_graphics_native_hooks.exe" /Fo"%OUT%\amd_graphics_native_hooks.obj" /link "%DETOURS%" || goto fail
"%OUT%\amd_graphics_native_hooks.exe" || goto fail
%CXX% /utf-8 /I"%INC%" tests\host\amd_runtime_host_load.cpp /Fe"%OUT%\amd_runtime_host_load.exe" /Fo"%OUT%\amd_runtime_host_load.obj" /link "%DETOURS%" || goto fail
"%AMD_TEST_PYTHON%" -B tests\host\amd_runtime_host_fixture.py "%OUT%\amd_runtime_bootstrap_fixture.dll" || goto fail
"%OUT%\amd_runtime_host_load.exe" "%OUT%\amd_runtime_bootstrap_fixture.dll" || goto fail
%CXX% /utf-8 tests\host\amd_retirement_diagnostics.cpp /Fe"%OUT%\amd_retirement_diagnostics.exe" /Fo"%OUT%\amd_retirement_diagnostics.obj" || goto fail
if exist "%OUT%\retirement" rmdir /s /q "%OUT%\retirement"
mkdir "%OUT%\retirement"
"%OUT%\amd_retirement_diagnostics.exe" "%OUT%\retirement" || goto fail
"%AMD_TEST_PYTHON%" -B -m unittest tests\host\test_retirement_stats.py || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%\tests\host\hip_load.ps1" "%OUT%\hip" || goto fail
"%AMD_TEST_PYTHON%" -B tests\host\test_config_priority.py || goto fail

call tests\host\person-inference.cmd || goto fail
call tests\host\person-worker.cmd || goto fail

echo host ci: PASS
if /i not "%TIER%"=="all" exit /b 0

:device
%CXX% /utf-8 tests\host\amd_graphics_d3.cpp /Fe"%OUT%\amd_graphics_d3.exe" /Fo"%OUT%\amd_graphics_d3.obj" /link d3d12.lib dxgi.lib || goto fail
"%OUT%\amd_graphics_d3.exe" || goto fail
echo host device: PASS
exit /b 0

:fail
echo host %TIER%: FAIL
exit /b 1
