@echo off
rem Forwarder to tools\build\build-lmxxf-runtime.cmd. Kept because sync-lmxxf-upstream.ps1 and
rem stage-lmxxf-beside-optiscaler.ps1 still call this path. They move (and this file goes) in one
rem commit once the pending lmxxf integration closes; until then their content must not change.
call "%~dp0build\build-lmxxf-runtime.cmd" %*
exit /b %errorlevel%
