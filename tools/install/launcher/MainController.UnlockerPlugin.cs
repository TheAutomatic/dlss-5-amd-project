using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Button _unlockerImportButton, _openPluginsFolderButton;

        void InitUnlockerPlugin()
        {
            _unlockerImportButton = _window.FindName("UnlockerImportButton") as Button;
            if (_unlockerImportButton != null) _unlockerImportButton.Click += delegate { ImportUnlockerPlugin(); };
            _openPluginsFolderButton = _window.FindName("OpenPluginsFolderButton") as Button;
            if (_openPluginsFolderButton != null) _openPluginsFolderButton.Click += delegate { OpenPluginsFolder(); };
            if (_optiPatcherToggle != null)
            {
                _optiPatcherToggle.Checked += delegate { RefreshUnlockerPlugin(); };
                _optiPatcherToggle.Unchecked += delegate { RefreshUnlockerPlugin(); };
            }
        }

        void RefreshUnlockerLanguage()
        {
            if (_unlockerImportButton != null)
            {
                _unlockerImportButton.Content = L("导入 XeFG 解锁插件…", "Import XeFG unlocker…");
                _unlockerImportButton.ToolTip = L("可选 MFGUnlocker.asi、XeFGUnlock.asi 或对应 .dll（.dll 会改名为 .asi 复制）", "Pick MFGUnlocker.asi, XeFGUnlock.asi or their .dll (a .dll is copied renamed to .asi).");
            }
            if (_openPluginsFolderButton != null) _openPluginsFolderButton.Content = L("打开插件文件夹", "Open plugins folder");
        }

        string _cachedAsiIniPath;
        long _cachedAsiIniLen = -1, _cachedAsiIniTicks = -1;
        bool _cachedAsiEnabled;

        bool CanShowUnlockerPlugin(IniGameIdentity game)
        {
            if (game == null || !Directory.Exists(Path.Combine(game.Directory, "OptiScaler"))) return false;
            // ASI plugins are on by default now; OptiPatcher still implies plugin loading.
            if (_optiPatcherToggle != null && _optiPatcherToggle.IsChecked == true) return true;
            try
            {
                var iniPath = Path.Combine(game.Directory, "OptiScaler.ini");
                var info = new FileInfo(iniPath);
                if (!info.Exists) return false;
                if (string.Equals(_cachedAsiIniPath, info.FullName, StringComparison.OrdinalIgnoreCase) &&
                    _cachedAsiIniLen == info.Length && _cachedAsiIniTicks == info.LastWriteTimeUtc.Ticks)
                    return _cachedAsiEnabled;
                var enabled = string.Equals(UnlockerPlugin.LoadAsiValue(UnlockerPlugin.ReadIni(iniPath)), "true", StringComparison.OrdinalIgnoreCase);
                _cachedAsiIniPath = info.FullName;
                _cachedAsiIniLen = info.Length;
                _cachedAsiIniTicks = info.LastWriteTimeUtc.Ticks;
                _cachedAsiEnabled = enabled;
                return enabled;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        void RefreshUnlockerPlugin()
        {
            var game = CaptureIniGame();
            if (_unlockerImportButton != null)
            {
                _unlockerImportButton.Visibility = CanShowUnlockerPlugin(game) ? Visibility.Visible : Visibility.Collapsed;
                _unlockerImportButton.IsEnabled = !IniBusy();
            }
            if (_openPluginsFolderButton != null)
            {
                bool installed = game != null && Directory.Exists(Path.Combine(game.Directory, "OptiScaler"));
                _openPluginsFolderButton.Visibility = installed ? Visibility.Visible : Visibility.Collapsed;
                _openPluginsFolderButton.IsEnabled = installed && !IniBusy();
            }
        }

        void OpenPluginsFolder()
        {
            if (IniBusy()) return;
            var game = CaptureIniGame();
            if (game == null) return;
            try
            {
                var directory = UnlockerPlugin.DestinationDirectory(game.Directory, UnlockerPlugin.ReadIni(Path.Combine(game.Directory, "OptiScaler.ini")));
                if (!Directory.Exists(Path.Combine(game.Directory, "OptiScaler")) || !Directory.Exists(directory))
                {
                    SetIniStatus("plugins-missing", "");
                    return;
                }
                Process.Start(new ProcessStartInfo("explorer.exe", "\"" + directory + "\"") { UseShellExecute = true });
            }
            catch (Exception ex) { UnlockerStatus(L("无法打开插件文件夹：", "Could not open plugins folder: ") + ex.Message); }
        }

        void UnlockerStatus(string text)
        {
            SetIniStatus("unlocker", text);
            AppendLog("unlocker: " + text);
        }

        bool UnlockerGameRunning(string executable)
        {
            if (string.IsNullOrWhiteSpace(executable)) return false;
            var processes = Process.GetProcessesByName(Path.GetFileNameWithoutExtension(executable));
            try
            {
                foreach (var process in processes)
                {
                    try
                    {
                        if (process.HasExited) continue;
                        var module = process.MainModule;
                        if (module == null || SamePath(module.FileName, executable)) return true;
                    }
                    catch (System.ComponentModel.Win32Exception) { return true; }
                    catch (InvalidOperationException) { if (!process.HasExited) return true; }
                    catch (NotSupportedException) { return true; }
                }
                return false;
            }
            finally { foreach (var process in processes) process.Dispose(); }
        }

        bool RefuseRunningUnlockerGame(string executable)
        {
            if (!UnlockerGameRunning(executable)) return false;
            UnlockerStatus(L("请先关闭游戏，再导入解锁插件（同名进程无法验证路径时也需先关闭）。", "Close the game before importing the unlocker (also close a same-name process whose path cannot be verified)."));
            return true;
        }

        void ImportUnlockerPlugin()
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) { AppendLog("unlocker: " + _iniStatus); return; }
            if (!CanShowUnlockerPlugin(game))
            {
                UnlockerStatus(L("请先为所选游戏安装 OptiScaler 并启用 ASI 加载。", "Install OptiScaler for the selected game and enable ASI loading first."));
                return;
            }
            var executable = _selectedGame.Exe;
            var picker = new OpenFileDialog {
                Title = L("导入解锁插件：MFGUnlocker.asi / XeFGUnlock.asi", "Import unlocker: MFGUnlocker.asi / XeFGUnlock.asi"),
                Filter = "Plugin files (*.asi;*.dll)|*.asi;*.dll", CheckFileExists = true, RestoreDirectory = true
            };
            if (picker.ShowDialog(_window) != true) { UnlockerStatus(L("已取消导入。", "Import cancelled.")); return; }
            if (!CanUseIniGame(game)) { AppendLog("unlocker: " + _iniStatus); return; }
            var locked = false;
            var busy = false;
            var copied = false;
            var outcome = "";
            try
            {
                var name = UnlockerPlugin.DestinationName(picker.FileName);
                UnlockerPlugin.ValidatePe(picker.FileName);
                if (RefuseRunningUnlockerGame(executable)) return;
                locked = TryLockTarget(game.Directory);
                if (!locked) { UnlockerStatus(L("游戏文件夹已被另一个启动器锁定。", "The game folder is locked by another launcher.")); return; }
                SetBusy(true, "unlocker-plugin"); busy = true;
                var iniPath = Path.Combine(game.Directory, "OptiScaler.ini");
                var directory = UnlockerPlugin.DestinationDirectory(game.Directory, UnlockerPlugin.ReadIni(iniPath));
                var destination = Path.Combine(directory, name);
                var exists = File.Exists(destination);
                if (exists)
                {
                    var identical = SamePath(picker.FileName, destination) || UnlockerPlugin.Identical(picker.FileName, destination);
                    if (identical)
                    {
                        var same = L("已是同一文件，无需覆盖", "Same file already installed");
                        UnlockerStatus(same);
                        if (DialogTheme.Show(_window, same + L("。仍要强制重新复制吗？", ". Force re-copy anyway?"), L("导入 XeFG 解锁插件", "Import XeFG unlocker"), MessageBoxButton.YesNo, MessageBoxImage.Information) != MessageBoxResult.Yes) return;
                    }
                    else
                    {
                        var choice = DialogTheme.Show(_window,
                            L("目标已存在不同文件：", "A different file already exists: ") + destination + "\n\n" +
                            L("是：覆盖（旧文件先复制为 <name>.bak-<yyyyMMdd-HHmmss-fff>，再同盘原子替换）\n否：保留现有文件\n取消：取消导入", "Yes: overwrite (copy the old file to <name>.bak-<yyyyMMdd-HHmmss-fff>, then atomically replace on the same volume)\nNo: keep the existing file\nCancel: cancel import"),
                            L("覆盖解锁插件？", "Overwrite unlocker plugin?"), MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
                        if (choice != MessageBoxResult.Yes)
                        {
                            UnlockerStatus(choice == MessageBoxResult.No ? L("已保留现有插件。", "Kept the existing plugin.") : L("已取消导入。", "Import cancelled."));
                            return;
                        }
                    }
                }
                if (RefuseRunningUnlockerGame(executable)) return;
                var backup = UnlockerPlugin.CopyAtomic(picker.FileName, destination, exists);
                copied = true;
                outcome = L("已导入解锁插件：", "Unlocker imported: ") + destination;
                if (backup.Length > 0) outcome += "\n" + L("旧插件备份：", "Original plugin backup: ") + backup;
                if (UnlockerPlugin.HasMultipleUnlockers(directory)) outcome += "\n" + L("建议只保留一个帧生成解锁器（多解锁器可能冲突）", "Keep only one frame-generation unlocker (multiple unlockers can conflict).");
                var value = UnlockerPlugin.LoadAsiValue(UnlockerPlugin.ReadIni(iniPath));
                var enable = value.Length == 0 || string.Equals(value, "auto", StringComparison.OrdinalIgnoreCase);
                if (string.Equals(value, "false", StringComparison.OrdinalIgnoreCase))
                    enable = DialogTheme.Show(_window, L("游戏 INI 明确关闭了 LoadAsiPlugins。要启用 ASI 插件加载吗？", "The game INI explicitly disables LoadAsiPlugins. Enable ASI plugin loading?"), L("启用 ASI 加载？", "Enable ASI loading?"), MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes;
                if (enable)
                {
                    var iniBackup = UnlockerPlugin.EnableAsi(iniPath);
                    outcome += "\n" + L("已启用 LoadAsiPlugins。", "LoadAsiPlugins enabled.");
                    if (iniBackup.Length > 0) outcome += " " + L("INI 备份：", "INI backup: ") + iniBackup;
                }
                else if (!string.Equals(value, "true", StringComparison.OrdinalIgnoreCase))
                    outcome += "\n" + L("插件在启用 LoadAsiPlugins 前不会加载。", "The plugin will not load until LoadAsiPlugins is enabled.");
                UnlockerStatus(outcome);
            }
            catch (Exception ex)
            {
                UnlockerStatus((copied ? outcome + "\n" + L("插件已复制，但后续操作失败：", "Plugin copied, but a subsequent operation failed: ") : L("导入失败（需为 4KB–100MB 的 AMD64 PE .asi/.dll 插件）：", "Import failed (requires a 4 KB–100 MB AMD64 PE .asi/.dll plugin): ")) + ex.Message);
            }
            finally
            {
                if (locked) ReleaseLock();
                if (busy) { SetBusy(false, ""); UpdatePrimaryButtonState(); }
                RefreshProfileActions();
            }
        }
    }
}
