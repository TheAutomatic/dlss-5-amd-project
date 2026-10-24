# Exercise the actual WPF controller with isolated preferences; no windows or workers are launched.
[CmdletBinding()]
param([string]$LauncherExe)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo 'exports/tests/launcher-localization'
if ($LauncherExe) { $output = Split-Path -Parent $LauncherExe } else { & (Join-Path $repo 'tools/build/build-launcher.ps1') -OutDir $output }
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml
$assembly = [Reflection.Assembly]::LoadFrom((Join-Path $output 'OptScalerNR.Launcher.exe'))
$controllerType = $assembly.GetType('OptScalerNR.Launcher.MainController', $true)
$flags = [Reflection.BindingFlags]'NonPublic,Instance'
$create = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static')
$fixture = Join-Path $output ('preferences-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
$originalCulture = [Threading.Thread]::CurrentThread.CurrentUICulture
$script:checks = 0

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
    $script:checks++
}
function New-TestWindow([string]$culture, [string]$directory) {
    [Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo($culture)
    $arguments = New-Object object[] 2
    $arguments[0] = [string[]]@()
    $arguments[1] = $directory
    return $create.Invoke($null, $arguments)
}
function Invoke-Controller($window, [string]$method, [object[]]$arguments = @()) {
    $target = $controllerType.GetMethod($method, $flags)
    if ($method -eq 'ApplyInspect' -and $arguments.Length -eq 1) { $arguments += $controllerType.GetField('_packageRoot', $flags).GetValue($window.Tag) }
    while ($arguments.Length -lt $target.GetParameters().Length) { $arguments += [Type]::Missing }
    return $target.Invoke($window.Tag, $arguments)
}
function Get-UiStrings($element) {
    if ($element -is [Windows.Window]) { $element.Title }
    if ($element -is [Windows.Controls.TextBlock]) { $element.Text }
    if ($element -is [Windows.Controls.HeaderedItemsControl] -and $element.Header -is [string]) { $element.Header }
    if ($element -is [Windows.Controls.ContentControl] -and $element.Content -is [string]) { $element.Content }
    if ($element -is [Windows.DependencyObject]) {
        $toolTip = [Windows.Controls.ToolTipService]::GetToolTip($element)
        if ($toolTip -is [string]) { $toolTip }
    }
    if ($element -is [Windows.FrameworkElement] -and $null -ne $element.ContextMenu) { Get-UiStrings $element.ContextMenu }
    if ($element -is [Windows.DependencyObject]) {
        foreach ($child in [Windows.LogicalTreeHelper]::GetChildren($element)) {
            if ($child -is [Windows.DependencyObject]) { Get-UiStrings $child }
        }
    }
}
function Dialog-Radios($node) {
    if ($node -is [Windows.Controls.RadioButton]) { $node }
    if ($node -is [Windows.DependencyObject]) {
        foreach ($child in [Windows.LogicalTreeHelper]::GetChildren($node)) { Dialog-Radios $child }
    }
}
function Assert-English($window, [string]$context) {
    $han = @(Get-UiStrings $window | Where-Object { $_ -match '[\u3400-\u9fff]' })
    Assert-True ($han.Count -eq 0) ("Chinese text in English $context : " + ($han -join ' | '))
}
function Assert-NewControls($window) {
    $controls = @{
        ThemeToggle = [Windows.Controls.Button]
        ThemeHeading = [Windows.Controls.TextBlock]
        ThemeCombo = [Windows.Controls.ComboBox]
        LangZh = [Windows.Controls.Button]
        LangEn = [Windows.Controls.Button]
        HardwareInfo = [Windows.Controls.TextBlock]
        QuickLogButton = [Windows.Controls.Button]
    }
    foreach ($name in $controls.Keys) {
        $control = $window.FindName($name)
        Assert-True ($null -ne $control -and $controls[$name].IsInstanceOfType($control)) "Missing or incorrect $name control"
    }
    foreach ($name in @('ThemeToggle', 'ThemeHeading', 'LangZh', 'LangEn', 'HardwareInfo', 'QuickLogButton')) {
        $text = @(Get-UiStrings $window.FindName($name)) -join ' '
        Assert-True (-not [string]::IsNullOrWhiteSpace($text)) "Empty localized $name control"
    }
    foreach ($name in @('ThemeToggle', 'LangZh', 'LangEn', 'HardwareInfo', 'RefreshBackendsButton', 'PrimaryButton')) {
        Assert-True (-not [string]::IsNullOrWhiteSpace([string][Windows.Controls.ToolTipService]::GetToolTip($window.FindName($name)))) "Missing localized $name tooltip"
    }
    $combo = $window.FindName('ThemeCombo')
    Assert-True ($combo.Items.Count -eq 3) 'Theme selector must expose auto, light and dark'
    foreach ($theme in @('auto', 'light', 'dark')) {
        $items = @($combo.Items | Where-Object { $_.Tag -eq $theme })
        Assert-True ($items.Count -eq 1) "Theme selector is missing $theme"
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$items[0].Content)) "Theme selector has an empty $theme label"
    }
}
function Save-Preview($window, [string]$name, [bool]$settings = $false) {
    if ($settings) { Invoke-Controller $window 'SwitchTab' @($true) }
    $size = New-Object Windows.Size 1240, 900
    $window.Content.Measure($size)
    $window.Content.Arrange((New-Object Windows.Rect $size))
    $window.Content.UpdateLayout()
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap 1240, 900, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($window.Content)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = [IO.File]::Create((Join-Path $output $name))
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
}
function Save-BackendPreview($window, [string]$name) {
    $section = $window.FindName('BackendSection')
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap ([int][Math]::Ceiling($section.ActualWidth)), ([int][Math]::Ceiling($section.ActualHeight)), 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $visual = New-Object Windows.Media.DrawingVisual
    $drawing = $visual.RenderOpen()
    $drawing.DrawRectangle((New-Object Windows.Media.VisualBrush $section), $null, (New-Object Windows.Rect 0, 0, $section.ActualWidth, $section.ActualHeight))
    $drawing.Close()
    $bitmap.Render($visual)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = [IO.File]::Create((Join-Path $output $name))
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
}

try {
    # Includes simplified, traditional, neutral and legacy Windows Chinese cultures.
    foreach ($culture in @('zh-CN','zh-SG','zh-TW','zh-HK','zh-MO','zh-Hans','zh-Hant','zh-CHS','zh-CHT','zh')) {
        $dir = Join-Path $fixture $culture
        $window = New-TestWindow $culture $dir
        try {
            $saved = Get-Content -LiteralPath (Join-Path $dir 'settings.json') -Raw | ConvertFrom-Json
            Assert-NewControls $window
            foreach ($name in @('ThemeHeading', 'QuickLogButton')) {
                Assert-True ((@(Get-UiStrings $window.FindName($name)) -join ' ') -match '[\u3400-\u9fff]') "Chinese $name label missing for $culture"
            }
            Assert-True ($saved.Language -eq 'zh') "Expected Chinese for $culture"
            Assert-True ($window.FindName('LocalGamesHeading').Text -match '[\u3400-\u9fff]') "Chinese library heading missing for $culture"
            Assert-True (([string]$window.FindName('ManageGameButton').Content) -match '[\u3400-\u9fff]') "Chinese Manage action missing for $culture"
        } finally { $window.Close() }
    }
    foreach ($culture in @('en-US','ja-JP','fr-FR','de-DE','ar-SA')) {
        $dir = Join-Path $fixture $culture
        $window = New-TestWindow $culture $dir
        try {
            $saved = Get-Content -LiteralPath (Join-Path $dir 'settings.json') -Raw | ConvertFrom-Json
            Assert-True ($saved.Language -eq 'en') "Expected English for $culture"
            Assert-NewControls $window
            Assert-True (-not [string]::IsNullOrWhiteSpace([string]$window.FindName('ManageGameButton').Content)) "English Manage action missing for $culture"
            Assert-English $window "first launch ($culture)"
            Invoke-Controller $window 'SwitchTab' @($true)
            Assert-True ($window.FindName('SettingsPage').Visibility -eq 'Visible') 'Settings drawer did not open'
            Assert-True ($window.FindName('LibraryPage').Visibility -eq 'Visible') 'Opening settings hid the game library'
            Assert-True ($window.FindName('SettingsTab').Content -eq 'Settings') 'Opening settings changed the settings action'
            $englishTooltip = [string]$window.FindName('PrimaryButton').ToolTip
            Invoke-Controller $window 'ToggleLanguage'
            Assert-True ($window.FindName('SettingsTab').Content -eq '设置') 'Settings action was not translated with its drawer open'
            $chineseTooltip = [string]$window.FindName('PrimaryButton').ToolTip
            Assert-True ($chineseTooltip -cne $englishTooltip -and $chineseTooltip -match '[\u3400-\u9fff]') 'Primary action tooltip did not switch to Chinese'
            Invoke-Controller $window 'ToggleLanguage'
            Assert-English $window 'settings after two toggles'
            Assert-True ([string]$window.FindName('PrimaryButton').ToolTip -ceq $englishTooltip) 'Primary action tooltip did not return to English'
            $window.FindName('SettingsCloseButton').RaiseEvent((New-Object Windows.RoutedEventArgs ([Windows.Controls.Button]::ClickEvent)))
            Assert-True ($window.FindName('SettingsPage').Visibility -eq 'Collapsed') 'Settings close button did not close the drawer'
            Assert-True ($window.FindName('LibraryPage').Visibility -eq 'Visible') 'Closing settings hid the game library'
            $window.FindName('LangZh').RaiseEvent((New-Object Windows.RoutedEventArgs ([Windows.Controls.Button]::ClickEvent)))
            Assert-True ($window.FindName('LangCombo').SelectedItem.Tag -eq 'zh') 'Chinese segmented button did not synchronize LangCombo'
            $window.FindName('LangEn').RaiseEvent((New-Object Windows.RoutedEventArgs ([Windows.Controls.Button]::ClickEvent)))
            Assert-True ($window.FindName('LangCombo').SelectedItem.Tag -eq 'en') 'English segmented button did not synchronize LangCombo'
            Assert-English $window 'settings after segmented language selection'
        } finally { $window.Close() }
    }

    # Loading preferences must not save partial/default scan/package settings through change handlers.
    $dir = Join-Path $fixture 'existing'
    [void][IO.Directory]::CreateDirectory($dir)
    $settings = Join-Path $dir 'settings.json'
    $json = @{ Language = 'en'; ScanOnStartup = $false; PackageRoot = 'D:\My Package'; RecentGameId = 'remember-me' } | ConvertTo-Json
    [IO.File]::WriteAllText($settings, $json)
    $window = New-TestWindow 'zh-TW' $dir
    try {
        Assert-English $window 'saved English on a Chinese system'
        Assert-True ([IO.File]::ReadAllText($settings) -eq $json) 'Loading existing settings rewrote them'
        Assert-True ($null -eq (([IO.File]::ReadAllText($settings) | ConvertFrom-Json).PSObject.Properties['Theme'])) 'Loading old settings added a Theme preference'
        # Exercise the settings selector, not just the top-bar toggle.
        $window.FindName('LangCombo').SelectedIndex = 0
        $saved = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json
        Assert-True ($saved.Language -eq 'zh') 'Manual language was not saved'
        Assert-True (-not $saved.ScanOnStartup -and $saved.PackageRoot -eq 'D:\My Package' -and $saved.RecentGameId -eq 'remember-me') 'Language change clobbered unrelated settings'
    } finally { $window.Close() }
    $window = New-TestWindow 'en-US' $dir
    try {
        Assert-True ($window.FindName('SettingsHeading').Text -eq '设置') 'Restart ignored manually selected Chinese'
        Invoke-Controller $window 'ToggleLanguage'
    } finally { $window.Close() }
    $window = New-TestWindow 'zh-CN' $dir
    try {
        Assert-English $window 'restart after manually selecting English'

        # A locale refresh must not reapply inspection and reset proxy/backend/INI choices.
        $resultType = $assembly.GetType('OptScalerNR.Launcher.LauncherResult')
        $backendType = $assembly.GetType('OptScalerNR.Launcher.BackendInfo')
        $result = [Activator]::CreateInstance($resultType)
        $result.Status = 'success'
        $result.Recommend = 'dxgi.dll'
        $result.Backends = [Array]::CreateInstance($backendType, 3)
        $ids = @('lmxxf','daniel','mochizuki')
        for ($i = 0; $i -lt 3; $i++) {
            $backend = [Activator]::CreateInstance($backendType)
            $backend.Id = $ids[$i]
            $backend.Enabled = $true
            $backend.ReasonCode = 'ready'
            $result.Backends[$i] = $backend
        }
        $mochiType = $assembly.GetType('OptScalerNR.Launcher.MochizukiInstallInfo')
        $mochiInfo = [Activator]::CreateInstance($mochiType)
        $mochiInfo.Detected = $true
        $mochiInfo.Files = [string[]]@('dlssnr_core.dll', 'dlssnr-amd-install.txt')
        $result.MochizukiInstall = $mochiInfo
        Invoke-Controller $window 'ApplyInspect' @($result)
        Invoke-Controller $window 'SelectProxy' @('winmm.dll')
        $window.FindName('DanielCheck').IsChecked = $false
        $window.FindName('ReplaceIni').IsChecked = $false
        Invoke-Controller $window 'SetActiveBackend' @('mochizuki')
        Invoke-Controller $window 'ToggleLanguage'
        Assert-True ($window.FindName('LmxxfReason').Text -eq '就绪') 'Inspected badges did not switch to Chinese'
        Assert-True ($window.FindName('DanielReason').Text -eq '未选择' -and $window.FindName('DanielHint').Visibility -eq 'Collapsed') 'Unchecked backend did not stay neutral in Chinese'
        Invoke-Controller $window 'ToggleLanguage'
        Assert-English $window 'after inspection and toggling'
        Assert-True ($window.FindName('PillWinmm').IsChecked -eq $true) 'Locale change reset proxy choice'
        Assert-True ($window.FindName('DanielCheck').IsChecked -eq $false) 'Locale change reset backend choice'
        Assert-True ($window.FindName('DanielReason').Text -eq 'Not selected' -and $window.FindName('DanielHint').Text -eq '') 'Unchecked backend did not retranslate neutrally'
        Assert-True ($window.FindName('ReplaceIni').IsChecked -eq $false) 'Locale change reset INI choice'
        Assert-True ($controllerType.GetField('_activeBackend', $flags).GetValue($window.Tag) -eq 'mochizuki') 'Locale change reset active backend'
        foreach ($theme in @('light', 'dark', 'auto')) {
            Invoke-Controller $window 'SetTheme' @($theme)
            Assert-English $window "after switching to $theme theme"
            Assert-True ($window.FindName('PillWinmm').IsChecked -eq $true) 'Theme change reset proxy choice'
            Assert-True ($window.FindName('DanielCheck').IsChecked -eq $false) 'Theme change reset backend choice'
            Assert-True ($window.FindName('ReplaceIni').IsChecked -eq $false) 'Theme change reset INI choice'
            Assert-True ($controllerType.GetField('_activeBackend', $flags).GetValue($window.Tag) -eq 'mochizuki') 'Theme change reset active backend'
        }
        # The Mochizuki coexistence note must survive every refresh of the recommendation text.
        $recText = [string]($window.FindName('InjectRecommendation').Text)
        Assert-True ($recText -match 'Mochizuki') 'Mochizuki coexistence note lost after inspection refresh'
        Save-Preview $window 'english-library.png'
        Save-Preview $window 'english-settings.png' $true
        $result.Backends[2].PreparationState = 'ready'
        $result.Backends[2].OriginalState = 'preparable'
        $result.Backends[2].SmallState = 'preparable'
        foreach ($language in @('zh','en')) {
            Invoke-Controller $window 'SetLanguage' @($language)
            $dialog = Invoke-Controller $window 'CreateMochizukiModelDialog'
            try {
                if ($language -eq 'en') { Assert-English $dialog 'model dialog' }
                else {
                    Assert-True ($dialog.Title -eq '选择 Mochizuki 模型') 'Chinese model dialog missing'
                    Save-Preview $window 'chinese-settings.png' $true
                    Invoke-Controller $window 'SwitchTab' @($false)
                    Save-Preview $window 'chinese-library.png'
                }
                $radios = @(Dialog-Radios $dialog)
                Assert-True (@($radios | Where-Object { $_.IsChecked }).Count -eq 0) 'Model dialog silently selected an option'
                Assert-True ($radios.Count -eq 3) 'Model dialog omitted a supported model choice'
                Assert-True (@($radios | Where-Object { -not $_.IsEnabled }).Count -eq 0) 'Extraction-ready model choices were disabled'
                $radios[1].IsChecked = $true
                Assert-True ($dialog.Tag -eq 'small') 'Localized model label changed the protocol value'
            } finally { $dialog.Close() }
        }
        # Without the extractor, only already installed models may be selected.
        $result.Backends[2].PreparationState = 'missing'
        $result.Backends[2].OriginalState = 'ready'
        $result.Backends[2].SmallState = 'missing'
        $dialog = Invoke-Controller $window 'CreateMochizukiModelDialog'
        try {
            Assert-English $dialog 'model dialog without extraction helper'
            $radios = @(Dialog-Radios $dialog)
            Assert-True (@($radios | Where-Object { $_.IsChecked }).Count -eq 0) 'Unavailable extractor silently selected a model'
            Assert-True (-not $radios[0].IsEnabled -and -not $radios[1].IsEnabled) 'Missing new model was selectable without extraction support'
            Assert-True $radios[2].IsEnabled 'Installed original model was disabled without extraction support'
            $radios[2].IsChecked = $true
            Assert-True ($dialog.Tag -eq 'original') 'Installed original model produced the wrong protocol choice'
        } finally { $dialog.Close() }
        # Missing inputs are localized without reapplying detection or losing user choices.
        $result.PackageRoot = Join-Path $fixture 'package with spaces'
        foreach ($backend in $result.Backends) { $backend.Enabled = $false; $backend.ReasonCode = 'missing-model' }
        $result.Backends[0].ReasonCode = 'missing-runtime'
        $result.Backends[1].ReasonCode = 'missing-runtime'
        $result.Backends[0].Hints = [string[]]@('lmxxf-weights')
        $result.Backends[0].MissingPackageFiles = [string[]]@('LmxxfNrRuntime.dll', 'lmxxf-modules/')
        $result.Backends[1].Hints = [string[]]@('daniel-runtime', 'daniel-weights', 'daniel-setup', 'daniel-source')
        $result.Backends[2].Hints = [string[]]@('mochi-source', 'mochi-versions')
        Invoke-Controller $window 'ApplyInspect' @($result)
        Invoke-Controller $window 'SetLanguage' @('en')
        Assert-English $window 'missing input hints'
        foreach ($prefix in @('Lmxxf', 'Daniel', 'Mochi')) {
            Assert-True ($window.FindName($prefix + 'Reason').Text -eq 'Not selected') "$prefix unchecked missing status was not neutral"
            Assert-True ($window.FindName($prefix + 'Hint').Text -eq '' -and $window.FindName($prefix + 'Hint').Visibility -eq 'Collapsed') "$prefix unchecked missing hint was visible"
        }
        Assert-True ($window.FindName('BackendDetailsText').Text -eq '' -and $window.FindName('BackendDetails').Visibility -eq 'Collapsed') 'Unchecked missing components were exposed'
        foreach ($prefix in @('Lmxxf', 'Daniel', 'Mochi')) { $window.FindName($prefix + 'Check').IsChecked = $true }
        Assert-True ($window.FindName('DanielHint').Text.Contains('version.dll')) 'Daniel runtime filename missing'
        Assert-True ($window.FindName('MochiHint').Text.Contains('nvngx_dlssnr.dll')) 'Mochi source filename missing'
        Assert-True ($window.FindName('BackendPackagePath').Text.Contains($result.PackageRoot)) 'Actual package directory missing'
        Assert-True ($window.FindName('BackendDetailsText').Text.Contains('LmxxfNrRuntime.dll')) 'Missing package filename not displayed'
        Invoke-Controller $window 'SwitchTab' @($false)
        Save-Preview $window 'english-backend-hints.png'
        Save-BackendPreview $window 'english-backend-cards.png'
        Invoke-Controller $window 'SetLanguage' @('zh')
        Assert-True ($window.FindName('MochiHint').Text -match '[\u3400-\u9fff]') 'Chinese source hint missing'
        Assert-True ($window.FindName('DanielHint').Text -match '[\u3400-\u9fff]' -and $window.FindName('LmxxfHint').Text -match '[\u3400-\u9fff]') 'Selected missing backend hints did not retranslate'
        Assert-True ($window.FindName('DanielCheck').IsChecked -eq $true -and $window.FindName('LmxxfCheck').IsChecked -eq $true) 'Hint retranslation reset selected missing backends'
        Save-Preview $window 'chinese-backend-hints.png'
        Save-BackendPreview $window 'chinese-backend-cards.png'
        foreach ($backend in $result.Backends) { $backend.Hints = [string[]]@(); $backend.MissingPackageFiles = [string[]]@() }
        Invoke-Controller $window 'ApplyInspect' @($result)
        Assert-True ($window.FindName('MochiHint').Visibility -eq 'Collapsed') 'Ready backend retained stale hints'
        Assert-True ($window.FindName('BackendPackagePath').Visibility -eq 'Collapsed') 'Empty hint section retained the package path'

        # Progress text changes language as well; diagnostic event strings stay in the log.
        Invoke-Controller $window 'SetLanguage' @('zh')
        Invoke-Controller $window 'OnEvent' @('validate', 'fixture diagnostic')
        [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Windows.Threading.DispatcherPriority]::ApplicationIdle, [Action]{})
        Assert-True ($window.FindName('StatusText').Text -eq '正在检查安装条件…') 'Progress stage was not localized'
        Invoke-Controller $window 'SetLanguage' @('en')
        Assert-English $window 'progress after switching to English'
    } finally { $window.Close() }

    # Invalid/missing language keys use the display language and recover a valid saved choice.
    foreach ($raw in @('{"Language":"invalid","ScanOnStartup":false}', '{"ScanOnStartup":false}', '{broken')) {
        [IO.File]::WriteAllText($settings, $raw)
        $window = New-TestWindow 'zh-Hant' $dir
        try {
            $saved = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json
            Assert-True ($saved.Language -eq 'zh') 'Missing/invalid preference did not recover from UI culture'
        } finally { $window.Close() }
    }
    Write-Host "launcher localization: PASS ($script:checks checks)"
} finally {
    [Threading.Thread]::CurrentThread.CurrentUICulture = $originalCulture
}
