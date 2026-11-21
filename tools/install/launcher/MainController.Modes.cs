using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        readonly Dictionary<string, string> _gameModes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _gameReshadeApis = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _gameRenoDx = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _gameLumenite = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _gameLumeniteZips = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, bool> _mcMochiUnlockedByGame = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, bool> _openRrForceUnlockedByGame = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);

        RadioButton _modeOptiScalerRadio, _modeStandaloneRadio, _modeReShadeRadio;
        Border _modeDetectBadge, _modeSwitchNotice, _mochiRequiredBadge, _mochiMcWarningBadge, _openRrNoDlssdWarningBadge;
        TextBlock _modeDetectText, _modeSwitchNoticeText, _mochiMcWarningText, _openRrNoDlssdWarningText;
        FrameworkElement _mochiCard, _mochiCollapsedBar, _openRrCard, _openRrCollapsedBar, _openRrUnsupportedBar, _proxyCard, _standaloneModeCard, _reShadeModeCard, _modeCardsGrid, _rtxRemixMcModeCard;
        FrameworkElement _compatPanel, _optiScalerInstallExtras, _optiScalerIniButtonsLeft, _optiScalerIniProfileGroup;
        Button _switchToOptiScalerBtn, _unlockMochiMcBtn, _relockMochiMcBtn, _unlockOpenRrBtn, _relockOpenRrBtn, _lumeniteZipBrowseBtn, _lumeniteZipClearBtn;
        RadioButton _reshadeApiDxRadio, _reshadeApiDx9Radio, _reshadeApiVulkanRadio;
        RadioButton _reshadeStyleRenoDxRadio, _reshadeStyleWilsjo2Radio;
        CheckBox _lumeniteCheck;
        FrameworkElement _lumeniteZipRow;
        TextBox _lumeniteZipText;

        bool _selectingMode;
        string _modeAppliedForKey = "";
        PeTargetInfo _lastPeInfo;
        string _nativeDlssdCacheDir;
        bool _nativeDlssdCacheVal;

        void InitModes()
        {
            _modeOptiScalerRadio = (RadioButton)_window.FindName("ModeOptiScalerRadio");
            _modeStandaloneRadio = (RadioButton)_window.FindName("ModeStandaloneRadio");
            _modeReShadeRadio = (RadioButton)_window.FindName("ModeReShadeRadio");
            _modeDetectBadge = (Border)_window.FindName("ModeDetectBadge");
            _modeDetectText = (TextBlock)_window.FindName("ModeDetectText");
            _modeSwitchNotice = (Border)_window.FindName("ModeSwitchNotice");
            _modeSwitchNoticeText = (TextBlock)_window.FindName("ModeSwitchNoticeText");
            _mochiRequiredBadge = (Border)_window.FindName("MochiRequiredBadge");
            _mochiMcWarningBadge = (Border)_window.FindName("MochiMcWarningBadge");
            _mochiMcWarningText = (TextBlock)_window.FindName("MochiMcWarningText");
            _openRrNoDlssdWarningBadge = (Border)_window.FindName("OpenRrNoDlssdWarningBadge");
            _openRrNoDlssdWarningText = (TextBlock)_window.FindName("OpenRrNoDlssdWarningText");

            _mochiCard = (FrameworkElement)_window.FindName("MochiCard");
            _mochiCollapsedBar = (FrameworkElement)_window.FindName("MochiCollapsedBar");
            _unlockMochiMcBtn = (Button)_window.FindName("UnlockMochiMcBtn");
            _relockMochiMcBtn = (Button)_window.FindName("RelockMochiMcBtn");
            _openRrCard = (FrameworkElement)_window.FindName("OpenRrCard");
            _openRrCollapsedBar = (FrameworkElement)_window.FindName("OpenRrCollapsedBar");
            _openRrUnsupportedBar = (FrameworkElement)_window.FindName("OpenRrUnsupportedBar");
            _unlockOpenRrBtn = (Button)_window.FindName("UnlockOpenRrBtn");
            _relockOpenRrBtn = (Button)_window.FindName("RelockOpenRrBtn");
            _switchToOptiScalerBtn = (Button)_window.FindName("SwitchToOptiScalerBtn");
            _proxyCard = (FrameworkElement)_window.FindName("ProxyCard");
            _standaloneModeCard = (FrameworkElement)_window.FindName("StandaloneModeCard");
            _reShadeModeCard = (FrameworkElement)_window.FindName("ReShadeModeCard");
            _modeCardsGrid = (FrameworkElement)_window.FindName("ModeCardsGrid");
            _rtxRemixMcModeCard = (FrameworkElement)_window.FindName("RtxRemixMcModeCard");

            _compatPanel = (FrameworkElement)_window.FindName("CompatPanel");
            _optiScalerInstallExtras = (FrameworkElement)_window.FindName("OptiScalerInstallExtras");
            _optiScalerIniButtonsLeft = (FrameworkElement)_window.FindName("OptiScalerIniButtonsLeft");
            _iniApplyButton = (Button)_window.FindName("IniApplyButton");
            _optiScalerIniProfileGroup = (FrameworkElement)_window.FindName("OptiScalerIniProfileGroup");

            _reshadeApiDxRadio = (RadioButton)_window.FindName("ReshadeApiDxRadio");
            _reshadeApiDx9Radio = (RadioButton)_window.FindName("ReshadeApiDx9Radio");
            _reshadeApiVulkanRadio = (RadioButton)_window.FindName("ReshadeApiVulkanRadio");
            _reshadeStyleRenoDxRadio = (RadioButton)_window.FindName("ReshadeStyleRenoDxRadio");
            _reshadeStyleWilsjo2Radio = (RadioButton)_window.FindName("ReshadeStyleWilsjo2Radio");

            _lumeniteCheck = (CheckBox)_window.FindName("LumeniteCheck");
            _lumeniteZipRow = (FrameworkElement)_window.FindName("LumeniteZipRow");
            _lumeniteZipText = (TextBox)_window.FindName("LumeniteZipText");
            _lumeniteZipBrowseBtn = (Button)_window.FindName("LumeniteZipBrowseBtn");
            _lumeniteZipClearBtn = (Button)_window.FindName("LumeniteZipClearBtn");

            RoutedEventHandler modeChanged = (s, e) =>
            {
                if (_selectingMode || _loadingSettings) return;
                SaveModePreferences();
                ApplyModeVisibility();
            };
            if (_modeOptiScalerRadio != null) _modeOptiScalerRadio.Checked += modeChanged;
            if (_modeStandaloneRadio != null) _modeStandaloneRadio.Checked += modeChanged;
            if (_modeReShadeRadio != null) _modeReShadeRadio.Checked += modeChanged;
            if (_reshadeApiDxRadio != null) _reshadeApiDxRadio.Checked += modeChanged;
            if (_reshadeApiDx9Radio != null) _reshadeApiDx9Radio.Checked += modeChanged;
            if (_reshadeApiVulkanRadio != null) _reshadeApiVulkanRadio.Checked += modeChanged;
            if (_reshadeStyleRenoDxRadio != null) _reshadeStyleRenoDxRadio.Checked += modeChanged;
            if (_reshadeStyleWilsjo2Radio != null) _reshadeStyleWilsjo2Radio.Checked += modeChanged;

            if (_lumeniteCheck != null)
            {
                _lumeniteCheck.Checked += modeChanged;
                _lumeniteCheck.Unchecked += modeChanged;
            }
            if (_switchToOptiScalerBtn != null)
            {
                _switchToOptiScalerBtn.Click += (s, e) =>
                {
                    if (_modeOptiScalerRadio != null) _modeOptiScalerRadio.IsChecked = true;
                };
            }
            if (_unlockMochiMcBtn != null)
            {
                _unlockMochiMcBtn.Click += (s, e) => RequestUnlockMochiForMc();
            }
            if (_relockMochiMcBtn != null)
            {
                _relockMochiMcBtn.Click += (s, e) => LockMochiForMc();
            }
            if (_unlockOpenRrBtn != null)
            {
                _unlockOpenRrBtn.Click += (s, e) => RequestForceUnlockOpenRr();
            }
            if (_relockOpenRrBtn != null)
            {
                _relockOpenRrBtn.Click += (s, e) => LockOpenRrForUnsupportedGame();
            }
            if (_lumeniteZipBrowseBtn != null)
            {
                _lumeniteZipBrowseBtn.Click += (s, e) => BrowseLumeniteZip();
            }
            if (_lumeniteZipClearBtn != null)
            {
                _lumeniteZipClearBtn.Click += (s, e) =>
                {
                    if (_lumeniteZipText != null) _lumeniteZipText.Text = "";
                    SaveModePreferences();
                    RefreshModesLanguage();
                };
            }

            ApplyModeVisibility();
        }

        string ModeKey()
        {
            if (_selectedGame == null) return null;
            return !string.IsNullOrEmpty(_selectedGame.Id) ? _selectedGame.Id : _selectedGame.Exe;
        }

        public bool IsRtxRemixMinecraftSelected()
        {
            return _selectedGame != null && _selectedGame.IsRtxRemixMinecraft;
        }

        public bool IsMcMochiUnlocked()
        {
            var key = ModeKey();
            bool unlocked;
            return !string.IsNullOrEmpty(key) && _mcMochiUnlockedByGame.TryGetValue(key, out unlocked) && unlocked;
        }

        public bool IsMcMochiLocked()
        {
            return IsRtxRemixMinecraftSelected() && !IsMcMochiUnlocked();
        }

        void RequestUnlockMochiForMc()
        {
            if (_selectedGame == null || !IsRtxRemixMinecraftSelected()) return;
            var key = ModeKey();
            if (string.IsNullOrEmpty(key)) return;
            var msg = L(
                "RTX Remix for Minecraft 使用体素光线追踪画面，一般不建议同时启用神经渲染（Mochizuki / DLSSNR）：\n\n" +
                "• 可能导致方块像素纹理观感失真或画面闪烁\n" +
                "• 额外占用显存与 GPU 开销\n\n" +
                "日常游玩建议仅使用 OpenRR 光线重建。确定仍要解锁 Mochizuki（NR）的安装选项吗？",
                "RTX Remix for Minecraft uses voxel path-traced visuals, where Neural Rendering (Mochizuki / DLSSNR) is generally not recommended:\n\n" +
                "• May cause voxel texture distortion or shimmering\n" +
                "• Adds extra VRAM and GPU overhead\n\n" +
                "For normal play, OpenRR alone is recommended. Unlock Mochizuki (NR) installation anyway?");
            if (DialogTheme.Show(_window, msg, _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return;
            _mcMochiUnlockedByGame[key] = true;
            _backendChoices[key] = new BackendChoice { Backends = new[] { "mochizuki" }, ActiveBackend = "mochizuki" };
            SaveSettings();
            ApplyModeVisibility();
            RefreshInspectionText();
        }

        void LockMochiForMc()
        {
            if (_selectedGame == null || !IsRtxRemixMinecraftSelected()) return;
            var key = ModeKey();
            if (string.IsNullOrEmpty(key)) return;
            _mcMochiUnlockedByGame[key] = false;
            _backendChoices[key] = new BackendChoice { Backends = new string[0], ActiveBackend = "" };
            SaveSettings();
            ApplyModeVisibility();
            RefreshInspectionText();
        }

        void InvalidateNativeDlssdCache()
        {
            _nativeDlssdCacheDir = null;
        }

        public bool SelectedGameHasNativeDlssd()
        {
            if (_selectedGame == null || !_selectedGame.HasExecutable || string.IsNullOrEmpty(_selectedGame.Exe))
                return true;
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            if (string.IsNullOrEmpty(dir)) return false;
            if (!string.Equals(_nativeDlssdCacheDir, dir, StringComparison.OrdinalIgnoreCase))
            {
                _nativeDlssdCacheVal = GameDirectoryHasNativeDlssdDll(dir);
                _nativeDlssdCacheDir = dir;
            }
            return _nativeDlssdCacheVal;
        }

        public bool IsOpenRrForceUnlocked()
        {
            var key = ModeKey();
            bool unlocked;
            return !string.IsNullOrEmpty(key) && _openRrForceUnlockedByGame.TryGetValue(key, out unlocked) && unlocked;
        }

        public bool IsOpenRrUnsupportedLocked()
        {
            return IsOptiScalerSelectedMode() &&
                   _selectedGame != null &&
                   _selectedGame.HasExecutable &&
                   !SelectedGameHasNativeDlssd() &&
                   !IsOpenRrForceUnlocked();
        }

        public bool IsOpenRrUnsupportedForceUnlocked()
        {
            return IsOptiScalerSelectedMode() &&
                   _selectedGame != null &&
                   _selectedGame.HasExecutable &&
                   !SelectedGameHasNativeDlssd() &&
                   IsOpenRrForceUnlocked();
        }

        void RequestForceUnlockOpenRr()
        {
            if (_selectedGame == null || !_selectedGame.HasExecutable || !IsOptiScalerSelectedMode()) return;
            var key = ModeKey();
            if (string.IsNullOrEmpty(key)) return;
            var msg = L(
                "未在当前游戏目录中检测到原生 nvngx_dlssd.dll，该游戏可能不支持 DLSS 光线重建（Ray Reconstruction）。\n\n" +
                "• 对没有光线重建的游戏强行安装 OpenRR 不会带来任何画质或光追效果，反而可能导致异常或冲突\n" +
                "• 如仅需 DLSS 神经渲染或普通超分，请保持 OpenRR 关闭\n\n" +
                "确定要强制解锁此游戏的 OpenRR 选项吗？（仅建议高级用户）",
                "No native nvngx_dlssd.dll was found in this game's folder. This game may not support NVIDIA DLSS Ray Reconstruction.\n\n" +
                "• Installing OpenRR on a game without Ray Reconstruction has no visual benefit and may cause issues or conflicts\n" +
                "• For standard upscaling or Neural Rendering only, leave OpenRR off\n\n" +
                "Force-unlock OpenRR for this game anyway? (Advanced users only)");
            if (DialogTheme.Show(_window, msg, _window.Title, MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return;
            _openRrForceUnlockedByGame[key] = true;
            if (!string.IsNullOrEmpty(_selectedGame.Id)) _openRrByGame[_selectedGame.Id] = true;
            SaveSettings();
            ApplyModeVisibility();
            RefreshInspectionText();
        }

        void LockOpenRrForUnsupportedGame()
        {
            if (_selectedGame == null) return;
            var key = ModeKey();
            if (string.IsNullOrEmpty(key)) return;
            _openRrForceUnlockedByGame[key] = false;
            if (!string.IsNullOrEmpty(_selectedGame.Id)) _openRrByGame[_selectedGame.Id] = false;
            SaveSettings();
            ApplyModeVisibility();
            RefreshInspectionText();
        }

        public bool IsOptiScalerSelectedMode()
        {
            if (IsRtxRemixMinecraftSelected()) return true;
            return GetSelectedBaseMode() == "optiscaler";
        }

        public string GetSelectedBaseMode()
        {
            if (IsRtxRemixMinecraftSelected()) return "optiscaler";
            if (_modeStandaloneRadio != null && _modeStandaloneRadio.IsChecked == true) return "standalone";
            if (_modeReShadeRadio != null && _modeReShadeRadio.IsChecked == true) return "reshade";
            return "optiscaler";
        }

        public string GetSelectedReshadeApi()
        {
            if (_reshadeApiDx9Radio != null && _reshadeApiDx9Radio.IsChecked == true) return "reshade-feeder-dx9";
            if (_reshadeApiVulkanRadio != null && _reshadeApiVulkanRadio.IsChecked == true) return "reshade-feeder-vulkan";
            return "reshade-feeder";
        }

        public string GetSelectedInstallMode()
        {
            if (IsRtxRemixMinecraftSelected()) return "rtx-remix-mc";
            var baseMode = GetSelectedBaseMode();
            if (baseMode == "reshade") return GetSelectedReshadeApi();
            return baseMode;
        }

        public string GetSelectedRenoDx()
        {
            var baseMode = GetSelectedBaseMode();
            if (baseMode == "reshade" && _reshadeStyleWilsjo2Radio != null && _reshadeStyleWilsjo2Radio.IsChecked == true)
                return "off";
            return "on";
        }

        public string GetSelectedLumenite()
        {
            return GetSelectedBaseMode() == "reshade" && _lumeniteCheck != null && _lumeniteCheck.IsChecked == true
                ? "on"
                : "off";
        }

        public string GetSelectedLumeniteZip()
        {
            if (GetSelectedLumenite() != "on" || _lumeniteZipText == null) return "";
            return (_lumeniteZipText.Text ?? "").Trim();
        }

        public string CurrentInstalledModeForSelectedGame()
        {
            if (_lastInspectResult != null && !string.IsNullOrEmpty(_lastInspectResult.InstalledMode))
                return _lastInspectResult.InstalledMode;
            if (_lastPeInfo != null && !string.IsNullOrEmpty(_lastPeInfo.InstalledMode))
                return _lastPeInfo.InstalledMode;
            if (_selectedGame != null && _selectedGame.HasExecutable)
            {
                try { return PeProbe.DetectInstalledMode(Path.GetDirectoryName(_selectedGame.Exe)); }
                catch { }
            }
            return null;
        }

        void SaveModePreferences()
        {
            var key = ModeKey();
            if (_selectingMode || _loadingSettings || string.IsNullOrEmpty(key)) return;
            _gameModes[key] = GetSelectedBaseMode();
            _gameReshadeApis[key] = GetSelectedReshadeApi();
            _gameRenoDx[key] = GetSelectedRenoDx();
            _gameLumenite[key] = GetSelectedLumenite();
            _gameLumeniteZips[key] = _lumeniteZipText != null ? (_lumeniteZipText.Text ?? "").Trim() : "";
            SaveSettings();
        }

        void BrowseLumeniteZip()
        {
            var dlg = new OpenFileDialog
            {
                Title = L("选择 LumeniteFX 本地 ZIP 压缩包", "Select LumeniteFX ZIP archive"),
                Filter = L("ZIP 压缩包 (*.zip)|*.zip|所有文件 (*.*)|*.*", "ZIP archives (*.zip)|*.zip|All files (*.*)|*.*"),
                CheckFileExists = true
            };
            if (dlg.ShowDialog(_window) == true && !string.IsNullOrEmpty(dlg.FileName))
            {
                if (_lumeniteZipText != null) _lumeniteZipText.Text = dlg.FileName;
                if (_lumeniteCheck != null && _lumeniteCheck.IsChecked != true) _lumeniteCheck.IsChecked = true;
                SaveModePreferences();
                RefreshModesLanguage();
            }
        }

        void ApplyBaseModeToUi(string baseMode, string reshadeApi, string renoDx, string lumenite, string lumeniteZip)
        {
            _selectingMode = true;
            try
            {
                if (baseMode == "standalone" && _modeStandaloneRadio != null) _modeStandaloneRadio.IsChecked = true;
                else if (baseMode == "reshade" && _modeReShadeRadio != null) _modeReShadeRadio.IsChecked = true;
                else if (_modeOptiScalerRadio != null) _modeOptiScalerRadio.IsChecked = true;

                if (reshadeApi == "reshade-feeder-dx9" && _reshadeApiDx9Radio != null) _reshadeApiDx9Radio.IsChecked = true;
                else if (reshadeApi == "reshade-feeder-vulkan" && _reshadeApiVulkanRadio != null) _reshadeApiVulkanRadio.IsChecked = true;
                else if (_reshadeApiDxRadio != null) _reshadeApiDxRadio.IsChecked = true;

                if (renoDx == "off" && _reshadeStyleWilsjo2Radio != null) _reshadeStyleWilsjo2Radio.IsChecked = true;
                else if (_reshadeStyleRenoDxRadio != null) _reshadeStyleRenoDxRadio.IsChecked = true;

                if (_lumeniteCheck != null) _lumeniteCheck.IsChecked = (lumenite == "on");
                if (_lumeniteZipText != null) _lumeniteZipText.Text = lumeniteZip ?? "";
            }
            finally { _selectingMode = false; }
        }

        void SelectModeForGame(LauncherResult inspectResult, PeTargetInfo fastPe)
        {
            if (fastPe != null) _lastPeInfo = fastPe;
            var key = ModeKey();
            if (string.IsNullOrEmpty(key))
            {
                ApplyModeVisibility();
                return;
            }

            if (string.Equals(_modeAppliedForKey, key, StringComparison.OrdinalIgnoreCase) && inspectResult == null)
            {
                ApplyModeVisibility();
                return;
            }

            string installed = inspectResult != null && !string.IsNullOrEmpty(inspectResult.InstalledMode)
                ? inspectResult.InstalledMode
                : (fastPe != null ? fastPe.InstalledMode : null);

            if (!string.Equals(_modeAppliedForKey, key, StringComparison.OrdinalIgnoreCase) || !string.IsNullOrEmpty(installed))
            {
                string savedBase;
                _gameModes.TryGetValue(key, out savedBase);

                string baseMode = "optiscaler";
                if (!string.IsNullOrEmpty(installed))
                {
                    baseMode = installed.StartsWith("reshade", StringComparison.OrdinalIgnoreCase)
                        ? "reshade"
                        : (installed == "standalone" ? "standalone" : "optiscaler");
                }
                else if (!string.IsNullOrEmpty(savedBase))
                {
                    baseMode = savedBase;
                }
                else
                {
                    var recMode = inspectResult != null && !string.IsNullOrEmpty(inspectResult.RecommendedMode)
                        ? inspectResult.RecommendedMode
                        : (fastPe != null ? fastPe.RecommendedMode : null);
                    if (recMode == "standalone" || recMode == "reshade") baseMode = recMode;
                }

                string reshadeApi;
                if (!string.IsNullOrEmpty(installed) && installed.StartsWith("reshade", StringComparison.OrdinalIgnoreCase))
                    reshadeApi = installed;
                else if (!_gameReshadeApis.TryGetValue(key, out reshadeApi) || string.IsNullOrEmpty(reshadeApi))
                    reshadeApi = inspectResult != null && !string.IsNullOrEmpty(inspectResult.RecommendedReshadeApi)
                        ? inspectResult.RecommendedReshadeApi
                        : (fastPe != null && !string.IsNullOrEmpty(fastPe.RecommendedReshadeApi) ? fastPe.RecommendedReshadeApi : "reshade-feeder");

                string renoDx;
                if (!_gameRenoDx.TryGetValue(key, out renoDx) || string.IsNullOrEmpty(renoDx)) renoDx = "on";
                string lumenite;
                if (!_gameLumenite.TryGetValue(key, out lumenite) || string.IsNullOrEmpty(lumenite)) lumenite = "off";
                string lumeniteZip;
                _gameLumeniteZips.TryGetValue(key, out lumeniteZip);

                ApplyBaseModeToUi(baseMode, reshadeApi, renoDx, lumenite, lumeniteZip);
                _modeAppliedForKey = key;
            }

            ApplyModeVisibility();
        }

        void ApplyModeVisibility()
        {
            bool isRtxMc = IsRtxRemixMinecraftSelected();
            var baseMode = GetSelectedBaseMode();
            var isOpti = (baseMode == "optiscaler");
            bool mcMochiLocked = isRtxMc && !IsMcMochiUnlocked();
            bool mcMochiUnlocked = isRtxMc && IsMcMochiUnlocked();
            bool openRrUnsupportedLocked = isOpti && IsOpenRrUnsupportedLocked();
            bool openRrUnsupportedForceUnlocked = isOpti && IsOpenRrUnsupportedForceUnlocked();

            if (_modeCardsGrid != null) _modeCardsGrid.Visibility = isRtxMc ? Visibility.Collapsed : Visibility.Visible;
            if (_rtxRemixMcModeCard != null) _rtxRemixMcModeCard.Visibility = isRtxMc ? Visibility.Visible : Visibility.Collapsed;

            if (_mochiCard != null) _mochiCard.Visibility = mcMochiLocked ? Visibility.Collapsed : Visibility.Visible;
            if (_mochiCollapsedBar != null) _mochiCollapsedBar.Visibility = mcMochiLocked ? Visibility.Visible : Visibility.Collapsed;
            if (_mochiMcWarningBadge != null) _mochiMcWarningBadge.Visibility = mcMochiUnlocked ? Visibility.Visible : Visibility.Collapsed;
            if (_relockMochiMcBtn != null) _relockMochiMcBtn.Visibility = mcMochiUnlocked ? Visibility.Visible : Visibility.Collapsed;
            if (_mochiMcWarningText != null) _mochiMcWarningText.Visibility = mcMochiUnlocked ? Visibility.Visible : Visibility.Collapsed;

            if (_openRrCard != null) _openRrCard.Visibility = (isOpti && !openRrUnsupportedLocked) ? Visibility.Visible : Visibility.Collapsed;
            if (_openRrUnsupportedBar != null) _openRrUnsupportedBar.Visibility = openRrUnsupportedLocked ? Visibility.Visible : Visibility.Collapsed;
            if (_openRrCollapsedBar != null) _openRrCollapsedBar.Visibility = isOpti ? Visibility.Collapsed : Visibility.Visible;
            if (_openRrNoDlssdWarningBadge != null) _openRrNoDlssdWarningBadge.Visibility = openRrUnsupportedForceUnlocked ? Visibility.Visible : Visibility.Collapsed;
            if (_relockOpenRrBtn != null) _relockOpenRrBtn.Visibility = openRrUnsupportedForceUnlocked ? Visibility.Visible : Visibility.Collapsed;
            if (_openRrNoDlssdWarningText != null) _openRrNoDlssdWarningText.Visibility = openRrUnsupportedForceUnlocked ? Visibility.Visible : Visibility.Collapsed;
            if (_mochiRequiredBadge != null) _mochiRequiredBadge.Visibility = isOpti ? Visibility.Collapsed : Visibility.Visible;

            if (_proxyCard != null) _proxyCard.Visibility = (isOpti && !isRtxMc) ? Visibility.Visible : Visibility.Collapsed;
            if (_standaloneModeCard != null) _standaloneModeCard.Visibility = (!isRtxMc && baseMode == "standalone") ? Visibility.Visible : Visibility.Collapsed;
            if (_reShadeModeCard != null) _reShadeModeCard.Visibility = (!isRtxMc && baseMode == "reshade") ? Visibility.Visible : Visibility.Collapsed;

            if (_compatPanel != null) _compatPanel.Visibility = (isOpti && !isRtxMc) ? Visibility.Visible : Visibility.Collapsed;
            if (_optiScalerInstallExtras != null) _optiScalerInstallExtras.Visibility = isOpti ? Visibility.Visible : Visibility.Collapsed;
            if (_optiPatcherToggle != null) _optiPatcherToggle.Visibility = isRtxMc ? Visibility.Collapsed : Visibility.Visible;
            if (_optiScalerIniButtonsLeft != null) _optiScalerIniButtonsLeft.Visibility = isOpti ? Visibility.Visible : Visibility.Collapsed;
            if (_iniApplyButton != null) _iniApplyButton.Visibility = isOpti ? Visibility.Visible : Visibility.Collapsed;
            if (_optiScalerIniProfileGroup != null) _optiScalerIniProfileGroup.Visibility = isOpti ? Visibility.Visible : Visibility.Collapsed;

            if (_lumeniteZipRow != null)
            {
                bool lumOn = _lumeniteCheck != null && _lumeniteCheck.IsChecked == true;
                _lumeniteZipRow.Visibility = (!isRtxMc && baseMode == "reshade" && lumOn) ? Visibility.Visible : Visibility.Collapsed;
            }

            _updatingBackends = true;
            try
            {
                if (!isOpti)
                {
                    if (_mochiCheck != null)
                    {
                        _mochiCheck.IsChecked = true;
                        _mochiCheck.IsEnabled = false;
                    }
                    _selectingOpenRr = true;
                    _selectingOptiPatcher = true;
                    try
                    {
                        if (_openRrToggle != null) _openRrToggle.IsChecked = false;
                        if (_optiPatcherToggle != null) _optiPatcherToggle.IsChecked = false;
                    }
                    finally
                    {
                        _selectingOpenRr = false;
                        _selectingOptiPatcher = false;
                    }
                    _decisions.OpenRr = "skip";
                    _decisions.OptiPatcher = "skip";
                }
                else
                {
                    if (_mochiCheck != null)
                    {
                        if (mcMochiLocked)
                        {
                            _mochiCheck.IsChecked = false;
                            _mochiCheck.IsEnabled = false;
                            _activeBackend = "";
                        }
                        else
                        {
                            _mochiCheck.IsEnabled = true;
                        }
                    }
                    if (isRtxMc)
                    {
                        _selectingOptiPatcher = true;
                        try
                        {
                            if (_optiPatcherToggle != null) _optiPatcherToggle.IsChecked = false;
                        }
                        finally { _selectingOptiPatcher = false; }
                        _decisions.OptiPatcher = "skip";
                    }
                    if (openRrUnsupportedLocked)
                    {
                        _selectingOpenRr = true;
                        try
                        {
                            if (_openRrToggle != null)
                            {
                                _openRrToggle.IsChecked = false;
                                _openRrToggle.IsEnabled = false;
                            }
                        }
                        finally { _selectingOpenRr = false; }
                        _decisions.OpenRr = "skip";
                    }
                    else
                    {
                        if (_openRrToggle != null) _openRrToggle.IsEnabled = true;
                    }
                }
            }
            finally { _updatingBackends = false; }

            if (isOpti && !_loadingSettings && _selectedGame != null)
            {
                RestoreBackendChoice();
                UpdateProxyRecommendation();
            }
            else if (_selectedGame != null)
            {
                RefreshOpenRrCard();
            }

            RefreshBackendHints();
            UpdateModeBanners();
            RefreshModelImportBar();
            UpdatePrimaryActionText();
            UpdatePrimaryButtonState();
        }

        void UpdateModeBanners()
        {
            var installed = CurrentInstalledModeForSelectedGame();
            var selected = GetSelectedInstallMode();
            var detectedApi = (_lastInspectResult != null && !string.IsNullOrEmpty(_lastInspectResult.DetectedApi))
                ? _lastInspectResult.DetectedApi
                : (_lastPeInfo != null ? _lastPeInfo.DetectedApi : null);

            if (_modeDetectBadge != null && _modeDetectText != null)
            {
                if (_selectedGame == null || !_selectedGame.HasExecutable)
                {
                    _modeDetectBadge.Visibility = Visibility.Collapsed;
                }
                else if (!string.IsNullOrEmpty(installed))
                {
                    _modeDetectText.Text = L("已安装：", "Installed: ") + FormatModeDisplayName(installed);
                    _modeDetectBadge.Visibility = Visibility.Visible;
                }
                else if (!string.IsNullOrEmpty(detectedApi) && !string.Equals(detectedApi, "Unknown", StringComparison.OrdinalIgnoreCase))
                {
                    _modeDetectText.Text = L("检测到接口：", "Detected API: ") + detectedApi;
                    _modeDetectBadge.Visibility = Visibility.Visible;
                }
                else
                {
                    _modeDetectBadge.Visibility = Visibility.Collapsed;
                }
            }

            if (_modeSwitchNotice != null && _modeSwitchNoticeText != null)
            {
                bool switching = _selectedGame != null && _selectedGame.Installed &&
                    !string.IsNullOrEmpty(installed) &&
                    !string.Equals(installed, selected, StringComparison.OrdinalIgnoreCase);
                if (switching)
                {
                    _modeSwitchNoticeText.Text = string.Format(
                        L("切换安装模式（{0} -> {1}）：点击安装时将先卸载旧模式并保留已有模型权重。", "Mode switch ({0} -> {1}): Installing will uninstall the previous mode first while preserving existing model weights."),
                        FormatModeDisplayName(installed),
                        FormatModeDisplayName(selected));
                    _modeSwitchNotice.Visibility = Visibility.Visible;
                }
                else
                {
                    _modeSwitchNotice.Visibility = Visibility.Collapsed;
                }
            }
        }

        string FormatModeDisplayName(string mode)
        {
            if (string.IsNullOrEmpty(mode)) return "";
            switch (mode.ToLowerInvariant())
            {
                case "optiscaler": return L("OptiScaler（默认）", "OptiScaler (Default)");
                case "rtx-remix-mc": return "RTX Remix (MC)";
                case "standalone": return L("独立直通 (Standalone)", "Standalone");
                case "reshade": return L("ReShade 兼容模式", "ReShade Compatible");
                case "reshade-feeder": return "ReShade (DX10/11/12)";
                case "reshade-feeder-dx9": return "ReShade (DX9 · DXVK)";
                case "reshade-feeder-vulkan": return "ReShade (Vulkan)";
                default: return mode;
            }
        }

        string FormatModeShortTag(string mode)
        {
            if (string.IsNullOrEmpty(mode)) return "";
            switch (mode.ToLowerInvariant())
            {
                case "optiscaler": return "OptiScaler";
                case "rtx-remix-mc": return "RTX Remix";
                case "standalone": return "Standalone";
                case "reshade":
                case "reshade-feeder": return "ReShade";
                case "reshade-feeder-dx9": return "ReShade DX9";
                case "reshade-feeder-vulkan": return "ReShade VK";
                default: return mode;
            }
        }

        void RefreshModesLanguage()
        {
            SetLabel("InstallModeHeading", "安装模式", "Installation Mode");
            SetLabel("ModeOptiTitle", "OptiScaler", "OptiScaler");
            SetLabel("ModeOptiTag", "推荐", "Default");
            SetCardDescription("ModeOptiDesc", "大多数场景选这个", "Best for most cases", "DLSS / FSR / XeSS 现代游戏 · 支持 OpenRR / OptiPatcher", "Modern DLSS / FSR / XeSS games · OpenRR & OptiPatcher");
            SetLabel("ModeStandaloneTitle", "独立直通", "Standalone");
            SetLabel("ModeStandaloneTag", "直通", "Direct");
            SetCardDescription("ModeStandaloneDesc", "视频播放器、OBS 等非游戏程序", "Video players, OBS and other apps", "DX11/12 / Vulkan · 视频 / OBS / 任意程序 · 免超分接管", "DX11/12 / Vulkan · video players, OBS, generic apps");
            SetLabel("ModeReShadeTitle", "ReShade 兼容模式", "ReShade Compatible");
            SetLabel("ModeReShadeTag", "经典兼容", "Legacy");
            SetCardDescription("ModeReShadeDesc", "推荐老游戏和 DX9 游戏", "Best for older and DX9 games", "DX9 (DXVK) / DX10-12 / Vulkan 老游戏 · 支持 LumeniteFX", "DX9 (DXVK) / DX10-12 / Vulkan legacy games · LumeniteFX");
            SetLabel("RtxRemixMcTitle", "RTX Remix for Minecraft Java", "RTX Remix for Minecraft Java");
            SetLabel("RtxRemixMcTag", "专用模式", "Dedicated");
            SetCardDescription("RtxRemixMcDesc", "自动部署并备份原文件，卸载可还原。建议只开 OpenRR。", "Auto-deploys with backups. Uninstall restores originals. Use OpenRR only.",
                "自动向 remix-sdk\\ 原名部署 OptiScaler.dll 与 AMD 兼容版 remixbridge.dll（自动备份 NVIDIA 原版，完整卸载可无损还原），并自动启用 VulkanExtensionSpoofing = true。建议使用 OpenRR 光线重建；Mochizuki 神经渲染默认锁定且不建议用于 MC。",
                "Deploys unrenamed OptiScaler.dll + AMD-patched remixbridge.dll into remix-sdk\\ (backs up original NVIDIA remixbridge.dll for clean uninstall) and automatically sets VulkanExtensionSpoofing = true. OpenRR is recommended; Mochizuki NR is locked and not recommended for MC by default.");
            SetLabel("MochiRequiredText", "当前模式必选", "Required");
            SetCardDescription("MochiCollapsedText", "Minecraft 默认关闭 Mochizuki", "Mochizuki is off by default for Minecraft",
                "RTX Remix Minecraft 不推荐使用神经渲染（Mochizuki NR），已默认锁定",
                "Neural Rendering (Mochizuki NR) is not recommended for RTX Remix Minecraft and is locked by default");
            SetLabel("UnlockMochiMcBtn", "仍要解锁", "Unlock anyway");
            SetLabel("RelockMochiMcBtn", "重新锁定", "Lock again");
            SetLabel("MochiMcWarningBadgeText", "不建议用于 MC", "Not recommended for MC");
            SetLabel("MochiMcWarningText",
                "⚠ 不建议在 RTX Remix Minecraft 中启用神经渲染（NR）：可能导致像素纹理风格失真、闪烁或额外性能开销，仅建议自行测试。",
                "⚠ Not recommended for RTX Remix Minecraft: Neural Rendering (NR) may cause voxel texture distortion, shimmering, or extra GPU overhead; for testing only.");
            SetLabel("OpenRrCollapsedText",
                "当前模式仅适用 DLSSNR-AMD；OpenRR 与 OptiPatcher 已自动禁用",
                "Current mode runs DLSSNR-AMD only; OpenRR & OptiPatcher are disabled.");
            SetLabel("UnlockOpenRrBtn", "强制解锁", "Force unlock");
            SetLabel("RelockOpenRrBtn", "重新锁定", "Lock again");
            SetLabel("OpenRrNoDlssdWarningBadgeText", "未检测到原生 DLSSD", "No native DLSSD");
            SetLabel("OpenRrNoDlssdWarningText",
                "⚠ 当前游戏未检测到原生 nvngx_dlssd.dll（可能不支持 DLSS 光线重建）：强行安装通常无效，仅建议高级用户手动测试。",
                "⚠ No native nvngx_dlssd.dll detected in this game (may not support DLSS Ray Reconstruction): forcing OpenRR usually has no effect; for advanced testing only.");
            SetLabel("SwitchToOptiScalerBtn", "切回 OptiScaler", "Switch to OptiScaler");

            SetLabel("StandaloneCardHeading", "独立直通配置", "Standalone Configuration");
            SetCardDescription("StandaloneCardDesc", "无需游戏内置超分支持", "No in-game upscaler required",
                "直接部署 dxgi.dll + dlssnr-amd.asi + vulkan-1.dll 与 Vulkan 隐式层（vk-override），在 Present 阶段以无向导 RenoDX 模式运行。无需游戏内置 DLSS/FSR/XeSS，适合视频播放器（mpv/PotPlayer/VLC）、OBS、Magpie 或各类 DirectX 11/12/Vulkan 程序。",
                "Deploys dxgi.dll + dlssnr-amd.asi + vulkan-1.dll and the Vulkan implicit layer (vk-override), running guide-less RenoDX at Present. No in-game DLSS/FSR/XeSS required; ideal for video players, OBS, Magpie, or DirectX 11/12/Vulkan apps.");
            SetLabel("StandaloneHotkeysText",
                "End 控制面板 · F6 开关 · F5 截图 · Ctrl+F11 切换网络",
                "End: panel · F6: toggle · F5: screenshot · Ctrl+F11: network");
            ((TextBlock)_window.FindName("StandaloneHotkeysText")).ToolTip = L("截图保存至 NR Screenshots", "Screenshots are saved to NR Screenshots");
            ((TextBlock)_window.FindName("StandaloneCardHeading")).ToolTip = L("安装到受保护目录时，应用数据与日志写入 %LOCALAPPDATA%\\DLSSNR-AMD\\。", "In protected folders, runtime data and logs use %LOCALAPPDATA%\\DLSSNR-AMD\\.");
            ((TextBlock)_window.FindName("StandaloneProtectedNote")).Visibility = Visibility.Collapsed;

            SetLabel("ReShadeCardHeading", "ReShade 兼容配置", "ReShade Compatibility Settings");
            SetLabel("ReshadeApiLabel", "图形接口", "Graphics API");
            SetLabel("ReshadeApiDxRadio", "DX10 / DX11 / DX12", "DX10 / DX11 / DX12");
            SetLabel("ReshadeApiDx9Radio", "DX9 · DXVK", "DX9 · DXVK");
            SetLabel("ReshadeApiVulkanRadio", "Vulkan", "Vulkan");
            SetLabel("ReshadeStyleLabel", "前后处理风格", "Processing Style");
            SetLabel("ReshadeStyleRenoDxRadio", "RenoDX", "RenoDX");
            SetLabel("ReshadeStyleWilsjo2Radio", "wilsjo2 经典风格", "wilsjo2 Classic Style");
            SetLabel("LumeniteCheck", "启用 LumeniteFX 运动矢量", "Enable LumeniteFX motion vectors");
            ((CheckBox)_window.FindName("LumeniteCheck")).ToolTip = L("替代内置 VORT", "Replaces bundled VORT");
            SetLabel("LumeniteHintText",
                "留空本地 ZIP 时，安装将自动从 GitHub 下载并校验兼容版本 f80e98d。",
                "Leave local ZIP empty to download & verify pinned LumeniteFX commit f80e98d from GitHub.");
            SetLabel("LumeniteZipBrowseBtn", "选择本地 ZIP", "Browse ZIP");
            SetLabel("LumeniteZipClearBtn", "清除", "Clear");

            SetLabel("OpenRrUnsupportedText", "OpenRR 已锁定：未检测到原生 DLSSD", "OpenRR locked: no native DLSSD detected");
            ((TextBlock)_window.FindName("OpenRrUnsupportedText")).ToolTip = L("游戏可能不支持 DLSS 光线重建；强制解锁仅供高级用户测试。", "This game may not support DLSS Ray Reconstruction. Force unlock is for advanced testing only.");
            UpdateModeBanners();
        }
    }
}
