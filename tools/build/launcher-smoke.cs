// STA smoke test for the WPF launcher (C# 5, compiled with the .NET Framework csc in CI).
// Loads OptiScalerNRLauncher.exe, creates the main window with a throw-away settings folder and a fake
// library, then cycles light/dark x zh/en x the four install modes, the settings overlay and the
// 1020x660 minimum size. Fails (exit code 1) on any dispatcher exception or clipped key control, and
// writes PNG previews of every state into the output folder.
//
// Usage: launcher-smoke.exe <OptiScalerNRLauncher.exe> <settingsDir> <outDir>
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

static class LauncherSmoke
{
    static readonly List<string> Failures = new List<string>();
    static Window _window;
    static object _controller;
    static string _out;

    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length < 3) { Console.Error.WriteLine("usage: launcher-smoke <exe> <settingsDir> <outDir>"); return 2; }
        _out = args[2];
        Directory.CreateDirectory(_out);
        var app = new Application();
        app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        app.DispatcherUnhandledException += delegate(object s, DispatcherUnhandledExceptionEventArgs e) {
            Fail("dispatcher exception: " + e.Exception);
            e.Handled = true;
        };
        try
        {
            var asm = Assembly.LoadFrom(Path.GetFullPath(args[0]));
            var paletteType = asm.GetType("OptScalerNR.Launcher.LauncherTheme", true);
            var normalize = paletteType.GetMethod("Normalize");
            foreach (var value in new[] { null, "", "unknown" })
                Check((string)normalize.Invoke(null, new object[] { value }) == "auto", "unknown appearance falls back to auto");
            foreach (var appearance in new[] { "auto", "light", "dark", "classic", "midnight" })
                Check((string)normalize.Invoke(null, new object[] { appearance }) == appearance, "appearance accepted: " + appearance);
            foreach (var appearance in new[] { "light", "dark", "classic", "midnight" }) {
                paletteType.GetMethod("Apply").Invoke(null, new object[] { appearance });
                foreach (var pair in new[] { new[] { "ThemeInk", "ThemeCardBg" }, new[] { "ThemeMuted", "ThemeCardBg" },
                    new[] { "ThemeHeroInk", "ThemeHeroBg" }, new[] { "ThemeHeroMuted", "ThemeHeroBg" },
                    new[] { "ThemeHeroInk", "ThemeHeroHover" }, new[] { "ThemeDanger", "ThemeDangerBg" } }) {
                    var a = (Color)paletteType.GetMethod("ColorOf").Invoke(null, new object[] { pair[0] });
                    var b = (Color)paletteType.GetMethod("ColorOf").Invoke(null, new object[] { pair[1] });
                    double x = Luminance(a), y = Luminance(b);
                    Check((Math.Max(x, y) + 0.05) / (Math.Min(x, y) + 0.05) >= 4.5, appearance + " contrast: " + pair[0]);
                }
            }
            var type = asm.GetType("OptScalerNR.Launcher.MainController", true);
            CheckPreferences(type, args[1]);
            var classify = type.GetMethod("ClassifyGpu", BindingFlags.Static | BindingFlags.NonPublic);
            foreach (var sample in new[] {
                new[] { "AMD Radeon RX 9070 XT", "Rdna4" }, new[] { "AMD Radeon RX 7900 XTX", "Rdna3" },
                new[] { "AMD Radeon RX 6800", "Rdna2" }, new[] { "AMD Radeon 780M Graphics", "OtherAmd" },
                new[] { "AMD Radeon RX 580", "OtherAmd" }, new[] { "NVIDIA GeForce RTX 4090", "NonAmd" },
                new[] { "Intel Arc B580", "NonAmd" }, new[] { "Microsoft Basic Display Adapter", "Unknown" },
                new[] { "", "Unknown" }, new[] { (string)null, "Unknown" } })
                Check(classify.Invoke(null, new object[] { sample[0] }).ToString() == sample[1], "GPU family: " + sample[0]);
            var create = type.GetMethod("Create", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public, null,
                new[] { typeof(string[]), typeof(string) }, null);
            _window = (Window)create.Invoke(null, new object[] { new string[0], Path.GetFullPath(args[1]) });
            _controller = _window.Tag;
            _window.Width = 1240; _window.Height = 800;
            _window.WindowStartupLocation = WindowStartupLocation.Manual;
            _window.Left = 0; _window.Top = 0;
            _window.ShowActivated = false;
            app.MainWindow = _window;
            _window.Show();
            Pump(3500);
            WaitForInspection();

            var list = (ListBox)Find("GameList");
            if (args.Length > 3 && args[3] == "community-links") {
                Invoke("SetTheme", "light");
                Invoke("SetLanguage", "zh");
                ((Button)Find("SettingsTab")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Pump(150);
                Shot("community-links-light-zh", false);
                return FinishSmoke();
            }
            if (args.Length > 3 && args[3] == "proxy-modes") {
                CheckProxyModes(args[1]);
                return FinishSmoke();
            }
            Check(_window.FindName("ShowAdvancedOptions") == null, "Settings advanced toggle removed");
            foreach (var name in new[] { "AdvancedProxySection", "AdvancedInstallSection", "AdvancedProfilesSection", "AdvancedLogSection" })
                Check(((FrameworkElement)Find(name)).IsVisible, "section visible by default: " + name);
            Check(((StackPanel)Find("ProfilesCardBody")).Visibility == Visibility.Collapsed, "OptiScaler config default collapsed");
            Check(((Border)Find("StyleShareBar")).IsVisible && ((ComboBox)Find("AsiProfileCombo")).IsVisible, "look sharing and dlssnr profiles always visible");
            Check(((StackPanel)Find("InstallLogCardBody")).Visibility == Visibility.Collapsed, "log default collapsed");
            Check(!string.IsNullOrEmpty(((TextBlock)Find("ProfilesSummary")).Text), "collapsed profile state shown");
            Check(!string.IsNullOrEmpty(((TextBlock)Find("LogPreviewHeading")).Text), "collapsed log state shown");
            var settingsPath = Path.Combine(args[1], "settings.json");
            var settings = File.ReadAllText(settingsPath);
            foreach (var oldValue in new[] { "false", "true" }) {
                File.WriteAllText(settingsPath, settings.TrimEnd().TrimEnd('}') + ",\"ShowAdvancedOptions\":" + oldValue + "}");
                Call("LoadSettings");
                Check(((StackPanel)Find("ProfilesCardBody")).Visibility == Visibility.Collapsed && ((StackPanel)Find("AdvancedProxySection")).IsVisible,
                    "legacy advanced key ignored: " + oldValue);
            }
            Call("SaveSettings");
            Check(!File.ReadAllText(settingsPath).Contains("ShowAdvancedOptions"), "legacy advanced key no longer saved");
            Check(list.Items.Count >= 2, "fake library loaded (" + list.Items.Count + " items)");
            SelectGame(list, false);
            CheckProfileStores(args[1]);
            ((TextBox)Find("LogView")).Clear();
            Check(((TextBlock)Find("LogPreviewHeading")).Text.Contains("暂无") || ((TextBlock)Find("LogPreviewHeading")).Text == "No log entries", "empty log summarized");
            Call("AppendLog", "Smoke install result"); Pump(150);
            Check(((TextBlock)Find("LogPreviewHeading")).Text.Contains("1"), "log summary updates while collapsed");

            var backendChecked = ((CheckBox)Find("MochiCheck")).IsChecked;
            var backendReason = ((TextBlock)Find("MochiReason")).Text;
            foreach (var appearance in new[] { "classic", "midnight", "light", "dark" }) {
                Invoke("SetTheme", appearance);
                Check(((Border)Find("MochiCard")).BorderThickness == new Thickness(appearance == "classic" ? 0 : 1), "card border follows appearance: " + appearance);
                Check(((Border)Find("MochiBadge")).BorderThickness == new Thickness(0), "status is borderless: " + appearance);
                var pending = (Border)Find("PendingActionsCard");
                Check(pending.BorderThickness == new Thickness(appearance == "classic" ? 0 : 1), "pending card uses standard border: " + appearance);
                Check(((SolidColorBrush)pending.Background).Color == ((SolidColorBrush)_window.FindResource("ThemeCardBg")).Color, "pending card uses normal surface: " + appearance);
                Check(((SolidColorBrush)((TextBlock)Find("ModelImportText")).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeInk")).Color, "pending title uses normal ink: " + appearance);
                Check(((SolidColorBrush)((TextBlock)Find("OpenRrUnsupportedText")).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeMuted")).Color, "game-trait lock uses muted ink: " + appearance);
                Check(((SolidColorBrush)((Border)Find("OpenRrUnsupportedBar")).Background).Color.A == 0, "game-trait lock is unfilled: " + appearance);
                Check(((CheckBox)Find("MochiCheck")).IsChecked == backendChecked, "appearance preserves backend selection: " + appearance);
                Check(((TextBlock)Find("MochiReason")).Text == backendReason, "appearance preserves backend status: " + appearance);
            }

            foreach (var theme in new[] { "light", "dark" })
            {
                Invoke("SetTheme", theme);
                foreach (var lang in new[] { "zh", "en" })
                {
                    Invoke("SetLanguage", lang);
                    SelectGame(list, false);
                    CheckMochizukiImportCopy(lang);
                    foreach (var mode in new[] { "ModeOptiScalerRadio", "ModeStandaloneRadio", "ModeReShadeRadio" })
                    {
                        ((RadioButton)Find(mode)).IsChecked = true;
                        Pump(400);
                        AssertLayout(theme + "-" + lang + "-" + mode);
                        if (mode == "ModeStandaloneRadio") Check(((FrameworkElement)Find("StandaloneModeCard")).IsVisible, "standalone settings remain visible");
                        if (mode == "ModeReShadeRadio") Check(((FrameworkElement)Find("ReShadeModeCard")).IsVisible, "ReShade settings remain visible");
                        Check(((FrameworkElement)Find("OptiScalerInstallExtras")).IsVisible == (mode == "ModeOptiScalerRadio"), "OptiScaler extras remain mode-specific: " + mode);
                        Shot(theme + "-" + lang + "-" + mode.Replace("Mode", "").Replace("Radio", "").ToLowerInvariant());
                    }
                    ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
                    if (SelectGame(list, true))
                    {
                        Pump(600);
                        Check(((FrameworkElement)Find("RtxRemixMcModeCard")).IsVisible, theme + "-" + lang + ": RTX Remix MC card visible");
                        Check(!((FrameworkElement)Find("ModeCardsGrid")).IsVisible, theme + "-" + lang + ": mode cards hidden for RTX Remix MC");
                        AssertLayout(theme + "-" + lang + "-rtxmc");
                        Shot(theme + "-" + lang + "-rtxremixmc");
                    }
                    else Fail("no RTX Remix MC entry in the fake library");
                    CheckEnglish(lang);
                }
                // Settings overlay + Esc close.
                SelectGame(list, false);
                ((Button)Find("SettingsTab")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Pump(300);
                Check(((FrameworkElement)Find("SettingsPage")).IsVisible, theme + ": settings overlay opens");
                Shot(theme + "-en-settings");
                ((Button)Find("SettingsCloseButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Pump(200);
                Check(!((FrameworkElement)Find("SettingsPage")).IsVisible, theme + ": settings overlay closes");
            }

            foreach (var theme in new[] { "classic", "midnight" }) {
                Invoke("SetTheme", theme);
                Invoke("SetLanguage", "zh");
                SelectGame(list, false);
                ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
                Pump(400);
                var reasonBeforeRefresh = ((TextBlock)Find("MochiReason")).Text;
                _controller.GetType().GetMethod("RefreshInspectionText", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(_controller, null);
                Console.WriteLine("BACKEND " + theme + ": selected=" + ((CheckBox)Find("MochiCheck")).IsChecked + ", before=" + reasonBeforeRefresh + ", refreshed=" + ((TextBlock)Find("MochiReason")).Text);
                Pump(300);
                AssertLayout(theme + "-zh-optiscaler");
                Shot(theme + "-zh-optiscaler");
                ((Button)Find("SettingsTab")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Pump(300);
                Shot(theme + "-zh-settings");
                ((Button)Find("SettingsCloseButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                var toggle = (Button)Find("ThemeToggle");
                toggle.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Pump(200);
                Check(toggle.ContextMenu.IsOpen && toggle.ContextMenu.Items.Count == 5, "appearance menu opens");
                foreach (MenuItem item in toggle.ContextMenu.Items)
                    Check(item.IsChecked == ((string)item.Tag == theme), "appearance menu selection: " + item.Tag);
                toggle.ContextMenu.IsOpen = false;
                Invoke("SetLanguage", "en");
                Shot(theme + "-en-optiscaler");
                ((Button)Find("SettingsTab")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Pump(200);
                Shot(theme + "-en-settings");
                ((Button)Find("SettingsCloseButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            }

            // Minimum window size: every key control must stay inside the window.
            foreach (var theme in new[] { "light", "dark" })
            {
                Invoke("SetTheme", theme);
                foreach (var lang in new[] { "zh", "en" })
                {
                    Invoke("SetLanguage", lang);
                    _window.Width = _window.MinWidth; _window.Height = _window.MinHeight;
                    Pump(600);
                    Check(Math.Abs(_window.ActualWidth - 1020) < 1 && Math.Abs(_window.ActualHeight - 660) < 1,
                        "min size is 1020x660 (got " + _window.ActualWidth + "x" + _window.ActualHeight + ")");
                    AssertLayout("min-" + theme + "-" + lang);
                    Shot("min-" + theme + "-" + lang);
                }
            }
            // Preview images used by the PR description.
            _window.Width = 1240; _window.Height = 800;
            Invoke("SetLanguage", "zh");
            SelectGame(list, false);
            foreach (var theme in new[] { "light", "dark" }) { Invoke("SetTheme", theme); Pump(500); Preview("preview-" + theme + "-zh"); }
            Invoke("SetLanguage", "en");
            foreach (var theme in new[] { "light", "dark" }) { Invoke("SetTheme", theme); Pump(500); Preview("preview-" + theme + "-en"); }
            Invoke("SetTheme", "light");
            Invoke("SetLanguage", "zh");
            ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
            ((CheckBox)Find("MochiCheck")).IsChecked = true;
            _controller.GetType().GetMethod("RefreshInspectionText", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(_controller, null);
            Preview("restored-light-zh");
            CheckProfileRows("zh", false);
            double previousY = -1;
            foreach (var name in new[] { "AdvancedProxySection", "AdvancedInstallSection", "AdvancedProfilesSection", "AdvancedLogSection" }) {
                var card = (FrameworkElement)Find(name);
                var y = card.TranslatePoint(new Point(), (UIElement)((ScrollViewer)Find("DetailsScroll")).Content).Y;
                Check(y > previousY, "main-page card order: " + name);
                previousY = y;
            }
            Invoke("SetTheme", "dark"); Preview("restored-dark-zh");
            Invoke("SetTheme", "light");
            _window.Width = 1020; Pump(300); Preview("restored-narrow-light-zh");
            _window.Width = 1240; Pump(300);
            string selectedProxy = (string)Member(_controller, "CurrentSelectedProxy");
            Invoke("SelectProxy", "winmm.dll");
            Invoke("UpdateProxyRecommendation", null);
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "proxy automatically returns to Wiki recommendation");
            ((RadioButton)Find("PillVersion")).IsChecked = true;
            Invoke("UpdateProxyRecommendation", null);
            Check((string)Member(_controller, "CurrentSelectedProxy") == "version.dll", "saved manual proxy takes precedence");
            ((Button)Find("UseRecommendationButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "Use suggestion restores Wiki recommendation");
            ((Button)Find("ProfilesExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(((StackPanel)Find("InstallLogCardBody")).Visibility == Visibility.Collapsed, "profile expansion leaves log collapsed");
            ((Button)Find("InstallLogExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Pump(300);
            Check(File.ReadAllText(settingsPath).Contains("\"OptiScalerConfigExpanded\":true") && File.ReadAllText(settingsPath).Contains("\"InstallLogExpanded\":true"), "card expansion persisted independently");
            Call("LoadSettings");
            Check(((StackPanel)Find("ProfilesCardBody")).IsVisible && ((StackPanel)Find("InstallLogCardBody")).IsVisible, "card expansion restored from settings");
            foreach (var name in new[] { "AdvancedProxySection", "AdvancedInstallSection", "AdvancedProfilesSection", "AdvancedLogSection" })
                Check(((FrameworkElement)Find(name)).IsVisible, "expanded section visible: " + name);
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "card expansion preserves proxy selection");
            Check(((StackPanel)Find("ProxyDetails")).Visibility == Visibility.Visible, "proxy selector always expanded");
            Preview("restored-expanded-light-zh");
            CheckProfileRows("zh", true);
            Check(!((Button)Find("ProxyChangeButton")).IsVisible, "proxy collapse action removed");
            Preview("advanced-proxy-light-zh");
            ((Button)Find("ProfileMoreButton")).BringIntoView(); Pump(300);
            ((Button)Find("ProfileMoreButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Pump(300);
            Check(((Popup)Find("ProfileMorePopup")).IsOpen, "profile overflow opens");
            Shot("advanced-profiles-menu-light-zh");
            var menuRoot = (FrameworkElement)((Popup)Find("ProfileMorePopup")).Child;
            var menuBitmap = new RenderTargetBitmap((int)Math.Ceiling(menuRoot.ActualWidth), (int)Math.Ceiling(menuRoot.ActualHeight), 96, 96, PixelFormats.Pbgra32);
            menuBitmap.Render(menuRoot);
            var menuEncoder = new PngBitmapEncoder();
            menuEncoder.Frames.Add(BitmapFrame.Create(menuBitmap));
            using (var fs = File.Create(Path.Combine(_out, "profile-overflow-light-zh.png"))) menuEncoder.Save(fs);
            ((Popup)Find("ProfileMorePopup")).IsOpen = false;
            _window.Width = 1020; Pump(200);
            foreach (var lang in new[] { "zh", "en" }) {
                Invoke("SetLanguage", lang);
                ((FrameworkElement)Find("AdvancedProfilesSection")).BringIntoView(); Pump(200);
                foreach (var pair in new[] { new[] { "ProfileMoreButton", "ProfileMorePopup" }, new[] { "AsiProfileMoreButton", "AsiProfileMorePopup" } }) {
                    ((Button)Find(pair[0])).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Pump(200);
                    var popup = (Popup)Find(pair[1]);
                    var popupRoot = (FrameworkElement)popup.Child;
                    var left = popupRoot.PointToScreen(new Point());
                    var windowLeft = _window.PointToScreen(new Point());
                    Check(left.X >= windowLeft.X && left.X + popupRoot.ActualWidth <= windowLeft.X + _window.ActualWidth + 1, "overflow fits narrow window: " + lang + pair[1]);
                    var root = (FrameworkElement)_window.Content;
                    var origin = root.PointToScreen(new Point());
                    Check(left.Y >= origin.Y && left.Y + popupRoot.ActualHeight <= origin.Y + root.ActualHeight + 1, "overflow fits narrow window vertically: " + lang + pair[1]);
                    var windowBitmap = new RenderTargetBitmap((int)Math.Ceiling(root.ActualWidth), (int)Math.Ceiling(root.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                    windowBitmap.Render(root);
                    var popupBitmap = new RenderTargetBitmap((int)Math.Ceiling(popupRoot.ActualWidth), (int)Math.Ceiling(popupRoot.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                    popupBitmap.Render(popupRoot);
                    var visual = new DrawingVisual();
                    using (var drawing = visual.RenderOpen()) {
                        drawing.DrawImage(windowBitmap, new Rect(0, 0, root.ActualWidth, root.ActualHeight));
                        drawing.DrawImage(popupBitmap, new Rect(left.X - origin.X, left.Y - origin.Y, popupRoot.ActualWidth, popupRoot.ActualHeight));
                    }
                    var bitmap = new RenderTargetBitmap((int)Math.Ceiling(root.ActualWidth), (int)Math.Ceiling(root.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                    bitmap.Render(visual);
                    var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using (var fs = File.Create(Path.Combine(_out, "narrow-" + pair[1] + "-" + lang + ".png"))) encoder.Save(fs);
                    popup.IsOpen = false;
                }
                var styleButton = (Button)Find("StyleInButton");
                styleButton.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                Check(styleButton.ActualWidth >= styleButton.DesiredSize.Width - 1, "shared look label fits: " + lang);
                Preview("restored-expanded-narrow-light-" + lang);
                CheckProfileRows(lang, true);
            }
            Invoke("SetLanguage", "zh"); _window.Width = 1240; Pump(200);
            ((Button)Find("ProfilesExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            ((Button)Find("InstallLogExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(File.ReadAllText(settingsPath).Contains("\"OptiScalerConfigExpanded\":false") && File.ReadAllText(settingsPath).Contains("\"InstallLogExpanded\":false"), "card collapse persisted");
            Invoke("SelectProxy", "");
            Check(((Button)Find("ProxyRequiredButton")).IsVisible, "missing proxy action remains visible");
            ((Button)Find("ProxyRequiredButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(((StackPanel)Find("ProxyDetails")).IsVisible, "missing proxy action reveals selector");
            Invoke("SelectProxy", selectedProxy);
            Check(!((Button)Find("LangZh")).IsVisible && !((Button)Find("LangEn")).IsVisible, "language buttons removed from title bar");
            Check(((ComboBox)Find("LangCombo")).Items.Count == 2, "language selector retained in Settings");
            Check(((TextBlock)Find("MochiHint")).Visibility == Visibility.Collapsed, "duplicate backend hints hidden");
            Check(((Border)Find("BeforeInstallChecklist")).IsVisible, "prerequisite checklist visible");
            Check(!((Border)Find("BeforeInstallChecklist")).IsAncestorOf((Border)Find("OpenRrUnsupportedBar")), "game-trait lock is outside pending actions");
            Check(((Border)Find("BackendSection")).IsAncestorOf((Border)Find("OpenRrUnsupportedBar")), "game-trait lock is in quality plugins");
            Check(((System.Windows.Documents.Hyperlink)Find("WikiDetailsLink")).NavigateUri.Scheme == "https", "Wiki details use a navigable URL");
            Call("SetStatus", "库已刷新", "Library refreshed");
            Invoke("SetLanguage", "en");
            Check(((TextBlock)Find("StatusText")).Text == "Library refreshed", "specific status localizes");
            Call("SetStatus", "success", "success");
            Call("RefreshStatusLanguage");
            Pump(50);
            Check(((TextBlock)Find("StatusText")).Text == "", "generic success hidden");
            Check(!((FrameworkElement)Find("StatusLed")).IsVisible, "no lone status dot without text");
            Call("SetStatus", "库已刷新", "Library refreshed");
            Pump(50);
            Check(((FrameworkElement)Find("StatusLed")).IsVisible, "specific status retains dot");
            Call("SetStatus", "", "");
            Check(!((FrameworkElement)Find("ModeStandaloneTag")).IsVisible && !((FrameworkElement)Find("ModeReShadeTag")).IsVisible, "redundant mode badges removed");
            foreach (var name in new[] { "MochiHotkeysButton", "MochiShareCodeButton", "UnlockOpenRrBtn" }) {
                var button = (Button)Find(name);
                Check(button.BorderThickness == new Thickness(0) && ((SolidColorBrush)button.Background).Color.A == 0, "secondary action is borderless: " + name);
            }
            Check(((Button)Find("ModelImportButton")).BorderThickness == new Thickness(1), "pending model action uses rounded secondary button");

            var selectedGame = Member(_controller, "_selectedGame");
            var cacheDir = Member(_controller, "_nativeDlssdCacheDir");
            var cacheValue = Member(_controller, "_nativeDlssdCacheVal");
            SetMember(_controller, "_nativeDlssdCacheDir", Path.GetDirectoryName((string)Member(selectedGame, "Exe")));
            SetMember(_controller, "_nativeDlssdCacheVal", true);
            Call("ApplyModeVisibility");
            foreach (var lang in new[] { "zh", "en" }) {
                Invoke("SetLanguage", lang);
                foreach (var gpu in new[] { new[] { "AMD Radeon RX 9070 XT", "9070xt" }, new[] { "AMD Radeon RX 6800", "6800" }, new[] { "", "unknown" }, new[] { "AMD Radeon RX 7900 XTX", "7900xtx" } }) {
                    Invoke("SetDetectedAdapter", gpu[0]);
                    var mochiHint = (TextBlock)Find("MochiDescription");
                    var openRrHint = (TextBlock)Find("OpenRrDescription");
                    Check((mochiHint.Visibility == Visibility.Collapsed) == (gpu[1] == "9070xt" || gpu[1] == "7900xtx"), "Mochizuki applicability: " + gpu[1]);
                    Check((openRrHint.Visibility == Visibility.Collapsed) == (gpu[1] == "9070xt"), "OpenRR applicability: " + gpu[1]);
                    var expectedColor = (SolidColorBrush)_window.FindResource(gpu[1] == "6800" ? "ThemeWarning" : "ThemeMuted");
                    if (openRrHint.Visibility == Visibility.Visible) Check(((SolidColorBrush)openRrHint.Foreground).Color == expectedColor.Color, "GPU hint severity: " + gpu[1]);
                    ((ScrollViewer)Find("DetailsScroll")).ScrollToTop(); Pump(250);
                    AssertLayout("gpu-light-" + lang + "-" + gpu[1]);
                    Shot("gpu-light-" + lang + "-" + gpu[1]);
                }
                ((Button)Find("ProfilesExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                ((Button)Find("InstallLogExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                ((StackPanel)Find("ProxyDetails")).Visibility = Visibility.Visible;
                Preview("density-advanced-light-" + lang);
                ((Button)Find("ProfilesExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                ((Button)Find("InstallLogExpandButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            }

            var inspection = Member(_controller, "_lastInspectResult");
            var savedPackageRoot = Member(inspection, "PackageRoot");
            SetMember(inspection, "PackageRoot", Path.GetDirectoryName(args[0]));
            var backend = ((Array)Member(inspection, "Backends")).GetValue(0);
            var savedMissing = Member(backend, "MissingPackageFiles");
            var savedHints = Member(backend, "Hints");
            SetMember(backend, "MissingPackageFiles", new[] { "runtime.dll", "runtime.dll" });
            SetMember(backend, "Hints", new string[0]);
            Call("RefreshModelImportBar");
            Check(((StackPanel)Find("MissingPackageItems")).Children.Count == 1, "duplicate missing components deduplicated");
            Check(((TextBlock)((DockPanel)((StackPanel)Find("MissingPackageItems")).Children[0]).Children[1]).Text.Contains("runtime.dll"), "component checklist identifies missing file");
            var packageRow = (DockPanel)((StackPanel)Find("MissingPackageItems")).Children[0];
            Check(!((TextBlock)packageRow.Children[1]).Text.Contains("⚠") && ((SolidColorBrush)((TextBlock)packageRow.Children[1]).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeInk")).Color, "package title is calm normal ink");
            Check(((Button)packageRow.Children[0]).BorderThickness == new Thickness(1), "package action is a proper secondary button");
            ((Button)Find("PrimaryButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check((bool)Member(_controller, "_pendingActionsWarned") && Member(_controller, "_session") == null && !(bool)Member(_controller, "_writeBusy"), "missing component click warns without starting install");
            Check(((Border)Find("PendingActionsCard")).HasAnimatedProperties, "warning pulses after blocked click");
            Call("RefreshModelImportBar");
            Check((bool)Member(_controller, "_pendingActionsWarned"), "warning survives inspection presentation refresh");
            SetMember(backend, "MissingPackageFiles", new string[0]);
            Call("RefreshModelImportBar");
            Check(((Border)Find("BeforeInstallChecklist")).Visibility == Visibility.Collapsed, "no prerequisites means no checklist");
            Check(!((TextBlock)Find("ModelImportRequirement")).IsVisible, "model filename detail hides with resolved item");
            Check(!(bool)Member(_controller, "_pendingActionsWarned") && !((Border)Find("PendingActionsCard")).HasAnimatedProperties, "resolution clears warning and animation");
            SetMember(_controller, "_nativeDlssdCacheVal", false);
            Call("RefreshModelImportBar");
            Check(((Border)Find("BeforeInstallChecklist")).Visibility == Visibility.Collapsed && ((Border)Find("OpenRrUnsupportedBar")).Visibility == Visibility.Visible, "lock-only state has a neutral plugin row but no pending block");
            SetMember(_controller, "_nativeDlssdCacheVal", true);
            SetMember(backend, "Hints", new[] { "mochi-unsupported-source" });
            Call("RefreshModelImportBar");
            Check(((TextBlock)Find("ModelImportText")).Text.Contains("unsupported"), "unsupported model source has actionable checklist item");
            CheckMochizukiImportCopy("en");
            Check(((TextBlock)Find("ModelImportRequirement")).IsVisible, "unsupported model source retains filename/version guidance");
            SetMember(backend, "MissingPackageFiles", savedMissing);
            SetMember(backend, "Hints", savedHints);
            SetMember(inspection, "PackageRoot", savedPackageRoot);
            SetMember(_controller, "_nativeDlssdCacheDir", cacheDir);
            SetMember(_controller, "_nativeDlssdCacheVal", cacheValue);
            Call("ApplyModeVisibility");
            Call("RefreshModelImportBar");
            Invoke("SetDetectedAdapter", "");
            SelectGame(list, false);
            Invoke("SetLanguage", "zh");
            foreach (var theme in new[] { "light", "dark", "classic", "midnight", "auto" }) {
                Invoke("SetTheme", theme);
                ((ScrollViewer)Find("DetailsScroll")).ScrollToTop();
                ((Button)Find("PrimaryButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
                Check((bool)Member(_controller, "_pendingActionsWarned") && Member(_controller, "_session") == null, "missing package/model click blocked: " + theme);
                Pump(1700);
                var card = (Border)Find("PendingActionsCard");
                Check(Math.Abs(card.Opacity - 1) < 0.001, "pulse finishes at steady opacity: " + theme);
                Check(((SolidColorBrush)card.Background).Color == ((SolidColorBrush)_window.FindResource("ThemeDangerBg")).Color && ((SolidColorBrush)card.BorderBrush).Color == ((SolidColorBrush)_window.FindResource("ThemeDanger")).Color, "warning uses danger colors: " + theme);
                Check(((SolidColorBrush)((TextBlock)Find("ModelImportText")).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeDanger")).Color, "model item uses danger ink: " + theme);
                Check(((SolidColorBrush)((TextBlock)Find("ModelImportRequirement")).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeDanger")).Color, "warned model requirement uses danger ink: " + theme);
                double requirementInk = Luminance(((SolidColorBrush)((TextBlock)Find("ModelImportRequirement")).Foreground).Color);
                double warningSurface = Luminance(((SolidColorBrush)card.Background).Color);
                Check((Math.Max(requirementInk, warningSurface) + 0.05) / (Math.Min(requirementInk, warningSurface) + 0.05) >= 4.5, "warned model requirement contrast: " + theme);
                Check(((SolidColorBrush)((TextBlock)Find("OpenRrUnsupportedText")).Foreground).Color == ((SolidColorBrush)_window.FindResource("ThemeMuted")).Color, "game-trait lock stays neutral when warned: " + theme);
                var bounds = card.TransformToAncestor((ScrollViewer)Find("DetailsScroll")).TransformBounds(new Rect(card.RenderSize));
                Check(bounds.Bottom <= ((ScrollViewer)Find("DetailsScroll")).ActualHeight + 1, "pending warning scrolled into view: " + theme);
                Check((string)((Button)Find("QuickLogButton")).Content == "安装日志" && ((TextBlock)Find("LogHeading")).Text == "安装日志", "install log labels in Chinese");
                Shot("warned-" + theme + "-zh-optiscaler");
                if (theme == "light") {
                    Invoke("SetLanguage", "en");
                    Check((string)((Button)Find("QuickLogButton")).Content == "Install log" && ((TextBlock)Find("LogHeading")).Text == "Install log", "install log labels in English");
                    Shot("warned-light-en-optiscaler");
                    Invoke("SetLanguage", "zh");
                }
                ((RadioButton)Find("ModeStandaloneRadio")).IsChecked = true;
                Check(!(bool)Member(_controller, "_pendingActionsWarned"), "mode change clears warning: " + theme);
                Check(((SolidColorBrush)card.Background).Color == ((SolidColorBrush)_window.FindResource("ThemeCardBg")).Color, "mode change restores calm card: " + theme);
                ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
            }
            ((Button)Find("PrimaryButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            SelectGame(list, true);
            Check(!(bool)Member(_controller, "_pendingActionsWarned"), "game change clears warning");
            CheckProxyModes(args[1]);
            CheckCommunityLinks();
        }
        catch (Exception ex)
        {
            Fail("exception: " + ex);
        }
        return FinishSmoke();
    }

    static int FinishSmoke()
    {
        try { if (_window != null) _window.Close(); } catch { }
        Pump(200);
        Console.WriteLine(Failures.Count == 0 ? "SMOKE OK" : ("SMOKE FAILED (" + Failures.Count + ")"));
        foreach (var f in Failures) Console.WriteLine("  - " + f);
        return Failures.Count == 0 ? 0 : 1;
    }

    static void CheckCommunityLinks()
    {
        var names = new[] { "GithubButton", "OpenRrAsiButton", "RtxRemixMcPatchButton", "MochizukiAuthorButton", "BilibiliButton" };
        var urls = new[] { "https://github.com/TheAutomatic/dlss-5-amd-project", "https://github.com/TheAutomatic/OpenRR-ASI", "https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch", "https://gitlab.com/mochizuki0323/DLSSNR-AMD", "https://space.bilibili.com/291088" };
        var panel = (WrapPanel)VisualTreeHelper.GetParent((Button)Find(names[0]));
        Check(panel.Children.Count == names.Length && _window.FindName("OpenRrAuthorButton") == null, "community links replace the old OpenRR button");
        for (int i = 0; i < names.Length; i++) {
            var button = (Button)Find(names[i]);
            Check(ReferenceEquals(panel.Children[i], button) && (string)button.ToolTip == urls[i], "community link order and URL: " + names[i]);
            var store = typeof(UIElement).GetProperty("EventHandlersStore", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(button, null);
            var handlers = (Array)store.GetType().GetMethod("GetRoutedEventHandlers", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic).Invoke(store, new object[] { ButtonBase.ClickEvent });
            Check(handlers.Length > 0, "community link click wired: " + names[i]);
        }
        foreach (var language in new[] { "en", "zh" }) {
            Invoke("SetLanguage", language);
            Check((string)((Button)Find("OpenRrAsiButton")).Content == "OpenRR-ASI", "OpenRR community label: " + language);
            Check((string)((Button)Find("RtxRemixMcPatchButton")).Content == (language == "zh" ? "RTX Remix MC 补丁" : "RTX Remix MC patch"), "RTX Remix community label: " + language);
        }
        Invoke("SetTheme", "light");
        ((Button)Find("SettingsTab")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent)); Pump(150);
        Shot("community-links-light-zh", false);
        ((Button)Find("SettingsCloseButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
    }

    static void CheckProxyModes(string settingsDirectory)
    {
        Invoke("SetTheme", "light");
        Invoke("SetLanguage", "zh");
        var previous = Member(_controller, "_selectedGame");
        var directory = Path.Combine(settingsDirectory, "proxy-mode-game", "64bit");
        Directory.CreateDirectory(directory);
        var executable = Path.Combine(directory, "obs64.exe");
        File.Copy(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "where.exe"), executable, true);
        var marker = Path.Combine(directory, "amd-presr-install.txt");
        File.WriteAllText(marker, "installMode=standalone\r\n");
        var game = Activator.CreateInstance(previous.GetType());
        SetMember(game, "Id", "manual:proxy-modes");
        SetMember(game, "Name", "64bit");
        SetMember(game, "Exe", executable);
        SetMember(game, "Root", directory);
        SetMember(game, "Source", "Manual");
        ((System.Collections.IList)Member(_controller, "_allGames")).Add(game);
        Call("RefreshGameListView");
        Call("SelectGame", game);
        WaitForInspection(false);
        var required = (Button)Find("ProxyRequiredButton");
        foreach (var mode in new[] { "standalone", "reshade" }) {
            ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
            Call("SelectProxy", ""); Pump(100);
            Check((string)Member(_controller, "CurrentSelectedProxy") == "" && required.Visibility == Visibility.Visible, "proxy required for OptiScaler without a proxy: " + mode);
            ((RadioButton)Find(mode == "standalone" ? "ModeStandaloneRadio" : "ModeReShadeRadio")).IsChecked = true;
            Pump(100);
            Console.WriteLine("PROXY_MODE switch " + mode + ": selected=" + Call("GetSelectedBaseMode") + ", proxy='" + Member(_controller, "CurrentSelectedProxy") + "', required=" + required.Visibility);
            Check(required.Visibility == Visibility.Collapsed, "proxy required hidden after switching to " + mode);
            if (mode == "standalone") {
                Check((string)Member(Member(_controller, "_lastInspectResult"), "InstalledMode") == "standalone", "fixture inspection detects installed Standalone");
                ((ScrollViewer)Find("DetailsScroll")).ScrollToTop(); Pump(100);
                Shot("proxy-64bit-standalone-light-zh", false);
                ((ScrollViewer)Find("DetailsScroll")).ScrollToVerticalOffset(360); Pump(100);
                Shot("proxy-64bit-standalone-guidance-light-zh", false);
                Call("RefreshProxySummary");
                Check(required.Visibility == Visibility.Collapsed, "explicit refresh disproves an incorrect mode gate");
            }
        }
        foreach (var installed in new[] { "standalone", "reshade-feeder" }) {
            Call("SelectGame", previous); WaitForInspection(false);
            ((RadioButton)Find("ModeOptiScalerRadio")).IsChecked = true;
            Call("SelectProxy", "");
            File.WriteAllText(marker, "installMode=" + installed + "\r\n");
            Call("SelectGame", game); WaitForInspection(false);
            var expected = installed == "standalone" ? "standalone" : "reshade";
            Check((string)Call("GetSelectedBaseMode") == expected, "inspection restores installed " + installed);
            Console.WriteLine("PROXY_MODE restore " + installed + ": selected=" + Call("GetSelectedBaseMode") + ", required=" + required.Visibility);
            Check(required.Visibility == Visibility.Collapsed, "proxy required hidden after restoring installed " + installed);
        }
        Call("SelectGame", (object)null);
        Check(required.Visibility == Visibility.Collapsed, "no proxy warning without a selected game");
        Call("SelectGame", previous); WaitForInspection(false);
    }

    static double Luminance(Color color)
    {
        return 0.2126 * Linear(color.R) + 0.7152 * Linear(color.G) + 0.0722 * Linear(color.B);
    }
    static double Linear(byte channel)
    {
        double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : Math.Pow((value + 0.055) / 1.055, 2.4);
    }

    static void CheckPreferences(Type controllerType, string settingsDirectory)
    {
        var root = Path.Combine(settingsDirectory, "preference-fixtures");
        Directory.CreateDirectory(root);
        var legacy = Path.Combine(root, "legacy-settings.json");
        var local = Path.Combine(root, "local", "settings.json");
        File.WriteAllText(legacy, "{\"Language\":\"en\",\"Theme\":\"classic\",\"OptiScalerConfigExpanded\":true,\"InstallLogExpanded\":true,\"RecentGameId\":\"do-not-migrate\",\"ShowAdvancedOptions\":true}");
        var read = controllerType.GetMethod("ReadSettingsWithMigration", BindingFlags.Static | BindingFlags.NonPublic);
        var migrated = read.Invoke(null, new object[] { local, legacy });
        Check((string)Member(migrated, "Theme") == "classic" && (string)Member(migrated, "Language") == "en", "legacy UI preferences migrate to LocalAppData file");
        Check((bool)Member(migrated, "OptiScalerConfigExpanded") && (bool)Member(migrated, "InstallLogExpanded"), "collapse states migrate");
        Check(Member(migrated, "RecentGameId") == null && !File.ReadAllText(local).Contains("ShowAdvancedOptions"), "migration excludes game state and obsolete key");
        File.WriteAllText(local, "{\"Theme\":\"midnight\",\"Language\":\"zh\"}");
        File.Delete(legacy);
        var fresh = read.Invoke(null, new object[] { local, Path.Combine(root, "fresh-zip", "settings.json") });
        Check((string)Member(fresh, "Theme") == "midnight", "fresh launcher directory restores existing LocalAppData appearance");
        File.WriteAllText(legacy, "{\"Theme\":\"light\"}");
        Check((string)Member(read.Invoke(null, new object[] { local, legacy }), "Theme") == "midnight", "LocalAppData takes precedence over old launcher preferences");
        var blocked = Path.Combine(root, "blocked-localappdata");
        File.WriteAllText(blocked, "not a directory");
        var fallback = read.Invoke(null, new object[] { Path.Combine(blocked, "settings.json"), legacy });
        Check((string)Member(fallback, "Theme") == "light", "unwritable LocalAppData falls back without crashing");
        File.WriteAllText(local, "not json");
        Check(read.Invoke(null, new object[] { local, legacy }) == null, "malformed preferences do not crash");
    }

    static void CheckProfileStores(string settingsDirectory)
    {
        var game = Member(_controller, "_selectedGame");
        var directory = Path.GetDirectoryName((string)Member(game, "Exe"));
        var asi = Path.Combine(directory, "dlssnr-amd.ini");
        var opti = Path.Combine(directory, "OptiScaler.ini");
        Check(!((Button)Find("IniExportButton")).IsEnabled, "OptiScaler export disabled without any source");
        Check(!(bool)Call("SaveAsiProfile", "Missing"), "missing dlssnr INI blocks profile save");
        Check(((TextBlock)Find("IniProfileStatus")).Text.Contains("dlssnr-amd.ini"), "missing ASI profile reports noasi");
        File.WriteAllText(opti, "[OptiScaler]\r\nName=legacy\r\n[Plugins]\r\nLoadAsiPlugins=true\r\n");
        Check((bool)Call("SaveIniProfile", "Existing OptiScaler profile"), "existing OptiScaler profile store remains usable");
        var optiBytes = File.ReadAllBytes(opti);
        var optiExport = Path.Combine(settingsDirectory, "exported-opti.ini");
        var optiIdentity = Call("CaptureIniGame");
        var optiPicker = (Microsoft.Win32.SaveFileDialog)Call("CreateIniExportPicker");
        Check(optiPicker.FileName == "OptiScaler.ini" && optiPicker.Filter == "INI (*.ini)|*.ini" && optiPicker.OverwritePrompt && optiPicker.RestoreDirectory, "OptiScaler export picker configuration");
        File.WriteAllText(opti, "[OptiScaler]\r\nName=current\r\n");
        Check((bool)Call("ExportIniProfileCore", optiIdentity, "Existing OptiScaler profile", optiExport) && File.ReadAllText(optiExport) == System.Text.Encoding.UTF8.GetString(optiBytes), "OptiScaler export uses selected profile bytes, not game INI");
        Check((bool)Call("ExportIniProfileCore", optiIdentity, "", optiExport) && File.ReadAllText(optiExport) == File.ReadAllText(opti), "current-game OptiScaler export preserves bytes");
        File.Delete(opti);
        Call("RefreshProfileActions");
        Check(((Button)Find("IniExportButton")).IsEnabled, "saved OptiScaler profile exports even without a game INI");
        Check(!(bool)Call("ExportIniProfileCore", optiIdentity, "", optiExport), "missing current OptiScaler INI blocks export");
        ((ComboBox)Find("IniProfileCombo")).SelectedIndex = 0;
        Check(!((Button)Find("IniExportButton")).IsEnabled, "current-game export disabled after its INI disappears");
        Check(!(bool)Call("ExportIniProfileCore", optiIdentity, "Gone", optiExport), "missing named OptiScaler profile blocks export");
        File.WriteAllBytes(opti, optiBytes);
        Check(!(bool)Call("ExportIniProfileCore", optiIdentity, "", directory), "unwritable export destination reports failure without crashing");
        Call("RefreshProfileActions");
        Check(((Button)Find("IniExportButton")).IsEnabled, "current-game export re-enabled when INI is restored");
        var original = "[NR]\r\nEnabled=true\r\n[Custom]\r\nValue=preserved\r\n";
        File.WriteAllText(asi, original);
        Check((bool)Call("SaveAsiProfile", "Shared look"), "dlssnr profile saved");
        var asiStore = Member(_controller, "_asiProfiles");
        var optiStore = Member(_controller, "_iniProfiles");
        Check((string)Member(asiStore, "RootDirectory") != (string)Member(optiStore, "RootDirectory"), "profile stores have independent folders");
        Check(((ComboBox)Find("IniProfileCombo")).Items.Count == 2 && ((ComboBox)Find("AsiProfileCombo")).Items.Count == 2, "both independent profile lists retained");
        var changed = "[NR]\r\nEnabled=false\r\n";
        File.WriteAllText(asi, changed);
        Check((bool)Call("ApplyAsiProfile"), "dlssnr profile applies through guarded controller");
        Check(File.ReadAllText(asi) == original && File.ReadAllText(opti).Contains("Name=legacy"), "dlssnr round trip preserves all sections without changing OptiScaler INI");
        var backups = Directory.GetFiles(directory, "dlssnr-amd.ini.bak-*");
        Check(backups.Length == 1 && File.ReadAllText(backups[0]) == changed, "atomic dlssnr apply creates exact original backup");
        var exported = Path.Combine(settingsDirectory, "exported.ini");
        var identity = Call("CaptureIniGame");
        Check((bool)Call("RunAsiProfileAction", identity, "export", exported, "Shared look") && File.ReadAllText(exported) == original, "dlssnr profile export round trip");
        Check((bool)Call("RunAsiProfileAction", identity, "import", "Imported look", exported), "dlssnr profile import");
        Check((bool)Call("RunAsiProfileAction", identity, "rename", "Renamed look", "Imported look"), "dlssnr profile rename");
        Check((bool)Call("RunAsiProfileAction", identity, "delete", "", "Renamed look"), "dlssnr profile delete");
        var invalid = Path.Combine(settingsDirectory, "invalid.ini");
        File.WriteAllText(invalid, "not an ini");
        Check(!(bool)Call("RunAsiProfileAction", identity, "import", "Invalid", invalid), "invalid dlssnr profile rejected");
        Check(((ComboBox)Find("AsiProfileCombo")).Items.Count == 2 && ((ComboBox)Find("IniProfileCombo")).Items.Count == 2, "failed import preserves both stores");
        var combo = (ComboBox)Find("AsiProfileCombo");
        combo.SelectedIndex = 1;
        Check((string)Member(_controller, "_asiSelectedProfile") == "Shared look", "main dropdown selects dlssnr profile");
        var optiCombo = (ComboBox)Find("IniProfileCombo");
        optiCombo.SelectedIndex = 0;
        Check(((TextBlock)Find("ProfilesSummary")).Text.Contains("OptiScaler.ini"), "OptiScaler collapsed header identifies current INI");
    }

    static void CheckProfileRows(string language, bool expanded)
    {
        var root = (UIElement)_window.Content;
        var label = (TextBlock)Find("AsiProfilesHeading");
        var optiLabel = (TextBlock)Find("OptiScalerConfigHeading");
        var combo = (ComboBox)Find("AsiProfileCombo");
        var a = label.TranslatePoint(new Point(), root);
        var b = combo.TranslatePoint(new Point(), root);
        Check(label.Text == (language == "zh" ? "dlssnr-amd 配置" : "dlssnr-amd config"), "inline profile label: " + language);
        Check(optiLabel.Text == (language == "zh" ? "OptiScaler 配置" : "OptiScaler config"), "OptiScaler inline label: " + language);
        Check((string)((ComboBoxItem)combo.Items[0]).Content == (language == "zh" ? "当前游戏 dlssnr-amd.ini" : "Current game dlssnr-amd.ini"), "NR current-game item keeps filename: " + language);
        Check((string)((ComboBoxItem)((ComboBox)Find("IniProfileCombo")).Items[0]).Content == (language == "zh" ? "当前游戏 OptiScaler.ini" : "Current game OptiScaler.ini"), "OptiScaler current-game item keeps filename: " + language);
        Check(Math.Abs(a.Y + label.ActualHeight / 2 - b.Y - combo.ActualHeight / 2) < 1, "NR label shares dropdown row: " + language);
        Check(label.FontSize == optiLabel.FontSize && label.Foreground.ToString() == optiLabel.Foreground.ToString(), "profile label typography matches");
        Check(Math.Abs(a.X - optiLabel.TranslatePoint(new Point(), root).X) < 1, "profile labels align");
        Check(((TextBlock)Find("ProfilesSummary")).Visibility == (expanded ? Visibility.Collapsed : Visibility.Visible), "collapsed summary does not overlap expanded dropdown");
        if (expanded) {
            foreach (var pair in new[] { new[] { "AsiProfileCombo", "IniProfileCombo" }, new[] { "AsiProfileApplyButton", "IniApplyButton" }, new[] { "AsiProfileSaveButton", "IniSaveButton" }, new[] { "AsiProfileMoreButton", "ProfileMoreButton" } }) {
                var first = (FrameworkElement)Find(pair[0]); var second = (FrameworkElement)Find(pair[1]);
                Check(Math.Abs(first.TranslatePoint(new Point(), root).X - second.TranslatePoint(new Point(), root).X) < 1, "paired profile columns align: " + language + pair[0]);
            }
            Check(Math.Abs(optiLabel.TranslatePoint(new Point(), root).Y + optiLabel.ActualHeight / 2 - ((ComboBox)Find("IniProfileCombo")).TranslatePoint(new Point(), root).Y - 16) < 1, "OptiScaler controls share label row");
        }
        Call("SetIniStatus", "", ""); Pump(50);
        Check(((TextBlock)Find("IniProfileStatus")).Visibility == Visibility.Collapsed, "empty profile status uses no space");
        Check(optiLabel.TranslatePoint(new Point(), root).Y - combo.TranslatePoint(new Point(0, combo.ActualHeight), root).Y <= 28, "no empty band between profile rows");
        Call("SetIniStatus", "io", "");
        Check(((TextBlock)Find("IniProfileStatus")).Visibility == Visibility.Visible, "profile errors remain visible");
        Call("SetIniStatus", "", "");
        Check((string)((Button)Find("IniExportButton")).Content == (language == "zh" ? "导出…" : "Export…"), "OptiScaler export label: " + language);
    }

    // Top of the details pane plus two scrolled pages so the lower sections are reviewable too.
    static void Preview(string name)
    {
        var scroll = (ScrollViewer)Find("DetailsScroll");
        scroll.ScrollToTop(); Pump(250); Shot(name);
        scroll.ScrollToVerticalOffset(scroll.ViewportHeight * 0.9); Pump(250); Shot(name + "-2");
        scroll.ScrollToBottom(); Pump(250); Shot(name + "-3");
        scroll.ScrollToTop(); Pump(150);
    }

    static void CheckMochizukiImportCopy(string language)
    {
        var requirement = (TextBlock)Find("ModelImportRequirement");
        var button = (Button)Find("ModelImportButton");
        Check(requirement.Text == (language == "zh" ? "需要 nvngx_dlssnr.dll（310.9.16 或 310.8.0）" : "Needs nvngx_dlssnr.dll (310.9.16 or 310.8.0)"), "model filename and versions: " + language);
        Check((string)button.Content == (language == "zh" ? "导入 dlssnr.dll…" : "Import dlssnr.dll…"), "model button names DLL: " + language);
        Check(((string)button.ToolTip).Contains(language == "zh" ? "复制到启动器目录" : "copied into the launcher folder"), "model button explains copy destination: " + language);
        var picker = (Microsoft.Win32.OpenFileDialog)Call("CreateMochizukiSourcePicker");
        Check(picker.Filter == "nvngx_dlssnr.dll|nvngx_dlssnr.dll" && picker.CheckFileExists && picker.RestoreDirectory, "Mochizuki picker filters to exact source DLL");
        Check(picker.Title.Contains("nvngx_dlssnr.dll"), "Mochizuki picker names source DLL");
        Check(((TextBlock)Find("ModeOptiDesc")).Text == (language == "zh" ? "大多数场景选这个" : "Best for most cases"), "OptiScaler mode recommendation: " + language);
        Check(((TextBlock)Find("ModeReShadeDesc")).Text == (language == "zh" ? "推荐老游戏和 DX9 游戏" : "Best for older and DX9 games"), "ReShade mode recommendation: " + language);
    }

    static object Find(string name)
    {
        var o = _window.FindName(name);
        if (o == null) throw new InvalidOperationException("missing x:Name " + name);
        return o;
    }

    static void Invoke(string method, string arg)
    {
        var m = _controller.GetType().GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
        if (m == null) throw new MissingMethodException(method);
        m.Invoke(_controller, new object[] { arg });
        Pump(300);
    }

    static bool SelectGame(ListBox list, bool rtxMc)
    {
        for (int i = 0; i < list.Items.Count; i++)
        {
            var info = Member(list.Items[i], "Info") ?? list.Items[i];
            var isMc = Member(info, "IsRtxRemixMinecraft") as bool? == true;
            if (isMc == rtxMc)
            {
                if (list.SelectedIndex != i) { list.SelectedIndex = i; Pump(150); }
                WaitForInspection();
                return true;
            }
        }
        return false;
    }

    static object Member(object o, string name)
    {
        if (o == null) return null;
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        var f = o.GetType().GetField(name, flags);
        if (f != null) return f.GetValue(o);
        var p = o.GetType().GetProperty(name, flags);
        return p != null ? p.GetValue(o, null) : null;
    }

    static object Call(string name, params object[] args)
    {
        return _controller.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic).Invoke(_controller, args);
    }
    static void SetMember(object target, string name, object value)
    {
        target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic).SetValue(target, value);
    }

    static void CheckEnglish(string lang)
    {
        if (lang != "en") return;
        var status = (TextBlock)Find("StatusText");
        foreach (var c in status.Text ?? "")
            if (c >= 0x4E00 && c <= 0x9FFF) { Fail("English mode StatusText contains Chinese: " + status.Text); return; }
    }

    static void AssertLayout(string state)
    {
        var root = (FrameworkElement)_window.Content;
        var w = root.ActualWidth; var h = root.ActualHeight;
        foreach (var name in new[] { "SearchBox", "RefreshButton", "ThemeToggle", "SettingsTab", "MinBtn", "MaxBtn", "CloseBtn",
                                     "AddMenuButton", "GameList", "PrimaryButton", "StatusText", "DetailsScroll" })
        {
            var el = _window.FindName(name) as FrameworkElement;
            if (el == null) { Fail(state + ": missing " + name); continue; }
            if (!el.IsVisible) continue;
            var tl = el.TranslatePoint(new Point(0, 0), root);
            var br = el.TranslatePoint(new Point(el.ActualWidth, el.ActualHeight), root);
            if (tl.X < -0.5 || tl.Y < -0.5 || br.X > w + 0.5 || br.Y > h + 0.5)
                Fail(state + ": " + name + " outside the window (" + tl + " .. " + br + " in " + w + "x" + h + ")");
            if (el.ActualWidth < 1 || el.ActualHeight < 1) Fail(state + ": " + name + " has zero size");
        }
        // Hero action buttons must not be squeezed below their desired width (text clipping).
        foreach (var name in new[] { "PrimaryButton", "LaunchGameButton", "ManageGameButton", "OpenFolderButton", "UninstallButton" })
        {
            var el = _window.FindName(name) as FrameworkElement;
            if (el == null || !el.IsVisible) continue;
            // DesiredSize includes the margin; ActualWidth does not.
            if (el.ActualWidth + el.Margin.Left + el.Margin.Right + 0.5 < el.DesiredSize.Width) Fail(state + ": " + name + " clipped (" + el.ActualWidth + " < " + el.DesiredSize.Width + ")");
        }
    }

    static void Shot(string name)
    {
        Shot(name, true);
    }

    static void Shot(string name, bool settle)
    {
        try
        {
            if (settle) WaitForInspection();
            var root = (FrameworkElement)_window.Content;
            int w = (int)Math.Ceiling(root.ActualWidth), h = (int)Math.Ceiling(root.ActualHeight);
            if (w < 1 || h < 1) return;
            var dv = new DrawingVisual();
            using (var dc = dv.RenderOpen())
            {
                var bg = _window.Background as SolidColorBrush;
                // Mica/transparent canvases render as the solid fallback colour.
                dc.DrawRectangle(bg != null && bg.Color.A > 0 ? (Brush)bg : (_window.FindResource("ThemeBackground") as Brush), null, new Rect(0, 0, w, h));
                dc.DrawRectangle(new VisualBrush(root) { Stretch = Stretch.None, AlignmentX = AlignmentX.Left, AlignmentY = AlignmentY.Top }, null, new Rect(0, 0, w, h));
            }
            var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
            rtb.Render(dv);
            var enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(rtb));
            using (var fs = File.Create(Path.Combine(_out, name + ".png"))) enc.Save(fs);
        }
        catch (Exception ex) { Fail("screenshot " + name + ": " + ex.Message); }
    }

    static void Check(bool ok, string what)
    {
        if (!ok) Fail(what);
    }

    static void Fail(string message)
    {
        Failures.Add(message);
        Console.WriteLine("FAIL: " + message);
    }

    static void WaitForInspection()
    {
        WaitForInspection(true);
    }

    static void WaitForInspection(bool refreshText)
    {
        var end = DateTime.UtcNow.AddSeconds(30);
        while (DateTime.UtcNow < end) {
            var session = Member(_controller, "_session");
            if (session == null && !(bool)Member(_controller, "_presentationBusy") && Member(_controller, "_lastInspectResult") != null) {
                Pump(100);
                if (Member(_controller, "_session") == null && !(bool)Member(_controller, "_presentationBusy")) {
                    if (refreshText) Call("RefreshInspectionText");
                    Pump(100);
                    if (Member(_controller, "_session") == null && !(bool)Member(_controller, "_presentationBusy")) return;
                }
            }
            Pump(50);
        }
        throw new TimeoutException("Inspection did not settle before screenshot");
    }

    static void Pump(int ms)
    {
        var end = DateTime.UtcNow.AddMilliseconds(ms);
        do
        {
            var frame = new DispatcherFrame();
            Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Background,
                new DispatcherOperationCallback(delegate(object f) { ((DispatcherFrame)f).Continue = false; return null; }), frame);
            Dispatcher.PushFrame(frame);
            Thread.Sleep(15);
        } while (DateTime.UtcNow < end);
    }
}
