# Shipping module build and verification helpers. Dot-sourced by sync-lmxxf-upstream.ps1.
function Get-FileSha256Hex([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try {
            return (-join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }))
        } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Get-TreeFingerprint([string]$dir, [string[]]$filters) {
    if (-not (Test-Path -LiteralPath $dir)) { return 'missing' }
    $files = @()
    foreach ($f in $filters) {
        $files += @(Get-ChildItem -LiteralPath $dir -File -Filter $f -ErrorAction SilentlyContinue)
    }
    $files = @($files | Sort-Object { $_.Name.ToLowerInvariant() } -Unique)
    if ($files.Count -lt 1) { return 'empty' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $ms = New-Object IO.MemoryStream
    try {
        foreach ($file in $files) {
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($file.Name + ':' + $file.Length + ':')
            $ms.Write($nameBytes, 0, $nameBytes.Length)
            $payload = [IO.File]::ReadAllBytes($file.FullName)
            $ms.Write($payload, 0, $payload.Length)
        }
        return (-join ($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString('x2') }))
    } finally {
        $ms.Dispose()
        $sha.Dispose()
    }
}

function Resolve-ModulesPath([string]$override) {
    # Upstream git never publishes .hsaco (release/ is gitignored). Only an explicit path counts.
    if (-not $override) { return $null }
    if (-not (Test-Path -LiteralPath $override -PathType Container)) {
        throw ("ModulesPath not found: " + $override)
    }
    return (Resolve-Path -LiteralPath $override).Path
}

function Invoke-BuildGfx1201Modules([string]$hipDir, [string]$outDir) {
    $buildPs1 = Join-Path $hipDir 'build-modules.ps1'
    if (-not (Test-Path -LiteralPath $buildPs1 -PathType Leaf)) {
        throw ("Missing " + $buildPs1 + "; cannot build shipping .hsaco")
    }
    $compiler = Join-Path $hipDir 'rtc_compile.exe'
    # Rebuild the small compiler too: its source may have changed since the previous sync.
    $rtcCpp = Join-Path $hipDir 'rtc_compile.cpp'
    if (-not (Test-Path -LiteralPath $rtcCpp -PathType Leaf)) {
        throw ("Missing rtc_compile.exe / rtc_compile.cpp under " + $hipDir)
    }
    Write-Host "  Building rtc_compile.exe from rtc_compile.cpp..." -ForegroundColor Cyan
    & cl.exe /nologo /O2 /EHsc /Fe:$compiler $rtcCpp | Write-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        throw "Failed to build rtc_compile.exe (need MSVC cl in PATH)"
    }
    if (Test-Path -LiteralPath $outDir) {
        Remove-SyncTree -Path $outDir -Within $hipDir
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    # Kernel gates the author turns on in his own deployment scripts but not in the recipe.
    # hip/build-modules.ps1's `defines` column is only part of his configuration: prod7 builds
    # mhfast.generated.hip from a lab generator we do not have, so a speed-up he ships can sit
    # here behind a #define the recipe never sets. tools/audit-lmxxf-enablements.py reports the
    # gaps; this table is the ones we decided to take. The macro is module-wide, so only add it
    # where the per-instantiation preconditions hold (see the audit and the kernel comment).
    $ModuleDefineOverrides = @{}
    $defineConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'module-defines.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    foreach ($entry in $defineConfig.PSObject.Properties) { $ModuleDefineOverrides[$entry.Name] = @($entry.Value) }
    $localRecipe = $null
    if ($ModuleDefineOverrides.Count -gt 0) {
        $recipeText = [IO.File]::ReadAllText($buildPs1)
        foreach ($m in $ModuleDefineOverrides.Keys) {
            $rx = New-Object Text.RegularExpressions.Regex(
                "(?m)^(\s*@\{\s*name = '" + [regex]::Escape($m) + "';\s*defines = @\()([^)]*)(\))")
            $hit = $rx.Match($recipeText)
            if (-not $hit.Success) {
                throw "Module define override: no recipe row named '$m' in $buildPs1"
            }
            $inner = $hit.Groups[2].Value.Trim()
            $additions = @()
            foreach ($define in $ModuleDefineOverrides[$m]) {
                if ($define -notmatch '^(HIP_[A-Z0-9_]+) ([0-9]+)$') { throw "Invalid module override: $define" }
                $macro = $Matches[1]
                $existing = [regex]::Match($inner, "'" + [regex]::Escape($macro) + " ([^']+)'")
                if ($existing.Success) {
                    if ($existing.Value -ne "'$define'") { throw "Upstream now sets $macro differently in $m; review module-defines.json" }
                } else { $additions += "'$define'" }
            }
            if ($additions.Count -eq 0) { continue }
            $extra = $additions -join ', '
            if ($inner.Length -gt 0) { $combined = $inner + ', ' + $extra } else { $combined = $extra }
            $recipeText = $recipeText.Substring(0, $hit.Groups[2].Index) + $combined +
                          $recipeText.Substring($hit.Groups[2].Index + $hit.Groups[2].Length)
            Write-Host ("  module define override: $m + " + ($ModuleDefineOverrides[$m] -join ', ')) -ForegroundColor Yellow
        }
        $localRecipe = Join-Path $outDir 'build-modules.local.ps1'
        [IO.File]::WriteAllText($localRecipe, $recipeText, [Text.UTF8Encoding]::new($false))
    }
    if (-not $localRecipe) { $localRecipe = $buildPs1 }
    Write-Host ("  Building gfx1201 modules via build-modules.ps1 -> " + $outDir) -ForegroundColor Cyan
    try {
        # | Write-Host, NOT the pipeline: the recipe Write-Outputs one hash line per module, and
        # this function's return value is a PATH. Capturing that output turned $modulesSrc into
        # an array of 24 hash lines plus the path, and every later -LiteralPath / -Filter argument
        # in Sync-LmxxfModules then shifted out of place.
        $global:LASTEXITCODE = 0
        & $localRecipe -OutputDir $outDir -Compiler $compiler -SourceDir $hipDir -Targets gfx1201 | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "Module recipe exited with $LASTEXITCODE" }
    } catch {
        throw ("build-modules.ps1 failed: " + $_.Exception.Message + " [at: " + $_.InvocationInfo.PositionMessage + "]")
    }
    $built = @(Get-ChildItem -LiteralPath $outDir -Filter '*.hsaco' -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer })
    if ($built.Count -lt 1) {
        throw ("build-modules.ps1 produced no .hsaco under " + $outDir)
    }
    return $outDir
}

function Assert-ModulesMatchHipSums([string]$modulesDir, [string]$hipSums, [switch]$allowStale) {
    if (-not (Test-Path -LiteralPath $hipSums -PathType Leaf)) {
        throw 'hip SHA256SUMS missing; cannot verify shipping modules.'
    }
    $bad = @()
    $rowCount = 0
    foreach ($line in Get-Content -LiteralPath $hipSums -Encoding UTF8) {
        if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+gfx1201/(?<n>.+\.hsaco)$') { continue }
        $rowCount++
        $name = $Matches['n']
        if ($name -match '[/\\]' -or $name -eq '..') { throw 'Unsafe module path in SHA256SUMS' }
        $want = $Matches['h'].ToLowerInvariant()
        $fp = Join-Path $modulesDir $name
        if (-not (Test-Path -LiteralPath $fp -PathType Leaf)) {
            $bad += ('missing ' + $name)
            continue
        }
        $got = Get-FileSha256Hex $fp
        if ($got -ne $want) {
            $bad += ($name + ' (want ' + $want + ' got ' + $got + ')')
        }
    }
    if ($rowCount -eq 0) { throw 'hip SHA256SUMS contains no gfx1201 modules.' }
    if ($bad.Count -eq 0) {
        Write-Host '  modules match hip/SHA256SUMS gfx1201 entries' -ForegroundColor Green
        return $true
    }
    $msg = "Shipping modules do not match hip/SHA256SUMS gfx1201 recipes:`n  - " + ($bad -join "`n  - ")
    if ($allowStale) {
        Write-Warning $msg
        return $false
    }
    throw ($msg + "`nRebuild with hip/build-modules.ps1, pass a matching -ModulesPath, or use -AllowStaleModules.")
}

# hip/SHA256SUMS: non-gfx1201 rows follow upstream. gfx1201 rows are OURS: they hash the
# shipping modules built here with local COMGR; upstream's rows never match those bytes.
# With -modulesDir, gfx1201 rows are recomputed from that dir; otherwise the current local
# rows are kept. A module upstream lists but we lack keeps upstream's row, so the
# modules-vs-recipe check still reports it as missing.
function Merge-HipSums([string]$upstreamSums, [string]$dstSums, [string]$modulesDir) {
    $rowPattern = '^(?<h>[0-9a-fA-F]{64})(?<sep>\s+)gfx1201/(?<n>.+\.hsaco)$'
    $local = @{}
    if (Test-Path -LiteralPath $dstSums -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $dstSums -Encoding UTF8) {
            if ($line -match $rowPattern) { $local[$Matches['n']] = $Matches['h'].ToLowerInvariant() }
        }
    }
    $out = @()
    foreach ($line in Get-Content -LiteralPath $upstreamSums -Encoding UTF8) {
        if ($line -match $rowPattern) {
            $name = $Matches['n']
            $sep = $Matches['sep']
            $hash = $null
            if ($modulesDir) {
                $fp = Join-Path $modulesDir $name
                if (Test-Path -LiteralPath $fp -PathType Leaf) { $hash = Get-FileSha256Hex $fp }
            } elseif ($local.ContainsKey($name)) {
                $hash = $local[$name]
            }
            if ($hash) { $line = $hash + $sep + 'gfx1201/' + $name }
        }
        $out += $line
    }
    [IO.File]::WriteAllText($dstSums, (($out -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}

function Sync-LmxxfModules([string]$srcDir, [string]$dstDir, [string]$commitHash) {
    if (-not (Test-Path -LiteralPath $dstDir)) {
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    }
    $hsacos = @(Get-ChildItem -LiteralPath $srcDir -Filter '*.hsaco' -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer })
    if ($hsacos.Count -lt 1) {
        throw ('No .hsaco files found in modules source: ' + $srcDir)
    }
    $inputSums = Join-Path $srcDir 'SHA256SUMS'
    if (Test-Path -LiteralPath $inputSums -PathType Leaf) {
        $listed = @{}
        foreach ($line in Get-Content -LiteralPath $inputSums -Encoding UTF8) {
            if (-not $line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
            if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+\*?(?<n>[^/\\]+\.hsaco)$') {
                throw "Invalid ModulesPath SHA256SUMS row: $line"
            }
            $name = $Matches['n']; $expected = $Matches['h']
            if ($listed.ContainsKey($name)) { throw "Duplicate ModulesPath checksum: $name" }
            $listed[$name] = $true
            $file = Assert-SyncPath (Join-Path $srcDir $name) $srcDir
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-FileSha256Hex $file) -ne $expected) {
                throw "ModulesPath checksum mismatch: $name"
            }
        }
        foreach ($file in $hsacos) {
            if (-not $listed.ContainsKey($file.Name)) { throw "ModulesPath SHA256SUMS omits $($file.Name)" }
        }
    }
    Sync-FlatFiles -Source $srcDir -Destination $dstDir -Filter '*.hsaco'
    foreach ($extra in @('modules.json', 'runtime-manifest.json')) {
        $srcExtra = Join-Path $srcDir $extra
        if (Test-Path -LiteralPath $srcExtra -PathType Leaf) {
            Copy-Item -LiteralPath $srcExtra -Destination (Join-Path $dstDir $extra) -Force
        }
    }
    $sumsDst = Join-Path $dstDir 'SHA256SUMS'
    $rows = @($hsacos | Sort-Object Name | ForEach-Object {
        (Get-FileSha256Hex (Join-Path $dstDir $_.Name)) + '  ' + $_.Name
    })
    [IO.File]::WriteAllText($sumsDst, (($rows -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
    $readme = Join-Path $dstDir 'README.md'
    if (Test-Path -LiteralPath $readme -PathType Leaf) {
        $md = Get-Content -LiteralPath $readme -Encoding UTF8 -Raw
        $md2 = [regex]::Replace($md, '(?m)^(- \*\*Commit Base\*\*: `)[^`]+(`)', '${1}' + $commitHash + '${2}')
        if ($md2 -eq $md) {
            $md2 = [regex]::Replace($md, '(?m)^(- \*\*Commit Base\*\*: ).*$', '${1}`' + $commitHash + '`')
        }
        if ($md2 -ne $md) {
            [IO.File]::WriteAllText($readme, $md2, [Text.UTF8Encoding]::new($false))
        }
    }
    $manifest = Join-Path $dstDir 'runtime-manifest.json'
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        $js = Get-Content -LiteralPath $manifest -Encoding UTF8 -Raw
        $js2 = [regex]::Replace($js, '("upstream_commit"\s*:\s*")[0-9a-fA-F]*(")', '${1}' + $commitHash + '${2}')
        if ($js2 -ne $js) {
            [IO.File]::WriteAllText($manifest, $js2, [Text.UTF8Encoding]::new($false))
        }
    }
    Write-Host ('  Synchronized modules (' + $hsacos.Count + ' .hsaco) from ' + $srcDir) -ForegroundColor Green
}


