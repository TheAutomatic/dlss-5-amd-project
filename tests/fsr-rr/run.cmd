@echo off
rem FSR-RR offline chain: build + run the real converter -> RR 1.2 -> composition ->
rem FSR upscaler runner against the current tree closure and, optionally, the frozen
rem baseline closure (exports/rr-baseline-2c2f619b.zip or its extraction).
rem Usage: tests\fsr-rr\run.cmd [cpu^|gpu^|perf] [out-dir] [flags]
rem   flags: --manifest path          explicit reviewed manifest (gpu/perf)
rem          --baseline-source p      zip or extracted dir of the frozen closure
rem          --build-only             build the exe(s) and exit 0 (no run)
rem          --baseline-only          build/run only the frozen baseline closure
rem          --scenes csv             scene filter forwarded to the runner
rem          --fixture-dir path       replay a producer capture (Godot schema)
rem   cpu  : ABI/contract only, no GPU required (default)
rem   gpu  : serial correctness chain (execution PASS/FAIL + metrics/images; quality
rem          acceptance comes from compare_runs.py baseline-vs-candidate, never from
rem          this tier alone)
rem   perf : per-stage GPU timestamps with a bounded in-flight ring
rem The gpu/perf tiers REQUIRE an explicit reviewed manifest (adapter LUID + absolute
rem provider DLLs). If it is missing, a template is probed into <out>\manifest.ini and
rem the tier FAILS with instructions - no silent defaults, no SKIP-with-PASS.
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "TIER=%~1"
if not defined TIER set "TIER=cpu"
set "OUT=%~2"
set "MANIFEST="
set "BASELINE_SOURCE="
set "BUILD_ONLY="
set "BASELINE_ONLY="
set "EXTRA_SCENES="
set "FIXTURE_DIR="
set "FEATURE_CASE="
set "ZERO_REFERENCE="
set "SET_VALUES="
if /i "%TIER%"=="native" goto Native

rem ---- argument parsing (tier and out-dir are positional, rest are flags) ----
set "ALL_ARGS=%*"
call :ParseFlags %ALL_ARGS%
if errorlevel 1 exit /b 2
if defined BASELINE_ONLY if not defined BASELINE_SOURCE (
  echo FAIL: --baseline-only requires --baseline-source.
  exit /b 2
)
if not defined OUT set "OUT=exports\test-run\fsr-rr"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"

call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1

set "P=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler"
set "EXT=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\external"
set "FFXV2=%EXT%\FidelityFX-SDK-v2\Kits\FidelityFX"

rem Include roots for the product header chain (mirrors tests\host\run.cmd: pch.h ->
rem Config.h -> State.h pull nvngx/xess/streamline/spdlog/... headers).
set "INCLUDE=%INCLUDE%;%EXT%\nvngx_dlss_sdk;%EXT%\xess\inc\xess;%EXT%\xess\inc\xell;%EXT%\xess\inc\xess_fg;%EXT%\simpleini;%EXT%\unordered_dense\include;%EXT%\spdlog\include;%EXT%\freetype;%EXT%\streamline;%EXT%\streamline1;%EXT%\nvapi;%EXT%\nlohmann;%EXT%\fakenvapi;%EXT%\magic_enum\include\magic_enum;%EXT%\AntiLag2-SDK;%EXT%\latencyflex;%EXT%\vulkan\include"

rem The FidelityFX-SDK-v2 API roots go FIRST as /I so <ffx_api.h> and
rem "dx12/ffx_api_dx12.h" always resolve to the closure the signed effect DLLs are
rem built against (never the older FidelityFX-SDK ffx-api tree). These roots serve
rem every tier, including the CPU ABI test that includes the same product headers.
set "FFX_INC0=%FFXV2%\api\include"
set "FFX_INC1=%FFXV2%\upscalers\include"
rem Repo root goes at the END of INCLUDE, never /I: /I is searched BEFORE the SDK/STL
rem directories and the repo-root VERSION file would then shadow <version> (AGENTS rule).
set "INCLUDE=%INCLUDE%;%REPO%"
set "PRODUCT_INCS=/I"%FFX_INC0%" /I"%FFX_INC1%" /I"%P%" /I"%P%\include""

set "CXX=cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2"
set "LINKS=d3d12.lib dxgi.lib dxguid.lib bcrypt.lib user32.lib version.lib d3dcompiler.lib "%P%\library\detours\detours.lib""

if /i "%TIER%"=="cpu" goto Cpu
if /i "%TIER%"=="gpu" goto Chain
if /i "%TIER%"=="perf" goto Chain
echo usage: tests\fsr-rr\run.cmd [cpu^|gpu^|perf] [out-dir] [flags]
exit /b 2

:Native
call "%REPO%\tests\fsr-rr\fsrd_feature_smoke.cmd" %2 %3 %4 %5
exit /b %errorlevel%

:Cpu
echo === fsr-rr cpu: ABI + contract ===
%CXX% %PRODUCT_INCS% /DUNICODE /D_UNICODE tests\fsr-rr\abi_contract.cpp "%P%\OptiTypes.cpp" /Fe"%OUT%\abi_contract.exe" /Fo"%OUT%/" /link "%P%\library\detours\detours.lib" dxgi.lib || goto fail
"%OUT%\abi_contract.exe" || goto fail
python -B -m unittest discover -s tests/fsr-rr -p test_compare_runs.py || goto fail
echo fsr-rr cpu: PASS
exit /b 0

:Chain
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "BASE_ROOT="
if defined BASELINE_SOURCE call :PrepareBaseline || goto fail
if defined BUILD_ONLY if defined BASELINE_ONLY goto Done
if defined BASELINE_ONLY goto RunBaseline

if not defined BASELINE_SOURCE goto BuildCandidate
rem Build the candidate only when its closure is expected to compile (the baseline
rem scratch root below is deliberately not on the candidate include path).
:BuildCandidate
echo === fsr-rr %TIER%: build candidate chain (tree closure) ===
call :BuildChain "%P%\shaders\fsrd_preprocess\FSRDPreprocessor_Dx12.cpp" "%P%" "%OUT%\obj\cand" "%OUT%\offline_chain.exe" || goto fail
if defined BUILD_ONLY (
  echo fsr-rr %TIER%: build-only complete ^(candidate: %OUT%\offline_chain.exe^)
  exit /b 0
)

rem ---- manifest gate ----
set "M=%MANIFEST%"
if not defined M set "M=%OUT%\manifest.ini"
if not exist "%M%" (
  echo === fsr-rr %TIER%: manifest missing; probing a template for review ===
  "%OUT%\offline_chain.exe" --probe --manifest-out "%M%"
  if not exist "%M%" goto fail
  echo FAIL: manifest template written to %M%. Review adapter_luid and the absolute
  echo provider DLL paths, then rerun with --manifest "%M%".
  exit /b 1
)

set "CHAINMODE=serial"
if /i "%TIER%"=="perf" set "CHAINMODE=perf"

echo === fsr-rr %TIER%: candidate run (%CHAINMODE%) ===
set "RUNFLAGS=--mode %CHAINMODE% --manifest "%M%" --out "%OUT%\candidate" --closure-label candidate"
if defined EXTRA_SCENES set "RUNFLAGS=%RUNFLAGS% --scenes "%EXTRA_SCENES%""
if defined FIXTURE_DIR set "RUNFLAGS=%RUNFLAGS% --fixture-dir "%FIXTURE_DIR%""
if defined FEATURE_CASE set "RUNFLAGS=%RUNFLAGS% --feature-case "%FEATURE_CASE%""
if defined SET_VALUES set "RUNFLAGS=%RUNFLAGS% %SET_VALUES%"
if defined ZERO_REFERENCE set "RUNFLAGS=%RUNFLAGS% --zero-reference "%ZERO_REFERENCE%""
"%OUT%\offline_chain.exe" %RUNFLAGS% || goto fail

:RunBaseline
if not defined BASE_ROOT goto Done
echo === fsr-rr %TIER%: frozen baseline run (%CHAINMODE%) ===
set "BRUNFLAGS=--mode %CHAINMODE% --manifest "%M%" --out "%OUT%\baseline" --closure-label baseline"
if defined EXTRA_SCENES set "BRUNFLAGS=%BRUNFLAGS% --scenes "%EXTRA_SCENES%""
if defined FIXTURE_DIR set "BRUNFLAGS=%BRUNFLAGS% --fixture-dir "%FIXTURE_DIR%""
"%OUT%\offline_chain_baseline.exe" %BRUNFLAGS% || goto fail
if exist "%OUT%\candidate\run.json" (
  echo === fsr-rr %TIER%: baseline vs candidate analysis ===
  python -B tests\fsr-rr\compare_runs.py "%OUT%\baseline" "%OUT%\candidate" --out "%OUT%\compare" || goto fail
)
goto Done

:Done
if defined BUILD_ONLY echo fsr-rr %TIER%: build-only complete
if not defined BUILD_ONLY echo fsr-rr %TIER%: execution PASS ^(quality acceptance only via compare_runs.py^)
exit /b 0

rem ==== subroutines ==========================================================

:ParseFlags
shift
shift
:ParseNext
if "%~1"=="" exit /b 0
if /i "%~1"=="--manifest" ( set "MANIFEST=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--baseline-source" ( set "BASELINE_SOURCE=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--scenes" ( set "EXTRA_SCENES=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--fixture-dir" ( set "FIXTURE_DIR=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--feature-case" ( set "FEATURE_CASE=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--zero-reference" ( set "ZERO_REFERENCE=%~2" & shift & shift & goto ParseNext )
if /i "%~1"=="--set" ( call set "SET_VALUES=%%SET_VALUES%% --set "%~2"" & shift & shift & goto ParseNext )
if /i "%~1"=="--build-only" ( set "BUILD_ONLY=1" & shift & goto ParseNext )
if /i "%~1"=="--baseline-only" ( set "BASELINE_ONLY=1" & shift & goto ParseNext )
echo unknown flag: %~1
exit /b 1

rem BuildChain <converterCpp> <converterIncludeRootFirst> <objDir> <exe>
rem The converter include root that must win for "shaders/fsrd_preprocess/*.h" is
rem passed explicitly (baseline scratch tree for baseline builds, product tree for
rem candidate builds). Everything else resolves from the product tree.
:BuildChain
set "CONV_CPP=%~1"
set "CONV_ROOT=%~2"
set "OBJD=%~3"
set "EXE=%~4"
if not exist "%OBJD%" mkdir "%OBJD%"
if not exist "%CONV_CPP%" (
  echo FAIL: converter TU not found: %CONV_CPP%
  exit /b 1
)
%CXX% /DUNICODE /D_UNICODE /I"%FFX_INC0%" /I"%FFX_INC1%" /I"%CONV_ROOT%" /I"%P%" /I"%P%\include" tests\fsr-rr\offline_chain.cpp tests\fsr-rr\offline_support.cpp "%CONV_CPP%" "%P%\misc\DeferredRelease.cpp" /Fe"%EXE%" /Fo"%OBJD%/" /link %LINKS% || exit /b 1
"%EXE%" --metric-self-test || exit /b 1
exit /b 0

rem PrepareBaseline: extract/locate the frozen closure and mirror the one shared
rem shader header the frozen converter reaches through "../" (everything else in the
rem frozen closure's own directory came from the archive; shared infra headers -
rem pch/Config/State/dlssnr - deliberately resolve from the live tree, unchanged).
:PrepareBaseline
set "BSRC=%BASELINE_SOURCE%"
if not exist "%BSRC%" (
  echo FAIL: --baseline-source not found: %BSRC%
  exit /b 1
)
rem A .zip is expanded under the output dir; a directory is used as-is and must
rem already carry the OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler prefix.
if /i "%BSRC:~-4%"==".zip" (
  if not exist "%OUT%\baseline-extract\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\shaders\fsrd_preprocess\FSRDPreprocessor_Dx12.cpp" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Force '%BSRC%' '%OUT%\baseline-extract'" || exit /b 1
  )
  set "BSRC=%OUT%\baseline-extract"
)
set "BASE_ROOT=%BSRC%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler"
if not exist "%BASE_ROOT%\shaders\fsrd_preprocess\FSRDPreprocessor_Dx12.cpp" (
  echo FAIL: baseline closure missing under %BASE_ROOT% ^(expected OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler prefix^)
  exit /b 1
)
if not exist "%BASE_ROOT%\shaders" mkdir "%BASE_ROOT%\shaders"
copy /y "%P%\shaders\Shader_Dx12Utils.h" "%BASE_ROOT%\shaders\Shader_Dx12Utils.h" >nul || exit /b 1
echo === fsr-rr %TIER%: build frozen baseline chain ^(from %BASELINE_SOURCE%^) ===
call :BuildChain "%BASE_ROOT%\shaders\fsrd_preprocess\FSRDPreprocessor_Dx12.cpp" "%BASE_ROOT%" "%OUT%\obj\base" "%OUT%\offline_chain_baseline.exe" || exit /b 1
if defined BUILD_ONLY (
  echo fsr-rr %TIER%: build-only complete ^(baseline: %OUT%\offline_chain_baseline.exe^)
  exit /b 0
)
rem A baseline-only run still needs the manifest gate.
if not defined M set "M=%MANIFEST%"
if not defined M set "M=%OUT%\manifest.ini"
if not exist "%M%" (
  echo === fsr-rr %TIER%: manifest missing; probing a template for review ===
  "%OUT%\offline_chain_baseline.exe" --probe --manifest-out "%M%"
  if not exist "%M%" goto fail
  echo FAIL: manifest template written to %M%. Review adapter_luid and the absolute
  echo provider DLL paths, then rerun with --manifest "%M%".
  exit /b 1
)
set "CHAINMODE=serial"
if /i "%TIER%"=="perf" set "CHAINMODE=perf"
exit /b 0

:fail
echo fsr-rr %TIER%: FAIL
exit /b 1
