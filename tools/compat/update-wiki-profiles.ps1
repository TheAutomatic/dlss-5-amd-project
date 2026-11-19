# Regenerate assets/compatibility/proxy-profiles.json from the live OptiScaler Wiki.
# The output is produced by the launcher's own C# parser (ParseWikiCompatibilityMarkdown)
# merged over assets/compatibility/curated-profiles.json, so the bundled seed and a live
# in-app refresh yield the same data.
# Usage: powershell -File tools/compat/update-wiki-profiles.ps1 [-MarkdownPath <file>] [-ReviewedDate yyyy-MM-dd]
[CmdletBinding()]
param(
    [string]$MarkdownPath,
    [string]$ReviewedDate,
    [string]$LauncherExe
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wikiUrl = 'https://raw.githubusercontent.com/wiki/optiscaler/OptiScaler/Compatibility-List.md'
$markdownFile = if ($MarkdownPath) { Resolve-Path $MarkdownPath } else { Join-Path $env:TEMP ('optiscaler-wiki-' + [Guid]::NewGuid().ToString('N') + '.md') }
if (-not $MarkdownPath) {
    Write-Host "Downloading $wikiUrl"
    Invoke-WebRequest -Uri $wikiUrl -OutFile $markdownFile -UseBasicParsing
}
if (-not $LauncherExe) {
    $LauncherExe = Join-Path $root 'exports/launcher/OptiScalerNRLauncher.exe'
    if (-not (Test-Path -LiteralPath $LauncherExe)) {
        & (Join-Path $root 'tools/build/build-launcher.ps1') | Out-Null
    }
}
$LauncherExe = (Resolve-Path $LauncherExe).Path
Add-Type -AssemblyName WindowsBase
$assembly = [Reflection.Assembly]::LoadFrom($LauncherExe)
$type = $assembly.GetType('OptScalerNR.Launcher.ProxyRecommendations', $true)
$flags = [Reflection.BindingFlags]'Public,NonPublic,Static'
$jsonType = $assembly.GetType('OptScalerNR.Launcher.JsonUtil', $true)
$readJson = $jsonType.GetMethod('Read').MakeGenericMethod(@($assembly.GetType('OptScalerNR.Launcher.ProxyProfileDocument', $true)))
$writeJson = $jsonType.GetMethod('Write').MakeGenericMethod(@($assembly.GetType('OptScalerNR.Launcher.ProxyProfileDocument', $true)))
$validate = $type.GetMethod('Validate', $flags)
$parse = $type.GetMethod('ParseWikiCompatibilityMarkdown')

$curatedPath = Join-Path $root 'assets/compatibility/curated-profiles.json'
$curated = $readJson.Invoke($null, @([IO.File]::ReadAllText($curatedPath, [Text.Encoding]::UTF8)))
$markdown = [IO.File]::ReadAllText($markdownFile, [Text.Encoding]::UTF8)
$document = $parse.Invoke($null, @($markdown, $curated, $ReviewedDate))
$validate.Invoke($null, @($document)) | Out-Null
$payload = [string]$writeJson.Invoke($null, @($document))

function Format-Json([string]$json) {
    $builder = New-Object Text.StringBuilder
    $indent = 0
    $inString = $false
    $escape = $false
    foreach ($ch in $json.ToCharArray()) {
        if ($escape) { [void]$builder.Append($ch); $escape = $false; continue }
        if ($inString) {
            if ($ch -eq '\') { [void]$builder.Append($ch); $escape = $true; continue }
            if ($ch -eq '"') { $inString = $false }
            [void]$builder.Append($ch); continue
        }
        switch ($ch) {
            '"' { $inString = $true; [void]$builder.Append($ch) }
            '{' { [void]$builder.Append($ch); $indent++; [void]$builder.Append(("`n" + '  ' * $indent)) }
            '[' { [void]$builder.Append($ch); $indent++; [void]$builder.Append(("`n" + '  ' * $indent)) }
            '}' { $indent--; [void]$builder.Append(("`n" + '  ' * $indent + $ch)) }
            ']' { $indent--; [void]$builder.Append(("`n" + '  ' * $indent + $ch)) }
            ',' { [void]$builder.Append($ch); [void]$builder.Append(("`n" + '  ' * $indent)) }
            default { [void]$builder.Append($ch) }
        }
    }
    (($builder.ToString() -replace '\[\s*\]', '[]') -replace '\\/', '/') + "`n"
}

$target = Join-Path $root 'assets/compatibility/proxy-profiles.json'
[IO.File]::WriteAllText($target, (Format-Json $payload), (New-Object Text.UTF8Encoding($false)))
# Round-trip the written file through the same serializer the launcher uses.
$reloaded = $readJson.Invoke($null, @([IO.File]::ReadAllText($target, [Text.Encoding]::UTF8)))
$validate.Invoke($null, @($reloaded)) | Out-Null
if ($reloaded.Profiles.Length -ne $document.Profiles.Length) { throw 'Round-trip profile count mismatch' }
Write-Host ("PROFILES_OK {0} entries -> {1} (revision {2}, reviewed {3})" -f $document.Profiles.Length, $target, $document.Revision, $document.Reviewed)
if (-not $MarkdownPath) { Remove-Item -LiteralPath $markdownFile -ErrorAction SilentlyContinue }
