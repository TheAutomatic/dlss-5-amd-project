@echo off
rem Enter the x64 MSVC 14.44 environment when cl.exe is not already on PATH.
rem CALL this without setlocal so the environment reaches the caller. CI (and any
rem developer prompt) already has cl, so this is a no-op there.
where cl.exe >nul 2>&1 && exit /b 0
set "MSVC_ENV_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%MSVC_ENV_VCVARS%" (
  echo FAIL: cl.exe is not on PATH and "%MSVC_ENV_VCVARS%" does not exist.
  exit /b 1
)
call "%MSVC_ENV_VCVARS%" x64 10.0.26100.0 -vcvars_ver=14.44.35207 >nul
where cl.exe >nul 2>&1 || (
  echo FAIL: vcvarsall did not put cl.exe on PATH.
  exit /b 1
)
exit /b 0
