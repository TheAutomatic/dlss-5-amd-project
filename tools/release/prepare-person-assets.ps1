# Prepare optional model dependencies for a clean release runner. Never writes a game directory.
[CmdletBinding()]
param([string]$CrtDir='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$assets=Join-Path $root 'assets/person-model'
$scratch=Join-Path $root 'work/scratch/person-release-assets'
New-Item -ItemType Directory -Force $assets,$scratch | Out-Null
$ort=Join-Path $assets 'onnxruntime.dll'
if(!(Test-Path -LiteralPath $ort)) {
    $zip=Join-Path $scratch 'onnxruntime-win-x64-1.23.2.zip'
    Invoke-WebRequest 'https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip' -OutFile $zip
    if((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne '0B38DF9AF21834E41E73D602D90DB5CB06DBD1CA618948B8F1D66D607AC9F3CD') {
        throw 'ONNX Runtime archive checksum mismatch.'
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $scratch -Force
    Copy-Item -LiteralPath (Join-Path $scratch 'onnxruntime-win-x64-1.23.2/lib/onnxruntime.dll') -Destination $ort
}
$version=[Diagnostics.FileVersionInfo]::GetVersionInfo($ort)
if($version.FileMajorPart -lt 1 -or ($version.FileMajorPart -eq 1 -and $version.FileMinorPart -lt 23)) {
    throw 'person-model/onnxruntime.dll is too old for API 23. Replace it with CPU x64 1.23.2 or newer.'
}
$pp=Join-Path $assets 'pphumanseg.onnx'
if(!(Test-Path -LiteralPath $pp)) {
    # OpenCV Zoo's fixed FP32 192x192 model. Hash/size come from its Git LFS pointer.
    $download=Join-Path $scratch 'pphumanseg.onnx'
    Invoke-WebRequest 'https://media.githubusercontent.com/media/opencv/opencv_zoo/2027dd2f5a8a5746b5d4964900a0465afc6d3a53/models/human_segmentation_pphumanseg/human_segmentation_pphumanseg_2023mar.onnx' -OutFile $download
    if((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne '552D8A984054E59B5D773D24B9B12022B22046CEB2BBC4C9AAEACEB36A9DDF24') {
        throw 'PP-HumanSeg model checksum mismatch.'
    }
    Copy-Item -LiteralPath $download -Destination $pp
}
$face=Join-Path $assets 'yunet.onnx'
if(!(Test-Path -LiteralPath $face)) {
    # Official dynamic FP32 export; the worker uses a fixed 320x320 input.
    $download=Join-Path $scratch 'yunet.onnx'
    Invoke-WebRequest 'https://media.githubusercontent.com/media/opencv/opencv_zoo/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/face_detection_yunet_2026may.onnx' -OutFile $download
    if((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne 'EBAFCE4E3C118D6554634BE5C27AB333B4C047A9A8C3FAF1D7CF93101C22F0F0') {
        throw 'YuNet model checksum mismatch.'
    }
    Copy-Item -LiteralPath $download -Destination $face
}
if(!(Test-Path -LiteralPath (Join-Path $assets 'yolo11n-seg.onnx'))) {
    Push-Location $scratch
    try {
        python -m pip install ultralytics==8.3.215 onnx==1.19.1
        if($LASTEXITCODE -ne 0){throw 'Unable to install the fixed YOLO export dependencies.'}
        python -c "from ultralytics import YOLO; YOLO('yolo11n-seg.pt').export(format='onnx', imgsz=640, opset=17, simplify=False, dynamic=False, half=False)"
        if($LASTEXITCODE -ne 0){throw 'Person model export failed.'}
        Copy-Item -LiteralPath (Join-Path $scratch 'yolo11n-seg.onnx') -Destination $assets
    } finally { Pop-Location }
}
if(!$CrtDir) {
    if(!$env:VCToolsRedistDir){throw 'Set CrtDir to the installed MSVC x64 redistributable CRT directory.'}
    $CrtDir=Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
foreach($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if(!(Test-Path -LiteralPath (Join-Path $CrtDir $name))){throw "Missing x64 redistributable CRT: $name"}
}
Get-ChildItem -LiteralPath $CrtDir -File -Filter '*.dll' | Copy-Item -Destination $assets -Force
Write-Host 'Person dependencies ready; CRT DLLs remain private to person-model.'
