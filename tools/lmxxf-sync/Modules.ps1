# lmxxf module build / sync / hashing helpers.
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lmxxf-module-package.ps1')

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
            $payload = [IO.File]::ReadAllBytes($file.FullName)
            # A UTF-8 BOM is required for non-ASCII PowerShell on Windows 5.1, but it
            # does not change a build recipe. Normalize only this encoding marker;
            # every other script byte, HIP source byte and binary byte still counts.
            if ($file.RelPath.EndsWith('.ps1') -and $payload.Length -ge 3 -and
                $payload[0] -eq 0xef -and $payload[1] -eq 0xbb -and $payload[2] -eq 0xbf) {
                if ($payload.Length -gt 3) { $payload = [byte[]]$payload[3..($payload.Length - 1)] }
                else { $payload = [byte[]]::new(0) }
            }
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($file.RelPath + ':' + $payload.Length + ':')
            $ms.Write($nameBytes, 0, $nameBytes.Length)
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

function Complete-LmxxfCompilerProvenance([string]$outDir, [string]$recipePath, [string]$prebuiltDir, [string[]]$targets) {
    $recipeText = [IO.File]::ReadAllText($recipePath)
    $llvmRows = @{}
    foreach ($match in [regex]::Matches($recipeText, "(?m)@\{ name = '([^']+)';[^\r\n]*compiler = 'llvm23'(?:; l23defines = @\(([^)]*)\))? \}")) {
        $llvmRows[$match.Groups[1].Value] = @([regex]::Matches($match.Groups[2].Value, "'([^']+)'") | ForEach-Object { $_.Groups[1].Value })
    }
    $llvm = $null
    if ($llvmRows.Count) {
        $llvm = [IO.File]::ReadAllText((Join-Path $prebuiltDir 'manifest.json')) | ConvertFrom-Json
        if ($llvm.compiler -notmatch 'clang version 23\.1\.2\b') { throw 'LLVM prebuild must record clang 23.1.2.' }
    }
    foreach ($target in $targets) {
        $leaf = Join-Path $outDir $target
        $metadataPath = Join-Path $leaf 'modules.json'
        $metadata = [IO.File]::ReadAllText($metadataPath) | ConvertFrom-Json
        foreach ($entry in $metadata) {
            $sourcePath = Join-Path $leaf ($entry.module + '.generated.hip')
            $source = [IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
            if ($llvmRows.ContainsKey($entry.module)) {
                $rows = @($llvm.modules | Where-Object { $_.target -ceq $target -and $_.module -ceq $entry.module })
                if ($rows.Count -ne 1) { throw "Missing/duplicate LLVM provenance: $target/$($entry.module)" }
                $row = $rows[0]
                $expectedDefines = @($entry.defines -split '; ') + $llvmRows[$entry.module]
                if (@(Compare-Object $expectedDefines @($row.defines) -SyncWindow 0 -CaseSensitive).Count) { throw "LLVM defines mismatch: $target/$($entry.module)" }
                if (@($row.commands).Count -lt 4 -or $row.commands[0] -cnotcontains '-real-true16' -or $row.commands[1] -cnotcontains '-real-true16') {
                    throw "LLVM prebuild must disable real-true16 in both stages: $target/$($entry.module)"
                }
                foreach ($option in ([string]$entry.opts -replace '^llvm23 prebuilt\s*', '').Split(' ')) {
                    if ($option.StartsWith('-mllvm=') -and $row.commands[1] -cnotcontains $option.Substring(7)) {
                        throw "LLVM row option missing from prebuild: $target/$($entry.module): $option"
                    }
                }
                $prefix = (@($entry.defines -split '; ') | ForEach-Object { '#define ' + $_ + "`n" }) -join ''
                if (-not $source.StartsWith($prefix, [StringComparison]::Ordinal)) { throw "Generated source prefix mismatch: $($entry.module)" }
                $llvmPrefix = ($expectedDefines | ForEach-Object { '#define ' + $_ + "`n" }) -join ''
                $source = $llvmPrefix + $source.Substring($prefix.Length)
                $sha = [Security.Cryptography.SHA256]::Create()
                try { $sourceHash = (-join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)) | ForEach-Object { $_.ToString('x2') })) }
                finally { $sha.Dispose() }
                if ($sourceHash -cne $row.source_sha256 -or $entry.sha256 -ine $row.sha256) { throw "LLVM source/object mismatch: $target/$($entry.module)" }
                $entry.defines = $expectedDefines -join '; '
                $entry | Add-Member NoteProperty compiler_version $llvm.compiler
                $entry | Add-Member NoteProperty commands $row.commands
                [IO.File]::WriteAllText($sourcePath, $source, [Text.UTF8Encoding]::new($false))
            }
            $entry | Add-Member NoteProperty source_sha256 (Get-FileSha256Hex $sourcePath)
        }
        [IO.File]::WriteAllText($metadataPath, (ConvertTo-Json -InputObject @($metadata) -Depth 8), [Text.UTF8Encoding]::new($false))
    }
}

function Invoke-BuildModules([string]$hipDir, [string]$outDir, [string[]]$targets = @('gfx1200', 'gfx1201'), [string]$prebuiltDir = '') {
    $buildPs1 = Join-Path $hipDir 'build-modules.ps1'
    if (-not (Test-Path -LiteralPath $buildPs1 -PathType Leaf)) {
        throw ("Missing " + $buildPs1 + "; cannot build shipping .hsaco")
    }
    # Use the author's per-row compiler/options once present in the production recipe.
    # Never silently replace reviewed LLVM rows with the default COMGR build.
    $rowOpts = [IO.File]::ReadAllText($buildPs1) -match "; (opts|compiler) = '"
    if ([IO.File]::ReadAllText($buildPs1) -match "; compiler = 'llvm23'") {
        if (-not $prebuiltDir) { $prebuiltDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'exports/lmxxf-llvm23' }
        if (-not (Test-Path -LiteralPath (Join-Path $prebuiltDir 'manifest.json') -PathType Leaf)) {
            throw 'LLVM23 rows require -LlvmPrebuiltDir containing the original compile-modules.py manifest.json and dual-architecture prebuilds; see docs/lmxxf-040-consumer-review.md.'
        }
    }
    $compiler = Join-Path $hipDir 'rtc_compile.exe'
    $rtcCpp = Join-Path $hipDir 'rtc_compile.cpp'
    # The upstream compiler source is synchronized on every run. Never reuse a stale EXE.
    if (-not (Test-Path -LiteralPath $rtcCpp -PathType Leaf)) {
        throw ("Missing rtc_compile.cpp under " + $hipDir)
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $msvcCl = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64\cl.exe'
        if (Test-Path -LiteralPath $msvcCl -PathType Leaf) {
            $env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64;$env:PATH"
            $env:INCLUDE = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.44.35207\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared;$env:INCLUDE"
            $env:LIB = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.44.35207\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64;$env:LIB"
        }
    }
    Write-Host "  Building rtc_compile.exe from rtc_compile.cpp..." -ForegroundColor Cyan
    $rtcObj = Join-Path $hipDir 'rtc_compile.obj'
    & cl.exe /nologo /O2 /EHsc /Fe:$compiler /Fo:$rtcObj $rtcCpp | Write-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        throw "Failed to build rtc_compile.exe (need MSVC cl in PATH)"
    }
    if (Test-Path -LiteralPath $outDir) {
        $exports = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'exports'
        $owner = $hipDir
        if ([IO.Path]::GetFullPath($outDir).StartsWith([IO.Path]::GetFullPath($exports) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { $owner = $exports }
        Remove-SyncTree -Path $outDir -Within $owner
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
        $recipeArgs = @{ OutputDir = $outDir; Compiler = $compiler; SourceDir = $hipDir; Targets = $targets }
        if ($rowOpts) { $recipeArgs.RowOpts = $true; $recipeArgs.PrebuiltDir = $prebuiltDir }
        & $localRecipe @recipeArgs | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "Module recipe exited with $LASTEXITCODE" }
        if ($rowOpts) { Complete-LmxxfCompilerProvenance $outDir $localRecipe $prebuiltDir $targets }
    } catch {
        throw ("build-modules.ps1 failed: " + $_.Exception.Message + " [at: " + $_.InvocationInfo.PositionMessage + "]")
    }
    $built = @(Get-ChildItem -LiteralPath $outDir -Recurse -Filter '*.hsaco' -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer })
    if ($built.Count -lt 1) {
        throw ("build-modules.ps1 produced no .hsaco under " + $outDir)
    }
    return $outDir
}

function Invoke-BuildGfx1201Modules([string]$hipDir, [string]$outDir, [string[]]$targets = @('gfx1200', 'gfx1201'), [string]$prebuiltDir = '') {
    return (Invoke-BuildModules -hipDir $hipDir -outDir $outDir -targets $targets -prebuiltDir $prebuiltDir)
}

function Assert-ModulesMatchHipSums([string]$modulesDir, [string]$hipSums, [switch]$allowStale) {
    # A stale-recipe waiver must never waive a corrupt/incomplete shipping package.
    Assert-LmxxfModulePackage $modulesDir
    $package = Read-LmxxfModuleSums (Join-Path $modulesDir 'SHA256SUMS')
    $recipes = Read-LmxxfModuleSums $hipSums
    $bad = @($package.Keys | Where-Object { -not $recipes.ContainsKey($_) -or $package[$_] -ne $recipes[$_] })
    if ($bad.Count -eq 0) {
        Write-Host '  Complete dual-architecture package matches hip/SHA256SUMS.' -ForegroundColor Green
        return $true
    }
    $msg = 'Shipping modules do not match hip/SHA256SUMS recipes: ' + ($bad -join ', ')
    if ($allowStale) { Write-Warning $msg; return $false }
    throw ($msg + '. Rebuild, pass matching -ModulesPath, or explicitly use -AllowStaleModules.')
}

function Merge-HipSums([string]$upstreamSums, [string]$dstSums, [string]$modulesDir) {
    $rowPattern = '^(?<h>[0-9a-fA-F]{64})(?<sep>\s+)(?<arch>gfx1200|gfx1201)/(?<n>.+\.hsaco)$'
    $local = @{}
    if ($modulesDir) {
        Assert-LmxxfModulePackage $modulesDir
        $local = Read-LmxxfModuleSums (Join-Path $modulesDir 'SHA256SUMS')
    } elseif (Test-Path -LiteralPath $dstSums -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $dstSums -Encoding UTF8) {
            if ($line -match $rowPattern) { $local[$Matches['arch'] + '/' + $Matches['n']] = $Matches['h'].ToLowerInvariant() }
        }
    }
    $out = @()
    $seen = @{}
    foreach ($line in Get-Content -LiteralPath $upstreamSums -Encoding UTF8) {
        if ($line -match $rowPattern) {
            $key = $Matches['arch'] + '/' + $Matches['n']
            $seen[$key] = $true
            if ($local.ContainsKey($key)) { $line = $local[$key] + '  ' + $key }
            elseif ($modulesDir) { continue } # dropped from local recipe
        }
        $out += $line
    }
    # 0.33+ recipe can ship modules that older hip/SHA256SUMS still lacks.
    if ($modulesDir) {
        foreach ($key in ($local.Keys | Sort-Object)) {
            if (-not $seen.ContainsKey($key)) { $out += ($local[$key] + '  ' + $key) }
        }
    }
    [IO.File]::WriteAllLines($dstSums, $out, [Text.UTF8Encoding]::new($false))
}

function Sync-LmxxfModules([string]$srcDir, [string]$dstDir, [string]$commitHash) {
    # Validate both architectures and provenance before creating or changing the destination.
    # Upstream build outputs omit product metadata; staging supplies it before final validation.
    $stage = New-LmxxfModuleStage $srcDir $dstDir $commitHash
    try {
        Publish-LmxxfModuleStage $stage $dstDir
    } finally {
        Remove-LmxxfTemporaryTree $stage ([IO.Path]::GetDirectoryName($stage))
    }
    Write-Host ('  Synchronized verified dual-architecture modules from ' + $srcDir) -ForegroundColor Green
}
