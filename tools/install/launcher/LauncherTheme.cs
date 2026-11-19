using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Data;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    // Shared mutable brushes keep already-created controls, badges and dialogs live.
    public static class LauncherTheme
    {
        public static ResourceDictionary Resources { get; private set; }
        static readonly string[] Keys = { "ThemeBackground", "ThemeSurface", "ThemeSubtle", "ThemeInk", "ThemeMuted", "ThemeBorder", "ThemeAccent", "ThemeHover", "ThemeSuccess", "ThemeSuccessBg", "ThemeWarning", "ThemeWarningBg", "ThemeOnAccent" };
        static readonly string[] Light = { "#F4F5F9", "#FFFFFF", "#F7F8FB", "#0D1220", "#66708A", "#E7E9F0", "#E3122B", "#ECEFF5", "#128445", "#EAF7EF", "#94500A", "#FDF6E7", "#FFFFFF" };
        static readonly string[] Dark = { "#0E1117", "#161B22", "#1F242D", "#F0F6FC", "#9BA8BA", "#30363D", "#E3122B", "#29313E", "#4ADE80", "#172E23", "#FFCA70", "#392D18", "#FFFFFF" };
        sealed class PaletteColor : DependencyObject
        {
            public static readonly DependencyProperty ColorProperty = DependencyProperty.Register("Color", typeof(Color), typeof(PaletteColor));
            public Color Color { get { return (Color)GetValue(ColorProperty); } set { SetValue(ColorProperty, value); } }
        }
        static readonly PaletteColor[] Colors = new PaletteColor[Keys.Length];
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
        public static string Normalize(string value)
        {
            return value == "light" || value == "dark" ? value : "auto";
        }
        public static void Apply(string mode)
        {
            bool light = mode == "light";
            if (mode == "auto")
            {
                try
                {
                    using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
                        light = key == null || Convert.ToInt32(key.GetValue("AppsUseLightTheme", 1)) != 0;
                }
                catch { light = true; }
            }
            var palette = light ? Light : Dark;
            for (int i = 0; i < Keys.Length; i++)
                Colors[i].Color = (Color)ColorConverter.ConvertFromString(palette[i]);
        }
    }
}
