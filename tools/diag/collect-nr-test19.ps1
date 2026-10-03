param(
    [string]$GameDirectory=$PSScriptRoot,
    [string]$CaptureDirectory=(Join-Path ([IO.Path]::GetTempPath()) 'Lmxxf-NR-test19'),
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
# A PowerShell 7 caller can pass its module path to Windows PowerShell 5.
# Prefer the matching system modules; this affects this process only.
if($PSVersionTable.PSEdition -ne 'Core'){$env:PSModulePath="$PSHOME\Modules;${env:ProgramFiles}\WindowsPowerShell\Modules;"+$env:PSModulePath}
$gamePath=(Resolve-Path -LiteralPath $GameDirectory).Path
if (-not $OutputDirectory) {$OutputDirectory=$gamePath}
$outputPath=(Resolve-Path -LiteralPath $OutputDirectory).Path
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$folder=Join-Path $outputPath "NR-test19-results-$stamp"
New-Item -ItemType Directory -Path $folder | Out-Null
$summary=[Collections.Generic.List[string]]::new()
$reports=@(Get-ChildItem -LiteralPath $CaptureDirectory -Filter 'capture-*.nrhl.info.txt' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
function Test-Binary([string]$Path,[bool]$Full) {
    $stream=[IO.File]::OpenRead($Path);$reader=[IO.BinaryReader]::new($stream)
    try {
        $magic=[Text.Encoding]::ASCII.GetString($reader.ReadBytes(8))
        $expected=if($Full){"NRFFV1`0`0"}else{"NRHLV2`0`0"}
        if($magic -ne $expected){throw "Wrong file header: $Path"}
        $count=0;$pairFrame=0;$pairW=0;$pairH=0
        while($stream.Position -lt $stream.Length){
            $h=$reader.ReadBytes(76);if($h.Length -ne 76){throw "Truncated record: $Path"}
            $stage=[BitConverter]::ToUInt32($h,4);$w=[BitConverter]::ToUInt32($h,28);$height=[BitConverter]::ToUInt32($h,32);$format=[BitConverter]::ToUInt32($h,36)
            $bpp=switch($format){2{16}10{8}11{8}41{4}54{2}default{0}}
            $edge=if($Full){8192}else{128}
            $bytes=[long]$w*$height*$bpp
            if(-not $bpp -or $w -lt 1 -or $height -lt 1 -or $w -gt $edge -or $height -gt $edge -or $stream.Position+$bytes -gt $stream.Length){throw "Invalid payload: $Path"}
            if($Full -and ($format -ne 10 -or $stage -notin @(1,2))){throw "Invalid full-frame stage: $Path"}
            if($Full){
                $frame=[BitConverter]::ToUInt32($h,0)
                if($count%2 -eq 0){if($stage -ne 1){throw 'Missing full input'};$pairFrame=$frame;$pairW=$w;$pairH=$height}
                elseif($stage -ne 2 -or $frame -ne $pairFrame -or $w -ne $pairW -or $height -ne $pairH){throw 'Unpaired full input/output'}
            }
            [void]$stream.Seek($bytes,[IO.SeekOrigin]::Current);$count++
        }
        if($Full -and $count%2){throw 'Unpaired full-frame file'}
        if(-not $count){throw "Empty capture: $Path"}
        return $count
    }finally{$reader.Dispose();$stream.Dispose()}
}
if($reports.Count){
    $report=$reports[0];$binary=$report.FullName.Substring(0,$report.FullName.Length-'.info.txt'.Length)
    $fields=@{};foreach($line in Get-Content -LiteralPath $report.FullName -Encoding UTF8){if($line -match '^([^=]+)=(.*)$'){$fields[$Matches[1]]=$Matches[2]}}
    foreach($file in @($binary,"$binary.csv","$binary.info.txt","$binary.full","$binary.reuse.csv")){
        if(Test-Path -LiteralPath $file -PathType Leaf){Copy-Item -LiteralPath $file -Destination $folder}else{$summary.Add("MISSING: $file")}
    }
    try{
        $roi=Test-Binary $binary $false;$full=Test-Binary "$binary.full" $true
        $summary.Add("Latest capture only: $($report.Name); ROI records=$roi; full-frame records=$full")
        if($fields.build -ne 'NR-test19'){throw 'Wrong diagnostic build'}
        if($fields.diagnostics_complete -ne '1' -or $fields.complete -ne '1' -or $fields.full_errors -ne '0' -or $fields.reuse_errors -ne '0' -or [int]$fields.full_frames -lt 1 -or $fields.full_frames -ne $fields.full_target -or $full -ne 2*[int]$fields.full_frames -or [int]$fields.reuse_requested -lt 1 -or $fields.reuse_requested -ne $fields.reuse_completed){throw 'Diagnostic completeness check failed.'}
        $timeline=@(Import-Csv -LiteralPath "$binary.csv");$reuseRows=@(Import-Csv -LiteralPath "$binary.reuse.csv")
        $timelineIds=[Collections.Generic.HashSet[string]]::new();$reuseIds=[Collections.Generic.HashSet[string]]::new()
        foreach($r in $timeline){if(-not $timelineIds.Add($r.frame)){throw 'Duplicate timeline frame'}}
        foreach($r in $reuseRows){if(-not $reuseIds.Add($r.frame) -or -not $timelineIds.Contains($r.frame)){throw 'Invalid reuse frame identity'}}
        if($reuseRows.Count -ne [int]$fields.reuse_completed -or $timeline.Count -ne [int]$fields.read_frames -or $reuseRows.Count -ne $timeline.Count){throw 'Reuse/timeline row counts differ'}
        $samples=@($timeline | Where-Object { ([int]$_.mask -band 47) -eq 47 });
        if($samples.Count -ne [int]$fields.pixel_frames -or $samples.Count -lt 2 -or ([long]$samples[-1].tick-[long]$samples[0].tick) -lt 17000){throw 'Colour samples do not cover the capture duration'}
        $summary.Add('COMPLETE: ROI, full-frame snapshots and reuse records saved. Send ZIP plus video.')
    }catch{$summary.Add("INCOMPLETE: $($_.Exception.Message) Send these files first; do not repeat blindly.")}
}else{$summary.Add('NO SAVED CAPTURE: collecting available game logs. Send these first; do not repeat blindly.')}
foreach($name in @('OptiScaler.log','amd_bridge.log')){ $file=Join-Path $gamePath $name;if(Test-Path -LiteralPath $file -PathType Leaf){Copy-Item -LiteralPath $file -Destination $folder}}
$summary | Set-Content -LiteralPath (Join-Path $folder 'CAPTURE-CHECK.txt') -Encoding UTF8
$hashes=foreach($file in Get-ChildItem -LiteralPath $folder -File){'{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$file.Name}
$hashes | Set-Content -LiteralPath (Join-Path $folder 'SHA256SUMS.txt') -Encoding ASCII
$zip="$folder.zip";Compress-Archive -LiteralPath $folder -DestinationPath $zip -CompressionLevel Optimal
$summary | Write-Host
Write-Host "Send this ZIP and the video: $zip"
Write-Output $zip
