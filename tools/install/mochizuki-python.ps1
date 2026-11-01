param([string]$Source, [string]$Output, [string]$Models)

# Dot-sourced by Setup, or run directly by Mochizuki-Model.bat.
function Find-MochizukiPython {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    foreach ($name in @('python', 'python3', 'py')) {
        foreach ($command in @(Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) {
            $remaining = 5000 - [int]$timer.ElapsedMilliseconds
            if ($remaining -le 0) { return $null }
            $process = $null
            $started = $false
            try {
                # A Windows execution alias or broken launcher may exist without Python.
                # Use a waiting wrapper for both EXEs and batch shims so a timeout can
                # terminate the entire probe tree. Isolate Python from user startup code.
                $quoted = $command.Source.Replace("'", "''")
                $code = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new(`$false); & '$quoted' -I -S -c `"import sys; print('MOCHI_PYTHON_OK' if sys.version_info >= (3,10) else 'MOCHI_PYTHON_TOO_OLD')`"; exit `$LASTEXITCODE"
                $start = New-Object Diagnostics.ProcessStartInfo
                $start.FileName = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
                $start.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
                $start.UseShellExecute = $false
                $start.CreateNoWindow = $true
                $start.RedirectStandardOutput = $true
                $start.RedirectStandardError = $true
                $start.StandardOutputEncoding = [Text.Encoding]::UTF8
                $start.StandardErrorEncoding = [Text.Encoding]::UTF8
                $process = New-Object Diagnostics.Process
                $process.StartInfo = $start
                if (-not $process.Start()) { continue }
                $started = $true
                $stdout = $process.StandardOutput.ReadToEndAsync()
                $stderr = $process.StandardError.ReadToEndAsync()
                if (-not $process.WaitForExit([Math]::Min(2000, $remaining))) { continue }
                # A shim that leaves a child holding its pipes must not stall inspection.
                $remaining = [Math]::Max(0, 5000 - [int]$timer.ElapsedMilliseconds)
                if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), $remaining)) { continue }
                if ($process.ExitCode -eq 0 -and ($stdout.Result -split '\r?\n') -contains 'MOCHI_PYTHON_OK') {
                    return $command.Source
                }
            } catch {
                # Continue to the next candidate within the same total deadline.
            } finally {
                if ($process) {
                    if ($started -and -not $process.HasExited) {
                        $kill = New-Object Diagnostics.ProcessStartInfo
                        $kill.FileName = Join-Path $env:SystemRoot 'System32/taskkill.exe'
                        $kill.Arguments = '/PID ' + $process.Id + ' /T /F'
                        $kill.UseShellExecute = $false
                        $kill.CreateNoWindow = $true
                        $kill.RedirectStandardOutput = $true
                        $kill.RedirectStandardError = $true
                        $killer = $null
                        try {
                            $killer = [Diagnostics.Process]::Start($kill)
                            $null = $killer.StandardOutput.ReadToEndAsync()
                            $null = $killer.StandardError.ReadToEndAsync()
                            if (-not $killer.WaitForExit(1000)) { $killer.Kill() }
                        } catch { }
                        finally { if ($killer) { $killer.Dispose() } }
                        try { if (-not $process.HasExited) { $process.Kill() } } catch { }
                    }
                    $process.Dispose()
                }
            }
        }
    }
    return $null
}

function Write-MochizukiPythonHelp {
    Write-Host 'Model extraction cannot run: no working Python 3.10+ was found (missing, too old, or unavailable).' -ForegroundColor Yellow
    Write-Host 'Open Microsoft Store (Windows Store), search for Python, and install Python 3.10 or newer.' -ForegroundColor Yellow
    Write-Host 'After installation, close and reopen Setup.bat or Mochizuki-Model.bat, then retry model extraction.' -ForegroundColor Yellow
}

if ($MyInvocation.InvocationName -ne '.') {
    # Shipped under model-tools/; the package root is the parent then. Direct runs
    # from tools/install keep working, so both layouts resolve.
    $packageRoot = if ((Split-Path -Leaf $PSScriptRoot) -ieq 'model-tools') { Split-Path -Parent $PSScriptRoot } else { $PSScriptRoot }
    if (-not $Source) { $Source = Join-Path $packageRoot 'nvngx_dlssnr.dll' }
    if (-not $Output) { $Output = Join-Path $packageRoot 'dlssnr-amd/dlssnr.bin' }
    $nativeHelper = Join-Path $PSScriptRoot 'lmxxf-small.ps1'
    if (-not (Test-Path -LiteralPath $nativeHelper -PathType Leaf)) { $nativeHelper = Join-Path $packageRoot 'lmxxf-small.ps1' }
    if (Test-Path -LiteralPath $nativeHelper -PathType Leaf) { . $nativeHelper }
    $canNative = (Test-Path -LiteralPath $Source -PathType Leaf) -and
        ((Get-Item -LiteralPath $Source).Length -ge 65536) -and
        [bool](Get-Command Ensure-NativeModelExtractor -ErrorAction SilentlyContinue)
    $python = $null
    if (-not $canNative) {
        $python = Find-MochizukiPython
        if (-not $python) { Write-MochizukiPythonHelp; exit 1 }
    }

    if (-not $Models) {
        Write-Host ''
        Write-Host 'Select Mochizuki models to install:' -ForegroundColor Cyan
        Write-Host '1. Both models (recommended)'
        Write-Host '2. New model only'
        Write-Host '3. Original model only'
        Write-Host '0. Cancel'
        Write-Host ''
        $choice = Read-Host 'Enter your choice (1/2/3/0)'
        switch ($choice.Trim()) {
            '1' { $Models = 'both' }
            '2' { $Models = 'small' }
            '3' { $Models = 'original' }
            '0' { Write-Host 'Operation cancelled.'; exit 0 }
            default {
                Write-Host 'Invalid selection. Operation cancelled.' -ForegroundColor Red
                exit 1
            }
        }
    }

    if ($Models -notin @('both', 'small', 'original')) {
        Write-Host "Invalid models parameter: '$Models'. Expected 'both', 'small', or 'original'." -ForegroundColor Red
        exit 1
    }

    if ($canNative) {
        try {
            Ensure-NativeModelExtractor
            [OptScaler.NativeModelExtractor]::ExtractMochizukiModels($Source, $Output, $Models)
            $result = 0
        } catch {
            Write-Host $_.Exception.Message -ForegroundColor Yellow
            Write-Host 'Model extraction failed. Check nvngx_dlssnr.dll 310.9.16 / 310.8.0. See docs/mochizuki.md.' -ForegroundColor Yellow
            $result = 1
        }
    } else {
        $extractor = Join-Path $PSScriptRoot 'mochizuki-model.py'
        if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) { $extractor = Join-Path $packageRoot 'mochizuki-model.py' }
        if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) {
            Write-Host 'Model extractor missing. Re-extract the complete OptiScaler package.' -ForegroundColor Yellow
            exit 1
        }

        & $python -X utf8 $extractor $Source $Output --models $Models
        $result = $LASTEXITCODE
        if ($result -ne 0) {
            Write-Host 'Model extraction failed. Python was found; see the error above and check nvngx_dlssnr.dll 310.9.16 / 310.8.0. See docs/mochizuki.md.' -ForegroundColor Yellow
        }
    }
    if ($result -eq 0 -and $Models -in @('both', 'original')) {
        try {
            . (Join-Path $PSScriptRoot 'mochizuki-int4.ps1')
            Initialize-MochizukiInt4 -Model $Output -Data (Join-Path $PSScriptRoot 'dlssnr-amd/int4') -Tools (Join-Path $PSScriptRoot 'model-tools')
        } catch {
            Write-Host $_.Exception.Message -ForegroundColor Yellow
            exit 1
        }
    }
    exit $result
}
