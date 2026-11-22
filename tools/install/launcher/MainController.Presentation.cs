using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
        enum GpuFamily { Unknown, Rdna4, Rdna3, Rdna2, OtherAmd, NonAmd }
        GpuFamily _gpuFamily;
        string _detectedAdapterName;

        static GpuFamily ClassifyGpu(string name)
        {
            if (string.IsNullOrWhiteSpace(name)) return GpuFamily.Unknown;
            if (!ContainsHardwareText(name, "AMD") && !ContainsHardwareText(name, "Radeon"))
                return ContainsHardwareText(name, "NVIDIA") || ContainsHardwareText(name, "Intel") ? GpuFamily.NonAmd : GpuFamily.Unknown;
            var match = System.Text.RegularExpressions.Regex.Match(name, @"\bRX\s*(\d{4})(?!\d)", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            if (match.Success) {
                switch (match.Groups[1].Value[0]) {
                    case '9': return GpuFamily.Rdna4;
                    case '7': return GpuFamily.Rdna3;
                    case '6': return GpuFamily.Rdna2;
                }
            }
            return GpuFamily.OtherAmd;
        }
        void SetDetectedAdapter(string name)
        {
            _detectedAdapterName = name;
            _gpuFamily = ClassifyGpu(name);
            _hardwareDetails = string.IsNullOrWhiteSpace(name) ? null : name;
            _hardwareWindowsDriver = null;
            _hardwareComplete = true;
            RefreshHardwareText();
            RefreshGpuHints();
        }
        void RefreshGpuHints()
        {
            bool unknown = _gpuFamily == GpuFamily.Unknown;
            bool mochiSupported = _gpuFamily == GpuFamily.Rdna4 || _gpuFamily == GpuFamily.Rdna3;
            SetGpuHint("MochiDescription", mochiSupported ? "" : unknown ? L("适合 RX 7000 / 9000 系列", "For RX 7000 / 9000 series")
                : _gpuFamily == GpuFamily.Rdna2 ? L("RX 6000 请使用旧版 Mochizuki", "RX 6000: use legacy Mochizuki")
                : L("当前显卡不在 Mochizuki 支持范围内", "This GPU is not supported by Mochizuki"), !unknown && !mochiSupported);
            SetGpuHint("OpenRrDescription", _gpuFamily == GpuFamily.Rdna4 ? "" : unknown ? L("适合 RX 9000；RX 7000 为实验性支持", "For RX 9000; experimental on RX 7000")
                : _gpuFamily == GpuFamily.Rdna3 ? L("RX 7000 为实验性支持", "Experimental on RX 7000")
                : L("当前显卡不在 OpenRR 支持范围内", "This GPU is not supported by OpenRR"), !unknown && _gpuFamily != GpuFamily.Rdna3 && _gpuFamily != GpuFamily.Rdna4);
        }
        void SetGpuHint(string name, string text, bool warning)
        {
            var label = (TextBlock)_window.FindName(name);
            label.Text = text;
            label.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
            label.Foreground = LauncherTheme.Brush(warning ? "ThemeWarning" : "ThemeMuted");
            label.ToolTip = _detectedAdapterName;
        }
        void SetCardDescription(string name, string zh, string en, string detailsZh, string detailsEn)
        {
            SetLabel(name, zh, en);
            var label = (TextBlock)_window.FindName(name);
            label.TextWrapping = TextWrapping.NoWrap;
            label.TextTrimming = TextTrimming.CharacterEllipsis;
            ((FrameworkElement)label.Parent).ToolTip = L(detailsZh, detailsEn);
        }
        bool _installLogExpanded;
        bool _closed, _presentationBusy, _hardwareStarted, _hardwareComplete;
        string _presentationAction = "", _hardwareDetails, _hardwareWindowsDriver;
        int _packageComparisonGeneration;
        string _comparisonPackageRoot;
        string[] _comparisonGamePaths;
        bool _comparisonWorkerRunning;
        PackageComparisonRequest _pendingPackageComparison;
        readonly Dictionary<string, bool?> _packageDifferences = new Dictionary<string, bool?>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _installedModes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        ComboBox _themeCombo;
        Button _themeToggle;
        static readonly Brush HeroMutedBrush = LauncherTheme.Brush("ThemeHeroMuted");
        // 16px line icons (stroked by the icon button template).
        static readonly Geometry SunGlyph = Glyph("M 5.25 8 A 2.75 2.75 0 1 1 10.75 8 A 2.75 2.75 0 1 1 5.25 8 Z M 8 1.5 V 3 M 8 13 V 14.5 M 1.5 8 H 3 M 13 8 H 14.5 M 3.4 3.4 L 4.46 4.46 M 11.54 11.54 L 12.6 12.6 M 3.4 12.6 L 4.46 11.54 M 11.54 4.46 L 12.6 3.4");
        static readonly Geometry MoonGlyph = Glyph("M 13.25 9.75 A 5.75 5.75 0 1 1 6.25 2.75 A 4.5 4.5 0 0 0 13.25 9.75 Z");
        static readonly Geometry AutoGlyph = Glyph("M 2.25 8 A 5.75 5.75 0 1 1 13.75 8 A 5.75 5.75 0 1 1 2.25 8 Z M 8 2.25 V 13.75 M 8 5 H 12.9 M 8 8 H 13.75 M 8 11 H 12.9");
        // 10px caption glyphs (match the XAML defaults of MaxBtn).
        static readonly Geometry MaximizeGlyph = Glyph("M 1.5 0.5 H 8.5 A 1 1 0 0 1 9.5 1.5 V 8.5 A 1 1 0 0 1 8.5 9.5 H 1.5 A 1 1 0 0 1 0.5 8.5 V 1.5 A 1 1 0 0 1 1.5 0.5 Z");
        static readonly Geometry RestoreGlyph = Glyph("M 2.5 2.5 H 7.5 A 1 1 0 0 1 8.5 3.5 V 8.5 A 1 1 0 0 1 7.5 9.5 H 1.5 A 1 1 0 0 1 0.5 8.5 V 3.5 A 1 1 0 0 1 1.5 2.5 Z M 2.5 0.5 H 8.5 A 1 1 0 0 1 9.5 1.5 V 7.5");
        static Geometry Glyph(string data)
        {
            var g = Geometry.Parse(data);
            g.Freeze();
            return g;
        }
        void InitPresentation()
        {
            _logView.TextChanged += delegate { RefreshSectionCards(); };
            ((Button)_window.FindName("InstallLogExpandButton")).Click += delegate {
                _installLogExpanded = !_installLogExpanded;
                RefreshSectionCards();
                SaveSettings();
            };
            ((Button)_window.FindName("ProxyRequiredButton")).Click += delegate {
                ((StackPanel)_window.FindName("ProxyDetails")).Visibility = Visibility.Visible;
                RefreshProxySummary();
                ((Border)_window.FindName("ProxyCard")).BringIntoView();
            };
            var hero = (Border)_window.FindName("HeroCard");
            hero.SizeChanged += delegate {
                hero.Clip = new RectangleGeometry(new Rect(0, 0, hero.ActualWidth, hero.ActualHeight), 8, 8);
            };
            _themeCombo = (ComboBox)_window.FindName("ThemeCombo");
            _themeToggle = (Button)_window.FindName("ThemeToggle");
            foreach (var id in new[] { "auto", "light", "dark", "classic", "midnight" })
                _themeCombo.Items.Add(new ComboBoxItem { Tag = id });
            _themeCombo.SelectionChanged += delegate {
                if (_loadingSettings || _applyingLanguage) return;
                var item = _themeCombo.SelectedItem as ComboBoxItem;
                if (item != null) SetTheme((string)item.Tag);
            };
            _themeToggle.Click += delegate {
                var menu = new ContextMenu { PlacementTarget = _themeToggle, Placement = PlacementMode.Bottom,
                    Style = (Style)_window.FindResource("GameContextMenu") };
                foreach (var id in new[] { "auto", "light", "dark", "classic", "midnight" }) {
                    var item = new MenuItem { Header = ThemeName(id), Tag = id, IsCheckable = true, IsChecked = id == _theme };
                    item.Click += delegate(object sender, RoutedEventArgs e) { SetTheme((string)((MenuItem)sender).Tag); };
                    menu.Items.Add(item);
                }
                _themeToggle.ContextMenu = menu;
                menu.IsOpen = true;
            };
            ((Button)_window.FindName("LangZh")).Click += delegate { SetLanguage("zh"); };
            ((Button)_window.FindName("LangEn")).Click += delegate { SetLanguage("en"); };
            var addMenu = (Button)_window.FindName("AddMenuButton");
            var addPopup = (Popup)_window.FindName("AddMenuPopup");
            addMenu.Click += delegate { if (PresentationIdle) addPopup.IsOpen = !addPopup.IsOpen; };
            ((Button)_window.FindName("AddButton")).Click += delegate { addPopup.IsOpen = false; };
            ((Button)_window.FindName("AddFolderButton")).Click += delegate { addPopup.IsOpen = false; };
            var addRtxRemixMc = (Button)_window.FindName("AddRtxRemixMcButton");
            if (addRtxRemixMc != null) addRtxRemixMc.Click += delegate { addPopup.IsOpen = false; };
            ((Button)_window.FindName("SettingsCloseButton")).Click += delegate { SwitchTab(false); };
            // Settings overlay: click on the scrim (outside the card) or press Esc to close.
            var settingsPage = (FrameworkElement)_window.FindName("SettingsPage");
            settingsPage.MouseLeftButtonDown += delegate(object sender, System.Windows.Input.MouseButtonEventArgs e) {
                if (ReferenceEquals(e.OriginalSource, settingsPage)) { SwitchTab(false); e.Handled = true; }
            };
            _window.PreviewKeyDown += delegate(object sender, System.Windows.Input.KeyEventArgs e) {
                if (e.Key == System.Windows.Input.Key.Escape && settingsPage.Visibility == Visibility.Visible) {
                    SwitchTab(false);
                    e.Handled = true;
                }
            };
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
            theme = LauncherTheme.Normalize(theme);
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
            RefreshSectionCards();
            RefreshProxySummary();
            SetLabel("ThemeHeading", "外观主题", "Appearance");
            SetLabel("LangZh", "中", "ZH");
            SetLabel("AddMenuButton", "添加", "Add");
            ((Button)_window.FindName("AddMenuButton")).ToolTip = L("添加游戏、文件夹或 RTX Remix 实例", "Add a game, folder or RTX Remix instance");
            SetLabel("SearchPlaceholder", "搜索游戏或程序路径", "Search games or paths");
            _settingsTab.ToolTip = L("设置", "Settings");
            SetLabel("SettingsCloseButton", "完成", "Done");
            ((Button)_window.FindName("RefreshBackendsButton")).ToolTip = L("重新检查后端与模型来源（不下载、不提取）", "Recheck backends and model sources (no download or extraction)");
            SetLabel("QuickLogButton", "安装日志", "Install log");
            SetLabel("PackLogButton", "打包日志", "Pack logs");
            ((Button)_window.FindName("PackLogButton")).ToolTip =
                L("收集所选游戏的 OptiScaler/mochizuki 日志与配置、系统信息（GPU/驱动/内存）和近两天的驱动与崩溃事件，打包成压缩包放到桌面。",
                  "Collect the selected game's OptiScaler/mochizuki logs and configs, a system summary (GPU/driver/RAM) and the last two days of driver and crash events into a zip on your desktop.");
            SetLabel("DiagnosticsHeading", "安装选项", "Install options");
            _searchBox.ToolTip = L("搜索游戏或程序路径", "Search games or executable paths");
            _refreshButton.ToolTip = L("刷新游戏库", "Refresh game library");
            _refreshButton.Content = "↻";
            ((Button)_window.FindName("LangZh")).ToolTip = L("使用简体中文", "Use Chinese");
            ((Button)_window.FindName("LangEn")).ToolTip = L("使用英语", "Use English");
            _themeToggle.Content = _theme == "auto" ? "◐" : _theme == "light" ? "☀" : "☾";
            DialogTheme.SetIcon(_themeToggle, _theme == "auto" ? AutoGlyph : _theme == "light" ? SunGlyph : MoonGlyph);
            _themeToggle.ToolTip = L("外观：", "Appearance: ") + ThemeName(_theme);
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
            // Segmented control: the selected segment is a raised card, the other one stays flat.
            zh.Background = _language == "zh" ? LauncherTheme.Brush("ThemeCardBg") : Brushes.Transparent;
            en.Background = _language == "en" ? LauncherTheme.Brush("ThemeCardBg") : Brushes.Transparent;
            zh.FontWeight = _language == "zh" ? FontWeights.SemiBold : FontWeights.Normal;
            en.FontWeight = _language == "en" ? FontWeights.SemiBold : FontWeights.Normal;
            zh.Foreground = LauncherTheme.Brush(_language == "zh" ? "ThemeInk" : "ThemeMuted");
            en.Foreground = LauncherTheme.Brush(_language == "en" ? "ThemeInk" : "ThemeMuted");
            RefreshDiagnosticModeUI();
            RefreshHardwareText();
            RefreshGpuHints();
            RefreshHeroState();
        }
        string ThemeName(string theme)
        {
            if (theme == "classic") return L("经典白", "Classic White");
            if (theme == "midnight") return L("午夜蓝", "Midnight");
            return theme == "light" ? L("浅色", "Light") : theme == "dark" ? L("深色", "Dark") : L("跟随系统", "System (auto)");
        }
        void RefreshSectionCards()
        {
            ((StackPanel)_window.FindName("InstallLogCardBody")).Visibility = _installLogExpanded ? Visibility.Visible : Visibility.Collapsed;
            ((System.Windows.Shapes.Path)_window.FindName("InstallLogChevron")).RenderTransform = new RotateTransform(_installLogExpanded ? 90 : 0);
            ((Button)_window.FindName("InstallLogExpandButton")).ToolTip = _installLogExpanded ? L("收起安装日志", "Collapse install log") : L("展开安装日志", "Expand install log");
            var text = _logView == null ? "" : _logView.Text;
            var lines = text.Length == 0 ? 0 : text.TrimEnd('\r', '\n').Split('\n').Length;
            SetLabel("LogPreviewHeading", lines == 0 ? "暂无日志" : lines + " 行", lines == 0 ? "No log entries" : lines + " lines");
        }
        void RefreshProxySummary()
        {
            var proxy = CurrentSelectedProxy;
            string manual;
            bool hasManual = _selectedGame != null && _proxyChoices.TryGetValue(_selectedGame.Id, out manual) && ProxyRecommendations.Supports(manual);
            var profile = _recommendations == null || _selectedGame == null ? null : _recommendations.Match(_selectedGame);
            var origin = hasManual ? L(" · 手动选择", " · Manual") : profile != null ? L(" · Wiki 推荐", " · Wiki recommended") : L(" · 本地检测", " · Local detection");
            SetLabel("ProxySummary", "代理：" + (string.IsNullOrEmpty(proxy) ? "未选择" : proxy + origin), "Proxy: " + (string.IsNullOrEmpty(proxy) ? "Not selected" : proxy + origin));
            var required = (Button)_window.FindName("ProxyRequiredButton");
            required.Content = L("⚠ 尚无可用代理，请选择代理…", "⚠ No proxy available. Choose a proxy…");
            required.Visibility = _selectedGame != null && _selectedGame.HasExecutable && IsOptiScalerSelectedMode() && !IsRtxRemixMinecraftSelected() && string.IsNullOrEmpty(proxy) ? Visibility.Visible : Visibility.Collapsed;
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
                    SetDetectedAdapter(adapter == null ? null : adapter.Name);
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
            UpdatePrimaryActionText();
            _heroKicker.Foreground = HeroMutedBrush;
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
            string importMessage;
            missing |= SelectedBackendNeedsImport(out importMessage);
            if (missing) { _heroKicker.Text = L("待准备", "Setup needed"); return; }
            _heroKicker.Text = _selectedGame.Installed ? (PackageDiffers(_selectedGame)
                ? L("已安装 · 可更新", "Installed · Update available")
                : PackageStatusText(_selectedGame)) : L("准备就绪 · 可安装", "Ready to install");
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
        sealed class PackageComparisonResult
        {
            public readonly Dictionary<string, bool> Differences = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            public readonly Dictionary<string, string> InstalledModes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
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
        bool TryGetPackageDifference(GameInfo game, out bool? different)
        {
            different = null;
            if (game == null || !game.Installed) return false;
            string path = ComparisonPath(game.Exe);
            return path != null && _packageDifferences.TryGetValue(path, out different);
        }
        public bool PackageDiffers(GameInfo game)
        {
            bool? different;
            return TryGetPackageDifference(game, out different) && different == true;
        }
        string PackageStatusText(GameInfo game)
        {
            string mode = null;
            string path = game != null ? ComparisonPath(game.Exe) : null;
            if (!string.IsNullOrEmpty(path)) _installedModes.TryGetValue(path, out mode);
            string tag = (!string.IsNullOrEmpty(mode) && !string.Equals(mode, "optiscaler", StringComparison.OrdinalIgnoreCase))
                ? (" · " + FormatModeShortTag(mode)) : "";
            bool? different;
            if (!TryGetPackageDifference(game, out different)) return L("正在核对版本…", "Checking version…") + tag;
            if (!different.HasValue) return L("已安装 · 版本未知", "Installed · Version unknown") + tag;
            return (different.Value ? L("可更新", "Update available") : L("已安装", "Installed")) + tag;
        }
        void DetectPackageUpdate(LauncherResult result, string requestedPackageRoot)
        {
            // Only the package selection captured by this inspection may resolve its root.
            if (!string.Equals(ComparisonPath(requestedPackageRoot ?? NormalPackageRoot), ComparisonPath(_packageRoot), StringComparison.OrdinalIgnoreCase)) return;
            RefreshPackageComparisons(result == null ? null : result.PackageRoot);
        }
        public void RefreshPackageComparisons(string packageRoot, bool force = false)
        {
            if (_closed || _window.Dispatcher.HasShutdownStarted || _writeBusy) return;
            string root = ComparisonPath(packageRoot);
            string[] capturedPaths = ComparisonGamePaths();
            bool samePackage = string.Equals(root, _comparisonPackageRoot, StringComparison.OrdinalIgnoreCase);
            if (samePackage && !force && SameComparisonPaths(capturedPaths, _comparisonGamePaths)) return;
            if (!samePackage) { _packageDifferences.Clear(); _installedModes.Clear(); }
            if (!samePackage || force) {
                ++_packageComparisonGeneration;
                _pendingPackageComparison = null;
            }
            var current = new HashSet<string>(capturedPaths, StringComparer.OrdinalIgnoreCase);
            foreach (var path in _packageDifferences.Keys.Where(path => !current.Contains(path)).ToArray())
                _packageDifferences.Remove(path);
            foreach (var path in _installedModes.Keys.Where(path => !current.Contains(path)).ToArray())
                _installedModes.Remove(path);
            _comparisonPackageRoot = root;
            _comparisonGamePaths = capturedPaths;
            if (root != null) {
                QueuePackageComparison(root, force ? capturedPaths : capturedPaths.Where(path => !_packageDifferences.ContainsKey(path)).ToArray());
            } else {
                foreach (var path in capturedPaths) _packageDifferences[path] = null;
            }
            RefreshGameListView();
            UpdatePrimaryButtonState();
            if (!_comparisonWorkerRunning) RunPackageComparisons();
        }
        // Re-hashes a single game after a write (install / update / uninstall / unlocker import) and keeps every other
        // game's verdict, so the rest of the library never falls back to "no result".
        public void RecheckPackageComparison(GameInfo game)
        {
            if (_closed || _window.Dispatcher.HasShutdownStarted || _writeBusy) return;
            if (game == null || string.IsNullOrEmpty(game.Exe)) return;
            string path = ComparisonPath(game.Exe);
            if (path == null) return;
            _comparisonGamePaths = ComparisonGamePaths();
            if (!game.Installed) {
                if (_packageDifferences.Remove(path)) { RefreshGameListView(); UpdatePrimaryButtonState(); }
                return;
            }
            if (_comparisonPackageRoot == null) { if (_packageRoot != null) RefreshPackageComparisons(_packageRoot, true); return; }
            _packageDifferences.Remove(path);
            QueuePackageComparison(_comparisonPackageRoot, new string[] { path });
            RefreshGameListView();
            UpdatePrimaryButtonState();
            if (!_comparisonWorkerRunning) RunPackageComparisons();
        }
        string[] ComparisonGamePaths()
        {
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var game in _allGames) {
                if (game == null || !game.Installed) continue;
                string path = ComparisonPath(game.Exe);
                if (path != null) paths.Add(path);
            }
            var captured = new string[paths.Count];
            paths.CopyTo(captured);
            Array.Sort(captured, StringComparer.OrdinalIgnoreCase);
            return captured;
        }
        static bool SameComparisonPaths(string[] left, string[] right)
        {
            if (left == null || right == null || left.Length != right.Length) return false;
            for (int i = 0; i < left.Length; ++i)
                if (!string.Equals(left[i], right[i], StringComparison.OrdinalIgnoreCase)) return false;
            return true;
        }
        void QueuePackageComparison(string root, string[] gamePaths)
        {
            if (root == null || gamePaths == null || gamePaths.Length == 0) return;
            var pending = _pendingPackageComparison;
            if (pending != null && string.Equals(pending.PackageRoot, root, StringComparison.OrdinalIgnoreCase)) {
                var union = new HashSet<string>(pending.GamePaths, StringComparer.OrdinalIgnoreCase);
                union.UnionWith(gamePaths);
                var merged = new string[union.Count];
                union.CopyTo(merged);
                Array.Sort(merged, StringComparer.OrdinalIgnoreCase);
                pending.GamePaths = merged;
                pending.Generation = _packageComparisonGeneration;
                return;
            }
            _pendingPackageComparison = new PackageComparisonRequest {
                Generation = _packageComparisonGeneration, PackageRoot = root, GamePaths = gamePaths
            };
        }
        void RunPackageComparisons()
        {
            var request = _pendingPackageComparison;
            if (request == null || _closed) return;
            _pendingPackageComparison = null;
            _comparisonWorkerRunning = true;
            Task.Run(delegate {
                var compared = new PackageComparisonResult();
                try {
                    using (var hash = SHA256.Create()) {
                        byte[] packageHash = null;
                        string optiDllPath = Path.Combine(request.PackageRoot, "OptiScaler.dll");
                        if (File.Exists(optiDllPath)) using (var source = File.OpenRead(optiDllPath)) packageHash = hash.ComputeHash(source);
                        byte[] pkgMochiHash = null, pkgOpenRrHash = null;
                        byte[] pkgStandaloneDxgiHash = null, pkgVulkanHash = null, pkgRemixBridgeHash = null;
                        string pkgMochiPath = Path.Combine(request.PackageRoot, @"OptiScaler\plugins\dlssnr-amd.asi");
                        if (File.Exists(pkgMochiPath)) using (var s = File.OpenRead(pkgMochiPath)) pkgMochiHash = hash.ComputeHash(s);
                        string pkgOpenRrPath = Path.Combine(request.PackageRoot, @"OptiScaler\plugins\openrr-amd.asi");
                        if (!File.Exists(pkgOpenRrPath)) pkgOpenRrPath = Path.Combine(request.PackageRoot, @"OptiScaler\plugins\openrr.asi");
                        if (File.Exists(pkgOpenRrPath) && new FileInfo(pkgOpenRrPath).Length > 0)
                            using (var s = File.OpenRead(pkgOpenRrPath)) pkgOpenRrHash = hash.ComputeHash(s);
                        string pStandaloneDxgi = Path.Combine(request.PackageRoot, @"standalone\dxgi.dll");
                        if (File.Exists(pStandaloneDxgi)) using (var s = File.OpenRead(pStandaloneDxgi)) pkgStandaloneDxgiHash = hash.ComputeHash(s);
                        string pVulkan = Path.Combine(request.PackageRoot, @"vulkan\vulkan-1.dll");
                        if (File.Exists(pVulkan)) using (var s = File.OpenRead(pVulkan)) pkgVulkanHash = hash.ComputeHash(s);
                        string pRemixBridge = Path.Combine(request.PackageRoot, @"rtx-remix-mc\remixbridge.dll");
                        if (File.Exists(pRemixBridge)) using (var s = File.OpenRead(pRemixBridge)) pkgRemixBridgeHash = hash.ComputeHash(s);

                        var routeHashes = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
                        Func<string, byte[]> routeHash = delegate(string relative) {
                            byte[] bytes;
                            if (!routeHashes.TryGetValue(relative, out bytes)) {
                                var path = Path.Combine(request.PackageRoot, relative);
                                if (File.Exists(path)) using (var s = File.OpenRead(path)) bytes = hash.ComputeHash(s);
                                routeHashes[relative] = bytes;
                            }
                            return bytes;
                        };
                        Func<string, byte[], bool> fileDiffers = delegate(string gameFile, byte[] pkgBytes) {
                            if (pkgBytes == null) return false;
                            if (!File.Exists(gameFile)) return true;
                            using (var fs = File.OpenRead(gameFile)) return !pkgBytes.SequenceEqual(hash.ComputeHash(fs));
                        };

                        foreach (var exe in request.GamePaths) {
                            if (System.Threading.Thread.VolatileRead(ref _packageComparisonGeneration) != request.Generation) break;
                            try {
                                string directory = Path.GetDirectoryName(exe), proxy = null, pluginDir = null, installMode = null;
                                bool expectsMochi = false, expectsOpenRr = false;
                                string markerPath = Path.Combine(directory, "amd-presr-install.txt");
                                if (File.Exists(markerPath)) {
                                    using (var marker = new StreamReader(markerPath)) {
                                        string line;
                                        while ((line = marker.ReadLine()) != null) {
                                            if (line.StartsWith("installMode=", StringComparison.OrdinalIgnoreCase)) installMode = line.Substring(12).Trim().ToLowerInvariant();
                                            else if (line.StartsWith("proxy=", StringComparison.OrdinalIgnoreCase)) proxy = line.Substring(6).Trim();
                                            else if (line.StartsWith("pluginDir=", StringComparison.OrdinalIgnoreCase)) {
                                                var recorded = line.Substring(10).Trim();
                                                if (Path.IsPathRooted(recorded)) pluginDir = ComparisonPath(recorded);
                                            }
                                            else if (line.Equals("backend=mochizuki", StringComparison.OrdinalIgnoreCase)) expectsMochi = true;
                                            else if (line.StartsWith("openRrSpoof=", StringComparison.OrdinalIgnoreCase)) expectsOpenRr = true;
                                        }
                                    }
                                }
                                if (string.IsNullOrEmpty(installMode))
                                    installMode = PeProbe.DetectInstalledMode(directory) ?? "optiscaler";
                                compared.InstalledModes[exe] = installMode;

                                if (installMode == "standalone") {
                                    bool diff = fileDiffers(Path.Combine(directory, "dxgi.dll"), pkgStandaloneDxgiHash) ||
                                        fileDiffers(Path.Combine(directory, "dlssnr-amd.asi"), pkgMochiHash) ||
                                        fileDiffers(Path.Combine(directory, "vulkan-1.dll"), pkgVulkanHash);
                                    compared.Differences[exe] = diff;
                                    continue;
                                }
                                if (installMode == "rtx-remix-mc") {
                                    if (pluginDir == null)
                                        pluginDir = UnlockerPlugin.DestinationDirectory(directory, UnlockerPlugin.ReadIni(Path.Combine(directory, "OptiScaler.ini")));
                                    bool diff = fileDiffers(Path.Combine(directory, "OptiScaler.dll"), packageHash) ||
                                        fileDiffers(Path.Combine(directory, "remixbridge.dll"), pkgRemixBridgeHash);
                                    if (!diff && pkgMochiHash != null) {
                                        string gameMochi = Path.Combine(pluginDir, "dlssnr-amd.asi");
                                        if (File.Exists(gameMochi)) diff = fileDiffers(gameMochi, pkgMochiHash);
                                        else if (expectsMochi) diff = true;
                                    }
                                    if (!diff && pkgOpenRrHash != null) {
                                        string gameOpenRr = Path.Combine(pluginDir, "openrr-amd.asi");
                                        if (!File.Exists(gameOpenRr)) gameOpenRr = Path.Combine(pluginDir, "openrr.asi");
                                        if (File.Exists(gameOpenRr)) diff = fileDiffers(gameOpenRr, pkgOpenRrHash);
                                        else if (expectsOpenRr || Directory.Exists(Path.Combine(directory, @"openrr-amd\shaders")) || Directory.Exists(Path.Combine(directory, @"opennr-rr\shaders"))) diff = true;
                                    }
                                    compared.Differences[exe] = diff;
                                    continue;
                                }
                                if (installMode.StartsWith("reshade", StringComparison.OrdinalIgnoreCase)) {
                                    bool is32Bit = File.Exists(Path.Combine(directory, "dlssnr_amd.addon32"));
                                    string addon = is32Bit ? "dlssnr_amd.addon32" : "dlssnr_amd.addon64";
                                    string dll = is32Bit ? "ReShade32.dll" : "ReShade64.dll";
                                    string arch = is32Bit ? @"x86\" : "";
                                    bool asLayer = installMode != "reshade-feeder";
                                    bool diff = fileDiffers(Path.Combine(directory, addon), routeHash(@"reshade\" + addon)) ||
                                                fileDiffers(Path.Combine(directory, asLayer ? dll : "dxgi.dll"), routeHash(@"reshade\" + dll));
                                    if (!diff && asLayer)
                                        diff = fileDiffers(Path.Combine(directory, "vulkan-1.dll"), routeHash(@"vulkan\" + arch + "vulkan-1.dll"));
                                    if (!diff && installMode == "reshade-feeder-dx9")
                                        diff = fileDiffers(Path.Combine(directory, "d3d9.dll"), routeHash(@"dxvk\" + arch + "d3d9.dll")) ||
                                               fileDiffers(Path.Combine(directory, "dxgi.dll"), routeHash(@"dxvk\" + arch + "dxgi.dll"));
                                    compared.Differences[exe] = diff;
                                    continue;
                                }

                                if (pluginDir == null)
                                    pluginDir = UnlockerPlugin.DestinationDirectory(directory, UnlockerPlugin.ReadIni(Path.Combine(directory, "OptiScaler.ini")));
                                if (packageHash == null || !ProxyRecommendations.Supports(proxy)) continue;
                                byte[] installedHash;
                                using (var target = File.OpenRead(Path.Combine(directory, proxy))) installedHash = hash.ComputeHash(target);
                                bool different = !packageHash.SequenceEqual(installedHash);

                                if (!different && pkgMochiHash != null) {
                                    string gameMochi = Path.Combine(pluginDir, "dlssnr-amd.asi");
                                    if (File.Exists(gameMochi)) {
                                        byte[] gmHash;
                                        using (var gm = File.OpenRead(gameMochi)) gmHash = hash.ComputeHash(gm);
                                        different = !pkgMochiHash.SequenceEqual(gmHash);
                                    } else if (expectsMochi) {
                                        different = true;
                                    }
                                }
                                if (!different && pkgOpenRrHash != null) {
                                    string gameOpenRr = Path.Combine(pluginDir, "openrr-amd.asi");
                                    if (!File.Exists(gameOpenRr)) gameOpenRr = Path.Combine(pluginDir, "openrr.asi");
                                    if (File.Exists(gameOpenRr)) {
                                        byte[] grHash;
                                        using (var gr = File.OpenRead(gameOpenRr)) grHash = hash.ComputeHash(gr);
                                        different = !pkgOpenRrHash.SequenceEqual(grHash);
                                    } else if (expectsOpenRr || Directory.Exists(Path.Combine(directory, @"openrr-amd\shaders")) || Directory.Exists(Path.Combine(directory, @"opennr-rr\shaders"))) {
                                        different = true;
                                    }
                                }
                                compared.Differences[exe] = different;
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
                    if (request.Generation == _packageComparisonGeneration &&
                        string.Equals(request.PackageRoot, _comparisonPackageRoot, StringComparison.OrdinalIgnoreCase)) {
                        var result = completed.Result;
                        if (result != null) {
                            var current = new HashSet<string>(_comparisonGamePaths, StringComparer.OrdinalIgnoreCase);
                            var pending = _pendingPackageComparison;
                            foreach (var path in request.GamePaths) {
                                if (!current.Contains(path) || (pending != null &&
                                    pending.GamePaths.Contains(path, StringComparer.OrdinalIgnoreCase))) continue;
                                bool different;
                                _packageDifferences[path] = result.Differences.TryGetValue(path, out different) ? (bool?)different : null;
                                string instMode;
                                if (result.InstalledModes.TryGetValue(path, out instMode))
                                    _installedModes[path] = instMode;
                                else
                                    _installedModes.Remove(path);
                            }
                        }
                        RefreshGameListView();
                        UpdatePrimaryButtonState();
                    }
                    if (_pendingPackageComparison != null) RunPackageComparisons();
                }));
            });
        }
    }
}
