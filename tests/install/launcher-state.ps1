# Real WPF controller, isolated preferences, no windows shown or workers launched.
[CmdletBinding()]
param([string]$LauncherExe)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'exports/tests/launcher-state'
if ($LauncherExe) { $out = Split-Path -Parent $LauncherExe } else { & (Join-Path $repo 'tools/build/build-launcher.ps1') -OutDir $out }
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml
$assembly = [Reflection.Assembly]::LoadFrom((Join-Path $out 'OptScalerNR.Launcher.exe'))
$controllerType = $assembly.GetType('OptScalerNR.Launcher.MainController', $true)
$flags = [Reflection.BindingFlags]'NonPublic,Instance'
$arguments = New-Object object[] 2
$arguments[0] = [string[]]@()
$arguments[1] = [string](Join-Path $out ('preferences-' + [Guid]::NewGuid().ToString('N')))
$window = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
$controller = $window.Tag
function Field([string]$name) { $controllerType.GetField($name, $flags).GetValue($controller) }
function Call([string]$name, [object[]]$values = @()) {
    for ($i = 0; $i -lt $values.Length; $i++) {
        if ($null -ne $values[$i]) { $values[$i] = $values[$i].PSObject.BaseObject }
    }
    $method = $controllerType.GetMethod($name, ([Reflection.BindingFlags]'Public,NonPublic,Instance'))
    while ($values.Length -lt $method.GetParameters().Length) { $values += [Type]::Missing }
    $method.Invoke($controller, $values)
}
function Check([bool]$condition, [string]$why) { if (-not $condition) { throw $why } }
function File-Bytes([string]$path) { [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) }
function Choose-IniProfile([string]$name) {
    $combo = Field '_iniProfileCombo'
    $item = @($combo.Items | Where-Object { [string]$_.Tag -ceq $name })
    Check ($item.Count -eq 1) "Profile selector does not expose $name"
    $combo.SelectedItem = $item[0]
}
function Dialog-Body($node) {
    if ($node -is [Windows.Controls.StackPanel] -and @($node.Children | Where-Object { $_ -is [Windows.Controls.TextBlock] }).Count) { return $node }
    if ($node -is [Windows.DependencyObject]) {
        foreach ($child in [Windows.LogicalTreeHelper]::GetChildren($node)) {
            $found = Dialog-Body $child
            if ($null -ne $found) { return $found }
        }
    }
    return $null
}
function Check-SharedResources($target, $resources) {
    foreach ($key in @('ThemeBackground', 'ThemeSurface', 'ThemeSubtle', 'ThemeInk', 'ThemeMuted', 'ThemeBorder', 'ThemeAccent', 'ThemeHover', 'ThemeSuccess', 'ThemeSuccessBg', 'ThemeWarning', 'ThemeWarningBg', 'ThemeOnAccent')) {
        $brush = $target.FindResource($key)
        Check ($brush -is [Windows.Media.SolidColorBrush] -and -not $brush.IsFrozen) "$key must remain a mutable shared brush"
        Check ([object]::ReferenceEquals($brush, $resources[$key])) "$key was copied instead of shared"
    }
}
try {
    $themeType = $assembly.GetType('OptScalerNR.Launcher.LauncherTheme', $true)
    $themeResources = $themeType.GetProperty('Resources').GetValue($null, $null)
    Check ($themeResources -is [Windows.ResourceDictionary]) 'Theme resources are not a shared resource dictionary'
    Check-SharedResources $window $themeResources
    foreach ($name in @('PillDxgi', 'PillD3d12', 'PillWinmm', 'PillWinhttp', 'PillWininet', 'PillDbghelp', 'PillVersion')) {
        Check ($window.FindName($name) -is [Windows.Controls.RadioButton]) "Missing proxy control $name"
    }
    $proxies = Field '_proxyPills'
    Check ($proxies.Count -eq 7) 'Supported proxy choices changed'
    foreach ($proxy in @('dxgi.dll', 'd3d12.dll', 'winmm.dll', 'winhttp.dll', 'wininet.dll', 'dbghelp.dll', 'version.dll')) {
        Call 'SelectProxy' @($proxy) | Out-Null
        Check $proxies[$proxy].IsChecked "Selecting $proxy did not check its control"
        Check (@($proxies.Values | Where-Object { $_.IsChecked }).Count -eq 1) 'Proxy choices are not mutually exclusive'
    }
    Call 'SelectProxy' @($null) | Out-Null
    Check (-not $window.FindName('DetailsScroll').Focusable -and -not $window.FindName('DetailsScroll').IsTabStop) 'Details container must not create its own focus stop'
    $statusVm = [Activator]::CreateInstance($assembly.GetType('OptScalerNR.Launcher.GameItemViewModel'))
    $statusVm.Info = [Activator]::CreateInstance($assembly.GetType('OptScalerNR.Launcher.GameInfo'))
    $statusVm.Info.Installed = $true
    Check ([object]::ReferenceEquals($statusVm.StatusBrush, $themeResources['ThemeMuted'])) 'Missing executable status must not use installed success color'
    $dialogTheme = $assembly.GetType('OptScalerNR.Launcher.DialogTheme', $true)
    $keyboardMode = $dialogTheme.GetField('keyboardNavigation', [Reflection.BindingFlags]'Static,NonPublic')
    $keyboardMode.SetValue($null, $true)
    $focusControl = $window.FindName('PillDxgi')
    $focusEvent = New-Object Windows.Input.KeyboardFocusChangedEventArgs ([Windows.Input.Keyboard]::PrimaryDevice), 0, $null, $focusControl
    $focusEvent.RoutedEvent = [Windows.Input.Keyboard]::GotKeyboardFocusEvent
    $focusControl.RaiseEvent($focusEvent)
    Check ($dialogTheme.GetMethod('GetKeyboardFocusVisible').Invoke($null, @($focusControl))) 'Keyboard navigation has no independent focus layer'
    $keyboardMode.SetValue($null, $false)
    $focusControl.RaiseEvent($focusEvent)
    Check (-not $dialogTheme.GetMethod('GetKeyboardFocusVisible').Invoke($null, @($focusControl))) 'Mouse focus retained the keyboard ring'
    $settingsWidths = @()
    foreach ($language in @('en', 'zh')) {
        Call 'SetLanguage' @($language) | Out-Null
        $size = New-Object Windows.Size 1020, 660
        $window.Content.Measure($size)
        $window.Content.Arrange((New-Object Windows.Rect $size))
        $window.Content.UpdateLayout()
        $settings = $window.FindName('SettingsTab')
        $settingsWidths += $settings.ActualWidth
        Check ($settings.ActualWidth -eq 76) 'Settings action must use one language-independent comfortable width'
        foreach ($name in @('MinBtn', 'MaxBtn', 'CloseBtn')) {
            $caption = $window.FindName($name)
            Check ($caption.ActualWidth -eq 32 -and $caption.ActualHeight -eq 32) "$name hit target must be square"
            Check ([Windows.Shell.WindowChrome]::GetIsHitTestVisibleInChrome($caption)) "$name lost chrome interaction"
            Check ($caption.Focusable -and $caption.IsTabStop) "$name lost keyboard access"
        }
    }
    Check ($settingsWidths[0] -eq $settingsWidths[1]) 'Language change resized Settings'

    # Loading old, explicit and unsupported theme preferences must never rewrite existing settings.
    foreach ($savedTheme in @('missing', 'auto', 'light', 'dark', 'unsupported')) {
        $directory = Join-Path ([string]$arguments[1]) ('theme-' + $savedTheme)
        [void][IO.Directory]::CreateDirectory($directory)
        $path = Join-Path $directory 'settings.json'
        $preferences = @{ Language = 'en'; ScanOnStartup = $false; PackageRoot = 'D:\My Package'; RecentGameId = 'remember-me' }
        if ($savedTheme -ne 'missing') { $preferences.Theme = $savedTheme }
        $original = $preferences | ConvertTo-Json
        [IO.File]::WriteAllText($path, $original)
        $themeArguments = New-Object object[] 2
        $themeArguments[0] = [string[]]@()
        $themeArguments[1] = [string]$directory
        $loaded = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $themeArguments)
        try {
            $expected = if ($savedTheme -in @('auto', 'light', 'dark')) { $savedTheme } else { 'auto' }
            Check ([IO.File]::ReadAllText($path) -ceq $original) "Loading $savedTheme theme rewrote settings"
            Check (-not $controllerType.GetField('_diagnosticMode', $flags).GetValue($loaded.Tag)) 'Saved settings enabled diagnostic mode without confirmation'
            Check ($controllerType.GetField('_packageRoot', $flags).GetValue($loaded.Tag) -eq (Split-Path -Parent $assembly.Location)) 'Historical package override became the normal source'
            Check ($loaded.FindName('DiagnosticPackagePanel').Visibility -eq 'Collapsed') 'Normal mode exposed package override'
            Check ($loaded.FindName('ThemeCombo').SelectedItem.Tag -eq $expected) "Loading $savedTheme theme did not select $expected"
            Check-SharedResources $loaded $themeResources
            foreach ($theme in @('auto', 'light', 'dark')) {
                $themeValue = $theme.PSObject.BaseObject
                $controllerType.GetMethod('SetTheme', $flags).Invoke($loaded.Tag, @($themeValue)) | Out-Null
                Check ($loaded.FindName('ThemeCombo').SelectedItem.Tag -eq $theme) 'Explicit theme change did not synchronize ThemeCombo'
                $saved = [IO.File]::ReadAllText($path) | ConvertFrom-Json
                Check ($saved.Theme -eq $theme) "Explicit $theme theme was not persisted"
                Check ($saved.Language -eq 'en' -and -not $saved.ScanOnStartup -and $saved.PackageRoot -eq 'D:\My Package' -and $saved.RecentGameId -eq 'remember-me') 'Theme change clobbered unrelated preferences'
            }
        } finally { $loaded.Close() }
        $persisted = [IO.File]::ReadAllText($path)
        $loaded = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $themeArguments)
        try {
            Check ($loaded.FindName('ThemeCombo').SelectedItem.Tag -eq 'dark') 'Restart lost the explicit theme preference'
            Check ([IO.File]::ReadAllText($path) -ceq $persisted) 'Loading the explicitly saved theme rewrote settings'
        } finally { $loaded.Close() }
    }
    # A remembered source is inert until this session explicitly enables diagnostic mode.
    $normalRoot = Split-Path -Parent $assembly.Location
    $diagnosticRoot = Join-Path ([string]$arguments[1]) 'diagnostic-package'
    [void][IO.Directory]::CreateDirectory($diagnosticRoot)
    [IO.File]::WriteAllText((Join-Path $diagnosticRoot 'launcher-worker.ps1'), '# isolated diagnostic worker')
    [IO.File]::WriteAllText((Join-Path $diagnosticRoot 'VERSION'), '9.8.7-test')
    $controllerType.GetField('_diagnosticPackageRoot', $flags).SetValue($controller, $diagnosticRoot)
    Call 'SetDiagnosticPackageRoot' @($diagnosticRoot) | Out-Null
    Check ((Field '_packageRoot') -eq $normalRoot) 'Normal mode allowed editing the active override'
    Call 'SetDiagnosticMode' @($true) | Out-Null
    Check ((Field '_diagnosticMode') -and (Field '_packageRoot') -eq $diagnosticRoot) 'Enabled diagnostic mode did not use its remembered source'
    Check ($controllerType.GetProperty('WorkerScript', $flags).GetValue($controller, $null) -eq (Join-Path $diagnosticRoot 'launcher-worker.ps1')) 'Worker did not follow the authorized diagnostic source'
    Check ($window.FindName('DiagnosticModeIndicator').Visibility -eq 'Visible' -and $window.FindName('DiagnosticPackagePanel').Visibility -eq 'Visible') 'Active diagnostic mode is not visible'
    Check ([string]$window.FindName('DiagnosticModeIndicator').ToolTip -match '9.8.7-test') 'Diagnostic source version is not visible in details'
    Call 'SetBusy' @($true, 'install') | Out-Null
    Call 'SetDiagnosticMode' @($false) | Out-Null
    Call 'SetDiagnosticPackageRoot' @('Z:\must-not-activate') | Out-Null
    Check ((Field '_diagnosticMode') -and (Field '_packageRoot') -eq $diagnosticRoot) 'In-flight installation source changed'
    Check (-not $window.FindName('DiagnosticModeButton').IsEnabled -and -not $window.FindName('PackageBrowse').IsEnabled) 'Source controls enabled during writes'
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'SaveSettings' | Out-Null
    $restart = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
    try {
        Check (-not $controllerType.GetField('_diagnosticMode', $flags).GetValue($restart.Tag)) 'Diagnostic mode survived restart'
        Check ($controllerType.GetField('_packageRoot', $flags).GetValue($restart.Tag) -eq $normalRoot) 'Restart activated remembered diagnostic source'
        Check ($controllerType.GetField('_diagnosticPackageRoot', $flags).GetValue($restart.Tag) -eq $diagnosticRoot) 'Restart lost the remembered diagnostic path'
    } finally { $restart.Close() }
    Check (Call 'TryLockTarget' @([string]$arguments[1])) 'Cannot acquire diagnostic failure fixture lock'
    Call 'SetBusy' @($true, 'update') | Out-Null
    Call 'SetBusy' @($false, '') | Out-Null
    Check (-not $window.FindName('DiagnosticModeButton').IsEnabled) 'Live target lock permitted package source changes'
    Call 'ReleaseLock' | Out-Null
    Check ($window.FindName('DiagnosticModeButton').IsEnabled -and $window.FindName('PackageBrowse').IsEnabled) 'Completed mutation left diagnostic controls disabled'
    Call 'SetDiagnosticPackageRoot' @('Z:\invalid"source') | Out-Null
    Check (-not (Call 'HasLocalPackage')) 'Malformed diagnostic source passed preflight'
    Call 'SetDiagnosticPackageRoot' @($diagnosticRoot) | Out-Null
    Call 'SetDiagnosticMode' @($false) | Out-Null
    Check ((Field '_packageRoot') -eq $normalRoot -and $window.FindName('DiagnosticModeIndicator').Visibility -eq 'Collapsed') 'Disabling mode did not restore ordinary package routing'
    $controllerType.GetField('_diagnosticPackageRoot', $flags).SetValue($controller, '')
    Check ($window.FindName('AuthorLink').NavigateUri.AbsoluteUri -eq 'https://github.com/TheAutomatic/dlss-5-amd-project') 'Footer author link points elsewhere'


    $backendType = $assembly.GetType('OptScalerNR.Launcher.BackendInfo', $true)
    $resultType = $assembly.GetType('OptScalerNR.Launcher.LauncherResult', $true)
    $capabilities = [Activator]::CreateInstance($resultType)
    $capabilities.Backends = [Array]::CreateInstance($backendType, 3)
    $ids = @('lmxxf', 'daniel', 'mochizuki')
    for ($i = 0; $i -lt $ids.Count; $i++) {
        $backend = [Activator]::CreateInstance($backendType)
        $backend.Id = $ids[$i]; $backend.Enabled = $true; $backend.ReasonCode = 'ready'
        $capabilities.Backends[$i] = $backend
    }
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Fresh inspection did not select the default ready backend'
    Call 'SetActiveBackend' @('lmxxf') | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Capability refresh discarded the selected active backend'
    Call 'SetActiveBackend' @('daniel') | Out-Null
    $capabilities.Backends[1].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Unavailable backend was not replaced with a ready backend'
    $capabilities.Backends[2].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Default did not fall back when Mochi is unavailable'

    $gameType = $assembly.GetType('OptScalerNR.Launcher.GameInfo', $true)
    $a = [Activator]::CreateInstance($gameType)
    $a.Id = 'A'; $a.Name = 'A'; $a.Exe = ''
    $b = [Activator]::CreateInstance($gameType)
    $b.Id = 'B'; $b.Name = 'B'; $b.Exe = ''
    foreach ($backend in $capabilities.Backends) { $backend.Enabled = $true }
    Call 'SelectGame' @($a) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    $window.FindName('DanielCheck').IsChecked = $false
    Call 'SetActiveBackend' @('lmxxf') | Out-Null
    Call 'SelectGame' @($b) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Backend choice leaked to another game'
    Call 'SelectGame' @($a) | Out-Null
    $capabilities.Backends[0].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Missing files silently changed the chosen backend'
    Check ($window.FindName('LmxxfCheck').IsChecked -eq $true) 'Inspection discarded the selected missing backend'
    Check ($window.FindName('DanielCheck').IsChecked -eq $false) 'Inspection reselected an unchecked backend'

    # Theme switches update existing controls/dialogs without refreshing inspection or losing choices.
    Call 'SelectProxy' @('winmm.dll') | Out-Null
    $window.FindName('ReplaceIni').IsChecked = $false
    $inspect = Field '_lastInspectResult'
    $checkedBackends = @((Call 'CheckedBackends')) -join '|'
    $proxyControl = $window.FindName('PillWinmm')
    $reasonControl = $window.FindName('MochiReason')
    $reasonBrush = $reasonControl.Foreground
    Check (@($themeResources.Values | Where-Object { [object]::ReferenceEquals($_, $reasonBrush) }).Count -gt 0) 'Backend status uses a private brush instead of shared theme resources'
    $modelDialog = Call 'CreateMochizukiModelDialog'
    try {
        Check-SharedResources $modelDialog $themeResources
        $dialogBackground = $modelDialog.Background
        foreach ($theme in @('light', 'dark', 'auto', 'light')) {
            Call 'SetTheme' @($theme) | Out-Null
            if ($theme -eq 'light') { $lightBackground = $window.FindResource('ThemeBackground').Color }
            if ($theme -eq 'dark') { Check ($window.FindResource('ThemeBackground').Color -ne $lightBackground) 'Light and dark themes have the same background' }
            Check ($window.Background.Color -eq $window.FindResource('ThemeBackground').Color) 'Existing main window did not follow the palette'
            Check-SharedResources $window $themeResources
            Check-SharedResources $modelDialog $themeResources
            Check ([object]::ReferenceEquals($window.FindName('PillWinmm'), $proxyControl)) 'Theme switch recreated proxy controls'
            Check ([object]::ReferenceEquals($window.FindName('MochiReason'), $reasonControl)) 'Theme switch recreated backend controls'
            Check ([object]::ReferenceEquals($reasonControl.Foreground, $reasonBrush)) 'Theme switch replaced an existing status brush'
            Check ([object]::ReferenceEquals($modelDialog.Background, $dialogBackground)) 'Theme switch replaced an existing dialog background brush'
            Check ($modelDialog.Background.Color -eq $window.FindResource('ThemeSurface').Color) 'Existing model dialog did not follow the palette'
            Check ([object]::ReferenceEquals((Field '_lastInspectResult'), $inspect)) 'Theme switch replaced inspection results'
            Check ($proxyControl.IsChecked -eq $true) 'Theme switch discarded the selected proxy'
            Check ($window.FindName('DanielCheck').IsChecked -eq $false) 'Theme switch reselected an unchecked backend'
            Check ((@((Call 'CheckedBackends')) -join '|') -eq $checkedBackends) 'Theme switch changed backend selection'
            Check ($window.FindName('ReplaceIni').IsChecked -eq $false) 'Theme switch reset ReplaceIni'
            Check ((Field '_activeBackend') -eq 'lmxxf') 'Theme switch changed the active backend'
        }
    } finally { $modelDialog.Close() }

    $reopened = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
    try {
        $controllerType.GetMethod('SelectGame', $flags).Invoke($reopened.Tag, @($a)) | Out-Null
        Check ($controllerType.GetField('_activeBackend', $flags).GetValue($reopened.Tag) -eq 'lmxxf') 'Restart lost the active backend'
        Check ($reopened.FindName('DanielCheck').IsChecked -eq $false) 'Restart lost the backend checkboxes'
    } finally { $reopened.Close() }
    Call 'SelectGame' @($b) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    foreach ($name in @('MochiCheck', 'LmxxfCheck', 'DanielCheck')) { $window.FindName($name).IsChecked = $false }
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Call 'CheckedBackends').Count -eq 0) 'Inspection ignored the deliberate empty backend selection'
    Check ((Field '_activeBackend') -eq '') 'Empty selection retained a misleading active marker'

    # Check first, then judge: exercise actual checkbox events against cached capabilities.
    $presentationGame = [Activator]::CreateInstance($gameType)
    $presentationGame.Id = 'backend-presentation'; $presentationGame.Name = 'Backend presentation'
    $presentationGame.Exe = $assembly.Location
    $controllerType.GetField('_selectedGame', $flags).SetValue($controller, $presentationGame)
    $presentation = [Activator]::CreateInstance($resultType)
    $presentation.Status = 'success'; $presentation.PackageRoot = $out
    $presentation.Backends = [Array]::CreateInstance($backendType, 3)
    for ($i = 0; $i -lt $ids.Count; $i++) {
        $presentation.Backends[$i] = [Activator]::CreateInstance($backendType)
        $presentation.Backends[$i].Id = $ids[$i]
    }
    $lmxxf = $presentation.Backends[0]
    $lmxxf.ReasonCode = 'missing-runtime'; $lmxxf.RuntimeState = 'missing'
    $lmxxf.OriginalState = 'missing'; $lmxxf.SmallState = 'missing'
    $lmxxf.Hints = [string[]]@('lmxxf-original', 'lmxxf-small')
    $lmxxf.MissingPackageFiles = [string[]]@('LmxxfNrRuntime.dll')
    $daniel = $presentation.Backends[1]
    $daniel.ReasonCode = 'missing-runtime'; $daniel.RuntimeState = 'missing'; $daniel.ModelState = 'missing'
    $daniel.Hints = [string[]]@('daniel-runtime', 'daniel-weights')
    $daniel.MissingPackageFiles = [string[]]@('dlssnr_on_amd.dll')
    $mochi = $presentation.Backends[2]
    $mochi.Enabled = $true; $mochi.ReasonCode = 'ready'; $mochi.RuntimeState = 'ready'
    $mochi.OriginalState = 'ready'; $mochi.SmallState = 'ready'; $mochi.ModelState = 'ready'
    Call 'SetLanguage' @('en') | Out-Null
    Call 'ApplyInspect' @($presentation) | Out-Null
    foreach ($prefix in @('Lmxxf', 'Daniel')) {
        Check ($window.FindName($prefix + 'Check').IsChecked -eq $false) "$prefix missing backend was selected automatically"
        Check ($window.FindName($prefix + 'Reason').Text -eq 'Not selected') "$prefix unchecked backend displayed a complaint"
        Check ([object]::ReferenceEquals($window.FindName($prefix + 'Reason').Foreground, $themeResources['ThemeMuted'])) "$prefix unchecked status was not neutral"
        Check ([object]::ReferenceEquals($window.FindName($prefix + 'Badge').Background, $themeResources['ThemeSubtle'])) "$prefix unchecked badge was not neutral"
        Check ([object]::ReferenceEquals($window.FindName($prefix + 'Badge').BorderBrush, $themeResources['ThemeBorder'])) "$prefix unchecked badge retained an error border"
        Check ($window.FindName($prefix + 'Hint').Text -eq '' -and $window.FindName($prefix + 'Hint').Visibility -eq 'Collapsed') "$prefix unchecked hint was visible"
    }
    Check ($window.FindName('BackendDetailsText').Text -eq '' -and $window.FindName('BackendDetails').Visibility -eq 'Collapsed') 'Unchecked backends leaked missing component details'
    Check ($window.FindName('BackendPackagePath').Visibility -eq 'Collapsed') 'Unchecked hints retained a package path'
    Check ((Call 'BackendHintText' @($lmxxf)) -eq '' -and (Call 'BackendHintText' @($daniel)) -eq '') 'Hint formatter bypassed selection gating'
    foreach ($prefix in @('Lmxxf', 'Daniel')) {
        $window.FindName($prefix + 'Check').IsChecked = $true
        Check ($window.FindName($prefix + 'Reason').Text -eq 'Missing Runtime') "$prefix selection did not immediately show missing status"
        Check ($window.FindName($prefix + 'Hint').Visibility -eq 'Visible') "$prefix selection did not immediately reveal hints"
    }
    Check ($window.FindName('LmxxfHint').Text.Contains('lmxxf Original') -and $window.FindName('LmxxfHint').Text.Contains('lmxxf Small')) 'lmxxf missing model hints were ambiguous'
    Check ($window.FindName('DanielHint').Text.Contains('version.dll') -and $window.FindName('DanielHint').Text.Contains('weights')) 'Selected Daniel omitted runtime or model hints'
    Check ($window.FindName('BackendDetailsText').Text.Contains('LmxxfNrRuntime.dll') -and $window.FindName('BackendDetailsText').Text.Contains('dlssnr_on_amd.dll')) 'Selected missing package details were hidden'
    $window.FindName('DanielCheck').IsChecked = $false
    Check ($window.FindName('DanielReason').Text -eq 'Not selected' -and $window.FindName('DanielHint').Text -eq '') 'Deselecting Daniel retained missing status or hints'
    Check (-not $window.FindName('BackendDetailsText').Text.Contains('daniel:') -and $window.FindName('BackendDetailsText').Text.Contains('lmxxf:')) 'Deselecting Daniel did not filter component details'

    # Mochizuki Small is ready while the independent lmxxf Small still requires preparation.
    $lmxxf.Enabled = $true; $lmxxf.ReasonCode = 'partial'; $lmxxf.RuntimeState = 'ready'
    $lmxxf.OriginalState = 'ready'; $lmxxf.SmallState = 'preparable'
    $lmxxf.Hints = [string[]]@('lmxxf-small-preparable'); $lmxxf.MissingPackageFiles = [string[]]@()
    Call 'ApplyInspect' @($presentation) | Out-Null
    Check ($window.FindName('LmxxfReason').Text -eq 'Some Models Ready') 'Selected lmxxf partial readiness was hidden'
    Check ($window.FindName('LmxxfHint').Text.Contains('lmxxf Small') -and $window.FindName('LmxxfHint').Text.Contains('extracted')) 'Selected preparable lmxxf Small hint missing or ambiguous'
    Check ($window.FindName('MochiReason').Text -eq 'Ready' -and $window.FindName('MochiHint').Text -eq '') 'lmxxf Small preparation was attributed to ready Mochizuki'
    Check ($window.FindName('BackendDetails').Visibility -eq 'Collapsed') 'Cleared package components retained stale details'
    foreach ($language in @('zh', 'en')) {
        Call 'SetLanguage' @($language) | Out-Null
        # Keep this BOM-less script ASCII so Windows PowerShell 5.1 decodes it consistently.
        $notSelected = if ($language -eq 'zh') { -join ([char[]]@(0x672A, 0x9009, 0x62E9)) } else { 'Not selected' }
        Check ($window.FindName('DanielReason').Text -eq $notSelected) 'Unselected status did not retranslate'
        Check ($window.FindName('LmxxfHint').Text.Contains('lmxxf Small')) 'Retranslation lost qualified lmxxf Small hint'
        Check ($window.FindName('LmxxfCheck').IsChecked -eq $true -and $window.FindName('MochiCheck').IsChecked -eq $true -and $window.FindName('DanielCheck').IsChecked -eq $false) 'Presentation refresh reset backend selections'
        Check ([object]::ReferenceEquals((Field '_lastInspectResult'), $presentation)) 'Presentation refresh reapplied or replaced inspection'
    }
    $window.FindName('LmxxfCheck').IsChecked = $false
    Check ($window.FindName('LmxxfReason').Text -eq 'Not selected' -and $window.FindName('LmxxfHint').Text -eq '' -and $window.FindName('LmxxfHint').Visibility -eq 'Collapsed') 'Deselecting preparable lmxxf retained status or hints'
    Check ($window.FindName('BackendPackagePath').Visibility -eq 'Collapsed') 'Deselecting preparable lmxxf retained the hint package path'
    $lmxxf.ReasonCode = 'preparable'
    $lmxxf.OriginalState = 'preparable'; $lmxxf.Hints = [string[]]@('lmxxf-original-preparable', 'lmxxf-small-preparable')
    Call 'ApplyInspect' @($presentation) | Out-Null
    Check ($window.FindName('LmxxfCheck').IsChecked -eq $false -and $window.FindName('LmxxfHint').Text -eq '') 'Inspection reselected or revealed unchecked preparable lmxxf'
    $window.FindName('LmxxfCheck').IsChecked = $true
    Check ($window.FindName('LmxxfReason').Text -eq 'Preparation Required') 'Selecting preparable lmxxf did not show full readiness'
    Check ($window.FindName('LmxxfHint').Text.Contains('lmxxf Original') -and $window.FindName('LmxxfHint').Text.Contains('lmxxf Small')) 'Combined preparable lmxxf hint omitted qualified model names'

    # Missing EXE and failed/pending inspection must also leave every unchecked chip neutral.
    $presentationGame.Exe = ''
    Call 'RefreshInspectionText' | Out-Null
    Check ($window.FindName('LmxxfReason').Text -eq 'Select EXE first' -and $window.FindName('DanielReason').Text -eq 'Not selected') 'Missing EXE status ignored checkbox selection'
    foreach ($prefix in @('Lmxxf', 'Daniel', 'Mochi')) { $window.FindName($prefix + 'Check').IsChecked = $false }
    Check ($window.FindName('LmxxfHint').Text -eq '' -and $window.FindName('BackendDetailsText').Text -eq '') 'Deselecting with missing EXE retained hints or components'
    $presentationGame.Exe = $assembly.Location
    $presentation.Status = 'failed'; $presentation.Error = 'fixture inspection failure'
    Call 'ApplyInspect' @($presentation) | Out-Null
    foreach ($prefix in @('Lmxxf', 'Daniel', 'Mochi')) {
        Check ($window.FindName($prefix + 'Reason').Text -eq 'Not selected') "$prefix unchecked backend complained after inspection failure"
    }
    $controllerType.GetField('_lastInspectResult', $flags).SetValue($controller, $null)
    Call 'RefreshInspectionText' | Out-Null
    foreach ($prefix in @('Lmxxf', 'Daniel', 'Mochi')) {
        Check ($window.FindName($prefix + 'Reason').Text -eq 'Not selected') "$prefix unchecked backend complained before inspection"
    }

    # Compatibility repair is independent of backend/model/proxy installation choices.
    Call 'SelectGame' @($a) | Out-Null
    $jsonType = $assembly.GetType('OptScalerNR.Launcher.JsonUtil', $true)
    $read = $jsonType.GetMethod('Read').MakeGenericMethod($resultType)
    $compat = $read.Invoke($null, @('{"schema":1,"compatibility":{"groups":[{"id":"fg","ready":true,"different":true,"files":[{"name":"libxess_fg.dll","state":"different","gameVersion":"1.0","packageVersion":"3.0"},{"name":"libxell.dll","state":"missing","packageVersion":"2.0"}]}],"active":false,"fingerprint":"token","evidence":[]}}'))
    Call 'ApplyInspect' @($compat) | Out-Null
    Check $window.FindName('CompatButton').IsEnabled 'Repair requires an installed backend or selected proxy'
    Check (-not $window.FindName('RestoreCompatButton').IsEnabled) 'Restore enabled without backups'
    Check ($window.FindName('CompatSummary').Text.Contains('libxess_fg.dll')) 'Automatic comparison summary hidden'
    $dialog = Call 'CreateCompatibilityDialog' @($false)
    try {
        Check-SharedResources $dialog $themeResources
        foreach ($theme in @('dark', 'light')) {
            Call 'SetTheme' @($theme) | Out-Null
            Check ($dialog.Background.Color -eq $window.FindResource('ThemeSurface').Color) 'Existing compatibility dialog did not follow the palette'
        }
        $panel = Dialog-Body $dialog
        $checks = @($panel.Children | Where-Object { $_ -is [Windows.Controls.CheckBox] })
        Check ($checks.Count -eq 1) 'Paired XeFG/XeLL exposed as separate choices'
        Check ($checks[0].IsChecked -ne $true) 'Repair was selected without a user choice'
        $buttons = $panel.Children[$panel.Children.Count - 1]
        Check (-not $buttons.Children[1].IsEnabled) 'Repair allowed without component selection'
        $checks[0].IsChecked = $true
        Check $buttons.Children[1].IsEnabled 'Selecting a group did not enable repair'
        $size = New-Object Windows.Size 650,650
        $dialog.Content.Measure($size)
        $dialog.Content.Arrange((New-Object Windows.Rect $size))
        $dialog.Content.UpdateLayout()
        $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap 650,650,96,96,([Windows.Media.PixelFormats]::Pbgra32)
        $bitmap.Render($dialog.Content)
        $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
        $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
        $stream = [IO.File]::Create((Join-Path $out 'xess-repair-dialog.png'))
        try { $encoder.Save($stream) } finally { $stream.Dispose() }
    } finally { $dialog.Close() }
    $compat.Compatibility.Active = $true
    $compat.Compatibility.RestoreFiles = [string[]]@('libxess_fg.dll','libxell.dll')
    Call 'ApplyInspect' @($compat) | Out-Null
    Check $window.FindName('RestoreCompatButton').IsEnabled 'Restoration hidden after repair'
    $dialog = Call 'CreateCompatibilityDialog' @($true)
    try {
        $texts = @((Dialog-Body $dialog).Children | Where-Object { $_ -is [Windows.Controls.TextBlock] } | ForEach-Object { $_.Text }) -join '|'
        Check ($texts.Contains('libxess_fg.dll') -and $texts.Contains('libxell.dll')) 'Restore preview omitted actual files'
        Check (-not $texts.Contains('libxess_dx11.dll')) 'Restore preview claims unrelated files'
    } finally { $dialog.Close() }
    Call 'SetBusy' @($true, 'repairXess') | Out-Null
    Call 'UpdatePrimaryButtonState' | Out-Null
    Check (-not $window.FindName('CompatButton').IsEnabled) 'Repair button enabled during writes'
    Check (-not $window.FindName('RestoreCompatButton').IsEnabled) 'Restore button enabled during writes'
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'SelectGame' @($a) | Out-Null
    (Field '_decisions').ForeignProxy['dxgi.dll'] = 'backup'
    Check (Call 'TryLockTarget' @([string]$arguments[1])) 'Cannot acquire isolated target lock'
    Call 'SetBusy' @($true, 'install') | Out-Null
    Check (-not (Field '_gameList').IsEnabled) 'Game list enabled during write'
    Call 'SelectGame' @($b) | Out-Null
    Check ((Field '_selectedGame').Id -eq 'A') 'Write target changed during installation'
    Check ((Field '_decisions').ForeignProxy['dxgi.dll'] -eq 'backup') 'In-flight decisions changed'
    # Also covers a worker that exited before its queued completion callback runs.
    $closing = New-Object System.ComponentModel.CancelEventArgs
    Call 'OnClosing' @($window, $closing) | Out-Null
    Check $closing.Cancel 'Window closed before write completion'
    Check ($null -ne (Field '_targetMutex')) 'Closing released the live target lock'
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'SelectGame' @($b) | Out-Null
    Check ((Field '_selectedGame').Id -eq 'B') 'Game selection did not resume'
    Check ((Field '_decisions').ForeignProxy.Count -eq 0) 'Foreign proxy consent leaked to the next game'
    $closing = New-Object System.ComponentModel.CancelEventArgs
    Call 'OnClosing' @($window, $closing) | Out-Null
    Check (-not $closing.Cancel) 'Window cannot close after completion'
    Check ($null -eq (Field '_targetMutex')) 'Completed target lock was retained'

    # Primary action follows target selection and remains disabled during writes.
    Call 'SetLanguage' @('en') | Out-Null
    Call 'SelectGame' @($null) | Out-Null
    Check (-not $window.FindName('PrimaryButton').IsEnabled) 'Primary action enabled without a game'
    Call 'SelectGame' @($b) | Out-Null
    Check $window.FindName('PrimaryButton').IsEnabled 'Game folder cannot request an executable'
    Call 'SetBusy' @($true, 'install') | Out-Null
    Call 'UpdatePrimaryButtonState' | Out-Null
    Check (-not $window.FindName('PrimaryButton').IsEnabled) 'Busy operation enabled primary action'
    Call 'SetBusy' @($false, '') | Out-Null
    $window.FindName('StatusText').Text = 'Business operation status'
    $controllerType.GetField('_hardwareComplete', $flags).SetValue($controller, $true)
    Call 'RefreshHardwareText' | Out-Null
    Check ($window.FindName('StatusText').Text -eq 'Business operation status') 'Hardware presentation overwrote business status'
    # Shared profiles use only fixtures beneath the injected preference directory.
    $profileType = $assembly.GetType('OptScalerNR.Launcher.IniProfiles', $true)
    $profileConstructor = $profileType.GetConstructor([type[]]@([string]))
    $profileSettings = Join-Path ([string]$arguments[1]) 'profile-store'
    $profiles = $profileConstructor.Invoke(@([string]$profileSettings))
    Check ($profiles.RootDirectory -eq (Join-Path $profileSettings 'ini-profiles')) 'Injected preferences escaped into the product profile library'
    Check (@($profiles.List()).Count -eq 0) 'New isolated profile library is not empty'
    Check (-not [IO.File]::Exists($profiles.MetadataPath)) 'Reading a new profile library wrote metadata'
    $source = Join-Path ([string]$arguments[1]) 'source.ini'
    $sourceText = "; fixture profile`r`n[OptiScaler]`r`nFixtureValue=from-profile`r`n[Plugins]`r`nLoadAsiPlugins=true`r`n"
    [IO.File]::WriteAllText($source, $sourceText)
    $savedName = $profiles.Save($source, 'Shared fixture')
    $savedFiles = [IO.Directory]::GetFiles($profiles.RootDirectory, '*.ini')
    Check ($savedFiles.Count -eq 1) 'Saving a profile did not create exactly one shared INI'
    Check ((File-Bytes $savedFiles[0]) -ceq (File-Bytes $source)) 'Saving a profile changed its bytes'
    # Native auto/absent plugin settings resolve false; applying a shared true profile must not opt in.
    foreach ($mode in @('auto', 'absent', 'missing-ini')) {
        $defaultTarget = Join-Path ([string]$arguments[1]) ('plugin-default-' + $mode)
        [void][IO.Directory]::CreateDirectory($defaultTarget)
        $defaultIni = Join-Path $defaultTarget 'OptiScaler.ini'
        if ($mode -eq 'auto') { [IO.File]::WriteAllText($defaultIni, "[Plugins]`r`nLoadAsiPlugins=auto`r`n") }
        if ($mode -eq 'absent') { [IO.File]::WriteAllText($defaultIni, "[OptiScaler]`r`nFixture=untouched`r`n") }
        $enabled = $profiles.GetPluginsForGame('default-' + $mode, $defaultTarget)
        Check (-not $enabled) "Native $mode plugin default unexpectedly enabled ASI"
        if ($mode -ne 'missing-ini') {
            $profiles.Apply($defaultTarget, $savedName, $enabled) | Out-Null
            Check ([IO.File]::ReadAllText($defaultIni) -match '(?im)^LoadAsiPlugins\s*=\s*false\s*$') "Applying a profile opted into ASI for $mode"
        }
    }
    $importedName = $profiles.Import($source, 'Shared fixture')
    Check ($importedName -cne $savedName) 'Import overwrote an existing same-name profile'
    Check (@($profiles.List()).Count -eq 2) 'Save/import did not share one library'
    $profiles.SetSelection('fixture-A', $savedName)
    $profiles.SetSelection('fixture-B', $savedName)
    $profiles.SetPlugins('fixture-A', $false)
    $profiles.SetPlugins('fixture-B', $true)
    $profiles = $profileConstructor.Invoke(@([string]$profileSettings))
    Check ($profiles.GetSelection('fixture-A') -ceq $savedName -and $profiles.GetSelection('fixture-B') -ceq $savedName) 'Restart lost shared profile references'
    Check (-not $profiles.GetPlugins('fixture-A') -and $profiles.GetPlugins('fixture-B')) 'Restart lost per-game plugin preferences'
    $target = Join-Path ([string]$arguments[1]) 'profile-target'
    [void][IO.Directory]::CreateDirectory($target)
    $targetIni = Join-Path $target 'OptiScaler.ini'
    [IO.File]::WriteAllText($targetIni, "; pre-apply fixture`r`n[OptiScaler]`r`nFixtureValue=old`r`n")
    $before = File-Bytes $targetIni
    $backup = $profiles.Apply($target, $savedName, $false)
    Check ([IO.File]::Exists($backup) -and [IO.Path]::GetDirectoryName($backup) -eq $target) 'Profile apply did not return a local backup'
    Check ((File-Bytes $backup) -ceq $before) 'Profile backup did not preserve the previous INI bytes'
    $applied = [IO.File]::ReadAllText($targetIni)
    Check ($applied.Contains('FixtureValue=from-profile')) 'Profile apply did not replace the target configuration'
    Check ($applied -match '(?im)^LoadAsiPlugins\s*=\s*false\s*$') 'Profile apply ignored disabled plugins'
    $fallbackMetadata = [IO.File]::ReadAllText($profiles.MetadataPath)
    Check (-not $profiles.GetPluginsForGame('unrecorded-game', $target)) 'Unrecorded game ignored disabled plugins in its current INI'
    Check ($profiles.GetPluginsForGame('fixture-B', $target)) 'Current INI overrode an explicit saved plugin preference'
    Check ([IO.File]::ReadAllText($profiles.MetadataPath) -ceq $fallbackMetadata) 'Reading current plugin state wrote metadata'
    $before = File-Bytes $targetIni
    $pluginBackup = $profiles.Apply($target, '', $true)
    Check ($pluginBackup -ne $backup -and (File-Bytes $pluginBackup) -ceq $before) 'Plugin-only apply did not retain its own exact backup'
    $applied = [IO.File]::ReadAllText($targetIni)
    Check ($applied.Contains('FixtureValue=from-profile') -and $applied -match '(?im)^LoadAsiPlugins\s*=\s*true\s*$') 'Plugin-only apply replaced unrelated settings or failed to enable plugins'
    Check ($profiles.GetPluginsForGame('unrecorded-game', $target)) 'Unrecorded game ignored enabled plugins in its current INI'
    Check (-not $profiles.GetPluginsForGame('fixture-A', $target)) 'Enabled INI overrode an explicit disabled plugin preference'
    $profiles.Rename($savedName, 'Renamed fixture')
    $profiles = $profileConstructor.Invoke(@([string]$profileSettings))
    Check (@($profiles.List()) -contains 'Renamed fixture') 'Renamed profile did not persist'
    Check (@($profiles.List()) -notcontains $savedName) 'Rename retained the obsolete profile'
    Check ($profiles.GetSelection('fixture-A') -eq 'Renamed fixture' -and $profiles.GetSelection('fixture-B') -eq 'Renamed fixture') 'Rename did not persist every game reference'
    $profiles.Delete('Renamed fixture')
    $profiles = $profileConstructor.Invoke(@([string]$profileSettings))
    Check (@($profiles.List()).Count -eq 1 -and @($profiles.List()) -contains $importedName) 'Delete removed the wrong shared profile'
    Check ([string]::IsNullOrEmpty($profiles.GetSelection('fixture-A')) -and [string]::IsNullOrEmpty($profiles.GetSelection('fixture-B'))) 'Delete retained dangling profile references'
    Check (-not $profiles.GetPlugins('fixture-A') -and $profiles.GetPlugins('fixture-B')) 'Rename/delete clobbered plugin preferences'

    # Controller paths enforce busy guards and reset the selected profile between games.
    $gameA = [Activator]::CreateInstance($gameType)
    $gameA.Id = 'profile-A'; $gameA.Name = 'Profile A'; $gameA.Root = $target; $gameA.Exe = ''
    $gameB = [Activator]::CreateInstance($gameType)
    $gameB.Id = 'profile-B'; $gameB.Name = 'Profile B'; $gameB.Exe = ''
    $gameB.Root = Join-Path ([string]$arguments[1]) 'profile-target-B'
    [void][IO.Directory]::CreateDirectory($gameB.Root)
    [IO.File]::WriteAllText((Join-Path $gameB.Root 'OptiScaler.ini'), $sourceText)
    Call 'SelectGame' @($gameA) | Out-Null
    Check ((Field '_iniPluginToggle').IsChecked -eq $true) 'Initial selection ignored enabled plugins in the current game INI'
    Check (Call 'SaveIniProfile' @('Controller fixture')) 'Controller refused to save an isolated current INI'
    $controllerProfiles = Field '_iniProfiles'
    $controllerName = [string](Field '_iniSelectedProfile')
    Check (-not [string]::IsNullOrEmpty($controllerName)) 'Saving did not select the saved profile'
    Check ($controllerProfiles.GetSelection($gameA.Id) -eq $controllerName) 'Save did not persist the current game reference'
    Check (Call 'ImportIniProfile' @($source, 'Imported controller fixture')) 'Controller refused to import a local INI'
    $controllerImported = [string](Field '_iniSelectedProfile')
    Check ($controllerImported -ne $controllerName) 'Import retained the previous selected profile'
    $pendingIni = File-Bytes $targetIni
    Choose-IniProfile $controllerName
    (Field '_iniPluginToggle').IsChecked = $false
    Check ((File-Bytes $targetIni) -ceq $pendingIni) 'Choosing a profile or toggling plugins wrote the game INI before Apply'
    $profileMetadata = [IO.File]::ReadAllText($controllerProfiles.MetadataPath)
    Call 'ToggleLanguage' | Out-Null
    Call 'SetTheme' @('dark') | Out-Null
    Check ((Field '_iniSelectedProfile') -eq $controllerName -and (Field '_iniPluginToggle').IsChecked -eq $false) 'Language/theme refresh lost pending profile choices'
    Check ([IO.File]::ReadAllText($controllerProfiles.MetadataPath) -ceq $profileMetadata) 'Language/theme refresh rewrote profile metadata'
    $aMetadata = [IO.File]::ReadAllText($controllerProfiles.MetadataPath)
    Call 'SelectGame' @($gameB) | Out-Null
    Check ([string]::IsNullOrEmpty([string](Field '_iniSelectedProfile'))) 'Profile selection leaked to another game'
    Check ((Field '_iniPluginToggle').IsChecked -eq $true) 'Plugin preference leaked to another game'
    Check ([IO.File]::ReadAllText($controllerProfiles.MetadataPath) -ceq $aMetadata) 'Selecting another game rewrote profile metadata'
    Choose-IniProfile $controllerImported
    Call 'SelectGame' @($gameA) | Out-Null
    Check ((Field '_iniSelectedProfile') -eq $controllerName -and (Field '_iniPluginToggle').IsChecked -eq $false) 'Returning to a game lost its profile/plugin selection'
    $before = File-Bytes $targetIni
    $backupsBefore = @([IO.Directory]::GetFiles($target, 'OptiScaler.ini.bak-*'))
    Check (Call 'ApplyIniProfile') 'Controller refused to apply an isolated profile'
    $newBackups = @([IO.Directory]::GetFiles($target, 'OptiScaler.ini.bak-*') | Where-Object { $_ -notin $backupsBefore })
    Check ($newBackups.Count -eq 1 -and (File-Bytes $newBackups[0]) -ceq $before) 'Controller apply did not preserve the previous INI'
    Check ([IO.File]::ReadAllText($targetIni) -match '(?im)^LoadAsiPlugins\s*=\s*false\s*$') 'Controller apply ignored the selected plugin state'
    Check (-not (Field '_writeBusy') -and $null -eq (Field '_targetMutex')) 'Profile apply retained its busy state or target lock'
    $metadataBefore = [IO.File]::ReadAllText($controllerProfiles.MetadataPath)
    $libraryBefore = @($controllerProfiles.List()) -join '|'
    $before = File-Bytes $targetIni
    Call 'SetBusy' @($true, 'install') | Out-Null
    try {
        Check (-not (Call 'SaveIniProfile' @('Refused busy save'))) 'Busy controller saved a profile'
        Check (-not (Call 'ImportIniProfile' @($source, 'Refused busy import'))) 'Busy controller imported a profile'
        Check (-not (Call 'ApplyIniProfile')) 'Busy controller applied a profile'
        Check (-not (Call 'RenameIniProfile' @('Refused busy rename'))) 'Busy controller renamed a profile'
        Check (-not (Call 'DeleteIniProfile')) 'Busy controller deleted a profile'
        Call 'SelectGame' @($gameB) | Out-Null
        Check ((Field '_selectedGame').Id -eq $gameA.Id -and (Field '_iniSelectedProfile') -eq $controllerName) 'Busy profile target or selection changed'
        Check ((File-Bytes $targetIni) -ceq $before) 'Busy refusal modified the game INI'
        Check ([IO.File]::ReadAllText($controllerProfiles.MetadataPath) -ceq $metadataBefore) 'Busy refusal modified profile references'
        Check ((@($controllerProfiles.List()) -join '|') -ceq $libraryBefore) 'Busy refusal modified the shared library'
    } finally { Call 'SetBusy' @($false, '') | Out-Null }
    Check (Call 'RenameIniProfile' @('Controller renamed')) 'Controller refused profile rename after busy completion'
    Check ((Field '_iniSelectedProfile') -eq 'Controller renamed') 'Controller rename retained the obsolete selection'
    Check (Call 'DeleteIniProfile') 'Controller refused profile deletion after busy completion'
    Check ([string]::IsNullOrEmpty([string](Field '_iniSelectedProfile'))) 'Controller delete retained the removed selection'
    Call 'SelectGame' @($gameB) | Out-Null
    Check ((Field '_iniSelectedProfile') -eq $controllerImported) 'Deleting another profile lost the second game reference'
    Call 'SelectGame' @($null) | Out-Null
    Check ([string]::IsNullOrEmpty([string](Field '_iniSelectedProfile'))) 'Clearing the game retained a profile selection'
    $controllerProfiles = $profileConstructor.Invoke(@([string]$arguments[1]))
    Check ([string]::IsNullOrEmpty($controllerProfiles.GetSelection($gameA.Id))) 'Controller deletion did not persist removal of the game reference'
    Check ($controllerProfiles.GetSelection($gameB.Id) -eq $controllerImported) 'Controller operations did not persist the other game reference'
    Check (-not $controllerProfiles.GetPlugins($gameA.Id)) 'Controller operations did not persist the disabled plugin preference'
    $reopened = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
    try {
        $persistedProfiles = [IO.File]::ReadAllText($controllerProfiles.MetadataPath)
        $controllerType.GetMethod('SelectGame', $flags).Invoke($reopened.Tag, @($gameA)) | Out-Null
        Check ([string]::IsNullOrEmpty([string]$controllerType.GetField('_iniSelectedProfile', $flags).GetValue($reopened.Tag))) 'Restart restored a deleted profile reference'
        Check ($controllerType.GetField('_iniPluginToggle', $flags).GetValue($reopened.Tag).IsChecked -eq $false) 'Restart lost the disabled plugin choice'
        $controllerType.GetMethod('SelectGame', $flags).Invoke($reopened.Tag, @($gameB)) | Out-Null
        Check ($controllerType.GetField('_iniSelectedProfile', $flags).GetValue($reopened.Tag) -eq $controllerImported) 'Restart lost the other game profile selection'
        Check ([IO.File]::ReadAllText($controllerProfiles.MetadataPath) -ceq $persistedProfiles) 'Restart/selection rewrote profile metadata'
    } finally { $reopened.Close() }

    # Compare every installed game to the selected package, without changing selection.
    $comparison = Join-Path ([string]$arguments[1]) 'comparison'
    $packageA = Join-Path $comparison 'package-A'
    $packageB = Join-Path $comparison 'package-B'
    $installedDir = Join-Path $comparison 'game'
    foreach ($directory in @($packageA, $packageB, $installedDir)) { [void][IO.Directory]::CreateDirectory($directory) }
    [IO.File]::WriteAllBytes((Join-Path $packageA 'OptiScaler.dll'), [byte[]]@(1,2,3))
    [IO.File]::WriteAllBytes((Join-Path $packageB 'OptiScaler.dll'), [byte[]]@(4,5,6))
    [IO.File]::WriteAllBytes((Join-Path $installedDir 'dxgi.dll'), [byte[]]@(4,5,6))
    [IO.File]::WriteAllText((Join-Path $installedDir 'amd-presr-install.txt'), "project=OptScaler(NR)`r`nproxy=dxgi.dll`r`n")
    $installed = [Activator]::CreateInstance($gameType)
    $installed.Id = 'comparison'; $installed.Name = 'Comparison'; $installed.Installed = $true
    $installed.Exe = Join-Path $installedDir 'game.exe'
    [IO.File]::WriteAllBytes($installed.Exe, [byte[]]@(0))
    $allGames = $controllerType.GetField('_allGames', $flags).GetValue($controller)
    $allGames.Add($installed)
    $selectionBefore = Field '_selectedGame'
    Call 'RefreshPackageComparisons' @($packageA) | Out-Null
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    Check (-not (Field '_comparisonWorkerRunning')) 'Package comparison did not complete'
    Check (Call 'PackageDiffers' @($installed)) 'Different package was not reported for an unselected installed game'
    Check ([object]::ReferenceEquals((Field '_selectedGame'), $selectionBefore)) 'Comparison changed the selected game'
    Call 'RefreshPackageComparisons' @($packageB) | Out-Null
    Check (-not (Call 'PackageDiffers' @($installed))) 'Changing packages retained stale difference evidence'
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    Check (-not (Call 'PackageDiffers' @($installed))) 'Identical selected package was reported as different'
    # One primary action follows async comparison; status remains noninteractive.
    $controllerType.GetField('_selectedGame', $flags).SetValue($controller, $installed)
    Call 'SetLanguage' @('en') | Out-Null
    Call 'SelectProxy' @('dxgi.dll') | Out-Null
    Call 'RememberProxyChoice' | Out-Null
    Call 'UpdatePrimaryButtonState' | Out-Null
    $primary = $window.FindName('PrimaryButton')
    $equalCaption = [string]$primary.Content
    Check ($equalCaption -match '(?i)reinstall|changes') 'Equal package must offer reinstallation/application of changes'
    Check ($window.FindName('HeroKicker') -is [Windows.Controls.TextBlock] -and -not $window.FindName('HeroKicker').Focusable) 'Package status must be noninteractive'
    $backendBefore = Field '_activeBackend'
    $proxyBefore = $controllerType.GetProperty('CurrentSelectedProxy', $flags).GetValue($controller, $null)
    $replaceBefore = $window.FindName('ReplaceIni').IsChecked
    Call 'RefreshPackageComparisons' @($packageA) | Out-Null
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    Check ([string]$primary.Content -match '(?i)update' -and [string]$primary.Content -cne $equalCaption) 'Async differing hash did not update primary caption'
    Check ([string]$primary.ToolTip -match '(?i)hash|package' -and [string]$primary.ToolTip -match '(?i)not.*older') 'Update tooltip must explain the comparison without implying an older installed version'
    Check ((Field '_activeBackend') -eq $backendBefore -and $window.FindName('ReplaceIni').IsChecked -eq $replaceBefore) 'Action relabel changed backend or INI decision'
    Call 'SetLanguage' @('zh') | Out-Null
    Check ($controllerType.GetProperty('CurrentSelectedProxy', $flags).GetValue($controller, $null) -eq $proxyBefore) 'Action relabel changed the selected proxy'
    Check ([string]$primary.Content -match '[\u3400-\u9fff]' -and [string]$primary.Content -notmatch '(?i)update') 'Installed-different primary was not localized'
    Call 'SetLanguage' @('en') | Out-Null
    $installed.Installed = $false
    Call 'UpdatePrimaryButtonState' | Out-Null
    Check ([string]$primary.Content -match '(?i)^install\b') 'Uninstalled game must offer installation'
    $installed.Installed = $true
    Call 'SetBusy' @($true, 'update') | Out-Null
    Call 'UpdatePrimaryButtonState' | Out-Null
    Check (-not $primary.IsEnabled) 'Unified primary enabled during update'
    Call 'SetBusy' @($false, '') | Out-Null
    Check ($window.FindName('IniApplyButton') -is [Windows.Controls.Button] -and -not [object]::ReferenceEquals($primary, $window.FindName('IniApplyButton'))) 'Profile-only Apply must remain separate'
    # Inspection A cannot replace a comparison already requested for newly selected B.
    $controllerType.GetField('_packageRoot', $flags).SetValue($controller, $packageB)
    Call 'RefreshPackageComparisons' @($packageB, $true) | Out-Null
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    $oldInspection = [Activator]::CreateInstance($resultType)
    $oldInspection.Status = 'success'; $oldInspection.PackageRoot = $packageA
    $beforeInspection = Field '_lastInspectResult'
    Call 'ApplyInspect' @($oldInspection, $packageA) | Out-Null
    Check ([object]::ReferenceEquals((Field '_lastInspectResult'), $beforeInspection)) 'Old package inspection replaced current capabilities'
    Check ((Field '_comparisonPackageRoot') -eq $packageB -and -not (Call 'PackageDiffers' @($installed))) 'Old package inspection superseded selected package comparison'
    # Preserve timestamps/length to prove explicit freshness is not a weak path/stat cache.
    $packageDll = Join-Path $packageB 'OptiScaler.dll'
    $packageStamp = [IO.File]::GetLastWriteTimeUtc($packageDll)
    [IO.File]::WriteAllBytes($packageDll, [byte[]]@(7,8,9))
    [IO.File]::SetLastWriteTimeUtc($packageDll, $packageStamp)
    $currentInspection = [Activator]::CreateInstance($resultType)
    $currentInspection.Status = 'success'; $currentInspection.PackageRoot = $packageB
    Call 'ApplyInspect' @($currentInspection, $packageB) | Out-Null
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    Check (Call 'PackageDiffers' @($installed)) 'Reinspection did not detect an in-place package replacement'
    $proxyDll = Join-Path $installedDir 'dxgi.dll'
    $proxyStamp = [IO.File]::GetLastWriteTimeUtc($proxyDll)
    [IO.File]::WriteAllBytes($proxyDll, [byte[]]@(7,8,9))
    [IO.File]::SetLastWriteTimeUtc($proxyDll, $proxyStamp)
    Call 'ApplyInspect' @($currentInspection, $packageB) | Out-Null
    for ($i = 0; $i -lt 100 -and (Field '_comparisonWorkerRunning'); $i++) {
        Start-Sleep -Milliseconds 10
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
    }
    Check (-not (Call 'PackageDiffers' @($installed))) 'Reinspection retained a stale installed proxy hash'
    $controllerType.GetField('_packageRoot', $flags).SetValue($controller, (Split-Path -Parent $assembly.Location))
    # Stable IDs, search composition, separator and identity survive language/filter refresh.
    $installed.Source = 'GOG'
    $manual = [Activator]::CreateInstance($gameType)
    $manual.Id = 'filter-manual'; $manual.Name = 'Filter manual'; $manual.Source = 'Manual'; $manual.Installed = $false
    $allGames.Add($manual)
    Call 'RefreshGameListView' | Out-Null
    $filter = $window.FindName('FilterCombo')
    Check (@($filter.Items | Where-Object { $_ -is [Windows.Controls.Separator] -and -not $_.IsEnabled -and -not $_.Focusable }).Count -eq 1) 'Filter separator is selectable'
    foreach ($id in @('all','installed','uninstalled','source:steam','source:xbox','source:epic','source:manual','source:gog')) {
        $choice = @($filter.Items | Where-Object { $_ -is [Windows.Controls.ComboBoxItem] -and $_.Tag -eq $id })
        Check ($choice.Count -eq 1) "Missing stable filter $id"
        $filter.SelectedItem = $choice[0]
        foreach ($item in $window.FindName('GameList').Items) {
            if ($id -eq 'installed') { Check $item.Info.Installed 'Installed filter admitted uninstalled game' }
            if ($id -eq 'uninstalled') { Check (-not $item.Info.Installed) 'Uninstalled filter admitted installed game' }
            if ($id.StartsWith('source:')) { Check ($item.Info.Source -eq $id.Substring(7)) 'Platform filter admitted wrong source' }
        }
    }
    $filter.SelectedItem = @($filter.Items | Where-Object { $_.Tag -eq 'source:gog' })[0]
    Call 'ToggleLanguage' | Out-Null
    Check ($filter.SelectedItem.Tag -eq 'source:gog') 'Language switch reset platform filter'
    $identity = Field '_selectedGame'
    $window.FindName('SearchBox').Text = 'no-result-for-this-query'
    Check ($window.FindName('GameList').Items.Count -eq 0 -and $window.FindName('CountBadge').Text -eq '0') 'Search/filter count is not visible result count'
    Check ([object]::ReferenceEquals((Field '_selectedGame'), $identity)) 'Empty filter changed target identity'
    $window.FindName('SearchBox').Text = ''
    Check ($window.FindName('GameList').Items.Count -eq 1) 'Search clear lost the active platform filter'
    $ranker = $controllerType.GetMethod('PhysicalAdapterRank', [Reflection.BindingFlags]'NonPublic,Static')
    foreach ($virtual in @('GameViewer Virtual Display', 'UU Remote Display', 'Microsoft Remote Display Adapter')) {
        Check ($ranker.Invoke($null, @([string]$virtual, [string]'ROOT\DISPLAY')) -eq -1) 'Virtual adapter was accepted as physical hardware'
    }
    # More than the old 256-image queue bound, with repeated stale views and a newly selected hero.
    $artDir = Join-Path ([string]$arguments[1]) 'artwork-saturation'
    [void][IO.Directory]::CreateDirectory($artDir)
    $artPath = Join-Path $artDir 'landscape.png'
    $visual = New-Object Windows.Media.DrawingVisual
    $drawing = $visual.RenderOpen()
    $drawing.DrawRectangle([Windows.Media.Brushes]::SteelBlue, $null, (New-Object Windows.Rect 0, 0, 640, 240))
    $drawing.Close()
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap 640, 240, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = [IO.File]::Create($artPath)
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
    $allGames.Clear()
    for ($i = 0; $i -lt 600; $i++) {
        $artGame = [Activator]::CreateInstance($gameType)
        $artGame.Id = 'art-' + $i; $artGame.Name = 'Artwork ' + $i; $artGame.Root = $artDir
        $artGame.Source = 'Manual'; $artGame.Cover = $artPath; $artGame.Hero = $artPath
        $allGames.Add($artGame)
    }
    $filter.SelectedItem = @($filter.Items | Where-Object { $_.Tag -eq 'all' })[0]
    for ($i = 0; $i -lt 6; $i++) { Call 'RefreshGameListView' | Out-Null }
    Call 'SelectGame' @($allGames[599]) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
        Start-Sleep -Milliseconds 5
        $missingArt = @($window.FindName('GameList').Items | Where-Object { $null -eq $_.DisplayImage }).Count
    } while (($missingArt -gt 0 -or $null -eq $window.FindName('HeroBgImage').Source) -and [DateTime]::UtcNow -lt $deadline)
    Check ($missingArt -eq 0) 'Current library artwork was dropped after saturation/filter refresh'
    Check ($null -ne $window.FindName('HeroBgImage').Source) 'Selected hero starved behind obsolete library artwork'
    Write-Host 'launcher state: PASS'
} finally {
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'ReleaseLock' | Out-Null
    $window.Close()
}
