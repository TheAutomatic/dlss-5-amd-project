param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('original', 'copy-current', 'staging-current', 'staging-previous', 'proxy-original', 'split-original', 'off')]
    [string]$Mode,
    [string]$GameDir = 'C:\Program Files\yysls\yysls_medium\Engine\Binaries\Win64r - NR',
    [string]$ArchiveRoot
)
$ErrorActionPreference = 'Stop'
if (!$ArchiveRoot) { $ArchiveRoot = Join-Path $PSScriptRoot '..\exports\lmxxf-color-probe\runs' }
$gameRoot = (Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\')
$running = Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and $_.ExecutablePath.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}
if ($running) { throw 'Exit the game and its local launcher before switching diagnostics.' }
$ini = Join-Path $gameRoot 'OptiScaler.ini'
$lines = [Collections.Generic.List[string]]::new()
$lines.AddRange([IO.File]::ReadAllLines($ini))
$section = -1
for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -match '^\s*\[DlssNr\]\s*$') { $section = $i; break }
}
if ($section -lt 0) { throw 'Missing [DlssNr] section; configuration left unchanged.' }
$end = $section + 1
while ($end -lt $lines.Count -and $lines[$end] -notmatch '^\s*\[') { ++$end }
$settings = [ordered]@{ NrBackend = 'lmxxf'; LmxxfDiagnostic = $Mode; Enabled = 'true'; RunBeforeSR = 'true' }
foreach ($key in $settings.Keys) {
    $found = $false
    for ($i = $section + 1; $i -lt $end; ++$i) {
        if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) {
            $lines[$i] = "$key = $($settings[$key])"
            $found = $true
        }
    }
    if (!$found) { $lines.Insert($end, "$key = $($settings[$key])"); ++$end }
}
# Preserve the previous run before the next launch overwrites OptiScaler.log.
$archive = Join-Path $ArchiveRoot ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-before-' + $Mode)
New-Item -ItemType Directory -Path $archive -Force | Out-Null
foreach ($name in @('OptiScaler.ini', 'OptiScaler.log', 'amd_bridge.log', 'amd_presr.log')) {
    $source = Join-Path $gameRoot $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $archive }
}
$tempIni = Join-Path $gameRoot ('OptiScaler.ini.probe-' + [guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllLines($tempIni, $lines, [Text.UTF8Encoding]::new($true))
Move-Item -LiteralPath $tempIni -Destination $ini -Force
Write-Output "Ready: NrBackend=lmxxf LmxxfDiagnostic=$Mode; restart game."
Write-Output "Previous configuration and logs: $archive"
if ($Mode -eq 'off') { Write-Output 'off requests NR; missing same-frame boundary keeps original Color (NO NR). No delayed fallback.' }
if ($Mode -in @('proxy-original', 'split-original')) { Write-Output 'Boundary diagnostic only: original Color, no copy, no runtime/HIP. Require positive proxy/split evidence in the log.' }
