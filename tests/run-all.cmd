@echo off
rem Single entry point for every test area. Each area folder owns exactly one run.cmd.
rem Usage: tests\run-all.cmd [--tier release^|ci^|device^|gpu^|all] [--out exports\test-run] [--skip-sync] [--keep-going]
setlocal EnableExtensions
cd /d "%~dp0.."
set "REPO=%CD%"
set "RUN_CI="
set "FULL_CI="
set "RUN_DEVICE="
set "RUN_GPU="
set "ANY_TIER="
set "OUT=exports\test-run"
set "SKIP_SYNC="
set "KEEP_GOING="
set "RELEASE_CHECKS="

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
if "%~1"=="" goto parsed
set "ARG=%~1"
if "%ARG:~0,2%"=="--" goto parse
for %%T in (%ARG%) do call :AddTier %%T || goto usage
shift
goto tiers

:AddTier
if /i "%~1"=="release" (set "RUN_CI=1" & set "RELEASE_CHECKS=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="ci" (set "RUN_CI=1" & set "FULL_CI=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="device" (set "RUN_DEVICE=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="gpu" (set "RUN_GPU=1" & set "ANY_TIER=1" & exit /b 0)
if /i "%~1"=="all" (set "RUN_CI=1" & set "FULL_CI=1" & set "RUN_DEVICE=1" & set "RUN_GPU=1" & set "ANY_TIER=1" & exit /b 0)
echo Unknown tier: %~1
exit /b 1

:usage
echo usage: tests\run-all.cmd [--tier release^|ci^|device^|gpu^|all] [--out exports\test-run] [--skip-sync] [--keep-going]
exit /b 2

:parsed
if not defined ANY_TIER set "RUN_CI=1"
if defined FULL_CI set "RELEASE_CHECKS="
set "HOST_TIER=ci"
set "INSTALL_TIER="
if defined RELEASE_CHECKS (
  set "HOST_TIER=release"
  set "INSTALL_TIER=release"
)
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
set "SUMMARY=%OUT%\summary.txt"
type nul > "%SUMMARY%"
set "FAILED="

call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1

if not defined RUN_CI goto afterCi
echo === ci: host ===
call :Start
call "%REPO%\tests\host\run.cmd" %HOST_TIER% "%OUT%\host"
call :Result "host %HOST_TIER%" || goto done
echo === ci: shader ===
call :Start
call "%REPO%\tests\shader\run.cmd" "%OUT%\shader"
call :Result "shader" || goto done
echo === ci: fsr-rr cpu ===
call :Start
call "%REPO%\tests\fsr-rr\run.cmd" cpu "%OUT%\fsr-rr"
call :Result "fsr-rr cpu" || goto done
echo === ci: install ===
call :Start
call "%REPO%\tests\install\run.cmd" %INSTALL_TIER%
call :Result "install" || goto done
if defined RELEASE_CHECKS (
  call :Record INFO "release scope; full installer regression not requested"
  goto afterCi
)
:afterCi

if not defined RUN_DEVICE goto afterDevice
echo === device: host ===
call :Start
call "%REPO%\tests\host\run.cmd" device "%OUT%\host"
call :Result "host device" || goto done
:afterDevice

:done
echo.
echo ===== summary =====
type "%SUMMARY%"
if defined FAILED exit /b 1
exit /b 0

:Result
set "SUITE_EXIT=%errorlevel%"
powershell -NoProfile -Command "Write-Host ('SUITE %~1 seconds=' + [Math]::Round(([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()-[long]$env:SUITE_STARTED)/1000,3) + ' exit=%SUITE_EXIT%')"
if "%SUITE_EXIT%"=="0" (
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

:Start
for /f %%T in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "SUITE_STARTED=%%T"
exit /b 0