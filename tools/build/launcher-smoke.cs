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

            var list = (ListBox)Find("GameList");
            var advanced = (CheckBox)Find("ShowAdvancedOptions");
            Check(advanced.IsChecked != true, "advanced options default off");
            foreach (var name in new[] { "AdvancedProxySection", "AdvancedInstallSection", "AdvancedProfilesSection", "AdvancedLogSection" })
                Check(((FrameworkElement)Find(name)).Visibility == Visibility.Collapsed, "advanced hidden by default: " + name);
            Check(list.Items.Count >= 2, "fake library loaded (" + list.Items.Count + " items)");

            var backendChecked = ((CheckBox)Find("MochiCheck")).IsChecked;
            var backendReason = ((TextBlock)Find("MochiReason")).Text;
            foreach (var appearance in new[] { "classic", "midnight", "light", "dark" }) {
                Invoke("SetTheme", appearance);
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
                    foreach (var mode in new[] { "ModeOptiScalerRadio", "ModeStandaloneRadio", "ModeReShadeRadio" })
                    {
                        ((RadioButton)Find(mode)).IsChecked = true;
                        Pump(400);
                        AssertLayout(theme + "-" + lang + "-" + mode);
                        if (mode == "ModeStandaloneRadio") Check(((FrameworkElement)Find("StandaloneModeCard")).IsVisible, "standalone settings remain visible");
                        if (mode == "ModeReShadeRadio") Check(((FrameworkElement)Find("ReShadeModeCard")).IsVisible, "ReShade settings remain visible");
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
            Preview("advanced-off-light-zh");
            string selectedProxy = (string)Member(_controller, "CurrentSelectedProxy");
            Invoke("SelectProxy", "winmm.dll");
            Invoke("UpdateProxyRecommendation", null);
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "hidden proxy automatically returns to Wiki recommendation");
            ((RadioButton)Find("PillVersion")).IsChecked = true;
            Invoke("UpdateProxyRecommendation", null);
            Check((string)Member(_controller, "CurrentSelectedProxy") == "version.dll", "saved manual proxy takes precedence");
            ((Button)Find("UseRecommendationButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "Use suggestion restores Wiki recommendation");
            advanced.IsChecked = true;
            Pump(300);
            Check(File.ReadAllText(Path.Combine(args[1], "settings.json")).Contains("\"ShowAdvancedOptions\":true"), "advanced preference persisted on");
            foreach (var name in new[] { "AdvancedProxySection", "AdvancedInstallSection", "AdvancedProfilesSection", "AdvancedLogSection" })
                Check(((FrameworkElement)Find(name)).IsVisible, "advanced visible: " + name);
            Check((string)Member(_controller, "CurrentSelectedProxy") == selectedProxy, "advanced toggle preserves proxy selection");
            Check(((StackPanel)Find("ProxyDetails")).Visibility == Visibility.Collapsed, "proxy details initially collapsed");
            Preview("advanced-on-light-zh");
            ((Button)Find("ProxyChangeButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(((StackPanel)Find("ProxyDetails")).Visibility == Visibility.Visible, "proxy Change expands details");
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
            advanced.IsChecked = false;
            Check(File.ReadAllText(Path.Combine(args[1], "settings.json")).Contains("\"ShowAdvancedOptions\":false"), "advanced preference persisted off");
            Invoke("SelectProxy", "");
            Check(((Button)Find("ProxyRequiredButton")).IsVisible, "missing proxy action remains visible with advanced off");
            ((Button)Find("ProxyRequiredButton")).RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(advanced.IsChecked == true && ((StackPanel)Find("ProxyDetails")).IsVisible, "missing proxy action reveals selector");
            Invoke("SelectProxy", selectedProxy);
            advanced.IsChecked = false;
        }
        catch (Exception ex)
        {
            Fail("exception: " + ex);
        }
        try { if (_window != null) _window.Close(); } catch { }
        Pump(200);
        Console.WriteLine(Failures.Count == 0 ? "SMOKE OK" : ("SMOKE FAILED (" + Failures.Count + ")"));
        foreach (var f in Failures) Console.WriteLine("  - " + f);
        return Failures.Count == 0 ? 0 : 1;
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

    // Top of the details pane plus two scrolled pages so the lower sections are reviewable too.
    static void Preview(string name)
    {
        var scroll = (ScrollViewer)Find("DetailsScroll");
        scroll.ScrollToTop(); Pump(250); Shot(name);
        scroll.ScrollToVerticalOffset(scroll.ViewportHeight * 0.9); Pump(250); Shot(name + "-2");
        scroll.ScrollToBottom(); Pump(250); Shot(name + "-3");
        scroll.ScrollToTop(); Pump(150);
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
                if (list.SelectedIndex != i) { list.SelectedIndex = i; Pump(1200); }
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
        try
        {
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
