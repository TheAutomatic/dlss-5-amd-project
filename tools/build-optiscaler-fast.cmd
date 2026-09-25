@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
if errorlevel 1 exit /b 1
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/OptiScaler-DLSSNR-PreSR-Multipass-main/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo
if errorlevel 1 exit /b 1
echo BUILD_OK exports\release-local\OptiScaler.dll
exit /b 0
