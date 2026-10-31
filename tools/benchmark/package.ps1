param(
    [Parameter(Mandatory=$true)][string]$DataRoot,
    [string]$Profile='',
    [string]$OutDir='',
    [ValidatePattern('^[a-zA-Z0-9._-]+$')][string]$Name='Offline-NR-Benchmark'
)
$ErrorActionPreference='Stop'
if(-not $Profile){$Profile=Join-Path $PSScriptRoot 'profiles/aco-models.json'}
. "$PSScriptRoot/common.ps1"
$p=Read-BenchmarkProfile $Profile
$DataRoot=(Resolve-Path -LiteralPath $DataRoot).Path
$files=@(Get-BenchmarkFiles $p $DataRoot)
if(-not $OutDir){$OutDir=Join-Path $PSScriptRoot '../../dist'}
$OutDir=[IO.Path]::GetFullPath($OutDir)
$target=Join-Path $OutDir ($Name+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $target -Force|Out-Null
foreach($f in $files){
    $relative=$f.Substring($DataRoot.TrimEnd('\').Length+1)
    $to=Join-Path $target $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force|Out-Null
    Copy-Item -LiteralPath $f -Destination $to
}
Copy-Item -LiteralPath "$PSScriptRoot/run.ps1","$PSScriptRoot/common.ps1" -Destination $target
Copy-Item -LiteralPath "$PSScriptRoot/README.md" -Destination "$target/README.md"
$p|ConvertTo-Json -Depth 10|Set-Content "$target/profile.json" -Encoding UTF8
@'
@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" -DataRoot "%~dp0." -Profile "%~dp0profile.json" %*
pause
'@ | Set-Content -LiteralPath "$target/Run_All_Benchmarks.bat" -Encoding ascii
$entries=@(Get-ChildItem -LiteralPath $target -Recurse -File|ForEach-Object{[pscustomobject]@{path=$_.FullName.Substring($target.Length+1).Replace('\','/');sha256=(Get-BenchmarkFileHash -LiteralPath $_.FullName).Hash;size=$_.Length}})
@{schema=1;profile=$p.name;files=$entries}|ConvertTo-Json -Depth 6|Set-Content "$target/package-manifest.json" -Encoding UTF8
Compress-Archive -LiteralPath $target -DestinationPath "$target.zip"
$hash=(Get-BenchmarkFileHash -LiteralPath "$target.zip").Hash
"$hash  $([IO.Path]::GetFileName($target)).zip"|Set-Content "$target.zip.sha256" -Encoding ascii
Write-Output "PACKAGE: $target.zip"


