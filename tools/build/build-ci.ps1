# Run independent host compilation and CI suites concurrently on one runner.
# The parent returns success only after both workers have completed successfully.
[CmdletBinding()]
param([ValidateSet('all', 'host', 'tests')][string]$Worker = 'all')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location -LiteralPath $root
$out = Join-Path $root 'exports/release-ci'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if ($Worker -eq 'host') {
    $sln = "$root/OptiScaler-DLSSNR-PreSR-Multipass-main/"
    msbuild "$sln/OptiScaler/OptiScaler.vcxproj" /m:2 /t:Build `
        /p:Configuration=Release /p:Platform=x64 `
        /p:PlatformToolset=$env:PLATFORM_TOOLSET /p:VCToolsVersion=$env:MSVC_VERSION `
        /p:WindowsTargetPlatformVersion=$env:WINDOWS_SDK_VERSION `
        /p:PostBuildEventUseInBuild=false /p:SolutionDir="$sln" `
        /p:OutDir="$out/" /p:IntDir="$out/obj/" /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (!(Test-Path -LiteralPath "$out/OptiScaler.dll")) { throw 'Host DLL was not produced' }
    exit 0
}
if ($Worker -eq 'tests') {
    # This entrypoint always tests its own newly built lmxxf DLL.
    $env:LMXXF_TEST_RUNTIME = $null
    cmd /d /c tests\run-all.cmd --tier ci --out exports\release-ci\tests
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    New-Item -ItemType Directory -Force exports/lmxxf-runtime | Out-Null
    Copy-Item -LiteralPath "$out/tests/runtime/LmxxfNrRuntime.dll" -Destination exports/lmxxf-runtime/LmxxfNrRuntime.dll
    Copy-Item -LiteralPath "$out/tests/runtime-ci.sha256" -Destination exports/lmxxf-runtime/runtime-ci.sha256
    exit 0
}

$workers = @()
try {
    foreach ($name in @('host', 'tests')) {
        $stdout = Join-Path $out "$name.log"
        $stderr = Join-Path $out "$name.stderr.log"
        $process = Start-Process -FilePath (Get-Command pwsh).Source -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-File', "`"$PSCommandPath`"", '-Worker', $name) `
            -WorkingDirectory $root -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $workers += [pscustomobject]@{ Name=$name; Process=$process; Out=$stdout; Err=$stderr }
    }
} finally {
    # Even if launching a worker failed, join any worker already started.
    foreach ($item in $workers) { $item.Process.WaitForExit() }
}
$failed = $false
foreach ($item in $workers) {
    Write-Host "--- $($item.Name), exit $($item.Process.ExitCode) ---"
    Get-Content -LiteralPath $item.Out
    Get-Content -LiteralPath $item.Err
    if ($item.Process.ExitCode -ne 0) { $failed = $true }
}
if ($failed) { exit 1 }
Write-Host 'HOST_AND_CI_PASSED'
