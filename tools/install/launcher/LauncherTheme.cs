using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Data;
using System.Windows.Shell;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    // Shared mutable brushes keep already-created controls, badges and dialogs live.
    public static class LauncherTheme
    {
        public static ResourceDictionary Resources { get; private set; }
        // The first 13 keys are the original contract used by code and older dialogs; keep their order.
        // The remaining keys are the Fluent semantic layers (canvas -> layer -> card) used by the redesign.
        static readonly string[] Keys = {
            "ThemeBackground", "ThemeSurface", "ThemeSubtle", "ThemeInk", "ThemeMuted", "ThemeBorder", "ThemeAccent", "ThemeHover", "ThemeSuccess", "ThemeSuccessBg", "ThemeWarning", "ThemeWarningBg", "ThemeOnAccent",
            "ThemeCanvas", "ThemeLayer", "ThemeSidebar", "ThemeCardBg", "ThemeCardHover", "ThemeHairline", "ThemeFaint",
            "ThemeAccentHover", "ThemeAccentPressed", "ThemeAccentSoft", "ThemeAccentBorder", "ThemeAccentText",
            "ThemeDanger", "ThemeDangerBg", "ThemeInputBg", "ThemeControl", "ThemeControlHover", "ThemeControlPressed",
            "ThemeScrim", "ThemeHeroTint", "ThemeSelected", "ThemeOverlay" };
        static readonly string[] Light = {
            "#F3F3F3", "#FFFFFF", "#F6F6F6", "#1B1B1B", "#5F5F5F", "#E1E1E1", "#E3122B", "#EFEFEF", "#0F7B0F", "#E3F4E1", "#9D5D00", "#FFF4CE", "#FFFFFF",
            "#F3F3F3", "#F9F9F9", "#F3F3F3", "#FFFFFF", "#FBFBFB", "#E5E5E5", "#8A8A8A",
            "#CC1027", "#B00E22", "#FDEFF1", "#F3BCC4", "#C50F24",
            "#C42B1C", "#FDE7E9", "#FFFFFF", "#FBFBFB", "#F4F4F4", "#EDEDED",
            "#59000000", "#FCEEF0", "#EAEAEA", "#FFFFFF" };
        static readonly string[] Dark = {
            "#202020", "#2B2B2B", "#323232", "#F3F3F3", "#ADADAD", "#3D3D3D", "#E3122B", "#383838", "#6CCB5F", "#1E3320", "#F2C661", "#3D3218", "#FFFFFF",
            "#202020", "#272727", "#202020", "#2D2D2D", "#323232", "#383838", "#8B8B8B",
            "#F0283F", "#C8102A", "#3A262A", "#6B3039", "#FF7B8A",
            "#FF99A4", "#442726", "#2A2A2A", "#333333", "#3B3B3B", "#2E2E2E",
            "#8C000000", "#36252A", "#3A3A3A", "#000000" };
        // With a Mica backdrop the outer layers become (semi) transparent so DWM material shows through.
        static readonly string[] MicaKeys = { "ThemeCanvas", "ThemeSidebar", "ThemeLayer" };
        static readonly string[] MicaLight = { "#00000000", "#00000000", "#B3FAFAFA" };
        static readonly string[] MicaDark = { "#00000000", "#00000000", "#4D2C2C2C" };
        sealed class PaletteColor : DependencyObject
        {
            public static readonly DependencyProperty ColorProperty = DependencyProperty.Register("Color", typeof(Color), typeof(PaletteColor));
            public Color Color { get { return (Color)GetValue(ColorProperty); } set { SetValue(ColorProperty, value); } }
        }
        static readonly PaletteColor[] Colors = new PaletteColor[Keys.Length];
        static string _mode = "auto";
        public static bool IsDark { get; private set; }
        public static bool MicaActive { get; private set; }
        // Raised after every palette change (explicit switch, system auto change, Mica toggle).
        public static event EventHandler Changed;
        static LauncherTheme()
        {
            Resources = new ResourceDictionary();
            for (int i = 0; i < Keys.Length; i++) {
                Colors[i] = new PaletteColor();
                var brush = new SolidColorBrush();
                // A binding prevents WPF style sealing from freezing this shared brush.
                BindingOperations.SetBinding(brush, SolidColorBrush.ColorProperty, new Binding("Color") { Source = Colors[i] });
                Resources[Keys[i]] = brush;
            }
            Apply("auto");
        }
        public static Brush Brush(string key) { return (Brush)Resources[key]; }
        public static Color ColorOf(string key)
        {
            for (int i = 0; i < Keys.Length; i++) if (Keys[i] == key) return Colors[i].Color;
            return System.Windows.Media.Colors.Transparent;
        }
        public static string Normalize(string value)
        {
            return value == "light" || value == "dark" ? value : "auto";
        }
        public static void SetMica(bool active)
        {
            if (MicaActive == active) return;
            MicaActive = active;
            Apply(_mode);
        }
        public static void Apply(string mode)
        {
            _mode = Normalize(mode);
            bool light = mode == "light";
            if (mode != "light" && mode != "dark")
            {
                try
                {
                    using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
                        light = key == null || Convert.ToInt32(key.GetValue("AppsUseLightTheme", 1)) != 0;
                }
                catch { light = true; }
            }
            IsDark = !light;
            var palette = light ? Light : Dark;
            for (int i = 0; i < Keys.Length; i++)
                Colors[i].Color = (Color)ColorConverter.ConvertFromString(palette[i]);
            if (MicaActive)
            {
                var mica = light ? MicaLight : MicaDark;
                for (int m = 0; m < MicaKeys.Length; m++)
                    for (int i = 0; i < Keys.Length; i++)
                        if (Keys[i] == MicaKeys[m]) Colors[i].Color = (Color)ColorConverter.ConvertFromString(mica[m]);
            }
            var handler = Changed;
            if (handler != null) handler(null, EventArgs.Empty);
        }
    }

    // Win11 native frame integration through DWM. Every call is best effort: on Windows 10 (or when an
    // attribute is unsupported) DWM returns an error and the WPF-drawn 1px border / solid colours remain.
    public static class NativeFrame
    {
        const int DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19;
        const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        const int DWMWA_BORDER_COLOR = 34;
        const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
        const int DWMWCP_ROUND = 2;
        const int DWMWCP_ROUNDSMALL = 3;
        const int DWMSBT_MAINWINDOW = 2; // Mica

        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
        [StructLayout(LayoutKind.Sequential)]
        struct Margins { public int Left, Right, Top, Bottom; }
        [DllImport("dwmapi.dll")]
        static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct OsVersionInfo
        {
            public int Size, Major, Minor, Build, Platform;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string ServicePack;
        }
        [DllImport("ntdll.dll", CharSet = CharSet.Unicode)]
        static extern int RtlGetVersion(ref OsVersionInfo info);

        sealed class Entry { public WeakReference<Window> Window; public bool Main; }
        static readonly List<Entry> Windows = new List<Entry>();
        static readonly int Build = ReadBuild();

        public static bool IsWindows11 { get { return Build >= 22000; } }
        static bool MicaSupported
        {
            get { return Build >= 22621 && Environment.GetEnvironmentVariable("OPTSCALER_NO_MICA") != "1"; }
        }
        // Win11 draws the 1px border and rounded corners itself; Win10 keeps the WPF border.
        public static Thickness NormalBorder { get { return IsWindows11 ? new Thickness(0) : new Thickness(1); } }

        static NativeFrame()
        {
            LauncherTheme.Changed += delegate { RefreshAll(); };
        }
        static int ReadBuild()
        {
            try
            {
                var info = new OsVersionInfo();
                info.Size = Marshal.SizeOf(typeof(OsVersionInfo));
                if (RtlGetVersion(ref info) == 0 && info.Build > 0) return info.Build;
            }
            catch { }
            try
            {
                using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                {
                    int build;
                    if (key != null && int.TryParse(Convert.ToString(key.GetValue("CurrentBuildNumber")), out build)) return build;
                }
            }
            catch { }
            return 0;
        }

        // main=true: resizable launcher window (rounded corners + optional Mica). Dialogs get small-radius corners.
        public static void Attach(Window window, bool main)
        {
            if (window == null) return;
            lock (Windows) Windows.Add(new Entry { Window = new WeakReference<Window>(window), Main = main });
            if (IsWindows11) window.BorderThickness = new Thickness(0);
            if (new WindowInteropHelper(window).Handle != IntPtr.Zero) Initialize(window, main);
            else window.SourceInitialized += delegate { Initialize(window, main); };
        }

        static void Initialize(Window window, bool main)
        {
            var hwnd = new WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return;
            try
            {
                if (IsWindows11)
                {
                    int corner = main ? DWMWCP_ROUND : DWMWCP_ROUNDSMALL;
                    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));
                }
                if (main && MicaSupported) TryEnableMica(window, hwnd);
            }
            catch { }
            ApplyColors(window);
        }

        static void TryEnableMica(Window window, IntPtr hwnd)
        {
            int backdrop = DWMSBT_MAINWINDOW;
            if (DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int)) != 0) return;
            var source = HwndSource.FromHwnd(hwnd);
            if (source == null || source.CompositionTarget == null) return;
            // Extend the DWM frame over the whole client area so the backdrop renders behind WPF content.
            var old = WindowChrome.GetWindowChrome(window);
            var chrome = new WindowChrome {
                CaptionHeight = old != null ? old.CaptionHeight : 48,
                ResizeBorderThickness = old != null ? old.ResizeBorderThickness : new Thickness(6),
                CornerRadius = new CornerRadius(0),
                GlassFrameThickness = new Thickness(-1),
                UseAeroCaptionButtons = false
            };
            WindowChrome.SetWindowChrome(window, chrome);
            source.CompositionTarget.BackgroundColor = System.Windows.Media.Colors.Transparent;
            var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
            DwmExtendFrameIntoClientArea(hwnd, ref margins);
            window.Background = Brushes.Transparent;
            LauncherTheme.SetMica(true);
        }

        static void ApplyColors(Window window)
        {
            var hwnd = new WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return;
            try
            {
                int dark = LauncherTheme.IsDark ? 1 : 0;
                if (Build >= 18985) DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
                else if (Build >= 17763) DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, ref dark, sizeof(int));
                if (IsWindows11)
                {
                    var c = LauncherTheme.ColorOf(LauncherTheme.IsDark ? "ThemeBorder" : "ThemeHairline");
                    int colorRef = c.R | (c.G << 8) | (c.B << 16);
                    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, ref colorRef, sizeof(int));
                }
            }
            catch { }
        }

        static void RefreshAll()
        {
            List<Window> live = new List<Window>();
            lock (Windows)
            {
                for (int i = Windows.Count - 1; i >= 0; i--)
                {
                    Window w;
                    if (Windows[i].Window.TryGetTarget(out w)) live.Add(w);
                    else Windows.RemoveAt(i);
                }
            }
            foreach (var w in live)
            {
                var target = w;
                if (target.Dispatcher.CheckAccess()) ApplyColors(target);
                else target.Dispatcher.BeginInvoke(new Action(delegate { ApplyColors(target); }));
            }
        }
    }
}
