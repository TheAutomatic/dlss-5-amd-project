@echo off
rem Local release build matching CI: ordinary Release, multi-slot default, no diagnostics.
setlocal
cd /d "%~dp0.."
if not exist "exports\release-local" mkdir "exports\release-local"
if not exist "exports\release-local\smoke" mkdir "exports\release-local\smoke"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
if errorlevel 1 exit /b 1

echo === regression: submission state machine ===
cl /nologo /std:c++20 /EHsc /W4 tests\amd_submission_state.cpp /Feexports\release-local\amd_submission_state.exe /Foexports\release-local\amd_submission_state.obj
if errorlevel 1 exit /b 1
exports\release-local\amd_submission_state.exe
if errorlevel 1 exit /b 1

echo === Release build (CI-equivalent) ===
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo /fl /flp:logfile=%CD%\exports\release-local\build.log;verbosity=normal
if errorlevel 1 exit /b 1
if not exist "exports\release-local\OptiScaler.dll" (
  echo FAIL: OptiScaler.dll not produced
  exit /b 1
)
echo BUILD_OK
exit /b 0
