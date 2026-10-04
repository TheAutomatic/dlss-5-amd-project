@echo off
rem Local release build matching CI: ordinary Release, multi-slot default, no diagnostics.
rem Usage: tools\build\build-release-local.cmd [--fast]
rem   default  LmxxfNrRuntime.dll, tests\run-all.cmd --tier ci,device against that DLL, then OptiScaler.dll
rem            with exports\release-local\build.log.
rem   --fast   OptiScaler.dll only: no tests, no runtime build, no build.log.
setlocal
cd /d "%~dp0..\.."
set "FAST="
if /i "%~1"=="--fast" set "FAST=1"
if not "%~1"=="" if not defined FAST (
  echo usage: tools\build\build-release-local.cmd [--fast]
  exit /b 2
)
if not exist "exports\release-local" mkdir "exports\release-local"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.26100.0 -vcvars_ver=14.44.35207
if errorlevel 1 exit /b 1
set "LOGARGS="
if defined FAST goto optiscaler

call tools\build\build-mochizuki-runtime.cmd --reuse
if errorlevel 1 exit /b 1

echo === Release build: LmxxfNrRuntime.dll ===
call tools\build\build-lmxxf-runtime.cmd exports\release-local
if errorlevel 1 exit /b 1
set "LMXXF_TEST_RUNTIME=%CD%\exports\release-local\LmxxfNrRuntime.dll"
echo === regression: tests\run-all.cmd --tier ci,device ===
call tests\run-all.cmd --tier ci,device --out exports\release-local\tests
if errorlevel 1 exit /b 1
rem The freshness gate and PACKAGE_RELEASE read exports\lmxxf-runtime\.
if not exist "exports\lmxxf-runtime" mkdir "exports\lmxxf-runtime"
copy /Y "exports\release-local\LmxxfNrRuntime.dll" "exports\lmxxf-runtime\LmxxfNrRuntime.dll" >nul
if errorlevel 1 exit /b 1
fc /b "%LMXXF_TEST_RUNTIME%" "exports\lmxxf-runtime\LmxxfNrRuntime.dll" >nul
if errorlevel 1 exit /b 1
copy /Y "exports\release-local\tests\runtime-ci.sha256" "exports\lmxxf-runtime\runtime-ci.sha256" >nul
if errorlevel 1 exit /b 1
set "LOGARGS=/fl /flp:logfile=%CD%\exports\release-local\build.log;verbosity=normal"

:optiscaler
echo === Release build (CI-equivalent): OptiScaler.dll ===
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo %LOGARGS%
if errorlevel 1 exit /b 1
if not exist "exports\release-local\OptiScaler.dll" (
  echo FAIL: OptiScaler.dll not produced
  exit /b 1
)
echo BUILD_OK exports\release-local\OptiScaler.dll
exit /b 0
