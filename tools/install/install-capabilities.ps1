# Common ASI plugin capability and weight checking for OptScaler NR Launcher.
# Shared between launcher-worker.ps1 and install.ps1.

function Test-MochizukiModelHeaderFile([string]$Path, [uint32]$Entries = 599) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $header = New-Object byte[] 16
        if ($stream.Read($header, 0, 16) -ne 16) { return $false }
        return ([Text.Encoding]::ASCII.GetString($header, 0, 8) -ceq 'NRMODEL1' -and
            [BitConverter]::ToUInt32($header, 8) -eq $Entries -and $stream.Length -gt 16)
    } catch { return $false } finally { if ($stream) { $stream.Dispose() } }
}

function Get-OptScalerFileSha256([string]$Path) {
    $stream = $null
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-OptScalerBackendCapabilities([string]$pkgRoot, [string]$gameDir, [string]$ModelSourceDll = '') {
    $backends = @()
    $sourceDlls = @()
    if ($ModelSourceDll) { $sourceDlls += $ModelSourceDll }
    foreach ($root in @($pkgRoot, $gameDir)) {
        if ($root) { $sourceDlls += Join-Path $root 'nvngx_dlssnr.dll' }
    }

    # --- mochizuki: the official ASI plugin (dlssnr-amd.asi) ---
    $mochiAsi = $null
    $mochiModel = $null
    $mochiSmall = $null
    $mochiExperimental = $null
    $mochiInt4 = $null
    $mochiNv = $null
    if ($pkgRoot) {
        foreach ($cand in @((Join-Path $pkgRoot 'OptiScaler\plugins\dlssnr-amd.asi'), (Join-Path $pkgRoot 'dlssnr-amd.asi'))) {
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiAsi = $cand; break }
        }
        foreach ($rel in @('dlssnr-amd\CC_SILVER_AARDWOLD.bin', 'dlssnr-amd\dlssnr.bin')) {
            $cand = Join-Path $pkgRoot $rel
            if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiModel = $cand; break }
        }
        foreach ($rel in @('dlssnr-amd\CC_SMALL_FP8.bin', 'dlssnr-amd\dlssnr-small.bin')) {
            $cand = Join-Path $pkgRoot $rel
            if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiSmall = $cand; break }
        }
        foreach ($rel in @('dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin', 'dlssnr-amd\dlssnr-small-experimental.bin')) {
            $cand = Join-Path $pkgRoot $rel
            if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiExperimental = $cand; break }
        }
        foreach ($rel in @('dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin', 'dlssnr-amd\dlssnr-int4.bin')) {
            $cand = Join-Path $pkgRoot $rel
            if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiInt4 = $cand; break }
        }
        $cand = Join-Path $pkgRoot 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
    }
    if ($gameDir) {
        if (-not $mochiModel) {
            foreach ($rel in @('dlssnr-amd\CC_SILVER_AARDWOLD.bin', 'dlssnr-amd\dlssnr.bin')) {
                $cand = Join-Path $gameDir $rel
                if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiModel = $cand; break }
            }
        }
        if (-not $mochiSmall) {
            foreach ($rel in @('dlssnr-amd\CC_SMALL_FP8.bin', 'dlssnr-amd\dlssnr-small.bin')) {
                $cand = Join-Path $gameDir $rel
                if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiSmall = $cand; break }
            }
        }
        if (-not $mochiExperimental) {
            foreach ($rel in @('dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin', 'dlssnr-amd\dlssnr-small-experimental.bin')) {
                $cand = Join-Path $gameDir $rel
                if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiExperimental = $cand; break }
            }
        }
        if (-not $mochiInt4) {
            foreach ($rel in @('dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin', 'dlssnr-amd\dlssnr-int4.bin')) {
                $cand = Join-Path $gameDir $rel
                if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Get-Item -LiteralPath $cand).Length -gt 0) { $mochiInt4 = $cand; break }
            }
        }
        if (-not $mochiNv) {
            $cand = Join-Path $gameDir 'nvngx_dlssnr.dll'
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
        }
    }
    if (-not $mochiNv) {
        foreach ($dll in $sourceDlls) {
            if (Test-Path -LiteralPath $dll -PathType Leaf) { $mochiNv = $dll; break }
        }
    }
    $missingMochiPackage = @()
    if (-not $mochiAsi) { $missingMochiPackage += 'OptiScaler/plugins/dlssnr-amd.asi' }
    if ($mochiAsi) {
        foreach ($relative in @('dlssnr-amd/shaders', 'dlssnr-amd/aco', 'dlssnr-amd/aco/d3d12_outer.nra', 'dlssnr-amd/aco/rdna3/d3d12_outer.nra', 'model-tools/dlssnr_extract_model.exe')) {
            if (-not (Test-Path -LiteralPath (Join-Path $pkgRoot $relative))) { $missingMochiPackage += $relative }
        }
    }
    $canMochiRuntime = $missingMochiPackage.Count -eq 0
    $hasMochiModel = [bool]($mochiModel -or $mochiSmall)
    $mochiPreparable = (-not $hasMochiModel) -and [bool]$mochiNv
    $mochiOriginalReady = [bool]$mochiModel
    $mochiSmallReady = [bool]$mochiSmall
    $mochiExperimentalReady = [bool]$mochiExperimental
    $mochiInt4Ready = [bool]$mochiInt4
    $mochiSourceProvidesOriginal = [bool]$mochiNv
    $mochiSourceProvidesSmall = [bool]$mochiNv
    $mochiDelta = $pkgRoot -and (Test-Path -LiteralPath (Join-Path $pkgRoot 'experimental/dlssnr-small-experimental.delta') -PathType Leaf)
    $mochiInt4Data = $pkgRoot -and (
        (Test-Path -LiteralPath (Join-Path $pkgRoot 'dlssnr-amd/int4-ultra') -PathType Container) -or
        (Test-Path -LiteralPath (Join-Path $pkgRoot 'dlssnr-amd/int4') -PathType Container)
    )
    $mochiInt4Tool = $pkgRoot -and (Test-Path -LiteralPath (Join-Path $pkgRoot 'model-tools/dlssnr-int4-weights.exe') -PathType Leaf)
    $mochiExperimentalPreparable = (-not $mochiExperimentalReady) -and
        ($mochiSmallReady -or $mochiSourceProvidesSmall) -and $mochiDelta
    $mochiInt4Preparable = (-not $mochiInt4Ready) -and
        ($mochiOriginalReady -or $mochiSourceProvidesOriginal) -and $mochiInt4Data -and $mochiInt4Tool
    $mochiPreparation = if (-not $canMochiRuntime) { 'missing-runtime' } elseif ($hasMochiModel) { 'ready' } elseif ($mochiNv) { 'preparable' } else { 'missing-helper' }
    $mochiEnabled = $canMochiRuntime -and $hasMochiModel
    $mochiReason = if (-not $canMochiRuntime) { 'Runtime missing' } elseif ($hasMochiModel) { 'Ready' } elseif ($mochiPreparable) { 'Model preparation required' } else { 'Model missing' }
    $backends += @{
        id = 'mochizuki'
        originalState = if ($mochiOriginalReady) { 'ready' } elseif ($mochiSourceProvidesOriginal) { 'preparable' } else { 'missing' }
        smallState = if ($mochiSmallReady) { 'ready' } elseif ($mochiSourceProvidesSmall) { 'preparable' } else { 'missing' }
        experimentalState = if ($mochiExperimentalReady) { 'ready' } elseif ($mochiExperimentalPreparable) { 'preparable' } else { 'missing' }
        int4State = if ($mochiInt4Ready) { 'ready' } elseif ($mochiInt4Preparable) { 'preparable' } else { 'missing' }
        preparationState = $mochiPreparation
        runtimeState = if ($canMochiRuntime) { 'ready' } else { 'missing' }
        modelState = if ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing' }
        enabled = $mochiEnabled
        reasonCode = if (-not $canMochiRuntime) { 'missing-runtime' } elseif ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing-model' }
        reason = $mochiReason
        hints = @(
            if (-not $hasMochiModel) {
                if ($mochiPreparable) { 'mochi-preparable' }
                else { 'mochi-source' }
                if (-not $mochiPreparable) { 'mochi-versions' }
            }
        )
        missingPackageFiles = $missingMochiPackage
    }

    return @($backends)
}
