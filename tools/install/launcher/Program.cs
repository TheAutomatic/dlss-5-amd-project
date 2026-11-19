using System;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Markup;

namespace OptScalerNR.Launcher
{
    public static class Program
    {
        public static DateTime StartedUtc { get; private set; }

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
    }
}
