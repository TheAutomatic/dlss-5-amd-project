using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using Microsoft.Win32;
using Path = System.IO.Path;

namespace OptScalerNR.Launcher
{
    public sealed class GameItemViewModel : System.ComponentModel.INotifyPropertyChanged
    {
        public GameInfo Info { get; set; }
        public bool IsZh { get; set; }
        ImageSource _image;
        public event System.ComponentModel.PropertyChangedEventHandler PropertyChanged;
        public ImageSource IconSource { get { return _image; } set { _image = value; if (PropertyChanged != null) foreach (var name in new[] { "DisplayImage", "CoverVisibility", "InitialVisibility", "ImageStretch" }) PropertyChanged(this, new System.ComponentModel.PropertyChangedEventArgs(name)); } }
        public bool PackageDiffers { get; set; }
        public bool PackageMatches { get; set; }
        public string InstalledStatusText { get; set; }
        public string StatusExplanation { get { return PackageDiffers ? (IsZh ? "已安装的 OptiScaler NR 与当前选择的安装包不同，不一定是旧版本。可按当前选项更新，无需先卸载。" : "The installed OptiScaler NR differs from the package you selected; it is not necessarily older. Update with your current options without uninstalling first.") : ""; } }

        public string Name { get { return Info != null ? Info.DisplayName : ""; } }
        public string Initial
        {
            get
            {
                if (string.IsNullOrEmpty(Name)) return "?";
                return Name.Substring(0, 1).ToUpperInvariant();
            }
        }
        public string SourceText { get { return Info != null ? Info.Source : ""; } }
        public string StatusText
        {
            get
            {
                if (Info != null && !Info.HasExecutable) return IsZh ? "待选 EXE" : "Select EXE";
                if (Info != null && Info.Installed) return InstalledStatusText;
                return IsZh ? "未安装" : "Not installed";
            }
        }
        public Brush StatusBrush
        {
            get
            {
                if (Info != null && !Info.HasExecutable) return LauncherTheme.Brush("ThemeMuted");
                if (PackageDiffers) return LauncherTheme.Brush("ThemeWarning");
                if (Info != null && Info.Installed && PackageMatches)
                    return LauncherTheme.Brush("ThemeSuccess");
                return LauncherTheme.Brush("ThemeMuted");
            }
        }
        public string Cover { get { return Info != null ? Info.DisplayCover : null; } }
        public bool HasCover
        {
            get
            {
                try { return !string.IsNullOrEmpty(Cover) && File.Exists(Cover); }
                catch { return false; }
            }
        }

        public ImageSource DisplayImage { get { return _image; } }
        public Stretch ImageStretch { get { var bitmap = _image as BitmapSource; return HasCover && bitmap != null && bitmap.PixelHeight > bitmap.PixelWidth * 1.15 ? Stretch.UniformToFill : Stretch.Uniform; } }
        public Visibility CoverVisibility { get { return _image != null ? Visibility.Visible : Visibility.Collapsed; } }
        public Visibility InitialVisibility { get { return _image != null ? Visibility.Collapsed : Visibility.Visible; } }
        public string Hero { get { return Info != null ? Info.DisplayHero : null; } }
    }

    public sealed partial class MainController
    {
        readonly Window _window;
        TextBox _searchBox, _logView, _packageRootBox;
        Button _minBtn, _maxBtn, _closeBtn;
        Button _libraryTab, _settingsTab, _addButton, _primaryButton, _launchGameButton, _openFolderButton, _uninstallButton, _openLogButton, _refreshButton, _langToggle, _packageBrowse;
        FrameworkElement _libraryPage, _settingsPage;
        ComboBox _filterCombo, _proxyCombo, _langCombo;
        ListBox _gameList;
        TextBlock _pageTitle, _countBadge, _heroKicker, _heroTitle, _heroMeta, _heroPath, _heroPosterInitial, _injectText, _injectIcon, _injectRecommendation, _refundWarning;
        TextBlock _mochiReason, _statusText, _countText, _versionText;
        Border _mochiBadge;
        Button _compatButton, _restoreCompatButton, _compatHelpButton;
        MenuItem _removeGameMenuItem;
        TextBlock _compatSummary;
        CheckBox _mochiCheck, _replaceIni, _optiPatcherToggle, _openRrToggle, _openRrAutoSpoofToggle, _scanOnStartup;
        ProgressBar _progress;
        Image _heroPosterImage, _heroBgImage;
        Ellipse _statusLed;

        RadioButton _pillDxgi, _pillD3d12, _pillWinmm, _pillWinhttp, _pillWininet, _pillDbghelp;
        Dictionary<string, RadioButton> _proxyPills;

        WorkerSession _session;
        Mutex _targetMutex;
        readonly string _settingsDir;
        readonly string _settingsPath;
        readonly string _legacySettingsPath;
        readonly string _libraryPath;
        string _language = "en";
        bool _loadingSettings = true;
        bool _applyingLanguage;
        bool _saveInitialLanguage;
        string _productVersion = "";
        string _recentGameId;
        string _eventStage;
        bool _isScanOnStartup = true;
        string _packageRoot = NormalPackageRoot;
        string _pendingAction = "";
        bool _writeBusy;
        bool _updatingBackends;
        string _currentLogPath = "";
        string _activeBackend = "mochizuki";
        DateTime _shownUtc;

        readonly List<GameInfo> _allGames = new List<GameInfo>();
        readonly Dictionary<string, BackendChoice> _backendChoices = new Dictionary<string, BackendChoice>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _hiddenGames = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string _resumeInstallAction = "";
        GameInfo _selectedGame;
        LauncherResult _lastInspectResult;
        Decisions _decisions = new Decisions();

        static readonly Brush ReadyBrush = LauncherTheme.Brush("ThemeSuccess");
        static readonly Brush ReadyBorder = LauncherTheme.Brush("ThemeSuccessBg");
        static readonly Brush ReadyBg = LauncherTheme.Brush("ThemeSuccessBg");
        static readonly Brush MissingBrush = LauncherTheme.Brush("ThemeMuted");
        static readonly Brush MissingBorder = LauncherTheme.Brush("ThemeHairline");
        static readonly Brush MissingBg = LauncherTheme.Brush("ThemeSubtle");
        static readonly Brush WarningBrush = LauncherTheme.Brush("ThemeWarning");

        public static Window Create(string[] args)
        {
            return Create(args, null);
        }

        internal static Window Create(string[] args, string settingsDirectory)
        {
            // Merge the theme BEFORE the window XAML is parsed: GhostButton's
            // Template={DynamicResource MaterialButtonTemplate} does not re-resolve for a
            // dictionary merged after XamlReader.Load in this path, and the buttons fall back
            // to the default chrome (the white bevel ring). At app level the lookup succeeds
            // during parsing itself.
            if (Application.Current != null)
                Application.Current.Resources.MergedDictionaries.Add(DialogTheme.Resources());
            Window window;
            using (var stream = Program.OpenXaml("MainWindow.xaml"))
                window = (Window)XamlReader.Load(stream);
            var windowIcon = Program.LoadWindowIcon();
            if (windowIcon != null)
                window.Icon = windowIcon;
            window.Resources.MergedDictionaries.Add(DialogTheme.Resources());
            var controller = new MainController(window, args, settingsDirectory);
            window.Tag = controller;
            // Win11: native rounded corners, dark caption sync and (22H2+) Mica. Win10 keeps the solid frame.
            NativeFrame.Attach(window, true);
            return window;
        }

        MainController(Window window, string[] args, string settingsDirectory)
        {
            _window = window;
            _libraryArtwork = new LocalArtwork.Scope(window.Dispatcher);
            _heroArtwork = new LocalArtwork.Scope(window.Dispatcher);
            var titleIcon = window.FindName("AppTitleIcon") as Image;
            if (titleIcon != null)
                titleIcon.Source = Program.LoadTitleIcon();

            _minBtn = (Button)window.FindName("MinBtn");
            _maxBtn = (Button)window.FindName("MaxBtn");
            _closeBtn = (Button)window.FindName("CloseBtn");

            _searchBox = (TextBox)window.FindName("SearchBox");
            _pageTitle = (TextBlock)window.FindName("PageTitle");
            _countBadge = (TextBlock)window.FindName("CountBadge");
            _refundWarning = (TextBlock)window.FindName("RefundWarning");
            _langToggle = (Button)window.FindName("LangToggle");
            _libraryTab = (Button)window.FindName("LibraryTab");
            _settingsTab = (Button)window.FindName("SettingsTab");
            _libraryPage = (FrameworkElement)window.FindName("LibraryPage");
            _settingsPage = (FrameworkElement)window.FindName("SettingsPage");
            _filterCombo = (ComboBox)window.FindName("FilterCombo");
            _addButton = (Button)window.FindName("AddButton");
            _gameList = (ListBox)window.FindName("GameList");

            _heroKicker = (TextBlock)window.FindName("HeroKicker");
            _heroTitle = (TextBlock)window.FindName("HeroTitle");
            _heroMeta = (TextBlock)window.FindName("HeroMeta");
            _heroPath = (TextBlock)window.FindName("HeroPath");
            _heroPosterInitial = (TextBlock)window.FindName("HeroPosterInitial");
            _heroPosterImage = (Image)window.FindName("HeroPosterImage");
            _heroBgImage = (Image)window.FindName("HeroBgImage");

            _primaryButton = (Button)window.FindName("PrimaryButton");
            _launchGameButton = (Button)window.FindName("LaunchGameButton");
            _openFolderButton = (Button)window.FindName("OpenFolderButton");
            _uninstallButton = (Button)window.FindName("UninstallButton");
            _compatButton = (Button)window.FindName("CompatButton");
            _restoreCompatButton = (Button)window.FindName("RestoreCompatButton");
            _compatHelpButton = (Button)window.FindName("CompatHelpButton");
            _compatSummary = (TextBlock)window.FindName("CompatSummary");
            _progress = (ProgressBar)window.FindName("Progress");

            _injectText = (TextBlock)window.FindName("InjectText");
            _injectIcon = (TextBlock)window.FindName("InjectIcon");
            _injectRecommendation = (TextBlock)window.FindName("InjectRecommendation");
            _proxyCombo = (ComboBox)window.FindName("ProxyCombo");

            _pillDxgi = (RadioButton)window.FindName("PillDxgi");
            _pillD3d12 = (RadioButton)window.FindName("PillD3d12");
            _pillWinmm = (RadioButton)window.FindName("PillWinmm");
            _pillWinhttp = (RadioButton)window.FindName("PillWinhttp");
            _pillWininet = (RadioButton)window.FindName("PillWininet");
            _pillDbghelp = (RadioButton)window.FindName("PillDbghelp");

            _mochiCheck = (CheckBox)window.FindName("MochiCheck");
            _mochiBadge = (Border)window.FindName("MochiBadge");
            _mochiReason = (TextBlock)window.FindName("MochiReason");
            _replaceIni = (CheckBox)window.FindName("ReplaceIni");
            _optiPatcherToggle = (CheckBox)window.FindName("OptiPatcherToggle");
            _openRrToggle = (CheckBox)window.FindName("OpenRrToggle");
            _openRrAutoSpoofToggle = (CheckBox)window.FindName("OpenRrAutoSpoofToggle");
            if (_openRrToggle != null)
            {
                _openRrToggle.Checked += (s, e) => { RememberOpenRrChoice(); RefreshModelImportBar(); };
                _openRrToggle.Unchecked += (s, e) => { RememberOpenRrChoice(); RefreshModelImportBar(); };
            }
            if (_openRrAutoSpoofToggle != null)
            {
                _openRrAutoSpoofToggle.Checked += (s, e) => { RememberOpenRrSpoofChoice(); };
                _openRrAutoSpoofToggle.Unchecked += (s, e) => { RememberOpenRrSpoofChoice(); };
            }
            if (_optiPatcherToggle != null)
            {
                _optiPatcherToggle.Checked += (s, e) => { RememberOptiPatcherChoice(); RefreshOpenRrCard(); };
                _optiPatcherToggle.Unchecked += (s, e) => { RememberOptiPatcherChoice(); RefreshOpenRrCard(); };
            }
            _openLogButton = (Button)window.FindName("OpenLogButton");
            _logView = (TextBox)window.FindName("LogView");
            _statusText = (TextBlock)window.FindName("StatusText");
            _statusLed = (Ellipse)window.FindName("StatusLed");
            _countText = (TextBlock)window.FindName("CountText");
            _versionText = (TextBlock)window.FindName("VersionText");

            _langCombo = (ComboBox)window.FindName("LangCombo");
            _scanOnStartup = (CheckBox)window.FindName("ScanOnStartup");
            _refreshButton = (Button)window.FindName("RefreshButton");
            _packageRootBox = (TextBox)window.FindName("PackageRootBox");
            _packageBrowse = (Button)window.FindName("PackageBrowse");
            var githubBtn = window.FindName("GithubButton") as Button;
            if (githubBtn != null) githubBtn.Click += (s, e) => OpenUrl("https://github.com/TheAutomatic/dlss-5-amd-project");
            var bilibiliBtn = window.FindName("BilibiliButton") as Button;
            if (bilibiliBtn != null) bilibiliBtn.Click += (s, e) => OpenUrl("https://space.bilibili.com/291088");
            foreach (var name in new[] { "OpenRrAsiButton", "RtxRemixMcPatchButton", "MochizukiAuthorButton" }) {
                var author = window.FindName(name) as Button;
                if (author != null) author.Click += (s, e) => OpenUrl((string)((Button)s).ToolTip);
            }

            _settingsDir = settingsDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR", "Launcher");
            _settingsPath = Path.Combine(_settingsDir, "settings.json");
            _legacySettingsPath = settingsDirectory == null ? Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "settings.json") : Path.Combine(settingsDirectory, "legacy-launcher", "settings.json");
            _libraryPath = Path.Combine(_settingsDir, "library.json");
            InitPresentation();
            InitDiagnostics();
            InitLibraryActions();
            InitRecommendations();
            InitProfiles();

            _decisions.ExistingInstall = "overwrite";
            _decisions.ExistingIni = "overwrite";
            _decisions.MissingMochizukiModel = "skip";
            _decisions.ShortFileContinue = "abort";
            _decisions.ForeignProxy = new Dictionary<string, string>();

            if (_minBtn != null) _minBtn.Click += (s, e) => _window.WindowState = WindowState.Minimized;
            if (_maxBtn != null) _maxBtn.Click += (s, e) =>
            {
                _window.WindowState = (_window.WindowState == WindowState.Maximized) ? WindowState.Normal : WindowState.Maximized;
            };
            if (_closeBtn != null) _closeBtn.Click += (s, e) => _window.Close();

            _window.StateChanged += (s, e) =>
            {
                if (_window.WindowState == WindowState.Maximized)
                {
                    _window.BorderThickness = new Thickness(7);
                    if (_maxBtn != null) DialogTheme.SetIcon(_maxBtn, RestoreGlyph);
                }
                else
                {
                    _window.BorderThickness = NativeFrame.NormalBorder;
                    if (_maxBtn != null) DialogTheme.SetIcon(_maxBtn, MaximizeGlyph);
                }
            };

            if (_libraryTab != null) _libraryTab.Click += (s, e) => SwitchTab(false);
            if (_settingsTab != null) _settingsTab.Click += (s, e) => SwitchTab(_settingsPage == null || _settingsPage.Visibility != Visibility.Visible);
            if (_langToggle != null) _langToggle.Click += (s, e) => ToggleLanguage();

            if (_filterCombo != null)
            {
                RefreshLibraryFilters();
                _filterCombo.SelectionChanged += delegate { if (!_refreshingFilters) RefreshGameListView(); };
            }

            if (_searchBox != null)
                _searchBox.TextChanged += (s, e) => RefreshGameListView();

            InitProxyPills();

            if (_proxyCombo != null)
            {
                _proxyCombo.Items.Add("请选择");
                foreach (var name in ProxyRecommendations.ProxyNames)
                    _proxyCombo.Items.Add(name);
                _proxyCombo.SelectedIndex = 0;
                _proxyCombo.SelectionChanged += (s, e) =>
                {
                    var selected = Convert.ToString(_proxyCombo.SelectedItem);
                    SelectProxyPillOnly(selected);
                    RememberProxyChoice();
                    UpdatePrimaryButtonState();
                };
            }

            if (_langCombo != null)
            {
                _langCombo.Items.Add(new ComboBoxItem { Tag = "zh", Content = "Chinese" });
                _langCombo.Items.Add(new ComboBoxItem { Tag = "en", Content = "English" });
                _langCombo.SelectionChanged += (s, e) =>
                {
                    if (_loadingSettings || _applyingLanguage) return;
                    var item = _langCombo.SelectedItem as ComboBoxItem;
                    if (item != null) SetLanguage(Convert.ToString(item.Tag));
                };
            }

            if (_scanOnStartup != null)
            {
                _scanOnStartup.Checked += (s, e) => { _isScanOnStartup = true; SaveSettings(); };
                _scanOnStartup.Unchecked += (s, e) => { _isScanOnStartup = false; SaveSettings(); };
            }

            if (_packageBrowse != null) _packageBrowse.Click += (s, e) => BrowsePackageRoot();
            if (_packageRootBox != null)
            {
                _packageRootBox.LostKeyboardFocus += (s, e) =>
                {
                    if (_diagnosticMode && CanChangePackageSource()) SetDiagnosticPackageRoot(_packageRootBox.Text);
                };
            }

            if (_addButton != null) _addButton.Click += (s, e) => AddGame();
            if (_gameList != null) _gameList.SelectionChanged += (s, e) => OnGameSelected();
            if (_launchGameButton != null) _launchGameButton.Click += (s, e) => LaunchSelectedGame();
            if (_openFolderButton != null) _openFolderButton.Click += (s, e) => OpenSelectedFolder();
            if (_primaryButton != null) _primaryButton.Click += (s, e) => OnPrimaryAction();
            if (_uninstallButton != null) _uninstallButton.Click += (s, e) => OnUninstall();
            InitGameContextMenu();
            if (_compatButton != null) _compatButton.Click += (s, e) => OnCompatibility(false);
            if (_restoreCompatButton != null) _restoreCompatButton.Click += (s, e) => OnCompatibility(true);
            if (_compatHelpButton != null) _compatHelpButton.Click += (s, e) => ShowCompatibilityHelp();
            if (_openLogButton != null) _openLogButton.Click += (s, e) => OpenLog();
            if (_refreshButton != null) _refreshButton.Click += (s, e) => RunScan();

            RoutedEventHandler backendChanged = (s, e) => {
                if (_updatingBackends) return;
                UpdateActiveCombo(); SaveBackendChoice(); UpdatePrimaryButtonState(); RefreshInspectionText(); RefreshModelImportBar();
            };
            if (_mochiCheck != null) { _mochiCheck.Checked += backendChanged; _mochiCheck.Unchecked += backendChanged; }

            _window.AllowDrop = true;
            _window.Drop += OnDrop;
            _window.DragOver += (s, e) => { e.Effects = DragDropEffects.Copy; e.Handled = true; };

            _window.Closing += OnClosing;
            _window.Loaded += (s, e) => OnFirstShown(args);

            InitModes();
            LoadSettings();
            LauncherTheme.Apply(_theme);
            _loadingSettings = false;
            LoadLibraryCache();
            RefreshPackageComparisons(_packageRoot);
            ShowVersion();
            ApplyLang();
            ApplyModeVisibility();
            if (_saveInitialLanguage) SaveSettings();
            RefreshGameListView();
            UpdateActiveCombo();
            UpdatePrimaryButtonState();
        }

        void InitProxyPills()
        {
            _proxyPills = new Dictionary<string, RadioButton>(StringComparer.OrdinalIgnoreCase);
            if (_pillDxgi != null) _proxyPills["dxgi.dll"] = _pillDxgi;
            if (_pillD3d12 != null) _proxyPills["d3d12.dll"] = _pillD3d12;
            if (_pillWinmm != null) _proxyPills["winmm.dll"] = _pillWinmm;
            if (_pillWinhttp != null) _proxyPills["winhttp.dll"] = _pillWinhttp;
            if (_pillWininet != null) _proxyPills["wininet.dll"] = _pillWininet;
            if (_pillDbghelp != null) _proxyPills["dbghelp.dll"] = _pillDbghelp;
            var versionPill = (RadioButton)_window.FindName("PillVersion");
            if (versionPill != null) _proxyPills["version.dll"] = versionPill;

            foreach (var kvp in _proxyPills)
            {
                var dllName = kvp.Key;
                var pill = kvp.Value;
                pill.Checked += (s, e) =>
                {
                    if (_proxyCombo != null)
                    {
                        for (int i = 0; i < _proxyCombo.Items.Count; i++)
                        {
                            if (string.Equals(Convert.ToString(_proxyCombo.Items[i]), dllName, StringComparison.OrdinalIgnoreCase))
                            {
                                _proxyCombo.SelectedIndex = i;
                                break;
                            }
                        }
                    }
                    UpdatePrimaryButtonState();
                    RememberProxyChoice();
                };
            }
        }

        void SelectProxyPillOnly(string dllName)
        {
            if (_proxyPills == null) return;
            foreach (var kvp in _proxyPills)
            {
                if (string.Equals(kvp.Key, dllName, StringComparison.OrdinalIgnoreCase))
                {
                    if (kvp.Value.IsChecked != true) kvp.Value.IsChecked = true;
                }
            }
        }

        void SelectProxy(string dllName)
        {
            var previousSelectionMode = _selectingProxy;
            _selectingProxy = true;
            try {
            if (string.IsNullOrEmpty(dllName))
            {
                if (_proxyPills != null)
                {
                    foreach (var p in _proxyPills.Values) p.IsChecked = false;
                }
                if (_proxyCombo != null) _proxyCombo.SelectedIndex = 0;
                return;
            }

            if (_proxyPills != null && _proxyPills.ContainsKey(dllName))
            {
                _proxyPills[dllName].IsChecked = true;
            }

            if (_proxyCombo != null)
            {
                for (int i = 0; i < _proxyCombo.Items.Count; i++)
                {
                    if (string.Equals(Convert.ToString(_proxyCombo.Items[i]), dllName, StringComparison.OrdinalIgnoreCase))
                    {
                        _proxyCombo.SelectedIndex = i;
                        break;
                    }
                }
            }
            UpdatePrimaryButtonState();
            } finally { _selectingProxy = previousSelectionMode; RefreshProxySummary(); }
        }

        string CurrentSelectedProxy
        {
            get
            {
                if (_proxyPills != null)
                {
                    foreach (var kvp in _proxyPills)
                    {
                        if (kvp.Value.IsChecked == true) return kvp.Key;
                    }
                }
                if (_proxyCombo != null && _proxyCombo.SelectedIndex > 0)
                    return Convert.ToString(_proxyCombo.SelectedItem);
                return "";
            }
        }

        void UpdateActiveUI()
        {
            _activeBackend = (_mochiCheck != null && _mochiCheck.IsChecked == true) ? "mochizuki" : "";
        }

        static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
            catch { }
        }

        void SwitchTab(bool toSettings)
        {
            if (_libraryPage != null) _libraryPage.Visibility = Visibility.Visible;
            if (_settingsPage != null) _settingsPage.Visibility = toSettings ? Visibility.Visible : Visibility.Collapsed;
            if (_settingsTab != null) _settingsTab.Content = L("设置", "Settings");
            if (_settingsTab != null) _settingsTab.ToolTip = L("设置", "Settings");
        }

        void ToggleLanguage()
        {
            SetLanguage(_language == "zh" ? "en" : "zh");
        }

        void SetLanguage(string language)
        {
            if (language != "zh" && language != "en") return;
            _language = language;
            SaveSettings();
            ApplyLang();
        }

        internal static string ResolveLanguage(string saved, CultureInfo uiCulture)
        {
            if (string.Equals(saved, "zh", StringComparison.OrdinalIgnoreCase)) return "zh";
            if (string.Equals(saved, "en", StringComparison.OrdinalIgnoreCase)) return "en";
            return uiCulture != null && uiCulture.TwoLetterISOLanguageName == "zh" ? "zh" : "en";
        }

        void OnFirstShown(string[] args)
        {
            _shownUtc = DateTime.UtcNow;
            if (args != null)
            {
                foreach (var a in args)
                {
                    if (a == "--startup-measure")
                    {
                        WriteStartupMeasure();
                        _window.Close();
                        return;
                    }
                    if (a != null && a.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && GamePaths.CanUse(a))
                    {
                        AddGameFromPath(a);
                    }
                }
            }

            StartHardwareDetection();
            RefreshProfiles();
            if (_selectedGame == null && _allGames.Count > 0 && _gameList != null && _gameList.Items.Count > 0)
            {
                int targetIdx = 0;
                if (!string.IsNullOrEmpty(_recentGameId))
                {
                    for (int i = 0; i < _gameList.Items.Count; i++)
                    {
                        var vm = _gameList.Items[i] as GameItemViewModel;
                        if (vm != null && vm.Info != null && string.Equals(vm.Info.Id, _recentGameId, StringComparison.OrdinalIgnoreCase))
                        {
                            targetIdx = i;
                            break;
                        }
                    }
                }
                _gameList.SelectedIndex = targetIdx;
            }
            if (_isScanOnStartup)
            {
                RunScan(_allGames.Count > 0);
            }
        }

        void WriteStartupMeasure()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var line = DateTime.UtcNow.ToString("o") + " interactiveMs=" +
                    (DateTime.UtcNow - Program.StartedUtc).TotalMilliseconds.ToString("0");
                File.AppendAllText(Path.Combine(_settingsDir, "startup.log"), line + Environment.NewLine);
                var outPath = Environment.GetEnvironmentVariable("OPTSCALER_STARTUP_LOG");
                if (!string.IsNullOrEmpty(outPath))
                    File.WriteAllText(outPath, line);
            }
            catch { }
        }

        void ShowVersion()
        {
            try
            {
                var roots = new List<string>();
                try
                {
                    var asmLoc = typeof(MainController).Assembly.Location;
                    if (!string.IsNullOrEmpty(asmLoc)) roots.Add(Path.GetDirectoryName(asmLoc));
                }
                catch { }
                if (!string.IsNullOrEmpty(AppDomain.CurrentDomain.BaseDirectory))
                    roots.Add(AppDomain.CurrentDomain.BaseDirectory);

                foreach (var start in roots)
                {
                    var dir = start;
                    for (int depth = 0; depth < 5 && !string.IsNullOrEmpty(dir); depth++)
                    {
                        var candidate = Path.Combine(dir, "VERSION");
                        if (File.Exists(candidate))
                        {
                            var raw = File.ReadAllText(candidate).Trim();
                            if (!string.IsNullOrEmpty(raw))
                            {
                                _productVersion = " " + raw;
                                if (_versionText != null)
                                    _versionText.Text = "OptiScaler NR v" + raw.TrimStart('v', 'V');
                                ((TextBlock)_window.FindName("VersionTag")).Text = raw.TrimStart('v', 'V');
                            }
                            return;
                        }
                        dir = Path.GetDirectoryName(dir.TrimEnd('\\', '/'));
                    }
                }
            }
            catch { }
        }

        static string GetReasonText(string reasonCode, string fallbackReason, bool isZh)
        {
            if (string.IsNullOrEmpty(reasonCode)) return fallbackReason ?? "";
            if (isZh)
            {
                switch (reasonCode.ToLowerInvariant())
                {
                    case "ready": return "就绪";
                    case "missing-weights": return "权重缺失";
                    case "missing-runtime": return "运行库缺失";
                    case "preparable": return "需准备模型";
                    case "missing-model": return "模型缺失";
                    case "partial": return "部分模型就绪";
                    default: return fallbackReason ?? reasonCode;
                }
            }
            else
            {
                switch (reasonCode.ToLowerInvariant())
                {
                    case "ready": return "Ready";
                    case "missing-weights": return "Missing Weights";
                    case "missing-runtime": return "Missing Runtime";
                    case "preparable": return "Preparation Required";
                    case "missing-model": return "Missing Model";
                    case "partial": return "Some Models Ready";
                    default: return fallbackReason ?? reasonCode;
                }
            }
        }

        void ApplyLang()
        {
            _applyingLanguage = true;
            try
            {
                var isZh = _language == "zh";
                _window.Language = XmlLanguage.GetLanguage(isZh ? "zh-CN" : "en-US");
                if (_window != null)
                    _window.Title = "OptiScaler NR" + _productVersion + (isZh ? " - 本软件免费，如付费购买请立即退款！" : " - Free software — refund if paid!");
                if (_refundWarning != null)
                    _refundWarning.Text = isZh ? "本软件免费，如付费购买请立即退款！" : "Free software — refund if paid!";
                if (_pageTitle != null) _pageTitle.Text = isZh ? "游戏库" : "Library";
                if (_langToggle != null) _langToggle.Content = isZh ? "🌐 English" : "🌐 Chinese";
                if (_libraryTab != null) _libraryTab.Content = isZh ? "游戏库" : "Library";
                if (_settingsTab != null) _settingsTab.Content = L("设置", "Settings");
                if (_addButton != null) _addButton.Content = L("添加游戏 EXE…", "Add game EXE…");
                if (_launchGameButton != null)
                {
                    _launchGameButton.Content = isZh ? "启动游戏" : "Launch";
                    _launchGameButton.ToolTip = isZh ? "直接运行当前选中的游戏主程序" : "Directly run the selected game executable";
                }
                if (_openFolderButton != null) _openFolderButton.Content = isZh ? "打开目录" : "Open Folder";
                if (_uninstallButton != null) _uninstallButton.Content = isZh ? "卸载" : "Uninstall";
                if (_removeGameMenuItem != null) _removeGameMenuItem.Header = L("从列表移除", "Remove from list");
                if (_compatButton != null) _compatButton.Content = L("兼容修复", "Compat Repair");
                if (_restoreCompatButton != null) _restoreCompatButton.Content = L("恢复原文件", "Restore Files");
                if (_compatHelpButton != null) _compatHelpButton.ToolTip = L("一般在什么情况下需要使用兼容修复？", "When should you use Compatibility Repair?");
                UpdateCompatibilitySummary();
                if (_openLogButton != null) _openLogButton.Content = isZh ? "打开完整日志" : "Open Full Log";
                if (_refreshButton != null) _refreshButton.Content = isZh ? "刷新" : "Refresh";
                if (_packageBrowse != null) _packageBrowse.Content = isZh ? "浏览…" : "Browse…";
                if (_heroKicker != null) _heroKicker.Text = isZh ? "已选游戏" : "Selected game";
                if (_replaceIni != null) _replaceIni.Content = isZh ? "覆盖现有配置" : "Replace existing configuration";
                SetLabel("OptiPatcherToggle", "安装 OptiPatcher", "Install OptiPatcher");
                ApplyOptiPatcherTooltip(IsOptiPatcherSupported(_selectedGame, (_selectedGame == null || _recommendations == null) ? null : _recommendations.Match(_selectedGame)));
                SetLabel("OpenRrToggle", "OpenRR 0.1.7 Plus 3", "OpenRR 0.1.7 Plus 3");
                SetLabel("OpenRrAutoSpoofToggle",
                    "实验性：减少全局显卡伪装（Dxgi=auto，配合 OptiPatcher 保持真实 A 卡身份）",
                    "Experimental: Reduce global GPU spoofing (Dxgi=auto, keeps native AMD adapter with OptiPatcher)");
                RefreshOpenRrCard();
                if (_openRrToggle != null) _openRrToggle.ToolTip = L("勾选时部署 openrr-amd.asi 与 openrr-amd/（shaders / aco / lang），并在检测到您自备或导入的 nvngx_dlssd.dll (310.9.1.0) 时放入 openrr-amd/。取消勾选时移除 openrr-amd.asi。", "Installs openrr-amd.asi and openrr-amd/ (shaders / aco / lang), and places your supplied/imported nvngx_dlssd.dll (310.9.1.0) into openrr-amd/. When unchecked, removes openrr-amd.asi.");
                if (_scanOnStartup != null) _scanOnStartup.Content = isZh ? "启动时自动扫描游戏库" : "Scan game library on startup";

                SetLabel("LocalGamesHeading", "游戏库", "Library");
                SetLabel("ProxyHeading", "注入代理", "Injection proxy");
                SetLabel("BackendsHeading", "画质插件", "Quality plugins");
                SetLabel("MochiCheck", "mochizuki 0.0.9 M10", "mochizuki 0.0.9 M10");
                RefreshGpuHints();
                SetLabel("DiagnosticsHeading", "安装选项", "Install options");
                SetLabel("LogHeading", "安装日志", "Install log");
                SetLabel("SettingsHeading", "设置", "Settings");
                SetLabel("LanguageHeading", "语言", "Language");
                SetLabel("PackageHeading", "安装包目录", "Package Folder");
                SetLabel("PackageDesc",
                    "默认无需填写（自动使用启动器所在目录）；仅在将启动器单独移出完整解压包或源码调试时手动指定。",
                    "Leave blank by default (uses launcher folder); only set when running outside the extracted package.");
                SetLabel("CommunityHeading", "关于与社区", "About & Community");
                SetLabel("GithubButton", "GitHub · 项目主页", "GitHub · Repository");
                SetLabel("RtxRemixMcPatchButton", "RTX Remix MC 补丁", "RTX Remix MC patch");
                SetLabel("BilibiliButton", "Bilibili · 作者主页", "Bilibili · Channel");
                if (_selectedGame == null)
                {
                    SetLabel("HeroTitle", "选择一个游戏以开始", "Select a game to get started");
                    SetLabel("HeroMeta", "选择游戏后自动检查运行状态与注入环境", "Select a game to inspect its installation and proxy options.");
                }
                if (_langCombo != null)
                {
                    ((ComboBoxItem)_langCombo.Items[0]).Content = L("简体中文", "Chinese");
                    ((ComboBoxItem)_langCombo.Items[1]).Content = "English";
                    _langCombo.SelectedIndex = isZh ? 0 : 1;
                }

                RefreshLibraryFilters();

                if (_proxyCombo != null && _proxyCombo.Items.Count > 0)
                {
                    var idx = _proxyCombo.SelectedIndex;
                    _proxyCombo.Items[0] = isZh ? "请选择" : "Select DLL";
                    if (idx >= 0) _proxyCombo.SelectedIndex = idx;
                }

                UpdateActiveUI();
                UpdatePrimaryButtonState();
                RefreshInspectionText();
                RefreshLibraryActions();
                UpdateProxyRecommendation();
                RefreshStatusLanguage();
                RefreshPresentationLanguage();
                RefreshModesLanguage();
                RefreshProfilesLanguage();
                RefreshGameListView();
            }
            finally { _applyingLanguage = false; }
        }

        string L(string zh, string en) { return _language == "zh" ? zh : en; }

        void SetLabel(string name, string zh, string en)
        {
            var element = _window.FindName(name);
            var text = element as TextBlock;
            if (text != null) text.Text = L(zh, en);
            var content = element as ContentControl;
            if (content != null) content.Content = L(zh, en);
        }

        bool IsBackendVisible(string id)
        {
            return id == "mochizuki";
        }

        bool IsBackendSelected(string id)
        {
            return id == "mochizuki" && _mochiCheck != null && _mochiCheck.IsChecked == true;
        }

        void RefreshBackendStatus(string id, bool ready, string reason)
        {
            if (id != "mochizuki") return;
            if (!IsBackendSelected(id))
            {
                ready = false;
                reason = L("未选择", "Not selected");
            }
            if (_mochiReason != null)
            {
                _mochiReason.Text = reason;
                _mochiReason.Foreground = ready ? ReadyBrush : IsBackendSelected(id) ? WarningBrush : MissingBrush;
            }
        }

        void RefreshInspectionText()
        {
            // Refresh presentation only. Reapplying inspection would reset the user's choices.
            var result = _lastInspectResult;
            var failed = result != null && !string.IsNullOrEmpty(result.Status) && result.Status != "success";
            if (_selectedGame != null && !_selectedGame.HasExecutable) {
                if (_injectText != null) _injectText.Text = L("尚未定位启动程序。可以保留文件夹条目，扫描或选择 EXE 后再安装。", "Executable not selected. Keep this folder entry and find or choose its EXE before installing.");
                UpdateProxyRecommendation();
                foreach (var id in new[] { "mochizuki" })
                    RefreshBackendStatus(id, false, L("待选择 EXE", "Select EXE first"));
                RefreshBackendHints();
                RefreshModelImportBar();
                return;
            }
            if (_injectText != null) _injectText.Text = failed
                ? L("检查失败，请重新选择游戏以重试。", "Inspection failed. Select the game again to retry.")
                : result != null
                ? L("分析完成，请确认注入代理与后端配置：", "Analysis complete. Confirm the proxy DLL and backends:")
                : (_selectedGame != null
                    ? L("正在检查导入与后端…", "Checking imports and backends…")
                    : L("请从下方选择游戏启动时所加载的拦截代理 DLL：", "Choose the proxy DLL to load when the game starts:"));
            if (_injectRecommendation != null) _injectRecommendation.Text = failed
                ? L("检测未完成：", "Inspection incomplete: ") + result.Error
                : result == null
                ? L("选择游戏后将分析 PE 导入表并自动匹配推荐代理。", "Select a game to inspect its imports and recommend a proxy.")
                : (!string.IsNullOrEmpty(result.Recommend)
                    ? L("导入表候选：" + result.Recommend + "，实际可用性待确认。", "Import candidate: " + result.Recommend + "; compatibility is unverified.")
                    : L("没有足够证据推荐代理，请手动选择。", "Insufficient evidence to recommend a proxy. Choose manually."));
            foreach (var id in new[] { "mochizuki" })
            {
                BackendInfo capability = null;
                if (result != null && result.Backends != null)
                    foreach (var candidate in result.Backends)
                        if (candidate != null && candidate.Id == id) { capability = candidate; break; }
                var ready = capability != null && capability.Enabled;
                var reason = capability != null ? GetReasonText(capability.ReasonCode, capability.Reason, _language == "zh")
                    : failed ? L("检查失败", "Check failed") : L("未检查", "Not checked");
                RefreshBackendStatus(id, ready, reason);
            }
            RefreshBackendHints();
            RefreshModelImportBar();
            if (!failed) UpdateProxyRecommendation();
        }

        string BackendHintText(BackendInfo backend)
        {
            if (backend == null || !IsBackendSelected(backend.Id)) return "";
            var lines = new List<string>();
            if (backend.MissingPackageFiles != null && backend.MissingPackageFiles.Length > 0)
                lines.Add(L("安装包组件不完整，详见组件详情。", "Package components incomplete; see component details."));
            foreach (var hint in backend.Hints ?? new string[0])
            {
                switch (hint)
                {
                    case "mochi-source": lines.Add(L("缺少 Mochi 模型：请将 nvngx_dlssnr.dll 放到安装包目录。", "Mochi models are missing. Place nvngx_dlssnr.dll in the package folder.")); break;
                    case "mochi-preparable": lines.Add(L("已找到模型来源，安装时自动提取。", "Model source found. Models will be prepared during installation.")); break;
                    case "mochi-versions": lines.Add(L("310.9.16 支持新旧模型；310.8.0 仅支持原始模型。", "310.9.16: both models. 310.8.0: original only.")); break;
                    case "mochi-unsupported-source": lines.Add(L("此 nvngx_dlssnr.dll 版本不受支持，请更换为 310.9.16 或 310.8.0。", "Unsupported nvngx_dlssnr.dll. Supply 310.9.16 or 310.8.0.")); break;
                }
            }
            return string.Join(Environment.NewLine, lines.ToArray());
        }

        void RefreshBackendHints()
        {
            var details = new List<string>();
            foreach (var id in new[] { "mochizuki" })
            {
                BackendInfo backend = null;
                if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                    foreach (var item in _lastInspectResult.Backends)
                        if (item != null && item.Id == id) { backend = item; break; }
                var text = backend != null ? BackendHintText(backend) : "";
                if (IsBackendSelected(id) && backend != null && backend.MissingPackageFiles != null && backend.MissingPackageFiles.Length > 0)
                    details.Add(id + ": " + string.Join(", ", backend.MissingPackageFiles));
                var label = _window.FindName("MochiHint") as TextBlock;
                if (label != null)
                {
                    label.Text = text;
                    label.Visibility = Visibility.Collapsed;
                    label.ToolTip = text;
                }
            }
            var path = _lastInspectResult != null ? _lastInspectResult.PackageRoot : null;
            var pathLabel = _window.FindName("BackendPackagePath") as TextBlock;
            if (pathLabel != null)
            {
                pathLabel.Text = L("安装包目录：", "Package folder: ") + (path ?? "");
                pathLabel.Visibility = Visibility.Collapsed;
            }
            var detailText = (TextBlock)_window.FindName("BackendDetailsText");
            detailText.Text = string.Join(Environment.NewLine, details.ToArray());
            ((Expander)_window.FindName("BackendDetails")).Visibility = details.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            ((Expander)_window.FindName("BackendDetails")).Header = L("组件详情", "Component details");
            SetLabel("BackendDetailsHeading", "安装包中缺少的组件", "Missing package components");
        }

        // Remembers both translations of the latest free-form status message so a language switch can
        // re-render it (previously e.g. the Wiki offline fallback stayed Chinese after switching to English).
        string _statusZh, _statusEn;
        void SetStatus(string zh, string en)
        {
            if (zh == "成功" || zh == "success" || en == "success") { zh = ""; en = ""; }
            _statusZh = zh;
            _statusEn = en;
            if (_statusText != null) _statusText.Text = L(zh, en);
        }

        void RefreshStatusLanguage()
        {
            if (_statusText == null) return;
            if (_session != null && _session.IsRunning)
            {
                _statusText.Text = StageText(_eventStage ?? _session.Action);
                return;
            }
            if (_statusZh != null && (_statusText.Text == _statusZh || _statusText.Text == _statusEn))
            {
                _statusText.Text = L(_statusZh, _statusEn);
                return;
            }
            switch (_statusText.Text)
            {
                case "":
                case "就绪": case "Ready": SetStatus("", ""); break;
                case "正在扫描游戏库…": case "Scanning library…": SetStatus("正在扫描游戏库…", "Scanning library…"); break;
                case "游戏库已更新": case "Library updated": SetStatus("游戏库已更新", "Library updated"); break;
                case "Cancelled.": case "已取消。": SetStatus("已取消。", "Cancelled."); break;
                case "success": case "成功": SetStatus("", ""); break;
                case "failed": case "失败": SetStatus("失败", "failed"); break;
                case "正在检查安装环境…": case "Inspecting installation…": _statusText.Text = StageText("inspect"); break;
                case "正在检查安装条件…": case "Validating installation…": _statusText.Text = StageText("validate"); break;
                case "正在准备模型…": case "Preparing models…": _statusText.Text = StageText("prepare-model"); break;
                case "正在保存备份…": case "Saving backups…": _statusText.Text = StageText("backup"); break;
                case "正在验证安装结果…": case "Verifying installation…": _statusText.Text = StageText("verify"); break;
                case "操作已结束": case "Operation finished": _statusText.Text = StageText("done"); break;
                case "正在应用更改…": case "Applying changes…": _statusText.Text = StageText("install"); break;
                case "目标文件夹已被另一个安装进程锁定。": case "Target folder is locked by another launcher.":
                    SetStatus("目标文件夹已被另一个安装进程锁定。", "Target folder is locked by another launcher."); break;
                case "正在从 OptiScaler 官方 Wiki 同步推荐资料…": case "Syncing recommendations from the official OptiScaler Wiki…":
                    SetStatus("正在从 OptiScaler 官方 Wiki 同步推荐资料…", "Syncing recommendations from the official OptiScaler Wiki…"); break;
            }
        }

        string StageText(string stage)
        {
            switch (stage)
            {
                case "scan": return L("正在扫描游戏库…", "Scanning library…");
                case "uninstall-plan": return L("正在检查卸载范围…", "Checking files to uninstall…");
                case "uninstall": return L("正在卸载 OptiScaler NR…", "Uninstalling OptiScaler NR…");
                case "inspect": return L("正在检查安装环境…", "Inspecting installation…");
                case "validate": return L("正在检查安装条件…", "Validating installation…");
                case "prepare-model": return L("正在准备模型…", "Preparing models…");
                case "backup": return L("正在保存备份…", "Saving backups…");
                case "verify": return L("正在验证安装结果…", "Verifying installation…");
                case "done": return L("操作已结束", "Operation finished");
                default: return L("正在应用更改…", "Applying changes…");
            }
        }

        void LoadSettings()
        {
            GuiSettings rec = null;
            try
            {
                rec = ReadSettingsWithMigration(_settingsPath, _legacySettingsPath);
            }
            catch { }
            // CurrentUICulture uses the Windows display language, independently of number/date formats.
            // Only an absent/unsupported preference consults it; the resolved choice is saved once.
            var hasSavedLanguage = rec != null && (string.Equals(rec.Language, "zh", StringComparison.OrdinalIgnoreCase)
                || string.Equals(rec.Language, "en", StringComparison.OrdinalIgnoreCase));
            _saveInitialLanguage = !hasSavedLanguage;
            _language = hasSavedLanguage ? rec.Language.ToLowerInvariant() : ResolveLanguage(null, CultureInfo.CurrentUICulture);
            _theme = LauncherTheme.Normalize(rec == null ? null : rec.Theme);
            _installLogExpanded = rec != null && rec.InstallLogExpanded;
            RefreshSectionCards();
            if (rec != null)
            {
                _recentGameId = rec.RecentGameId;
                if (rec.ProxyChoices != null)
                    foreach (var pair in rec.ProxyChoices) if (ProxyRecommendations.Supports(pair.Value)) _proxyChoices[pair.Key] = pair.Value;
                if (rec.OptiPatcherByGame != null)
                    foreach (var pair in rec.OptiPatcherByGame) _optiPatcherByGame[pair.Key] = pair.Value;
                if (rec.OpenRrByGame != null)
                    foreach (var pair in rec.OpenRrByGame) _openRrByGame[pair.Key] = pair.Value;
                if (rec.OpenRrSpoofByGame != null)
                    foreach (var pair in rec.OpenRrSpoofByGame)
                    {
                        var mode=pair.Value==null?null:pair.Value.ToLowerInvariant();
                        if(mode=="full"||mode=="auto")_openRrSpoofByGame[pair.Key]=mode;
                    }
                if (rec.HiddenGames != null)
                    foreach (var pair in rec.HiddenGames) _hiddenGames[pair.Key] = pair.Value;
                if (rec.BackendChoices != null)
                    foreach (var pair in rec.BackendChoices)
                        if (pair.Value != null && pair.Value.Backends != null)
                            _backendChoices[pair.Key] = pair.Value;
                if (rec.GameModes != null)
                    foreach (var pair in rec.GameModes) _gameModes[pair.Key] = pair.Value;
                if (rec.GameReshadeApis != null)
                    foreach (var pair in rec.GameReshadeApis) _gameReshadeApis[pair.Key] = pair.Value;
                if (rec.GameBitnessOverrides != null)
                    foreach (var pair in rec.GameBitnessOverrides)
                        if (pair.Value == 32 || pair.Value == 64) _gameBitnessOverrides[pair.Key] = pair.Value;
                if (rec.GameRenoDx != null)
                    foreach (var pair in rec.GameRenoDx) _gameRenoDx[pair.Key] = pair.Value;
                if (rec.GameLumenite != null)
                    foreach (var pair in rec.GameLumenite) _gameLumenite[pair.Key] = pair.Value;
                if (rec.GameLumeniteZips != null)
                    foreach (var pair in rec.GameLumeniteZips) _gameLumeniteZips[pair.Key] = pair.Value;
                if (rec.McMochiUnlockedByGame != null)
                    foreach (var pair in rec.McMochiUnlockedByGame) _mcMochiUnlockedByGame[pair.Key] = pair.Value;
                if (rec.OpenRrForceUnlockedByGame != null)
                    foreach (var pair in rec.OpenRrForceUnlockedByGame) _openRrForceUnlockedByGame[pair.Key] = pair.Value;
                _isScanOnStartup = rec.ScanOnStartup;
                if (_scanOnStartup != null) _scanOnStartup.IsChecked = _isScanOnStartup;
                if (!string.IsNullOrEmpty(rec.PackageRoot))
                {
                    _diagnosticPackageRoot = rec.PackageRoot;
                    if (_packageRootBox != null) _packageRootBox.Text = _diagnosticPackageRoot;
                }
            }
        }

        void SaveSettings()
        {
            if (_loadingSettings) return;
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var set = new GuiSettings
                {
                    Language = _language,
                    Theme = _theme,
                    InstallLogExpanded = _installLogExpanded,
                    ScanOnStartup = _isScanOnStartup,
                    PackageRoot = _diagnosticPackageRoot,
                    RecentGameId = _selectedGame != null ? _selectedGame.Id : _recentGameId,
                    BackendChoices = _backendChoices,
                    HiddenGames = _hiddenGames,
                    ProxyChoices = _proxyChoices,
                    OptiPatcherByGame = _optiPatcherByGame,
                    OpenRrByGame = _openRrByGame,
                    OpenRrSpoofByGame = _openRrSpoofByGame,
                    GameModes = _gameModes,
                    GameReshadeApis = _gameReshadeApis,
                    GameBitnessOverrides = _gameBitnessOverrides,
                    GameRenoDx = _gameRenoDx,
                    GameLumenite = _gameLumenite,
                    GameLumeniteZips = _gameLumeniteZips,
                    McMochiUnlockedByGame = _mcMochiUnlockedByGame,
                    OpenRrForceUnlockedByGame = _openRrForceUnlockedByGame
                };
                JsonUtil.WriteFile(_settingsPath, set);
            }
            catch
            {
                try {
                    GuiSettings fallback = File.Exists(_legacySettingsPath) ? JsonUtil.Read<GuiSettings>(File.ReadAllText(_legacySettingsPath)) : null;
                    fallback = fallback ?? new GuiSettings();
                    fallback.Language = _language; fallback.Theme = _theme;
                    fallback.InstallLogExpanded = _installLogExpanded;
                    fallback.ScanOnStartup = _isScanOnStartup;
                    Directory.CreateDirectory(Path.GetDirectoryName(_legacySettingsPath));
                    JsonUtil.WriteFile(_legacySettingsPath, fallback);
                } catch { }
            }
        }

        static GuiSettings ReadSettingsWithMigration(string localPath, string legacyPath)
        {
            try {
                if (File.Exists(localPath)) return JsonUtil.Read<GuiSettings>(File.ReadAllText(localPath));
                if (!File.Exists(legacyPath)) return null;
                var old = JsonUtil.Read<GuiSettings>(File.ReadAllText(legacyPath));
                if (old == null) return null;
                var preferences = new GuiSettings {
                    Language = old.Language, Theme = old.Theme, ScanOnStartup = old.ScanOnStartup,
                    InstallLogExpanded = old.InstallLogExpanded
                };
                try {
                    Directory.CreateDirectory(Path.GetDirectoryName(localPath));
                    JsonUtil.WriteFile(localPath, preferences);
                } catch { }
                return preferences;
            } catch { return null; }
        }

        void LoadLibraryCache()
        {
            try
            {
                if (!File.Exists(_libraryPath)) return;
                var text = File.ReadAllText(_libraryPath);
                var rec = JsonUtil.Read<LibraryCache>(text);
                if (rec != null && rec.Games != null)
                {
                    _allGames.Clear();
                    bool migrated = false;
                    foreach (var g in rec.Games)
                    {
                        if (g != null && (GamePaths.IsWindowsAppsPath(g.Exe) || GamePaths.IsWindowsAppsPath(g.Root)))
                        {
                            migrated = true;
                            continue;
                        }
                        if (g != null && (!string.IsNullOrEmpty(g.Exe) || !string.IsNullOrEmpty(g.Root)) &&
                            (g.ExeOverride == true || g.Source == "Manual" || g.HasExecutable))
                        {
                            if (string.IsNullOrEmpty(g.Id)) { g.Id = "manual:" + Guid.NewGuid().ToString("N"); migrated = true; }
                            if (string.IsNullOrEmpty(g.Root) && !string.IsNullOrEmpty(g.Exe)) { g.Root = Path.GetDirectoryName(g.Exe); migrated = true; }
                            if (g.Source == "Xbox") { g.Source = "XBOX"; migrated = true; }

                            _allGames.Add(g);
                        }
                    }
                    var count = _allGames.Count;
                    MergeScannedGames(new GameInfo[0]);
                    if (_allGames.Count != count || migrated) SaveLibraryCache();
                }
            }
            catch { }
        }

        void SaveLibraryCache()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var cache = new LibraryCache { Games = _allGames.ToArray() };
                var tmp = _libraryPath + ".tmp";
                JsonUtil.WriteFile(tmp, cache);
                File.Copy(tmp, _libraryPath, true);
                File.Delete(tmp);
            }
            catch { }
        }

        [System.Runtime.Serialization.DataContract]
        sealed class GuiSettings
        {
            [System.Runtime.Serialization.DataMember] public Dictionary<string, string> ProxyChoices;
            [System.Runtime.Serialization.DataMember(Name = "optiPatcherByGame")] public Dictionary<string, bool> OptiPatcherByGame;
            [System.Runtime.Serialization.DataMember(Name = "openRrByGame")] public Dictionary<string, bool> OpenRrByGame;
            [System.Runtime.Serialization.DataMember(Name = "openRrSpoofByGame")] public Dictionary<string, string> OpenRrSpoofByGame;
            [System.Runtime.Serialization.DataMember] public Dictionary<string, string> HiddenGames;
            [System.Runtime.Serialization.DataMember] public string Language;
            [System.Runtime.Serialization.DataMember] public string Theme;
            [System.Runtime.Serialization.DataMember] public bool InstallLogExpanded;
            [System.Runtime.Serialization.DataMember] public bool ScanOnStartup = true;
            [System.Runtime.Serialization.DataMember] public string PackageRoot;
            [System.Runtime.Serialization.DataMember] public string RecentGameId;
            [System.Runtime.Serialization.DataMember] public Dictionary<string, BackendChoice> BackendChoices;
            [System.Runtime.Serialization.DataMember(Name = "gameModes", EmitDefaultValue = false)] public Dictionary<string, string> GameModes;
            [System.Runtime.Serialization.DataMember(Name = "gameReshadeApis", EmitDefaultValue = false)] public Dictionary<string, string> GameReshadeApis;
            [System.Runtime.Serialization.DataMember(Name = "gameBitnessOverrides", EmitDefaultValue = false)] public Dictionary<string, int> GameBitnessOverrides;
            [System.Runtime.Serialization.DataMember(Name = "gameRenoDx", EmitDefaultValue = false)] public Dictionary<string, string> GameRenoDx;
            [System.Runtime.Serialization.DataMember(Name = "gameLumenite", EmitDefaultValue = false)] public Dictionary<string, string> GameLumenite;
            [System.Runtime.Serialization.DataMember(Name = "gameLumeniteZips", EmitDefaultValue = false)] public Dictionary<string, string> GameLumeniteZips;
            [System.Runtime.Serialization.DataMember(Name = "mcMochiUnlockedByGame", EmitDefaultValue = false)] public Dictionary<string, bool> McMochiUnlockedByGame;
            [System.Runtime.Serialization.DataMember(Name = "openRrForceUnlockedByGame", EmitDefaultValue = false)] public Dictionary<string, bool> OpenRrForceUnlockedByGame;
        }

        [System.Runtime.Serialization.DataContract]
        sealed class BackendChoice
        {
            [System.Runtime.Serialization.DataMember] public string[] Backends;
            [System.Runtime.Serialization.DataMember] public string ActiveBackend;
        }

        string BackendChoiceKey()
        {
            return _selectedGame == null ? null : (!string.IsNullOrEmpty(_selectedGame.Id) ? _selectedGame.Id : _selectedGame.Exe);
        }

        BackendChoice CurrentBackendChoice()
        {
            BackendChoice choice;
            var key = BackendChoiceKey();
            return !string.IsNullOrEmpty(key) && _backendChoices.TryGetValue(key, out choice) ? choice : null;
        }

        List<string> CheckedBackends()
        {
            var list = new List<string>();
            if (_mochiCheck != null && _mochiCheck.IsChecked == true) list.Add("mochizuki");
            return list;
        }

        void SaveBackendChoice()
        {
            var key = BackendChoiceKey();
            if (_loadingSettings || _updatingBackends || string.IsNullOrEmpty(key)) return;
            var list = CheckedBackends();
            _backendChoices[key] = new BackendChoice { Backends = list.ToArray(), ActiveBackend = list.Contains(_activeBackend) ? _activeBackend : "" };
            SaveSettings();
        }

        void RestoreBackendChoice()
        {
            if (IsMcMochiLocked())
            {
                _updatingBackends = true;
                try
                {
                    if (_mochiCheck != null)
                    {
                        _mochiCheck.IsChecked = false;
                        _mochiCheck.IsEnabled = false;
                    }
                    _activeBackend = "";
                    UpdateActiveCombo();
                }
                finally { _updatingBackends = false; }
                return;
            }
            var choice = CurrentBackendChoice();
            var list = new List<string>(choice != null ? choice.Backends : new[] { "mochizuki" });
            _updatingBackends = true;
            try
            {
                if (_mochiCheck != null) _mochiCheck.IsChecked = list.Contains("mochizuki") || choice == null;
                _activeBackend = (_mochiCheck != null && _mochiCheck.IsChecked == true) ? "mochizuki" : "";
                UpdateActiveCombo();
            }
            finally { _updatingBackends = false; }
        }

        [System.Runtime.Serialization.DataContract]
        sealed class LibraryCache
        {
            [System.Runtime.Serialization.DataMember] public GameInfo[] Games;
        }

        string WorkerScript
        {
            get
            {
                var worker = Path.Combine(_packageRoot, "launcher-worker.ps1");
                if (File.Exists(worker)) return worker;
                throw new FileNotFoundException(L("请从完整解压的安装包启动 OptiScalerNRLauncher.exe。若需指定其他安装包，请先确认开启诊断模式。", "Run OptiScalerNRLauncher.exe from the complete extracted package. Enable diagnostic mode explicitly to select another package."));
            }
        }

        void BrowsePackageRoot()
        {
            if (!_diagnosticMode || !CanChangePackageSource()) return;
            var dlg = new OpenFileDialog
            {
                Title = L("选择完整安装包目录（选择 Setup.ps1 或 OptiScaler.dll）", "Select package folder (pick Setup.ps1 or OptiScaler.dll)"),
                Filter = L("安装包文件 (Setup.ps1;OptiScaler.dll)|Setup.ps1;OptiScaler.dll|所有文件 (*.*)|*.*", "Package files (Setup.ps1;OptiScaler.dll)|Setup.ps1;OptiScaler.dll|All files (*.*)|*.*"),
                CheckFileExists = true
            };
            if (dlg.ShowDialog(_window) == true)
            {
                SetDiagnosticPackageRoot(Path.GetDirectoryName(dlg.FileName));
            }
        }

        void AddGame()
        {
            var picked = GamePaths.PickExe(_language == "zh");
            if (string.IsNullOrEmpty(picked)) return;
            AddGameFromPath(picked);
        }

        static bool IsDirectoryInstalled(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return false;
            return File.Exists(Path.Combine(dir, "amd-presr-install.txt")) ||
                   !string.IsNullOrEmpty(PeProbe.DetectInstalledMode(dir));
        }

        void AddGameFromPath(string exePath)
        {
            if (RejectWindowsApps(exePath)) return;
            var norm = GamePaths.Normalize(exePath);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;
            var restored = new List<string>();
            foreach (var pair in _hiddenGames)
                if (string.Equals(pair.Value, norm, StringComparison.OrdinalIgnoreCase)) restored.Add(pair.Key);
            foreach (var id in restored) _hiddenGames.Remove(id);
            SaveSettings();

            GameInfo found = null;
            foreach (var g in _allGames)
            {
                if (string.Equals(g.Exe, norm, StringComparison.OrdinalIgnoreCase))
                {
                    found = g;
                    break;
                }
            }
            if (found == null)
            {
                foreach (var candidate in _allGames)
                    if (!candidate.HasExecutable && !string.IsNullOrEmpty(candidate.RootDirectory) &&
                        norm.StartsWith(candidate.RootDirectory.TrimEnd('\\','/') + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
                        (found == null || candidate.RootDirectory.Length > found.RootDirectory.Length)) found = candidate;
            }
            if (found == null)
            {
                var dir = Path.GetDirectoryName(norm);
                var folderName = Path.GetFileName(dir);
                var name = Path.GetFileNameWithoutExtension(norm);
                if (!string.IsNullOrEmpty(folderName) && !folderName.Equals("bin", StringComparison.OrdinalIgnoreCase)
                    && !folderName.Equals("win64", StringComparison.OrdinalIgnoreCase) && !folderName.Equals("wingdk", StringComparison.OrdinalIgnoreCase))
                {
                    name = folderName;
                }
                found = new GameInfo
                {
                    Id = "manual:" + Guid.NewGuid().ToString("N"),
                    Name = name,
                    Exe = norm,
                    Root = dir,
                    Source = "Manual",
                    Installed = IsDirectoryInstalled(dir)
                };
                _allGames.Add(found);
            }

            // Picking an existing scanned entry is also an explicit EXE choice.
            found.Exe = norm;
            found.ExeOverride = true;
            found.Installed = IsDirectoryInstalled(Path.GetDirectoryName(norm));
            SaveLibraryCache();
            RecheckPackageComparison(found);
            RefreshGameListView();
            SelectGame(found);
        }

        void ChangeExe()
        {
            if (_selectedGame == null) return;
            var picked = GamePaths.PickExe(_language == "zh", _selectedGame.HasExecutable ? Path.GetDirectoryName(_selectedGame.Exe) : _selectedGame.RootDirectory);
            if (string.IsNullOrEmpty(picked)) return;
            BindExecutable(_selectedGame, picked);
        }

        void InitGameContextMenu()
        {
            if (_gameList == null) return;
            var menu = new ContextMenu();
            menu.Style = (Style)_window.FindResource("GameContextMenu");
            _removeGameMenuItem = new MenuItem { Header = L("从列表移除", "Remove from list"), IsEnabled = false };
            _removeGameMenuItem.Style = (Style)_window.FindResource("GameContextMenuItem");
            _removeGameMenuItem.Click += (s, e) => RemoveGame(_removeGameMenuItem.Tag as GameInfo);
            menu.Items.Add(_removeGameMenuItem);
            _gameList.ContextMenu = menu;
            // Right-click targets a row without starting its automatic inspection.
            _gameList.PreviewMouseRightButtonDown += (s, e) =>
            {
                if (ItemsControl.ContainerFromElement(_gameList, e.OriginalSource as DependencyObject) is ListBoxItem)
                    e.Handled = true;
            };
            _gameList.ContextMenuOpening += (s, e) =>
            {
                var row = ItemsControl.ContainerFromElement(_gameList, e.OriginalSource as DependencyObject) as ListBoxItem;
                var item = row == null ? null : row.Content as GameItemViewModel;
                if (item == null && e.CursorLeft < 0) item = _gameList.SelectedItem as GameItemViewModel;
                if (item == null || item.Info == null) { e.Handled = true; return; }
                _removeGameMenuItem.Tag = item.Info;
                _removeGameMenuItem.Header = L("从列表移除", "Remove from list");
                _removeGameMenuItem.IsEnabled = !_writeBusy && (_session == null || !_session.IsRunning);
            };
        }

        void RemoveGame(GameInfo removed)
        {
            if (removed == null || _writeBusy || (_session != null && _session.IsRunning)) return;
            var wasSelected = _selectedGame != null && SameLibraryGame(_selectedGame, removed);
            _hiddenGames[removed.Id ?? ("custom:" + EntryLocation(removed).ToLowerInvariant())] = EntryLocation(removed);
            _allGames.RemoveAll(g => SameLibraryGame(g, removed));
            if (wasSelected) SelectGame(null);
            SaveSettings();
            SaveLibraryCache();
            RefreshGameListView();
            if (wasSelected && _gameList != null && _gameList.Items.Count > 0) _gameList.SelectedIndex = 0;
            if (_statusText != null) SetStatus("已从列表移除；游戏文件未改动。可通过“添加游戏”重新添加。", "Removed from the list; game files were kept. Use Add Game to add it again.");
        }

        void LaunchSelectedGame()
        {
            if (_selectedGame == null || !_selectedGame.HasExecutable) return;
            try
            {
                if (_selectedGame.IsRtxRemixMinecraft)
                {
                    string instDirName;
                    var sdkDir = Path.GetDirectoryName(_selectedGame.Exe);
                    var prismExe = PeProbe.TryFindPrismLauncherExe(sdkDir, out instDirName);
                    if (!string.IsNullOrEmpty(prismExe) && !string.IsNullOrEmpty(instDirName))
                    {
                        Process.Start(new ProcessStartInfo
                        {
                            FileName = prismExe,
                            Arguments = "--launch \"" + instDirName + "\"",
                            WorkingDirectory = Path.GetDirectoryName(prismExe),
                            UseShellExecute = true
                        });
                        if (_statusText != null)
                        {
                            SetStatus("已请求 PrismLauncher 启动：" + _selectedGame.DisplayName, "Requested PrismLauncher to launch: " + _selectedGame.DisplayName);
                        }
                        return;
                    }
                    DialogTheme.Show(_window,
                        L("未在实例上层目录找到 prismlauncher.exe，请通过 PrismLauncher 或 Minecraft 官方启动器启动游戏。",
                          "prismlauncher.exe was not found above this instance. Please launch Minecraft via PrismLauncher or the Minecraft Launcher."),
                        _window.Title, MessageBoxButton.OK, MessageBoxImage.Information);
                    return;
                }
                var workDir = _selectedGame.RootDirectory;
                if (string.IsNullOrEmpty(workDir) || !Directory.Exists(workDir))
                {
                    try { workDir = Path.GetDirectoryName(_selectedGame.Exe); } catch { }
                }

                var psi = new ProcessStartInfo
                {
                    FileName = _selectedGame.Exe,
                    WorkingDirectory = workDir ?? "",
                    UseShellExecute = true
                };
                Process.Start(psi);
                if (_statusText != null)
                {
                    SetStatus("已启动游戏：" + _selectedGame.DisplayName, "Launched game: " + _selectedGame.DisplayName);
                }
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window,
                    L("启动游戏失败：" + ex.Message, "Failed to launch game: " + ex.Message),
                    L("启动失败", "Launch Failed"));
            }
        }

        void OpenSelectedFolder()
        {
            if (_selectedGame == null) return;
            try
            {
                // The install location is the chosen EXE's directory; the added Root
                // (folder-scan games) is only a fallback when no EXE is set. Directory
                // buttons point where the install buttons point.
                string dir = null;
                if (!string.IsNullOrEmpty(_selectedGame.Exe))
                {
                    try { dir = Path.GetDirectoryName(_selectedGame.Exe); } catch { }
                }
                if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                    dir = _selectedGame.RootDirectory;
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                {
                    Process.Start("explorer.exe", dir);
                }
            }
            catch { }
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files == null || files.Length == 0) return;
            var path = files[0];
            if (Directory.Exists(path))
            {
                AddFolderFromPath(path);
                return;
            }
            if (path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                AddGameFromPath(path);
            }
        }

        bool _refreshingFilters;
        string _libraryFilter = "all";
        void RefreshLibraryFilters()
        {
            if (_filterCombo == null || _refreshingFilters) return;
            var selected = _filterCombo.SelectedItem as ComboBoxItem;
            if (selected != null) _libraryFilter = (string)selected.Tag;
            _refreshingFilters = true;
            try {
                _filterCombo.Items.Clear();
                Action<string, string> add = delegate(string id, string label) { var item = new ComboBoxItem { Tag = id, Content = label }; _filterCombo.Items.Add(item); if (id == _libraryFilter) _filterCombo.SelectedItem = item; };
                add("all", L("全部", "All")); add("installed", L("已安装", "Installed")); add("uninstalled", L("未安装", "Not installed"));
                _filterCombo.Items.Add(new Separator { Style = (Style)_window.FindResource("GameContextMenuSeparator"), Width = 74, IsEnabled = false, Focusable = false, IsHitTestVisible = false });
                var sources = new List<string> { "Steam", "XBOX", "Epic", "Manual" };
                foreach (var game in _allGames) if (!string.IsNullOrEmpty(game.Source) && !sources.Exists(s => string.Equals(s, game.Source, StringComparison.OrdinalIgnoreCase))) sources.Add(game.Source);
                foreach (var source in sources) add("source:" + source.ToLowerInvariant(), source);
                if (_filterCombo.SelectedItem == null) { _libraryFilter = "all"; _filterCombo.SelectedItem = _filterCombo.Items[0]; }
            } finally { _refreshingFilters = false; }
        }

        readonly LocalArtwork.Scope _libraryArtwork, _heroArtwork;
        static IEnumerable<LocalArtwork.Request> LibraryArtworkRequests(IEnumerable<GameItemViewModel> items, int pixels)
        {
            foreach (var item in items) {
                var current = item;
                string iconPath = current.HasCover ? current.Cover : current.Info.Exe;
                if (!current.HasCover && current.Info != null && current.Info.IsRtxRemixMinecraft && !string.IsNullOrEmpty(current.Info.Exe))
                {
                    string instDirName;
                    var prismExe = PeProbe.TryFindPrismLauncherExe(Path.GetDirectoryName(current.Info.Exe), out instDirName);
                    if (!string.IsNullOrEmpty(prismExe)) iconPath = prismExe;
                }
                yield return new LocalArtwork.Request { Path = iconPath,
                    Pixels = pixels, Icon = !current.HasCover, Complete = image => current.IconSource = image };
            }
        }

        int ArtworkPixels(int dips)
        {
            var source = PresentationSource.FromVisual(_window);
            return (int)Math.Ceiling(dips * (source != null && source.CompositionTarget != null ? source.CompositionTarget.TransformToDevice.M11 : 1));
        }

        void RefreshGameListView()
        {
            RefreshLibraryFilters();
            var query = (_searchBox != null ? _searchBox.Text : "").Trim().ToLowerInvariant();
            var filter = _filterCombo != null ? _filterCombo.SelectedItem as ComboBoxItem : null;
            var filterId = filter != null ? (string)filter.Tag : _libraryFilter;
            var isZh = _language == "zh";

            var list = new List<GameItemViewModel>();
            foreach (var g in _allGames)
            {
                if (filterId == "installed" && !g.Installed) continue;
                if (filterId == "uninstalled" && g.Installed) continue;
                if (filterId.StartsWith("source:", StringComparison.Ordinal) && !string.Equals(filterId.Substring(7), g.Source, StringComparison.OrdinalIgnoreCase)) continue;
                if (!string.IsNullOrEmpty(query))
                {
                    var matchName = (g.DisplayName ?? "").ToLowerInvariant().Contains(query) || (g.Name ?? "").ToLowerInvariant().Contains(query);
                    var matchExe = g.Exe != null && g.Exe.ToLowerInvariant().Contains(query);
                    if (!matchName && !matchExe) continue;
                }
                bool? packageDiffers;
                TryGetPackageDifference(g, out packageDiffers);
                var item = new GameItemViewModel {
                    Info = g, IsZh = isZh, PackageDiffers = packageDiffers == true,
                    PackageMatches = packageDiffers == false, InstalledStatusText = PackageStatusText(g)
                };
                list.Add(item);
            }

            if (_gameList != null)
            {
                _gameList.ItemsSource = list;
                if (_selectedGame != null)
                {
                    for (int i = 0; i < list.Count; i++)
                    {
                        if (list[i].Info != null && string.Equals(list[i].Info.Id, _selectedGame.Id, StringComparison.OrdinalIgnoreCase))
                        {
                            _gameList.SelectedIndex = i;
                            break;
                        }
                    }
                }
            }
            _libraryArtwork.Replace(LibraryArtworkRequests(list, ArtworkPixels(42)));

            if (_countBadge != null) _countBadge.Text = list.Count.ToString();
            if (_countText != null)
            {
                _countText.Text = isZh
                    ? string.Format("共 {0} 款游戏", list.Count)
                    : string.Format("{0} games", list.Count);
            }
        }

        void OnGameSelected()
        {
            if (_gameList == null) return;
            var item = _gameList.SelectedItem as GameItemViewModel;
            if (item != null && item.Info != null)
            {
                if (_selectedGame != null && string.Equals(_selectedGame.Id, item.Info.Id, StringComparison.OrdinalIgnoreCase))
                    return;
                SelectGame(item.Info);
            }
        }

        void SelectGame(GameInfo game)
        {
            if (_writeBusy) return;
            if (_session != null && _session.CanKill && _session.Action == "inspect") CancelReadSession();
            if (_folderScan != null) _folderScan.Cancel();
            SelectProxy(null);
            _decisions.ForeignProxy.Clear();
            _resumeInstallAction = "";
            if (game == null || _selectedGame == null || !SameLibraryGame(game, _selectedGame)) _decisions.ModelSourceDll = null;
            _modeAppliedForKey = "";
            _lastPeInfo = null;
            InvalidateNativeDlssdCache();
            _selectedGame = game;
            _detectedGameBitness = game != null && game.HasExecutable ? PeProbe.ReadGameBitness(game.Exe) : 0;
            SelectGameProfiles();
            _lastInspectResult = null;
            _heroArtwork.Replace(null);
            if (game == null) {
                _recentGameId = null;
                if (_heroTitle != null) _heroTitle.Text = L("选择一个游戏以开始", "Select a game to begin");
                if (_heroMeta != null) _heroMeta.Text = "";
                if (_heroPath != null) _heroPath.Text = "";
                if (_heroPosterImage != null) _heroPosterImage.Source = null;
                if (_heroBgImage != null) _heroBgImage.Source = null;
                ApplyModeVisibility();
                RefreshInspectionText();
                UpdatePrimaryButtonState();
                return;
            }
            RestoreBackendChoice();
            ApplyModeVisibility();
            RefreshInspectionText();
            if (_heroTitle != null) _heroTitle.Text = game.DisplayName;
            if (_heroMeta != null) _heroMeta.Text = game.Source;
            if (_heroPath != null) _heroPath.Text = EntryLocation(game);

            if (_heroBgImage != null) {
                _heroBgImage.Source = null;
                var identity = game;
                LocalArtwork.Load(_heroArtwork, game.DisplayHero, ArtworkPixels(1000), false, true, delegate(ImageSource image) {
                    if (ReferenceEquals(_selectedGame, identity)) _heroBgImage.Source = image;
                });
            }
            // Hidden legacy poster fields remain readable, but never supply the hero background.
            if (_heroPosterImage != null) _heroPosterImage.Source = null;

            UpdatePrimaryButtonState();
            InspectGame(game);
        }

        void UpdatePrimaryActionText()
        {
            UpdateModeBanners();
            if (_primaryButton == null) return;
            if (_selectedGame == null) {
                _primaryButton.Content = L("选择一个游戏", "Select a game");
                _primaryButton.ToolTip = L("选择游戏后设置安装选项。", "Select a game to configure installation.");
            } else if (!_selectedGame.HasExecutable) {
                _primaryButton.Content = L("扫描 / 选择启动程序", "Find / choose game EXE");
                _primaryButton.ToolTip = L("选择此游戏的启动程序，不会开始安装。", "Choose this game's executable; this does not install anything.");
            } else if (IsRtxRemixMinecraftSelected() && EffectiveGameBitness() == 32) {
                _primaryButton.Content = L("不支持 32 位程序", "32-bit EXE unsupported");
                _primaryButton.ToolTip = L("不支持 32 位程序：请改选 64 位 (x64) 主程序。", "32-bit executable is not supported; please select the 64-bit (x64) executable.");
            } else if (!_selectedGame.Installed) {
                var selMode = GetSelectedInstallMode();
                if (IsRtxRemixMinecraftSelected()) {
                    _primaryButton.Content = L("安装 RTX Remix 补丁", "Install RTX Remix Patch");
                    _primaryButton.ToolTip = L("向 remix-sdk\\ 原名部署 OptiScaler.dll、应用 AMD remixbridge.dll 补丁（自动备份原版）并安装所选插件。", "Deploy unrenamed OptiScaler.dll, AMD remixbridge.dll patch (backing up the original), and selected plugins into remix-sdk\\.");
                } else if (!IsOptiScalerSelectedMode()) {
                    _primaryButton.Content = L("安装 " + FormatModeShortTag(selMode), "Install " + FormatModeShortTag(selMode));
                    _primaryButton.ToolTip = L("按所选模式、网络与配置选项安装。", "Install using the selected mode, networks and configuration.");
                } else {
                    _primaryButton.Content = L("安装 OptiScaler NR", "Install OptiScaler NR");
                    _primaryButton.ToolTip = L("按所选后端、模型、代理和 INI 选项安装。", "Install with the selected backends, models, proxy and INI options.");
                }
            } else {
                var installedMode = CurrentInstalledModeForSelectedGame();
                var selectedMode = GetSelectedInstallMode();
                bool modeSwitch = !string.IsNullOrEmpty(installedMode) &&
                    !string.Equals(installedMode, selectedMode, StringComparison.OrdinalIgnoreCase);
                if (modeSwitch) {
                    _primaryButton.Content = L("卸载旧模式并安装 " + FormatModeShortTag(selectedMode), "Switch & install " + FormatModeShortTag(selectedMode));
                    _primaryButton.ToolTip = L("检测到当前已安装其他模式：将先自动卸载旧文件（保留已有模型权重），再安装所选新模式。", "Another mode is currently installed: will cleanly uninstall previous files first (preserving model weights), then install the selected mode.");
                } else if (IsRtxRemixMinecraftSelected()) {
                    _primaryButton.Content = PackageDiffers(_selectedGame)
                        ? L("更新 RTX Remix 补丁", "Update RTX Remix Patch")
                        : L("重新安装 / 应用更改", "Reinstall / apply changes");
                    _primaryButton.ToolTip = PackageDiffers(_selectedGame)
                        ? L("已安装的 RTX Remix 补丁 / OptiScaler NR 与当前选择的安装包不同。按当前选项更新，将先清理并保留权重、配置及 NVIDIA 原版备份。", "The installed RTX Remix patch / OptiScaler NR differs from the package you selected. Updating cleanly resets files while preserving weights, INI (if unchecked) and the original NVIDIA backup.")
                        : L("重新安装并应用当前插件与配置选项。取消勾选“覆盖现有 INI”可保留现有配置。", "Reinstall and apply your current plugins and configuration options. Uncheck Replace INI to keep your current configuration.");
                } else {
                    _primaryButton.Content = PackageDiffers(_selectedGame)
                        ? L("更新 OptiScaler NR", "Update OptiScaler NR")
                        : L("重新安装 / 应用更改", "Reinstall / apply changes");
                    _primaryButton.ToolTip = PackageDiffers(_selectedGame)
                        ? L("已安装的 OptiScaler NR 与当前选择的安装包不同（文件哈希不同），不一定是旧版本。按当前选项更新，无需先卸载；取消勾选“覆盖现有 INI”可保留现有配置。", "The installed OptiScaler NR differs from the package you selected (different file hashes); it is not necessarily older. Update with your current options without uninstalling first. Uncheck Replace INI to keep your current configuration.")
                        : L("重新安装并应用当前模式、模型和配置选项。取消勾选“覆盖现有 INI”可保留现有配置。", "Reinstall and apply your current mode, model and configuration options. Uncheck Replace INI to keep your current configuration.");
                }
            }
        }

        void UpdatePrimaryButtonState()
        {
            UpdateCompatibilitySummary();
            RefreshLibraryActions();
            RefreshProfileActions();
            RefreshHeroState();
            if (_removeGameMenuItem != null) _removeGameMenuItem.IsEnabled = _removeGameMenuItem.Tag is GameInfo && !_writeBusy && (_session == null || !_session.IsRunning);
            if (_primaryButton == null) return;
            if (_presentationBusy) { _primaryButton.IsEnabled = false; return; }
            if (_selectedGame == null)
            {
                _primaryButton.IsEnabled = false;
                if (_launchGameButton != null) _launchGameButton.IsEnabled = false;
                if (_uninstallButton != null) _uninstallButton.IsEnabled = false;
                return;
            }

            var hasBackend = true;

            var proxyChosen = !IsOptiScalerSelectedMode() || IsRtxRemixMinecraftSelected() || !string.IsNullOrEmpty(CurrentSelectedProxy);
            var is32BitBlocked = IsRtxRemixMinecraftSelected() && EffectiveGameBitness() == 32;
            var isBusy = _session != null && _session.IsRunning && (_presentationBusy || !_session.CanKill);
            if (!_selectedGame.HasExecutable) {
                _primaryButton.IsEnabled = !isBusy && !_writeBusy && _folderScan == null;
                if (_launchGameButton != null) _launchGameButton.IsEnabled = false;
                if (_uninstallButton != null) _uninstallButton.IsEnabled = false;
                return;
            }

            if (_launchGameButton != null)
                _launchGameButton.IsEnabled = !isBusy && !_writeBusy && _folderScan == null;

            _primaryButton.IsEnabled = !isBusy && !_writeBusy && _folderScan == null && hasBackend && proxyChosen && !is32BitBlocked;
            if (_uninstallButton != null)
                _uninstallButton.IsEnabled = !isBusy && _selectedGame.Installed;
        }

        void UpdateActiveCombo()
        {
            UpdateActiveUI();
        }

        void RunScan(bool background = false)
        {
            if (_session != null && _session.IsRunning) return;
            if (!background) SetBusy(true, "scan");
            if (_statusText != null) _statusText.Text = _language == "zh" ? "正在扫描游戏库…" : "Scanning library…";
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
            if (_progress != null && !background) _progress.IsIndeterminate = true;

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = "scan",
                PackageRoot = _packageRoot
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = WorkerSession.Start(WorkerScript, req, work, canKill: true);
                _session.EventReceived += OnEvent;
                _session.Completed += OnScanCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                AppendLog("Scan error: " + ex.Message);
            }
        }

        static bool SameLibraryGame(GameInfo a, GameInfo b)
        {
            return (!string.IsNullOrEmpty(a.Id) && !string.IsNullOrEmpty(b.Id) &&
                    string.Equals(LibraryId(a.Id, a.RootDirectory), LibraryId(b.Id, b.RootDirectory), StringComparison.OrdinalIgnoreCase)) ||
                   SamePath(a.Exe, b.Exe) ||
                   ((string.IsNullOrEmpty(a.Exe) || string.IsNullOrEmpty(b.Exe)) && SamePath(a.RootDirectory, b.RootDirectory));
        }

        static bool IsAuxiliaryExe(string path)
        {
            var name = Path.GetFileName(path);
            return string.Equals(name, "gamelaunchhelper.exe", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(name, "person-worker.exe", StringComparison.OrdinalIgnoreCase) ||
                   FolderGameScanner.Auxiliary(path) ||
                   path.IndexOf("\\person-model\\", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   path.IndexOf("\\OptiScaler\\", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        static bool KeepCachedExe(GameInfo game)
        {
            if (game.ExeOverride == true || game.Source == "Manual") return true;
            // Legacy caches have no provenance. Correct clear helper/missing paths,
            // but preserve a plausible existing EXE that could have been chosen by hand.
            return !game.ExeOverride.HasValue && GamePaths.CanUse(game.Exe) && !IsAuxiliaryExe(game.Exe);
        }

        bool MergeScannedGames(GameInfo[] scanned)
        {
            var previousSelection = _selectedGame;
            var previousExe = previousSelection != null ? previousSelection.Exe : null;
            var previousName = previousSelection != null ? previousSelection.Name : null;
            var merged = new List<GameInfo>();
            var slots = new LibraryIndex();
            var hidden = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var hiddenIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var pair in _hiddenGames)
            {
                hiddenIds.Add(LibraryId(pair.Key, pair.Value));
                var key = PathKey(pair.Value);
                if (key != null) hidden.Add(key);
            }
            Predicate<GameInfo> skip = g => g == null || (string.IsNullOrEmpty(g.Exe) && string.IsNullOrEmpty(g.Root)) ||
                hiddenIds.Contains(LibraryId(g.Id, g.RootDirectory)) || hidden.Contains(PathKey(EntryLocation(g)));
            foreach (var cached in _allGames)
            {
                if (skip(cached)) continue;
                var index = slots.Find(cached);
                if (index < 0)
                {
                    slots.Set(cached, merged.Count, true);
                    merged.Add(cached);
                }
                else
                {
                    var existing = merged[index];
                    if (string.IsNullOrEmpty(existing.CustomName)) existing.CustomName = cached.CustomName;
                    if (string.IsNullOrEmpty(existing.CustomCover)) existing.CustomCover = cached.CustomCover;
                    var explicitChoice = cached.ExeOverride == true || cached.Source == "Manual";
                    var existingExplicit = existing.ExeOverride == true || existing.Source == "Manual";
                    if ((explicitChoice && !existingExplicit) ||
                        (explicitChoice == existingExplicit && KeepCachedExe(cached) && !KeepCachedExe(existing)))
                    {
                        if (string.IsNullOrEmpty(cached.CustomName)) cached.CustomName = existing.CustomName;
                        if (string.IsNullOrEmpty(cached.CustomCover)) cached.CustomCover = existing.CustomCover;
                        slots.Set(existing, index, false);
                        merged[index] = cached;
                        slots.Set(cached, index, true);
                    }
                }
            }
            foreach (var incoming in scanned)
            {
                if (skip(incoming) || GamePaths.IsWindowsAppsPath(incoming.Exe) || GamePaths.IsWindowsAppsPath(incoming.Root)) continue;
                var index = slots.Find(incoming);
                if (index < 0)
                {
                    slots.Set(incoming, merged.Count, true);
                    incoming.ExeOverride = false;
                    merged.Add(incoming);
                    continue;
                }
                var existing = merged[index];
                slots.Set(existing, index, false);
                if (!KeepCachedExe(existing) || (!existing.HasExecutable && incoming.HasExecutable))
                {
                    existing.Exe = incoming.Exe;
                    existing.ExeOverride = false;
                }
                if (existing.Source != "Manual" && string.Equals(LibraryId(existing.Id, existing.RootDirectory), LibraryId(incoming.Id, incoming.RootDirectory), StringComparison.OrdinalIgnoreCase))
                {
                    existing.Name = incoming.Name;
                    existing.Source = incoming.Source;
                }
                if (!string.IsNullOrEmpty(incoming.TargetKind)) existing.TargetKind = incoming.TargetKind;
                if (!string.IsNullOrEmpty(incoming.Root)) existing.Root = incoming.Root;
                if (!string.IsNullOrEmpty(incoming.Cover)) existing.Cover = incoming.Cover;
                if (!string.IsNullOrEmpty(incoming.Hero)) existing.Hero = incoming.Hero;
                slots.Set(existing, index, true);
            }
            foreach (var game in merged)
                game.Installed = game.HasExecutable && IsDirectoryInstalled(Path.GetDirectoryName(game.Exe));
            _allGames.Clear();
            _allGames.AddRange(merged);
            if (previousSelection != null)
            {
                var index = slots.Find(previousSelection);
                _selectedGame = index < 0 ? previousSelection : merged[index];
            }
            return previousSelection != null &&
                (!ReferenceEquals(previousSelection, _selectedGame) ||
                 !string.Equals(previousExe, _selectedGame.Exe, StringComparison.OrdinalIgnoreCase) ||
                 previousName != _selectedGame.Name);
        }

        void OnScanCompleted(WorkerSession source, LauncherResult result, int code)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!ReferenceEquals(_session, source))
                {
                    Detach(source);
                    return;
                }
                Detach(source);
                _session = null;
                SetBusy(false, "");
                if (_progress != null) _progress.IsIndeterminate = false;

                if (code != 0 || result == null || result.Status != "success" || result.Games == null)
                {
                    if (result != null && !string.IsNullOrEmpty(result.LogPath)) _currentLogPath = result.LogPath;
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    if (_statusText != null) SetStatus("游戏库扫描失败", "Library scan failed");
                    return;
                }
                var selectionChanged = MergeScannedGames(result.Games);
                SaveLibraryCache();
                RefreshPackageComparisons(_packageRoot, true);

                if (_statusLed != null) _statusLed.Fill = ReadyBrush;
                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "游戏库已更新" : "Library updated";

                if (selectionChanged) SelectGame(_selectedGame);
                else if (_selectedGame == null && _allGames.Count > 0 && _gameList != null && _gameList.Items.Count > 0)
                {
                    _gameList.SelectedIndex = 0;
                }
            }));
        }

        void CancelReadSession()
        {
            var old = _session;
            _session = null; // Invalidate even a completion already queued on the dispatcher.
            _pendingAction = "";
            old.CancelInspect();
            Detach(old);
            SetBusy(false, "");
            if (_progress != null) _progress.IsIndeterminate = false;
        }

        void InspectGame(GameInfo game)
        {
            if (_writeBusy) return;
            if (game == null || !game.HasExecutable) { RefreshInspectionText(); return; }

            _lastInspectResult = null;
            InvalidateNativeDlssdCache();
            var fastProxy = PeProbe.QuickRecommend(game.Exe);
            UpdateProxyRecommendation(fastProxy);
            _decisions.OptiPatcher = (_optiPatcherToggle != null && _optiPatcherToggle.IsEnabled && _optiPatcherToggle.IsChecked == true) ? "install" : "skip";
            _decisions.OpenRr = (!IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true) ? "install" : "skip";
            if (TryFastInspect(game, fastProxy)) return;

            if (_session != null)
            {
                if (_session.CanKill) CancelReadSession();
                else return;
            }

            SetBusy(true, "inspect");
            _pendingAction = "inspect";
            if (_injectText != null) _injectText.Text = _language == "zh" ? "正在检查导入与后端…" : "Checking imports and backends…";
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
            if (_progress != null) _progress.IsIndeterminate = true;
            UpdatePrimaryButtonState();

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = "inspect",
                GameExe = game.Exe,
                ProxyChoice = "auto",
                BackendChoice = "auto",
                ExplicitDecisions = _decisions,
                PackageRoot = _packageRoot
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = WorkerSession.Start(WorkerScript, req, work, canKill: true);
                _session.EventReceived += OnEvent;
                _session.Completed += OnCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                AppendLog("Inspect failed: " + ex.Message);
            }
        }

        void OnPrimaryAction()
        {
            if (_selectedGame == null) return;
            if (!_selectedGame.HasExecutable) { ScanExecutables(); return; }
            if (WarnPendingActions()) return;
            var action = _selectedGame.Installed ? "update" : "install";
            ExecuteAction(action);
        }

        void OnUninstall()
        {
            if (_selectedGame == null || !_selectedGame.HasExecutable) return;
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            var installedMode = CurrentInstalledModeForSelectedGame();
            var modeTag = string.IsNullOrEmpty(installedMode) ? "" : (" [" + FormatModeShortTag(installedMode) + "]");
            var isZh = _language == "zh";
            var msg = isZh
                ? ("确定要从以下目录完整卸载 OptiScaler NR" + modeTag + " 吗？\n" + dir + "\n\n将还原备份的游戏原文件，并清理本目录的 OptiScaler / dlssnr-amd / openrr-amd 等组件及模型、缓存；公共 %LOCALAPPDATA%\\DLSSNR-AMD 数据不会受到影响。")
                : ("Uninstall OptiScaler NR" + modeTag + " from:\n" + dir + "\n\nThis restores backed-up game files and removes the project's OptiScaler / dlssnr-amd / openrr-amd folders, models, and caches from this directory. Shared data under %LOCALAPPDATA%\\DLSSNR-AMD is untouched.");

            var confirm = DialogTheme.Show(_window, msg, _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (confirm != MessageBoxResult.OK) return;

            ExecuteAction("uninstall");
        }

        string XessGroupLabel(string id)
        {
            switch (id)
            {
                case "fg": return L("XeSS 插帧 + XeLL（配套替换）", "XeSS frame generation + XeLL (replace together)");
                case "dx11": return L("XeSS 超分（DX11）", "XeSS upscaling (DX11)");
                case "sr": return L("XeSS 超分（DX12 / Vulkan）", "XeSS upscaling (DX12 / Vulkan)");
                case "fsr_fg": return L("AMD FidelityFX 帧生成（DX12）", "AMD FidelityFX frame generation (DX12)");
                case "fsr_upscaler": return L("AMD FidelityFX 超分（DX12）", "AMD FidelityFX upscaler (DX12)");
                case "fsr_loader": return L("AMD FidelityFX 加载器（DX12）", "AMD FidelityFX loader (DX12)");
                case "fsr_vk": return L("AMD FidelityFX 运行库（Vulkan）", "AMD FidelityFX runtime (Vulkan)");
                default: return id;
            }
        }

        Window CreateCompatibilityHelpDialog()
        {
            var title = L("关于兼容修复与配套库", "About Compatibility Repair & Companion Libraries");
            var dialog = DialogTheme.Create(_window, title, 620);
            var panel = new StackPanel { Margin = new Thickness(22) };

            var statusBox = new Border
            {
                Background = LauncherTheme.Brush("ThemeSubtle"),
                BorderBrush = LauncherTheme.Brush("ThemeBorder"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(14, 10, 14, 10),
                Margin = new Thickness(0, 0, 0, 16)
            };
            var statusStack = new StackPanel();
            statusStack.Children.Add(new TextBlock
            {
                Text = L("当前游戏配套文件状态", "Current Game Companion Library Status"),
                Foreground = LauncherTheme.Brush("ThemeSuccess"),
                FontWeight = FontWeights.SemiBold,
                FontSize = 12,
                Margin = new Thickness(0, 0, 0, 4)
            });
            var summaryText = (_compatSummary != null && !string.IsNullOrEmpty(_compatSummary.Text))
                ? _compatSummary.Text
                : L("选择游戏后自动检查配套运行库文件。", "Select a game to check its companion runtime files automatically.");
            statusStack.Children.Add(new TextBlock
            {
                Text = summaryText,
                TextWrapping = TextWrapping.Wrap,
                Foreground = LauncherTheme.Brush("ThemeInk"),
                FontSize = 12
            });
            statusBox.Child = statusStack;
            panel.Children.Add(statusBox);

            var qaText = L(
                "【兼容修复常见问题 Q&A】\n\n" +
                "1. 什么时候需要使用兼容修复？\n" +
                "   • 游戏设置中的 DLSS 帧生成 / XeSS / FSR 选项灰显、无法开启或提示硬件不支持（部分游戏自带了较旧的配套 DLL）。\n" +
                "   • 开启插帧后游戏闪退、报错，或插帧倍率受限。\n" +
                "   • 游戏更新后自带的文件与 OptiScaler 补丁发生版本冲突。\n\n" +
                "2. 兼容修复做了什么？\n" +
                "   • 自动备份：将游戏目录中原有的配套 DLL（libxess.dll、amd_fidelityfx_*.dll 等）完整备份到游戏目录下的 OptiScaler-compat-backup 文件夹中。\n" +
                "   • 配套替换：将本工具箱经过实测验证的配套运行库一键部署到游戏目录，替换厂商未更新的老旧文件，使补丁与游戏运行库完美协同。\n\n" +
                "3. 如何还原或应对游戏更新？\n" +
                "   • 若修复后没有改善或出现异常，随时点击“恢复原文件”即可一键无损还原。\n" +
                "   • 游戏更新后若正常运行，可在还原窗口中选择“保留当前文件并结束修复”，安全清理旧记录。",
                "[Compatibility Repair Q&A]\n\n" +
                "1. When should you use Compatibility Repair?\n" +
                "   • In-game DLSS Frame Generation / XeSS / FSR options are greyed out, cannot be toggled on, or show hardware unsupported errors (some games bundle outdated companion DLLs).\n" +
                "   • Game crashes or throws errors when frame generation is enabled, or frame rate multipliers are limited.\n" +
                "   • Game updates introduced conflicting companion libraries.\n\n" +
                "2. What does Compatibility Repair do?\n" +
                "   • Automatic Backup: Safely backs up original companion DLLs (libxess.dll, amd_fidelityfx_*.dll, etc.) to the OptiScaler-compat-backup folder inside the game directory.\n" +
                "   • Companion Replacement: Deploys validated runtime libraries bundled with this toolbox, replacing stale vendor files so the mod and game work seamlessly together.\n\n" +
                "3. How to restore or handle game updates?\n" +
                "   • If repair does not help, click 'Restore Files' anytime for one-click lossless rollback.\n" +
                "   • After a game update, you can choose 'Keep current files and finish repair' to safely keep the latest files."
            );
            panel.Children.Add(new TextBlock
            {
                Text = qaText,
                TextWrapping = TextWrapping.Wrap,
                Foreground = LauncherTheme.Brush("ThemeInk"),
                Margin = new Thickness(0, 0, 0, 20)
            });

            var row = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right };
            var ok = new Button { Content = L("确定", "OK"), MinWidth = 90, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, ok);
            ok.Click += delegate { dialog.DialogResult = true; };
            row.Children.Add(ok);
            panel.Children.Add(row);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        void ShowCompatibilityHelp()
        {
            CreateCompatibilityHelpDialog().ShowDialog();
        }

        void UpdateCompatibilitySummary()
        {
            var busy = _writeBusy || (_session != null && _session.IsRunning && (_presentationBusy || !_session.CanKill));
            var report = _lastInspectResult == null ? null : _lastInspectResult.Compatibility;
            var available = _selectedGame != null && report != null && !busy;
            if (_compatButton != null) _compatButton.IsEnabled = available;
            // Keep restoration reachable even when inspection reports damaged package inputs.
            if (_restoreCompatButton != null) _restoreCompatButton.IsEnabled = available && (report.Active || !string.IsNullOrEmpty(report.Error));
            if (_compatSummary == null) return;
            _compatSummary.ToolTip = null;
            if (_selectedGame == null || report == null) {
                _compatSummary.Text = L("选择游戏后自动检查配套运行库文件。", "Select a game to check its companion runtime files automatically.");
                return;
            }
            if (!string.IsNullOrEmpty(report.Error)) {
                _compatSummary.Text = L("配套文件检查未完成：", "Companion files inspection incomplete: ") + report.Error;
                return;
            }
            if (report.Active) {
                _compatSummary.Text = L("已有修复备份，可恢复原文件", "Repair backup available; originals can be restored");
                return;
            }
            var names = new List<string>();
            if (report.Groups != null) foreach (var group in report.Groups)
                if (group.Files != null) foreach (var file in group.Files)
                    if (file.State == "different") names.Add(file.Name);
            _compatSummary.Text = L("游戏打不开或插帧异常时再用", "Use only for startup or frame-generation issues");
            _compatSummary.ToolTip = names.Count > 0 ? L("配套文件存在差异：", "Companion files differ: ") + string.Join(", ", names) : null;
        }

        Window CreateCompatibilityDialog(bool restore)
        {
            var report = _lastInspectResult == null ? null : _lastInspectResult.Compatibility;
            var dialog = new Window {
                Owner = _window.IsVisible ? _window : null,
                Title = restore ? L("恢复游戏原文件", "Restore game files") : L("兼容修复与配套库替换", "Compatibility repair & companion replacement"),
                Width = 650, SizeToContent = SizeToContent.Height, MaxHeight = 720,
                WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
                ShowInTaskbar = false
            };
            DialogTheme.Apply(dialog, _window);
            var panel = new StackPanel { Margin = new Thickness(24) };
            Action<string> text = value => panel.Children.Add(new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,12) });
            text(_selectedGame == null ? "" : _selectedGame.Exe);
            text(restore
                ? L("关闭游戏后恢复上次修复前的文件，并移除修复时新增的文件。游戏更新过的文件不会被覆盖。若游戏已更新或希望保留现状，可选“保留当前文件并结束修复”：不恢复任何 DLL，保留全部备份，并允许再次修复。", "Close the game first. Restore files from before the last repair and remove files added by it. Files changed by game updates will be preserved. Alternatively, Keep current files and finish repair changes no DLLs, retains all backups and allows a new repair.")
                : L("关闭游戏后，将自动把游戏目录中的配套运行库（XeSS、AMD FidelityFX 等）安全备份到 OptiScaler-compat-backup，并替换为本工具箱经过验证的最新配套组件。\n解决部分游戏中设置里无法开启 DLSS 帧生成 / FSR / XeSS，或因游戏自带 DLL 老旧冲突导致的异常。\n无改善可随时点击“恢复原文件”一键还原。", "Close the game first. Safely back up game companion runtime libraries (XeSS, AMD FidelityFX, etc.) into OptiScaler-compat-backup, and replace them with tested companion components bundled with this toolbox.\nSolves issues in some games where in-game DLSS-FG / FSR / XeSS cannot be toggled on or conflicts with outdated bundled DLLs.\nIf it does not help, you can restore original files at any time."));
            var selections = new Dictionary<string, CheckBox>();
            if (report != null && !string.IsNullOrEmpty(report.Error)) text(report.Error);
            var canRestore = restore && report != null && report.Active && !string.IsNullOrEmpty(report.Fingerprint);
            var proceed = new Button { Content = restore ? L("恢复原文件", "Restore originals") : L("备份并尝试修复", "Back up and try repair"), IsEnabled = canRestore, MinWidth = 150, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, proceed);
            if (restore && report != null && report.RestoreFiles != null) text(string.Join("\n", report.RestoreFiles));
            if (!restore && report != null && report.Groups != null) {
                if (report.Groups.Length > 1) {
                    var selectAllBtn = new Button {
                        Content = L("全选所有就绪组件", "Select all ready components"),
                        HorizontalAlignment = HorizontalAlignment.Left,
                        Margin = new Thickness(0, 0, 0, 8),
                        Padding = new Thickness(8, 4, 8, 4)
                    };
                    selectAllBtn.Click += delegate {
                        foreach (var item in selections.Values) {
                            if (item.IsEnabled) item.IsChecked = true;
                        }
                    };
                    panel.Children.Add(selectAllBtn);
                }
                foreach (var group in report.Groups) {
                    var check = new CheckBox { Content = XessGroupLabel(group.Id), Foreground = LauncherTheme.Brush("ThemeInk"),
                        IsEnabled = group.Ready && !report.Active, Margin = new Thickness(0,6,0,6) };
                    selections[group.Id] = check;
                    RoutedEventHandler changed = (s,e) => {
                        var any = false;
                        foreach (var item in selections.Values) if (item.IsChecked == true) any = true;
                        proceed.IsEnabled = any;
                    };
                    check.Checked += changed; check.Unchecked += changed;
                    panel.Children.Add(check);
                    if (!group.Ready) text(L("安装包缺少有效配套文件，请重新解压完整包。", "Valid companion files are missing from this package. Extract a complete package again."));
                    if (group.Files != null) foreach (var file in group.Files) {
                        var gameVersion = file.State == "missing" ? L("无", "absent") : string.IsNullOrEmpty(file.GameVersion) ? L("版本未知", "unknown version") : file.GameVersion;
                        var packageVersion = string.IsNullOrEmpty(file.PackageVersion) ? L("版本未知", "unknown version") : file.PackageVersion;
                        text(file.Name + "\n" + L("游戏目录：", "Game folder: ") + gameVersion +
                            L("    包内：", "    Package: ") + packageVersion +
                            (file.State == "same" ? L("（文件相同）", " (identical)") : L("（文件不同或缺失）", " (different or absent)")));
                    }
                }
            }
            if (!restore && report != null && report.Active) text(L("请先打开“恢复游戏原文件”，恢复上次修复，或保留当前文件并结束旧修复，再尝试其他组件。", "Open Restore game files first: restore the previous repair, or keep current files and finish it before trying other components."));
            if (report != null && report.Evidence != null && report.Evidence.Length > 0) {
                text(L("最近日志中的配套记录（可能来自之前的运行，不能单独证明本次故障）：", "Companion entries in the latest log (may be from a previous run; not proof of the current fault):"));
                text(string.Join("\n", report.Evidence));
            }
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            buttons.Children.Add(new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 90, Margin = new Thickness(4) });
            if (canRestore) {
                var keep = new Button { Content = L("保留当前文件并结束修复", "Keep current files and finish repair"), Margin = new Thickness(4) };
                keep.Click += delegate { dialog.Uid = "keepXess"; dialog.DialogResult = true; };
                buttons.Children.Add(keep);
            }
            proceed.Click += delegate {
                var chosen = new List<string>();
                foreach (var item in selections) if (item.Value.IsChecked == true) chosen.Add(item.Key);
                dialog.Tag = chosen.ToArray(); dialog.DialogResult = true;
            };
            buttons.Children.Add(proceed); panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        void OnCompatibility(bool restore)
        {
            if (_selectedGame == null || _lastInspectResult == null || _writeBusy || (_session != null && _session.IsRunning)) return;
            var fingerprint = _lastInspectResult.Compatibility == null ? null : _lastInspectResult.Compatibility.Fingerprint;
            var dialog = CreateCompatibilityDialog(restore);
            if (dialog.ShowDialog() != true) return;
            _decisions.XessGroups = dialog.Tag as string[];
            ExecuteAction(dialog.Uid == "keepXess" ? "keepXess" : restore ? "restoreXess" : "repairXess", false, fingerprint);
        }

        string SelectMochizukiModels(string action)
        {
            return SelectModelDialog(CreateMochizukiModelDialog(), action);
        }

        string SelectModelDialog(Window dialog, string action)
        {
            var accepted = dialog.ShowDialog() == true;
            if (dialog.Uid == "modelSource") ChooseModelSource(action);
            return accepted ? dialog.Tag as string : null;
        }

        void AddModelSourceChoice(StackPanel panel, Window dialog, bool originalAvailable, bool smallAvailable)
        {
            if (originalAvailable && smallAvailable) return;
            var source = new Button {
                Name = "ChooseModelSource", Content = L("选择模型来源 DLL…", "Choose model source DLL…"),
                HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 12, 0, 0),
                IsEnabled = _selectedGame != null && _selectedGame.HasExecutable
            };
            source.Click += delegate { dialog.Uid = "modelSource"; dialog.DialogResult = false; };
            panel.Children.Add(source);
        }

        Window CreateMochizukiModelDialog()
        {
            BackendInfo capability = null;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends)
                    if (backend != null && backend.Id == "mochizuki") { capability = backend; break; }
            Func<string, bool> available = state =>
                string.Equals(state, "ready", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(state, "preparable", StringComparison.OrdinalIgnoreCase);
            var states = new[] {
                new { Value = "small", Label = L("Small — CC_SMALL_FP8（通用）", "Small — CC_SMALL_FP8 (General)"),
                    State = capability == null ? null : capability.SmallState },
                new { Value = "experimental", Label = L("Experimental — CC_SMALL_FP8_EXPERIMENTAL（ACO 模式 · RX 7000 / 9000）", "Experimental — CC_SMALL_FP8_EXPERIMENTAL (ACO Mode · RX 7000 / 9000)"),
                    State = capability == null ? null : capability.ExperimentalState },
                new { Value = "original", Label = L("Original — CC_SILVER_AARDWOLD（通用）", "Original — CC_SILVER_AARDWOLD (General)"),
                    State = capability == null ? null : capability.OriginalState },
                new { Value = "int4", Label = L("Optimized — CC_SILVER_AARDWOLD_OPTIMIZED（ACO 模式 · RX 7000 / 9000）", "Optimized — CC_SILVER_AARDWOLD_OPTIMIZED (ACO Mode · RX 7000 / 9000)"),
                    State = capability == null ? null : capability.Int4State }
            };
            var ready = new bool[4];
            var canInstall = new bool[4];
            for (var i = 0; i < states.Length; i++)
            {
                ready[i] = string.Equals(states[i].State, "ready", StringComparison.OrdinalIgnoreCase);
                canInstall[i] = available(states[i].State);
            }
            var originalAvailable = canInstall[2];
            var smallAvailable = canInstall[0];
            var dialog = new Window
            {
                Owner = _window.IsVisible ? _window : null, Title = L("选择 Mochizuki 网络", "Select Mochizuki networks"), Width = 560,
                SizeToContent = SizeToContent.Height, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false,
                Background = LauncherTheme.Brush("ThemeSurface"), Foreground = LauncherTheme.Brush("ThemeInk")
            };
            DialogTheme.Apply(dialog, _window);
            var panel = new StackPanel { Margin = new Thickness(24) };
            panel.Children.Add(new TextBlock
            {
                Text = L("选择要安装的网络，再选择游戏启动时使用的网络。Experimental 依赖 Small，Optimized 依赖 Original；父模型会自动安装或提取。",
                    "Choose the networks to install, then the one the game starts with. Experimental needs Small and Optimized needs Original; parent models are installed or extracted automatically."),
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 12)
            });
            var checks = new CheckBox[4];
            var startCombo = new ComboBox { MinWidth = 300, Margin = new Thickness(0, 12, 0, 0) };
            var proceed = new Button { Content = L("继续", "Continue"), IsEnabled = false, MinWidth = 100, Margin = new Thickness(4) };
            var syncing = false;
            Action updateProceed = null;
            Action refreshStart = null;
            Action updateLocks = null;

            updateProceed = delegate {
                var any = false;
                for (var i = 0; i < checks.Length; i++) any |= checks[i].IsChecked == true;
                proceed.IsEnabled = any && startCombo.SelectedItem != null;
            };
            refreshStart = delegate {
                var previous = startCombo.SelectedItem as ComboBoxItem;
                var previousTag = previous != null ? previous.Tag as string : null;
                startCombo.Items.Clear();
                for (var i = 0; i < checks.Length; i++)
                {
                    if (checks[i].IsChecked != true) continue;
                    startCombo.Items.Add(new ComboBoxItem { Tag = states[i].Value, Content = states[i].Label });
                }
                ComboBoxItem restore = null;
                foreach (var entry in startCombo.Items)
                {
                    var item = entry as ComboBoxItem;
                    if (item != null && previousTag != null && (item.Tag as string) == previousTag) restore = item;
                }
                if (restore != null) startCombo.SelectedItem = restore;
                else if (startCombo.Items.Count > 0) startCombo.SelectedIndex = 0;
                else startCombo.SelectedIndex = -1;
                startCombo.IsEnabled = startCombo.Items.Count > 0;
                updateProceed();
            };
            updateLocks = delegate {
                var experimentalLocksSmall = checks[1] != null && checks[1].IsChecked == true;
                var int4LocksOriginal = checks[3] != null && checks[3].IsChecked == true;
                checks[0].IsEnabled = canInstall[0];
                checks[0].Tag = !canInstall[0] ? "unavailable" : experimentalLocksSmall ? ModelDependencyLock.TagValue : null;
                checks[0].ToolTip = experimentalLocksSmall && canInstall[0]
                    ? L("Experimental 已选择，Small 是必需依赖，不能取消。", "Experimental is selected, so Small is a required dependency and cannot be cleared.")
                    : null;
                checks[2].IsEnabled = canInstall[2];
                checks[2].Tag = !canInstall[2] ? "unavailable" : int4LocksOriginal ? ModelDependencyLock.TagValue : null;
                checks[2].ToolTip = int4LocksOriginal && canInstall[2]
                    ? L("Optimized 已选择，Original 是必需依赖，不能取消。", "Optimized is selected, so Original is a required dependency and cannot be cleared.")
                    : null;
            };
            Action syncParents = delegate {
                if (syncing) return;
                syncing = true;
                try {
                    if (checks[1].IsChecked == true && canInstall[0]) checks[0].IsChecked = true;
                    if (checks[3].IsChecked == true && canInstall[2]) checks[2].IsChecked = true;
                } finally { syncing = false; }
                updateLocks();
                refreshStart();
            };

            for (var i = 0; i < states.Length; i++)
            {
                var check = new CheckBox {
                    Content = new TextBlock { Text = states[i].Label, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center },
                    IsChecked = canInstall[i], IsEnabled = canInstall[i],
                    Style = dialog.FindResource("ModelCard") as Style,
                    Tag = canInstall[i] ? null : "unavailable",
                    Foreground = LauncherTheme.Brush("ThemeInk")
                };
                checks[i] = check;
                panel.Children.Add(check);
            }
            for (var i = 0; i < checks.Length; i++)
            {
                var check = checks[i];
                check.Checked += delegate { if (!syncing) syncParents(); };
                ModelDependencyLock.Attach(check, () => syncing,
                    delegate { updateLocks(); refreshStart(); },
                    value => syncing = value);
            }
            panel.Children.Add(new TextBlock {
                Text = L("启动网络", "Startup network"), Foreground = LauncherTheme.Brush("ThemeMuted"), Margin = new Thickness(0, 8, 0, 0)
            });
            panel.Children.Add(startCombo);
            if (!canInstall[0] && !canInstall[1] && !canInstall[2] && !canInstall[3])
                panel.Children.Add(new TextBlock {
                    Text = L("部分模型暂不可用，请补齐来源或完整安装包后重试。", "Some models are unavailable. Add the source or restore the complete package and retry."),
                    TextWrapping = TextWrapping.Wrap, Foreground = LauncherTheme.Brush("ThemeWarning"), Margin = new Thickness(0, 12, 0, 0)
                });
            AddModelSourceChoice(panel, dialog, originalAvailable, smallAvailable);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var cancel = new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 100, Margin = new Thickness(4) };
            proceed.Click += delegate {
                var networks = new List<string>();
                for (var i = 0; i < checks.Length; i++) if (checks[i].IsChecked == true) networks.Add(states[i].Value);
                if (networks.Count == 0) return;
                var active = startCombo.SelectedItem as ComboBoxItem;
                var activeValue = active == null ? null : active.Tag as string;
                if (string.IsNullOrEmpty(activeValue) || !networks.Contains(activeValue)) activeValue = networks[0];
                _decisions.MochizukiNetworks = networks.ToArray();
                _decisions.MochizukiActiveModel = activeValue;
                dialog.Tag = activeValue;
                dialog.DialogResult = true;
            };
            DialogTheme.Primary(dialog, proceed);
            syncParents();
            updateLocks();
            refreshStart();
            buttons.Children.Add(cancel);
            buttons.Children.Add(proceed);
            panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        bool PrepareMissingModelSource(string action, List<string> selected)
        {
            var needsSource = false;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends) {
                    if (backend == null || !selected.Contains(backend.Id)) continue;
                    if (backend.Id == "mochizuki" && backend.RuntimeState == "ready" && backend.ModelState == "missing") needsSource = true;
                }
            if (needsSource)
            {
                if (!string.IsNullOrEmpty(_decisions.ModelSourceDll))
                    DialogTheme.Show(_window, L("所选 DLL 无法提供需要的模型。请选择受支持的 nvngx_dlssnr.dll；Small 需要 310.9.16。", "The selected DLL cannot supply the required model. Choose a supported nvngx_dlssnr.dll; Small requires 310.9.16."), _window.Title);
                ChooseModelSource(action);
                return false;
            }
            if (!IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true && !HasValidOpenRrDlssd())
            {
                if (!ChooseOpenRrDlssd()) return false;
            }
            return true;
        }

        void ChooseModelSource(string action)
        {
            var directory = Path.GetDirectoryName(_selectedGame.Exe);
            var picker = new OpenFileDialog {
                Title = L("缺少模型来源：选择 nvngx_dlssnr.dll（用于离线提取）", "Model source missing: select nvngx_dlssnr.dll for offline extraction"),
                Filter = "nvngx_dlssnr.dll|nvngx_dlssnr.dll|DLL (*.dll)|*.dll", CheckFileExists = true,
                InitialDirectory = directory, FileName = Path.Combine(directory, "nvngx_dlssnr.dll"), RestoreDirectory = true
            };
            if (picker.ShowDialog(_window) != true) return;
            _decisions.ModelSourceDll = picker.FileName;
            _resumeInstallAction = action;
            ReleaseLock();
            // Inspect the selected source before showing model choices or writing game files.
            InspectGame(_selectedGame);
        }

        bool ChooseOpenRrDlssd()
        {
            var directory = _selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe) ? Path.GetDirectoryName(_selectedGame.Exe) : null;
            var picker = new OpenFileDialog {
                Title = L("缺少 OpenRR 模型源：选择 nvngx_dlssd.dll（需 310.9.1.0）", "OpenRR model source missing: select nvngx_dlssd.dll (310.9.1.0)"),
                Filter = "nvngx_dlssd.dll (310.9.1.0)|nvngx_dlssd.dll|DLL (*.dll)|*.dll", CheckFileExists = true,
                FileName = "nvngx_dlssd.dll", RestoreDirectory = true
            };
            if (!string.IsNullOrEmpty(directory)) picker.InitialDirectory = directory;
            if (picker.ShowDialog(_window) != true) return false;
            try
            {
                var source = picker.FileName;
                ValidateModelSourcePe(source);
                if (!IsDlssd310_9_1(source))
                    throw new IOException(L("所选文件不是 310.9.1.0 版本的 nvngx_dlssd.dll。OpenRR 仅支持 310.9.1.0。", "The selected file is not nvngx_dlssd.dll version 310.9.1.0. OpenRR requires 310.9.1.0."));
                var root = ResolveImportRoot();
                Directory.CreateDirectory(root);
                var destination = Path.Combine(root, "nvngx_dlssd.dll");
                if (!string.Equals(Path.GetFullPath(source), Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase))
                    File.Copy(source, destination, true);
                RefreshModelImportBar();
                return true;
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window, L("导入 nvngx_dlssd.dll 失败：", "Failed to import nvngx_dlssd.dll: ") + ex.Message, _window.Title);
                return false;
            }
        }

        static bool IsExcludedDlssdFolder(string name)
        {
            if (string.IsNullOrEmpty(name)) return true;
            if (name.IndexOf("backup", StringComparison.OrdinalIgnoreCase) >= 0) return true;
            return string.Equals(name, "openrr-amd", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "opennr-rr", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "dlssnr-amd", StringComparison.OrdinalIgnoreCase);
        }

        static bool IsClimbableGameSubdir(string name)
        {
            if (string.IsNullOrEmpty(name)) return false;
            return string.Equals(name, "Win64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "WinGDK", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "Win32", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "Binaries", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "bin", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x64_dx12", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x64_dx11", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x86_64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "retail", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "shipping", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "Release", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "game", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "pc", StringComparison.OrdinalIgnoreCase);
        }

        static bool IsAllowedDlssdSubdir(string name)
        {
            if (IsExcludedDlssdFolder(name)) return false;
            if (string.Equals(name, "runtime", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "marketplace", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "thirdparty", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "vendor", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "binaries", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "bin", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "win64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "wingdk", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x64_dx12", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "x86_64", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "plugins", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "sl", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, ".trex", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "core", StringComparison.OrdinalIgnoreCase))
                return true;
            return name.IndexOf("nvidia", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("streamline", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("dlss", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("ngx", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("rtx", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("remix", StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.IndexOf("interposer", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        static string TryFindDirectUePluginDlssd(string pluginRoot)
        {
            if (string.IsNullOrEmpty(pluginRoot) || !Directory.Exists(pluginRoot)) return null;
            var prefixes = new[]
            {
                @"Runtime\Nvidia\Streamline",
                @"Runtime\Nvidia\StreamlineCore",
                @"Runtime\Nvidia\StreamlineDLSSG",
                @"Runtime\Nvidia\DLSS",
                @"Marketplace\Streamline",
                @"Marketplace\StreamlineCore",
                @"Marketplace\DLSS",
                @"Nvidia\Streamline",
                @"Nvidia\DLSS",
                "Streamline",
                "DLSS"
            };
            var plats = new[] { "Win64", "WinGDK" };
            for (int i = 0; i < prefixes.Length; i++)
            {
                var baseDir = Path.Combine(pluginRoot, prefixes[i]);
                var tpDir = Path.Combine(Path.Combine(baseDir, "Binaries"), "ThirdParty");
                for (int j = 0; j < plats.Length; j++)
                {
                    var cand = Path.Combine(Path.Combine(tpDir, plats[j]), "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                }
            }
            return null;
        }

        internal static string FindGameNativeDlssdPath(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir) || !Directory.Exists(gameDir)) return null;
            try
            {
                // Tier 1: O(1) fixed-location probes (zero directory enumeration).
                var direct = Path.Combine(gameDir, "nvngx_dlssd.dll");
                if (File.Exists(direct)) return direct;

                var gameSubDirs = new[] { ".trex", "Streamline", "dlss", "nvngx", "ThirdParty", "bin", "x64", "Win64" };
                for (int i = 0; i < gameSubDirs.Length; i++)
                {
                    var cand = Path.Combine(Path.Combine(gameDir, gameSubDirs[i]), "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                }

                var climbed = new List<string>();
                var current = gameDir;
                bool sawBinaries = false;
                for (int up = 0; up < 4 && !string.IsNullOrEmpty(current); up++)
                {
                    var leaf = Path.GetFileName(current.TrimEnd('\\', '/'));
                    if (string.Equals(leaf, "Binaries", StringComparison.OrdinalIgnoreCase)) sawBinaries = true;
                    if (!IsClimbableGameSubdir(leaf)) break;
                    var parent = Path.GetDirectoryName(current.TrimEnd('\\', '/'));
                    if (string.IsNullOrEmpty(parent) || string.Equals(parent, current, StringComparison.OrdinalIgnoreCase)) break;
                    climbed.Add(parent);
                    current = parent;
                }

                var gameRoot = current;
                string ueRoot = null;
                if (sawBinaries)
                {
                    var p = Path.GetDirectoryName(gameRoot.TrimEnd('\\', '/'));
                    if (!string.IsNullOrEmpty(p) && !string.Equals(p, gameRoot, StringComparison.OrdinalIgnoreCase) && Directory.Exists(p))
                        ueRoot = p;
                }

                for (int i = 0; i < climbed.Count; i++)
                {
                    var cand = Path.Combine(climbed[i], "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                }
                if (!string.IsNullOrEmpty(ueRoot))
                {
                    var cand = Path.Combine(ueRoot, "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                }

                var rootRels = new[] { "Streamline", "bin", @"bin\x64", @"bin\.trex", @"Plugins\x86_64" };
                for (int i = 0; i < rootRels.Length; i++)
                {
                    var cand = Path.Combine(Path.Combine(gameRoot, rootRels[i]), "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                }

                var uePluginRoots = new List<string>();
                uePluginRoots.Add(Path.Combine(gameRoot, "Plugins"));
                uePluginRoots.Add(Path.Combine(Path.Combine(gameRoot, "Engine"), "Plugins"));
                if (!string.IsNullOrEmpty(ueRoot))
                    uePluginRoots.Add(Path.Combine(Path.Combine(ueRoot, "Engine"), "Plugins"));
                for (int i = 0; i < uePluginRoots.Count; i++)
                {
                    var hit = TryFindDirectUePluginDlssd(uePluginRoots[i]);
                    if (!string.IsNullOrEmpty(hit)) return hit;
                }

                try
                {
                    if (Directory.Exists(gameRoot))
                    {
                        var dataDirs = Directory.GetDirectories(gameRoot, "*_Data");
                        for (int i = 0; i < dataDirs.Length; i++)
                        {
                            var pluginsDir = Path.Combine(dataDirs[i], "Plugins");
                            var x64Cand = Path.Combine(Path.Combine(pluginsDir, "x86_64"), "nvngx_dlssd.dll");
                            if (File.Exists(x64Cand)) return x64Cand;
                            var flatCand = Path.Combine(pluginsDir, "nvngx_dlssd.dll");
                            if (File.Exists(flatCand)) return flatCand;
                        }
                    }
                }
                catch { }

                // Tier 2: shallow depth-1 check under gameDir + whitelist-pruned BFS (visited dedup, max 64 dirs).
                try
                {
                    var depth1 = Directory.GetDirectories(gameDir);
                    for (int i = 0; i < depth1.Length; i++)
                    {
                        var sub = depth1[i];
                        var leaf = Path.GetFileName(sub.TrimEnd('\\', '/'));
                        if (IsExcludedDlssdFolder(leaf)) continue;
                        var cand = Path.Combine(sub, "nvngx_dlssd.dll");
                        if (File.Exists(cand)) return cand;
                    }
                }
                catch { }

                var seeds = new List<string>();
                var seedBases = new[] { gameDir, gameRoot };
                var seedRels = new[] { "Plugins", "Streamline", "ThirdParty", ".trex", "bin" };
                for (int b = 0; b < seedBases.Length; b++)
                {
                    for (int r = 0; r < seedRels.Length; r++)
                        seeds.Add(Path.Combine(seedBases[b], seedRels[r]));
                }
                seeds.Add(Path.Combine(Path.Combine(gameRoot, "Engine"), "Plugins"));
                if (!string.IsNullOrEmpty(ueRoot))
                    seeds.Add(Path.Combine(Path.Combine(ueRoot, "Engine"), "Plugins"));

                var queue = new Queue<Tuple<string, int>>();
                var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < seeds.Count; i++)
                {
                    if (Directory.Exists(seeds[i]))
                        queue.Enqueue(Tuple.Create(seeds[i], 0));
                }

                const int maxDirs = 64;
                const int maxDepth = 7;
                int visitedCount = 0;
                while (queue.Count > 0 && visitedCount < maxDirs)
                {
                    var item = queue.Dequeue();
                    var dir = item.Item1;
                    var depth = item.Item2;
                    string norm;
                    try { norm = Path.GetFullPath(dir).TrimEnd('\\', '/'); }
                    catch { continue; }
                    if (!visited.Add(norm)) continue;
                    visitedCount++;

                    var cand = Path.Combine(norm, "nvngx_dlssd.dll");
                    if (File.Exists(cand)) return cand;
                    if (depth >= maxDepth) continue;

                    string[] children;
                    try { children = Directory.GetDirectories(norm); }
                    catch { continue; }
                    for (int i = 0; i < children.Length; i++)
                    {
                        var child = children[i];
                        var cLeaf = Path.GetFileName(child.TrimEnd('\\', '/'));
                        if (IsAllowedDlssdSubdir(cLeaf))
                            queue.Enqueue(Tuple.Create(child, depth + 1));
                    }
                }
            }
            catch { }
            return null;
        }

        static bool GameDirectoryHasNativeDlssdDll(string gameDir)
        {
            return !string.IsNullOrEmpty(FindGameNativeDlssdPath(gameDir));
        }

        void ExecuteAction(string action, bool keepModelChoice = false, string compatibilityFingerprint = null)
        {
            var installing = action == "install" || action == "update";
            if (_selectedGame == null || !_selectedGame.HasExecutable) return;
            if (!_writeBusy && _session != null && _session.IsRunning && _session.CanKill)
            {
                CancelReadSession();
            }
            if (_writeBusy || _folderScan != null || (_session != null && _session.IsRunning))
            {
                DialogTheme.Show(_window, _language == "zh" ? "操作正在进行中…" : "An operation is already in progress.", _window.Title);
                return;
            }
            if (installing && !HasLocalPackage()) {
                DialogTheme.Show(_window, L("请从完整解压的安装包启动 OptiScalerNRLauncher.exe。若需使用其他目录，请在设置中确认开启诊断模式并选择完整包。", "Run OptiScalerNRLauncher.exe from a complete extracted package. To use another folder, explicitly enable diagnostic mode in Settings and select a complete package."), _window.Title);
                return;
            }

            // Decisions are valid only for this attempt and its explicit needsInput retries.
            if (!keepModelChoice) { _decisions.ForeignProxy.Clear(); }
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            var installMode = GetSelectedInstallMode();
            var isOptiMode = IsOptiScalerSelectedMode();
            var isRtxMc = IsRtxRemixMinecraftSelected();
            if (installing && EffectiveGameBitness() == 32 && !installMode.StartsWith("reshade", StringComparison.Ordinal))
            {
                DialogTheme.Show(_window, L("32 位游戏仅支持 ReShade 模式。", "32-bit games support ReShade mode only."), _window.Title, MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            if (!TryLockTarget(dir))
            {
                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "目标文件夹已被另一个安装进程锁定。" : "Target folder is locked by another launcher.";
                return;
            }

            var proxy = CurrentSelectedProxy;
            if (installing && isOptiMode && !isRtxMc && string.IsNullOrEmpty(proxy))
            {
                DialogTheme.Show(_window, _language == "zh" ? "请先选择要注入的代理 DLL。" : "Please select a proxy DLL to inject first.", _window.Title);
                ReleaseLock();
                return;
            }

            bool needElevation = (installing || action == "uninstall" || action == "repairXess" || action == "restoreXess") &&
                DlssnrLocalData.IsProtectedDirectory(dir) && !DlssnrLocalData.IsCurrentProcessElevated();
            if (needElevation)
            {
                var uacMsg = L(
                    "目标目录受系统权限保护，需要管理员权限才能写入或卸载：\n\n" + dir + "\n\n是否请求管理员权限（UAC）并继续？",
                    "This folder is protected and requires administrator privileges to install or uninstall:\n\n" + dir + "\n\nRequest administrator privileges (UAC) and continue?");
                if (DialogTheme.Show(_window, uacMsg, _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                {
                    ReleaseLock();
                    return;
                }
            }

            var checkedBackends = new List<string>();
            if (!isOptiMode)
            {
                checkedBackends.Add("mochizuki");
            }
            else if (!IsMcMochiLocked() && _mochiCheck != null && _mochiCheck.IsChecked == true)
            {
                checkedBackends.Add("mochizuki");
            }

            if (installing && isOptiMode)
            {
                bool nrOn = checkedBackends.Contains("mochizuki");
                bool rrOn = !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true;
                if (!nrOn)
                {
                    if (!keepModelChoice)
                    {
                        if (isRtxMc && rrOn)
                        {
                            // MC mode recommends OpenRR and locks NR by default; proceed without extra NR warning.
                        }
                        else
                        {
                            string warnMsg = isRtxMc
                                ? L(
                                    "当前未启用 OpenRR 光线重建与神经渲染（Mochizuki），本次将仅安装基础版 OptiScaler 与 AMD 兼容版 remixbridge.dll。\n\n是否继续？",
                                    "Neither OpenRR (Ray Reconstruction) nor Mochizuki Neural Rendering is enabled. Only base OptiScaler and AMD-patched remixbridge.dll will be installed.\n\nContinue anyway?")
                                : (!rrOn
                                    ? L(
                                        "当前未启用神经渲染（Mochizuki）与 DLSS 光线重建（OpenRR），本次将仅安装官方基础版 OptiScaler（支持超分与帧生成），不安装神经渲染与光线重建插件。\n\n是否继续？",
                                        "Neither Mochizuki Neural Rendering nor OpenRR (DLSS Ray Reconstruction) is enabled. Only official base OptiScaler will be installed.\n\nContinue anyway?")
                                    : L(
                                        "当前未启用神经渲染（Mochizuki），本次将仅安装官方 OptiScaler 与 OpenRR 光线重建插件。\n\n是否继续？",
                                        "Mochizuki Neural Rendering is not enabled; only official OptiScaler and OpenRR will be installed.\n\nContinue anyway?"));
                            if (DialogTheme.Show(_window, warnMsg, _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                            {
                                ReleaseLock();
                                return;
                            }
                        }
                    }
                    checkedBackends.Add("none");
                }
            }
            if (installing && !PrepareMissingModelSource(action, checkedBackends)) { ReleaseLock(); return; }
            if (installing && checkedBackends.Contains("mochizuki") &&
                (!keepModelChoice || string.IsNullOrEmpty(_decisions.MochizukiModels)))
            {
                _decisions.MochizukiModels = SelectMochizukiModels(action);
                if (string.IsNullOrEmpty(_decisions.MochizukiModels)) { ReleaseLock(); return; }
            }
            if (installing && isOptiMode && !keepModelChoice && !IsOpenRrUnsupportedLocked() && !IsOpenRrForceUnlocked() && _openRrToggle != null && _openRrToggle.IsChecked == true)
            {
                if (!GameDirectoryHasNativeDlssdDll(dir))
                {
                    var rrWarn = L(
                        "未在当前游戏目录中检测到原生 nvngx_dlssd.dll，该游戏可能不支持 DLSS 光线重建（Ray Reconstruction）。\n\n是否仍要安装 OpenRR？",
                        "No native nvngx_dlssd.dll was found in the game folder. This game may not support DLSS Ray Reconstruction.\n\nInstall OpenRR anyway?");
                    if (DialogTheme.Show(_window, rrWarn, _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                    {
                        ReleaseLock();
                        return;
                    }
                }
            }

            SetBusy(true, action);
            _pendingLog.Length = 0;
            if (_logView != null) _logView.Clear();
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
            if (_progress != null) _progress.IsIndeterminate = true;
            _pendingAction = action;

            _decisions.ExistingIni = (_replaceIni != null && _replaceIni.IsChecked == true) ? "overwrite" : "keep";
            _decisions.OptiPatcher = (isOptiMode && !isRtxMc && _optiPatcherToggle != null && _optiPatcherToggle.IsEnabled && _optiPatcherToggle.IsChecked == true) ? "install" : "skip";
            _decisions.OpenRr = (isOptiMode && !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true) ? "install" : "skip";
            _decisions.OpenRrSpoof = (isOptiMode && !isRtxMc && _decisions.OptiPatcher == "install" && GetOpenRrSpoof(_selectedGame) == "auto") ? "auto" : "full";
            _decisions.InstallMode = installMode;
            _decisions.RenoDX = GetSelectedRenoDx();
            _decisions.Lumenite = GetSelectedLumenite();
            _decisions.LumeniteZip = GetSelectedLumeniteZip();

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = action,
                Language = _language,
                GameExe = _selectedGame.Exe,
                ProxyChoice = (isOptiMode && !isRtxMc) ? proxy : "dxgi.dll",
                Backends = installing ? checkedBackends.ToArray() : null,
                ActiveBackend = (installing && !checkedBackends.Contains("none")) ? "mochizuki" : null,
                PackageRoot = _packageRoot,
                ExplicitDecisions = _decisions,
                InstallMode = installMode,
                GameBitness = GameBitnessOverride(),
                Fingerprint = (action != "inspect" && _lastInspectResult != null) ? _lastInspectResult.Fingerprint : null,
                CompatibilityFingerprint = compatibilityFingerprint ?? (_lastInspectResult != null && _lastInspectResult.Compatibility != null ? _lastInspectResult.Compatibility.Fingerprint : null)
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = needElevation
                    ? WorkerSession.StartElevated(WorkerScript, req, work, canKill: false)
                    : WorkerSession.Start(WorkerScript, req, work, canKill: false);
                _session.EventReceived += OnEvent;
                _session.Completed += OnCompleted;
            }
            catch (OperationCanceledException)
            {
                SetBusy(false, "");
                ReleaseLock();
                if (_statusLed != null) _statusLed.Fill = MissingBrush;
                if (_progress != null) _progress.IsIndeterminate = false;
                if (_statusText != null) SetStatus("已取消管理员权限授权。", "Administrator elevation was cancelled.");
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                ReleaseLock();
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                if (_statusText != null) _statusText.Text = ex.Message;
                AppendLog(ex.Message);
            }
        }

        void OnEvent(string stage, string message)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (stage != "log") _eventStage = stage;
                if (_statusText != null) _statusText.Text = StageText(_eventStage);
                AppendLog((stage ?? "") + ": " + (message ?? ""));
            }));
        }

        void OnCompleted(WorkerSession source, LauncherResult result, int code)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!ReferenceEquals(_session, source))
                {
                    Detach(source);
                    return;
                }
                if (_progress != null) _progress.IsIndeterminate = false;
                SetBusy(false, "");

                var action = (source != null && !string.IsNullOrEmpty(source.Action)) ? source.Action : _pendingAction;
                _pendingAction = "";
                Detach(source);
                _session = null;
                ReleaseLock();

                if (action == "inspect" && (_selectedGame == null || !_selectedGame.HasExecutable ||
                    !SamePath(source.GameExe, _selectedGame.Exe))) {
                    _resumeInstallAction = "";
                    UpdatePrimaryButtonState();
                    return;
                }
                if (action == "inspect" && !string.Equals(ComparisonPath(source.PackageRoot), ComparisonPath(_packageRoot), StringComparison.OrdinalIgnoreCase)) {
                    _resumeInstallAction = "";
                    if (_selectedGame != null) InspectGame(_selectedGame);
                    return;
                }

                if (result != null && !string.IsNullOrEmpty(result.LogPath))
                    _currentLogPath = result.LogPath;

                if (result == null)
                {
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    if (_statusText != null) SetStatus("执行失败（退出代码 " + code + "）。", "Worker failed (exit " + code + ").");
                }
                else if (string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase) && code != 0)
                {
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    if (_statusText != null) SetStatus("执行退出代码 " + code + " 与状态 " + result.Status + " 不一致。", "Worker exit " + code + " does not match status " + result.Status);
                }
                else
                {
                    if (_statusLed != null) _statusLed.Fill = ReadyBrush;
                    if (_statusText != null) {
                        if (result.Status == "success") {
                            if (action == "install") SetStatus("安装完成", "Installation complete");
                            else if (action == "uninstall") SetStatus("卸载完成", "Uninstall complete");
                            else SetStatus("", "");
                        } else _statusText.Text = result.Status + (string.IsNullOrEmpty(result.Error) ? "" : (": " + result.Error));
                    }
                    RefreshStatusLanguage();
                }

                if (result != null && result.Compatibility != null && _lastInspectResult != null &&
                    (action == "repairXess" || action == "restoreXess" || action == "keepXess"))
                    _lastInspectResult.Compatibility = result.Compatibility;

                if (string.Equals(action, "inspect", StringComparison.OrdinalIgnoreCase))
                {
                    ApplyInspect(result, source.PackageRoot);
                    var resume = _resumeInstallAction;
                    _resumeInstallAction = "";
                    if (!string.IsNullOrEmpty(resume) && code == 0 && result != null && result.Status == "success") {
                        ExecuteAction(resume);
                        return;
                    }
                }
                else if (result != null && string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase))
                {
                    if (action == "repairXess" || action == "restoreXess" || action == "keepXess") {
                        if (_lastInspectResult != null) _lastInspectResult.Compatibility = result.Compatibility;
                        DialogTheme.Show(_window, action == "repairXess"
                            ? L("已备份并替换所选配套运行库文件（XeSS / AMD FidelityFX 等）。请启动游戏测试；无改善可回到这里点“恢复游戏原文件”。", "Selected companion files (XeSS / AMD FidelityFX, etc.) were backed up and replaced. Start the game to test. If it did not help, return here and choose Restore game files.")
                            : action == "keepXess"
                            ? L("已保留当前游戏文件并结束旧修复，全部备份仍保留。现在可以重新进行兼容修复。", "Current game files and all backups were kept. The old repair is finished; you can now perform compatibility repair again.")
                            : L("游戏原文件已恢复。请重新启动游戏。", "Original game files restored. Restart the game."), _window.Title);
                        if (_selectedGame != null) InspectGame(_selectedGame);
                    }
                    if (action == "install" || action == "update" || action == "uninstall")
                    {
                        if (action == "uninstall")
                        {
                            if (_selectedGame != null) _selectedGame.Installed = false;
                        }
                        else
                        {
                            if (_selectedGame != null) _selectedGame.Installed = true;
                        }
                        RecheckPackageComparison(_selectedGame);
                        SaveLibraryCache();
                        SelectGameProfiles();
                        _modeAppliedForKey = "";
                        RefreshGameListView();
                        if (_selectedGame != null) InspectGame(_selectedGame);
                    }

                }

                AppendLog(_statusText != null ? _statusText.Text : "");

                if (result != null && string.Equals(result.Status, "needsInput", StringComparison.OrdinalIgnoreCase))
                {
                    AskDecisions(result, action);
                }

                UpdatePrimaryButtonState();
                if (action != "inspect" && result != null && result.Status == "failed") {
                    var detail = result.Error ?? "";
                    if (detail.Length > 1200) detail = detail.Substring(0, 1200) + "…";
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    DialogTheme.Show(_window, FormatWorkerFailureMessage(detail) +
                        "\n\n" + L("完整日志：", "Full log: ") + _currentLogPath, _window.Title, MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }));
        }

        string FormatWorkerFailureMessage(string detail)
        {
            if (!string.IsNullOrEmpty(detail))
            {
                if (detail.IndexOf("ELEVATED_WRITE_DENIED:", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return L(
                        "操作失败：启动器已以管理员权限运行，但系统仍拒绝写入目标目录。\n\n" +
                        "常见原因与排查建议：\n" +
                        "1. 安全软件拦截：检查 Windows 安全中心「勒索软件防护 / 受控文件夹访问」或火绒、360、电脑管家是否拦截了 powershell.exe；\n" +
                        "2. 游戏或反作弊驻留：彻底关闭游戏与官方启动器（含托盘与反作弊进程）后重试；\n" +
                        "3. 目录权限异常：右键目标文件夹 → 属性 → 安全 → 高级，检查是否存在「拒绝」规则或缺少 Administrators 写入权限（也可将游戏移出 C:\\Program Files）。\n\n" +
                        "详细信息：\n",
                        "Operation failed: The launcher already ran with Administrator privileges, but Windows still denied writing to the target folder.\n\n" +
                        "Common causes:\n" +
                        "1. Antivirus or Windows Defender Controlled Folder Access blocking powershell.exe;\n" +
                        "2. The game, its official launcher, or an anti-cheat driver still protecting the folder;\n" +
                        "3. Restrictive NTFS permissions (explicit Deny rule or missing Administrators write access — check Properties -> Security -> Advanced, or move the game out of C:\\Program Files).\n\n" +
                        "Details:\n") + detail;
                }
                if (detail.IndexOf("ADMIN_REQUIRED:", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return L(
                        "操作失败：目标目录受系统保护，需要管理员权限才能写入。请在 UAC 提示中允许授权，或右键以管理员身份运行启动器后再试。\n\n详细信息：\n",
                        "Operation failed: The target folder requires Administrator privileges to write. Approve the UAC prompt or run the launcher as Administrator and try again.\n\nDetails:\n") + detail;
                }
            }
            return L("操作失败：", "Operation failed: ") + detail;
        }

        void ApplyInspect(LauncherResult result, string requestedPackageRoot = null)
        {
            if (result == null) return;
            if (!string.Equals(ComparisonPath(requestedPackageRoot ?? NormalPackageRoot), ComparisonPath(_packageRoot), StringComparison.OrdinalIgnoreCase)) return;
            _lastInspectResult = result;
            if (!string.IsNullOrEmpty(result.Status) && result.Status != "success") {
                RefreshInspectionText();
                UpdatePrimaryButtonState();
                RefreshModelImportBar();
                return;
            }

            // Checkbox events can temporarily fall back while capabilities are applied
            // in worker order. Resolve the preferred backend after the complete batch.
            var preferredActiveBackend = IsMcMochiLocked() ? "" : _activeBackend;
            var hasUserChoice = CurrentBackendChoice() != null;
            _updatingBackends = true;
            try
            {
                if (result.Backends != null)
                {
                    if (IsMcMochiLocked())
                    {
                        if (_mochiCheck != null)
                        {
                            _mochiCheck.IsChecked = false;
                            _mochiCheck.IsEnabled = false;
                        }
                    }
                    else
                    {
                        var savedChoice = CurrentBackendChoice();
                        bool onlyRetiredSaved = savedChoice != null && savedChoice.Backends != null &&
                            savedChoice.Backends.Length > 0 && Array.IndexOf(savedChoice.Backends, "mochizuki") < 0;
                        if ((!hasUserChoice || onlyRetiredSaved) && _mochiCheck != null && _mochiCheck.IsChecked != true)
                            _mochiCheck.IsChecked = true;
                        foreach (var b in result.Backends)
                        {
                            if (b != null && b.Id == "mochizuki" && _mochiCheck != null)
                            {
                                _mochiCheck.IsEnabled = true;
                                if (!hasUserChoice) _mochiCheck.IsChecked = b.Enabled || b.RuntimeState == "ready";
                            }
                        }
                    }
                }

                _activeBackend = preferredActiveBackend;
                UpdateActiveCombo();
            }
            finally { _updatingBackends = false; }
            RefreshInspectionText();
            if (_selectedGame != null && _selectedGame.HasExecutable)
            {
                bool detectedInstalled = !string.IsNullOrEmpty(result.InstalledMode) ||
                    IsDirectoryInstalled(Path.GetDirectoryName(_selectedGame.Exe));
                if (_selectedGame.Installed != detectedInstalled)
                {
                    _selectedGame.Installed = detectedInstalled;
                    SaveLibraryCache();
                    RefreshGameListView();
                    RecheckPackageComparison(_selectedGame);
                }
            }
            SelectModeForGame(result, null);
            UpdatePrimaryButtonState();
            DetectPackageUpdate(result, requestedPackageRoot);

            if (result.Notes != null)
            {
                foreach (var n in result.Notes) AppendLog(n);
            }
        }


        bool AskDecisions(LauncherResult result, string action)
        {
            if (result.RequiredDecisions == null) return false;
            foreach (var d in result.RequiredDecisions)
            {
                if (d.Id == "foreignProxy" && d.Options != null && d.Options.Length > 0)
                {
                    var prompt = L(d.File + " 已存在，且不属于本项目。\n\n是 = 备份后替换，否 = 保留，取消 = 停止操作。",
                        d.File + " exists and is not this project.\n\nYes = backup, No = keep, Cancel = abort.");
                    var pick = DialogTheme.Show(_window, prompt, _window.Title, MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
                    if (_decisions.ForeignProxy == null) _decisions.ForeignProxy = new Dictionary<string, string>();
                    if (pick == MessageBoxResult.Yes) _decisions.ForeignProxy[d.File] = "backup";
                    else if (pick == MessageBoxResult.No) _decisions.ForeignProxy[d.File] = "keep";
                    else
                    {
                        if (_statusText != null) SetStatus("已取消。", "Cancelled.");
                        return false;
                    }
                }

                else if (d.Id == "mochizukiModels" || d.Id == "mochizukiNetworks")
                {
                    _decisions.MochizukiModels = SelectMochizukiModels(action);
                    if (string.IsNullOrEmpty(_decisions.MochizukiModels)) return false;
                }
                else if (d.Id == "openRrDlssd")
                {
                    if (!ChooseOpenRrDlssd()) return false;
                }
                else if (d.Id == "reinspect")
                {
                    _decisions.ForeignProxy.Clear();
                    if (_selectedGame != null) InspectGame(_selectedGame);
                    return true;
                }
            }
            ExecuteAction(string.IsNullOrEmpty(action) ? "install" : action, true);
            return true;
        }

        void OpenLog()
        {
            try
            {
                if (!string.IsNullOrEmpty(_currentLogPath) && File.Exists(_currentLogPath))
                {
                    try
                    {
                        var psi = new ProcessStartInfo
                        {
                            FileName = _currentLogPath,
                            UseShellExecute = true
                        };
                        Process.Start(psi);
                        return;
                    }
                    catch { }

                    try
                    {
                        var winDir = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
                        var classicNotepad = Path.Combine(winDir, "notepad.exe");
                        if (File.Exists(classicNotepad))
                        {
                            Process.Start(classicNotepad, "\"" + _currentLogPath + "\"");
                            return;
                        }
                    }
                    catch { }

                    Process.Start("explorer.exe", "/select,\"" + _currentLogPath + "\"");
                }
                else
                {
                    DialogTheme.Show(_window, _language == "zh" ? "当前尚无可用日志。" : "No log file available yet.", _window.Title);
                }
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window, ex.Message, _window.Title);
            }
        }

        bool TryLockTarget(string dir)
        {
            ReleaseLock();
            var hash = Convert.ToBase64String(SHA256.Create()
                .ComputeHash(Encoding.UTF8.GetBytes((dir ?? "").ToLowerInvariant())))
                .Replace('+', '-').Replace('/', '_').TrimEnd('=');
            var name = @"Global\OptScalerNR.Launcher." + hash;
            bool created;
            _targetMutex = new Mutex(true, name, out created);
            if (created) return true;
            _targetMutex.Dispose();
            _targetMutex = null;
            return false;
        }

        void ReleaseLock()
        {
            if (_targetMutex != null) {
                try { _targetMutex.ReleaseMutex(); } catch { }
                _targetMutex.Dispose();
                _targetMutex = null;
            }
            RefreshDiagnosticModeUI();
        }

        void SetBusy(bool busy, string action)
        {
            _writeBusy = busy && action != "inspect" && action != "scan";
            _presentationBusy = busy;
            _presentationAction = action;
            if (_gameList != null) _gameList.IsEnabled = !_writeBusy;
            RefreshProfileActions();
            RefreshDiagnosticModeUI();
            if (busy) _eventStage = action;
            if (_addButton != null) _addButton.IsEnabled = !busy;
            if (_removeGameMenuItem != null) _removeGameMenuItem.IsEnabled = !busy && _removeGameMenuItem.Tag is GameInfo;
            if (_launchGameButton != null) _launchGameButton.IsEnabled = !busy && _selectedGame != null && _selectedGame.HasExecutable;
            if (_openFolderButton != null) _openFolderButton.IsEnabled = !busy;
            if (_refreshButton != null) _refreshButton.IsEnabled = !busy;
            if (_primaryButton != null) _primaryButton.IsEnabled = !busy;
            if (_uninstallButton != null) _uninstallButton.IsEnabled = !busy && (_selectedGame != null && _selectedGame.Installed);
            if (_proxyCombo != null) _proxyCombo.IsEnabled = !busy;
            if (_proxyPills != null)
            {
                foreach (var p in _proxyPills.Values) p.IsEnabled = !busy;
            }
            RefreshLibraryActions();
            RefreshHeroState();
        }

        void Detach(WorkerSession session)
        {
            if (session == null) return;
            session.EventReceived -= OnEvent;
            session.Completed -= OnCompleted;
            session.Completed -= OnScanCompleted;
            session.Dispose();
        }

        readonly StringBuilder _pendingLog = new StringBuilder();
        bool _logFlushScheduled;

        void AppendLog(string line)
        {
            if (_logView == null || string.IsNullOrEmpty(line)) return;
            _pendingLog.Append(line).Append(Environment.NewLine);
            if (_logFlushScheduled) return;
            _logFlushScheduled = true;
            _window.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, new Action(() =>
            {
                _logFlushScheduled = false;
                if (_logView == null || _pendingLog.Length == 0) return;
                var batch = _pendingLog.ToString();
                _pendingLog.Length = 0;
                _logView.AppendText(batch);
                if (_logView.IsVisible) _logView.ScrollToEnd();
            }));
        }

        void OnClosing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            // Keep the target mutex until the write completion callback has run,
            // even if the process has exited but its result is still queued.
            if (_writeBusy) {
                e.Cancel = true;
                if (_statusText != null) SetStatus("安装或卸载正在进行，请等待完成后关闭。", "Installation or removal is in progress. Wait for completion before closing.");
                return;
            }
            if (_session != null && _session.IsRunning)
            {
                if (_session.CanKill)
                {
                    _session.CancelInspect();
                    Detach(_session);
                    _session = null;
                }
                else
                {
                    e.Cancel = true;
                }
            }
            if (!e.Cancel) {
                if (_folderScan != null) _folderScan.Cancel();
                if (_recommendations != null) _recommendations.Dispose();
                ReleaseLock();
            }
        }
    }
}
