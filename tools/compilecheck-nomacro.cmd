@echo off
rem Compile AmdPreSr.cpp with NO diagnostic macros. Used to prove the
rem stripped (pre-diagnostics) tree still builds before committing it.
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
if errorlevel 1 exit /b 1
cl /nologo /c /std:c++latest /EHsc /MD /O2 /DNDEBUG /D_UNICODE /DUNICODE OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\amd\AmdPreSr.cpp /Foanalysis\_stripcheck.obj
exit /b %ERRORLEVEL%
