using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace OptScalerNR.Launcher
{
    // One bounded background queue keeps disk/Shell decoding off the dispatcher.
    internal static class LocalArtwork
    {
        internal sealed class Request { public string Path; public int Pixels; public bool Icon, Landscape; public Action<ImageSource> Complete; }
        internal sealed class Scope
        {
            internal Dispatcher Dispatcher;
            internal IEnumerator<Request> Requests;
            internal int Generation;
            internal bool Queued;
            internal Scope(Dispatcher dispatcher) { Dispatcher = dispatcher; }
            internal void Replace(IEnumerable<Request> requests)
            {
                lock (Gate) {
                    ++Generation;
                    if (Requests != null) Requests.Dispose();
                    Requests = requests == null ? null : requests.GetEnumerator();
                    if (Requests != null && !Queued) { Pending.Enqueue(this); Queued = true; }
                    if (!running && Pending.Count != 0) { running = true; ThreadPool.QueueUserWorkItem(Drain); }
                }
            }
        }
        static readonly object Gate = new object();
        // One cursor per live surface, not one queue entry per image or discarded row.
        static readonly Queue<Scope> Pending = new Queue<Scope>();
        static readonly Dictionary<string, ImageSource> Cache = new Dictionary<string, ImageSource>(StringComparer.OrdinalIgnoreCase);
        static readonly Queue<string> Order = new Queue<string>();
        static bool running;
        internal static void Load(Scope scope, string path, int pixels, bool icon, bool landscape, Action<ImageSource> complete)
        {
            scope.Replace(new[] { new Request { Path = path, Pixels = pixels, Icon = icon, Landscape = landscape, Complete = complete } });
        }
        static void Drain(object unused)
        {
            CoInitializeEx(IntPtr.Zero, 0);
            try {
                while (true) {
                    Scope scope; Request request; int generation;
                    lock (Gate) {
                        if (Pending.Count == 0) { running = false; return; }
                        scope = Pending.Dequeue(); scope.Queued = false;
                        if (scope.Requests == null) continue;
                        if (!scope.Requests.MoveNext()) { scope.Requests.Dispose(); scope.Requests = null; continue; }
                        request = scope.Requests.Current; generation = scope.Generation;
                        Pending.Enqueue(scope); scope.Queued = true;
                    }
                    ImageSource image = null;
                    try {
                        if (!string.IsNullOrEmpty(request.Path)) {
                            int pixels = Math.Max(42, Math.Min(request.Pixels, 1920));
                            var key = request.Path + "|" + File.GetLastWriteTimeUtc(request.Path).Ticks + "|" + pixels + "|" + request.Icon + "|" + request.Landscape;
                            if (!Cache.TryGetValue(key, out image)) {
                                image = request.Icon ? ReadIcon(request.Path, pixels) : ReadImage(request.Path, pixels, request.Landscape);
                                Cache[key] = image; Order.Enqueue(key);
                                while (Order.Count > 192) Cache.Remove(Order.Dequeue());
                            }
                        }
                    } catch { }
                    var result = image;
                    if (!scope.Dispatcher.HasShutdownStarted) {
                        try { scope.Dispatcher.Invoke(DispatcherPriority.Background, new Action(delegate {
                            if (generation == scope.Generation) request.Complete(result);
                        })); } catch (System.Threading.Tasks.TaskCanceledException) { }
                    }
                }
            } finally { CoUninitialize(); }
        }
        static ImageSource ReadImage(string path, int pixels, bool landscape)
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 32 * 1024 * 1024) return null;
            using (var stream = File.OpenRead(path)) {
                var frame = BitmapFrame.Create(stream, BitmapCreateOptions.DelayCreation, BitmapCacheOption.None);
                if (landscape && (frame.PixelWidth < 320 || frame.PixelHeight < 160 || frame.PixelWidth < frame.PixelHeight * 1.5)) return null;
            }
            var bitmap = new BitmapImage(); bitmap.BeginInit(); bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.DecodePixelWidth = pixels; bitmap.UriSource = new Uri(Path.GetFullPath(path)); bitmap.EndInit(); bitmap.Freeze(); return bitmap;
        }
        [StructLayout(LayoutKind.Sequential)] struct Size { public int Width, Height; }
        [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellItemImageFactory { [PreserveSig] int GetImage(Size size, uint flags, out IntPtr bitmap); }
        [DllImport("ole32.dll")] static extern int CoInitializeEx(IntPtr reserved, uint flags);
        [DllImport("ole32.dll")] static extern void CoUninitialize();
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
        static extern int SHCreateItemFromParsingName(string path, IntPtr context, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory factory);
        [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr handle);
        static ImageSource ReadIcon(string path, int pixels)
        {
            IShellItemImageFactory factory = null; IntPtr handle = IntPtr.Zero;
            try {
                var iid = typeof(IShellItemImageFactory).GUID;
                if (SHCreateItemFromParsingName(Path.GetFullPath(path), IntPtr.Zero, ref iid, out factory) != 0 || factory == null) return null;
                // ICONONLY | BIGGERSIZEOK: request the native multi-resolution Shell resource.
                if (factory.GetImage(new Size { Width = Math.Min(256, Math.Max(96, pixels)), Height = Math.Min(256, Math.Max(96, pixels)) }, 0x5, out handle) != 0 || handle == IntPtr.Zero) return null;
                var bitmap = Imaging.CreateBitmapSourceFromHBitmap(handle, IntPtr.Zero, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
                bitmap.Freeze(); return bitmap;
            } finally {
                if (handle != IntPtr.Zero) DeleteObject(handle);
                if (factory != null) Marshal.ReleaseComObject(factory);
            }
        }
    }
}
