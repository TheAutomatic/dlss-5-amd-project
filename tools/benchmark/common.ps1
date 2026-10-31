function Get-BenchmarkFileHash([string]$LiteralPath,[string]$Algorithm='SHA256') {
    $stream=[IO.File]::OpenRead($LiteralPath)
    $hash=[Security.Cryptography.SHA256]::Create()
    try { return [pscustomobject]@{Hash=([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','')} }
    finally { $hash.Dispose();$stream.Dispose() }
}
# Shared by the runner, packager and CPU tests. Paths in profiles are relative to DataRoot.
function Read-BenchmarkProfile([string]$Path) {
    $p = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($p.schema -ne 1 -or $p.rounds -lt 1 -or $p.frames -lt 1 -or $p.warmup -lt 0) { throw 'Invalid benchmark profile' }
    foreach ($key in 'models','modes','resolutions') { if (@($p.$key).Count -eq 0) { throw "Empty profile field: $key" } }
    foreach ($key in 'models','modes') {
        $ids = @($p.$key | ForEach-Object { $_.id })
        if (($ids | Sort-Object -Unique).Count -ne $ids.Count) { throw "Duplicate $key IDs" }
        foreach ($id in $ids) { if ($id -notmatch '^[a-zA-Z0-9_-]+$') { throw 'IDs must be filename-safe' } }
    }
    foreach ($r in $p.resolutions) { if ($r.width -lt 1 -or $r.height -lt 1) { throw 'Invalid resolution' } }
    return $p
}
function Resolve-BenchmarkPath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative)) { throw "Profile paths must be relative: $Relative" }
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $full = [IO.Path]::GetFullPath((Join-Path $rootPath $Relative))
    if (-not $full.StartsWith($rootPath,[StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes DataRoot: $Relative" }
    return $full
}
function Get-BenchmarkFiles($Profile, [string]$DataRoot) {
    $paths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    [void]$paths.Add((Resolve-BenchmarkPath $DataRoot $Profile.probe))
    foreach ($relative in @($Profile.extraFiles)) {
        if (-not $relative) { continue }
        $full=Resolve-BenchmarkPath $DataRoot $relative
        if (Test-Path -LiteralPath $full -PathType Container) {
            foreach ($f in Get-ChildItem -LiteralPath $full -Recurse -File) { [void]$paths.Add($f.FullName) }
        } else { [void]$paths.Add($full) }
    }
    foreach ($m in $Profile.models) {
        [void]$paths.Add((Resolve-BenchmarkPath $DataRoot $m.file))
        foreach ($r in $Profile.resolutions) {
            $plan = $m.plan.Replace('{width}',[string]$r.width).Replace('{height}',[string]$r.height)
            [void]$paths.Add((Resolve-BenchmarkPath $DataRoot $plan))
        }
    }
    foreach ($m in $Profile.modes) {
        [void]$paths.Add((Resolve-BenchmarkPath $DataRoot $m.exe))
        foreach ($dir in @($m.shaders,$m.aco) | Where-Object { $_ }) {
            $full = Resolve-BenchmarkPath $DataRoot $dir
            if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Missing asset directory: $dir" }
            foreach ($f in Get-ChildItem -LiteralPath $full -Recurse -File) { [void]$paths.Add($f.FullName) }
        }
    }
    foreach ($path in $paths) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" } }
    return @($paths | Sort-Object)
}
function Get-BenchmarkMedian($Values) {
    $v = @($Values | Sort-Object); if (-not $v.Count) { return $null }
    $i = [int][Math]::Floor($v.Count/2)
    if ($v.Count % 2) { return [double]$v[$i] }
    return ([double]$v[$i-1] + [double]$v[$i])/2
}
function Save-BenchmarkSummary($Rows, $Profile, $System, [string]$Out) {
    $all = @($Rows | ForEach-Object { $_ })
    if ($all.Count) { $all | Export-Csv -LiteralPath "$Out/runs.csv" -NoTypeInformation -Encoding UTF8 }
    $lines = @("Offline benchmark: $($Profile.name)","Profile rounds=$($Profile.rounds), warmup=$($Profile.warmup), measured frames=$($Profile.frames)", 'GPU milliseconds per frame; lower is better. Median of per-run GPU means.','')
    if ($System) {
        $d=$System.gpu
        $lines += @("GPU: $($d.name)","Driver: $($d.driverName) / $($d.driverInfo) (raw $($d.driverVersion))", "PCI: $($d.vendor):$($d.device); index=$($d.index); UUID=$($d.uuid); LUID=$($d.luid)","Device-local heap bytes (not necessarily dedicated VRAM): $($d.deviceLocalHeapBytes)","CPU: $($System.cpu)","RAM KiB: $($System.totalRamKiB); free at start=$($System.freeRamKiB)","OS: $($System.osVersion) build $($System.osBuild)","Power: $($System.powerPlan)","Start: $($System.start)")
    }
    foreach ($m in $Profile.modes) { $lines += "Mode $($m.id): $($m.label); source=$($m.source)" }
    $lines += @("Input/timing: $($Profile.description)",'')
    foreach ($r in $all) { $lines += "RUN $($r.Resolution) $($r.Model) $($r.Mode) round=$($r.Round) status=$($r.Status) exit=$($r.ExitCode) warmup=$($r.Warmup) frames=$($r.Frames) ms=$($r.GpuMs)" }
    $summaries = @()
    foreach ($g in ($all | Group-Object Resolution,Model,Mode)) {
        $ok = @($g.Group | Where-Object Status -eq 'OK')
        if ($ok.Count) {
            $v=@($ok.GpuMs | Sort-Object); $med=Get-BenchmarkMedian $v
            $lines += ('{0}: valid={1}/{2} median={3:F4} min={4:F4} max={5:F4}' -f $g.Name,$v.Count,$Profile.rounds,$med,$v[0],$v[-1])
            $summaries += [pscustomobject]@{Resolution=$ok[0].Resolution;Model=$ok[0].Model;Mode=$ok[0].Mode;Count=$ok.Count;Median=$med;Min=$v[0];Max=$v[-1]}
        } else { $lines += "$($g.Name): NO VALID TIMING" }
    }
    if ($Profile.referenceMode) {
        foreach ($s in $summaries | Where-Object { $_.Mode -ne $Profile.referenceMode -and $_.Count -eq $Profile.rounds }) {
            $ref=@($summaries | Where-Object { $_.Mode -eq $Profile.referenceMode -and $_.Resolution -eq $s.Resolution -and $_.Model -eq $s.Model -and $_.Count -eq $Profile.rounds })
            if ($ref.Count -eq 1 -and $ref[0].Median -gt 0 -and $s.Median -gt 0) { $lines += ('{0} {1} {2} vs {3}: time reduction={4:F2}%; throughput={5:F3}x' -f $s.Resolution,$s.Model,$s.Mode,$ref[0].Mode,(100*(1-$s.Median/$ref[0].Median)),($ref[0].Median/$s.Median)) }
        }
    }
    $lines += @('','Package/input file hashes: input-manifest.json. Commands and raw logs accompany every run.','Partial runs are marked by valid counts. Exit success and nonzero output do not establish image quality.')
    $lines | Set-Content -LiteralPath "$Out/benchmark_results.txt" -Encoding UTF8
    ConvertTo-Json -InputObject @($summaries) -Depth 6 | Set-Content -LiteralPath "$Out/summary.json" -Encoding UTF8
}



