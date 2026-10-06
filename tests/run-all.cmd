@echo off
rem Single entry point for every test area. Each area folder owns exactly one run.cmd.
rem Usage: tests\run-all.cmd [--tier ci^|device^|gpu^|all] [--out exports\test-run] [--skip-sync] [--keep-going]
rem   --tier  ci      what the GitHub runner can do, no GPU: host ci, shader, lmxxf abi + warp,
rem                   install, sync. Default.
rem           device  hardware D3D12 adapter: host device (amd_graphics_d3), lmxxf device.
rem           gpu     AMD GPU + LMXXF_ASSETS (weights folder): lmxxf gpu.
rem           all     ci + device + gpu.
rem           Several tiers may be combined: --tier ci,device
rem   sync reuses a matching successful tooling run in the current UTC week; it never reuses runtime tests.
rem   --skip-sync   skip tests\sync for inner loops; reported as SKIP and disables release proof.
rem   --keep-going  run every suite and fail at the end instead of stopping at the first failure.
rem The runtime is built once into <out>\runtime unless LMXXF_TEST_RUNTIME already names a DLL.
setlocal EnableExtensions
cd /d "%~dp0.."
set "REPO=%CD%"
set "RUN_CI="
set "RUN_DEVICE="
set "RUN_GPU="
set "ANY_TIER="
set "OUT=exports\test-run"
set "SKIP_SYNC="
set "KEEP_GOING="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--tier" (
  shift
  goto tiers
)
if /i "%~1"=="--out" (
  if "%~2"=="" goto usage
  set "OUT=%~2"
  shift
  shift
  goto parse
)
if /i "%~1"=="--skip-sync" (
  set "SKIP_SYNC=1"
  shift
  goto parse
)
if /i "%~1"=="--keep-going" (
  set "KEEP_GOING=1"
  shift
  goto parse
)
goto usage

:tiers
rem cmd splits "ci,device" into two arguments, so take every argument up to the next option.
if "%~1"=="" goto parsed
set "ARG=%~1"
if "%ARG:~0,2%"=="--" goto parse
for %%T in (%ARG%) do call :AddTier %%T || goto usage
shift
goto tiers

:AddTier
if /i "%~1"=="ci" (set "RUN_CI=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="device" (set "RUN_DEVICE=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="gpu" (set "RUN_GPU=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="all" (set "RUN_CI=1" & set "RUN_DEVICE=1" & set "RUN_GPU=1" & set "ANY_TIER=1" & exit /b 0)
echo Unknown tier: %~1
exit /b 1

:usage
echo usage: tests\run-all.cmd [--tier ci^|device^|gpu^|all] [--out exports\test-run] [--skip-sync] [--keep-going]
exit /b 2

:parsed
if not defined ANY_TIER set "RUN_CI=1"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
set "SUMMARY=%OUT%\summary.txt"
set "RUNTIME_HASH="
rem Invalidate proof from an earlier run before any suite can fail.
if exist "%OUT%\runtime-ci.sha256" del "%OUT%\runtime-ci.sha256"
if exist "%OUT%\runtime-ci.sha256" exit /b 1
type nul > "%SUMMARY%"
set "FAILED="

call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1

if not defined RUN_CI if not defined RUN_GPU goto afterRuntime
if defined LMXXF_TEST_RUNTIME if exist "%LMXXF_TEST_RUNTIME%" (
  for %%I in ("%LMXXF_TEST_RUNTIME%") do set "LMXXF_TEST_RUNTIME=%%~fI"
  call :Record PASS "runtime (prebuilt LMXXF_TEST_RUNTIME)"
  goto afterRuntime
)
echo === build: LmxxfNrRuntime.dll ===
call "%REPO%\tools\build\build-lmxxf-runtime.cmd" "%OUT%\runtime"
call :Result "runtime build" || goto done
set "LMXXF_TEST_RUNTIME=%OUT%\runtime\LmxxfNrRuntime.dll"
:afterRuntime
if not defined RUN_CI goto afterRuntimeHash
for /f %%H in ('powershell -NoProfile -Command "$f=[IO.File]::OpenRead($env:LMXXF_TEST_RUNTIME); $s=[Security.Cryptography.SHA256]::Create(); try {[BitConverter]::ToString($s.ComputeHash($f)).Replace('-','')} finally {$f.Dispose(); $s.Dispose()}"') do set "RUNTIME_HASH=%%H"
if not defined RUNTIME_HASH exit /b 1
:afterRuntimeHash

if not defined RUN_CI goto afterMochizuki
if not exist exports\mochizuki-runtime\build-manifest.json (
  call tools\build\build-mochizuki-runtime.cmd || exit /b 1
)
python -X utf8 tools\build\mochizuki-manifest.py exports\mochizuki-runtime || exit /b 1
call tests\mochizuki\run.cmd abi
call :Result "mochizuki abi" || goto done
:afterMochizuki

if not defined RUN_CI goto afterCi
echo === ci: host ===
call "%REPO%\tests\host\run.cmd" ci "%OUT%\host"
call :Result "host ci" || goto done
echo === ci: shader ===
call "%REPO%\tests\shader\run.cmd" "%OUT%\shader"
call :Result "shader" || goto done
echo === ci: lmxxf abi ===
call "%REPO%\tests\lmxxf\run.cmd" abi "%OUT%\lmxxf"
call :Result "lmxxf abi" || goto done
echo === ci: lmxxf warp ===
call "%REPO%\tests\lmxxf\run.cmd" warp "%OUT%\lmxxf"
call :Result "lmxxf warp" || goto done
echo === ci: install ===
call "%REPO%\tests\install\run.cmd"
call :Result "install" || goto done
if defined SKIP_SYNC (
  call :Record SKIP "sync (--skip-sync)"
  goto afterCi
)
echo === ci: sync ===
call "%REPO%\tests\sync\run.cmd"
call :Result "sync" || goto done
:afterCi

if not defined RUN_DEVICE goto afterDevice
echo === device: host ===
call "%REPO%\tests\host\run.cmd" device "%OUT%\host"
call :Result "host device" || goto done
echo === device: lmxxf ===
call "%REPO%\tests\lmxxf\run.cmd" device "%OUT%\lmxxf"
call :Result "lmxxf device" || goto done
:afterDevice

if not defined RUN_GPU goto afterGpu
call tests\mochizuki\run.cmd gpu
call :Result "mochizuki gpu" || goto done
echo === gpu: lmxxf ===
call "%REPO%\tests\lmxxf\run.cmd" gpu "%OUT%\lmxxf"
call :Result "lmxxf gpu" || goto done
:afterGpu

:done
echo.
echo ===== summary =====
type "%SUMMARY%"
if defined FAILED exit /b 1
if not defined RUNTIME_HASH exit /b 0
set "FINAL_RUNTIME_HASH="
for /f %%H in ('powershell -NoProfile -Command "$f=[IO.File]::OpenRead($env:LMXXF_TEST_RUNTIME); $s=[Security.Cryptography.SHA256]::Create(); try {[BitConverter]::ToString($s.ComputeHash($f)).Replace('-','')} finally {$f.Dispose(); $s.Dispose()}"') do set "FINAL_RUNTIME_HASH=%%H"
if not "%RUNTIME_HASH%"=="%FINAL_RUNTIME_HASH%" (
  echo FAIL: tested runtime changed during regression suites
  exit /b 1
)
if defined SKIP_SYNC exit /b 0
>"%OUT%\runtime-ci.sha256" echo %RUNTIME_HASH%
exit /b 0

rem :Result <suite> - records the caller's errorlevel. Fails (exit 1) only when the run must stop.
:Result
if "%errorlevel%"=="0" (
  call :Record PASS "%~1"
  exit /b 0
)
call :Record FAIL "%~1"
set "FAILED=1"
if defined KEEP_GOING exit /b 0
exit /b 1

:Record
echo %~1  %~2
>>"%SUMMARY%" echo %~1  %~2
exit /b 0
