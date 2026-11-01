# Compile OptScalerNR.Launcher.exe with the machine Framework 4 WPF assemblies.
# Output: exports/launcher/ (never the repo root).
[CmdletBinding()]
param(
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'tools/install/launcher'
$profileCatalog = Get-Content -LiteralPath (Join-Path $root 'assets/compatibility/proxy-profiles.json') -Raw -Encoding UTF8 | ConvertFrom-Json
# The embedded catalog is authoritative. Keep PowerShell's parameter/probe aliases in contract.
$profileExpected = @($profileCatalog.proxies | Sort-Object)
foreach ($proxyContract in @(
    @{ Path = 'tools/install/proxy-probe.ps1'; Pattern = '^\$CandidateNames\s*=' },
    @{ Path = 'tools/install-amd-presr.ps1'; Pattern = 'ValidateSet.*dxgi\.dll' },
    @{ Path = 'tools/uninstall-amd-presr.ps1'; Pattern = '^\$proxyNames\s*=' }
)) {
    $proxyLine = Get-Content -LiteralPath (Join-Path $root $proxyContract.Path) -Encoding UTF8 |
        Where-Object { $_ -match $proxyContract.Pattern } | Select-Object -First 1
    $proxyActual = @([regex]::Matches([string]$proxyLine, "'([^']+\.dll)'") | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    if (($proxyActual -join '|') -ne ($profileExpected -join '|')) { throw "Proxy catalog mismatch: $($proxyContract.Path)" }
}
if (-not $OutDir) { $OutDir = Join-Path $root 'exports/launcher' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Join-Path $fw 'csc.exe'
$wpf = Join-Path $fw 'WPF'
foreach ($need in @($csc, (Join-Path $wpf 'PresentationFramework.dll'))) {
    if (-not (Test-Path -LiteralPath $need)) { throw "Missing compiler dependency: $need" }
}

$exe = Join-Path $OutDir 'OptScalerNR.Launcher.exe'
$obj = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$cs = @(
    'Program.cs', 'MainWindow.cs', 'Protocol.cs', 'WorkerClient.cs', 'GamePaths.cs',
    'DialogTheme.cs', 'FolderGameScanner.cs', 'ProxyRecommendations.cs',
    'MainController.Library.cs', 'MainController.Recommendations.cs', 'LauncherTheme.cs', 'MainController.Presentation.cs',
    'MainController.Profiles.cs', 'MainController.ShareCode.cs', 'MainController.ModelImport.cs', 'MochizukiShareCode.cs', 'MainController.Diagnostics.cs', 'MainController.LogPack.cs', 'IniProfiles.cs', 'LocalArtwork.cs'
    'MainController.UnlockerPlugin.cs', 'UnlockerPlugin.cs'
) | ForEach-Object { Join-Path $src $_ }
$refs = @(
    (Join-Path $fw 'mscorlib.dll'),
    (Join-Path $fw 'System.dll'),
    (Join-Path $fw 'System.Core.dll'),
    (Join-Path $fw 'System.Runtime.Serialization.dll'),
    (Join-Path $fw 'System.Xaml.dll'),
    (Join-Path $wpf 'WindowsBase.dll'),
    (Join-Path $wpf 'PresentationCore.dll'),
    (Join-Path $wpf 'PresentationFramework.dll'),
    (Join-Path $fw 'System.Drawing.dll'),
    (Join-Path $fw 'System.IO.Compression.dll'),
    (Join-Path $fw 'System.IO.Compression.FileSystem.dll')
)
$win32 = Join-Path $fw 'Microsoft.Build.Tasks.v4.0.dll'
$res = @(
    "/resource:`"$(Join-Path $src 'App.xaml')`",OptScalerNR.Launcher.App.xaml",
    "/resource:`"$(Join-Path $src 'MainWindow.xaml')`",OptScalerNR.Launcher.MainWindow.xaml",
    "/resource:`"$(Join-Path $src 'DialogTheme.xaml')`",OptScalerNR.Launcher.DialogTheme.xaml",
    "/resource:`"$(Join-Path $root 'assets/compatibility/proxy-profiles.json')`",OptScalerNR.Launcher.proxy-profiles.json"
)
$argFile = Join-Path $obj 'csc.rsp'
$lines = @(
    '/nologo',
    '/t:winexe',
    '/platform:x64',
    '/optimize+',
    '/debug:pdbonly',
    "/out:`"$exe`"",
    "/win32manifest:`"$(Join-Path $src 'app.manifest')`"",
    "/define:TRACE"
)
$lines += $refs | ForEach-Object { "/r:`"$_`"" }
$lines += $res
$lines += $cs | ForEach-Object { "`"$_`"" }
Set-Content -LiteralPath $argFile -Value $lines -Encoding ASCII
& $csc "@$argFile"
if ($LASTEXITCODE -ne 0) { throw "csc failed: $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Launcher EXE was not produced" }
Copy-Item -LiteralPath (Join-Path $src 'mock-worker.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $src 'PROTOCOL.md') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/launcher-worker.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/proxy-probe.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/library-scan.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/install-capabilities.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/xess-compat.ps1') -Destination $OutDir -Force
foreach ($name in @('lmxxf-small.ps1','lmxxf-small-manifest.json')) {
    Copy-Item -LiteralPath (Join-Path $root "tools/install/$name") -Destination $OutDir -Force
}
$srcFiles = Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -match '\.(cs|xaml|manifest|md|ps1)$' }
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'assets/compatibility/proxy-profiles.json')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/launcher-worker.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/proxy-probe.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/library-scan.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/install-capabilities.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/xess-compat.ps1')
foreach ($name in @('lmxxf-small.ps1','lmxxf-small-manifest.json')) { $srcFiles += Get-Item -LiteralPath (Join-Path $root "tools/install/$name") }
$sha = [Security.Cryptography.SHA256]::Create()
$concat = New-Object IO.MemoryStream
foreach ($f in ($srcFiles | Sort-Object FullName)) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $concat.Write($bytes, 0, $bytes.Length)
}
$sourceHash = ([BitConverter]::ToString($sha.ComputeHash($concat.ToArray()))).Replace('-','').ToLowerInvariant()
function Get-Sha256File([string]$path) {
    $h = [Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($h.ComputeHash($fs))).Replace('-','').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $h.Dispose() }
}
$refHash = @{}
foreach ($r in $refs) {
    $refHash[[IO.Path]::GetFileName($r)] = Get-Sha256File $r
}
$stamp = Join-Path $OutDir 'build.json'
@{
    exe = $exe
    length = (Get-Item -LiteralPath $exe).Length
    utc = [DateTime]::UtcNow.ToString('o')
    csc = $csc
    sourceHash = $sourceHash
    references = $refHash
    target = 'netfx4-wpf-x64'
} | ConvertTo-Json | Set-Content -LiteralPath $stamp -Encoding UTF8
Write-Host "LAUNCHER_OK $exe"
