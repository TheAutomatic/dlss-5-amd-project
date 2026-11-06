using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        string _theme = "auto";
        bool _closed, _presentationBusy, _hardwareStarted, _hardwareComplete;
        string _presentationAction = "", _hardwareDetails, _hardwareWindowsDriver;
        int _packageComparisonGeneration;
        string _comparisonPackageRoot;
        string[] _comparisonGamePaths;
        bool _comparisonWorkerRunning, _comparisonWriteBusy;
        PackageComparisonRequest _pendingPackageComparison;
        readonly Dictionary<string, bool> _packageDifferences = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        ComboBox _themeCombo;
        Button _themeToggle;
        static readonly Brush HeroMutedBrush = new SolidColorBrush(Color.FromRgb(204, 213, 228));
        void InitPresentation()
        {
            var hero = (Border)_window.FindName("HeroCard");
            hero.SizeChanged += delegate {
                hero.Clip = new RectangleGeometry(new Rect(0, 0, hero.ActualWidth, hero.ActualHeight), 18, 18);
            };
            _themeCombo = (ComboBox)_window.FindName("ThemeCombo");
            _themeToggle = (Button)_window.FindName("ThemeToggle");
            foreach (var id in new[] { "auto", "light", "dark" })
                _themeCombo.Items.Add(new ComboBoxItem { Tag = id });
            _themeCombo.SelectionChanged += delegate {
                if (_loadingSettings || _applyingLanguage) return;
                var item = _themeCombo.SelectedItem as ComboBoxItem;
                if (item != null) SetTheme((string)item.Tag);
            };
            _themeToggle.Click += delegate { SetTheme(_theme == "auto" ? "light" : _theme == "light" ? "dark" : "auto"); };
            ((Button)_window.FindName("LangZh")).Click += delegate { SetLanguage("zh"); };
            ((Button)_window.FindName("LangEn")).Click += delegate { SetLanguage("en"); };
            var addMenu = (Button)_window.FindName("AddMenuButton");
            var addPopup = (Popup)_window.FindName("AddMenuPopup");
            addMenu.Click += delegate { if (PresentationIdle) addPopup.IsOpen = !addPopup.IsOpen; };
            ((Button)_window.FindName("AddButton")).Click += delegate { addPopup.IsOpen = false; };
            ((Button)_window.FindName("AddFolderButton")).Click += delegate { addPopup.IsOpen = false; };
            ((Button)_window.FindName("SettingsCloseButton")).Click += delegate { SwitchTab(false); };
            ((Button)_window.FindName("RefreshBackendsButton")).Click += delegate {
                if (PresentationIdle && _selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
            };
            ((Button)_window.FindName("QuickLogButton")).Click += delegate { OpenLog(); };
            ((Button)_window.FindName("PackLogButton")).Click += delegate { PackLogs(); };
            SystemEvents.UserPreferenceChanged += OnSystemThemeChanged;
            _window.Closed += delegate {
                _closed = true;
                ++_packageComparisonGeneration;
                _pendingPackageComparison = null;
                _libraryArtwork.Replace(null);
                _heroArtwork.Replace(null);
                SystemEvents.UserPreferenceChanged -= OnSystemThemeChanged;
            };
        }
        void SetTheme(string theme)
        {
            if (theme != "auto" && theme != "light" && theme != "dark") return;
            _theme = theme;
            LauncherTheme.Apply(theme);
            RefreshPresentationLanguage();
            SaveSettings();
        }
        void OnSystemThemeChanged(object sender, UserPreferenceChangedEventArgs e)
        {
            if (_closed || _theme != "auto" || _window.Dispatcher.HasShutdownStarted) return;
            _window.Dispatcher.BeginInvoke(new Action(delegate {
                if (!_closed && _theme == "auto") LauncherTheme.Apply("auto");
            }));
        }
        void RefreshPresentationLanguage()
        {
            SetLabel("ThemeHeading", "外观主题", "Appearance");
            SetLabel("LangZh", "中", "ZH");
            SetLabel("AddMenuButton", "添加游戏 ▾", "Add game ▾");
            SetLabel("SettingsCloseButton", "完成", "Done");
            ((Button)_window.FindName("RefreshBackendsButton")).ToolTip = L("重新检查后端与模型来源（不下载、不提取）", "Recheck backends and model sources (no download or extraction)");
            SetLabel("QuickLogButton", "诊断日志", "Diagnostic log");
            SetLabel("PackLogButton", "打包日志", "Pack logs");
            ((Button)_window.FindName("PackLogButton")).ToolTip =
                L("收集所选游戏的 OptiScaler/mochizuki 日志与配置、系统信息（GPU/驱动/内存）和近两天的驱动与崩溃事件，打包成压缩包放到桌面。",
                  "Collect the selected game's OptiScaler/mochizuki logs and configs, a system summary (GPU/driver/RAM) and the last two days of driver and crash events into a zip on your desktop.");
            SetLabel("DiagnosticsHeading", "配置与运行库兼容诊断", "Configuration & Runtime Compatibility");
            _searchBox.ToolTip = L("搜索游戏或程序路径", "Search games or executable paths");
            _refreshButton.ToolTip = L("刷新游戏库", "Refresh game library");
            _refreshButton.Content = "↻";
            ((Button)_window.FindName("LangZh")).ToolTip = L("使用简体中文", "Use Chinese");
            ((Button)_window.FindName("LangEn")).ToolTip = L("使用英语", "Use English");
            _themeToggle.Content = _theme == "auto" ? "◐" : _theme == "light" ? "☀" : "☾";
            _themeToggle.ToolTip = L("主题：", "Theme: ") + ThemeName(_theme) + L("（点击切换）", " (click to cycle)");
            bool previous = _applyingLanguage;
            _applyingLanguage = true;
            try {
                foreach (ComboBoxItem item in _themeCombo.Items) {
                    item.Content = ThemeName((string)item.Tag);
                    if ((string)item.Tag == _theme) _themeCombo.SelectedItem = item;
                }
            } finally { _applyingLanguage = previous; }
            var zh = (Button)_window.FindName("LangZh");
            var en = (Button)_window.FindName("LangEn");
            zh.Background = LauncherTheme.Brush(_language == "zh" ? "ThemeSurface" : "ThemeSubtle");
            en.Background = LauncherTheme.Brush(_language == "en" ? "ThemeSurface" : "ThemeSubtle");
            RefreshDiagnosticModeUI();
            RefreshHardwareText();
            RefreshHeroState();
        }
        string ThemeName(string theme)
        {
            return theme == "light" ? L("浅色", "Light") : theme == "dark" ? L("深色", "Dark") : L("跟随系统", "System (auto)");
        }
        void RefreshHardwareText()
        {
            var label = (TextBlock)_window.FindName("HardwareInfo");
            label.Text = _hardwareDetails ?? (_hardwareComplete ? L("GPU 信息不可用", "GPU unavailable") : L("正在检测 GPU…", "Detecting GPU…"));
            label.ToolTip = L("检测到的物理显卡；不代表游戏当前使用的渲染显卡。", "Detected physical GPU; not a claim about the game's rendering adapter.") +
                (string.IsNullOrWhiteSpace(_hardwareWindowsDriver) ? "" : L("\nWindows 驱动版本：", "\nWindows driver version: ") + _hardwareWindowsDriver);
        }
        sealed class HardwareAdapter
        {
            public string Name, WindowsDriver, RadeonVersion;
            public int Rank;
            public bool Primary;
        }
        static readonly string[] NonPhysicalAdapterNames = { "virtual", "remote", "gameviewer", "uu", "parsec", "basic", "indirect", "rdp", "citrix", "vmware", "vnc", "displaylink" };
        static bool ContainsHardwareText(string value, string text)
        {
            return value.IndexOf(text, StringComparison.OrdinalIgnoreCase) >= 0;
        }
        static int PhysicalAdapterRank(string name, string device)
        {
            foreach (var excluded in NonPhysicalAdapterNames)
                if (ContainsHardwareText(name, excluded)) return -1;
            if (!string.IsNullOrEmpty(device) && !device.StartsWith(@"PCI\", StringComparison.OrdinalIgnoreCase)) return -1;
            if (ContainsHardwareText(name, "NVIDIA")) return 0;
            if (ContainsHardwareText(name, "AMD") || ContainsHardwareText(name, "Radeon")) {
                bool integrated = ContainsHardwareText(name, "Vega") && !ContainsHardwareText(name, "RX Vega") || ContainsHardwareText(name, "Radeon(TM) Graphics") ||
                    ContainsHardwareText(name, "Radeon Graphics") || ContainsHardwareText(name, "Ryzen") ||
                    ContainsHardwareText(name, "Radeon 610M") || ContainsHardwareText(name, "Radeon 660M") ||
                    ContainsHardwareText(name, "Radeon 680M") || ContainsHardwareText(name, "Radeon 740M") ||
                    ContainsHardwareText(name, "Radeon 760M") || ContainsHardwareText(name, "Radeon 780M") ||
                    ContainsHardwareText(name, "Radeon 880M") || ContainsHardwareText(name, "Radeon 890M");
                return integrated ? 1 : 0;
            }
            if (ContainsHardwareText(name, "Intel")) return ContainsHardwareText(name, "Arc A") || ContainsHardwareText(name, "Arc B") ||
                ContainsHardwareText(name, "Arc(TM) A") || ContainsHardwareText(name, "Arc(TM) B") ? 0 : 2;
            return -1;
        }
        static HardwareAdapter ReadPhysicalAdapter()
        {
            HardwareAdapter best = null;
            string primary = "";
            try {
                using (var video = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DEVICEMAP\VIDEO"))
                    if (video != null) primary = Convert.ToString(video.GetValue(@"\Device\Video0"));
                const string machinePrefix = @"\Registry\Machine\";
                if (primary.StartsWith(machinePrefix, StringComparison.OrdinalIgnoreCase))
                    using (var mapped = Registry.LocalMachine.OpenSubKey(primary.Substring(machinePrefix.Length)))
                        primary = mapped == null ? "" : Convert.ToString(mapped.GetValue("Driver"));
            } catch { }
            try {
                using (var root = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}")) {
                    if (root != null) foreach (var name in root.GetSubKeyNames()) {
                        int index; if (name.Length != 4 || !int.TryParse(name, out index)) continue;
                        try {
                            using (var key = root.OpenSubKey(name)) {
                                if (key == null) continue;
                                string gpu = Convert.ToString(key.GetValue("DriverDesc"));
                                if (string.IsNullOrWhiteSpace(gpu)) continue;
                                int rank = PhysicalAdapterRank(gpu, Convert.ToString(key.GetValue("MatchingDeviceId")));
                                if (rank < 0) continue;
                                var adapter = new HardwareAdapter {
                                    Name = gpu, Rank = rank,
                                    Primary = primary.EndsWith(@"\" + name, StringComparison.OrdinalIgnoreCase),
                                    WindowsDriver = Convert.ToString(key.GetValue("DriverVersion")),
                                    // Do not infer an Adrenalin release from the Windows driver number.
                                    RadeonVersion = ContainsHardwareText(gpu, "AMD") || ContainsHardwareText(gpu, "Radeon")
                                        ? Convert.ToString(key.GetValue("RadeonSoftwareVersion")) : null
                                };
                                if (best == null || adapter.Rank < best.Rank ||
                                    (adapter.Rank == best.Rank && adapter.Primary && !best.Primary)) best = adapter;
                            }
                        } catch { }
                    }
                }
            } catch { }
            return best;
        }
        void StartHardwareDetection()
        {
            if (_hardwareStarted) return;
            _hardwareStarted = true;
            // Explicit dispatcher handoff also supports native-hosted windows without a SynchronizationContext.
            Task.Run((Func<HardwareAdapter>)ReadPhysicalAdapter).ContinueWith(completed => {
                if (_closed || _window.Dispatcher.HasShutdownStarted) return;
                _window.Dispatcher.BeginInvoke(new Action(delegate {
                    if (_closed) return;
                    var adapter = completed.Result;
                    if (adapter != null) {
                        _hardwareDetails = adapter.Name + (string.IsNullOrWhiteSpace(adapter.RadeonVersion) ? "" : " · Adrenalin " + adapter.RadeonVersion.Trim());
                        _hardwareWindowsDriver = adapter.WindowsDriver;
                    }
                    _hardwareComplete = true;
                    RefreshHardwareText();
                }));
            });
        }
        void RefreshHeroState()
        {
            if (_heroKicker == null) return;
            ((TextBlock)_window.FindName("HeroMonogram")).Text = _selectedGame == null || string.IsNullOrEmpty(_selectedGame.DisplayName) ? "N" : _selectedGame.DisplayName.Substring(0, 1).ToUpperInvariant();
            if (_writeBusy && !_comparisonWriteBusy) {
                ++_packageComparisonGeneration;
                _comparisonGamePaths = null;
                _pendingPackageComparison = null;
                _packageDifferences.Clear();
                RefreshGameListView();
            }
            _comparisonWriteBusy = _writeBusy;
            UpdatePrimaryActionText();
            _heroKicker.Foreground = PackageDiffers(_selectedGame) ? LauncherTheme.Brush("ThemeWarning") : HeroMutedBrush;
            _heroKicker.ToolTip = PackageDiffers(_selectedGame) && _primaryButton != null ? _primaryButton.ToolTip : null;
            ((Button)_window.FindName("AddMenuButton")).IsEnabled = PresentationIdle;
            if (!PresentationIdle) ((Popup)_window.FindName("AddMenuPopup")).IsOpen = false;
            ((Button)_window.FindName("RefreshBackendsButton")).IsEnabled = PresentationIdle && _selectedGame != null && _selectedGame.HasExecutable;
            if (_presentationBusy) {
                _heroKicker.Text = StageText(_presentationAction);
                return;
            }
            if (_selectedGame == null) { _heroKicker.Text = L("游戏库 · 准备开始", "LIBRARY · GET STARTED"); return; }
            if (!_selectedGame.HasExecutable) { _heroKicker.Text = L("文件夹已添加 · 待选择 EXE", "FOLDER ADDED · CHOOSE EXE"); return; }
            var r = _lastInspectResult;
            if (r == null) { _heroKicker.Text = L("正在检查安装环境…", "CHECKING INSTALLATION…"); return; }
            if (!string.IsNullOrEmpty(r.Status) && r.Status != "success") { _heroKicker.Text = L("检查未完成 · 请重试", "INSPECTION INCOMPLETE · RETRY"); return; }
            bool missing = string.IsNullOrEmpty(r.PackageRoot);
            if (r.Backends != null) foreach (var backend in r.Backends)
                if (backend != null && IsBackendSelected(backend.Id) && backend.MissingPackageFiles != null && backend.MissingPackageFiles.Length > 0) missing = true;
            if (missing) { _heroKicker.Text = L("安装包不完整 · 请检查设置", "Package incomplete · Check settings"); return; }
            _heroKicker.Text = _selectedGame.Installed ? (PackageDiffers(_selectedGame)
                ? L("已安装 · 版本不同", "Installed · Version differs")
                : L("已安装", "Installed")) : L("准备就绪 · 可安装", "Ready to install");
        }
        bool PresentationIdle
        {
            get { return !_presentationBusy && !_writeBusy && _folderScan == null && (_session == null || !_session.IsRunning || _session.CanKill); }
        }
        sealed class PackageComparisonRequest
        {
            public int Generation;
            public string PackageRoot;
            public string[] GamePaths;
        }
        static string ComparisonPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            try {
                string full = Path.GetFullPath(path);
                return string.Equals(full, Path.GetPathRoot(full), StringComparison.OrdinalIgnoreCase)
                    ? full : full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            }
            catch { return null; }
        }
        public bool PackageDiffers(GameInfo game)
        {
            if (game == null || !game.Installed || string.IsNullOrEmpty(game.Exe)) return false;
            string path = ComparisonPath(game.Exe);
            bool different;
            return path != null && _packageDifferences.TryGetValue(path, out different) && different;
        }
        void DetectPackageUpdate(GameInfo game, LauncherResult result, string requestedPackageRoot)
        {
            // Only the package selection captured by this inspection may resolve its root.
            if (!string.Equals(ComparisonPath(requestedPackageRoot ?? NormalPackageRoot), ComparisonPath(_packageRoot), StringComparison.OrdinalIgnoreCase)) return;
            RefreshPackageComparisons(result == null ? null : result.PackageRoot, true);
        }
        public void RefreshPackageComparisons(string packageRoot, bool force = false)
        {
            if (_closed || _window.Dispatcher.HasShutdownStarted || _writeBusy) return;
            string root = ComparisonPath(packageRoot);
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var game in _allGames) {
                if (game == null || !game.Installed) continue;
                string path = ComparisonPath(game.Exe);
                if (path != null) paths.Add(path);
            }
            var capturedPaths = new string[paths.Count];
            paths.CopyTo(capturedPaths);
            Array.Sort(capturedPaths, StringComparer.OrdinalIgnoreCase);
            bool unchanged = string.Equals(root, _comparisonPackageRoot, StringComparison.OrdinalIgnoreCase) &&
                _comparisonGamePaths != null && _comparisonGamePaths.Length == capturedPaths.Length;
            if (unchanged) for (int i = 0; i < capturedPaths.Length; ++i)
                if (!string.Equals(capturedPaths[i], _comparisonGamePaths[i], StringComparison.OrdinalIgnoreCase)) { unchanged = false; break; }
            if (unchanged && !force) return;
            _comparisonPackageRoot = root;
            _comparisonGamePaths = capturedPaths;
            int generation = ++_packageComparisonGeneration;
            _packageDifferences.Clear();
            _pendingPackageComparison = root == null ? null : new PackageComparisonRequest {
                Generation = generation, PackageRoot = root, GamePaths = capturedPaths
            };
            RefreshGameListView();
            UpdatePrimaryButtonState();
            if (!_comparisonWorkerRunning && _pendingPackageComparison != null) RunPackageComparisons();
        }
        void RunPackageComparisons()
        {
            var request = _pendingPackageComparison;
            if (request == null || _closed) return;
            _pendingPackageComparison = null;
            _comparisonWorkerRunning = true;
            Task.Run(delegate {
                var compared = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
                try {
                    using (var hash = SHA256.Create()) {
                        byte[] packageHash;
                        using (var source = File.OpenRead(Path.Combine(request.PackageRoot, "OptiScaler.dll"))) packageHash = hash.ComputeHash(source);
                        foreach (var exe in request.GamePaths) {
                            if (System.Threading.Thread.VolatileRead(ref _packageComparisonGeneration) != request.Generation) break;
                            try {
                                string directory = Path.GetDirectoryName(exe), proxy = null;
                                using (var marker = new StreamReader(Path.Combine(directory, "amd-presr-install.txt"))) {
                                    string line;
                                    while ((line = marker.ReadLine()) != null)
                                        if (line.StartsWith("proxy=", StringComparison.OrdinalIgnoreCase)) { proxy = line.Substring(6).Trim(); break; }
                                }
                                if (!ProxyRecommendations.Supports(proxy)) continue;
                                byte[] installedHash;
                                using (var target = File.OpenRead(Path.Combine(directory, proxy))) installedHash = hash.ComputeHash(target);
                                bool different = packageHash.Length != installedHash.Length;
                                for (int i = 0; !different && i < packageHash.Length; ++i) different = packageHash[i] != installedHash[i];
                                compared[exe] = different;
                            } catch { }
                        }
                    }
                } catch { }
                return compared;
            }).ContinueWith(completed => {
                if (_closed || _window.Dispatcher.HasShutdownStarted) return;
                _window.Dispatcher.BeginInvoke(new Action(delegate {
                    _comparisonWorkerRunning = false;
                    if (_closed) return;
                    if (!_writeBusy && request.Generation == _packageComparisonGeneration &&
                        string.Equals(request.PackageRoot, _comparisonPackageRoot, StringComparison.OrdinalIgnoreCase)) {
                        _packageDifferences.Clear();
                        foreach (var entry in completed.Result) _packageDifferences.Add(entry.Key, entry.Value);
                        RefreshGameListView();
                        UpdatePrimaryButtonState();
                    }
                    if (_pendingPackageComparison != null) RunPackageComparisons();
                }));
            });
        }
    }
}
