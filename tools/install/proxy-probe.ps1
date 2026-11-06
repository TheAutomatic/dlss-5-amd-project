# Bounded PE import probe. Never LoadLibrary, never start the game.
[CmdletBinding()]
param(
    [string]$ExePath,
    [string]$OutJson
)
$ErrorActionPreference = 'Stop'
$script:MaxFiles = 32
$script:MaxDepth = 8
$script:MaxBytes = 8MB
$script:MaxMs = 5000

$CandidateNames = @('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll','version.dll')

function Read-UInt16([byte[]]$b, [int]$o) {
    if ($o -lt 0 -or ($o + 2) -gt $b.Length) { throw 'truncated uint16' }
    return [BitConverter]::ToUInt16($b, $o)
}
function Read-UInt32([byte[]]$b, [int]$o) {
    if ($o -lt 0 -or ($o + 4) -gt $b.Length) { throw 'truncated uint32' }
    return [BitConverter]::ToUInt32($b, $o)
}
function RvaToOffset([object[]]$secs, [uint32]$rva, [int64]$fileLen) {
    foreach ($s in $secs) {
        $size = [Math]::Max([uint32]$s.VSize, [uint32]$s.RawSize)
        if ($rva -ge $s.VA -and $rva -lt ($s.VA + $size)) {
            $off = [int64]$s.Raw + ($rva - $s.VA)
            if ($off -lt 0 -or $off -ge $fileLen) { return $null }
            return [int64]$off
        }
    }
    return $null
}
function Read-Slice([IO.FileStream]$fs, [byte[]]$head, [int64]$off, [int]$maxLen, [int64]$fileLen) {
    if ($off -lt 0 -or $off -ge $fileLen -or $maxLen -le 0) { return $null }
    $count = [int][Math]::Min([int64]$maxLen, $fileLen - $off)
    if (($off + $count) -le $head.Length) {
        $slice = New-Object byte[] $count
        [Array]::Copy($head, [int]$off, $slice, 0, $count)
        return $slice
    }
    $fs.Position = $off
    $buf = New-Object byte[] $count
    $got = $fs.Read($buf, 0, $count)
    if ($got -le 0) { return $null }
    if ($got -lt $count) { $buf = $buf[0..($got - 1)] }
    return $buf
}
function Read-AsciiZ([byte[]]$b, [int]$o, [int]$max) {
    if (-not $b -or $o -lt 0 -or $o -ge $b.Length) { return $null }
    $end = [Math]::Min($b.Length, $o + $max)
    $i = $o
    while ($i -lt $end -and $b[$i] -ne 0) { $i++ }
    if ($i -eq $o) { return '' }
    return [Text.Encoding]::ASCII.GetString($b, $o, $i - $o)
}

function Get-PeImports([string]$path) {
    $info = @{
        ok = $false
        arch = 'unknown'
        imports = @()
        delayImports = @()
        error = $null
        partial = $false
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $info.error = 'missing'
        return $info
    }
    try {
        $len = [int64](Get-Item -LiteralPath $path).Length
    } catch {
        $info.error = 'unreadable'
        $info.partial = $true
        return $info
    }
    if ($len -lt 64) { $info.error = 'too-small'; return $info }
    $read = [int][Math]::Min($len, 65536L)
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    } catch {
        $info.error = 'unreadable'
        $info.partial = $true
        return $info
    }
    try {
        $b = New-Object byte[] $read
        $got = $fs.Read($b, 0, $read)
        if ($got -lt $read) { $b = $b[0..($got-1)] }
        if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { $info.error = 'not-pe'; return $info }
        $pe = [int](Read-UInt32 $b 0x3C)
        if ($pe + 24 -gt $b.Length) { $info.error = 'truncated'; $info.partial = $true; return $info }
        if ($b[$pe] -ne 0x50 -or $b[$pe+1] -ne 0x45) { $info.error = 'not-pe'; return $info }
        $machine = Read-UInt16 $b ($pe + 4)
        $nsec = Read-UInt16 $b ($pe + 6)
        $optSize = Read-UInt16 $b ($pe + 20)
        $opt = $pe + 24
        if ($machine -eq 0x14C) { $info.arch = 'x86' }
        elseif ($machine -eq 0x8664) { $info.arch = 'x64' }
        elseif ($machine -eq 0xAA64) { $info.arch = 'arm64' }
        else { $info.arch = ('machine-{0:x}' -f $machine) }
        if ($info.arch -ne 'x64') { $info.error = 'unsupported-arch'; return $info }
        if ($opt + $optSize -gt $b.Length) { $info.error = 'truncated'; $info.partial = $true; return $info }
        $magic = Read-UInt16 $b $opt
        if ($magic -ne 0x20B) { $info.error = 'not-pe32plus'; return $info }
        $dd = $opt + 112
        $secs = @()
        $secOff = $opt + $optSize
        for ($i = 0; $i -lt $nsec; $i++) {
            $so = $secOff + 40 * $i
            if ($so + 40 -gt $b.Length) { $info.partial = $true; break }
            $secs += @{
                VA = Read-UInt32 $b ($so + 12)
                VSize = Read-UInt32 $b ($so + 8)
                Raw = Read-UInt32 $b ($so + 20)
                RawSize = Read-UInt32 $b ($so + 16)
            }
        }
        function Read-NameList([uint32]$rva) {
            $names = @()
            if ($rva -eq 0) { return $names }
            $off = RvaToOffset $secs $rva $len
            if ($null -eq $off) { return $names }
            $table = Read-Slice $fs $b $off (256 * 20) $len
            if (-not $table) { return $names }
            for ($n = 0; $n -lt 256; $n++) {
                $row = 20 * $n
                if ($row + 20 -gt $table.Length) { $script:partial = $true; break }
                $nameRva = Read-UInt32 $table ($row + 12)
                $first = Read-UInt32 $table $row
                if ($nameRva -eq 0 -and $first -eq 0) { break }
                $no = RvaToOffset $secs $nameRva $len
                if ($null -eq $no) { continue }
                $nb = Read-Slice $fs $b $no 260 $len
                $nm = Read-AsciiZ $nb 0 260
                if ($nm) { $names += $nm.ToLowerInvariant() }
            }
            return $names
        }
        $script:partial = $false
        $impRva = Read-UInt32 $b ($dd + 8)
        $info.imports = @(Read-NameList $impRva)
        # Delay import directory is index 13.
        $delayRva = Read-UInt32 $b ($dd + 13 * 8)
        $delay = @()
        if ($delayRva -ne 0) {
            $off = RvaToOffset $secs $delayRva $len
            if ($null -eq $off) { $script:partial = $true }
            else {
                $table = Read-Slice $fs $b $off (128 * 32) $len
                if (-not $table) { $script:partial = $true }
                else {
                    for ($n = 0; $n -lt 128; $n++) {
                        $row = 32 * $n
                        if ($row + 32 -gt $table.Length) { $script:partial = $true; break }
                        $nameRva = Read-UInt32 $table ($row + 4)
                        $attr = Read-UInt32 $table $row
                        if ($nameRva -eq 0 -and $attr -eq 0) { break }
                        $no = RvaToOffset $secs $nameRva $len
                        if ($null -eq $no) { continue }
                        $nb = Read-Slice $fs $b $no 260 $len
                        $nm = Read-AsciiZ $nb 0 260
                        if ($nm) { $delay += $nm.ToLowerInvariant() }
                    }
                }
            }
        }
        $info.delayImports = $delay
        $info.ok = $true
        $info.partial = [bool]$script:partial
    } catch {
        $info.error = $_.Exception.Message
        $info.partial = $true
    } finally {
        $fs.Dispose()
    }
    return $info
}

function Test-SystemLocation([string]$path) {
    $p = $path.ToLowerInvariant()
    return ($p -match '\\windows\\(system32|syswow64|winsxs|system)(\\|$)')
}

function Test-Reparse([string]$path) {
    try {
        $item = Get-Item -LiteralPath $path -Force
        return [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
    } catch { return $true }
}

function Get-RelatedDirs([string]$exeDir) {
    $out = New-Object System.Collections.Generic.List[string]
    function Add-Dir([string]$d) {
        if (-not $d) { return }
        try { $d = [IO.Path]::GetFullPath($d) } catch { return }
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { return }
        if (Test-SystemLocation $d) { return }
        if (Test-Reparse $d) { return }
        if (-not $out.Contains($d)) { [void]$out.Add($d) }
    }
    Add-Dir $exeDir
    $leaf = [IO.Path]::GetFileName($exeDir)
    if ($leaf -match '^(?i)(Win64|Win64Shipping|Binaries|bin)$') {
        Add-Dir (Split-Path $exeDir -Parent)
    }
    foreach ($rel in @('Binaries\Win64', 'Engine\Binaries\Win64', 'bin')) {
        Add-Dir (Join-Path $exeDir $rel)
    }
    return @($out)
}

function Get-LocalImportWalk([string]$startExe, [Diagnostics.Stopwatch]$sw) {
    $visited = @{}
    $queue = New-Object System.Collections.Generic.Queue[object]
    $queue.Enqueue(@{ Path = $startExe; Depth = 0 })
    $names = New-Object System.Collections.Generic.List[string]
    $files = 0
    $partial = $false
    $capped = $false
    while ($queue.Count -gt 0) {
        if ($files -ge $script:MaxFiles -or $sw.ElapsedMilliseconds -ge $script:MaxMs) {
            $partial = $true; $capped = $true; break
        }
        $item = $queue.Dequeue()
        $path = $item.Path
        $key = $path.ToLowerInvariant()
        if ($visited.ContainsKey($key)) { continue }
        $visited[$key] = $true
        if (Test-SystemLocation $path -or Test-Reparse $path) { continue }
        $files++
        $pe = Get-PeImports $path
        if ($pe.partial) { $partial = $true }
        $found = @($pe.imports + $pe.delayImports)
        foreach ($n in $found) { if ($n) { [void]$names.Add($n) } }
        if ($item.Depth -ge $script:MaxDepth) { continue }
        $dirs = Get-RelatedDirs ([IO.Path]::GetDirectoryName($path))
        foreach ($n in $found) {
            if ($n -notmatch '\.dll$' -or [IO.Path]::GetFileName($n) -ne $n) { continue }
            foreach ($d in $dirs) {
                $cand = [IO.Path]::Combine($d, $n)
                if ([IO.File]::Exists($cand)) {
                    $queue.Enqueue(@{ Path = $cand; Depth = $item.Depth + 1 })
                }
            }
        }
    }
    return @{ names = @($names); files = $files; visited = @($visited.Keys); partial = $partial; capped = $capped }
}

function Get-DirFingerprint([string]$exe, [string]$dir) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($p in @($exe) + @($CandidateNames | ForEach-Object { Join-Path $dir $_ })) {
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            try {
                $i = Get-Item -LiteralPath $p
                [void]$parts.Add(('{0}|{1}|{2}' -f $i.Name, $i.Length, $i.LastWriteTimeUtc.Ticks))
            } catch {
                [void]$parts.Add(([IO.Path]::GetFileName($p) + '|protected'))
            }
        }
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($parts -join ';'))
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Get-ProxyRecommendation([string]$exe) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $full = [IO.Path]::GetFullPath($exe)
    $dir = [IO.Path]::GetDirectoryName($full)
    $result = @{
        gameExe = $full
        gameDir = $dir
        pe = $null
        candidates = @()
        conflicts = @()
        hint = $null
        partial = $false
        walk = $null
        fingerprint = $null
        limits = @{ maxFiles = $script:MaxFiles; maxDepth = $script:MaxDepth; maxBytes = [int]$script:MaxBytes; maxMs = $script:MaxMs }
    }
    $result.pe = Get-PeImports $full
    $walk = Get-LocalImportWalk $full $sw
    $result.walk = @{ files = $walk.files; capped = $walk.capped; visited = $walk.visited }
    $script:rank = @()
    $seen = @{}
    function Add-Cand([string]$name, [string]$conf, [string]$reason) {
        if ($seen.ContainsKey($name)) { return }
        $seen[$name] = $true
        $script:rank += @{ name = $name; confidence = $conf; reason = $reason }
    }
    $owned = @()
    foreach ($name in $CandidateNames) {
        $p = Join-Path $dir $name
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            $isOpti = $false
            try {
                $vi = (Get-Item -LiteralPath $p).VersionInfo
                $isOpti = ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler')
            } catch { }
            if ($isOpti) { $owned += $name }
            else {
                $result.conflicts += @{ name = $name; kind = 'foreign-proxy'; detail = 'same-name DLL is not this project' }
            }
        }
    }
    if ($owned.Count -eq 1) { Add-Cand $owned[0] 'high' 'existing project proxy' }
    elseif ($owned.Count -gt 1) {
        $result.hint = '已安装多个本项目代理，请选择要继续使用的 DLL。'
        $result.selectionRequired = $true
    }
    $direct = @($result.pe.imports)
    $delay = @($result.pe.delayImports)
    $deps = @($walk.names)
    foreach ($n in $CandidateNames) { if ($direct -contains $n) { Add-Cand $n 'medium' 'imported by the selected EXE' } }
    foreach ($n in $CandidateNames) { if ($delay -contains $n) { Add-Cand $n 'medium' 'delay-imported by the selected EXE' } }
    foreach ($n in $CandidateNames) { if ($deps -contains $n) { Add-Cand $n 'medium' 'imported by a local dependency' } }
    for ($i = 0; $i -lt $script:rank.Count; $i++) {
        $c = $script:rank[$i]
        $c.rank = $i + 1
        $result.candidates += $c
    }
    $result.recommend = $null
    $blockedArch = ($result.pe.arch -and $result.pe.arch -ne 'x64' -and $result.pe.arch -ne 'unknown')
    if ($blockedArch) {
        $result.hint = '选中的 EXE 不是 x64。请改选游戏真正的渲染程序。'
        $result.blockInstall = $true
    } elseif ($owned.Count -eq 1) {
        $result.recommend = $owned[0]
        $result.hint = ("已安装 {0}，建议继续使用 {0}。" -f $owned[0])
    } elseif ($script:rank.Count -gt 0 -and -not $result.selectionRequired) {
        $result.recommend = $script:rank[0].name
        $result.hint = ("检测到程序导入 {0}，建议尝试注入 {0}。" -f $result.recommend)
    } elseif (-not $result.hint) {
        $result.hint = '无法判断，请选择 DLL 自行尝试。'
    }
    $result.partial = [bool]$result.pe.partial -or [bool]$walk.partial
    if ($sw.ElapsedMilliseconds -gt $script:MaxMs) { $result.partial = $true }
    $result.fingerprint = Get-DirFingerprint $full $dir
    return $result
}

if ($ExePath) {
    $r = Get-ProxyRecommendation $ExePath
    $json = $r | ConvertTo-Json -Depth 6
    if ($OutJson) { [IO.File]::WriteAllText($OutJson, $json) } else { Write-Output $json }
}
