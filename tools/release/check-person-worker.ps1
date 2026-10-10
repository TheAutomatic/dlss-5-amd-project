[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkerExe, [switch]$WriteReceipt)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source='OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person'
$paths=@("$source/PersonIpc.h", "$source/PersonModel.h", "$source/FaceModel.h", "$source/WorkerPolicy.h", "$source/worker/person_worker.cpp",
    "$source/worker/person_worker.vcxproj", 'tools/build/build-person-worker.cmd', 'tools/release/check-person-worker.ps1',
    'third_party/onnxruntime/onnxruntime_c_api.h')
function Hash([string]$path) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { $stream=[IO.File]::OpenRead($path); try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') } finally { $stream.Dispose() } }
    finally { $sha.Dispose() }
}
$worker=(Resolve-Path -LiteralPath $WorkerExe).Path
$expected=[ordered]@{schema=1;binary=(Hash $worker);sources=[ordered]@{}}
foreach($path in $paths){$expected.sources[$path]=Hash (Join-Path $root $path)}
$receipt=Join-Path (Split-Path -Parent $worker) 'person-worker-build.json'
if($WriteReceipt){[IO.File]::WriteAllText($receipt,($expected|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))}
else {
    if(!(Test-Path -LiteralPath $receipt)){throw 'Missing person-worker build receipt; rebuild the worker with tools/build/build-person-worker.cmd.'}
    $actual=Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    if($actual.schema -ne 1 -or $actual.binary -ne $expected.binary){throw 'Person worker binary does not match its build receipt.'}
    foreach($path in $paths){if($actual.sources.$path -ne $expected.sources[$path]){throw "Person worker source changed: $path; rebuild before packaging."}}
}
Write-Host 'Person worker: source and binary hashes match.'
