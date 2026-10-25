# Run independent host compilation and CI suites concurrently on one runner.
# The parent returns success only after both workers have completed successfully.
[CmdletBinding()]
param([ValidateSet('all', 'host', 'tests')][string]$Worker = 'all', [switch]$FullRegression)
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
    cmd /d /c tools\build\build-person-worker.cmd "$out/person-model"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}
if ($Worker -eq 'tests') {
    # Let Windows PowerShell 5.1 children discover their own modules instead of inheriting pwsh's modules.
    $env:PSModulePath = $null
    # This entrypoint always tests its own newly built lmxxf DLL.
    $env:LMXXF_TEST_RUNTIME = $null
    $tier = if ($FullRegression) { 'ci' } else { 'release' }
    cmd /d /c tests\run-all.cmd --tier $tier --out exports\release-ci\tests
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
        $arguments = @('-NoProfile', '-File', "`"$PSCommandPath`"", '-Worker', $name)
        if ($FullRegression) { $arguments += '-FullRegression' }
        $process = Start-Process -FilePath (Get-Command pwsh).Source -WindowStyle Hidden `
            -ArgumentList $arguments -WorkingDirectory $root `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $workers += [pscustomobject]@{ Name=$name; Process=$process; Started=[Diagnostics.Stopwatch]::StartNew(); Readers=@(
            [IO.StreamReader]::new([IO.File]::Open($stdout, 'Open', 'Read', 'ReadWrite')),
            [IO.StreamReader]::new([IO.File]::Open($stderr, 'Open', 'Read', 'ReadWrite'))); Reported=$false }
    }
    do {
        foreach ($item in $workers) {
            foreach ($reader in $item.Readers) {
                while ($reader.Peek() -ge 0) { Write-Host "[$($item.Name)] $($reader.ReadLine())" }
            }
            if ($item.Process.HasExited -and !$item.Reported) {
                $item.Process.WaitForExit()
                Write-Host "CI WORKER $($item.Name): exit=$($item.Process.ExitCode) seconds=$([Math]::Round($item.Started.Elapsed.TotalSeconds,3))"
                $item.Reported = $true
            }
        }
        if (@($workers | Where-Object { !$_.Reported }).Count) { Start-Sleep -Milliseconds 500 }
    } while (@($workers | Where-Object { !$_.Reported }).Count)
} finally {
    foreach ($item in $workers) {
        $item.Process.WaitForExit()
        foreach ($reader in $item.Readers) {
            while ($reader.Peek() -ge 0) { Write-Host "[$($item.Name)] $($reader.ReadLine())" }
            $reader.Dispose()
        }
    }
}
if (@($workers | Where-Object { $_.Process.ExitCode -ne 0 }).Count) { exit 1 }
Write-Host 'HOST_AND_CI_PASSED'
