# Exercise actual Setup/Uninstall helpers; never run their top-level code.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fixture = Join-Path $repo ('work/scratch/plugin-path-tests-' + [guid]::NewGuid().ToString('N'))
$oldLocation = Get-Location
function Check-Path([string]$actual, [string]$expected, [string]$label) {
    if ([IO.Path]::GetFullPath($actual) -ine [IO.Path]::GetFullPath($expected)) {
        throw "${label}: expected $expected; got $actual"
    }
}
try {
    $game = Join-Path $fixture 'game/bin'
    $launcher = Join-Path $fixture 'launcher'
    $deps = Join-Path $game 'CustomDeps'
    $shared = Join-Path $fixture 'shared'
    foreach ($dir in @($game, $launcher, $deps, $shared)) { [void][IO.Directory]::CreateDirectory($dir) }
    Set-Location -LiteralPath $launcher
    $ini = Join-Path $game 'OptiScaler.ini'
    foreach ($name in @('install', 'uninstall')) {
        $errors = $null; $tokens = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repo "tools/$name-amd-presr.ps1"), [ref]$tokens, [ref]$errors)
        if ($errors.Count) { throw ($errors | Out-String) }
        foreach ($functionName in @('Get-IniSetting', 'Get-PluginsTargetDirectory')) {
            $fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $functionName }, $true)
            if (-not $fn) { throw "Missing $functionName in $name" }
            . ([ScriptBlock]::Create($fn.Extent.Text))
        }
        if ($name -eq 'install') {
            foreach ($functionName in @('Install-One', 'Install-OptiPatcherFile')) {
                $fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $functionName }, $true)
                if (-not $fn) { throw "Missing $functionName" }
                . ([ScriptBlock]::Create($fn.Extent.Text))
            }
            $keep = @{}
            $gameFullPrefix = [IO.Path]::GetFullPath($game).TrimEnd('\', '/') + '\'
            $sourcePlugin = Join-Path $fixture 'source.asi'
            [IO.File]::WriteAllText($sourcePlugin, 'plugin copy fixture')
        }
        $cases = @(
            @('mods', 'CustomDeps', (Join-Path $game 'mods')),
            @($shared, 'CustomDeps', $shared),
            @('auto', 'CustomDeps', (Join-Path $deps 'plugins')),
            @('AUTO', $shared, (Join-Path $shared 'plugins')),
            @('auto', 'missing-deps', (Join-Path $game 'plugins')),
            @('auto', 'auto', (Join-Path $game 'plugins'))
        )
        foreach ($case in $cases) {
            $text = "[Unrelated]`r`nPath=wrong`r`n[Libraries]`r`nOptiDllPath=$($case[1])`r`n[Plugins]`r`nPath=$($case[0])`r`n"
            [IO.File]::WriteAllText($ini, $text)
            Check-Path (Get-PluginsTargetDirectory $game $ini) $case[2] "$name/$($case[0])/$($case[1])"
            if ($name -eq 'install') {
                $targetPluginsDir = Get-PluginsTargetDirectory $game $ini
                $targetPluginFile = Join-Path $targetPluginsDir 'OptiPatcher.asi'
                Install-OptiPatcherFile $sourcePlugin
                if ([IO.File]::ReadAllText((Join-Path $case[2] 'OptiPatcher.asi')) -cne 'plugin copy fixture') {
                    throw 'Installer did not publish the plugin into the resolved target'
                }
            }
            if ([IO.File]::ReadAllText($ini) -cne $text) { throw 'Path resolution rewrote user preferences' }
        }
        [void][IO.Directory]::CreateDirectory((Join-Path $game 'OptiScaler'))
        [IO.File]::WriteAllText($ini, "[Plugins]`r`nPath=auto`r`n")
        Check-Path (Get-PluginsTargetDirectory $game $ini) (Join-Path $game 'OptiScaler/plugins') "$name/package default"
        [IO.Directory]::Delete((Join-Path $game 'OptiScaler'), $false)
        Write-Host "PASS $name plugin paths: relative, absolute, auto, invalid OptiDllPath; foreign CWD and section isolation"
    }
} finally {
    Set-Location -LiteralPath $oldLocation.Path
    $full = [IO.Path]::GetFullPath($fixture)
    $prefix = [IO.Path]::GetFullPath((Join-Path $repo 'work/scratch')) + '\plugin-path-tests-'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe fixture cleanup: $full" }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
