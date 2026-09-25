# File ownership and patch application. Every destructive operation is confined to its owner.
function Assert-SyncPath([string]$Path, [string]$Within) {
    $base = [IO.Path]::GetFullPath($Within).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes sync owner '$base': $full"
    }
    $cursor = $full
    while ($cursor -and $cursor.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing sync through a link/junction: $cursor"
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
    return $full
}

function Remove-SyncTree([string]$Path, [string]$Within) {
    $full = Assert-SyncPath $Path $Within
    if (-not (Test-Path -LiteralPath $full)) { return }
    foreach ($entry in Get-ChildItem -LiteralPath $full -Recurse -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Link in cleanup tree: $($entry.FullName)" }
    }
    Remove-Item -LiteralPath $full -Recurse -Force
}

function Sync-FlatFiles([string]$Source, [string]$Destination, [string]$Filter) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "Missing source directory: $Source" }
    $files = @(Get-ChildItem -LiteralPath $Source -File -Filter $Filter)
    if ($files.Count -eq 0) { throw "No $Filter in $Source; refusing an empty mirror" }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $names = @{}
    foreach ($file in $files) {
        $src = Assert-SyncPath $file.FullName $Source
        $dst = Assert-SyncPath (Join-Path $Destination $file.Name) $Destination
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $names[$file.Name] = $true
    }
    foreach ($file in Get-ChildItem -LiteralPath $Destination -File -Filter $Filter) {
        if (-not $names.ContainsKey($file.Name)) {
            $path = Assert-SyncPath $file.FullName $Destination
            Remove-Item -LiteralPath $path -Force
            Write-Host "  Removed retired file: $path"
        }
    }
}

function Assert-LocalHeader($Spec, [string]$Tree) {
    $path = Join-Path $Tree $Spec.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing pinned header: $path (use -$($Spec.switch))" }
    $content = Get-Content -LiteralPath $path -Encoding UTF8 -Raw
    foreach ($needle in $Spec.required) {
        if (-not $content.Contains($needle)) { throw "Local contract missing '$needle' in $path; review $($Spec.patch)" }
    }
    foreach ($pattern in $Spec.patterns) {
        if ($content -notmatch $pattern) { throw "Local contract pattern missing in $path; review $($Spec.patch): $pattern" }
    }
    foreach ($pattern in $Spec.forbidden) {
        if ($content -match $pattern) { throw "Forbidden include/contract in $path; review $($Spec.patch): $pattern" }
    }
}

function Invoke-LocalHeaderPatch([string]$Tree, [string]$Patch) {
    if (-not (Test-Path -LiteralPath $Patch -PathType Leaf)) { throw "Missing local patch: $Patch" }
    & git -C $Tree apply --check --whitespace=nowarn $Patch
    if ($LASTEXITCODE -ne 0) { throw "Local patch no longer applies: $Patch. Rebase this patch against the selected upstream commit; vendor files have not been copied." }
    & git -C $Tree apply --whitespace=nowarn $Patch
    if ($LASTEXITCODE -ne 0) { throw "Failed applying local patch: $Patch" }
}

function Read-SyncJson([string]$Path) {
    Get-Content -LiteralPath $Path -Encoding UTF8 -Raw | ConvertFrom-Json
}

function Write-SyncJson([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 12) + "`n"), [Text.UTF8Encoding]::new($false))
}

function Resolve-AuditPython([string]$Requested) {
    $candidates = if ($Requested) { @($Requested) } else { @('python', 'python3', 'py') }
    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if (-not $command) { continue }
        # Actually run it: the Windows Store alias is not an installed interpreter.
        & $command.Source -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' | Out-Host
        if ($LASTEXITCODE -eq 0) { return $command.Source }
    }
    throw 'Python 3.9+ is required for enablement review. Specify -PythonPath, or use -SkipEnablementAudit only to stage sources for later review.'
}
