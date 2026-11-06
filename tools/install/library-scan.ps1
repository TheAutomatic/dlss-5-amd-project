# Known-store library scan. No full-disk walk. Does not write library.json.
function Test-OptScalerGameExeName([string]$name) {
    return $name -match '(?i)\.exe$' -and
        $name -notmatch '(?i)^(setup|install|unins|crash|unitycrash|vc_redist|dxsetup|easyanticheat|eac_|battleye|beclient|beservice|unrealcef|epicwebhelper|webview|cefsubprocess|shadercompile|bugsplat|crs-|redprelauncher|larilauncher|2klauncher|8za|7za|checkenv).*\.exe$' -and
        $name -notmatch '(?i)(prelauncher|launcher|crashreport|crashhandler|crs-handler|errorreporter|updater|patcher|uninstaller|benchmark|dedicatedserver)\.exe$' -and
        $name -notmatch '(?i)^(gamelaunchhelper|person-worker|activationui|cleanup|touchup|config)\.exe$'
}

function Test-OptScalerUpscalerAnchor([string]$exeDir) {
    if (-not $exeDir) { return $false }
    foreach ($marker in @(
        'amd-presr-install.txt',
        'nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll', 'sl.interposer.dll',
        'libxess.dll', 'libxess_fg.dll',
        'amd_fidelityfx_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_vk.dll'
    )) {
        if (Test-Path -LiteralPath (Join-Path $exeDir $marker) -PathType Leaf) { return $true }
    }
    return $false
}

function Get-OptScalerExeRank([string]$fullPath, [string]$rootDir, [int]$depth) {
    $leaf = [IO.Path]::GetFileName($fullPath)
    $exeDir = [IO.Path]::GetDirectoryName($fullPath)
    $rel = if ($rootDir -and $fullPath.Length -gt $rootDir.Length) { $fullPath.Substring($rootDir.Length).TrimStart('\','/') } else { $leaf }
    if (Test-Path -LiteralPath (Join-Path $exeDir 'amd-presr-install.txt') -PathType Leaf) { return -10 }
    $rank = 3
    if ($leaf -match '(?i)WinGDK-Shipping\.exe$') { $rank = 0 }
    elseif ($leaf -match '(?i)-Win64-Shipping\.exe$') { $rank = 1 }
    elseif ($leaf -match '(?i)Shipping\.exe$') { $rank = 2 }
    elseif ($rel -match '(?i)(^|[\\/])(x64_dx12|dx12)([\\/]|$)' -or $leaf -match '(?i)dx12') { $rank = 1 }
    elseif ($rel -match '(?i)^platform[\\/](steam|epic|wegame|gog|xbox)[\\/]' -or (Test-Path -LiteralPath (Join-Path $exeDir 'D3D12\D3D12Core.dll') -PathType Leaf)) { $rank = 1 }
    elseif ($rel -match '(?i)(^|[\\/])(binaries[\\/](win64|wingdk)|bin[\\/](x64|win64))([\\/]|$)') { $rank = 2 }
    elseif (Test-Path -LiteralPath (Join-Path $exeDir 'UnityPlayer.dll') -PathType Leaf) { $rank = 2 }
    if (Test-OptScalerUpscalerAnchor $exeDir) {
        $rank -= $(if ($rel -match '(?i)(^|[\\/])(x64_dx12|dx12)([\\/]|$)' -or $leaf -match '(?i)dx12') { 5 } else { 4 })
    }
    return $rank
}

function Get-OptScalerPrimaryExes([string]$dir, [int]$depth = 4) {
    $found = New-Object System.Collections.Generic.List[object]
    if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return @() }
    $rootFull = try { [IO.Path]::GetFullPath($dir).TrimEnd('\','/') } catch { $dir }
    $queue = New-Object System.Collections.Generic.Queue[object]
    $queue.Enqueue(@{ Path = $dir; Depth = 0 })
    $seen = 0
    # Keep the candidate/directory quota, but also bound all ordinary resources.
    $examined = 0
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($queue.Count -gt 0 -and $seen -lt 400 -and $examined -lt 4096 -and $timer.ElapsedMilliseconds -lt 2000) {
        $cur = $queue.Dequeue()
        if ($cur.Depth -gt $depth) { continue }
        $entries = $null
        try { $entries = [IO.Directory]::EnumerateFileSystemEntries($cur.Path).GetEnumerator() } catch { continue }
        $subDirs = New-Object System.Collections.Generic.List[string]
        try {
        while ($seen -lt 400 -and $examined -lt 4096 -and $timer.ElapsedMilliseconds -lt 2000 -and $entries.MoveNext()) {
            $entry = $entries.Current
            $examined++
            $leaf = [IO.Path]::GetFileName($entry)
            $isDir = $false
            $reparse = $false
            try {
                $attr = [IO.File]::GetAttributes($entry)
                $isDir = [bool]($attr -band [IO.FileAttributes]::Directory)
                $reparse = [bool]($attr -band [IO.FileAttributes]::ReparsePoint)
            } catch { continue }
            if ($reparse) { continue }
            if ($isDir) {
                if ($leaf -notmatch '(?i)^(OptiScaler|person-model|dlssnr-amd|_CommonRedist|__Installer|redist|DirectX|Launcher|REDprelauncher|CrashReporter|Bonus|Extras|Soundtrack|OST|Artbook|Wallpapers?|Manual|Docs|Support|ModTools|SDK|Paks|ShaderCache)$') {
                    [void]$subDirs.Add($entry)
                }
                continue
            }
            if ($leaf -notmatch '(?i)\.exe$') { continue }
            $seen++
            if (-not (Test-OptScalerGameExeName $leaf)) { continue }
            $rank = Get-OptScalerExeRank $entry $rootFull $cur.Depth
            $len = try { (Get-Item -LiteralPath $entry -Force -ErrorAction Stop).Length } catch { 0L }
            [void]$found.Add(@{ path = $entry; rank = $rank; name = $leaf; length = [long]$len })
        }
        } catch { } finally { if ($entries) { $entries.Dispose() } }
        foreach ($sd in ($subDirs | Sort-Object {
            $n = [IO.Path]::GetFileName($_)
            if ($n -match '(?i)^(bin|binaries|x64_dx12|x64|win64|wingdk|game|client|content|platform)$') { 0 } else { 1 }
        }, { [IO.Path]::GetFileName($_) })) {
            if ($seen -ge 400 -or $timer.ElapsedMilliseconds -ge 2000) { break }
            $seen++
            $queue.Enqueue(@{ Path = $sd; Depth = $cur.Depth + 1 })
        }
    }
    return @($found | Sort-Object { $_.rank }, { $_.name }, { -([long]$_.length) }, { $_.path } | Select-Object -First 8)
}

function Get-OptScalerXboxExes([string]$dir) {
    # AppxManifest can point at GameLaunchHelper; GDK's config names the actual game.
    $found = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $reader = $null
    try {
        $config = Join-Path $dir 'MicrosoftGame.Config'
        $settings = New-Object System.Xml.XmlReaderSettings
        $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $settings.MaxCharactersInDocument = 1MB
        $reader = [System.Xml.XmlReader]::Create($config, $settings)
        $xml = New-Object System.Xml.XmlDocument
        $xml.XmlResolver = $null
        $xml.Load($reader)
        $root = [IO.Path]::GetFullPath($dir).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        foreach ($node in $xml.SelectNodes('/*[local-name()="Game"]/*[local-name()="ExecutableList"]/*[local-name()="Executable"]')) {
            $name = $node.GetAttribute('Name')
            $family = $node.GetAttribute('TargetDeviceFamily')
            if ($family -and $family -ne 'PC') { continue }
            if ($node.GetAttribute('IsDevOnly') -eq 'true') { continue }
            if (-not $name -or [IO.Path]::IsPathRooted($name)) { continue }
            if (-not (Test-OptScalerGameExeName ([IO.Path]::GetFileName($name)))) { continue }
            try { $path = [IO.Path]::GetFullPath((Join-Path $dir $name)) } catch { continue }
            if (-not $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or $seen.ContainsKey($path)) { continue }
            $seen[$path] = $true
            [void]$found.Add(@{ path = $path; rank = -1; name = [IO.Path]::GetFileName($path) })
        }
    } catch {
        # Missing, stale or malformed store metadata falls back to the bounded scan.
    } finally {
        if ($reader) { $reader.Dispose() }
    }
    if ($found.Count -gt 0) { return @($found | Sort-Object { $_.name }, { $_.path }) }
    return @(Get-OptScalerPrimaryExes $dir 6)
}

function Get-OptScalerImageBounds([string]$path) {
    # Read container headers only: never ask an image codec to allocate decoded pixels.
    $stream = $null
    $reader = $null
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($stream.Length -lt 33 -or $stream.Length -gt 32MB) { return $null }
        $reader = New-Object IO.BinaryReader($stream)
        $head = $reader.ReadBytes(24)
        $width = 0
        $height = 0
        if ([BitConverter]::ToString($head, 0, 8) -eq '89-50-4E-47-0D-0A-1A-0A') {
            if ([BitConverter]::ToString($head, 8, 8) -ne '00-00-00-0D-49-48-44-52') { return $null }
            $width = [long]$head[16] * 16777216 + [long]$head[17] * 65536 + [long]$head[18] * 256 + $head[19]
            $height = [long]$head[20] * 16777216 + [long]$head[21] * 65536 + [long]$head[22] * 256 + $head[23]
            $stream.Position = $stream.Length - 12
            if ([BitConverter]::ToString($reader.ReadBytes(12)) -ne '00-00-00-00-49-45-4E-44-AE-42-60-82') { return $null }
        } elseif ($head[0] -eq 255 -and $head[1] -eq 216) {
            $stream.Position = $stream.Length - 2
            if ($reader.ReadByte() -ne 255 -or $reader.ReadByte() -ne 217) { return $null }
            $stream.Position = 2
            $limit = [Math]::Min($stream.Length - 2, 64KB)
            $markers = 0
            while ($stream.Position -lt $limit -and $markers -lt 256) {
                $markers++
                if ($reader.ReadByte() -ne 255) { return $null }
                $marker = $reader.ReadByte()
                while ($marker -eq 255 -and $stream.Position -lt $limit) { $marker = $reader.ReadByte() }
                if ($marker -eq 218 -or $marker -eq 217 -or $marker -eq 0) { return $null }
                if ($marker -eq 1 -or ($marker -ge 208 -and $marker -le 215)) { continue }
                $length = [int]$reader.ReadByte() * 256 + $reader.ReadByte()
                if ($length -lt 2 -or $stream.Position + $length - 2 -gt $limit) { return $null }
                if ($marker -in @(192,193,194)) {
                    if ($length -lt 8 -or $reader.ReadByte() -ne 8) { return $null }
                    $height = [int]$reader.ReadByte() * 256 + $reader.ReadByte()
                    $width = [int]$reader.ReadByte() * 256 + $reader.ReadByte()
                    $components = $reader.ReadByte()
                    if ($components -notin @(1,3,4) -or $length -ne 8 + 3 * $components) { return $null }
                    break
                }
                $stream.Position += $length - 2
            }
        } else { return $null }
        if ($width -lt 1 -or $height -lt 1 -or $width -gt 8192 -or $height -gt 8192 -or [long]$width * $height -gt 32000000) { return $null }
        return @{ width = [int]$width; height = [int]$height }
    } catch { return $null } finally {
        if ($reader) { $reader.Dispose() }
        elseif ($stream) { $stream.Dispose() }
    }
}

function Test-OptScalerArtworkPath([string]$path, [string]$root) {
    # A proven manifest mapping must still stay inside this game and avoid links.
    try {
        $full = [IO.Path]::GetFullPath($path)
        $base = [IO.Path]::GetFullPath($root).TrimEnd('\','/')
        if (-not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        $current = $full
        while ($current -and $current.Length -ge $base.Length) {
            if (([IO.File]::GetAttributes($current) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
            $current = [IO.Path]::GetDirectoryName($current)
        }
        return $true
    } catch { return $false }
}

function Get-OptScalerLocalArtwork {
    param(
        [ValidateSet('Steam','Xbox')][string]$Source,
        [string[]]$Roots,
        [hashtable]$Budget = $null,
        [int]$MaxEntries = 192,
        [int]$MaxDirectories = 24,
        [int]$MaxImages = 64,
        [int]$MaxMilliseconds = 150
    )
    $result = @{ cover = ''; hero = ''; candidates = @(); examined = 0; directories = 0; images = 0 }
    if ($Budget -and $Budget.RemainingMilliseconds -le 0) { return $result }
    if ($Budget) { $MaxMilliseconds = [Math]::Min($MaxMilliseconds, $Budget.RemainingMilliseconds) }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $queue = New-Object System.Collections.Generic.Queue[object]
    $visited = @{}
    $mapped = @{}
    $candidates = New-Object System.Collections.Generic.List[object]
    $coverScore = -1L
    $heroScore = -1L
    foreach ($root in $Roots) {
        if (-not $root) { continue }
        try {
            $root = [IO.Path]::GetFullPath($root)
            if (([IO.File]::GetAttributes($root) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
            $queue.Enqueue(@{ path = $root; root = $root; depth = 0 })
        } catch { }
    }
    try {
        while ($queue.Count -gt 0 -and $result.directories -lt $MaxDirectories -and $result.examined -lt $MaxEntries -and $result.images -lt $MaxImages -and $timer.ElapsedMilliseconds -lt $MaxMilliseconds) {
            $dir = $queue.Dequeue()
            if ($visited.ContainsKey($dir.path)) { continue }
            $visited[$dir.path] = $true
            $result.directories++
            if ($Source -eq 'Xbox' -and $dir.depth -eq 0) {
                foreach ($manifest in @('MicrosoftGame.Config','appxmanifest.xml')) {
                    if ($timer.ElapsedMilliseconds -ge $MaxMilliseconds -or $result.examined -ge $MaxEntries) { break }
                    $result.examined++
                    $reader = $null
                    try {
                        $file = Join-Path $dir.path $manifest
                        if (-not (Test-OptScalerArtworkPath $file $dir.root) -or (Get-Item -LiteralPath $file).Length -gt 1MB) { continue }
                        $settings = New-Object Xml.XmlReaderSettings
                        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
                        $settings.XmlResolver = $null
                        $settings.MaxCharactersInDocument = 1MB
                        $reader = [Xml.XmlReader]::Create($file, $settings)
                        $xml = New-Object Xml.XmlDocument
                        $xml.XmlResolver = $null
                        $xml.Load($reader)
                        foreach ($node in $xml.SelectNodes('//*[local-name()="ShellVisuals" or local-name()="VisualElements" or local-name()="DefaultTile" or local-name()="SplashScreen" or local-name()="Properties"]')) {
                            if ($timer.ElapsedMilliseconds -ge $MaxMilliseconds -or $result.examined -ge $MaxEntries -or $mapped.Count -ge $MaxImages) { break }
                            foreach ($attribute in $node.Attributes) {
                                if ($timer.ElapsedMilliseconds -ge $MaxMilliseconds -or $result.examined -ge $MaxEntries -or $mapped.Count -ge $MaxImages) { break }
                                $result.examined++
                                $role = if ($attribute.LocalName -match '^(Square\d+x\d+Logo|StoreLogo|Logo)$') { 'square' } elseif ($attribute.LocalName -eq 'SplashScreenImage' -or ($node.LocalName -eq 'SplashScreen' -and $attribute.LocalName -eq 'Image')) { 'hero' } else { '' }
                                if (-not $role -or [IO.Path]::IsPathRooted($attribute.Value)) { continue }
                                $resource = [IO.Path]::GetFullPath((Join-Path $dir.path $attribute.Value))
                                $parent = [IO.Path]::GetDirectoryName($resource)
                                # Qualifier variants can exist even when the unqualified manifest path does not.
                                if (-not (Test-OptScalerArtworkPath $parent $dir.root) -and $parent -ne $dir.root) { continue }
                                $mapped[$resource.Substring(0, $resource.Length - [IO.Path]::GetExtension($resource).Length)] = $role
                                if ($parent -ne $dir.path -and $queue.Count -lt $MaxDirectories) { $queue.Enqueue(@{ path = $parent; root = $dir.root; depth = 1 }) }
                            }
                        }
                    } catch { Write-Verbose ("Artwork metadata skipped: {0}: {1}" -f $file, $_.Exception.Message) } finally { if ($reader) { $reader.Dispose() } }
                }
            }
            $entries = $null
            try {
                $entries = [IO.Directory]::EnumerateFileSystemEntries($dir.path).GetEnumerator()
                while ($result.examined -lt $MaxEntries -and $result.images -lt $MaxImages -and $timer.ElapsedMilliseconds -lt $MaxMilliseconds -and $entries.MoveNext()) {
                    $result.examined++
                    $path = $entries.Current
                    $name = [IO.Path]::GetFileName($path)
                    try { $attributes = [IO.File]::GetAttributes($path) } catch { continue }
                    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
                    if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                        if ($dir.depth -eq 0 -and (($Source -eq 'Steam' -and $name -match '^[a-fA-F0-9]{32,64}$') -or ($Source -eq 'Xbox' -and $name -ieq 'Assets'))) {
                            if ($queue.Count -lt $MaxDirectories) { $queue.Enqueue(@{ path = $path; root = $dir.root; depth = 1 }) }
                        }
                        continue
                    }
                    if ($name -notmatch '(?i)\.(jpg|jpeg|png)$' -or $name -match '(?i)(blur|placeholder)') { continue }
                    $role = ''
                    $priority = 0
                    if ($Source -eq 'Steam') {
                        if ($name -match '(?i)^(?:\d+_)?library_(600x900|capsule)(?:_[a-z0-9_-]+)?\.(jpg|jpeg|png)$') { $role = 'poster' }
                        elseif ($name -match '(?i)^(?:\d+_)?library_hero(?:_[a-z0-9_-]+)?\.(jpg|jpeg|png)$') { $role = 'hero'; $priority = 1 }
                        elseif ($name -match '(?i)^(?:\d+_)?(?:library_)?header(?:_[a-z0-9_-]+)?\.(jpg|jpeg|png)$') { $role = 'hero' }
                    } else {
                        if ($name -match '(?i)^(StoreLogo|Logo|Square\d+x\d+Logo|LargeLogo|SmallLogo)(?:[._-][a-z0-9_.-]+)?\.(jpg|jpeg|png)$') { $role = 'square' }
                        elseif ($name -match '(?i)^(MC)?SplashScreen(?:[._-][a-z0-9_.-]+)?\.(jpg|jpeg|png)$') { $role = 'hero' }
                        $stem = $path.Substring(0, $path.Length - [IO.Path]::GetExtension($path).Length)
                        foreach ($key in $mapped.Keys) {
                            if ($stem.Equals($key, [StringComparison]::OrdinalIgnoreCase) -or $stem.StartsWith($key + '.', [StringComparison]::OrdinalIgnoreCase)) { $role = $mapped[$key]; break }
                        }
                        if ($role -eq 'hero' -and $name -match '(?i)logo') { $role = '' }
                    }
                    if (-not $role) { continue }
                    $result.images++
                    $bounds = Get-OptScalerImageBounds $path
                    if (-not $bounds) { continue }
                    $ratio = $bounds.width / [double]$bounds.height
                    $area = [long]$bounds.width * $bounds.height
                    $kind = ''
                    if ($role -eq 'poster' -and $bounds.width -ge 200 -and $bounds.height -ge 300 -and $ratio -ge 0.5 -and $ratio -le 0.85) { $kind = 'cover' }
                    elseif ($role -eq 'square' -and $bounds.width -ge 64 -and $bounds.height -ge 64 -and $ratio -ge 0.9 -and $ratio -le 1.1) { $kind = 'cover' }
                    elseif ($role -eq 'hero' -and $bounds.width -ge 480 -and $bounds.height -ge 200 -and $ratio -ge 1.5 -and $ratio -le 4) { $kind = 'hero' }
                    if (-not $kind) { continue }
                    [void]$candidates.Add(@{ path = $path; kind = $kind; width = $bounds.width; height = $bounds.height })
                    # Steam's dedicated hero wins over a header; resolution wins within each semantic class.
                    $score = [long]$priority * 100000000 + $area
                    if ($kind -eq 'cover' -and ($score -gt $coverScore -or ($score -eq $coverScore -and [string]::Compare($path, $result.cover, [StringComparison]::OrdinalIgnoreCase) -lt 0))) { $result.cover = $path; $coverScore = $score }
                    elseif ($kind -eq 'hero' -and ($score -gt $heroScore -or ($score -eq $heroScore -and [string]::Compare($path, $result.hero, [StringComparison]::OrdinalIgnoreCase) -lt 0))) { $result.hero = $path; $heroScore = $score }
                }
            } catch { } finally { if ($entries) { $entries.Dispose() } }
        }
    } finally {
        $timer.Stop()
        if ($Budget) { $Budget.RemainingMilliseconds = [Math]::Max(0, $Budget.RemainingMilliseconds - [Math]::Max(1, $timer.ElapsedMilliseconds)) }
    }
    $result.candidates = $candidates.ToArray()
    return $result
}

function Add-OptScalerGame($list, $seen, [string]$name, [string]$exe, [string]$source, [string]$id, [string]$cover = '', [string]$hero = '') {
    if (-not $exe) { return }
    try { $exe = [IO.Path]::GetFullPath($exe) } catch { return }
    $key = $exe.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { return }
    $seen[$key] = $true
    $dir = [IO.Path]::GetDirectoryName($exe)
    $installed = Test-Path -LiteralPath (Join-Path $dir 'amd-presr-install.txt')
    [void]$list.Add(@{
        id = $(if ($id) { $id } else { $key })
        name = $name
        exe = $exe
        source = $source
        installed = [bool]$installed
        selectionRequired = $false
        exeOverride = $false
        cover = $cover
        hero = $hero
    })
}

function Get-OptScalerGameLibrary {
    $games = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $artworkBudget = @{ RemainingMilliseconds = 1000 }
    $steamRoots = New-Object System.Collections.Generic.List[string]
    foreach ($reg in @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )) {
        if (-not (Test-Path $reg)) { continue }
        try {
            $p = Get-ItemProperty $reg
            $path = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($path) { [void]$steamRoots.Add($path) }
        } catch { }
    }
    foreach ($r in @("${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam")) {
        if ($r -and (Test-Path -LiteralPath $r)) { [void]$steamRoots.Add($r) }
    }
    foreach ($steamRoot in ($steamRoots | Select-Object -Unique)) {
        $libs = @($steamRoot)
        $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            try {
                $raw = Get-Content -LiteralPath $vdf -Raw -Encoding UTF8 -ErrorAction Stop
                [regex]::Matches($raw, '"path"\s+"([^"]+)"') | ForEach-Object { $libs += ($_.Groups[1].Value -replace '\\\\','\') }
            } catch { }
        }
        foreach ($lib in ($libs | Select-Object -Unique)) {
            $apps = Join-Path $lib 'steamapps'
            if (-not (Test-Path -LiteralPath $apps)) { continue }
            Get-ChildItem -LiteralPath $apps -Filter 'appmanifest_*.acf' -File -ErrorAction SilentlyContinue | ForEach-Object {
                try {
                    $raw = Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8
                    $n = [regex]::Match($raw, '"name"\s+"([^"]+)"').Groups[1].Value
                    $d = [regex]::Match($raw, '"installdir"\s+"([^"]+)"').Groups[1].Value
                    $appid = [regex]::Match($raw, '"appid"\s+"([^"]+)"').Groups[1].Value
                    if (-not $n -or $n -match '(?i)(dedicated server|sdk|soundtrack|benchmark|editor|steamworks|redistributable)') { return }
                    if ($d -match '(?i)Steamworks') { return }
                    $n = ($n -replace '[®™©]','').Trim()
                    $dir = Join-Path $lib "steamapps\common\$d"
                    $cands = @(Get-OptScalerPrimaryExes $dir 4)
                    if ($cands.Count -gt 0) {
                        $grid = Join-Path $steamRoot "appcache\librarycache\$appid"
                        $art = Get-OptScalerLocalArtwork -Source Steam -Roots @($grid) -Budget $artworkBudget
                        $cover = $art.cover
                        $hero = $art.hero
                        Add-OptScalerGame $games $seen $n $cands[0].path 'Steam' ("steam:" + $appid) $cover $hero
                        if (@($cands | Where-Object { $_.rank -eq $cands[0].rank }).Count -gt 1) {
                            $games[$games.Count - 1].selectionRequired = $true
                        }
                    }
                } catch { }
            }
        }
    }
    $epic = "$env:ProgramData\Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path -LiteralPath $epic) {
        Get-ChildItem -LiteralPath $epic -Filter '*.item' -File -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $j = Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
                if (-not $j.InstallLocation -or -not $j.DisplayName) { return }
                if ($j.DisplayName -match '(?i)(dedicated server|editor|prerequisite)') { return }
                $exe = $null
                $cands = @(Get-OptScalerPrimaryExes $j.InstallLocation 4)
                if ($j.LaunchExecutable) {
                    $cand = Join-Path $j.InstallLocation $j.LaunchExecutable
                    if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Test-OptScalerGameExeName ([IO.Path]::GetFileName($cand)))) {
                        $candRank = Get-OptScalerExeRank $cand $j.InstallLocation 0
                        if ($cands.Count -eq 0 -or $candRank -le $cands[0].rank) { $exe = $cand }
                    }
                }
                if (-not $exe -and $cands.Count -gt 0) { $exe = $cands[0].path }
                if ($exe) { Add-OptScalerGame $games $seen $j.DisplayName $exe 'Epic' ("epic:" + [string]$j.CatalogItemId) }
            } catch { }
        }
    }
    foreach ($regRoot in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games','HKLM:\SOFTWARE\GOG.com\Games','HKCU:\SOFTWARE\GOG.com\Games')) {
        if (-not (Test-Path $regRoot)) { continue }
        Get-ChildItem $regRoot -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $p = Get-ItemProperty $_.PSPath
                if ($p.path -and $p.exe -and $p.gameName) {
                    $cand = if ([IO.Path]::IsPathRooted([string]$p.exe)) { [string]$p.exe } else { Join-Path $p.path $p.exe }
                    $exe = $null
                    $cands = @(Get-OptScalerPrimaryExes ([string]$p.path) 4)
                    if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Test-OptScalerGameExeName ([IO.Path]::GetFileName($cand)))) {
                        $candRank = Get-OptScalerExeRank $cand ([string]$p.path) 0
                        if ($cands.Count -eq 0 -or $candRank -le $cands[0].rank) { $exe = $cand }
                    }
                    if (-not $exe -and $cands.Count -gt 0) { $exe = $cands[0].path }
                    if ($exe) {
                        Add-OptScalerGame $games $seen $p.gameName $exe 'GOG' ("gog:" + [string]$_.PSChildName)
                    }
                }
            } catch { }
        }
    }
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if (-not $drive.IsReady -or $drive.DriveType -ne [IO.DriveType]::Fixed) { continue }
        $root = Join-Path $drive.Name 'XboxGames'
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $content = Join-Path $_.FullName 'Content'
            $base = if (Test-Path -LiteralPath $content -PathType Container) { $content } else { $_.FullName }
            $cands = @(Get-OptScalerXboxExes $base)
            if ($cands.Count -gt 0) {
                $artRoots = @($base, $_.FullName) | Select-Object -Unique
                $art = Get-OptScalerLocalArtwork -Source Xbox -Roots $artRoots -Budget $artworkBudget
                $cover = $art.cover
                $hero = $art.hero
                Add-OptScalerGame $games $seen $_.Name $cands[0].path 'Xbox' ("xbox:" + $_.Name.ToLowerInvariant()) $cover $hero
            }
        }
    }
    return @($games | Sort-Object { $_.name }, { $_.exe })
}
