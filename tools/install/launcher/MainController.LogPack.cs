using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

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
                DialogTheme.Show(_window,
                    L("请先在游戏库中选择要收集日志的游戏。", "Select a game in the library first."),
                    L("一键打包日志", "Pack Logs"), MessageBoxButton.OK, MessageBoxImage.Information);
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
                // One dedup set across all directories: the exe directory is scanned
                // first, so its live files win and a stale same-named file in the added
                // root can never overwrite them (File.Copy runs with overwrite).
                var taken = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var dir in dirs)
                {
                    copied += CopyGlobs(dir, tempDir, taken, new[]
                    {
                        "OptiScaler.log", "OptiScaler.?.log", "OptiScaler.ini",
                        "dlssnr-amd.ini", "dlssnr-amd.default.ini",
                        "mochizuki_nr.log", "dlssnr-amd*.log", "dlssnr-amd-crash.dmp"
                    });
                    // The ASI's recovery config lives beside its resources, not at the game root.
                    var asiDir = Path.Combine(dir, "dlssnr-amd");
                    if (Directory.Exists(asiDir))
                        copied += CopyGlobs(asiDir, tempDir, taken, new[] { "dlssnr-amd.default.ini" });
                }
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
                    DialogTheme.Show(_window,
                        L("未在游戏目录中找到可收集的日志（无 OptiScaler 或 mochizuki 日志）。\n请先运行一次游戏再执行打包。",
                          "No logs found in the game directory (no OptiScaler or mochizuki logs).\nPlease run the game once before packing logs."),
                        L("一键打包日志", "Pack Logs"), MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }

                var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                var zip = Path.Combine(desktop, "OptScalerNR-logs-" + stamp + ".zip");
                if (File.Exists(zip)) File.Delete(zip);
                ZipFile.CreateFromDirectory(tempDir, zip, System.IO.Compression.CompressionLevel.Optimal, false);

                ShowLogPackSuccessDialog(zip);
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window,
                    L("打包日志失败：" + ex.Message, "Failed to pack logs: " + ex.Message),
                    L("一键打包日志", "Pack Logs"), MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                try { if (tempDir != null && Directory.Exists(tempDir)) Directory.Delete(tempDir, true); } catch { }
            }
        }

        void ShowLogPackSuccessDialog(string zip)
        {
            var title = L("一键打包日志", "Pack Logs");
            var dialog = DialogTheme.Create(_window, title, 580);
            var panel = new StackPanel { Margin = new Thickness(22) };

            panel.Children.Add(new TextBlock
            {
                Text = L("日志与诊断信息已成功打包到桌面：", "Logs and diagnostic info have been packed to your desktop:"),
                Foreground = LauncherTheme.Brush("ThemeInk"),
                FontWeight = FontWeights.SemiBold,
                FontSize = 13,
                Margin = new Thickness(0, 0, 0, 10)
            });

            var pathBox = new Border
            {
                Background = LauncherTheme.Brush("ThemeSubtle"),
                BorderBrush = LauncherTheme.Brush("ThemeBorder"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(10, 8, 10, 8),
                Margin = new Thickness(0, 0, 0, 14)
            };
            var pathText = new TextBox
            {
                Text = zip,
                IsReadOnly = true,
                Background = Brushes.Transparent,
                Foreground = LauncherTheme.Brush("ThemeInk"),
                BorderThickness = new Thickness(0),
                VerticalAlignment = VerticalAlignment.Center,
                FontSize = 11.5,
                TextWrapping = TextWrapping.Wrap
            };
            pathBox.Child = pathText;
            panel.Children.Add(pathBox);

            panel.Children.Add(new TextBlock
            {
                Text = L(
                    "压缩包包含以下内容：\n" +
                    "• 游戏目录下的 OptiScaler / mochizuki 运行日志与 INI 配置文件\n" +
                    "• 启动器安装日志、系统硬件概况（显卡型号、驱动版本、内存容量）\n" +
                    "• 近两天的显卡驱动超时重置 (TDR) 与游戏崩溃记录\n\n" +
                    "向作者反馈问题时，请直接发送该压缩包。",
                    "The archive contains:\n" +
                    "• OptiScaler / mochizuki runtime logs and INI configs in the game directory\n" +
                    "• Launcher install log and system summary (GPU, driver version, RAM capacity)\n" +
                    "• Graphics driver timeout reset (TDR) and game crash events from the past 2 days\n\n" +
                    "When requesting assistance, please share this archive with the maintainers."
                ),
                Foreground = LauncherTheme.Brush("ThemeMuted"),
                FontSize = 11.5,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 20)
            });

            var buttons = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right };

            var locateBtn = new Button
            {
                Content = L("在文件夹中定位", "Locate File"),
                MinWidth = 100,
                Margin = new Thickness(4)
            };
            locateBtn.Click += delegate
            {
                try { Process.Start("explorer.exe", "/select,\"" + zip + "\""); }
                catch { }
            };

            var copyBtn = new Button
            {
                Content = L("复制路径", "Copy Path"),
                MinWidth = 90,
                Margin = new Thickness(4)
            };
            copyBtn.Click += delegate
            {
                try
                {
                    Clipboard.SetText(zip);
                    copyBtn.Content = L("已复制！", "Copied!");
                }
                catch { }
            };

            var okBtn = new Button
            {
                Content = L("确定", "OK"),
                MinWidth = 90,
                Margin = new Thickness(4)
            };
            DialogTheme.Primary(dialog, okBtn);
            okBtn.Click += delegate { dialog.DialogResult = true; };

            buttons.Children.Add(locateBtn);
            buttons.Children.Add(copyBtn);
            buttons.Children.Add(okBtn);

            panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            dialog.ShowDialog();
        }

        static int CopyGlobs(string dir, string dest, HashSet<string> taken, IEnumerable<string> patterns)
        {
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
