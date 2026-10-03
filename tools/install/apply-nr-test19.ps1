param([string]$GameDirectory='', [string]$Proxy='')
$ErrorActionPreference='Stop'
function Hash([string]$p) {
    $f=[IO.File]::OpenRead($p);$h=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($h.ComputeHash($f)).Replace('-','').ToLowerInvariant()}finally{$h.Dispose();$f.Dispose()}
}
function Unlinked([string]$p) {
    $full=[IO.Path]::GetFullPath($p);$walk=$full
    while($walk){if(Test-Path -LiteralPath $walk){if((Get-Item -Force -LiteralPath $walk).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Linked path is not supported: $walk"}};$walk=[IO.Path]::GetDirectoryName($walk)}
    return $full
}
try {
    if(-not $GameDirectory){Add-Type -AssemblyName System.Windows.Forms;$picker=New-Object Windows.Forms.FolderBrowserDialog;$picker.Description='Select the existing game folder containing LmxxfNrRuntime.dll';if($picker.ShowDialog() -ne 'OK'){exit 1};$GameDirectory=$picker.SelectedPath;$picker.Dispose()}
    $game=Unlinked $GameDirectory
    if(-not (Test-Path -LiteralPath (Join-Path $game 'LmxxfNrRuntime.dll') -PathType Leaf)){throw 'Select the existing NR installation directory.'}
    $allowed=@('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll')
    $found=@($allowed | Where-Object {$p=Join-Path $game $_;(Test-Path -LiteralPath $p -PathType Leaf) -and [Diagnostics.FileVersionInfo]::GetVersionInfo($p).ProductName -eq 'OptiScaler'})
    if($Proxy){if($Proxy -notin $found){throw 'The selected proxy is not an existing OptiScaler DLL.'}}elseif($found.Count -eq 1){$Proxy=$found[0]}else{throw 'Cannot uniquely identify the OptiScaler proxy. Run with -Proxy dxgi.dll (or your installed proxy name).'}
    $payload=Join-Path $PSScriptRoot 'payload'
    $items=@(
        @{Source='OptiScaler.dll';Target=$Proxy},
        @{Source='LmxxfNrRuntime.dll';Target='LmxxfNrRuntime.dll'},
        @{Source='shaders/native_codec_encode.hlsl';Target='shaders/native_codec_encode.hlsl'},
        @{Source='shaders/native_codec_decode.hlsl';Target='shaders/native_codec_decode.hlsl'},
        @{Source='NR-test19-collect.ps1';Target='NR-test19-collect.ps1'},
        @{Source='NR-test19-collect.cmd';Target='NR-test19-collect.cmd'}
    )
    $sums=@{};foreach($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'PAYLOAD-SHA256.txt'))){if($line -notmatch '^([0-9a-f]{64})  (.+)$'){throw 'Invalid payload manifest'};$sums[$Matches[2]]=$Matches[1]}
    foreach($item in $items){
        $item.SourcePath=Unlinked (Join-Path $payload $item.Source);$item.TargetPath=Unlinked (Join-Path $game $item.Target)
        if(-not $sums.ContainsKey($item.Source) -or (Hash $item.SourcePath) -ne $sums[$item.Source]){throw "Payload hash mismatch: $($item.Source)"}
        $item.Existed=Test-Path -LiteralPath $item.TargetPath -PathType Leaf
        if($item.Existed){$f=[IO.File]::Open($item.TargetPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$f.Dispose()}
    }
    $backup=Join-Path $game ('backup-NR-test19-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    [void][IO.Directory]::CreateDirectory($backup)
    foreach($item in $items){if($item.Existed){$p=Join-Path $backup $item.Target;[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p));[IO.File]::Copy($item.TargetPath,$p,$false)}}
    $changed=New-Object Collections.Generic.List[object]
    try {
        foreach($item in $items){[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.TargetPath));$changed.Add($item);[IO.File]::Copy($item.SourcePath,$item.TargetPath,$true);if((Hash $item.TargetPath) -ne $sums[$item.Source]){throw "Installed hash mismatch: $($item.Target)"}}
    } catch {
        $failure=$_
        foreach($item in $changed){try{if($item.Existed){[IO.File]::Copy((Join-Path $backup $item.Target),$item.TargetPath,$true)}else{[IO.File]::Delete($item.TargetPath)}}catch{Write-Warning "Restore manually from $backup : $($item.Target)"}}
        throw $failure
    }
    Write-Host "TEST19 INSTALLED. Proxy: $Proxy" -ForegroundColor Green
    Write-Host "Backup: $backup"
    Write-Host 'Keep existing NR settings; History OFF, Smoothing 0, Debug OFF. F9 starts one 20s capture. Wait for SAVED, then run NR-test19-collect.cmd.'
}catch{Write-Host "UPDATE FAILED: $($_.Exception.Message)" -ForegroundColor Red;Write-Host 'Close the game and check the selected directory. No weights or INI are changed.';exit 1}
