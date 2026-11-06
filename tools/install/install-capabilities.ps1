# Common backend capability and weight checking for OptScaler NR.
# Shared between launcher-worker.ps1 and install-amd-presr.ps1.

function Test-LmxxfWeightsDir([string]$Directory) {
    if (-not $Directory -or -not (Get-Command Test-LmxxfModel -ErrorAction SilentlyContinue)) { return $false }
    return Test-LmxxfModel $Directory original -Quick
}

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

    # --- lmxxf ---
    $lmxxfRuntime = $null
    $lmxxfMods = $null
    $lmxxfShaders = $null
    $lmxxfWeights = $null
    $lmxxfSmall = $null
    if ($pkgRoot) {
        $cand = Join-Path $pkgRoot 'LmxxfNrRuntime.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $lmxxfRuntime = $cand }
        $cand = Join-Path $pkgRoot 'lmxxf-modules'
        if (Test-Path -LiteralPath $cand -PathType Container) { $lmxxfMods = $cand }
        $cand = Join-Path $pkgRoot 'shaders'
        if (Test-Path -LiteralPath (Join-Path $cand 'native_codec_encode.hlsl') -PathType Leaf) { $lmxxfShaders = $cand }
        $cand = Join-Path $pkgRoot 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if ($gameDir -and -not $lmxxfWeights) {
        $cand = Join-Path $gameDir 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if (Get-Command Test-LmxxfModel -ErrorAction SilentlyContinue) {
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $candidate = Join-Path $root 'native-game-tiled-assets/small'
            if (-not $lmxxfSmall -and (Test-LmxxfModel $candidate small -Quick)) { $lmxxfSmall = $candidate }
        }
    }
    $originalSource = $false
    if (-not $lmxxfWeights) {
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $packs = @((Join-Path $root 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'), (Join-Path $root 'dlssnr-amd/dlssnr.bin'))
            if ($root -eq $pkgRoot) { $packs += @(Join-Path $root 'CC_SILVER_AARDWOLD.bin'); $packs += @(Join-Path $root 'dlssnr.bin') }
            foreach ($pack in $packs) {
                if (Test-MochizukiModelHeaderFile $pack 599) { $originalSource = $true; break }
            }
            if ($originalSource) { break }
        }
        if (-not $originalSource) { foreach ($dll in $sourceDlls) {
            if (Test-Path -LiteralPath $dll -PathType Leaf) {
                try {
                    if ((Get-OptScalerFileSha256 $dll) -in @(
                        '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f',
                        'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e')) {
                        $originalSource = $true; break
                    }
                } catch { }
            }
        } }
    }
    $smallSource = $false
    if (-not $lmxxfSmall) {
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $packs = @((Join-Path $root 'dlssnr-amd/CC_SMALL_FP8.bin'), (Join-Path $root 'dlssnr-amd/dlssnr-small.bin'))
            if ($root -eq $pkgRoot) { $packs += @(Join-Path $root 'CC_SMALL_FP8.bin'); $packs += @(Join-Path $root 'dlssnr-small.bin') }
            foreach ($pack in $packs) {
                if (Test-MochizukiModelHeaderFile $pack 255) { $smallSource = $true; break }
            }
            if ($smallSource) { break }
        }
        if (-not $smallSource) { foreach ($dll in $sourceDlls) {
            if (Test-Path -LiteralPath $dll -PathType Leaf) {
                try { if ((Get-OptScalerFileSha256 $dll) -eq '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f') { $smallSource = $true; break } } catch { }
            }
        } }
    }
    # Installed models may be reused; installed/developer code cannot fill an incomplete update.

    $canLmxxfRuntime = [bool]($lmxxfRuntime -and $lmxxfMods -and $lmxxfShaders)
    $hasLmxxfWeights = [bool]($lmxxfWeights -or $lmxxfSmall)
    $lmxxfEnabled = $canLmxxfRuntime -and $hasLmxxfWeights
    $lmxxfReason = if (-not $canLmxxfRuntime) { 'Runtime missing' } elseif (-not $hasLmxxfWeights) { 'Weights missing' } else { 'Ready' }
    $backends += @{
        id = 'lmxxf'
        runtimeState = if ($canLmxxfRuntime) { 'ready' } else { 'missing' }
        modelState = if ($lmxxfWeights -and $lmxxfSmall) { 'ready' } elseif ($hasLmxxfWeights) { 'partial' } elseif ($originalSource -or $smallSource) { 'preparable' } else { 'missing' }
        originalState = if ($lmxxfWeights) { 'ready' } elseif ($originalSource) { 'preparable' } else { 'missing' }
        smallState = if ($lmxxfSmall) { 'ready' } elseif ($smallSource) { 'preparable' } else { 'missing' }
        enabled = $lmxxfEnabled
        reasonCode = if (-not $canLmxxfRuntime) { 'missing-runtime' } elseif ($lmxxfWeights -and $lmxxfSmall) { 'ready' } elseif ($hasLmxxfWeights) { 'partial' } elseif ($originalSource -or $smallSource) { 'preparable' } else { 'missing-weights' }
        reason = $lmxxfReason
        hints = @(
            if (-not $lmxxfWeights) { if ($originalSource) { 'lmxxf-original-preparable' } else { 'lmxxf-original' } }
            if (-not $lmxxfSmall) { if ($smallSource) { 'lmxxf-small-preparable' } else { 'lmxxf-small' } }
        )
        missingPackageFiles = @(
            if (-not $lmxxfRuntime) { 'LmxxfNrRuntime.dll' }
            if (-not $lmxxfMods) { 'lmxxf-modules/' }
            if (-not $lmxxfShaders) { 'shaders/native_codec_encode.hlsl' }
        )
    }

    # --- daniel ---
    $danielRuntime = $null
    $danielWeights = $null
    if ($pkgRoot) {
        foreach ($name in @('version.dll', 'dlssnr_amd_pass1.dll')) {
            $cand = Join-Path $pkgRoot $name
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielRuntime = $cand; break }
        }
        $cand = Join-Path $pkgRoot 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    if ($gameDir -and -not $danielWeights) {
        $cand = Join-Path $gameDir 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    if ($gameDir -and -not $danielRuntime) {
        foreach ($name in @('version.dll', 'dlssnr_amd_pass1.dll')) {
            $cand = Join-Path $gameDir $name
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielRuntime = $cand; break }
        }
    }
    # Presence is only a capability hint; Setup validates the supported runtime SHA before installation.
    $canDanielRuntime = [bool]$danielRuntime
    $hasDanielWeights = [bool]$danielWeights
    $danielSetup = $pkgRoot -and (Test-Path -LiteralPath (Join-Path $pkgRoot 'dlssnr_on_amd_setup.exe') -PathType Leaf)
    $danielNv = $false
    foreach ($dll in $sourceDlls) {
        if (Test-Path -LiteralPath $dll -PathType Leaf) { $danielNv = $true }
    }
    $danielRuntimePreparable = (-not $canDanielRuntime) -and $danielSetup -and ($hasDanielWeights -or $danielNv)
    $danielWeightsPreparable = (-not $hasDanielWeights) -and $danielNv -and ($canDanielRuntime -or $danielSetup)
    $danielPreparable = $danielRuntimePreparable -or $danielWeightsPreparable
    $danielEnabled = $canDanielRuntime -and $hasDanielWeights
    $danielReason = if (-not $canDanielRuntime) { 'Runtime missing' } elseif (-not $hasDanielWeights) { 'Weights missing' } else { 'Ready' }
    $backends += @{
        id = 'daniel'
        runtimeState = if ($canDanielRuntime) { 'ready' } elseif ($danielRuntimePreparable) { 'preparable' } else { 'missing' }
        modelState = if ($hasDanielWeights) { 'ready' } elseif ($danielWeightsPreparable) { 'preparable' } else { 'missing' }
        enabled = $danielEnabled
        reasonCode = if ($danielPreparable) { 'preparable' } elseif (-not $canDanielRuntime) { 'missing-runtime' } elseif (-not $hasDanielWeights) { 'missing-weights' } else { 'ready' }
        reason = $danielReason
        hints = @(
            if ($danielPreparable) { 'daniel-preparable' }
            else {
                if (-not $canDanielRuntime) { 'daniel-runtime' }
                if (-not $hasDanielWeights) {
                    'daniel-weights'
                    if (-not $danielSetup -and -not $canDanielRuntime) { 'daniel-setup' }
                    if (-not $danielNv) { 'daniel-source' }
                }
            }
        )
        missingPackageFiles = @()
    }

    # --- mochizuki: the official ASI plugin, not a built-in runtime ---
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
        foreach ($relative in @('dlssnr-amd/shaders', 'dlssnr-amd/aco', 'model-tools/dlssnr_extract_model.exe')) {
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

    # When the package ships the standalone Mochizuki ASI (v1.15+) and neither retired
    # runtime (LmxxfNrRuntime.dll / daniel) is present in the package, omit the retired
    # backends from the capability list so single-backend UI never displays stale lmxxf/daniel hints.
    if ($mochiAsi -and -not $lmxxfRuntime -and -not $danielRuntime -and -not $danielSetup) {
        return @($backends | Where-Object { $_.id -eq 'mochizuki' })
    }

    return @($backends)
}
