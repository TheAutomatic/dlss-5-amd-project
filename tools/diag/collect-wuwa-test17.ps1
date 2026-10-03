param(
    [string]$GameDirectory = $PSScriptRoot,
    [string]$CaptureDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'Lmxxf-WuWa-test17'),
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
if (-not $OutputDirectory) { $OutputDirectory = $gamePath }
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
$reports = @(Get-ChildItem -LiteralPath $CaptureDirectory -Filter 'capture-*.nrhl.info.txt' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
if ($reports.Count -eq 0) { throw 'No saved test17 capture. Finish F9 capture before collecting.' }
if ($reports[0].Name -notmatch '^capture-(\d+)-') { throw 'Invalid capture filename.' }
$sessionPid = $Matches[1]
$reports = @($reports | Where-Object { $_.Name -like "capture-$sessionPid-*" })
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$folder = Join-Path $outputPath "WuWa-test17-results-$stamp"
New-Item -ItemType Directory -Path $folder | Out-Null
$summary = [Collections.Generic.List[string]]::new()
$modes = @{}
function Test-CaptureFile([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(8))
        if ($magic -ne "NRHLV2`0`0") { throw "Invalid test17 capture: $Path" }
        $records = 0
        while ($stream.Position -lt $stream.Length) {
            $header = $reader.ReadBytes(76)
            if ($header.Length -ne 76) { throw "Truncated capture header: $Path" }
            $w = [BitConverter]::ToUInt32($header,28)
            $h = [BitConverter]::ToUInt32($header,32)
            $format = [BitConverter]::ToUInt32($header,36)
            $bpp = switch ($format) { 2 {16} 10 {8} 41 {4} 54 {2} default {0} }
            $count = [long]$w * $h * $bpp
            if (-not $bpp -or $w -lt 1 -or $w -gt 128 -or $h -lt 1 -or $h -gt 128 -or $stream.Position + $count -gt $stream.Length) {
                throw "Invalid or truncated capture payload: $Path"
            }
            [void]$stream.Seek($count,[IO.SeekOrigin]::Current)
            $records++
        }
        if (-not $records) { throw "Empty capture: $Path" }
    } finally { $reader.Dispose(); $stream.Dispose() }
}
foreach ($report in $reports) {
    $fields = @{}
    foreach ($line in Get-Content -LiteralPath $report.FullName -Encoding UTF8) {
        if ($line -match '^([^=]+)=(.*)$') { $fields[$Matches[1]] = $Matches[2] }
    }
    $binary = $report.FullName.Substring(0, $report.FullName.Length - '.info.txt'.Length)
    Test-CaptureFile $binary
    foreach ($file in @($binary, "$binary.csv", $report.FullName)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing capture file: $file" }
        Copy-Item -LiteralPath $file -Destination $folder
    }
    $summary.Add("$($report.Name): mode=$($fields.mode) complete=$($fields.complete) pixels=$($fields.pixel_frames) guides=$($fields.guide_frames) dropped=$($fields.dropped_frames) cancelled=$($fields.cancelled_frames)")
    if ($fields.complete -eq '1') { $modes[$fields.mode] = $true }
}
foreach ($name in @('OptiScaler.log', 'amd_bridge.log')) {
    $file = Join-Path $gamePath $name
    if (Test-Path -LiteralPath $file -PathType Leaf) { Copy-Item -LiteralPath $file -Destination $folder }
}
foreach ($mode in @('0','1','2')) {
    if (-not $modes.ContainsKey($mode)) { $summary.Add("INCOMPLETE: no complete capture for mode $mode. Keep the other completed modes; only this mode may need recapturing.") }
}
$summary.Add('Attach the original video separately. Modes: 0=F6 original, 1=F7 candidate, 2=F8 identity.')
$summary | Set-Content -LiteralPath (Join-Path $folder 'CAPTURE-CHECK.txt') -Encoding UTF8
$hashes = foreach ($file in Get-ChildItem -LiteralPath $folder -File) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $file.Name
}
$hashes | Set-Content -LiteralPath (Join-Path $folder 'SHA256SUMS.txt') -Encoding ASCII
$zip = "$folder.zip"
Compress-Archive -LiteralPath $folder -DestinationPath $zip -CompressionLevel Optimal
$summary | Write-Host
Write-Host "Send this ZIP and the video: $zip"
Write-Output $zip
