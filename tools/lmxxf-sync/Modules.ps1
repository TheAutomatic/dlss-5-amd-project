# lmxxf module build / sync / hashing helpers.

function Get-FileSha256Hex([string]$filePath) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [IO.File]::OpenRead($filePath)
    try {
        return (-join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }))
    } finally {
        $fs.Dispose()
        $sha.Dispose()
    }
}

function Get-TreeFingerprint([string]$dir, [string[]]$filters = @('*.hsaco')) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return 'missing' }
    $resolvedDir = (Resolve-Path -LiteralPath $dir).Path.TrimEnd('\', '/')
    $prefix = $resolvedDir + [IO.Path]::DirectorySeparatorChar
    $fileList = [System.Collections.Generic.List[psobject]]::new()
    foreach ($f in $filters) {
        $found = @(Get-ChildItem -LiteralPath $dir -Filter $f -File -Recurse -ErrorAction SilentlyContinue)
        foreach ($file in $found) {
            $full = [IO.Path]::GetFullPath($file.FullName)
            if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Path traversal in module fingerprint: $($file.FullName)"
            }
            $rel = $full.Substring($prefix.Length).Replace('\', '/').ToLowerInvariant()
            if ($rel -match '(^|/)\.\.(/|$)') {
                throw "Invalid traversal in module fingerprint: $rel"
            }
            $fileList.Add([pscustomobject]@{
                RelPath = $rel
                Length  = $file.Length
                FullName = $file.FullName
            })
        }
    }
    $files = @($fileList | Sort-Object { $_.RelPath } -Unique)
    if ($files.Count -lt 1) { return 'empty' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $ms = New-Object IO.MemoryStream
    try {
        foreach ($file in $files) {
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($file.RelPath + ':' + $file.Length + ':')
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
    if (-not $override) { return $null }
    if (-not (Test-Path -LiteralPath $override -PathType Container)) {
        throw ("ModulesPath not found: " + $override)
    }
    return (Resolve-Path -LiteralPath $override).Path
}

function Invoke-BuildModules([string]$hipDir, [string]$outDir, [string[]]$targets = @('gfx1200', 'gfx1201')) {
    $buildPs1 = Join-Path $hipDir 'build-modules.ps1'
    if (-not (Test-Path -LiteralPath $buildPs1 -PathType Leaf)) {
        throw ("Missing " + $buildPs1 + "; cannot build shipping .hsaco")
    }
    $compiler = Join-Path $hipDir 'rtc_compile.exe'
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
    if (-not (Get-Command Get-FileHash -ErrorAction SilentlyContinue)) {
        function global:Get-FileHash {
            param(
                [Parameter(Position = 0)]$Path,
                [Parameter(Position = 1)]$Second,
                $File,
                $LiteralPath,
                $Algorithm,
                [Parameter(ValueFromRemainingArguments = $true)]$Rest
            )
            foreach ($cand in (@($Path, $Second, $File, $LiteralPath) + @($Rest))) {
                if ($cand -is [string] -and (Test-Path -LiteralPath $cand -PathType Leaf)) {
                    return [pscustomobject]@{ Hash = (Get-FileSha256Hex $cand); Path = $cand }
                }
            }
            throw ("Get-FileHash shim: no readable file among its arguments")
        }
        Write-Host "  Get-FileHash is not resolvable in this session (inherited PSModulePath?); shimmed from Get-FileSha256Hex" -ForegroundColor DarkYellow
    }
    $targetDesc = $targets -join ', '
    Write-Host ("  Building modules ($targetDesc) via build-modules.ps1 -> " + $outDir) -ForegroundColor Cyan
    try {
        $global:LASTEXITCODE = 0
        & $localRecipe -OutputDir $outDir -Compiler $compiler -SourceDir $hipDir -Targets $targets | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "Module recipe exited with $LASTEXITCODE" }
    } catch {
        throw ("build-modules.ps1 failed: " + $_.Exception.Message + " [at: " + $_.InvocationInfo.PositionMessage + "]")
    }
    $built = @(Get-ChildItem -LiteralPath $outDir -Recurse -Filter '*.hsaco' -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer })
    if ($built.Count -lt 1) {
        throw ("build-modules.ps1 produced no .hsaco under " + $outDir)
    }
    return $outDir
}

function Invoke-BuildGfx1201Modules([string]$hipDir, [string]$outDir, [string[]]$targets = @('gfx1200', 'gfx1201')) {
    return (Invoke-BuildModules -hipDir $hipDir -outDir $outDir -targets $targets)
}

function Assert-ModulesMatchHipSums([string]$modulesDir, [string]$hipSums, [switch]$allowStale) {
    if (-not (Test-Path -LiteralPath $hipSums -PathType Leaf)) {
        throw 'hip SHA256SUMS missing; cannot verify shipping modules.'
    }
    $bad = @()
    $rowCount = 0
    $rows1200 = 0
    $rows1201 = 0
    foreach ($line in Get-Content -LiteralPath $hipSums -Encoding UTF8) {
        if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+(?<arch>gfx1200|gfx1201)/(?<n>[^/\\]+\.hsaco)$') { continue }
        $rowCount++
        $arch = $Matches['arch']
        $name = $Matches['n']
        if ($arch -eq 'gfx1200') { $rows1200++ } else { $rows1201++ }
        if ($name -match '[/\\]' -or $name -eq '..') { throw 'Unsafe module path in SHA256SUMS' }
        $want = $Matches['h'].ToLowerInvariant()
        
        $fp = Join-Path (Join-Path $modulesDir $arch) $name
        if (-not (Test-Path -LiteralPath $fp -PathType Leaf) -and $arch -eq 'gfx1201') {
            $flatFp = Join-Path $modulesDir $name
            if (Test-Path -LiteralPath $flatFp -PathType Leaf) { $fp = $flatFp }
        }
        if (-not (Test-Path -LiteralPath $fp -PathType Leaf)) {
            $bad += ("missing $arch/$name")
            continue
        }
        $got = Get-FileSha256Hex $fp
        if ($got -ne $want) {
            $bad += ("$arch/$name (want $want got $got)")
        }
    }
    if ($rowCount -eq 0) { throw 'hip SHA256SUMS contains no gfx1200 or gfx1201 modules.' }
    if ($bad.Count -eq 0) {
        Write-Host "  modules match hip/SHA256SUMS entries (gfx1200: $rows1200, gfx1201: $rows1201)" -ForegroundColor Green
        return $true
    }
    $msg = "Shipping modules do not match hip/SHA256SUMS recipes:`n  - " + ($bad -join "`n  - ")
    if ($allowStale) {
        Write-Warning $msg
        return $false
    }
    throw ($msg + "`nRebuild with hip/build-modules.ps1, pass a matching -ModulesPath, or use -AllowStaleModules.")
}

function Merge-HipSums([string]$upstreamSums, [string]$dstSums, [string]$modulesDir) {
    $rowPattern = '^(?<h>[0-9a-fA-F]{64})(?<sep>\s+)(?<arch>gfx1200|gfx1201)/(?<n>.+\.hsaco)$'
    $local = @{}
    if (Test-Path -LiteralPath $dstSums -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $dstSums -Encoding UTF8) {
            if ($line -match $rowPattern) {
                $key = $Matches['arch'] + '/' + $Matches['n']
                $local[$key] = $Matches['h'].ToLowerInvariant()
            }
        }
    }
    $out = @()
    foreach ($line in Get-Content -LiteralPath $upstreamSums -Encoding UTF8) {
        if ($line -match $rowPattern) {
            $arch = $Matches['arch']
            $name = $Matches['n']
            $sep = $Matches['sep']
            $key = "$arch/$name"
            $hash = $null
            if ($modulesDir) {
                $fp = Join-Path (Join-Path $modulesDir $arch) $name
                if (-not (Test-Path -LiteralPath $fp -PathType Leaf) -and $arch -eq 'gfx1201') {
                    $flatFp = Join-Path $modulesDir $name
                    if (Test-Path -LiteralPath $flatFp -PathType Leaf) { $fp = $flatFp }
                }
                if (Test-Path -LiteralPath $fp -PathType Leaf) {
                    $hash = Get-FileSha256Hex $fp
                }
            } elseif ($local.ContainsKey($key)) {
                $hash = $local[$key]
            }
            if ($hash) { $line = $hash + $sep + $key }
        }
        $out += $line
    }
    [IO.File]::WriteAllText($dstSums, (($out -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}

function Sync-LmxxfModules([string]$srcDir, [string]$dstDir, [string]$commitHash) {
    if (-not (Test-Path -LiteralPath $dstDir)) {
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    }
    $archDirs = @(Get-ChildItem -LiteralPath $srcDir -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^gfx[0-9a-zA-Z]+$' })
    
    if ($archDirs.Count -gt 0) {
        # --- Dual / Multi-Architecture Layout ---
        $rootSums = Join-Path $srcDir 'SHA256SUMS'
        $rootListed = @{}
        if (Test-Path -LiteralPath $rootSums -PathType Leaf) {
            foreach ($line in Get-Content -LiteralPath $rootSums -Encoding UTF8) {
                if (-not $line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
                if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+\*?(?<rel>[^/\\]+[/\\][^/\\]+\.hsaco)$') {
                    if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+\*?(?<rel>[^/\\]+\.hsaco)$') {
                        throw "Invalid ModulesPath SHA256SUMS row: $line"
                    }
                }
                $relName = $Matches['rel'].Replace('\', '/')
                if ($relName -match '(^|/)\.\.(/|$)') { throw "Traversal in ModulesPath SHA256SUMS: $relName" }
                $expected = $Matches['h'].ToLowerInvariant()
                if ($rootListed.ContainsKey($relName)) { throw "Duplicate ModulesPath checksum: $relName" }
                $rootListed[$relName] = $expected
            }
        }
        
        # Verify each architecture leaf before touching dstDir
        $archModules = @{}
        foreach ($arch in $archDirs) {
            $archName = $arch.Name
            $hsacos = @(Get-ChildItem -LiteralPath $arch.FullName -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
            if ($hsacos.Count -lt 1) {
                throw ("No .hsaco files found in architecture directory: " + $arch.FullName)
            }
            $archModules[$archName] = $hsacos
            $leafSums = Join-Path $arch.FullName 'SHA256SUMS'
            $leafListed = @{}
            if (Test-Path -LiteralPath $leafSums -PathType Leaf) {
                foreach ($line in Get-Content -LiteralPath $leafSums -Encoding UTF8) {
                    if (-not $line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
                    if ($line -notmatch '^(?<h>[0-9a-fA-F]{64})\s+\*?(?<n>[^/\\]+\.hsaco)$') {
                        throw "Invalid ModulesPath leaf SHA256SUMS row in $($archName): $line"
                    }
                    $name = $Matches['n']
                    if ($name -match '(^|/)\.\.(/|$)') { throw "Traversal in leaf SHA256SUMS: $name" }
                    $expected = $Matches['h'].ToLowerInvariant()
                    if ($leafListed.ContainsKey($name)) { throw "Duplicate ModulesPath checksum: $name" }
                    $leafListed[$name] = $expected
                    $fp = Join-Path $arch.FullName $name
                    if (-not (Test-Path -LiteralPath $fp -PathType Leaf) -or (Get-FileSha256Hex $fp) -ne $expected) {
                        throw "ModulesPath checksum mismatch: $archName/$name"
                    }
                }
                foreach ($file in $hsacos) {
                    if (-not $leafListed.ContainsKey($file.Name)) {
                        throw "ModulesPath leaf SHA256SUMS in $archName omits $($file.Name)"
                    }
                }
            }
            foreach ($file in $hsacos) {
                $relKey = "$archName/$($file.Name)"
                if ($rootListed.ContainsKey($relKey)) {
                    $expectedRoot = $rootListed[$relKey]
                    $actual = Get-FileSha256Hex $file.FullName
                    if ($actual -ne $expectedRoot) {
                        throw "ModulesPath checksum mismatch against root manifest: $relKey"
                    }
                }
            }
        }
        
        # All checks passed! Now safely synchronize into dstDir
        Get-ChildItem -LiteralPath $dstDir -Filter '*.hsaco' -File -ErrorAction SilentlyContinue | Remove-Item -Force
        
        $allRootRows = [System.Collections.Generic.List[string]]::new()
        foreach ($arch in $archDirs) {
            $archName = $arch.Name
            $dstArchDir = Join-Path $dstDir $archName
            if (-not (Test-Path -LiteralPath $dstArchDir)) {
                New-Item -ItemType Directory -Force -Path $dstArchDir | Out-Null
            }
            Sync-FlatFiles -Source $arch.FullName -Destination $dstArchDir -Filter '*.hsaco'
            foreach ($extra in @('modules.json', 'runtime-manifest.json')) {
                $srcExtra = Join-Path $arch.FullName $extra
                if (Test-Path -LiteralPath $srcExtra -PathType Leaf) {
                    Copy-Item -LiteralPath $srcExtra -Destination (Join-Path $dstArchDir $extra) -Force
                }
            }
            $leafHsacos = @(Get-ChildItem -LiteralPath $dstArchDir -Filter '*.hsaco' -File -ErrorAction SilentlyContinue)
            $leafRows = @($leafHsacos | Sort-Object Name | ForEach-Object {
                $h = Get-FileSha256Hex $_.FullName
                $allRootRows.Add("$h  $archName/$($_.Name)")
                "$h  $($_.Name)"
            })
            [IO.File]::WriteAllText((Join-Path $dstArchDir 'SHA256SUMS'), (($leafRows -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
        }
        
        $sortedRootRows = @($allRootRows | Sort-Object)
        [IO.File]::WriteAllText((Join-Path $dstDir 'SHA256SUMS'), (($sortedRootRows -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
        
        foreach ($extra in @('modules.json', 'runtime-manifest.json')) {
            $srcExtra = Join-Path $srcDir $extra
            if (Test-Path -LiteralPath $srcExtra -PathType Leaf) {
                Copy-Item -LiteralPath $srcExtra -Destination (Join-Path $dstDir $extra) -Force
            }
        }
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
        Write-Host ('  Synchronized dual-architecture modules (' + $allRootRows.Count + ' total .hsaco across ' + $archDirs.Count + ' targets) from ' + $srcDir) -ForegroundColor Green
    } else {
        # --- Flat Modules Layout (Single Target / Legacy) ---
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
                $name = $Matches['n']; $expected = $Matches['h'].ToLowerInvariant()
                if ($name -match '(^|/)\.\.(/|$)') { throw "Traversal in ModulesPath SHA256SUMS: $name" }
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
}
