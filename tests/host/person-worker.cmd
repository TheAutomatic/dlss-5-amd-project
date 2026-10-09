@echo off
setlocal
cd /d "%~dp0..\.."
call tests\_lib\msvc-env.cmd || exit /b 1
set "PERSON_TEST_OUT=exports\person-worker-test"
if not exist "%PERSON_TEST_OUT%\fixture" mkdir "%PERSON_TEST_OUT%\fixture"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX tests\host\person_worker_fixture.cpp /Fo"%PERSON_TEST_OUT%\fixture.obj" /Fe"%PERSON_TEST_OUT%\fixture\person-worker.exe" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX tests\host\person_worker_ipc.cpp /Fo"%PERSON_TEST_OUT%\ipc.obj" /Fe"%PERSON_TEST_OUT%\ipc.exe" || exit /b 1
powershell -NoProfile -Command "$p=Start-Process -WindowStyle Hidden -FilePath '%PERSON_TEST_OUT%\ipc.exe' -ArgumentList '%PERSON_TEST_OUT%\fixture' -RedirectStandardOutput '%PERSON_TEST_OUT%\result.log' -RedirectStandardError '%PERSON_TEST_OUT%\error.log' -PassThru; if(!$p.WaitForExit(30000)){$p.Kill(); throw 'IPC test timeout'}; Get-Content '%PERSON_TEST_OUT%\result.log'; Get-Content '%PERSON_TEST_OUT%\error.log'; exit $p.ExitCode"
exit /b %errorlevel%
