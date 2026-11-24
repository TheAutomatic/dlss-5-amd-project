using System;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace OptScalerNR.Launcher
{
    public static class Program
    {
        public static DateTime StartedUtc { get; private set; }
        static ImageSource _windowIcon;
        static ImageSource _titleIcon;
        static bool _iconLoaded;

        [STAThread]
        public static int Main(string[] args)
        {
            StartedUtc = DateTime.UtcNow;
            var app = new Application();
            app.ShutdownMode = ShutdownMode.OnMainWindowClose;
            var window = MainController.Create(args);
            app.MainWindow = window;
            window.Show();
            return app.Run();
        }

        public static Stream OpenXaml(string name)
        {
            var asm = Assembly.GetExecutingAssembly();
            var stream = asm.GetManifestResourceStream("OptScalerNR.Launcher." + name);
            if (stream == null)
                throw new FileNotFoundException("Missing embedded XAML " + name);
            return stream;
        }

        public static ImageSource LoadWindowIcon()
        {
            EnsureIconsLoaded();
            return _windowIcon;
        }

        public static ImageSource LoadTitleIcon()
        {
            EnsureIconsLoaded();
            return _titleIcon;
        }

        static void EnsureIconsLoaded()
        {
            if (_iconLoaded) return;
            _iconLoaded = true;
            var asm = Assembly.GetExecutingAssembly();
            using (var res = asm.GetManifestResourceStream("OptScalerNR.Launcher.app.ico"))
            {
                if (res == null) return;
                var ms = new MemoryStream();
                res.CopyTo(ms);
                ms.Position = 0;
                var decoder = new IconBitmapDecoder(ms, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
                if (decoder.Frames == null || decoder.Frames.Count == 0) return;
                BitmapFrame best = decoder.Frames[0];
                foreach (var frame in decoder.Frames)
                {
                    if (frame.CanFreeze) frame.Freeze();
                    if (frame.PixelWidth > best.PixelWidth) best = frame;
                }
                _windowIcon = decoder.Frames[0];
                _titleIcon = best;
            }
        }
    }
}
