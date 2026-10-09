$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out=Join-Path $root 'exports/person-worker-package'
& cmd /d /c (Join-Path $root 'tools/build/build-person-worker.cmd') $out
if($LASTEXITCODE -ne 0){throw 'Worker build failed'}
$fixture=Join-Path $out 'source-fixture'
New-Item -ItemType Directory -Force $fixture | Out-Null
$receipt=Get-Content (Join-Path $out 'person-worker-build.json') -Raw | ConvertFrom-Json
foreach($path in $receipt.sources.PSObject.Properties.Name){
    $dest=Join-Path $fixture $path
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $path) -Destination $dest -Force
}
Copy-Item -LiteralPath (Join-Path $out 'person-worker.exe'),(Join-Path $out 'person-worker-build.json') -Destination $fixture -Force
$checker=Join-Path $fixture 'tools/release/check-person-worker.ps1'
$exe=Join-Path $fixture 'person-worker.exe'
function Check([bool]$ShouldPass){
    $saved=$ErrorActionPreference
    try { $ErrorActionPreference='Continue'; $text=& powershell -NoProfile -ExecutionPolicy Bypass -File $checker -WorkerExe $exe 2>&1 }
    finally { $ErrorActionPreference=$saved }
    if(($LASTEXITCODE -eq 0) -ne $ShouldPass){throw "Unexpected worker gate result: $text"}
}
Check $true
$ipc=Join-Path $fixture 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonIpc.h'
[IO.File]::AppendAllText($ipc,"`n// changed after build`n")
Check $false
Copy-Item -LiteralPath (Join-Path $root 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonIpc.h') -Destination $ipc -Force
$bytes=[IO.File]::ReadAllBytes($exe);$bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1;[IO.File]::WriteAllBytes($exe,$bytes)
Check $false
Copy-Item -LiteralPath (Join-Path $out 'person-worker.exe') -Destination $exe -Force
Remove-Item -LiteralPath (Join-Path $fixture 'person-worker-build.json')
Check $false
Write-Host 'person worker package: PASS (actual build, source mutation, binary substitution, missing receipt)'
