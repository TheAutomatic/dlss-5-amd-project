param(
    [string]$Profile='',
    [Parameter(Mandatory=$true)][string]$DataRoot,
    [string]$ResultsRoot='', [int]$Gpu=-1, [switch]$Smoke, [switch]$PlanOnly
)
$ErrorActionPreference='Stop'
if(-not $Profile){$Profile=Join-Path $PSScriptRoot 'profiles/aco-models.json'}
. "$PSScriptRoot/common.ps1"
$p=Read-BenchmarkProfile $Profile
$DataRoot=(Resolve-Path -LiteralPath $DataRoot).Path
if($Smoke){$p.resolutions=@($p.resolutions[0]);$p.models=@($p.models[0]);$p.rounds=1;$p.warmup=2;$p.frames=3}
$files=@(Get-BenchmarkFiles $p $DataRoot)
if($PlanOnly){Write-Output "$($p.name): $($p.resolutions.Count*$p.models.Count*$p.rounds*$p.modes.Count) jobs; $($files.Count) input files";return}
if(-not $ResultsRoot){$ResultsRoot=Join-Path $DataRoot 'results'}
$out=Join-Path $ResultsRoot ((Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $out -Force|Out-Null
$rows=New-Object System.Collections.Generic.List[object]
$sys=$null;$child=$null;$fatal=$false
$envBefore=@{};Get-ChildItem Env:|Where-Object Name -match '^(NR_|DLSSNR_|MZ_)'|ForEach-Object{$envBefore[$_.Name]=$_.Value}
try {
    $probe=Resolve-BenchmarkPath $DataRoot $p.probe
    $raw=& $probe 2> "$out/probe-error.txt";if($LASTEXITCODE -ne 0){throw 'GPU enumeration failed'}
    $raw|Set-Content "$out/devices.json" -Encoding UTF8
    $devices=$raw|ConvertFrom-Json
    foreach($d in $devices){Write-Host "[$($d.index)] $($d.name); $($d.driverInfo)"}
    if($Gpu -lt 0){if(@($devices).Count -eq 1){$Gpu=[int]$devices[0].index}else{$Gpu=[int](Read-Host 'Select GPU index')}}
    $selected=@($devices|Where-Object index -eq $Gpu);if($selected.Count -ne 1){throw 'Invalid GPU index'}
    $os=$null;try{$os=Get-CimInstance Win32_OperatingSystem -ErrorAction Stop}catch{}
    $cpu=$null;try{$cpu=Get-CimInstance Win32_Processor -ErrorAction Stop|Select-Object -First 1}catch{}
    $sys=@{start=(Get-Date).ToString('o');gpu=$selected[0];cpu=$cpu.Name;osVersion=$os.Version;osBuild=$os.BuildNumber;totalRamKiB=$os.TotalVisibleMemorySize;freeRamKiB=$os.FreePhysicalMemory;powerPlan=(powercfg /getactivescheme|Out-String).Trim();inheritedNrEnvironment=$envBefore;smoke=[bool]$Smoke}
    $sys|ConvertTo-Json -Depth 8|Set-Content "$out/system.json" -Encoding UTF8
    $p|ConvertTo-Json -Depth 10|Set-Content "$out/profile.json" -Encoding UTF8
    $identity=@(foreach($f in $files){[pscustomobject]@{path=$f.Substring($DataRoot.TrimEnd('\').Length+1);sha256=(Get-BenchmarkFileHash -LiteralPath $f -Algorithm SHA256).Hash;size=(Get-Item -LiteralPath $f).Length}})
    ConvertTo-Json -InputObject $identity -Depth 5|Set-Content "$out/input-manifest.json" -Encoding UTF8
    $packageManifest=Join-Path $DataRoot 'package-manifest.json'
    if(Test-Path -LiteralPath $packageManifest){
        $declared=Get-Content -LiteralPath $packageManifest -Raw|ConvertFrom-Json
        foreach($f in $identity){$entry=@($declared.files|Where-Object {$_.path.Replace('/','\') -eq $f.path.Replace('/','\')});if($entry.Count -eq 1 -and $entry[0].sha256 -ne $f.sha256){throw "Package file changed: $($f.path)"}}
        Copy-Item -LiteralPath $packageManifest -Destination "$out/package-manifest.json"
    }
    Get-ChildItem Env:|Where-Object Name -match '^(NR_|DLSSNR_|MZ_)'|ForEach-Object{Remove-Item -LiteralPath "Env:$($_.Name)"}
    $env:NR_GPU="$Gpu";$env:NR_TCHAIN='0';$env:NR_PERSIST='0';$env:NR_RDNA3_PERSIST='0';$env:NR_NO_PERSIST='1'
    $index=0;$total=$p.resolutions.Count*$p.models.Count*$p.rounds*$p.modes.Count
    foreach($res in $p.resolutions){foreach($model in $p.models){foreach($round in 1..$p.rounds){
        $modes=@($p.modes);if($round%2 -eq 0){[array]::Reverse($modes)}
        foreach($mode in $modes){
            $index++;$w=$res.width;$h=$res.height;$name="$($mode.id)_${w}x${h}_$($model.id)_r$round"
            Write-Host "[$index/$total] $name"
            Remove-Item Env:NR_ACO_DIR,Env:NR_ACO_SHELL_DIR -ErrorAction SilentlyContinue
            if($mode.aco){$env:NR_ACO_DIR=Resolve-BenchmarkPath $DataRoot $mode.aco;$env:NR_ACO_SHELL_DIR=Join-Path $env:NR_ACO_DIR 'shells'}
            $exe=Resolve-BenchmarkPath $DataRoot $mode.exe
            $plan=Resolve-BenchmarkPath $DataRoot ($model.plan.Replace('{width}',"$w").Replace('{height}',"$h"))
            $argv=@('--no-reuse','--plan',$plan,'--model-pack',(Resolve-BenchmarkPath $DataRoot $model.file),'--spv-dir',(Resolve-BenchmarkPath $DataRoot $mode.shaders),'--warmup',"$($p.warmup)",'--repeats',"$($p.frames)",'--source-width',"$w",'--source-height',"$h")+@($mode.extraArgs)
            @{exe=$exe;arguments=$argv;gpu=$selected[0];start=(Get-Date).ToString('o')}|ConvertTo-Json -Depth 8|Set-Content "$out/$name.command.json" -Encoding UTF8
            $quoted=($argv|ForEach-Object{'"'+([regex]::Replace([regex]::Replace([string]$_,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}) -join ' '
            $timer=[Diagnostics.Stopwatch]::StartNew();$timeout=$false
            $child=Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $DataRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/$name.stdout.log" -RedirectStandardError "$out/$name.stderr.log"
            $handle=$child.Handle
            while(-not $child.WaitForExit(1000)){if($timer.Elapsed.TotalSeconds -gt $p.timeoutSeconds){$timeout=$true;$child.Kill();$child.WaitForExit();break}}
            $child.WaitForExit();$rc=$child.ExitCode
            $text=Get-Content -LiteralPath "$out/$name.stdout.log" -Raw -ErrorAction SilentlyContinue
            if([string]::IsNullOrEmpty($text)){$text=' '}
            $m=[regex]::Match([string]$text,'GPU total ([0-9.]+) ms over (\d+) frames')
            $status=if($timeout){'TIMEOUT'}elseif($null -eq $rc -or $rc -ne 0){'FAILED'}elseif(-not $m.Success -or [int]$m.Groups[2].Value -ne $p.frames){'MISSING_TIMING'}else{'OK'}
            $ms=$null;if($status -eq 'OK'){$ms=[double]::Parse($m.Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)/$p.frames}
            $row=[pscustomobject]@{Resolution="${w}x${h}";Model=$model.id;Mode=$mode.id;Round=$round;Status=$status;GpuMs=$ms;ExitCode=$rc;Seconds=$timer.Elapsed.TotalSeconds;Warmup=$p.warmup;Frames=$p.frames;Log="$name.stdout.log"}
            $rows.Add($row);$row|ConvertTo-Json -Compress|Add-Content "$out/journal.jsonl" -Encoding UTF8
            $child=$null;Save-BenchmarkSummary $rows $p $sys $out;Write-Host "  $status GPU ms=$ms"
        }
    }}}
} catch {
    $fatal=$true;$_|Out-String|Set-Content "$out/launcher-error.txt" -Encoding UTF8;Write-Host "Error: $_" -ForegroundColor Red
} finally {
    if($child -and -not $child.HasExited){$child.Kill();$child.WaitForExit()}
    Save-BenchmarkSummary $rows $p $sys $out
    Get-ChildItem Env:|Where-Object Name -match '^(NR_|DLSSNR_|MZ_)'|ForEach-Object{Remove-Item -LiteralPath "Env:$($_.Name)"}
    foreach($key in $envBefore.Keys){Set-Item -LiteralPath "Env:$key" -Value $envBefore[$key]}
    try{Compress-Archive -Path "$out/*" -DestinationPath "$out.zip";Write-Host "Send back: $out.zip"}catch{Write-Host "Send results folder: $out"}
}
if($fatal -or @($rows|Where-Object Status -ne 'OK').Count){exit 1}




