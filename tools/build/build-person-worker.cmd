@echo off
setlocal
cd /d "%~dp0..\.."
set "PERSON_OUT=%~1"
if not defined PERSON_OUT set "PERSON_OUT=exports\release-local\person-model"
if not exist "%PERSON_OUT%" mkdir "%PERSON_OUT%"
call tests\_lib\msvc-env.cmd || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
 OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\person\worker\person_worker.cpp ^
 /Fo"%PERSON_OUT%\person-worker.obj" /Fe"%PERSON_OUT%\person-worker.exe" ^
 /link /SUBSYSTEM:WINDOWS shell32.lib || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\check-person-worker.ps1 -WorkerExe "%PERSON_OUT%\person-worker.exe" -WriteReceipt
exit /b %errorlevel%
