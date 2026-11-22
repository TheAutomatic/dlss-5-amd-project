# Windows PowerShell 5.1. Uses only disposable fixtures, not installed games.
$ErrorActionPreference = 'Stop'
$install = Join-Path $PSScriptRoot '..\install'
$work = Join-Path ([IO.Path]::GetTempPath()) ('nr-regression-' + [guid]::NewGuid().ToString('N'))
$local = $env:LOCALAPPDATA
function Assert($condition, $description) { if (-not $condition) { throw $description } }
function Put([string]$path, [string]$text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $text)
}
function Game($id, $exe, $root, $source = 'XBOX') {
    $g = New-Object OptScalerNR.Launcher.GameInfo
    $g.Id = $id; $g.Exe = $exe; $g.Root = $root; $g.Source = $source; $g.Name = $id
    return $g
}
try {
    $env:LOCALAPPDATA = Join-Path $work 'local'
    & (Join-Path $PSScriptRoot 'build-launcher.ps1')
    Add-Type -AssemblyName PresentationFramework
    $asm = [Reflection.Assembly]::LoadFrom((Resolve-Path (Join-Path $PSScriptRoot '..\..\exports\launcher\OptiScalerNRLauncher.exe')))
    $type = $asm.GetType('OptScalerNR.Launcher.MainController', $true)
    $paths = $asm.GetType('OptScalerNR.Launcher.GamePaths', $true)
    $data = $asm.GetType('OptScalerNR.Launcher.DlssnrLocalData', $true)
    $static = [Reflection.BindingFlags]'Static,NonPublic'
    $instance = [Reflection.BindingFlags]'Instance,NonPublic'
    . (Join-Path $install 'library-scan.ps1')
    $games = @(
        foreach ($package in 'One','Two') {
            $content = Join-Path $work "XboxGames\$package\Content"
            $path = Join-Path $content 'bin\Game.exe'
            Put $path $package
            Put (Join-Path $content 'MicrosoftGame.Config') '<Game><ExecutableList><Executable Name="bin\Game.exe" TargetDeviceFamily="PC"/></ExecutableList></Game>'
            $hits = @(Get-OptScalerXboxExes $content)
            Assert ($hits.Count -eq 1 -and $hits[0].path -eq $path) "Wrong Xbox target: $package"
            Assert ($paths::Normalize($path) -eq $path) "Remapped direct path: $path"
            Game "xbox:$package" $path $content
        }
    )
    $win = Join-Path $work 'WindowsApps\One\Game.exe'
    Put $win 'protected fixture'
    Assert ($null -eq $paths::Normalize($win)) 'WindowsApps accepted by launcher'
    foreach ($action in 'inspect','update','uninstall') {
        $req = Join-Path $work 'request.json'
        $res = Join-Path $work 'result.json'
        # An explicitly empty package prevents real setup/uninstall even on a broken resolver.
        Put $req (@{schema=1;requestId='test';action=$action;gameExe=$win;packageRoot=$work} | ConvertTo-Json)
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $install 'launcher-worker.ps1') -RequestJson $req -ResultJson $res -EventsJsonl (Join-Path $work 'events.jsonl')
        $result = Get-Content -LiteralPath $res -Raw | ConvertFrom-Json
        Assert ($LASTEXITCODE -eq 1 -and $result.error -like '*XboxGames*' -and -not $result.effectiveGameDir) "$action used WindowsApps"
    }

    $probePath = Join-Path $install 'proxy-probe.ps1'
    . $probePath
    $bitnessDir = Join-Path $work 'ReShadeBitness'
    [void][IO.Directory]::CreateDirectory($bitnessDir)
    $peProbe = $asm.GetType('OptScalerNR.Launcher.PeProbe', $true)
    foreach ($bits in 32,64) {
        $systemDir = if ($bits -eq 32) { [Environment]::GetFolderPath('SystemX86') } else { [Environment]::SystemDirectory }
        $exe = Join-Path $bitnessDir "Game$bits.exe"
        Copy-Item -LiteralPath (Join-Path $systemDir 'where.exe') -Destination $exe
        $pe = Get-ProxyRecommendation $exe
        Assert ($pe.pe.bitness -eq $bits -and $pe.pe.ok -and !$pe.blockInstall -and @($pe.pe.imports).Count -gt 0) "Incorrect ${bits}-bit PE probe: $($pe.pe | ConvertTo-Json -Compress)"
        Assert ($peProbe::ReadGameBitness($exe) -eq $bits) "Launcher did not detect $bits-bit PE"
    }
    $x86 = Join-Path $bitnessDir 'Game32.exe'
    $pkgFixture = Join-Path $work 'ReShadePackage'
    Put (Join-Path $pkgFixture 'OptiScaler.dll') 'test package'
    Copy-Item -LiteralPath (Join-Path $install 'install.ps1'), (Join-Path $install 'uninstall.ps1') -Destination $pkgFixture
    Assert ($peProbe::ReadGameBitness((Join-Path $pkgFixture 'OptiScaler.dll')) -eq 0) 'Invalid PE was not reported as unknown'
    foreach ($mode in 'optiscaler','standalone') {
        Put $req (@{schema=1;requestId='test';action='install';gameExe=$x86;packageRoot=$pkgFixture;installMode=$mode} | ConvertTo-Json)
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $install 'launcher-worker.ps1') -RequestJson $req -ResultJson $res -EventsJsonl (Join-Path $work 'events.jsonl')
        $result = Get-Content -LiteralPath $res -Raw | ConvertFrom-Json
        Assert ($LASTEXITCODE -eq 1 -and $result.error -eq '32-bit games support ReShade mode only.') "x86 worker did not guard ${mode}: $($result.error)"
    }
    Put (Join-Path $bitnessDir 'dlssnr_amd.addon32') 'test add-on'
    Put (Join-Path $bitnessDir 'ReShade32.dll') 'test Vulkan layer'
    Assert ((Get-ProxyRecommendation $x86).installedMode -eq 'reshade-feeder-vulkan') 'x86 ReShade Vulkan install was not detected'
    Assert ($peProbe::DetectInstalledMode($bitnessDir) -eq 'reshade-feeder-vulkan') 'Launcher did not identify the x86 ReShade Vulkan install'
    Write-Host '32-bit and 64-bit PE probing, x86 mode restrictions, and x86 ReShade installation detection passed.'

    foreach ($rel in 'OptiScaler/plugins/dlssnr-amd.asi','model-tools/dlssnr_extract_model.exe',
            'dlssnr-amd/shaders/fixture','dlssnr-amd/aco/d3d12_outer.nra','dlssnr-amd/aco/rdna3/d3d12_outer.nra',
            'dlssnr-amd/CC_SMALL_FP8.bin','reshade/reshade-shaders/Shaders/DLSS5_Feed.fx',
            'reshade/ReShade32.dll','reshade/ReShade64.dll','reshade/dlssnr_amd.addon32','reshade/dlssnr_amd.addon64',
            'reshade/vk-override/implicit_layer/ReShade32.json','reshade/vk-override/implicit_layer/ReShade64.json',
            'dxvk/x86/d3d9.dll','dxvk/x86/dxgi.dll','vulkan/x86/vulkan-1.dll','vulkan/vulkan-1.dll') {
        Put (Join-Path $pkgFixture $rel) $rel
    }
    foreach ($bits in 32,64) {
        foreach ($mode in 'reshade-feeder','reshade-feeder-dx9','reshade-feeder-vulkan') {
            if ($bits -eq 64 -and $mode -eq 'reshade-feeder-dx9') { continue }
            $target = Join-Path $work "$mode-$bits"
            [void][IO.Directory]::CreateDirectory($target)
            $targetExe = Join-Path $target 'Game.exe'
            Copy-Item -LiteralPath $x86 -Destination $targetExe
            $request = @{schema=1;requestId='test';action='install';gameExe=$targetExe;packageRoot=$pkgFixture;installMode=$mode;
                explicitDecisions=@{mochizukiNetworks=@('small')}}
            if ($bits -eq 64) { $request.gameBitness = 64 }
            Put $req ($request | ConvertTo-Json -Depth 5)
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $install 'launcher-worker.ps1') -RequestJson $req -ResultJson $res -EventsJsonl (Join-Path $work 'events.jsonl')
            $code = $LASTEXITCODE
            if ($code -ne 0) {
                Get-Content -LiteralPath (Join-Path $work 'setup.stdout.log'), (Join-Path $work 'setup.stderr.log') -ErrorAction SilentlyContinue | Write-Host
                throw "$mode $bits-bit failed: $(Get-Content -LiteralPath $res -Raw)"
            }
            $addon = "dlssnr_amd.addon$bits"
            $otherBits = if ($bits -eq 32) { 64 } else { 32 }
            $component = "ReShade$bits.dll"
            $targetDll = if ($mode -eq 'reshade-feeder') { 'dxgi.dll' } else { $component }
            Assert ((Get-Content -LiteralPath (Join-Path $target $addon) -Raw) -eq "reshade/$addon") "$mode installed wrong add-on"
            Assert ((Get-Content -LiteralPath (Join-Path $target $targetDll) -Raw) -eq "reshade/$component") "$mode installed wrong ReShade DLL"
            Assert (!(Test-Path -LiteralPath (Join-Path $target "dlssnr_amd.addon$otherBits"))) "$mode installed the other architecture's add-on"
            if ($mode -ne 'reshade-feeder') {
                $loader = if ($bits -eq 32) { 'vulkan/x86/vulkan-1.dll' } else { 'vulkan/vulkan-1.dll' }
                $layer = "vk-override/implicit_layer/ReShade$bits.json"
                Assert ((Get-Content -LiteralPath (Join-Path $target 'vulkan-1.dll') -Raw) -eq $loader) "$mode installed wrong Vulkan loader"
                Assert ((Get-Content -LiteralPath (Join-Path $target $layer) -Raw) -eq "reshade/$layer") "$mode installed wrong layer"
                Assert (!(Test-Path -LiteralPath (Join-Path $target "vk-override/implicit_layer/ReShade$otherBits.json"))) "$mode installed the other architecture's layer"
            }
            if ($mode -eq 'reshade-feeder-dx9') {
                foreach ($dll in 'd3d9.dll','dxgi.dll') {
                    Assert ((Get-Content -LiteralPath (Join-Path $target $dll) -Raw) -eq "dxvk/x86/$dll") "DX9 installed wrong DXVK $dll"
                }
            }
            Assert ((Get-ProxyRecommendation $targetExe).installedMode -eq $mode) "Could not identify installed $mode"
            Write-Host "Installed and verified ${bits}-bit payload for $mode."
        }
    }

    # Exercise cache migration and selection without launching background workers.
    $ctrl = [Runtime.Serialization.FormatterServices]::GetUninitializedObject($type)
    $gamesField = $type.GetField('_allGames', $instance)
    $list = [Activator]::CreateInstance($gamesField.FieldType)
    $gamesField.SetValue($ctrl, $list)
    $hidden = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    $type.GetField('_hiddenGames', $instance).SetValue($ctrl, $hidden)
    $library = Join-Path $work 'library.json'
    $type.GetField('_settingsDir', $instance).SetValue($ctrl, $work)
    $type.GetField('_libraryPath', $instance).SetValue($ctrl, $library)
    Put $library (@{Games=@(@{id='stale';source='Manual';exe=$win})} | ConvertTo-Json -Depth 4)
    $type.GetMethod('LoadLibraryCache', $instance).Invoke($ctrl, $null)
    Assert ($list.Count -eq 0 -and (Get-Content $library -Raw) -notlike '*stale*') 'Old WindowsApps cache survived'
    $auto = Game $games[0].Id $games[0].Exe $games[0].Root
    $auto.ExeOverride = $false; $auto.CustomName = 'Custom'
    $manual = Game 'custom:1' $games[0].Exe $games[0].Root 'Manual'
    $manual.CustomCover = 'saved.png'
    $list.Add($auto); $list.Add($manual)
    $selection = $type.GetField('_selectedGame', $instance)
    $selection.SetValue($ctrl, $auto)
    $merge = $type.GetMethod('MergeScannedGames', $instance)
    $null = $merge.Invoke($ctrl, (,([OptScalerNR.Launcher.GameInfo[]]$games)))
    Assert ($list.Count -eq 2 -and $selection.GetValue($ctrl) -eq $manual -and $manual.CustomName -eq 'Custom' -and $manual.CustomCover -eq 'saved.png') 'Lost duplicate, overrides or selection'
    $earlier = Game 'folder' '' $games[0].Root 'Manual'
    $list.Clear(); $list.Add($earlier); $list.Add($games[1])
    $incoming = Game $games[1].Id $games[0].Exe $games[0].Root
    $null = $merge.Invoke($ctrl, (,([OptScalerNR.Launcher.GameInfo[]]@($incoming))))
    Assert ($list.Count -eq 2 -and $earlier.Exe -eq $games[0].Exe -and $games[1].Exe -ne $earlier.Exe) 'Did not retain earliest ID/EXE/root match'
    # Equal folder and EXE names on separate drives must not collapse. Simulate
    # a legacy first-drive row with a real explicit override outside its root.
    $legacy = Game 'xbox:Shared' $games[0].Exe 'C:\XboxGames\Shared\Content'
    $legacy.ExeOverride = $true
    $list.Clear(); $list.Add($legacy)
    $both = [OptScalerNR.Launcher.GameInfo[]]@(
        foreach ($drive in 'C','D') {
            $folder = $drive + ':\XboxGames\Shared'
            Game "xbox:$folder" "$folder\Content\Game.exe" "$folder\Content"
        }
    )
    $null = $merge.Invoke($ctrl, (,$both))
    Assert ($list.Count -eq 2 -and $list[0].Exe -eq $games[0].Exe -and $list[1].Id -eq $both[1].Id) 'Lost cross-volume Xbox game or explicit override'
    $hidden.Add('xbox:Shared', 'C:\XboxGames\Shared\Content\Old.exe')
    $list.Clear()
    $null = $merge.Invoke($ctrl, (,$both))
    Assert ($list.Count -eq 1 -and $list[0].Id -eq $both[1].Id) 'Legacy hide leaked across drives'
    Write-Host 'PASS direct XboxGames, WindowsApps worker/cache rejection, indexed merge and selection'

    $exe = Join-Path $games[0].Root 'bin\星.exe'
    Put $exe 'game'
    $g = Game 'logs' $exe $games[0].Root
    Assert ($data::ComputeAppStorageTag($exe, 'D:/Games/Example/BIN/') -ceq '____1e097cb3') 'Native host tag differs'
    Assert ($data::ComputeAppStorageTag($exe, 'D:\Games\Example\BIN\dlssnr-amd') -ceq '____f0ab12b4') 'Native resource tag differs'
    $dirs = $type.GetMethod('LogDirectories', $static).Invoke($null, @($g.psobject.BaseObject))
    $dest = Join-Path $work 'collection'
    $expected = @{}
    foreach ($dir in $dirs) {
        $group = if ($dir -eq $g.Root) { 'game-root' } else { 'game-exe' }
        $resource = Join-Path $dir 'dlssnr-amd'
        $hostFb = $data::ResolveAppDir($dir, $exe)
        $reportFb = $data::ResolveAppDir($resource, $exe)
        foreach ($file in @{
            "$dir\nr-report-series.txt" = "$group\nr-report-series.txt"
            "$resource\nr-report.txt" = "$group\dlssnr-amd\nr-report.txt"
            "$hostFb\nr-report-series.txt" = "localappdata\$(Split-Path $hostFb -Leaf)\nr-report-series.txt"
            "$reportFb\nr-report.txt" = "localappdata\$(Split-Path $reportFb -Leaf)\nr-report.txt"
        }.GetEnumerator()) {
            Put $file.Key $file.Key
            $expected[(Join-Path $dest $file.Value)] = $file.Key
        }
    }
    $foreign = $games[1].Root
    Put (Join-Path $foreign 'nr-report.txt') 'not selected'
    Put ($data::ResolveAppDir($foreign, $exe) + '\nr-report.txt') 'not selected'
    $copied = $type.GetMethod('CopyGameLogs', $static).Invoke($null, @($dirs.psobject.BaseObject, [string]$exe, [string]$dest))
    Assert ($copied -eq 8 -and $expected.Count -eq 8) 'Reports missing or unexpected sources collected'
    foreach ($file in $expected.Keys) { Assert ([IO.File]::ReadAllText($file) -ceq $expected[$file]) "Wrong source: $file" }
    $g.Root = $foreign
    $dirs = $type.GetMethod('LogDirectories', $static).Invoke($null, @($g.psobject.BaseObject))
    Assert ($dirs.Count -eq 1 -and $dirs[0] -eq [IO.Path]::GetDirectoryName($exe)) 'Unrelated root collected'
    Write-Host 'PASS host/resource tags, eight distinct report sources, foreign root excluded'

    # Import only the production functions; do not run installer startup or cleanup.
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path "$install\install.ps1"), [ref]$null, [ref]$errors)
    Assert (-not $errors) "Installer parse failure: $errors"
    foreach ($fn in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $n.Name -in 'Reset-ModelResourceTree','Remove-OwnedTreeSafe','Assert-LmxxfUnlinkedPath'}, $false)) {
        . ([scriptblock]::Create($fn.Extent.Text))
    }
    foreach ($locked in 'second.bin','stale.dat') {
        $parent = Join-Path $work $locked
        $tree = Join-Path $parent 'dlssnr-amd'
        foreach ($file in 'first.bin','second.bin','stale.dat') { Put "$tree\$file" $file }
        $handle = [IO.File]::OpenRead("$tree\$locked")
        $failure = $null
        try { Reset-ModelResourceTree $tree @('first.bin','second.bin') } catch { $failure = $_.Exception.Message } finally { $handle.Dispose() }
        $stage = @(Get-ChildItem -LiteralPath $parent -Filter '.dlssnr-models-stage-*' -Directory)
        Assert ($stage.Count -eq 1 -and $failure.Contains($stage[0].FullName)) "Missing recovery path after locking $locked"
        foreach ($file in 'first.bin','second.bin') {
            $copies = @(@("$tree\$file", "$($stage[0].FullName)\$file") | Where-Object { [IO.File]::Exists($_) })
            Assert ($copies.Count -eq 1 -and [IO.File]::ReadAllText($copies[0]) -ceq $file) "Lost $file after locking $locked"
        }
    }
    $tree = Join-Path $work 'success\dlssnr-amd'
    Put "$tree\model.bin" 'only copy'; Put "$tree\stale.dat" 'stale'
    Reset-ModelResourceTree $tree @('model.bin')
    Assert ([IO.File]::ReadAllText("$tree\model.bin") -ceq 'only copy' -and !(Test-Path "$tree\stale.dat")) 'Invalid successful refresh'
    # Simulate a competing writer after cleanup: the first restore completes,
    # the next must not overwrite the new file or lose the remaining model.
    $removeTree = (Get-Command Remove-OwnedTreeSafe).ScriptBlock
    function Remove-OwnedTreeSafe([string]$dir) {
        & $removeTree $dir
        Put "$dir\second.bin" 'new writer'
    }
    $tree = Join-Path $work 'collision\dlssnr-amd'
    Put "$tree\first.bin" 'first'; Put "$tree\second.bin" 'second'
    $failure = $null
    try { Reset-ModelResourceTree $tree @('first.bin','second.bin') } catch { $failure = $_.Exception.Message }
    $stage = @(Get-ChildItem (Split-Path $tree -Parent) -Directory -Filter '.dlssnr-models-stage-*')
    Assert ($stage.Count -eq 1 -and $failure.Contains($stage[0].FullName) -and
        [IO.File]::ReadAllText("$tree\first.bin") -ceq 'first' -and [IO.File]::ReadAllText("$tree\second.bin") -ceq 'new writer' -and
        [IO.File]::ReadAllText("$($stage[0].FullName)\second.bin") -ceq 'second') 'Partial restore lost or replaced a file'
    Write-Host 'PASS partial move, locked cleanup, partial restore and successful model restore'
} finally {
    $env:LOCALAPPDATA = $local
    if ([IO.Directory]::Exists($work)) { [IO.Directory]::Delete($work, $true) }
}
