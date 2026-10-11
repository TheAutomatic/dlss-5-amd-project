@echo off
setlocal
set "ACO_PROFILE="
cd /d "%~dp0..\.."
if /i "%~1"=="abi" (
  python -B tests\mochizuki\test_build_cache.py
  if errorlevel 1 exit /b 1
)
call tests\_lib\msvc-env.cmd || exit /b 1
if not exist exports\mochizuki-tests mkdir exports\mochizuki-tests
if /i "%~1"=="aco-unit" goto acounit
if /i "%~1"=="abi" call :acounit || exit /b 1
if /i "%~1"=="hdr-shader" goto hdrshader
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/runtime.cpp /Foexports/mochizuki-tests/runtime.obj /Feexports/mochizuki-tests/runtime.exe /link d3d12.lib dxgi.lib || exit /b 1
if /i "%~1"=="destroy-tail" goto destroytail
if /i "%~1"=="startup" goto startup
if /i "%~1"=="profile" goto profile
if /i "%~1"=="aco-profile" set "ACO_PROFILE=--aco"
if /i "%~1"=="aco-profile" goto profile
if /i "%~1"=="aco-upstream-profile" set "ACO_PROFILE=--aco-upstream-barriers"
if /i "%~1"=="aco-upstream-profile" goto profile
if /i "%~1"=="aco-barriers" goto acobarriers
if /i "%~1"=="composition" goto composition
if /i "%~1"=="aco" goto aco
if /i "%~1"=="shader-tail" goto shadertail
if /i "%~1"=="pass-switch" goto passswitch
if /i "%~1"=="gpu" (
  call tests\mochizuki\run.cmd hdr-shader || exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition
  if errorlevel 1 exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime"
) else (
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
)
if errorlevel 1 exit /b 1
python -X utf8 -c "from pathlib import Path; import hashlib; p=Path('exports/mochizuki-runtime'); (p/'abi-ci.sha256').write_text(hashlib.sha256((p/'MochizukiNrRuntime.dll').read_bytes()).hexdigest()+'\n')"
exit /b %errorlevel%

:passswitch
set "MZ_TEST_UPGRADE_REFUSALS="
set "MZ_TEST_UPGRADE_OOM_ONCE="
set "MZ_TEST_FRAME_BUDGET_ONCE="
set "MOCHI_PASS_MODE=normal"
if not "%~2"=="" set "MOCHI_PASS_MODE=%~2"
if /i "%~2"=="budget" set "MZ_TEST_UPGRADE_REFUSALS=1"
if /i "%~2"=="oom" set "MZ_TEST_UPGRADE_OOM_ONCE=1"
if /i "%~2"=="frame-budget" set "MZ_TEST_FRAME_BUDGET_ONCE=1"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --pass-switch "%MOCHI_PASS_MODE%"
exit /b %errorlevel%

:startup
set "ASSETS=%CD%\exports\mochizuki-runtime"
if not "%~2"=="" set "ASSETS=%~f2"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%ASSETS%" --startup "%~3" %4 %5
exit /b %errorlevel%

:shadertail
rem The ViT 64-token key chunk is partial at these render resolutions.
for %%S in (1280x720 1129x635 640x360) do (
  for /f "tokens=1,2 delims=x" %%W in ("%%S") do (
    exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --startup "%CD%\exports\mochizuki-tests\tail-%%S.bin" %%W %%X
    if errorlevel 1 exit /b 1
  )
)
exit /b 0

:destroytail
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --destroy-tail
exit /b %errorlevel%

:hdrshader
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/hdr_shader.cpp /Foexports/mochizuki-tests/hdr_shader.obj /Feexports/mochizuki-tests/hdr_shader.exe /link OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/vulkan/vulkan-1.lib || exit /b 1
set "MOCHI_HDR_SHADERS=%CD%\exports\mochizuki-runtime\dlssnr-amd\shaders\runtime"
if not "%~2"=="" set "MOCHI_HDR_SHADERS=%~f2"
exports\mochizuki-tests\hdr_shader.exe "%MOCHI_HDR_SHADERS%"
exit /b %errorlevel%

:profile
rem profile output.raw width height scale passes preprocess compact enlarge (16 warmup + 64 samples)
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --profile "%~2" %3 %4 %5 %6 %7 %8 %9 %ACO_PROFILE%
exit /b %errorlevel%

:composition
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition %2
exit /b %errorlevel%

:acounit
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/aco.cpp /Foexports/mochizuki-tests/aco.obj /Feexports/mochizuki-tests/aco.exe || exit /b 1
exports\mochizuki-tests\aco.exe third_party\mochizuki\windows\data\aco\records exports\mochizuki-tests\aco-test.nrb
exit /b %errorlevel%

:aco
rem GPU: ACO execution, history, resize/replay and compiler switching with retained recordings.
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-switch || exit /b 1
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition --aco
if errorlevel 1 exit /b 1
python -X utf8 -B tests\mochizuki\aco_fallback.py
if errorlevel 1 exit /b 1
python -X utf8 -B tests\mochizuki\aco_first_pass.py
exit /b %errorlevel%

:acobarriers
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-barrier-switch
exit /b %errorlevel%
