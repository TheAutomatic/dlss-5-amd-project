using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        // One-click log bundle for bug reports, modelled on the Mochizuki installer's
        // "collect logs" route: game-directory logs and configs, the launcher install log,
        // a system summary (GPU/driver/OS/RAM + game-dir listing) and the last two days of
        // driver/crash events, zipped to the desktop.
        void PackLogs()
        {
            var game = _selectedGame;
            // Install and INI operations target the chosen EXE's directory; collect there
            // first. GameInfo.Root can differ (root added, exe in a subfolder) and is only
            // a supplement for files that ended up there.
            var dirs = new List<string>();
            if (game != null && !string.IsNullOrEmpty(game.Exe))
            {
                try
                {
                    var exeDir = Path.GetDirectoryName(game.Exe);
                    if (!string.IsNullOrEmpty(exeDir) && Directory.Exists(exeDir))
                        dirs.Add(exeDir);
                }
                catch { }
            }
            string rootDir = null;
            try { rootDir = game != null ? game.RootDirectory : null; } catch { }
            if (!string.IsNullOrEmpty(rootDir) && Directory.Exists(rootDir) &&
                (dirs.Count == 0 || !string.Equals(rootDir, dirs[0], StringComparison.OrdinalIgnoreCase)))
                dirs.Add(rootDir);
            if (dirs.Count == 0)
            {
                MessageBox.Show(_window,
                    L("请先在游戏库中选择要收集日志的游戏。", "Select a game in the library first."),
                    _window.Title, MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            string tempDir = null;
            try
            {
                var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
                tempDir = Path.Combine(Path.GetTempPath(), "optscalernr-logs-" + stamp);
                if (Directory.Exists(tempDir)) Directory.Delete(tempDir, true);
                Directory.CreateDirectory(tempDir);

                int copied = 0;
                foreach (var dir in dirs)
                    copied += CopyGlobs(dir, tempDir, new[]
                    {
                        "OptiScaler.log", "OptiScaler.?.log", "OptiScaler.ini",
                        "mochizuki_nr.log", "dlssnr-amd*.log", "dlssnr-amd-crash.dmp"
                    });
                if (!string.IsNullOrEmpty(_currentLogPath) && File.Exists(_currentLogPath))
                {
                    File.Copy(_currentLogPath, Path.Combine(tempDir, "launcher-install" + Path.GetExtension(_currentLogPath)), true);
                    copied++;
                }

                File.WriteAllText(Path.Combine(tempDir, "system.txt"), BuildSystemSummary(game, dirs, _currentLogPath), new UTF8Encoding(false));
                var events = BuildRecentEvents(game != null ? game.Exe : null);
                if (events != null)
                    File.WriteAllText(Path.Combine(tempDir, "events.txt"), events, new UTF8Encoding(false));

                if (copied == 0 && events == null)
                {
                    MessageBox.Show(_window,
                        L("没有找到可收集的日志（游戏目录里没有 OptiScaler/mochizuki 日志）。\n请先运行一次游戏再打包。",
                          "No logs found to collect (no OptiScaler/mochizuki logs in the game directory).\nRun the game once before packing."),
                        _window.Title, MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }

                var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                var zip = Path.Combine(desktop, "OptScalerNR-logs-" + stamp + ".zip");
                if (File.Exists(zip)) File.Delete(zip);
                ZipFile.CreateFromDirectory(tempDir, zip, System.IO.Compression.CompressionLevel.Optimal, false);

                MessageBox.Show(_window,
                    L("日志已打包到桌面：\n" + zip + "\n\n压缩包包含游戏目录日志/配置、系统信息（GPU/驱动/内存）和近两天的驱动与崩溃事件。请把它发给支援者。",
                      "Logs packed to your desktop:\n" + zip + "\n\nThe archive contains the game directory logs/configs, a system summary (GPU/driver/RAM) and the last two days of driver and crash events. Send it to whoever is helping you."),
                    _window.Title, MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show(_window,
                    L("打包日志失败：" + ex.Message, "Failed to pack logs: " + ex.Message),
                    _window.Title, MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                try { if (tempDir != null && Directory.Exists(tempDir)) Directory.Delete(tempDir, true); } catch { }
            }
        }

        static int CopyGlobs(string dir, string dest, IEnumerable<string> patterns)
        {
            var taken = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            int n = 0;
            foreach (var pattern in patterns)
            {
                string[] files;
                try { files = Directory.GetFiles(dir, pattern); }
                catch { continue; }
                foreach (var file in files)
                {
                    if (!taken.Add(Path.GetFileName(file))) continue;
                    try { File.Copy(file, Path.Combine(dest, Path.GetFileName(file)), true); n++; } catch { }
                }
            }
            return n;
        }

        static string BuildSystemSummary(GameInfo game, IList<string> dirs, string launcherLog)
        {
            var sb = new StringBuilder();
            sb.AppendLine("game: " + (game != null ? game.DisplayName : "?"));
            sb.AppendLine("exe: " + (game != null ? game.Exe : "?"));
            sb.AppendLine("exe dir: " + (dirs.Count > 0 ? dirs[0] : "?"));
            for (int i = 1; i < dirs.Count; i++)
                sb.AppendLine("extra dir: " + dirs[i]);
            sb.AppendLine("launcher log: " + (launcherLog ?? ""));
            sb.AppendLine();

            sb.AppendLine("GPU (registry):");
            try
            {
                using (var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
                {
                    if (key != null)
                        foreach (var name in key.GetSubKeyNames())
                        {
                            if (!char.IsDigit(name[0])) continue;
                            using (var sub = key.OpenSubKey(name))
                            {
                                if (sub == null) continue;
                                var desc = sub.GetValue("DriverDesc") as string;
                                if (string.IsNullOrEmpty(desc)) continue;
                                sb.AppendLine("  " + desc + "  driver " + sub.GetValue("DriverVersion") + "  " + sub.GetValue("DriverDate"));
                            }
                        }
                }
            }
            catch (Exception ex) { sb.AppendLine("  (registry read failed: " + ex.Message + ")"); }
            sb.AppendLine();

            var mem = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf(typeof(MEMORYSTATUSEX)) };
            if (GlobalMemoryStatusEx(ref mem))
                sb.AppendLine("RAM: " + Math.Round(mem.ullTotalPhys / 1073741824.0, 1) + " GB physical; commit limit " +
                              Math.Round(mem.ullTotalPageFile / 1073741824.0, 1) + " GB, available " +
                              Math.Round(mem.ullAvailPageFile / 1073741824.0, 1) + " GB");
            sb.AppendLine();

            sb.AppendLine("game directory listing:");
            try
            {
                foreach (var dir in dirs)
            {
            foreach (var entry in new DirectoryInfo(dir).EnumerateFileSystemInfos())
                {
                    long size = 0;
                    var file = entry as FileInfo;
                    if (file != null) { try { size = file.Length; } catch { } }
                    sb.AppendLine(string.Format("{0,12} {1}", size, entry.Name));
                }
            }
            }
            catch (Exception ex) { sb.AppendLine("  (listing failed: " + ex.Message + ")"); }
            return sb.ToString();
        }

        // Returns null when event logs cannot be read (permissions, service disabled).
        static string BuildRecentEvents(string gameExe)
        {
            try
            {
                var since = DateTime.Now.AddDays(-2);
                var sb = new StringBuilder();
                var count = 0;

                using (var reader = new EventLogReader(new EventLogQuery("System", PathType.LogName,
                    string.Format("*[System[TimeCreated[timediff(@SystemTime) <= {0}]] and (System[Level=1] or System[Level=2] or System[Level=3])]",
                        (long)TimeSpan.FromDays(2).TotalMilliseconds))))
                {
                    EventRecord rec;
                    while (count < 300 && (rec = reader.ReadEvent()) != null)
                    {
                        using (rec)
                        {
                            if (rec.TimeCreated == null || rec.TimeCreated.Value < since) continue;
                            sb.AppendLine("[" + rec.TimeCreated.Value.ToString("yyyy-MM-dd HH:mm:ss") + "] System " +
                                          rec.ProviderName + " id=" + rec.Id);
                            count++;
                        }
                    }
                }

                var exeName = string.IsNullOrEmpty(gameExe) ? null : Path.GetFileName(gameExe);
                if (exeName != null)
                {
                    using (var reader = new EventLogReader(new EventLogQuery("Application", PathType.LogName,
                        string.Format("*[System[TimeCreated[timediff(@SystemTime) <= {0}]] and (System[EventID=1000] or System[EventID=1001] or System[EventID=1002])]",
                            (long)TimeSpan.FromDays(2).TotalMilliseconds))))
                    {
                        EventRecord rec;
                        while (count < 400 && (rec = reader.ReadEvent()) != null)
                        {
                            using (rec)
                            {
                                if (rec.TimeCreated == null || rec.TimeCreated.Value < since) continue;
                                string message = null;
                                try { message = rec.FormatDescription(); } catch { }
                                if (message == null || (!message.Contains(exeName) && !message.Contains("LiveKernelEvent"))) continue;
                                sb.AppendLine("[" + rec.TimeCreated.Value.ToString("yyyy-MM-dd HH:mm:ss") + "] Application " +
                                              rec.ProviderName + " id=" + rec.Id);
                                count++;
                            }
                        }
                    }
                }
                return sb.Length > 0 ? sb.ToString() : null;
            }
            catch { return null; }
        }

        [StructLayout(LayoutKind.Sequential)]
        struct MEMORYSTATUSEX
        {
            public uint dwLength;
            public uint dwMemoryLoad;
            public ulong ullTotalPhys;
            public ulong ullAvailPhys;
            public ulong ullTotalPageFile;
            public ulong ullAvailPageFile;
            public ulong ullTotalVirtual;
            public ulong ullAvailVirtual;
            public ulong ullAvailExtendedVirtual;
        }

        [DllImport("kernel32.dll", SetLastError = false)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX buffer);
    }
}
