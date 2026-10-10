@echo off
setlocal
cd /d "%~dp0..\.."
call tests\_lib\msvc-env.cmd || exit /b 1
if not exist exports\person-partition mkdir exports\person-partition
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX tests\host\person_inference.cpp /Foexports\person-partition\person_inference.obj /Feexports\person-partition\person_inference.exe || exit /b 1
exports\person-partition\person_inference.exe %* || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX tests\host\person_capture_schedule.cpp /Foexports\person-partition\person_capture_schedule.obj /Feexports\person-partition\person_capture_schedule.exe || exit /b 1
exports\person-partition\person_capture_schedule.exe

