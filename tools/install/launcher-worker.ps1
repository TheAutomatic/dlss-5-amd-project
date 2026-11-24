# Schema 1 worker. Inspect is read-only. Install/uninstall call the existing scripts.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RequestJson,
    [Parameter(Mandatory = $true)][string]$EventsJsonl,
    [Parameter(Mandatory = $true)][string]$ResultJson,
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'
# Match redirected child output and the launcher reader, independent of the OS code page.
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$here = $PSScriptRoot
. (Join-Path $here 'proxy-probe.ps1')

function Append-EventText([string]$text) {
    if (-not $text) { return }
    for ($try = 0; $try -lt 4; $try++) {
        try {
            [IO.File]::AppendAllText($EventsJsonl, $text)
            return
        } catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 35
        }
    }
    [IO.File]::AppendAllText($EventsJsonl, $text)
}
function Write-Event([string]$stage, [string]$message) {
    if ($null -ne $message) { $message = [string]::new($message.ToCharArray()) }
    if ($Elevated -and $message) { Write-Host ("[{0}] {1}" -f $stage, $message) }
    $line = (@{ schema = 1; requestId = $script:rid; stage = $stage; message = $message } | ConvertTo-Json -Compress)
    Append-EventText ($line + [Environment]::NewLine)
}
function Write-EventBatch([string]$stage, [string[]]$messages) {
    if (-not $messages -or $messages.Count -eq 0) { return }
    $sb = New-Object System.Text.StringBuilder
    foreach ($m in $messages) {
        $msg = if ($null -ne $m) { [string]::new($m.ToCharArray()) } else { '' }
        if ($Elevated -and $msg) { Write-Host $msg }
        $line = (@{ schema = 1; requestId = $script:rid; stage = $stage; message = $msg } | ConvertTo-Json -Compress)
        [void]$sb.AppendLine($line)
    }
    if ($sb.Length -gt 0) { Append-EventText $sb.ToString() }
}
function Write-Result($obj) {
    $tmp = $ResultJson + '.tmp'
    [IO.File]::WriteAllText($tmp, ($obj | ConvertTo-Json -Depth 8))
    [IO.File]::Copy($tmp, $ResultJson, $true)
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}
function New-Result([string]$status, [string]$error) {
    return @{
        schema = 1
        requestId = $script:rid
        status = $status
        effectiveGameDir = $script:gameDir
        resolvedProxy = $script:resolved
        candidates = $script:candidates
        conflicts = $script:conflicts
        requiredDecisions = $script:needed
        logPath = $EventsJsonl
        error = $error
        fingerprint = $script:fingerprint
        notes = $script:notes
        partial = $script:partial
        recommend = $script:recommend
        backends = $script:backends
        packageRoot = $script:packageRoot
        compatibility = $script:compatibility
        mochizukiInstall = $script:mochizukiInstall
        installedMode = $script:installedMode
        recommendedMode = $script:recommendedMode
        recommendedReshadeApi = $script:recommendedReshadeApi
        detectedApi = $script:detectedApi
        requiresAdmin = $script:requiresAdmin
    }
}
function Fail-Result([string]$status, [string]$error, [int]$code) {
    if ($Elevated -and $error) { Write-Host ("[{0}] {1}" -f $status, $error) -ForegroundColor Red }
    Write-Result (New-Result $status $error)
    exit $code
}
function Find-Sibling([string]$name) {
    $dirs = @(
        $here,
        (Split-Path $here -Parent),
        (Join-Path (Split-Path $here -Parent) 'tools'),
        (Join-Path $here '..\..\tools')
    )
    foreach ($dir in $dirs) {
        try {
            $p = [IO.Path]::GetFullPath((Join-Path $dir $name))
            if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
        } catch { }
    }
    return $null
}

if (-not [IO.Path]::IsPathRooted($RequestJson) -or -not [IO.Path]::IsPathRooted($EventsJsonl) -or -not [IO.Path]::IsPathRooted($ResultJson)) {
    throw 'RequestJson, EventsJsonl and ResultJson must be absolute paths'
}
$req = Get-Content -LiteralPath $RequestJson -Raw -Encoding UTF8 | ConvertFrom-Json
if ([int]$req.schema -ne 1) { throw "unsupported schema $($req.schema)" }
$script:rid = [string]$req.requestId
$script:gameDir = ''
$script:resolved = $null
$script:candidates = @()
$script:conflicts = @()
$script:needed = @()
$script:notes = @()
$script:fingerprint = $null
$script:partial = $false
$script:mochizukiInstall = $null
$script:installedMode = ''
$script:recommendedMode = ''
$script:recommendedReshadeApi = ''
$script:detectedApi = ''
$script:requiresAdmin = $false
$allowedProxy = @($CandidateNames)
$allowedAction = @('inspect','install','update','uninstall','scan','repairXess','restoreXess','keepXess')
$allowedBackend = @('auto','mochizuki','all','none')
if ($allowedAction -notcontains [string]$req.action) { throw "invalid action $($req.action)" }

# An explicitly empty list must not fall through to automatic/all-backend installation.
$hasRequestedBackends = $null -ne $req.backends
if ($req.action -in @('install','update')) {
    if ($hasRequestedBackends) {
        if (@($req.backends).Count -eq 0) { Fail-Result 'failed' 'Select at least one backend.' 1 }
        if ($req.backendChoice -and $req.backendChoice -ne 'auto') {
            Fail-Result 'failed' 'Pass either backendChoice or backends, not both.' 1
        }
        $seenRequestedBackends = @{}
        foreach ($id in @($req.backends)) {
            if ($id -notin @('mochizuki','none')) { Fail-Result 'failed' "Unknown backend id: $id" 1 }
            if ($seenRequestedBackends.ContainsKey($id)) { Fail-Result 'failed' "Duplicate backend id: $id" 1 }
            $seenRequestedBackends[$id] = $true
        }
        if ($req.activeBackend -and -not $seenRequestedBackends.ContainsKey([string]$req.activeBackend)) {
            Fail-Result 'failed' 'Active backend must be in the selected backends.' 1
        }
    } elseif ($req.activeBackend) {
        Fail-Result 'failed' 'Active backend requires an explicit backends list.' 1
    }
}

if ($req.action -eq 'scan') {
    Write-Event 'scan' 'Scanning game libraries...'
    $scan = Join-Path $here 'library-scan.ps1'
    if (-not (Test-Path -LiteralPath $scan)) { $scan = Find-Sibling 'library-scan.ps1' }
    if (-not $scan) { Fail-Result 'failed' 'library-scan.ps1 is missing from the package.' 1 }
    . $scan
    $games = @(Get-OptScalerGameLibrary)
    $obj = New-Result 'success' $null
    $obj.games = $games
    Write-Result $obj
    Write-Event 'done' "scan complete ($($games.Count) games found)"
    exit 0
}
$exe = [string]$req.gameExe
if (-not $exe -or -not [IO.Path]::IsPathRooted($exe)) { Fail-Result 'failed' 'gameExe must be an absolute path' 1 }
try { $exe = [IO.Path]::GetFullPath($exe) }
catch { Fail-Result 'failed' 'gameExe is not a valid absolute path' 1 }
if ($exe -match '\\WindowsApps(?:\\|$)') {
    Fail-Result 'failed' 'Select the executable in XboxGames, not WindowsApps.' 1
}
try {
    $item = Get-Item -LiteralPath $exe -Force -ErrorAction Stop
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail-Result 'failed' 'gameExe cannot be a reparse point' 1 }
} catch [UnauthorizedAccessException] {
    Write-Event 'inspect' 'selected exe is protected; using its folder'
} catch {
    $parent = [IO.Path]::GetDirectoryName($exe)
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        Fail-Result 'failed' 'gameExe not found' 1
    }
    Write-Event 'inspect' 'selected exe metadata unavailable; using its folder'
}

function Find-PackageRoot([string]$requested) {
    $dirs = @()
    if ($requested) { $dirs += $requested }
    else {
        $dirs += $here
        $dirs += (Split-Path $here -Parent)
    }
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        try { $d = [IO.Path]::GetFullPath($d) } catch { continue }
        $dll = Join-Path $d 'OptiScaler.dll'
        $setup = @(
            (Join-Path $d 'Setup.ps1'),
            (Join-Path $d 'install.ps1'),
            (Join-Path $d 'install-amd-presr.ps1')
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
        if ((Test-Path -LiteralPath $dll -PathType Leaf) -and $setup) { return $d }
    }
    return $null
}
function Find-Setup([string]$name) {
    # Scripts and runtime inputs must belong to the same selected package.
    if ($script:packageRoot) {
        $path = Join-Path $script:packageRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
        return $null
    }
    if ($req.packageRoot) { return $null }
    return Find-Sibling $name
}
if ($req.action -eq 'uninstall') {
    $script:gameDir = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($exe))
    $script:packageRoot = Find-PackageRoot ([string]$req.packageRoot)
    $uninstall = Find-Setup 'Uninstall_OptiScaler_NR.ps1'
    if (-not $uninstall) { $uninstall = Find-Setup 'uninstall.ps1' }
    if (-not $uninstall) { $uninstall = Find-Setup 'uninstall-amd-presr.ps1' }
    if (-not $uninstall) { Fail-Result 'failed' 'Complete package required: uninstall script is missing.' 1 }

    Write-Event 'uninstall-plan' 'checking paths and preparing uninstall'
    $ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $logDir = [IO.Path]::GetDirectoryName($ResultJson)
    $stderrLog = Join-Path $logDir 'setup.stderr.log'
    $stdout = [IO.StreamWriter]::new((Join-Path $logDir 'setup.stdout.log'), $false, [Text.UTF8Encoding]::new($false))
    $stdout.AutoFlush = $true
    $errorLines = [Collections.Generic.List[string]]::new()
    $reportErrors = $false
    try {
        $ErrorActionPreference = 'Continue'
        & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $uninstall -GameDir $script:gameDir -NonInteractive -NoPause -RemoveBackups 2>$stderrLog | ForEach-Object {
            $stdout.WriteLine([string]$_)
            if ($_ -eq 'Removing installed files...') {
                Write-Event 'uninstall' ([string]$_)
            } else {
                Write-Event 'log' ([string]$_)
            }
            if ($_ -eq 'Errors:') { $reportErrors = $true }
            elseif ($reportErrors -and $_ -match '^  - ') { $errorLines.Add($_.Trim()) }
            elseif ($_ -match '^ERROR: ') { $errorLines.Add([string]$_) }
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
        $stdout.Dispose()
    }
    if (Test-Path -LiteralPath $stderrLog) {
        $stderr = @(Get-Content -LiteralPath $stderrLog)
        Write-EventBatch 'error' $stderr
        $errorLines.AddRange([string[]]$stderr)
    }
    if ($code -eq 0) {
        Write-Event 'done' 'uninstall complete'
        Fail-Result 'success' $null 0
    }
    Write-Event 'done' "uninstall failed ($code)"
    Fail-Result 'failed' ("Uninstall exit $code " + (($errorLines | Select-Object -First 5) -join [Environment]::NewLine)).Trim() 1
}

Write-Event 'inspect' 'reading PE imports and local dependencies'
$probe = Get-ProxyRecommendation $exe
$script:gameDir = [string]$probe.gameDir
$script:candidates = @($probe.candidates)
$script:conflicts = @($probe.conflicts)
$script:fingerprint = $probe.fingerprint
$script:partial = [bool]$probe.partial
if ($probe.installedMode) { $script:installedMode = [string]$probe.installedMode }
if ($probe.recommendedMode) { $script:recommendedMode = [string]$probe.recommendedMode }
if ($probe.recommendedReshadeApi) { $script:recommendedReshadeApi = [string]$probe.recommendedReshadeApi }
if ($probe.detectedApi) { $script:detectedApi = [string]$probe.detectedApi }
$script:requiresAdmin = [bool]$probe.requiresAdmin

# A Mochizuki (DLSSNR-AMD) installation of its own in the game folder. Only files that can
# actually conflict are reported and later moved: his marker files (dlssnr_core.dll,
# dlssnr_amd.addon32/64, OptiScaler.asi -- an OptiScaler or an ASI loader picks these up), his
# proxy DLL when its name is one of this project's load-entry candidate names (both
# installations would hook the same entry), and his dlssnr-amd-install.txt record (so the
# detection clears together with the installation). The rest of his record is inert and
# stays put: LICENSE, OptiScaler.ini (the installer's own ini decision owns that), his
# disabled nvngx backups, and the dlssnr-amd\ model files this project shares. Read-only
# here: the launcher warns, and only an explicit "remove" decision moves files aside.
function Get-MochizukiInstall([string]$dir) {
    $presrMark = Join-Path $dir 'amd-presr-install.txt'
    if (Test-Path -LiteralPath $presrMark -PathType Leaf) {
        try {
            if ([IO.File]::ReadAllText($presrMark, [Text.Encoding]::UTF8) -match '(?m)^project=OptScaler\(NR\)\r?$') {
                return $null
            }
        } catch { }
    }
    $files = New-Object System.Collections.Generic.List[string]
    $proxy = ''
    $record = Join-Path $dir 'dlssnr-amd-install.txt'
    $recorded = $false
    if (Test-Path -LiteralPath $record -PathType Leaf) {
        $lines = @()
        try { $lines = [IO.File]::ReadAllLines($record) } catch { }
        $entries = @()
        foreach ($raw in $lines) {
            $line = $raw.TrimStart([char]0xFEFF)
            if ($line.Length -lt 3 -or $line[1] -ne ' ') { continue }
            $rel = $line.Substring(2).Trim()
            if (-not $rel -or [IO.Path]::IsPathRooted($rel) -or $rel -match '(^|[\\/])\.\.([\\/]|$)') { continue }
            $entries += ,@($line[0], $rel)
        }
        # His record, not this project's: it names his core, his add-on, his ASI or standalone markers.
        $recorded = @($entries | Where-Object { $_[0] -eq 'F' -and $_[1] -in @('dlssnr_core.dll','dlssnr_amd.addon32','dlssnr_amd.addon64','OptiScaler.asi','dlssnr-amd.asi','vk-override\implicit_layer\dlssnr_standalone.json','vk-override/implicit_layer/dlssnr_standalone.json') }).Count -gt 0
        if ($recorded) {
            foreach ($e in $entries) {
                if ($e[0] -ne 'F') { continue }
                # Only a load-entry name can collide with this project's proxy; his other
                # files (the nvngx pair, LICENSE, ini, dlssnr-amd\ models) are inert here.
                if ($CandidateNames -notcontains $e[1].ToLowerInvariant()) { continue }
                if (-not (Test-Path -LiteralPath (Join-Path $dir $e[1]) -PathType Leaf)) { continue }
                $proxy = $e[1]
                if (-not $files.Contains($e[1])) { $files.Add($e[1]) }
            }
            if (-not $files.Contains('dlssnr-amd-install.txt')) { $files.Add('dlssnr-amd-install.txt') }
        }
    }
    foreach ($name in @('dlssnr_core.dll','OptiScaler.asi','dlssnr_amd.addon32','dlssnr_amd.addon64','dlssnr-amd.asi','vk-override\implicit_layer\dlssnr_standalone.json')) {
        if ((Test-Path -LiteralPath (Join-Path $dir $name) -PathType Leaf) -and -not $files.Contains($name)) { $files.Add($name) }
    }
    # Only the record (or nothing) is left: the installation is already dismantled and the
    # remaining files are inert. Not a conflict -- no warning, no prompt, nothing to move.
    if (@($files | Where-Object { $_ -ne 'dlssnr-amd-install.txt' }).Count -eq 0) { return $null }
    return @{
        detected = $true
        recorded = $recorded
        files = @($files)
        proxy = $proxy
    }
}
try { $script:mochizukiInstall = Get-MochizukiInstall $script:gameDir }
catch { $script:notes += "Mochizuki installation check failed: $($_.Exception.Message)" }
if ($script:mochizukiInstall) {
    $script:notes += ('A Mochizuki (DLSSNR-AMD) installation is in the game folder: {0}. The same kind of NR mod twice can conflict; the installer leaves these files alone.' -f (@($script:mochizukiInstall.files) -join ', '))
}
$choice = [string]$req.proxyChoice
if (-not $choice -or $choice -eq 'auto') {
    if ($probe.recommend) { $script:resolved = [string]$probe.recommend }
    else { $script:resolved = $null }
} elseif ($allowedProxy -contains $choice) {
    $script:resolved = $choice
} else {
    Fail-Result 'failed' 'invalid proxyChoice' 1
}
$backend = [string]$req.backendChoice
if ($backend -and $allowedBackend -notcontains $backend) { Fail-Result 'failed' 'invalid backendChoice' 1 }

$script:packageRoot = Find-PackageRoot ([string]$req.packageRoot)
$pkg = $script:packageRoot
$compatScript = Join-Path $here 'xess-compat.ps1'
if (Test-Path -LiteralPath $compatScript -PathType Leaf) {
    . $compatScript
    function Read-XessCompatibilityReport {
        try { return Get-XessCompatibility $script:gameDir $pkg }
        catch {
            $failure = $_.Exception.Message
            $recovery = @{ active = $false; transaction = ''; fingerprint = ''; restoreFiles = @() }
            try { $recovery = Get-XessRecoveryInfo $script:gameDir } catch { }
            $recovery.error = $failure
            $recovery.groups = @()
            return $recovery
        }
    }
    $script:compatibility = Read-XessCompatibilityReport
} else {
    $script:compatibility = @{ error = 'Complete package required: xess-compat.ps1 is missing. Extract the full package again.'; groups = @(); active = $false }
}
if ($req.action -in @('repairXess','restoreXess','keepXess')) {
    if (-not (Test-Path -LiteralPath $compatScript -PathType Leaf)) { Fail-Result 'failed' 'Complete package required: xess-compat.ps1 is missing.' 1 }
    if ($probe.blockInstall) { Fail-Result 'failed' $probe.hint 1 }
    if ($req.action -eq 'repairXess' -and $probe.pe.bitness -eq 32) {
        Fail-Result 'failed' 'XeSS repair requires a 64-bit game executable.' 1
    }
    Write-Event 'validate' 'Close the game before repairing or restoring XeSS.'
    try {
        if ($req.action -ne 'repairXess' -and (-not $req.compatibilityFingerprint -or
            $req.compatibilityFingerprint -cne $script:compatibility.fingerprint)) {
            throw 'The repair record or files changed. Reopen Compatibility repair and review the file list again.'
        }
        if ($req.action -eq 'repairXess') {
            Write-Event 'backup' 'Backing up game XeSS files before replacement'
            Repair-XessCompatibility $script:gameDir $pkg @($req.explicitDecisions.xessGroups) ([string]$req.compatibilityFingerprint)
        } elseif ($req.action -eq 'restoreXess') {
            Write-Event 'backup' 'Restoring original game XeSS files'
            Restore-XessCompatibility $script:gameDir ([string]$script:compatibility.transaction)
        } else {
            Complete-XessCompatibility $script:gameDir ([string]$script:compatibility.transaction)
        }
    } catch {
        $failure = $_.Exception.Message
        $script:compatibility = Read-XessCompatibilityReport
        Fail-Result 'failed' $failure 1
    }
    $script:compatibility = Read-XessCompatibilityReport
    Write-Event 'done' "$($req.action) complete; restart the game to test"
    Fail-Result 'success' $null 0
}
if ($pkg) {
    $hasAsi = Test-Path -LiteralPath (Join-Path $pkg 'OptiScaler/plugins/dlssnr-amd.asi')
    $hasExtract = Test-Path -LiteralPath (Join-Path $pkg 'model-tools/dlssnr_extract_model.exe')
    $hasModel = (Test-Path -LiteralPath (Join-Path $pkg 'dlssnr-amd/CC_SILVER_AARDWOLD.bin')) -or
        (Test-Path -LiteralPath (Join-Path $pkg 'dlssnr-amd/CC_SMALL_FP8.bin'))
    $hasNv = Test-Path -LiteralPath (Join-Path $pkg 'nvngx_dlssnr.dll')
    $script:notes += ('Mochizuki NR ASI present: {0}; extractor present: {1}.' -f $hasAsi, $hasExtract)
    $script:notes += ('Package model file present: {0}. That is not a GPU compatibility test.' -f $hasModel)
    $script:notes += ('NVIDIA DLL for optional extraction present: {0}.' -f $hasNv)
}

$script:recommend = $probe.recommend
$script:backends = @()
$capScript = Join-Path $here 'install-capabilities.ps1'
if (-not (Test-Path -LiteralPath $capScript)) { $capScript = Find-Sibling 'install-capabilities.ps1' }
if ($capScript -and (Test-Path -LiteralPath $capScript)) {
    . $capScript
    $sourceDll = [string]$req.explicitDecisions.modelSourceDll
    if ($sourceDll -and (-not [IO.Path]::IsPathRooted($sourceDll) -or -not (Test-Path -LiteralPath $sourceDll -PathType Leaf))) {
        Fail-Result 'failed' 'Select an existing nvngx_dlssnr.dll using an absolute path.' 1
    }
    $script:backends = @(Get-OptScalerBackendCapabilities $script:packageRoot $script:gameDir $sourceDll)
}


if ($req.action -eq 'inspect') {
    if ($probe.hint) { $script:notes += $probe.hint }
    Write-Event 'done' 'inspect complete'
    Fail-Result 'success' $null 0
}

if ($req.action -in @('install','update') -and $req.fingerprint -and $req.fingerprint -ne $script:fingerprint) {
    $script:needed += @{ id = 'reinspect'; prompt = 'Game folder files changed since the last inspect. Inspect again.'; options = @('ok') }
    Fail-Result 'needsInput' 'stale inspect fingerprint' 2
}

$install = Find-Setup 'Setup.ps1'
if (-not $install) { $install = Find-Setup 'install.ps1' }
if (-not $install) { $install = Find-Setup 'install-amd-presr.ps1' }

if (-not $install) {
    Fail-Result 'failed' 'Complete package required: setup scripts are missing. Unzip the full package again.' 1
}

function ConvertTo-ForeignProxyMap($raw) {
    $map = @{}
    if ($null -eq $raw) { return $map }
    $items = @($raw)
    if ($items.Count -gt 0 -and $null -ne $items[0].PSObject.Properties['Key']) {
        foreach ($row in $items) {
            if ($row.Key) { $map[[string]$row.Key] = [string]$row.Value }
        }
        return $map
    }
    foreach ($p in $raw.PSObject.Properties) {
        $map[[string]$p.Name] = [string]$p.Value
    }
    return $map
}

$dec = $req.explicitDecisions
$installMode = ''
if ($req.PSObject.Properties['installMode'] -and $req.installMode) { $installMode = [string]$req.installMode }
elseif ($dec -and $dec.PSObject.Properties['installMode'] -and $dec.installMode) { $installMode = [string]$dec.installMode }
if (-not $installMode) { $installMode = 'optiscaler' }
if ($installMode -notin @('optiscaler','rtx-remix-mc','standalone','reshade-feeder','reshade-feeder-dx9','reshade-feeder-vulkan')) {
    Fail-Result 'failed' "Unknown installMode: $installMode" 1
}
$isRtxRemixMc = ($installMode -eq 'rtx-remix-mc')
$isOptiMode = ($installMode -eq 'optiscaler' -or $isRtxRemixMc)
$requestedBitness = if ($req.PSObject.Properties['gameBitness']) { [string]$req.gameBitness } else { '0' }
if ($requestedBitness -notin '0', '32', '64') { Fail-Result 'failed' 'Invalid gameBitness.' 1 }
$gameBitness = if (-not $isRtxRemixMc -and $requestedBitness -ne '0') { [int]$requestedBitness }
    elseif ($probe.pe.bitness -eq 32) { 32 } else { 64 }

if ($req.action -in @('install','update')) {
    if ($probe.blockInstall) { Fail-Result 'failed' $probe.hint 1 }
    if ($gameBitness -eq 32 -and $installMode -notlike 'reshade-feeder*') {
        Fail-Result 'failed' '32-bit games support ReShade mode only.' 1
    }
    if ($isOptiMode -and -not $isRtxRemixMc -and -not $script:resolved) { Fail-Result 'needsInput' 'No proxy recommendation is available. Select a DLL explicitly.' 2 }
    if (-not $script:packageRoot) {
        Fail-Result 'failed' 'Complete package required: OptiScaler.dll was not found next to the installer. Unzip the full package and run Setup.bat from that folder.' 1
    }
}
$mochiAvailable = @($script:backends | Where-Object { $_.id -eq 'mochizuki' -and $_.runtimeState -eq 'ready' }).Count -gt 0
$mochiSelected = if (-not $isOptiMode) { $true } elseif ($hasRequestedBackends) { @($req.backends) -contains 'mochizuki' } else {
    $backend -eq 'mochizuki' -or (($backend -in @('auto','all','')) -and $mochiAvailable)
}
if ($mochiSelected) {
    $modelChoice = if ($dec) { [string]$dec.mochizukiModels } else { '' }
    $networkChoice = if ($dec -and $dec.mochizukiNetworks) { @($dec.mochizukiNetworks | ForEach-Object { [string]$_ }) } else { @() }
    $activeChoice = if ($dec) { [string]$dec.mochizukiActiveModel } else { '' }
    if (@($networkChoice | Where-Object { $_ -notin @('small','experimental','original','int4','optimized') }).Count -gt 0) {
        Fail-Result 'failed' 'Invalid Mochizuki network selection.' 1
    }
    if (-not $networkChoice -and -not $modelChoice) {
        $script:needed += @{
            id = 'mochizukiNetworks'
            prompt = 'Select the Mochizuki networks to install: small, experimental, original, int4 (or all).'
            options = @('small','experimental','original','int4')
        }
    } elseif ($modelChoice -and $modelChoice -notin @('both','all','small','experimental','original','int4','optimized')) {
        Fail-Result 'failed' 'Invalid Mochizuki models choice.' 1
    }
    if ($activeChoice -and $activeChoice -notin @('small','experimental','original','int4','optimized')) {
        Fail-Result 'failed' 'Invalid Mochizuki active network.' 1
    }
}
$openRrExplicitlyRequested = $isOptiMode -and $dec -and $dec.PSObject.Properties['openRr'] -and ([string]$dec.openRr -eq 'install')
if ($openRrExplicitlyRequested) {
    $hasValidDlssd = $false
    $dlssdCandidates = @()
    if ($script:packageRoot) {
        $dlssdCandidates += @(
            (Join-Path $script:packageRoot 'nvngx_dlssd.dll'),
            (Join-Path $script:packageRoot 'openrr-amd\nvngx_dlssd.dll'),
            (Join-Path $script:packageRoot 'opennr-rr\nvngx_dlssd.dll')
        )
    }
    if ($script:gameDir) {
        $dlssdCandidates += @(
            (Join-Path $script:gameDir 'nvngx_dlssd.dll'),
            (Join-Path $script:gameDir 'openrr-amd\nvngx_dlssd.dll'),
            (Join-Path $script:gameDir 'opennr-rr\nvngx_dlssd.dll')
        )
    }
    foreach ($cand in $dlssdCandidates) {
        if ($cand -and (Test-Path -LiteralPath $cand -PathType Leaf)) {
            try {
                $v = (Get-Item -LiteralPath $cand).VersionInfo
                if ($v.FileMajorPart -eq 310 -and $v.FileMinorPart -eq 9 -and $v.FileBuildPart -eq 1) {
                    $hasValidDlssd = $true
                    break
                }
            } catch { }
        }
    }
    if (-not $hasValidDlssd) {
        $script:needed += @{
            id = 'openRrDlssd'
            prompt = 'OpenRR requires nvngx_dlssd.dll (310.9.1.0). Provide nvngx_dlssd.dll or uncheck OpenRR.'
            options = @('import','cancel')
        }
    }
}
if ($isOptiMode -and -not $isRtxRemixMc -and $probe.conflicts.Count -gt 0) {
    $map = @{}
    if ($dec -and $dec.foreignProxy) { $map = ConvertTo-ForeignProxyMap $dec.foreignProxy }
    foreach ($c in $probe.conflicts) {
        $name = $c.name
        $has = $map.ContainsKey($name) -or $map.ContainsKey($name.ToLowerInvariant())
        if (-not $has) {
            $script:needed += @{
                id = 'foreignProxy'
                file = $name
                prompt = "$name exists and is not this project. Backup, keep, or cancel?"
                options = @('backup','keep','cancel')
            }
        }
    }
}
if ($script:needed.Count -gt 0) {
    Write-Event 'validate' 'waiting for explicit decisions'
    Fail-Result 'needsInput' 'missing decisions' 2
}
Write-Event 'validate' 're-checking paths and locks'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$stdoutLog = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'setup.stdout.log'
$stderrLog = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'setup.stderr.log'
$dj = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'decisions.json'
$payload = @{
    existingInstall = 'overwrite'
    existingIni = 'keep'
    shortFileContinue = 'abort'
    optiPatcher = 'skip'
    installMode = $installMode
    language = if ($req.language -eq 'zh') { 'zh' } else { 'en' }
}
if ($dec) {
    foreach ($name in @('existingInstall','existingIni','mochizukiModels','mochizukiNetworks','mochizukiActiveModel','shortFileContinue','optiPatcher','openRr','openRrSpoof','foreignProxy','modelSourceDll','installMode','renoDX','lumenite','lumeniteZip')) {
        if ($dec.PSObject.Properties[$name] -and $null -ne $dec.$name) {
            if ($name -eq 'foreignProxy') { $payload[$name] = ConvertTo-ForeignProxyMap $dec.$name }
            else { $payload[$name] = $dec.$name }
        }
    }
}
if (-not $isOptiMode) {
    $payload['openRr'] = 'skip'
    $payload['optiPatcher'] = 'skip'
    $payload['backends'] = @('mochizuki')
    $payload['installMode'] = $installMode
} else {
    if ($isRtxRemixMc) {
        $payload['optiPatcher'] = 'skip'
    }
    if ($hasRequestedBackends) {
        $payload['backends'] = @($req.backends)
    }
}
($payload | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $dj -Encoding UTF8
Write-Event 'install' "starting $($req.action) ($installMode)"
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$proxyForSetup = if ($isRtxRemixMc) { 'dxgi.dll' } elseif ($script:resolved) { $script:resolved } else { 'dxgi.dll' }
$modeArgs = @('-InstallMode', $installMode)
if ($gameBitness -eq 32) { $modeArgs += @('-GameBitness', '32') }
if ($dec -and $dec.PSObject.Properties['renoDX'] -and $dec.renoDX) {
    $modeArgs += @('-RenoDX', [string]$dec.renoDX)
}
if ($dec -and $dec.PSObject.Properties['lumenite'] -and $dec.lumenite) {
    $modeArgs += @('-Lumenite', [string]$dec.lumenite)
}
if ($dec -and $dec.PSObject.Properties['lumeniteZip'] -and $dec.lumeniteZip) {
    $modeArgs += @('-LumeniteZip', [string]$dec.lumeniteZip)
}
if (-not $isOptiMode) {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $proxyForSetup -NoPause -NonInteractive -Root $script:packageRoot @modeArgs -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
    $code = $LASTEXITCODE
} elseif ($hasRequestedBackends) {
    $activeArguments = @()
    if ($req.activeBackend) { $activeArguments = @('-ActiveBackend', [string]$req.activeBackend) }
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $proxyForSetup -NoPause -NonInteractive -Root $script:packageRoot @modeArgs @activeArguments -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
    $code = $LASTEXITCODE
} elseif ($backend -and $backend -ne 'auto') {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $proxyForSetup -NoPause -NonInteractive -Root $script:packageRoot @modeArgs -Backend $backend -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
    $code = $LASTEXITCODE
} else {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $proxyForSetup -NoPause -NonInteractive -Root $script:packageRoot @modeArgs -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
    $code = $LASTEXITCODE
}
$ErrorActionPreference = $prevEap
if (Test-Path -LiteralPath $stdoutLog) {
    Write-EventBatch 'log' @(Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue)
}
if (Test-Path -LiteralPath $stderrLog) {
    Write-EventBatch 'error' @(Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue)
}
if ($code -eq 0) {
    Write-Event 'verify' 'setup process exit 0'
    Write-Event 'done' "$($req.action) complete"
    Fail-Result 'success' $null 0
}
Write-Event 'done' "$($req.action) failed ($code)"
$tailParts = [Collections.Generic.List[string]]::new()
if (Test-Path -LiteralPath $stdoutLog) {
    $errLines = @(Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue | Where-Object { $_ -match '^ERROR: ' } | Select-Object -First 5)
    if ($errLines.Count -gt 0) { $tailParts.Add(($errLines -join [Environment]::NewLine)) }
}
if (Test-Path -LiteralPath $stderrLog) {
    $stderrRaw = Get-Content -LiteralPath $stderrLog -Raw -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($stderrRaw)) { $tailParts.Add($stderrRaw.Trim()) }
}
$tail = ($tailParts -join [Environment]::NewLine).Trim()
Fail-Result 'failed' ("setup exit $code $tail").Trim() 1
