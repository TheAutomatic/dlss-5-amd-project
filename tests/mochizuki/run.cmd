@echo off
setlocal
cd /d "%~dp0..\.."
if /i "%~1"=="abi" (
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_build_cache.py
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_source_pin.py
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_aco_temporal.py
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_model_extract.py
  if errorlevel 1 exit /b 1
)
if /i "%~1"=="model" (
  python -B tests\mochizuki\test_model_extract.py || exit /b 1
  exit /b 0
)
if /i "%~1"=="source-pin" (
  python -B tests\mochizuki\test_source_pin.py || exit /b 1
  exit /b 0
)
call tests\_lib\msvc-env.cmd || exit /b 1
if not exist exports\mochizuki-tests mkdir exports\mochizuki-tests
if /i "%~1"=="stage-timing" goto stagetiming
if /i "%~1"=="shared-output" goto sharedoutput
if /i "%~1"=="shared-extended" goto sharedextended
if /i "%~1"=="abi" call :stagetiming || exit /b 1
if /i "%~1"=="renodx-feed" goto renodxfeed
if /i "%~1"=="abi" call :renodxfeed || exit /b 1
if /i "%~1"=="exposure-recording" goto exposurerecording
if /i "%~1"=="inline-auto-spin" goto inlineautospin
if /i "%~1"=="pool-trim" goto pooltrim
if /i "%~1"=="abi" call :acounit || exit /b 1
if /i "%~1"=="abi" call :compilepolicy || exit /b 1
if /i "%~1"=="abi" call :transportpolicy || exit /b 1
if /i "%~1"=="abi" call :networkmodel || exit /b 1
if /i "%~1"=="abi" call :exposurerecording || exit /b 1
if /i "%~1"=="abi" call :inlineautospin || exit /b 1
if /i "%~1"=="abi" call :pooltrim || exit /b 1
if /i "%~1"=="aco-unit" goto acounit
if /i "%~1"=="compile-policy" goto compilepolicy
if /i "%~1"=="transport-policy" goto transportpolicy
if /i "%~1"=="network-model" goto networkmodel
if /i "%~1"=="windows005" goto windows005
if /i "%~1"=="inline" goto inline
if /i "%~1"=="inline-armed" goto inline
if /i "%~1"=="inline-proof" goto inline
if /i "%~1"=="aco-inline" goto inline
if /i "%~1"=="aco-temporal" goto acotemporal
if /i "%~1"=="hdr-shader" goto hdrshader
if /i "%~1"=="phase-failure" goto phasefailure
if /i "%~1"=="inline-lifecycle" goto lifecycle
if /i "%~1"=="wait-profile" goto waitprofile
if /i "%~1"=="aco-wait-profile" goto waitprofile
if /i "%~1"=="inline-profile" goto waitprofile
if /i "%~1"=="aco-inline-profile" goto waitprofile
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/runtime.cpp /Foexports/mochizuki-tests/runtime.obj /Feexports/mochizuki-tests/runtime.exe /link d3d12.lib dxgi.lib || exit /b 1
if /i "%~1"=="destroy-tail" goto destroytail
if /i "%~1"=="startup" goto startup
if /i "%~1"=="profile" goto profile
if /i "%~1"=="composition" goto composition
if /i "%~1"=="first-pass" goto firstpass
if /i "%~1"=="stage-timing-gpu" goto stagetiminggpu
if /i "%~1"=="aco" goto aco
if /i "%~1"=="aco-rapid" goto acorapid
if /i "%~1"=="shader-tail" goto shadertail
if /i "%~1"=="pass-switch" goto passswitch
if /i "%~1"=="split-retain" goto splitretain
if /i "%~1"=="image" goto image
if /i "%~1"=="meter" goto meter
if /i "%~1"=="gpu" (
  call tests\mochizuki\run.cmd hdr-shader || exit /b 1
  call tests\mochizuki\run.cmd phase-failure || exit /b 1
  call tests\mochizuki\run.cmd inline || exit /b 1
  call tests\mochizuki\run.cmd meter || exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition
  if errorlevel 1 exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime"
) else (
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
)
if errorlevel 1 exit /b 1
python -X utf8 -c "from pathlib import Path; import hashlib; p=Path('exports/mochizuki-runtime'); (p/'abi-ci.sha256').write_text(hashlib.sha256((p/'MochizukiNrRuntime.dll').read_bytes()).hexdigest()+'\n')"
exit /b %errorlevel%

:sharedoutput
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/shared_output.cpp /Foexports/mochizuki-tests/shared_output.obj /Feexports/mochizuki-tests/shared_output.exe /link d3d12.lib dxgi.lib || exit /b 1
for %%F in (rgba16 rgba8) do for %%M in (small original) do for %%A in (native aco) do (
  exports\mochizuki-tests\shared_output.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" %%F %%M %%A
  if errorlevel 1 exit /b 1
)
exit /b 0

:sharedextended
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/shared_extended.cpp /Foexports/mochizuki-tests/shared_extended.obj /Feexports/mochizuki-tests/shared_extended.exe /link d3d12.lib dxgi.lib || exit /b 1
python -B tests\mochizuki\shared_extended.py exports\mochizuki-tests\shared_extended.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime"
exit /b %errorlevel%

:stagetiminggpu
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --stage-timing
exit /b %errorlevel%

:stagetiming
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler /I OptiScaler-DLSSNR-PreSR-Multipass-main/external/vulkan/include tests/mochizuki/stage_timing.cpp /Foexports/mochizuki-tests/stage_timing.obj /Feexports/mochizuki-tests/stage_timing.exe || exit /b 1
exports\mochizuki-tests\stage_timing.exe
exit /b %errorlevel%

:renodxfeed
python -B tests\mochizuki\test_renodx_feed.py
exit /b %errorlevel%

:splitretain
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --split-retain
exit /b %errorlevel%

:exposurerecording
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/exposure_recording.cpp /Foexports/mochizuki-tests/exposure_recording.obj /Feexports/mochizuki-tests/exposure_recording.exe /link d3d12.lib dxgi.lib d3dcompiler.lib || exit /b 1
exports\mochizuki-tests\exposure_recording.exe
exit /b %errorlevel%

:inlineautospin
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/inline_auto_spin.cpp /Foexports/mochizuki-tests/inline_auto_spin.obj /Feexports/mochizuki-tests/inline_auto_spin.exe /link d3d12.lib dxgi.lib || exit /b 1
exports\mochizuki-tests\inline_auto_spin.exe
exit /b %errorlevel%

:pooltrim
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/recording_pool_trim.cpp /Foexports/mochizuki-tests/recording_pool_trim.obj /Feexports/mochizuki-tests/recording_pool_trim.exe || exit /b 1
exports\mochizuki-tests\recording_pool_trim.exe
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

:phasefailure
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/queue_segments.cpp /Foexports/mochizuki-tests/queue_segments.obj /Feexports/mochizuki-tests/queue_segments.exe /link d3d12.lib dxgi.lib dxguid.lib || exit /b 1
set "MZ_TEST_INLINE_CHAIN=3"
for %%P in (2 3) do (
  set "MZ_TEST_PHASE_SUBMIT_FAIL_AT=%%P"
  exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" || exit /b 1
)
rem R8G8B8A8_UNORM: the whole chain is one compute phase; its submit fails.
set "MZ_TEST_PHASE_SUBMIT_FAIL_AT=1"
set "MZ_TEST_INLINE_CHAIN=1"
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" unorm8 || exit /b 1
rem R11G11B10_FLOAT + compute convert: input unpack and runtime_pack_r11 write-back keep the chain in one compute phase.
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" r11 || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" bgra8 || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" srgb8 || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" bgra8srgb || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" rgb10a2 || exit /b 1
rem Dynamic resolution with motion vectors of another size: NrInlineComputeMotion keeps one compute phase.
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" motion || exit /b 1
rem Model scale 0.5: NrInlineComputeDownscale keeps one compute phase (RGBA16F and R11G11B10).
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" scaled || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" scaledr11 || exit /b 1
rem Two layers (multi), and two layers with the first-layer export on compute (export).
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" multi || exit /b 1
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" export || exit /b 1
rem Two layers, R11G11B10 with larger RG16F motion vectors: NrInlineComputeMotion samples them in place on every layer.
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" multimotion || exit /b 1
set "MZ_TEST_PHASE_SUBMIT_FAIL_AT="
set "MZ_TEST_INLINE_CHAIN="
rem A refused whole-chain compute recording fails the frame without a passthrough.
set "MZ_TEST_COMPUTE_REFUSE_AT=1"
set "MZ_TEST_INLINE_CHAIN=1"
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" refuse
set "RC=%errorlevel%"
set "MZ_TEST_COMPUTE_REFUSE_AT="
set "MZ_TEST_INLINE_CHAIN="
exit /b %RC%

:lifecycle
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/queue_segments.cpp /Foexports/mochizuki-tests/queue_segments.obj /Feexports/mochizuki-tests/queue_segments.exe /link d3d12.lib dxgi.lib dxguid.lib || exit /b 1
rem All compute toggles on: geometry, format, motion extent, model scale, layers and queue change mid-run;
rem two inline frames in flight; ResetHistory; Destroy/Create. Every frame completes and collects.
set "MZ_TEST_PHASE_SUBMIT_FAIL_AT="
set "MZ_TEST_INLINE_CHAIN="
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" lifecycle
exit /b %errorlevel%

:waitprofile
set "MOCHI_ACO_ARG="
if /i "%~1"=="aco-wait-profile" set "MOCHI_ACO_ARG=--aco"
if /i "%~1"=="aco-inline-profile" set "MOCHI_ACO_ARG=--aco"
set "INCLUDE=%INCLUDE%;%CD%"
set "MOCHI_WAIT_OUT=exports\mochizuki-tests\%~1"
if not "%~2"=="" set "MOCHI_WAIT_OUT=%~2"
if not exist "%MOCHI_WAIT_OUT%" mkdir "%MOCHI_WAIT_OUT%"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/include tests/lmxxf/nr_slots_benchmark.cpp /Fo"%MOCHI_WAIT_OUT%/nr_slots_benchmark.obj" /Fe"%MOCHI_WAIT_OUT%/nr_slots_benchmark.exe" /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/detours/detours.lib || exit /b 1
if /i "%~1"=="inline-profile" goto productionprofile
if /i "%~1"=="aco-inline-profile" goto productionprofile
python -B tests\lmxxf\compare_wait_profiles.py "%MOCHI_WAIT_OUT%\nr_slots_benchmark.exe" "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" "%MOCHI_WAIT_OUT%" --mochizuki %MOCHI_ACO_ARG%
exit /b %errorlevel%

:productionprofile
python -B tests\mochizuki\compare_inline_profiles.py "%MOCHI_WAIT_OUT%\nr_slots_benchmark.exe" "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" "%MOCHI_WAIT_OUT%" %MOCHI_ACO_ARG% %3 %4
exit /b %errorlevel%

:inline
set "MOCHI_ACO_ARG="
if /i "%~1"=="aco-inline" set "MOCHI_ACO_ARG=--aco"
set "INCLUDE=%INCLUDE%;%CD%"
rem Shared recording ownership includes product headers using the vcxproj-style roots.
rem Append after SDK/STL paths so <version> never resolves to repository VERSION.
set "EXT=%CD%\OptiScaler-DLSSNR-PreSR-Multipass-main\external"
set "INCLUDE=%INCLUDE%;%CD%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler;%EXT%\nvngx_dlss_sdk;%EXT%\xess\inc\xess;%EXT%\xess\inc\xell;%EXT%\xess\inc\xess_fg;%EXT%\FidelityFX-SDK\ffx-api\include\ffx_api;%EXT%\simpleini;%EXT%\unordered_dense\include;%EXT%\spdlog\include;%EXT%\freetype;%EXT%\streamline;%EXT%\streamline1;%EXT%\nvapi;%EXT%\nlohmann;%EXT%\fakenvapi;%EXT%\magic_enum\include\magic_enum;%EXT%\AntiLag2-SDK;%EXT%\latencyflex;%EXT%\vulkan\include"
set "MOCHI_INLINE_RUNTIME=%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
if not "%~2"=="" set "MOCHI_INLINE_RUNTIME=%~f2"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/include tests/lmxxf/nr_inline_runtime_gpu.cpp /Foexports/mochizuki-tests/nr_inline_runtime_gpu.obj /Feexports/mochizuki-tests/nr_inline_runtime_gpu.exe /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/detours/detours.lib || exit /b 1
if /i "%~1"=="inline-proof" (
  for %%N in (1 2) do (
    exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 --signal-proof=%%N
    if errorlevel 1 exit /b 1
  )
  exit /b 0
)
rem Production inline defaults (NrGpuHandoff=true, NrSpinPollDelay=16), then a capture that lands
rem after the side watch: the input gate must close, skip the copies and select the original frame.
for %%M in ("" "--timeout" "--ahead" "--large --temporal") do (
  exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 %%~M %MOCHI_ACO_ARG%
  if errorlevel 1 exit /b 1
)
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 --late-capture=64 %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
rem Claimed gate, copy still pending when the side watch ends: the network must wait for the copy on the GPU.
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 --late-copy=64 --abort-ms=5000 %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
rem Claimed gate, capture never lands within the capped side wait: bypass, history reset, reclaim, no hang.
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 --stuck-copy=1024 %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
rem Same, ended by the watchdog abort word (1 ms) instead of the cap.
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --gpu-handoff --poll-delay=16 --stuck-copy=1024 --abort-ms=1 %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
if /i "%~1"=="inline-armed" exit /b 0
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --timeout %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --ahead %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --timeout --ahead %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --large --temporal %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --large --temporal --three-passes %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
rem Equal-extent FP16 and UNORM8 both support the whole compute chain.
set "MZ_TEST_INLINE_CHAIN=1"
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
set "MZ_TEST_INLINE_CHAIN=1"
for %%M in ("" "--timeout" "--ahead" "--timeout --ahead" "--large --temporal") do (
  exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --unorm8 %%~M %MOCHI_ACO_ARG%
  if errorlevel 1 (set "MZ_TEST_INLINE_CHAIN=" & exit /b 1)
)
rem Three passes leave the direct colour input: the UNORM8 frame falls back to segments.
set "MZ_TEST_INLINE_CHAIN=2"
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --unorm8 --large --temporal --three-passes %MOCHI_ACO_ARG%
set "RC=%errorlevel%"
set "MZ_TEST_INLINE_CHAIN="
exit /b %RC%

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
exports\mochizuki-tests\hdr_shader.exe "%MOCHI_HDR_SHADERS%" %3
exit /b %errorlevel%

:profile
rem profile output.raw width height scale passes preprocess compact enlarge (16 warmup + 64 samples)
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --profile "%~2" %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%

:firstpass
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --first-pass
exit /b %errorlevel%

:aco
call :acounit || exit /b 1
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco
if errorlevel 1 exit /b 1
set "MZ_TEST_REJECT_ACO_DEVICE=1"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-device
if errorlevel 1 exit /b 1
set "MZ_TEST_REJECT_ACO_DEVICE="
set "MZ_TEST_FRAME_BUDGET_AT=2"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-budget
if errorlevel 1 exit /b 1
set "MZ_TEST_FRAME_BUDGET_AT="
python -X utf8 -B tests\mochizuki\aco_fallback.py
exit /b %errorlevel%

:acorapid
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-rapid
exit /b %errorlevel%

:networkmodel
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I third_party/mochizuki/windows/src/core tests/mochizuki/network_model.cpp third_party/mochizuki/windows/src/core/nr_native_plan.cpp /Foexports/mochizuki-tests/ /Feexports/mochizuki-tests/network_model.exe || exit /b 1
exports\mochizuki-tests\network_model.exe || exit /b 1
exit /b 0

:compilepolicy
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/compile_policy.cpp /Foexports/mochizuki-tests/compile_policy.obj /Feexports/mochizuki-tests/compile_policy.exe || exit /b 1
exports\mochizuki-tests\compile_policy.exe exports\mochizuki-tests
exit /b %errorlevel%

:transportpolicy
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include tests/mochizuki/transport_policy.cpp /Foexports/mochizuki-tests/transport_policy.obj /Feexports/mochizuki-tests/transport_policy.exe || exit /b 1
exports\mochizuki-tests\transport_policy.exe
exit /b %errorlevel%

:acounit
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/aco.cpp /Foexports/mochizuki-tests/aco.obj /Feexports/mochizuki-tests/aco.exe || exit /b 1
exports\mochizuki-tests\aco.exe third_party\mochizuki\windows\data\aco\records exports\mochizuki-tests\aco-test.nrb || exit /b 1
python -X utf8 tools\build\mochizuki-aco.py --verify --out exports\mochizuki-runtime\dlssnr-amd\aco || exit /b 1
python -X utf8 tools\build\mochizuki-aco.py --verify --out exports\mochizuki-runtime\dlssnr-amd\aco\int4 || exit /b 1
python -X utf8 tools\install\mochizuki-owned.py --check || exit /b 1
exit /b 0

:composition
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition
exit /b %errorlevel%

:windows005
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/windows005.cpp /Foexports/mochizuki-tests/windows005.obj /Feexports/mochizuki-tests/windows005.exe /link d3d12.lib dxgi.lib || exit /b 1
exports\mochizuki-tests\windows005.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" %2
exit /b %errorlevel%

:acotemporal
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/aco_temporal.cpp /Foexports/mochizuki-tests/aco_temporal.obj /Feexports/mochizuki-tests/aco_temporal.exe /link d3d12.lib dxgi.lib || exit /b 1
python -X utf8 -B tests/mochizuki/aco_temporal.py %2 %3 %4 %5
exit /b %errorlevel%

:image
rem <in.f32> <w> <h> <dxgi> <white> <intensity> <preprocess> <linear>: the required --image fields, forwarded as
rem given. The optional tail (out.f32, style, curve, frames, exposure, ...) is longer than cmd's own %1-%9, so for a
rem full line call exports\mochizuki-tests\runtime.exe directly the way the meter case below does.
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --image %1 %2 %3 %4 %5 %6 %7 %8
exit /b %errorlevel%

:meter
rem The meter's own claim, on the one frame it cannot get wrong: a uniform linear 0.05 image has to be put on the key
rem luminance, so the white point must settle at 0.05/0.2073 = 0.2412 whatever the model makes of the picture. Needs
rem an AMD adapter, the built runtime and the user's own model, like the other GPU cases.
if not exist exports\mochizuki-runtime\dlssnr-amd\dlssnr.bin (
  echo MOCHIZUKI_METER_SKIP dlssnr-amd\dlssnr.bin is not installed
  exit /b 0
)
python -X utf8 -B tests\mochizuki\meter_frame.py exports\mochizuki-tests\meter-in.f32 || exit /b 1
rem --image's optional tail is positional: out.f32, mask, skin, style, structure, tone, transfer, colour, max_ratio,
rem curve, bias_ev, contrast, saturation, frames, exposure. Frame count is the fourteenth of those, hence thirteen
rem empty slots before it.
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --image exports\mochizuki-tests\meter-in.f32 256 256 10 1.0 1.0 0 1 "" "" "" "" "" "" "" "" "" "" "" "" "" 30 > exports\mochizuki-tests\meter.log 2>&1
if errorlevel 1 (
  type exports\mochizuki-tests\meter.log
  exit /b 1
)
python -X utf8 -B tests\mochizuki\meter_level.py exports\mochizuki-tests\meter.log
exit /b %errorlevel%
