# Shared shipping-module contract for Setup, packaging, staging and upstream sync.
# Keep the names aligned with LmxxfNrRuntime.cpp; tests exercise both validators.
function Get-LmxxfModuleNames {
    @(
        'boundary-fast.hsaco', 'boundary_reference.hsaco', 'c32_fast.hsaco',
        'c32_fast_attention.hsaco', 'c32_fused_attention.hsaco',
        'c32_fused_ffn_attention-packed.hsaco', 'c32_fused_ffn_attention.hsaco',
        'c32_prefix_reference.hsaco', 'c32_tiled.hsaco', 'c32_wmma.hsaco',
        'deep_fast-packed.hsaco', 'deep_fast.hsaco', 'deep_reference.hsaco', 'deep_wmma.hsaco',
        'multihead-fast-packed.hsaco', 'multihead-fast-padded-wave-packed.hsaco',
        'multihead-fast-padded-wave.hsaco', 'multihead-fast.hsaco', 'multihead-reference.hsaco',
        'multihead-tiled.hsaco', 'multihead-wmma.hsaco', 'multihead_fused_attention.hsaco',
        'prefix_fast.hsaco', 'wave-pointwise.hsaco'
    )
}

function Get-LmxxfSha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

function Assert-LmxxfUnlinkedPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -gt [IO.Path]::GetPathRoot($full).Length) { $full = $full.TrimEnd('\', '/') }
    $cursor = $full
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing linked module path (symlink/junction/reparse point): $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\', '/'))
    }
    return $full
}

function Get-LmxxfUnlinkedFiles([string]$Directory) {
    $full = Assert-LmxxfUnlinkedPath $Directory
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Missing module directory: $full" }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($full)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing linked module path (symlink/junction/reparse point): $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $item }
        }
    }
}

function Read-LmxxfModuleSums([string]$Path, [string]$Arch = '') {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing module SHA256SUMS: $Path" }
    if ((Get-Item -LiteralPath $Path).Length -gt 1MB) { throw "Oversized module SHA256SUMS: $Path" }
    $known = @(Get-LmxxfModuleNames)
    $rows = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})[ \t]+\*?(.+?)\s*$') { throw "Invalid module SHA256SUMS row in ${Path}: $line" }
        $hash = $Matches[1].ToLowerInvariant()
        $name = $Matches[2].Replace('\', '/')
        if ($name.StartsWith('./')) { $name = $name.Substring(2) }
        if ($Arch) {
            if ($name.StartsWith($Arch + '/')) { $name = $name.Substring($Arch.Length + 1) }
            if ($known -cnotcontains $name) { throw "Unknown or unsafe module name in ${Path}: $name" }
        } else {
            $parts = $name.Split('/')
            if ($parts.Count -ne 2 -or @('gfx1200', 'gfx1201') -cnotcontains $parts[0] -or $known -cnotcontains $parts[1]) {
                throw "Unknown or unsafe module path in ${Path}: $name"
            }
        }
        if ($rows.ContainsKey($name)) { throw "Duplicate module checksum in ${Path}: $name" }
        $rows[$name] = $hash
    }
    $expectedCount = if ($Arch) { 24 } else { 48 }
    if ($rows.Count -ne $expectedCount) { throw "Incomplete module SHA256SUMS in $Path (expected $expectedCount entries, got $($rows.Count))" }
    return $rows
}

function Assert-LmxxfModulePackage([string]$Directory, [switch]$BuildOutput) {
    $full = Assert-LmxxfUnlinkedPath $Directory
    $prefix = $full.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $files = @(Get-LmxxfUnlinkedFiles $full)
    $known = @(Get-LmxxfModuleNames)
    $arches = @('gfx1200', 'gfx1201')
    $archDirs = @(Get-ChildItem -LiteralPath $full -Directory -Force | Where-Object { $_.Name -match '^gfx' })
    if ($archDirs.Count -ne 2 -or @($archDirs | Where-Object { $arches -cnotcontains $_.Name }).Count) {
        throw 'Module package requires both gfx1200 and gfx1201 architecture directories, and no other gfx targets.'
    }
    if (Test-Path -LiteralPath (Join-Path $full 'modules.json')) { throw 'Legacy root modules.json is not allowed in a dual-architecture package.' }
    $hsacos = @($files | Where-Object { $_.Extension -ieq '.hsaco' })
    foreach ($file in $hsacos) {
        $rel = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        $parts = $rel.Split('/')
        if ($parts.Count -ne 2 -or $arches -cnotcontains $parts[0] -or $known -cnotcontains $parts[1]) {
            throw "Unknown or misplaced .hsaco module: $rel"
        }
    }
    if ($hsacos.Count -ne 48) { throw "Module package must contain exactly 48 known .hsaco modules; got $($hsacos.Count)." }
    $rootSums = Read-LmxxfModuleSums (Join-Path $full 'SHA256SUMS')
    foreach ($arch in $arches) {
        $leafDir = Join-Path $full $arch
        $leafSums = Read-LmxxfModuleSums (Join-Path $leafDir 'SHA256SUMS') $arch
        foreach ($name in $known) {
            $key = "$arch/$name"
            if (-not $rootSums.ContainsKey($key) -or -not $leafSums.ContainsKey($name) -or $rootSums[$key] -ne $leafSums[$name]) {
                throw "Module leaf SHA256SUMS mismatch with root: $key"
            }
            if ((Get-LmxxfSha256 (Join-Path $leafDir $name)) -ne $rootSums[$key]) { throw "Module checksum mismatch: $key" }
        }
        $metadataPath = Join-Path $leafDir 'modules.json'
        $metadata = [IO.File]::ReadAllText($metadataPath) | ConvertFrom-Json
        $seen = @{}
        foreach ($entry in $metadata) {
            $name = [string]$entry.module + '.hsaco'
            if ($entry.target -cne $arch -or $known -cnotcontains $name -or $seen.ContainsKey($name) -or
                [string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $entry.sha256 -ine $leafSums[$name]) {
                throw "Invalid or inconsistent modules.json entry in ${arch}: $name"
            }
            $seen[$name] = $true
        }
        if ($seen.Count -ne 24) { throw "Incomplete modules.json for $arch (expected 24 modules)." }
    }
    $manifestPath = Join-Path $full 'runtime-manifest.json'
    # Upstream build-modules.ps1 emits hashes + per-arch provenance. Product metadata
    # is added by Sync-LmxxfModules; release/install inputs must already contain it.
    if (-not $BuildOutput -or (Test-Path -LiteralPath $manifestPath)) {
        $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        if ($manifest.schema -ne 2 -or $manifest.runtime_abi -ne 1 -or $manifest.module_count -ne 48 -or
            $manifest.module_count_per_arch -ne 24 -or @($manifest.targets).Count -ne 2 -or
            @($manifest.targets | Where-Object { $arches -cnotcontains $_ }).Count -or
            @($manifest.targets | Select-Object -Unique).Count -ne 2) {
            throw 'Invalid dual-architecture runtime-manifest.json (schema, ABI, targets or module counts).'
        }
    }
}

function Assert-LmxxfModuleDestination([string]$Directory, [switch]$AllowLegacy) {
    $null = Assert-LmxxfUnlinkedPath $Directory
    if (-not (Test-Path -LiteralPath $Directory)) { return }
    $null = @(Get-LmxxfUnlinkedFiles $Directory)
    if ($AllowLegacy) { return }
    if (@(Get-ChildItem -LiteralPath $Directory -File -Force -Filter '*.hsaco').Count -or
        (Test-Path -LiteralPath (Join-Path $Directory 'modules.json'))) {
        throw ('Legacy flat lmxxf-modules detected. Uninstall the old installation first, then run Setup again. ' +
            'Use the uninstaller from the NEW package with the game folder argument. ' +
            'Move any remaining user-owned .hsaco files outside lmxxf-modules; they will not be deleted automatically.')
    }
}

function Remove-LmxxfTemporaryTree([string]$Path, [string]$Parent) {
    $base = Assert-LmxxfUnlinkedPath $Parent
    $full = Assert-LmxxfUnlinkedPath $Path
    if ([IO.Path]::GetDirectoryName($full) -ine $base -or [IO.Path]::GetFileName($full) -notmatch '^\.lmxxf-(stage|previous)-[0-9a-f]{32}$') {
        throw "Refusing cleanup outside the temporary module owner: $full"
    }
    if (Test-Path -LiteralPath $full) {
        $null = @(Get-LmxxfUnlinkedFiles $full)
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
    }
}

function New-LmxxfModuleStage([string]$Source, [string]$Destination, [string]$CommitHash = '', [switch]$Upgrade) {
    Assert-LmxxfModulePackage $Source -BuildOutput:([bool]$CommitHash)
    $Destination = Assert-LmxxfUnlinkedPath $Destination
    Assert-LmxxfModuleDestination $Destination -AllowLegacy:$Upgrade
    $parent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Destination))
    $stage = Join-Path $parent ('.lmxxf-stage-' + [guid]::NewGuid().ToString('N'))
    try {
        [void][IO.Directory]::CreateDirectory($stage)
        if (Test-Path -LiteralPath $Destination) {
            if ($Upgrade) {
                # Setup replaces the module set as a whole. Keep other user files
                # active; all former modules (including custom hsaco) remain in the
                # required backup when Publish-LmxxfModuleStage switches directories.
                $prefix = $Destination.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
                foreach ($file in Get-LmxxfUnlinkedFiles $Destination) {
                    $rel = $file.FullName.Substring($prefix.Length)
                    $top = ($rel -split '[\\/]')[0]
                    if ($file.Extension -ieq '.hsaco' -or $rel -ieq 'modules.json' -or
                        ($top -match '^gfx' -and @('gfx1200', 'gfx1201') -cnotcontains $top)) { continue }
                    $target = Join-Path $stage $rel
                    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                    Copy-Item -LiteralPath $file.FullName -Destination $target -Force -ErrorAction Stop
                }
            } else {
                # Developer staging and upstream sync retain their strict destination contract.
                Get-ChildItem -LiteralPath $Destination -Force | ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination $stage -Recurse -Force -ErrorAction Stop
                }
            }
        }
        $paths = @('SHA256SUMS', 'runtime-manifest.json', 'README.md')
        foreach ($arch in @('gfx1200', 'gfx1201')) {
            [void][IO.Directory]::CreateDirectory((Join-Path $stage $arch))
            $paths += @('SHA256SUMS', 'modules.json', 'README.md') + @(Get-LmxxfModuleNames) | ForEach-Object { "$arch/$_" }
        }
        foreach ($rel in $paths) {
            $src = Join-Path $Source $rel
            if (Test-Path -LiteralPath $src -PathType Leaf) {
                Copy-Item -LiteralPath $src -Destination (Join-Path $stage $rel) -Force -ErrorAction Stop
            }
        }
        if ($CommitHash) {
            $manifestPath = Join-Path $stage 'runtime-manifest.json'
            $manifest = if (Test-Path -LiteralPath $manifestPath) {
                [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
            } else {
                [pscustomobject]@{ schema = 2; runtime_abi = 1; targets = @('gfx1200', 'gfx1201'); module_count = 48; module_count_per_arch = 24 }
            }
            $manifest | Add-Member -NotePropertyName upstream_commit -NotePropertyValue $CommitHash -Force
            [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
            $readme = Join-Path $stage 'README.md'
            if (Test-Path -LiteralPath $readme -PathType Leaf) {
                $md = [IO.File]::ReadAllText($readme)
                $md = [regex]::Replace($md, '(?m)^(- \*\*Commit Base\*\*: ).*$', ('${1}' + [char]96 + $CommitHash + [char]96))
                [IO.File]::WriteAllText($readme, $md, [Text.UTF8Encoding]::new($false))
            }
        }
        Assert-LmxxfModulePackage $stage
        return $stage
    } catch {
        Remove-LmxxfTemporaryTree $stage $parent
        throw
    }
}

function Publish-LmxxfModuleStage([string]$Stage, [string]$Destination, [string]$Backup = '', [switch]$Upgrade) {
    $dest = Assert-LmxxfUnlinkedPath $Destination
    $parent = [IO.Path]::GetDirectoryName($dest)
    $staged = Assert-LmxxfUnlinkedPath $Stage
    if ([IO.Path]::GetDirectoryName($staged) -ine $parent -or [IO.Path]::GetFileName($staged) -notmatch '^\.lmxxf-stage-[0-9a-f]{32}$') {
        throw 'Module staging directory must be a private sibling of its destination.'
    }
    if ($Upgrade -and -not $Backup) { throw 'Upgrading installed modules requires a permanent backup path.' }
    Assert-LmxxfModuleDestination $dest -AllowLegacy:$Upgrade
    Assert-LmxxfModulePackage $staged
    $previous = if ($Backup) { Assert-LmxxfUnlinkedPath $Backup } else { Join-Path $parent ('.lmxxf-previous-' + [guid]::NewGuid().ToString('N')) }
    if (-not $previous.StartsWith($parent.TrimEnd('\', '/') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $previous)) { throw "Unsafe or occupied module backup path: $previous" }
    $moved = $false
    try {
        if (Test-Path -LiteralPath $dest) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($previous))
            [IO.Directory]::Move($dest, $previous)
            $moved = $true
        }
        [IO.Directory]::Move($staged, $dest)
    } catch {
        if ($moved) {
            try { [IO.Directory]::Move($previous, $dest) }
            catch { throw "Module switch and rollback failed. Previous modules remain at $previous; restore them before retrying. $($_.Exception.Message)" }
        }
        throw
    }
    if ($moved -and -not $Backup) {
        try { Remove-LmxxfTemporaryTree $previous $parent }
        catch { Write-Warning "New module package is active; old files retained at $previous : $($_.Exception.Message)" }
    }
}
