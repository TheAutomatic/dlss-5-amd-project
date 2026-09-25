@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
if errorlevel 1 exit /b 1
set "REPO_ROOT=..\dlss5-on-amd-9070xt-porting"
if not exist "%REPO_ROOT%\include\LmxxfNrApi.h" set "REPO_ROOT=analysis\lmxxf-dlss5-on-amd"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\lmxxf_nr_abi.cpp /I "%REPO_ROOT%\include" /Fe"%REPO_ROOT%\bin\lmxxf_nr_abi.exe" /Fo"%REPO_ROOT%\bin\lmxxf_nr_abi.obj"
if errorlevel 1 exit /b 1
"%REPO_ROOT%\bin\lmxxf_nr_abi.exe" "%REPO_ROOT%\bin\LmxxfNrRuntime.dll" third_party\lmxxf\modules
if errorlevel 1 exit /b 1
echo TEST_ABI_PASS
exit /b 0
