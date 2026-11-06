@echo off
rem Local release build matching CI: ordinary Release, multi-slot default, no diagnostics.
rem Usage: tools\build\build-release-local.cmd [--fast]
rem   default  tests\run-all.cmd --tier ci,device, then OptiScaler.dll with exports\release-local\build.log.
rem   --fast   Host + OpenRR runtime only: no tests, no build.log.
setlocal
rem MSVC/MSBuild emit UTF-8 zh-CN text; show it in a UTF-8 console instead of GBK mojibake.
chcp 65001 >nul
cd /d "%~dp0..\.."
set "FAST="
if /i "%~1"=="--fast" set "FAST=1"
if not "%~1"=="" if not defined FAST (
  echo usage: tools\build\build-release-local.cmd [--fast]
  exit /b 2
)
if not exist "exports\release-local" mkdir "exports\release-local"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.26100.0 -vcvars_ver=14.44.35207 || exit /b 1
set "LOGARGS="
if defined FAST goto optiscaler

echo === regression: tests\run-all.cmd --tier ci,device ===
call tests\run-all.cmd --tier ci,device --out exports\release-local\tests || exit /b 1
set "LOGARGS=/fl /flp:logfile=%CD%\exports\release-local\build.log;verbosity=normal"

:optiscaler
echo === Release build (CI-equivalent): OptiScaler.dll ===
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo %LOGARGS% || exit /b 1
if not exist "exports\release-local\OptiScaler.dll" (
  echo FAIL: OptiScaler.dll not produced
  exit /b 1
)
call tools\build\build-openrr-runtime.cmd || exit /b 1
echo BUILD_OK exports\release-local\OptiScaler.dll
exit /b 0