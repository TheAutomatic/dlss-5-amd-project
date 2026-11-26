using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Border _modelImportBar;
        TextBlock _modelImportText;
        Button _modelImportButton;
        Border _openRrBadge;
        TextBlock _openRrStatusText;
        Button _openRrImportButton;

        void InitModelImport()
        {
            _modelImportBar = _window.FindName("ModelImportBar") as Border;
            _modelImportText = _window.FindName("ModelImportText") as TextBlock;
            _modelImportButton = _window.FindName("ModelImportButton") as Button;
            _openRrBadge = _window.FindName("OpenRrBadge") as Border;
            _openRrStatusText = _window.FindName("OpenRrStatusText") as TextBlock;
            _openRrImportButton = _window.FindName("OpenRrImportButton") as Button;
            if (_modelImportButton != null) _modelImportButton.Click += delegate { PickModelSourceFile(); };
            if (_openRrImportButton != null) _openRrImportButton.Click += delegate { PickOpenRrDlssdFile(); };
            RefreshModelImportLanguage();
        }

        void RefreshModelImportLanguage()
        {
            if (_modelImportButton != null) {
                _modelImportButton.Content = L("导入 dlssnr.dll…", "Import dlssnr.dll…");
                _modelImportButton.ToolTip = L("选择 nvngx_dlssnr.dll；文件会复制到启动器目录，对所有游戏生效。", "Choose nvngx_dlssnr.dll; the file is copied into the launcher folder and applies to every game.");
            }
            SetLabel("ModelImportRequirement", "需要 nvngx_dlssnr.dll（310.9.16 或 310.8.0）", "Needs nvngx_dlssnr.dll (310.9.16 or 310.8.0)");
            if (_openRrImportButton != null)
                _openRrImportButton.Content = L("导入 nvngx_dlssd.dll…", "Import nvngx_dlssd.dll…");
            RefreshModelImportBar();
        }

        // A backend is import-fixable when a file the user can supply is the only thing missing:
        // the nvngx_dlssnr.dll source (mochizuki / lmxxf weights, daniel weights) or Daniel's own
        // setup executable. GPU-unsupported or runtime-missing-in-package states are not fixable
        // by an import and stay out of this bar.
        static bool BackendHintsContain(BackendInfo backend, params string[] needles)
        {
            if (backend == null || backend.Hints == null) return false;
            foreach (var hint in backend.Hints)
                foreach (var needle in needles)
                    if (string.Equals(hint, needle, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        static readonly System.Collections.Generic.Dictionary<string, Tuple<long, long, bool>> DlssdVersionCache =
            new System.Collections.Generic.Dictionary<string, Tuple<long, long, bool>>(StringComparer.OrdinalIgnoreCase);

        static bool IsDlssd310_9_1(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists) return false;
                var full = info.FullName;
                var len = info.Length;
                var ticks = info.LastWriteTimeUtc.Ticks;
                lock (DlssdVersionCache)
                {
                    Tuple<long, long, bool> cached;
                    if (DlssdVersionCache.TryGetValue(full, out cached) && cached.Item1 == len && cached.Item2 == ticks)
                        return cached.Item3;
                }
                var v = System.Diagnostics.FileVersionInfo.GetVersionInfo(full);
                var ok = v != null && v.FileMajorPart == 310 && v.FileMinorPart == 9 && v.FileBuildPart == 1;
                lock (DlssdVersionCache)
                {
                    if (DlssdVersionCache.Count > 64) DlssdVersionCache.Clear();
                    DlssdVersionCache[full] = Tuple.Create(len, ticks, ok);
                }
                return ok;
            }
            catch { return false; }
        }

        static readonly Dictionary<string, Tuple<long, long, bool>> DlssnrHashCache =
            new Dictionary<string, Tuple<long, long, bool>>(StringComparer.OrdinalIgnoreCase);
        string _autoScannedRoot;
        int _autoScanRunning;

        // Only auto-copy 310.9.16 nvngx_dlssnr.dll (contains both Small and Original networks)
        static bool IsDlssnr310_9_16(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            try
            {
                var fi = new FileInfo(path);
                if (!fi.Exists || fi.Length < 65536) return false;
                var full = fi.FullName;
                var len = fi.Length;
                var ticks = fi.LastWriteTimeUtc.Ticks;
                lock (DlssnrHashCache)
                {
                    Tuple<long, long, bool> cached;
                    if (DlssnrHashCache.TryGetValue(full, out cached) && cached.Item1 == len && cached.Item2 == ticks)
                        return cached.Item3;
                }
                string hex;
                using (var sha = SHA256.Create())
                using (var fs = File.OpenRead(full))
                    hex = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "").ToLowerInvariant();
                bool ok = hex == "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f";
                lock (DlssnrHashCache)
                {
                    if (DlssnrHashCache.Count > 64) DlssnrHashCache.Clear();
                    DlssnrHashCache[full] = Tuple.Create(len, ticks, ok);
                }
                return ok;
            }
            catch { return false; }
        }

        static bool IsValidMochizukiPack(string path, uint expectedEntries)
        {
            if (string.IsNullOrEmpty(path)) return false;
            try
            {
                var fi = new FileInfo(path);
                if (!fi.Exists || fi.Length <= 65536) return false;
                using (var fs = File.OpenRead(fi.FullName))
                {
                    var hdr = new byte[16];
                    if (fs.Read(hdr, 0, 16) != 16) return false;
                    return System.Text.Encoding.ASCII.GetString(hdr, 0, 8) == "NRMODEL1" &&
                           BitConverter.ToUInt32(hdr, 8) == expectedEntries &&
                           BitConverter.ToUInt32(hdr, 12) == 0;
                }
            }
            catch { return false; }
        }

        void EnsureSharedModelSources(string root)
        {
            if (string.IsNullOrEmpty(root) || _allGames == null || _allGames.Count == 0) return;
            if (string.Equals(_autoScannedRoot, root, StringComparison.OrdinalIgnoreCase)) return;
            if (System.Threading.Interlocked.CompareExchange(ref _autoScanRunning, 1, 0) != 0) return;

            var searchDirs = new System.Collections.Generic.List<string>();
            var parent = Path.GetDirectoryName(root.TrimEnd('\\', '/'));
            if (!string.IsNullOrEmpty(parent))
                searchDirs.Add(Path.Combine(parent, "weights"));
            if (_selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe))
            {
                var selDir = Path.GetDirectoryName(_selectedGame.Exe);
                if (!string.IsNullOrEmpty(selDir)) searchDirs.Add(selDir);
            }
            foreach (var g in _allGames)
            {
                if (g == null || string.IsNullOrEmpty(g.Exe)) continue;
                var gDir = Path.GetDirectoryName(g.Exe);
                if (!string.IsNullOrEmpty(gDir)) searchDirs.Add(gDir);
            }
            foreach (var ld in DlssnrLocalData.GetModelReuseDirectories()) searchDirs.Add(ld);
            var settingsDir = _settingsDir;

            System.Threading.Tasks.Task.Run(() =>
            {
                bool copiedAny = false;
                try
                {
                    var localApp = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                    if (!string.IsNullOrEmpty(localApp))
                    {
                        var legacyStore = Path.Combine(localApp, "OptScalerNR", "ModelStore");
                        if (Directory.Exists(legacyStore)) try { Directory.Delete(legacyStore, true); } catch { }
                    }
                    if (!string.IsNullOrEmpty(settingsDir))
                    {
                        var legacyModels = Path.Combine(settingsDir, "models");
                        if (Directory.Exists(legacyModels)) try { Directory.Delete(legacyModels, true); } catch { }
                    }
                    if (!Directory.Exists(Path.Combine(root, "dlssnr-amd"))) return;

                    var destDlssnr = Path.Combine(root, "nvngx_dlssnr.dll");
                    bool hasNr = File.Exists(destDlssnr);
                    var destSmallPack = Path.Combine(root, "dlssnr-amd", "CC_SMALL_FP8.bin");
                    bool hasSmall = NonEmptyFile(destSmallPack);
                    var destOrigPack = Path.Combine(root, "dlssnr-amd", "CC_SILVER_AARDWOLD.bin");
                    bool hasOrig = NonEmptyFile(destOrigPack);
                    var destDlssd = Path.Combine(root, "nvngx_dlssd.dll");
                    var destDlssdSub = Path.Combine(root, "openrr-amd", "nvngx_dlssd.dll");
                    bool hasDlssd = IsDlssd310_9_1(destDlssd) || IsDlssd310_9_1(destDlssdSub);

                    if (hasNr && hasSmall && hasOrig && hasDlssd)
                    {
                        _autoScannedRoot = root;
                        return;
                    }

                    if (!hasNr)
                    {
                        foreach (var dir in searchDirs)
                        {
                            var cand = Path.Combine(dir, "nvngx_dlssnr.dll");
                            if (IsDlssnr310_9_16(cand))
                            {
                                try { File.Copy(cand, destDlssnr, false); copiedAny = true; break; } catch { }
                            }
                        }
                    }

                    if (!hasSmall)
                    {
                        foreach (var dir in searchDirs)
                        {
                            foreach (var rel in new[] { @"dlssnr-amd\CC_SMALL_FP8.bin", "CC_SMALL_FP8.bin" })
                            {
                                var cand = Path.Combine(dir, rel);
                                if (IsValidMochizukiPack(cand, 255))
                                {
                                    try { File.Copy(cand, destSmallPack, false); copiedAny = true; break; } catch { }
                                }
                            }
                            if (copiedAny && NonEmptyFile(destSmallPack)) break;
                        }
                    }

                    if (!hasOrig)
                    {
                        foreach (var dir in searchDirs)
                        {
                            foreach (var rel in new[] { @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", "CC_SILVER_AARDWOLD.bin" })
                            {
                                var cand = Path.Combine(dir, rel);
                                if (IsValidMochizukiPack(cand, 599))
                                {
                                    try { File.Copy(cand, destOrigPack, false); copiedAny = true; break; } catch { }
                                }
                            }
                            if (NonEmptyFile(destOrigPack)) break;
                        }
                    }

                    if (!hasDlssd)
                    {
                        foreach (var dir in searchDirs)
                        {
                            foreach (var rel in new[] { "nvngx_dlssd.dll", @"openrr-amd\nvngx_dlssd.dll", @"opennr-rr\nvngx_dlssd.dll" })
                            {
                                var cand = Path.Combine(dir, rel);
                                if (IsDlssd310_9_1(cand))
                                {
                                    try { File.Copy(cand, destDlssd, true); copiedAny = true; break; } catch { }
                                }
                            }
                            if (!IsDlssd310_9_1(destDlssd))
                            {
                                var nativeCand = FindGameNativeDlssdPath(dir);
                                if (!string.IsNullOrEmpty(nativeCand) && IsDlssd310_9_1(nativeCand))
                                {
                                    try { File.Copy(nativeCand, destDlssd, true); copiedAny = true; } catch { }
                                }
                            }
                            if (IsDlssd310_9_1(destDlssd)) break;
                        }
                    }
                    _autoScannedRoot = root;
                }
                catch { }
                finally
                {
                    System.Threading.Interlocked.Exchange(ref _autoScanRunning, 0);
                }

                if (copiedAny && !_closed && !_window.Dispatcher.HasShutdownStarted)
                {
                    _window.Dispatcher.BeginInvoke(new Action(() =>
                    {
                        if (_closed || _writeBusy) return;
                        RefreshModelImportBar();
                        if (_selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
                    }));
                }
            });
        }
        bool HasValidOpenRrDlssd()
        {
            var root = ResolveImportRoot();
            if (!string.IsNullOrEmpty(root))
            {
                EnsureSharedModelSources(root);
                if (IsDlssd310_9_1(Path.Combine(root, "nvngx_dlssd.dll"))) return true;
                if (IsDlssd310_9_1(Path.Combine(root, "openrr-amd", "nvngx_dlssd.dll")) || IsDlssd310_9_1(Path.Combine(root, "opennr-rr", "nvngx_dlssd.dll"))) return true;
            }
            if (_selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe))
            {
                var gameDir = Path.GetDirectoryName(_selectedGame.Exe);
                if (!string.IsNullOrEmpty(gameDir))
                {
                    if (IsDlssd310_9_1(Path.Combine(gameDir, "openrr-amd", "nvngx_dlssd.dll")) || IsDlssd310_9_1(Path.Combine(gameDir, "opennr-rr", "nvngx_dlssd.dll"))) return true;
                    if (IsDlssd310_9_1(Path.Combine(gameDir, "nvngx_dlssd.dll"))) return true;
                }
            }
            return false;
        }
        bool SelectedBackendNeedsImport(out string message)
        {
            message = "";
            if (_lastInspectResult == null || _lastInspectResult.Backends == null) return false;
            var wanted = "";
            if (_mochiCheck != null && _mochiCheck.IsChecked == true && BackendHintsContain(FindBackend("mochizuki"), "mochi-source", "mochi-unsupported-source"))
                wanted += (wanted.Length == 0 ? "" : " / ") + L("nvngx_dlssnr.dll（Mochizuki 模型源，310.9.16 或 310.8.0）", "nvngx_dlssnr.dll (Mochizuki model source, 310.9.16 or 310.8.0)");
            if (IsOptiScalerSelectedMode() && !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true && !HasValidOpenRrDlssd())
                wanted += (wanted.Length == 0 ? "" : " / ") + L("nvngx_dlssd.dll（OpenRR 光线重建模型源，310.9.1.0）", "nvngx_dlssd.dll (OpenRR model source, 310.9.1.0)");
            if (wanted.Length == 0) return false;
            message = L("缺少模型源：", "Missing model source: ") + wanted +
                      L("。点击右侧按钮导入，文件会复制到启动器目录，对所有游戏生效。", ". Import it with the button on the right; the file is copied into the launcher folder and applies to every game.");
            return true;
        }

        BackendInfo FindBackend(string id)
        {
            if (_lastInspectResult == null || _lastInspectResult.Backends == null) return null;
            foreach (var backend in _lastInspectResult.Backends)
                if (backend != null && string.Equals(backend.Id, id, StringComparison.OrdinalIgnoreCase)) return backend;
            return null;
        }

        void RefreshOpenRrCard()
        {
            if (_openRrStatusText == null || _openRrBadge == null) return;
            var enabled = !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true;
            var hasDlssd = HasValidOpenRrDlssd();
            bool isPlaceholderAsi = false;
            try {
                var root = ResolveImportRoot();
                var asiPath = Path.Combine(root, "OptiScaler", "plugins", "openrr-amd.asi");
                if (!File.Exists(asiPath)) asiPath = Path.Combine(root, "OptiScaler", "plugins", "openrr.asi");
                if (File.Exists(asiPath) && new FileInfo(asiPath).Length == 0) isPlaceholderAsi = true;
            } catch { }
            if (!enabled)
            {
                _openRrStatusText.Text = L("已关闭", "Off");
                _openRrStatusText.ToolTip = null;
                _openRrStatusText.Foreground = MissingBrush;
            }
            else if (hasDlssd)
            {
                _openRrStatusText.Text = isPlaceholderAsi ? L("占位符待替换", "Placeholder ASI") : L("就绪", "Ready");
                _openRrStatusText.ToolTip = isPlaceholderAsi ? L("openrr.asi 是占位文件，请替换。", "openrr.asi is a placeholder; replace it.") : null;
                _openRrStatusText.Foreground = isPlaceholderAsi ? WarningBrush : ReadyBrush;
            }
            else
            {
                _openRrStatusText.Text = L("缺少 nvngx_dlssd.dll (310.9.1.0)", "Missing nvngx_dlssd.dll (310.9.1.0)");
                _openRrStatusText.ToolTip = isPlaceholderAsi
                    ? L("openrr.asi 是占位文件，请替换后导入 nvngx_dlssd.dll (310.9.1.0)。", "openrr.asi is a placeholder; replace it and import nvngx_dlssd.dll (310.9.1.0).")
                    : L("需要 nvngx_dlssd.dll (310.9.1.0)，可点击下方导入或放入启动器目录。", "Requires nvngx_dlssd.dll (310.9.1.0); import below or place in the launcher folder.");
                _openRrStatusText.Foreground = WarningBrush;
            }
            if (_openRrImportButton != null)
            {
                _openRrImportButton.Visibility = (enabled && !hasDlssd) ? Visibility.Visible : Visibility.Collapsed;
                _openRrImportButton.IsEnabled = PresentationIdle;
            }
            bool isRtxMc = IsRtxRemixMinecraftSelected();
            bool optiActive = !isRtxMc && _optiPatcherToggle != null && _optiPatcherToggle.IsEnabled && _optiPatcherToggle.IsChecked == true;
            bool wantAutoSpoof = !isRtxMc && optiActive && GetOpenRrSpoof(_selectedGame) == "auto";
            _decisions.OpenRrSpoof = wantAutoSpoof ? "auto" : "full";
            if (_openRrAutoSpoofToggle != null)
            {
                _openRrAutoSpoofToggle.Visibility = (enabled && !isRtxMc) ? Visibility.Visible : Visibility.Collapsed;
                _openRrAutoSpoofToggle.IsEnabled = PresentationIdle && enabled && optiActive;
                _openRrAutoSpoofToggle.Content = L("实验性：减少全局显卡伪装", "Experimental: Reduce global GPU spoofing");
                _selectingOpenRrSpoof = true;
                try { _openRrAutoSpoofToggle.IsChecked = wantAutoSpoof; }
                finally { _selectingOpenRrSpoof = false; }
                _openRrAutoSpoofToggle.ToolTip = optiActive
                    ? L(
                        "勾选时将 [Spoofing] Dxgi/Vulkan 设为 auto 而非强开 true：在 OptiPatcher 命中的游戏中自动关闭全局 DXGI N 卡伪装，保留真实 AMD 显卡身份，同时通过 OpenRR 的 sl.dlss_d 专用能力适配解锁光线重建；未命中时 OptiScaler 会自动回退开启 DXGI 伪装。",
                        "Sets [Spoofing] Dxgi/Vulkan to auto instead of forcing true: when OptiPatcher patches the game, global DXGI NVIDIA spoofing turns off so the game sees the native AMD adapter while OpenRR's sl.dlss_d capability hook unlocks Ray Reconstruction; if unpatched, OptiScaler falls back to DXGI spoofing.")
                    : L(
                        "需先启用 OptiPatcher 才能减少全局显卡伪装（当前游戏不支持或未勾选 OptiPatcher，自动保持完整伪装）。",
                        "Requires OptiPatcher enabled to reduce global GPU spoofing (OptiPatcher is unsupported or unchecked; full spoofing stays active).");
            }
            RefreshGpuHints();
            RefreshOpenRrHotkeyHint(enabled && hasDlssd && !isPlaceholderAsi);
        }
        void RefreshModelImportBar()
        {
            RefreshOpenRrCard();
            if (_modelImportBar == null) return;
            string message = null;
            var backend = FindBackend("mochizuki");
            var visible = _selectedGame != null && _selectedGame.HasExecutable && IsBackendSelected("mochizuki") && backend != null && BackendHintsContain(backend, "mochi-source", "mochi-unsupported-source");
            _modelImportBar.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (_modelImportText != null) {
                _modelImportText.Text = backend != null && BackendHintsContain(backend, "mochi-unsupported-source") ? L("更换不支持的 Mochizuki 模型源", "Replace unsupported Mochizuki model source") : L("缺少 Mochizuki 模型源", "Mochizuki model source missing");
                if (visible) SelectedBackendNeedsImport(out message);
                _modelImportText.ToolTip = message;
            }
            if (_modelImportButton != null) _modelImportButton.IsEnabled = visible && PresentationIdle;
            RefreshInstallChecklist();
        }

        bool _pendingActionsWarned;
        string _pendingWarningContext;

        void ApplyPendingActionsWarning(bool pulse)
        {
            var card = (Border)_window.FindName("PendingActionsCard");
            card.SetResourceReference(Border.BackgroundProperty, _pendingActionsWarned ? "ThemeDangerBg" : "ThemeCardBg");
            card.SetResourceReference(Border.BorderBrushProperty, _pendingActionsWarned ? "ThemeDanger" : "ThemeHairline");
            if (_pendingActionsWarned) card.BorderThickness = new Thickness(1);
            else card.SetResourceReference(Border.BorderThicknessProperty, "ThemeCardBorderThickness");
            var ink = LauncherTheme.Brush(_pendingActionsWarned ? "ThemeDanger" : "ThemeInk");
            foreach (DockPanel row in ((StackPanel)_window.FindName("MissingPackageItems")).Children)
                foreach (var child in row.Children) {
                    var title = child as TextBlock;
                    if (title != null) title.Foreground = ink;
                }
            _modelImportText.Foreground = ink;
            ((TextBlock)_window.FindName("ModelImportRequirement")).Foreground = LauncherTheme.Brush(_pendingActionsWarned ? "ThemeDanger" : "ThemeMuted");
            ((TextBlock)_window.FindName("OpenRrModelImportText")).Foreground = ink;
            if (!_pendingActionsWarned) card.BeginAnimation(UIElement.OpacityProperty, null);
            else if (pulse) card.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(1, 0.4, TimeSpan.FromSeconds(0.25)) {
                AutoReverse = true, RepeatBehavior = new RepeatBehavior(3), FillBehavior = FillBehavior.Stop
            });
        }

        bool WarnPendingActions()
        {
            RefreshModelImportBar();
            var pending = (Border)_window.FindName("BeforeInstallChecklist");
            if (pending.Visibility != Visibility.Visible) return false;
            _pendingActionsWarned = true;
            _pendingWarningContext = ModeKey() + "|" + GetSelectedInstallMode();
            ApplyPendingActionsWarning(true);
            pending.UpdateLayout();
            pending.BringIntoView();
            return true;
        }

        void RefreshInstallChecklist()
        {
            var items = (StackPanel)_window.FindName("MissingPackageItems");
            items.Children.Clear();
            var missing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var result = _lastInspectResult;
            if (_selectedGame != null && _selectedGame.HasExecutable && result != null) {
                if (string.IsNullOrEmpty(result.PackageRoot)) missing.Add(L("缺少安装包", "Installation package missing"));
                else if (result.Backends != null) foreach (var backend in result.Backends)
                    if (backend != null && IsBackendSelected(backend.Id)) foreach (var file in backend.MissingPackageFiles ?? new string[0])
                        missing.Add(L("缺少组件：", "Missing component: ") + file);
            }
            foreach (var text in missing) {
                var row = new DockPanel { Margin = new Thickness(0, 4, 0, 4) };
                var button = new Button { Content = L("获取完整安装包", "Get full package"), Style = (Style)_window.FindResource("GhostButton"), Height = 30, Margin = new Thickness(12, 0, 0, 0), IsEnabled = PresentationIdle };
                DockPanel.SetDock(button, Dock.Right);
                button.Click += delegate { OpenUrl("https://github.com/TheAutomatic/dlss-5-amd-project/releases/latest"); };
                row.Children.Add(button);
                row.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = LauncherTheme.Brush("ThemeInk"), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
                items.Children.Add(row);
            }
            bool openRrMissing = _selectedGame != null && _selectedGame.HasExecutable && IsOptiScalerSelectedMode() && !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true && !HasValidOpenRrDlssd();
            ((DockPanel)_window.FindName("OpenRrModelImportRow")).Visibility = openRrMissing ? Visibility.Visible : Visibility.Collapsed;
            SetLabel("OpenRrModelImportText", "缺少 OpenRR 模型源（需要 nvngx_dlssd.dll 310.9.1.0）", "OpenRR model source missing (needs nvngx_dlssd.dll 310.9.1.0)");
            ((TextBlock)_window.FindName("OpenRrModelImportText")).ToolTip = "nvngx_dlssd.dll 310.9.1.0";
            bool locked = _selectedGame != null && _selectedGame.HasExecutable && IsOpenRrUnsupportedLocked();
            ((Border)_window.FindName("OpenRrUnsupportedBar")).Visibility = locked ? Visibility.Visible : Visibility.Collapsed;
            SetLabel("BeforeInstallHeading", "还差这几步", "Before you install");
            bool blocking = missing.Count > 0 || _modelImportBar.Visibility == Visibility.Visible || openRrMissing;
            if (_pendingWarningContext != ModeKey() + "|" + GetSelectedInstallMode() || (!blocking && result != null))
                _pendingActionsWarned = false;
            ((Border)_window.FindName("BeforeInstallChecklist")).Visibility = blocking ? Visibility.Visible : Visibility.Collapsed;
            ApplyPendingActionsWarning(false);
        }

        void PickOpenRrDlssdFile()
        {
            if (IniBusyLike()) return;
            var directory = _selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe) ? Path.GetDirectoryName(_selectedGame.Exe) : null;
            var picker = new OpenFileDialog {
                Title = L("选择 nvngx_dlssd.dll (310.9.1.0) — 复制到启动器目录供所有游戏使用", "Choose nvngx_dlssd.dll (310.9.1.0) — copied into launcher folder for all games"),
                Filter = "nvngx_dlssd.dll (310.9.1.0)|nvngx_dlssd.dll|DLL (*.dll)|*.dll",
                CheckFileExists = true, RestoreDirectory = true
            };
            if (!string.IsNullOrEmpty(directory)) picker.InitialDirectory = directory;
            if (picker.ShowDialog(_window) != true) return;
            try {
                var source = picker.FileName;
                if (!string.Equals(Path.GetFileName(source), "nvngx_dlssd.dll", StringComparison.OrdinalIgnoreCase))
                    throw new IOException(L("请选择名为 nvngx_dlssd.dll 的文件。", "Please choose a file named nvngx_dlssd.dll."));
                ValidateModelSourcePe(source);
                var root = ResolveImportRoot();
                var destination = Path.Combine(root, "nvngx_dlssd.dll");
                if (!string.Equals(Path.GetFullPath(source), Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase)) {
                    Directory.CreateDirectory(root);
                    File.Copy(source, destination, true);
                }
                if (_statusText != null) SetStatus("已导入 nvngx_dlssd.dll (310.9.1.0)，OpenRR 就绪。", "Imported nvngx_dlssd.dll (310.9.1.0); OpenRR is ready.");
                RefreshModelImportBar();
                if (_selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
            } catch (Exception ex) {
                DialogTheme.Show(_window, L("导入失败：", "Import failed: ") + ex.Message, _window.Title);
            }
        }

        OpenFileDialog CreateMochizukiSourcePicker()
        {
            var directory = _selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe) ? Path.GetDirectoryName(_selectedGame.Exe) : null;
            var picker = new OpenFileDialog {
                Title = L("选择 nvngx_dlssnr.dll（复制到启动器目录）", "Choose nvngx_dlssnr.dll (copied into the launcher folder)"),
                Filter = "nvngx_dlssnr.dll|nvngx_dlssnr.dll",
                CheckFileExists = true, RestoreDirectory = true
            };
            if (!string.IsNullOrEmpty(directory)) picker.InitialDirectory = directory;
            return picker;
        }

        void PickModelSourceFile()
        {
            if (IniBusyLike()) return;
            var picker = CreateMochizukiSourcePicker();
            if (picker.ShowDialog(_window) != true) return;
            try {
                var source = picker.FileName;
                if (!string.Equals(Path.GetFileName(source), "nvngx_dlssnr.dll", StringComparison.OrdinalIgnoreCase))
                    throw new IOException(L("请选择名为 nvngx_dlssnr.dll 的文件。", "Please choose a file named nvngx_dlssnr.dll."));
                ValidateModelSourcePe(source);
                var root = ResolveImportRoot();
                var destination = Path.Combine(root, Path.GetFileName(source));
                if (string.Equals(Path.GetFullPath(source), Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase)) {
                    RefreshModelImportBar();
                    return;
                }
                if (File.Exists(destination) && !SameFileContent(source, destination)) {
                    var name = Path.GetFileName(destination);
                    if (DialogTheme.Show(_window,
                            string.Format(L("启动器目录已有同名文件：{0}。覆盖吗？", "The launcher folder already has '{0}'. Overwrite it?"), name),
                            _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
                }
                Directory.CreateDirectory(root);
                File.Copy(source, destination, true);
                if (_statusText != null) SetStatus(string.Format("已导入 {0}，正在重新检查后端…", Path.GetFileName(source)), string.Format("Imported {0}; rechecking backends…", Path.GetFileName(source)));
                if (_selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
            } catch (Exception ex) {
                DialogTheme.Show(_window, L("导入失败：", "Import failed: ") + ex.Message, _window.Title);
            }
        }

        // The importer's own package root: the worker inspects this folder for source DLLs, so an
        // imported file here is visible to every later install without per-run decisions.
        string ResolveImportRoot()
        {
            if (!string.IsNullOrEmpty(_packageRoot) && Directory.Exists(_packageRoot)) return _packageRoot;
            return AppDomain.CurrentDomain.BaseDirectory;
        }

        void ValidateModelSourcePe(string path)
        {
            using (var stream = File.OpenRead(path))
            using (var reader = new BinaryReader(stream)) {
                if (stream.Length < 4096 || stream.Length > 300L * 1024 * 1024) throw new IOException(L("文件大小不合理。", "Implausible file size."));
                var dos = reader.ReadUInt16();
                if (dos != 0x5A4D) throw new IOException(L("缺少 MZ 头，不是有效的 PE 文件。", "Missing MZ header; not a valid PE file."));
                stream.Seek(0x3C, SeekOrigin.Begin);
                var offset = reader.ReadInt32();
                if (offset <= 0 || offset + 6 > stream.Length) throw new IOException(L("PE 头偏移无效。", "Invalid PE header offset."));
                stream.Seek(offset, SeekOrigin.Begin);
                if (reader.ReadUInt32() != 0x00004550) throw new IOException(L("缺少 PE 签名。", "Missing PE signature."));
                var machine = reader.ReadUInt16();
                if (machine != 0x8664) throw new IOException(L("不是 x64 文件。", "Not an AMD64 image."));
            }
            if (string.Equals(Path.GetFileName(path), "nvngx_dlssd.dll", StringComparison.OrdinalIgnoreCase) && !IsDlssd310_9_1(path))
                throw new IOException(L("OpenRR 需要 nvngx_dlssd.dll 310.9.1.0 版本。", "OpenRR requires nvngx_dlssd.dll version 310.9.1.0."));
        }

        static bool SameFileContent(string a, string b)
        {
            var ha = ShaOf(a); var hb = ShaOf(b);
            return string.Equals(ha, hb, StringComparison.OrdinalIgnoreCase);
        }

        static string ShaOf(string path)
        {
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
        }

        bool IniBusyLike()
        {
            return _writeBusy || _folderScan != null || (_session != null && _session.IsRunning);
        }

        int _fastInspectGeneration;

        sealed class FastXessFileEntry
        {
            public bool Present, Valid;
            public string Hash = "", Version = "";
        }

        static readonly System.Collections.Generic.Dictionary<string, Tuple<long, long, FastXessFileEntry>> FastXessCache =
            new System.Collections.Generic.Dictionary<string, Tuple<long, long, FastXessFileEntry>>(StringComparer.OrdinalIgnoreCase);

        static readonly KeyValuePair<string, string[]>[] FastXessGroupDefs = new[]
        {
            new KeyValuePair<string, string[]>("sr", new[] { "libxess.dll" }),
            new KeyValuePair<string, string[]>("dx11", new[] { "libxess_dx11.dll" }),
            new KeyValuePair<string, string[]>("fg", new[] { "libxess_fg.dll", "libxell.dll" }),
            new KeyValuePair<string, string[]>("fsr_fg", new[] { "amd_fidelityfx_framegeneration_dx12.dll" }),
            new KeyValuePair<string, string[]>("fsr_upscaler", new[] { "amd_fidelityfx_upscaler_dx12.dll" }),
            new KeyValuePair<string, string[]>("fsr_loader", new[] { "amd_fidelityfx_loader_dx12.dll" }),
            new KeyValuePair<string, string[]>("fsr_vk", new[] { "amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_framegeneration_vk.dll" })
        };

        static bool HasReparseAncestor(string path)
        {
            var current = Path.GetFullPath(path);
            while (!string.IsNullOrEmpty(current))
            {
                if (File.Exists(current) || Directory.Exists(current))
                {
                    try
                    {
                        if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) return true;
                    }
                    catch { return true; }
                }
                var parent = Path.GetDirectoryName(current);
                if (string.Equals(parent, current, StringComparison.OrdinalIgnoreCase)) break;
                current = parent;
            }
            return false;
        }

        static FastXessFileEntry GetFastXessFile(string path)
        {
            var entry = new FastXessFileEntry();
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return entry;
            var fi = new FileInfo(path);
            var full = fi.FullName;
            var len = fi.Length;
            var ticks = fi.LastWriteTimeUtc.Ticks;
            lock (FastXessCache)
            {
                Tuple<long, long, FastXessFileEntry> cached;
                if (FastXessCache.TryGetValue(full, out cached) && cached.Item1 == len && cached.Item2 == ticks)
                    return cached.Item3;
            }
            entry.Present = true;
            entry.Hash = ShaOf(full).ToLowerInvariant();
            using (var stream = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new BinaryReader(stream))
            {
                if (stream.Length >= 64 && reader.ReadUInt16() == 0x5a4d)
                {
                    stream.Position = 60;
                    var offset = reader.ReadUInt32();
                    if (offset >= 64 && offset <= stream.Length - 26)
                    {
                        stream.Position = offset;
                        var sig = reader.ReadUInt32();
                        var machine = reader.ReadUInt16();
                        stream.Position = offset + 24;
                        entry.Valid = sig == 0x4550 && machine == 0x8664 && reader.ReadUInt16() == 0x20b;
                    }
                }
            }
            try { entry.Version = System.Diagnostics.FileVersionInfo.GetVersionInfo(full).FileVersion ?? ""; }
            catch { entry.Version = ""; }
            lock (FastXessCache)
            {
                if (FastXessCache.Count > 256) FastXessCache.Clear();
                FastXessCache[full] = Tuple.Create(len, ticks, entry);
            }
            return entry;
        }

        static XessCompatibility ComputeFastXessCompatibility(string gameDir, string pkgRoot)
        {
            var groups = new System.Collections.Generic.List<XessGroup>(FastXessGroupDefs.Length);
            var identity = new System.Collections.Generic.List<string>(16);
            foreach (var def in FastXessGroupDefs)
            {
                var files = new System.Collections.Generic.List<XessFile>(def.Value.Length);
                bool ready = !string.IsNullOrEmpty(pkgRoot);
                bool different = false;
                foreach (var name in def.Value)
                {
                    var game = GetFastXessFile(Path.Combine(gameDir, name));
                    var pkg = string.IsNullOrEmpty(pkgRoot) ? new FastXessFileEntry() : GetFastXessFile(Path.Combine(pkgRoot, "OptiScaler", name));
                    ready = ready && pkg.Valid;
                    var state = !game.Present ? "missing" : (game.Hash == pkg.Hash ? "same" : "different");
                    if (state != "same") different = true;
                    files.Add(new XessFile { Name = name, GameVersion = game.Version, PackageVersion = pkg.Version, State = state });
                    identity.Add(name + "|" + game.Hash + "|" + pkg.Hash);
                }
                groups.Add(new XessGroup { Id = def.Key, Ready = ready, Different = different, Files = files.ToArray() });
            }
            identity.Add(Path.GetFullPath(gameDir));
            string token;
            using (var sha = SHA256.Create())
            {
                var bytes = System.Text.Encoding.UTF8.GetBytes(string.Join("\n", identity.ToArray()));
                token = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
            }
            return new XessCompatibility
            {
                Groups = groups.ToArray(),
                Active = false,
                Fingerprint = token,
                Evidence = new string[0],
                RestoreFiles = new string[0]
            };
        }

        static string ComputeDirFingerprint(string exe, string dir)
        {
            var parts = new System.Collections.Generic.List<string>();
            var checkPaths = new System.Collections.Generic.List<string> { exe };
            foreach (var c in ProxyRecommendations.ProxyNames) checkPaths.Add(Path.Combine(dir, c));
            foreach (var extra in new[] { "OptiScaler.dll", "remixbridge.dll", @"dlssnr-amd-backup\remixbridge.dll", "d3d9.dll", "vulkan-1.dll", "dlssnr-amd.asi", "dlssnr_amd.addon64", "ReShade64.dll", "dlssnr_amd.addon32", "ReShade32.dll", "amd-presr-install.txt", "dlssnr-amd-install.txt" })
                checkPaths.Add(Path.Combine(dir, extra));
            foreach (var p in checkPaths)
            {
                if (File.Exists(p))
                {
                    try
                    {
                        var fi = new FileInfo(p);
                        parts.Add(string.Format(System.Globalization.CultureInfo.InvariantCulture, "{0}|{1}|{2}", fi.Name, fi.Length, fi.LastWriteTimeUtc.Ticks));
                    }
                    catch { parts.Add(Path.GetFileName(p) + "|protected"); }
                }
            }
            using (var sha = SHA256.Create())
            {
                var bytes = System.Text.Encoding.UTF8.GetBytes(string.Join(";", parts.ToArray()));
                return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
            }
        }

        static bool NonEmptyFile(string path)
        {
            try { return File.Exists(path) && new FileInfo(path).Length > 0; }
            catch { return false; }
        }

        static string FindFirstNonEmpty(string root, params string[] relatives)
        {
            if (string.IsNullOrEmpty(root)) return null;
            foreach (var rel in relatives)
            {
                var cand = Path.Combine(root, rel);
                if (NonEmptyFile(cand)) return cand;
            }
            return null;
        }

        static BackendInfo[] ComputeFastBackends(string pkgRoot, string gameDir, string modelSourceDll)
        {
            string mochiAsi = null;
            if (!string.IsNullOrEmpty(pkgRoot))
            {
                foreach (var rel in new[] { @"OptiScaler\plugins\dlssnr-amd.asi", "dlssnr-amd.asi" })
                {
                    var cand = Path.Combine(pkgRoot, rel);
                    if (File.Exists(cand)) { mochiAsi = cand; break; }
                }
            }
            var missingPkg = new System.Collections.Generic.List<string>();
            if (mochiAsi == null) missingPkg.Add("OptiScaler/plugins/dlssnr-amd.asi");
            else
            {
                if (!Directory.Exists(Path.Combine(pkgRoot, @"dlssnr-amd\shaders"))) missingPkg.Add("dlssnr-amd/shaders");
                if (!Directory.Exists(Path.Combine(pkgRoot, @"dlssnr-amd\aco"))) missingPkg.Add("dlssnr-amd/aco");
                if (!File.Exists(Path.Combine(pkgRoot, @"model-tools\dlssnr_extract_model.exe"))) missingPkg.Add("model-tools/dlssnr_extract_model.exe");
            }
            var localDirs = DlssnrLocalData.GetModelReuseDirectories();
            Func<string[], string> findLocalReuse = delegate(string[] rels) {
                foreach (var ld in localDirs) {
                    var found = FindFirstNonEmpty(ld, rels);
                    if (found != null) return found;
                }
                return null;
            };
            var mochiModel = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", @"dlssnr-amd\dlssnr.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", @"dlssnr-amd\dlssnr.bin")
                ?? findLocalReuse(new[] { "CC_SILVER_AARDWOLD.bin", @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", "dlssnr.bin" });
            var mochiSmall = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SMALL_FP8.bin", @"dlssnr-amd\dlssnr-small.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SMALL_FP8.bin", @"dlssnr-amd\dlssnr-small.bin")
                ?? findLocalReuse(new[] { "CC_SMALL_FP8.bin", @"dlssnr-amd\CC_SMALL_FP8.bin", "dlssnr-small.bin" });
            var mochiExperimental = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin", @"dlssnr-amd\dlssnr-small-experimental.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin", @"dlssnr-amd\dlssnr-small-experimental.bin")
                ?? findLocalReuse(new[] { "CC_SMALL_FP8_EXPERIMENTAL.bin", @"dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin" });
            var mochiInt4 = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin", @"dlssnr-amd\dlssnr-int4.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin", @"dlssnr-amd\dlssnr-int4.bin")
                ?? findLocalReuse(new[] { "CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin", @"dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin" });
            string mochiNv = null;
            if (!string.IsNullOrEmpty(modelSourceDll) && File.Exists(modelSourceDll)) mochiNv = modelSourceDll;
            else if (!string.IsNullOrEmpty(pkgRoot) && File.Exists(Path.Combine(pkgRoot, "nvngx_dlssnr.dll"))) mochiNv = Path.Combine(pkgRoot, "nvngx_dlssnr.dll");
            else if (!string.IsNullOrEmpty(gameDir) && File.Exists(Path.Combine(gameDir, "nvngx_dlssnr.dll"))) mochiNv = Path.Combine(gameDir, "nvngx_dlssnr.dll");
            else mochiNv = findLocalReuse(new[] { "nvngx_dlssnr.dll" });

            bool canRuntime = missingPkg.Count == 0;
            bool hasModel = mochiModel != null || mochiSmall != null;
            bool preparable = !hasModel && mochiNv != null;
            bool origReady = mochiModel != null;
            bool smallReady = mochiSmall != null;
            bool expReady = mochiExperimental != null;
            bool int4Ready = mochiInt4 != null;
            bool hasNv = mochiNv != null;
            bool hasDelta = !string.IsNullOrEmpty(pkgRoot) && File.Exists(Path.Combine(pkgRoot, @"experimental\dlssnr-small-experimental.delta"));
            bool hasInt4Data = !string.IsNullOrEmpty(pkgRoot) &&
                (Directory.Exists(Path.Combine(pkgRoot, @"dlssnr-amd\int4-ultra")) || Directory.Exists(Path.Combine(pkgRoot, @"dlssnr-amd\int4")));
            bool hasInt4Tool = !string.IsNullOrEmpty(pkgRoot) && File.Exists(Path.Combine(pkgRoot, @"model-tools\dlssnr-int4-weights.exe"));
            bool expPreparable = !expReady && (smallReady || hasNv) && hasDelta;
            bool int4Preparable = !int4Ready && (origReady || hasNv) && hasInt4Data && hasInt4Tool;

            var hints = new System.Collections.Generic.List<string>();
            if (!hasModel)
            {
                if (preparable) hints.Add("mochi-preparable");
                else { hints.Add("mochi-source"); hints.Add("mochi-versions"); }
            }

            var mochi = new BackendInfo
            {
                Id = "mochizuki",
                OriginalState = origReady ? "ready" : hasNv ? "preparable" : "missing",
                SmallState = smallReady ? "ready" : hasNv ? "preparable" : "missing",
                ExperimentalState = expReady ? "ready" : expPreparable ? "preparable" : "missing",
                Int4State = int4Ready ? "ready" : int4Preparable ? "preparable" : "missing",
                PreparationState = !canRuntime ? "missing-runtime" : hasModel ? "ready" : hasNv ? "preparable" : "missing-helper",
                RuntimeState = canRuntime ? "ready" : "missing",
                ModelState = hasModel ? "ready" : preparable ? "preparable" : "missing",
                Enabled = canRuntime && hasModel,
                ReasonCode = !canRuntime ? "missing-runtime" : hasModel ? "ready" : preparable ? "preparable" : "missing-model",
                Reason = !canRuntime ? "Runtime missing" : hasModel ? "Ready" : preparable ? "Model preparation required" : "Model missing",
                Hints = hints.ToArray(),
                MissingPackageFiles = missingPkg.ToArray()
            };
            return new[] { mochi };
        }

        bool TryFastInspect(GameInfo game, string fastProxy, bool preserveCompletionStatus)
        {
            if (game == null || !game.HasExecutable || !HasLocalPackage()) return false;
            var pkgRoot = _packageRoot;
            if (string.IsNullOrEmpty(pkgRoot) || !File.Exists(Path.Combine(pkgRoot, "xess-compat.ps1"))) return false;
            string exe;
            string gameDir;
            try
            {
                exe = GamePaths.Normalize(game.Exe);
                gameDir = Path.GetDirectoryName(exe);
                if (string.IsNullOrEmpty(gameDir) || !Directory.Exists(gameDir)) return false;
                if (HasReparseAncestor(gameDir) || HasReparseAncestor(pkgRoot)) return false;
                // Let the full worker script handle active XeSS repair journals or foreign Mochizuki installs.
                if (File.Exists(Path.Combine(gameDir, "OptiScaler-compat-backup", "active.json"))) return false;
                bool isOurOptiNr = false;
                var markPath = Path.Combine(gameDir, "amd-presr-install.txt");
                if (File.Exists(markPath))
                {
                    try
                    {
                        var markText = File.ReadAllText(markPath, System.Text.Encoding.UTF8);
                        isOurOptiNr = markText.IndexOf("project=OptScaler(NR)", StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    catch { }
                }
                foreach (var marker in new[] { "dlssnr_core.dll", "OptiScaler.asi" })
                    if (File.Exists(Path.Combine(gameDir, marker))) return false;
                if (!isOurOptiNr && (File.Exists(Path.Combine(gameDir, "dlssnr_amd.addon64")) ||
                    File.Exists(Path.Combine(gameDir, "dlssnr_amd.addon32")))) return false;
            }
            catch { return false; }

            var gen = ++_fastInspectGeneration;
            var sourceDll = _decisions != null ? _decisions.ModelSourceDll : null;
            EnsureSharedModelSources(pkgRoot);
            var peInfo = PeProbe.AnalyzeTarget(exe);
            _lastPeInfo = peInfo;
            var recommend = !string.IsNullOrEmpty(fastProxy) ? fastProxy : PeProbe.QuickRecommend(exe);
            var immediateResult = new LauncherResult
            {
                Schema = 1,
                RequestId = "fast-" + gen,
                Status = "success",
                EffectiveGameDir = gameDir,
                ResolvedProxy = recommend,
                Recommend = recommend,
                PackageRoot = pkgRoot,
                Fingerprint = ComputeDirFingerprint(exe, gameDir),
                Backends = ComputeFastBackends(pkgRoot, gameDir, sourceDll),
                InstalledMode = peInfo.InstalledMode,
                RecommendedMode = peInfo.RecommendedMode,
                RecommendedReshadeApi = peInfo.RecommendedReshadeApi,
                DetectedApi = peInfo.DetectedApi,
                RequiresAdmin = DlssnrLocalData.IsProtectedDirectory(gameDir)
            };
            if (_progress != null && (_session == null || !_session.IsRunning)) _progress.IsIndeterminate = false;
            if (_session == null || !_session.IsRunning) SetBusy(false, "");
            if (_statusLed != null) _statusLed.Fill = ReadyBrush;
            if (!preserveCompletionStatus && _statusText != null && (_session == null || !_session.IsRunning))
                SetStatus("", "");
            ApplyInspect(immediateResult, pkgRoot);
            var resume = _resumeInstallAction;
            _resumeInstallAction = "";
            if (!string.IsNullOrEmpty(resume))
            {
                ExecuteAction(resume);
                return true;
            }

            System.Threading.Tasks.Task.Run(() => ComputeFastXessCompatibility(gameDir, pkgRoot)).ContinueWith(t =>
            {
                if (_closed || _window.Dispatcher.HasShutdownStarted || t.IsFaulted || t.Result == null) return;
                _window.Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (_closed || gen != _fastInspectGeneration || _writeBusy) return;
                    if (_selectedGame == null || !_selectedGame.HasExecutable || !SamePath(exe, _selectedGame.Exe)) return;
                    if (!string.Equals(ComparisonPath(pkgRoot), ComparisonPath(_packageRoot), StringComparison.OrdinalIgnoreCase)) return;
                    if (_lastInspectResult != null)
                    {
                        _lastInspectResult.Compatibility = t.Result;
                        UpdateCompatibilitySummary();
                    }
                }));
            });
            return true;
        }
    }
}
