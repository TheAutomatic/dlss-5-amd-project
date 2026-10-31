$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path
. "$root/tools/benchmark/common.ps1"
function Assert($Value,[string]$Message){if(-not $Value){throw $Message}}
Assert ((Get-BenchmarkMedian @(30,10,20)) -eq 20) 'Odd median'
Assert ((Get-BenchmarkMedian @(20,10)) -eq 15) 'Even median'
$rejected=$false;try{Resolve-BenchmarkPath $root '../outside'|Out-Null}catch{$rejected=$true};Assert $rejected 'Traversal accepted'
$base=Join-Path $root ('exports/benchmark-tests/'+[guid]::NewGuid().ToString('N'))
$data=Join-Path $base 'data with spaces';New-Item -ItemType Directory -Path "$data/shaders","$data/plans" -Force|Out-Null
'x'|Set-Content "$data/input.f32";'x'|Set-Content "$data/model.bin";'x'|Set-Content "$data/shaders/x.txt";'x'|Set-Content "$data/plans/p_16x16.txt"
Add-Type -TypeDefinition @'
using System;
public class FakeProbe {
 public static void Main(){Console.WriteLine("[{\"index\":0,\"name\":\"CPU test adapter\",\"driverInfo\":\"fake\"}]");}
}
'@ -OutputAssembly "$data/gpu_probe.exe" -OutputType ConsoleApplication
Add-Type -TypeDefinition @'
using System;
public class FakeGraph {
 public static void Main(string[] a){
  if(Array.IndexOf(a,"--timeout")>=0){System.Threading.Thread.Sleep(10000);return;}
  Console.WriteLine("partial log retained");
  if(Array.IndexOf(a,"--fail")>=0){Console.Error.WriteLine("intentional failure");Environment.Exit(7);}
  if(Array.IndexOf(a,"--missing")>=0)return;
  int n=int.Parse(a[Array.IndexOf(a,"--repeats")+1]);
  int ms=Array.IndexOf(a,"--slow")>=0?20:10;
  Console.WriteLine("GPU total "+(n*ms)+" ms over "+n+" frames");
 }
}
'@ -OutputAssembly "$data/graph.exe" -OutputType ConsoleApplication
$p=@{schema=1;name='CPU integration';extraFiles=@('input.f32');description='fake nr_graph';probe='gpu_probe.exe';warmup=1;frames=2;rounds=3;timeoutSeconds=1;referenceMode='llpc';resolutions=@(@{width=16;height=16});models=@(@{id='small';file='model.bin';plan='plans/p_{width}x{height}.txt'});modes=@()}
foreach($m in @(@('aco',@()),@('llpc',@('--slow')),@('bad',@('--fail')),@('missing',@('--missing')),@('timeout',@('--timeout')))){
 $p.modes+=@{id=$m[0];label=$m[0];source='fake';exe='graph.exe';shaders='shaders';aco=$null;extraArgs=$m[1]}
}
$profile=Join-Path $base 'profile.json';$p|ConvertTo-Json -Depth 10|Set-Content $profile -Encoding UTF8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/tools/benchmark/run.ps1" -DataRoot $data -Profile $profile -ResultsRoot "$base/results" -Gpu 0
Assert ($LASTEXITCODE -eq 1) 'Failed jobs must produce nonzero runner status'
$result=Get-ChildItem "$base/results" -Directory|Select-Object -First 1
$rows=@(Import-Csv "$($result.FullName)/runs.csv")
Assert ($rows.Count -eq 15) 'Missing journal rows'
foreach($pair in @(@('OK',6),@('FAILED',3),@('MISSING_TIMING',3),@('TIMEOUT',3))){Assert (@($rows|Where-Object Status -eq $pair[0]).Count -eq $pair[1]) "Wrong count: $($pair[0])"}
Assert ($rows[5].Mode -eq 'timeout' -and $rows[9].Mode -eq 'aco') 'Even round not reversed'
$summary=Get-Content "$($result.FullName)/benchmark_results.txt" -Raw
Assert ($summary.Contains('CPU test adapter') -and $summary.Contains('fake')) 'Hardware identity absent from TXT'
Assert ($summary.Contains('50.00%')) 'Comparison percent incorrect'
Assert ((Get-Content "$($result.FullName)/bad_16x16_small_r1.stdout.log" -Raw).Contains('partial log')) 'Partial stdout lost'
# Successful-only profile: build a relocatable package and run from its extracted ZIP.
$p.modes=@($p.modes|Where-Object id -eq 'aco');$p|ConvertTo-Json -Depth 10|Set-Content $profile -Encoding UTF8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/tools/benchmark/package.ps1" -DataRoot $data -Profile $profile -OutDir "$base/packages"
Assert ($LASTEXITCODE -eq 0) 'Packaging failed'
$zip=Get-ChildItem "$base/packages" -Filter '*.zip'|Select-Object -First 1
Expand-Archive -LiteralPath $zip.FullName -DestinationPath "$base/relocated"
$package=Get-ChildItem "$base/relocated" -Directory|Select-Object -First 1
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$($package.FullName)/run.ps1" -DataRoot $package.FullName -Profile "$($package.FullName)/profile.json" -Gpu 0 -Smoke
Assert ($LASTEXITCODE -eq 0) 'Relocated package failed'
Assert (Test-Path -LiteralPath "$($package.FullName)/input.f32") 'Extra scenario input omitted'
'changed'|Set-Content "$($package.FullName)/model.bin"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$($package.FullName)/run.ps1" -DataRoot $package.FullName -Profile "$($package.FullName)/profile.json" -Gpu 0 -Smoke
Assert ($LASTEXITCODE -eq 1) 'Changed packaged input was accepted'
Write-Host 'BENCHMARK_TOOL_TESTS_OK (no GPU)'

