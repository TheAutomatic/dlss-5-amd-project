using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        IniProfiles _iniProfiles;
        IniProfiles _asiProfiles;
        ComboBox _asiProfileCombo;
        string _asiSelectedProfile = "";
        bool _asiUpdating;
        ComboBox _iniProfileCombo;
        TextBox _iniSearchBox;
        TextBlock _iniProfileStatus;
        Button _iniSaveButton, _iniQuickSaveButton, _iniImportButton, _iniExportButton, _iniRenameButton, _iniDeleteButton, _iniApplyButton, _iniFolderButton;
        Button _iniAsiExportButton, _iniAsiImportButton;
        string[] _iniProfileNames = new string[0];
        string _iniSelectedProfile = "";
        bool _iniUpdating, _iniInitialized;
        int _iniSelectionGeneration;
        string _iniStatus = "", _iniStatusDetail = "";

        sealed class IniGameIdentity
        {
            public string Key, Directory, Name;
            public int Generation;
        }

        void InitProfiles()
        {
            if (_iniInitialized) return;
            _iniInitialized = true;
            _iniProfileCombo = _window.FindName("IniProfileCombo") as ComboBox;
            _iniSearchBox = _window.FindName("IniSearchBox") as TextBox;
            _iniProfileStatus = _window.FindName("IniProfileStatus") as TextBlock;
            _iniSaveButton = _window.FindName("IniSaveButton") as Button;
            _iniQuickSaveButton = _window.FindName("IniQuickSaveButton") as Button;
            _iniImportButton = _window.FindName("IniImportButton") as Button;
            _iniExportButton = _window.FindName("IniExportButton") as Button;
            _iniRenameButton = _window.FindName("IniRenameButton") as Button;
            _iniDeleteButton = _window.FindName("IniDeleteButton") as Button;
            _iniApplyButton = _window.FindName("IniApplyButton") as Button;
            _iniFolderButton = _window.FindName("IniFolderButton") as Button;
            _iniAsiExportButton = _window.FindName("IniAsiExportButton") as Button;
            _iniAsiImportButton = _window.FindName("IniAsiImportButton") as Button;
            var more = (Popup)_window.FindName("ProfileMorePopup");
            AlignProfilePopup(more);
            ((Button)_window.FindName("ProfileMoreButton")).Click += delegate { more.IsOpen = !more.IsOpen; };
            ((StackPanel)_window.FindName("ProfileOverflowActions")).AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(delegate { more.IsOpen = false; }));
            InitShareCode();
            InitMochiHotkeys();
            InitModelImport();
            InitUnlockerPlugin();
            try { _iniProfiles = new IniProfiles(_settingsDir); }
            catch (Exception ex) { IniFailure(ex); }
            InitAsiProfiles();
            if (_iniProfileCombo != null) _iniProfileCombo.SelectionChanged += delegate { IniSelectionChanged(); };
            if (_iniSearchBox != null) _iniSearchBox.TextChanged += delegate { if (!_iniUpdating) PopulateIniProfiles(); };
            if (_iniSaveButton != null) _iniSaveButton.Click += delegate { PromptSaveIniProfile(); };
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.Click += delegate {
                var game = CaptureIniGame();
                if (!CanUseIniGame(game)) return;
                SaveIniProfileCore(game, game.Name + L(" 保存配置", " profile"));
            };
            if (_iniImportButton != null) _iniImportButton.Click += delegate { PromptImportIniProfile(); };
            if (_iniExportButton != null) _iniExportButton.Click += delegate { PromptExportIniProfile(); };
            if (_iniRenameButton != null) _iniRenameButton.Click += delegate { PromptRenameIniProfile(); };
            if (_iniDeleteButton != null) _iniDeleteButton.Click += delegate { PromptDeleteIniProfile(); };
            if (_iniApplyButton != null) _iniApplyButton.Click += delegate { ApplyIniProfile(); };
            if (_iniFolderButton != null) _iniFolderButton.Click += delegate {
                if (IniBusy() || _iniProfiles == null) return;
                try {
                    Directory.CreateDirectory(_iniProfiles.RootDirectory);
                    Process.Start(new ProcessStartInfo { FileName = _iniProfiles.RootDirectory, UseShellExecute = true });
                } catch (Exception ex) { IniFailure(ex); }
            };
            RefreshProfilesLanguage();
            SelectGameProfiles();
        }

        void RefreshProfilesLanguage()
        {
            SetLabel("InstallOptionsHeading", "安装选项", "Install options");
            ((TextBlock)_window.FindName("InstallOptionsHeading")).ToolTip = L("下次安装或更新时生效", "Applied on the next install or update");
            SetLabel("IniHeading", "画面风格与配置", "Look & config");
            SetLabel("OptiScalerConfigHeading", "OptiScaler 配置", "OptiScaler config");
            SetLabel("AsiProfilesHeading", "dlssnr-amd 配置", "dlssnr-amd config");
            SetLabel("AsiProfileApplyButton", "应用", "Apply");
            SetLabel("AsiProfileSaveButton", "保存", "Save");
            SetLabel("IniAsiImportButton", "导入…", "Import…");
            SetLabel("IniAsiExportButton", "导出…", "Export…");
            SetLabel("AsiProfileRenameButton", "重命名…", "Rename…");
            SetLabel("AsiProfileDeleteButton", "删除", "Delete");
            SetLabel("AsiProfileQuickSaveButton", "快速保存", "Quick save");
            SetLabel("AsiProfileFolderButton", "配置文件夹", "Profiles folder");
            ((Button)_window.FindName("AsiProfileMoreButton")).ToolTip = L("更多 dlssnr-amd.ini 配置操作", "More dlssnr-amd.ini profile actions");
            SetLabel("IniSelectHint", "", "");
            SetLabel("PluginHeading", "第三方插件", "Third-party plugins");
            if (_iniSaveButton != null) _iniSaveButton.Content = L("保存", "Save");
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.Content = L("快速保存", "Quick save");
            if (_iniImportButton != null) _iniImportButton.Content = L("导入…", "Import…");
            if (_iniExportButton != null) _iniExportButton.Content = L("导出…", "Export…");
            if (_iniRenameButton != null) _iniRenameButton.Content = L("重命名…", "Rename…");
            if (_iniDeleteButton != null) _iniDeleteButton.Content = L("删除", "Delete");
            if (_iniApplyButton != null) _iniApplyButton.Content = L("应用", "Apply");
            ((Button)_window.FindName("ProfileMoreButton")).ToolTip = L("更多配置操作", "More profile actions");
            if (_iniFolderButton != null) _iniFolderButton.Content = L("配置文件夹", "Profiles folder");
            if (_iniSearchBox != null) _iniSearchBox.ToolTip = L("搜索共享配置", "Search shared profiles");
            RefreshModelImportLanguage();
            RefreshUnlockerLanguage();
            PopulateIniProfiles();
            RenderIniStatus();
            RefreshProfileActions();
        }

        // Called synchronously immediately after _selectedGame is changed, including null selections.
        void SelectGameProfiles()
        {
            _iniSelectionGeneration++;
            var asiGame = CaptureIniGame();
            _asiSelectedProfile = _asiProfiles == null || asiGame == null ? "" : _asiProfiles.GetSelection(asiGame.Key);
            _iniUpdating = true;
            try
            {
                _iniSelectedProfile = "";
                _iniProfileNames = new string[0];
                _iniStatus = "";
                _iniStatusDetail = "";
                if (_iniProfileCombo != null) { _iniProfileCombo.SelectedIndex = -1; _iniProfileCombo.Items.Clear(); }
                if (_iniSearchBox != null) _iniSearchBox.Text = "";
                if (_iniProfiles != null)
                {
                    _iniProfileNames = _iniProfiles.List();
                    var game = CaptureIniGame();
                    if (game != null)
                    {
                        var saved = _iniProfiles.GetSelection(game.Key);
                        _iniSelectedProfile = FindIniName(saved);
                    }
                }
            }
            catch (Exception ex) { IniFailure(ex); }
            finally { _iniUpdating = false; }
            PopulateIniProfiles();
            RenderIniStatus();
            RefreshProfileActions();
        }

        void RefreshProfileActions()
        {
            RefreshSectionCards();
            if (!_iniInitialized) return;
            var available = _iniProfiles != null && !IniBusy();
            var game = CaptureIniGame();
            var hasGame = available && game != null;
            var hasIni = hasGame && File.Exists(Path.Combine(game.Directory, "OptiScaler.ini"));
            var activeAsiIni = game != null ? DlssnrLocalData.ResolveActiveDlssnrIni(game.Directory, _selectedGame != null ? _selectedGame.Exe : null) : null;
            RefreshShareCode(game != null && !IniBusy() && activeAsiIni != null && File.Exists(activeAsiIni));
            RefreshMochiHotkeys(!IniBusy());
            RefreshModelImportBar();
            var selected = hasGame && !string.IsNullOrEmpty(_iniSelectedProfile);
            if (_iniProfileCombo != null) _iniProfileCombo.IsEnabled = hasGame;
            if (_iniSearchBox != null) _iniSearchBox.IsEnabled = available;
            if (_iniSaveButton != null) _iniSaveButton.IsEnabled = hasIni;
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.IsEnabled = hasIni;
            if (_iniImportButton != null) _iniImportButton.IsEnabled = hasGame;
            if (_iniExportButton != null) _iniExportButton.IsEnabled = hasGame && HasIniExportSource(game, _iniSelectedProfile);
            var asiIni = activeAsiIni;
            if (_iniAsiExportButton != null) {
                _iniAsiExportButton.ToolTip = L("完整导出 dlssnr-amd.ini（ASI NR 配置），与 OptiScaler.ini 无关", "Export complete dlssnr-amd.ini (ASI NR configuration), independent of OptiScaler.ini");
                _iniAsiExportButton.IsEnabled = hasGame && asiIni != null && File.Exists(asiIni);
            }
            if (_iniAsiImportButton != null) {
                _iniAsiImportButton.ToolTip = L("保存完整的 dlssnr-amd.ini 配置；点击“应用”时备份并替换游戏 INI。", "Save a complete dlssnr-amd.ini profile; Apply backs up and replaces the game INI.");
                _iniAsiImportButton.IsEnabled = hasGame;
            }
            if (_iniRenameButton != null) _iniRenameButton.IsEnabled = selected;
            if (_iniDeleteButton != null) _iniDeleteButton.IsEnabled = selected;
            if (_iniApplyButton != null) _iniApplyButton.IsEnabled = hasIni;
            if (_iniFolderButton != null) _iniFolderButton.IsEnabled = available;
            // Single source of truth: the game-available gate AND the OptiPatcher pin are decided
            // together, so a profile-actions refresh can never re-enable a toggle the installer
            // would force back on.
            RefreshUnlockerPlugin();
            RefreshAsiProfiles();
        }

        bool IniBusy() { return _writeBusy || (_session != null && (_presentationBusy || !_session.CanKill)) || _folderScan != null; }
        IniGameIdentity CaptureIniGame()
        {
            var game = _selectedGame;
            if (game == null) return null;
            try
            {
                var directory = string.IsNullOrWhiteSpace(game.Exe) ? game.RootDirectory : Path.GetDirectoryName(game.Exe);
                if (string.IsNullOrWhiteSpace(directory)) return null;
                directory = Path.GetFullPath(directory);
                var key = string.IsNullOrEmpty(game.Id) ? "path:" + Path.GetFullPath(EntryLocation(game)).ToLowerInvariant() : game.Id;
                return new IniGameIdentity { Key = key, Directory = directory, Name = game.DisplayName ?? L("游戏", "Game"), Generation = _iniSelectionGeneration };
            }
            catch { return null; }
        }
        bool CanUseIniGame(IniGameIdentity captured)
        {
            if (IniBusy()) { SetIniStatus("busy", ""); return false; }
            var current = CaptureIniGame();
            if (_iniProfiles == null) { SetIniStatus("metadata", ""); return false; }
            if (captured == null || current == null) { SetIniStatus("game", ""); return false; }
            if (captured.Generation != current.Generation || !string.Equals(captured.Key, current.Key, StringComparison.OrdinalIgnoreCase) || !SamePath(captured.Directory, current.Directory))
            { SetIniStatus("changed", ""); return false; }
            return true;
        }
        string FindIniName(string name)
        {
            foreach (var item in _iniProfileNames) if (string.Equals(item, name, StringComparison.OrdinalIgnoreCase)) return item;
            return "";
        }
        void PopulateIniProfiles()
        {
            if (_iniProfileCombo == null) return;
            _iniUpdating = true;
            try
            {
                var query = _iniSearchBox == null ? "" : _iniSearchBox.Text.Trim();
                _iniProfileCombo.Items.Clear();
                var current = new ComboBoxItem { Content = L("当前游戏 OptiScaler.ini", "Current game OptiScaler.ini"), ToolTip = L("仅插件设置", "Plugin setting only"), Tag = "" };
                _iniProfileCombo.Items.Add(current);
                ComboBoxItem chosen = string.IsNullOrEmpty(_iniSelectedProfile) ? current : null;
                foreach (var name in _iniProfileNames)
                {
                    var isSelected = string.Equals(name, _iniSelectedProfile, StringComparison.OrdinalIgnoreCase);
                    // Keep the active selection visible while filtering; Apply must never have a hidden target.
                    if (!isSelected && query.Length > 0 && name.IndexOf(query, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    var item = new ComboBoxItem { Content = name, Tag = name };
                    _iniProfileCombo.Items.Add(item);
                    if (isSelected) chosen = item;
                }
                _iniProfileCombo.SelectedItem = chosen ?? current;
            }
            finally { _iniUpdating = false; }
        }
        void IniSelectionChanged()
        {
            if (_iniUpdating || _iniProfileCombo == null) return;
            var game = CaptureIniGame();
            var selected = _iniProfileCombo.SelectedItem as ComboBoxItem;
            if (selected == null) return;
            if (!CanUseIniGame(game)) { PopulateIniProfiles(); return; }
            try
            {
                var name = selected.Tag as string ?? "";
                _iniProfiles.SetSelection(game.Key, name);
                _iniSelectedProfile = name;
                SetIniStatus("pending", "");
            }
            catch (Exception ex) { PopulateIniProfiles(); IniFailure(ex); }
            RefreshProfileActions();
        }
        string PromptIniName(string title, string initial)
        {
            var dialog = DialogTheme.Create(_window, title, 450);
            var panel = new StackPanel { Margin = new Thickness(22) };
            panel.Children.Add(new TextBlock { Text = L("配置名称", "Profile name"), Margin = new Thickness(0, 0, 0, 8) });
            var name = new TextBox { Text = initial, MaxLength = 80 };
            panel.Children.Add(name);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 16, 0, 0) };
            var cancel = new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 85, Margin = new Thickness(4) };
            var accept = new Button { Content = L("确定", "OK"), IsDefault = true, MinWidth = 85, Margin = new Thickness(4) };
            accept.Click += delegate { if (!string.IsNullOrWhiteSpace(name.Text)) dialog.DialogResult = true; };
            DialogTheme.Primary(dialog, accept);
            buttons.Children.Add(cancel); buttons.Children.Add(accept); panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            dialog.Loaded += delegate { name.Focus(); name.SelectAll(); };
            return dialog.ShowDialog() == true ? name.Text : null;
        }
        void PromptSaveIniProfile()
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            var name = PromptIniName(L("保存共享配置", "Save shared profile"), game.Name + L(" 保存配置", " profile"));
            if (name != null) SaveIniProfileCore(game, name);
        }
        void AlignProfilePopup(Popup popup)
        {
            popup.CustomPopupPlacementCallback = delegate(Size popupSize, Size targetSize, Point offset) {
                var root = (FrameworkElement)_window.Content;
                var position = popup.PlacementTarget.TranslatePoint(new Point(), root);
                var y = position.Y + targetSize.Height + popupSize.Height + 4 <= root.ActualHeight ? targetSize.Height + 4 : -popupSize.Height - 4;
                return new[] { new CustomPopupPlacement(new Point(targetSize.Width - popupSize.Width, y), PopupPrimaryAxis.Vertical) };
            };
        }
        void InitAsiProfiles()
        {
            try { _asiProfiles = new IniProfiles(_settingsDir, "dlssnr-amd.ini"); }
            catch (Exception ex) { IniFailure(ex); }
            _asiProfileCombo = (ComboBox)_window.FindName("AsiProfileCombo");
            _asiProfileCombo.SelectionChanged += delegate {
                if (_asiUpdating) return;
                var game = CaptureIniGame();
                var item = _asiProfileCombo.SelectedItem as ComboBoxItem;
                if (item == null || !CanUseAsiProfileGame(game)) return;
                try {
                    _asiProfiles.SetSelection(game.Key, (string)item.Tag);
                    _asiSelectedProfile = (string)item.Tag;
                    SetIniStatus("pending", "");
                } catch (Exception ex) { IniFailure(ex); }
                RefreshAsiProfiles();
            };
            var popup = (Popup)_window.FindName("AsiProfileMorePopup");
            AlignProfilePopup(popup);
            ((Button)_window.FindName("AsiProfileMoreButton")).Click += delegate { popup.IsOpen = !popup.IsOpen; };
            ((StackPanel)_window.FindName("AsiProfileOverflowActions")).AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(delegate { popup.IsOpen = false; }));
            ((Button)_window.FindName("AsiProfileApplyButton")).Click += delegate { AsiProfileAction("apply"); };
            ((Button)_window.FindName("AsiProfileSaveButton")).Click += delegate { AsiProfileAction("save"); };
            _iniAsiImportButton.Click += delegate { AsiProfileAction("import"); };
            _iniAsiExportButton.Click += delegate { AsiProfileAction("export"); };
            ((Button)_window.FindName("AsiProfileRenameButton")).Click += delegate { AsiProfileAction("rename"); };
            ((Button)_window.FindName("AsiProfileDeleteButton")).Click += delegate { AsiProfileAction("delete"); };
            ((Button)_window.FindName("AsiProfileQuickSaveButton")).Click += delegate { AsiProfileAction("quick-save"); };
            ((Button)_window.FindName("AsiProfileFolderButton")).Click += delegate { AsiProfileAction("folder"); };
        }
        bool CanUseAsiProfileGame(IniGameIdentity game)
        {
            if (IniBusy()) { SetIniStatus("busy", ""); return false; }
            if (_asiProfiles == null) { SetIniStatus("metadata", ""); return false; }
            var current = CaptureIniGame();
            if (game == null || current == null) { SetIniStatus("game", ""); return false; }
            if (game.Generation != current.Generation || game.Key != current.Key || !SamePath(game.Directory, current.Directory))
            { SetIniStatus("changed", ""); return false; }
            return true;
        }
        string ActiveAsiProfilePath(IniGameIdentity game)
        {
            return game == null ? null : DlssnrLocalData.ResolveActiveDlssnrIni(game.Directory, _selectedGame == null ? null : _selectedGame.Exe);
        }
        void RefreshAsiProfiles()
        {
            if (_asiProfileCombo == null) return;
            var game = CaptureIniGame();
            var available = _asiProfiles != null && !IniBusy();
            var path = ActiveAsiProfilePath(game);
            var hasIni = available && path != null && File.Exists(path);
            _asiUpdating = true;
            try {
                _asiProfileCombo.Items.Clear();
                var current = new ComboBoxItem { Content = L("当前游戏 dlssnr-amd.ini", "Current game dlssnr-amd.ini"), Tag = "" };
                _asiProfileCombo.Items.Add(current);
                _asiProfileCombo.SelectedItem = current;
                foreach (var name in _asiProfiles == null ? new string[0] : _asiProfiles.List()) {
                    var item = new ComboBoxItem { Content = name, Tag = name };
                    _asiProfileCombo.Items.Add(item);
                    if (name == _asiSelectedProfile) _asiProfileCombo.SelectedItem = item;
                }
            } catch (Exception ex) { IniFailure(ex); }
            finally { _asiUpdating = false; }
            _asiProfileCombo.IsEnabled = hasIni;
            foreach (var name in new[] { "AsiProfileApplyButton", "AsiProfileSaveButton", "AsiProfileQuickSaveButton", "IniAsiImportButton", "IniAsiExportButton" })
                ((Button)_window.FindName(name)).IsEnabled = hasIni;
            foreach (var name in new[] { "AsiProfileRenameButton", "AsiProfileDeleteButton" })
                ((Button)_window.FindName(name)).IsEnabled = hasIni && !string.IsNullOrEmpty(_asiSelectedProfile);
            ((Button)_window.FindName("AsiProfileFolderButton")).IsEnabled = available;
        }
        void AsiProfileAction(string operation)
        {
            var game = CaptureIniGame();
            if (!CanUseAsiProfileGame(game)) return;
            var source = _asiSelectedProfile;
            string value = "";
            if (operation == "save" || operation == "rename") {
                value = PromptIniName(operation == "save" ? L("保存 dlssnr-amd.ini 配置", "Save dlssnr-amd.ini profile") : L("重命名配置", "Rename profile"),
                    operation == "save" ? game.Name + L(" 保存配置", " profile") : source);
                if (value == null) return;
            } else if (operation == "import") {
                var picker = new OpenFileDialog { Title = L("导入 dlssnr-amd.ini 配置", "Import dlssnr-amd.ini profile"), Filter = "INI (*.ini)|*.ini", CheckFileExists = true, RestoreDirectory = true };
                if (picker.ShowDialog(_window) != true) return;
                source = picker.FileName;
                value = Path.GetFileNameWithoutExtension(source);
            } else if (operation == "export") {
                var picker = new SaveFileDialog { FileName = "dlssnr-amd.ini", Filter = "INI (*.ini)|*.ini", OverwritePrompt = true, RestoreDirectory = true };
                if (picker.ShowDialog(_window) != true) return;
                value = picker.FileName;
            } else if (operation == "delete" && DialogTheme.Show(_window,
                L("删除共享配置？游戏 INI 不会改变。", "Delete this shared profile? The game INI will not change."), L("删除配置", "Delete profile"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
            RunAsiProfileAction(game, operation, value, source);
        }
        bool RunAsiProfileAction(IniGameIdentity game, string operation, string value, string source)
        {
            if (!CanUseAsiProfileGame(game)) return false;
            bool locked = false, busy = false;
            try {
                if (operation == "folder") {
                    Directory.CreateDirectory(_asiProfiles.RootDirectory);
                    Process.Start(new ProcessStartInfo { FileName = _asiProfiles.RootDirectory, UseShellExecute = true });
                    return true;
                }
                var target = ActiveAsiProfilePath(game);
                if (target == null || !File.Exists(target)) { SetIniStatus("noasi", ""); return false; }
                var status = "";
                switch (operation) {
                    case "save": case "quick-save":
                        _asiSelectedProfile = _asiProfiles.Save(target, operation == "save" ? value : game.Name + L(" 保存配置", " profile"));
                        status = "saved"; break;
                    case "import": _asiSelectedProfile = _asiProfiles.Import(source, value); status = "imported"; break;
                    case "rename": _asiProfiles.Rename(source, value); _asiSelectedProfile = _asiProfiles.GetSelection(game.Key); status = "renamed"; break;
                    case "delete": _asiProfiles.Delete(source); _asiSelectedProfile = ""; status = "deleted"; break;
                    case "export":
                        if (string.IsNullOrEmpty(source)) File.WriteAllBytes(value, File.ReadAllBytes(target));
                        else _asiProfiles.Export(source, value);
                        SetIniStatus("asi-exported", Path.GetFileName(value)); return true;
                    case "apply":
                        locked = TryLockTarget(game.Directory);
                        if (!locked) { SetIniStatus("locked", ""); return false; }
                        SetBusy(true, "asi-profile"); busy = true; RefreshProfileActions();
                        var backup = _asiProfiles.Apply(Path.GetDirectoryName(target), source);
                        SetIniStatus("asi-imported", Path.GetFileName(backup)); return true;
                    default: return false;
                }
                _asiProfiles.SetSelection(game.Key, _asiSelectedProfile);
                SetIniStatus(status, _asiSelectedProfile);
                return true;
            } catch (Exception ex) {
                var error = ex as IniProfileException;
                if (error != null && error.Code == "invalid") SetIniStatus("invalidasi", "");
                else IniFailure(ex);
                return false;
            }
            finally {
                if (locked) ReleaseLock();
                if (busy) { SetBusy(false, ""); UpdatePrimaryButtonState(); }
                RefreshProfileActions();
            }
        }

        bool SaveAsiProfile(string name) { return RunAsiProfileAction(CaptureIniGame(), "save", name, ""); }
        bool ApplyAsiProfile() { return RunAsiProfileAction(CaptureIniGame(), "apply", "", _asiSelectedProfile); }

        void PromptImportIniProfile()
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            var picker = new OpenFileDialog { Title = L("导入 OptiScaler.ini 配置", "Import OptiScaler.ini profile"), Filter = "INI (*.ini)|*.ini", CheckFileExists = true, RestoreDirectory = true };
            if (picker.ShowDialog(_window) != true || !CanUseIniGame(game)) return;
            var source = picker.FileName;
            var name = PromptIniName(L("导入共享配置", "Import shared profile"), Path.GetFileNameWithoutExtension(source));
            if (name != null) ImportIniProfileCore(game, source, name);
        }
        bool HasIniExportSource(IniGameIdentity game, string name)
        {
            return game != null && _iniProfiles != null && File.Exists(string.IsNullOrEmpty(name)
                ? Path.Combine(game.Directory, "OptiScaler.ini")
                : Path.Combine(_iniProfiles.RootDirectory, name + ".ini"));
        }
        SaveFileDialog CreateIniExportPicker()
        {
            return new SaveFileDialog { Title = L("导出 OptiScaler.ini 配置", "Export OptiScaler.ini profile"), FileName = "OptiScaler.ini",
                Filter = "INI (*.ini)|*.ini", OverwritePrompt = true, RestoreDirectory = true };
        }
        void PromptExportIniProfile()
        {
            var game = CaptureIniGame();
            var name = _iniSelectedProfile;
            if (!CanUseIniGame(game) || !HasIniExportSource(game, name)) return;
            var picker = CreateIniExportPicker();
            if (picker.ShowDialog(_window) == true) ExportIniProfileCore(game, name, picker.FileName);
        }
        bool ExportIniProfileCore(IniGameIdentity game, string name, string destination)
        {
            if (!CanUseIniGame(game)) return false;
            if (!HasIniExportSource(game, name)) { SetIniStatus(string.IsNullOrEmpty(name) ? "noini" : "missing", ""); return false; }
            try {
                if (string.IsNullOrEmpty(name)) File.WriteAllBytes(destination, File.ReadAllBytes(Path.Combine(game.Directory, "OptiScaler.ini")));
                else _iniProfiles.Export(name, destination);
                SetIniStatus("exported", Path.GetFileName(destination));
                return true;
            } catch (Exception ex) { IniFailure(ex); return false; }
        }
        void PromptRenameIniProfile()
        {
            var game = CaptureIniGame();
            var oldName = _iniSelectedProfile;
            if (!CanUseIniGame(game) || string.IsNullOrEmpty(oldName)) return;
            var name = PromptIniName(L("重命名共享配置", "Rename shared profile"), oldName);
            if (name != null) RenameIniProfileCore(game, oldName, name);
        }
        void PromptDeleteIniProfile()
        {
            var game = CaptureIniGame();
            var name = _iniSelectedProfile;
            if (!CanUseIniGame(game) || string.IsNullOrEmpty(name)) return;
            if (DialogTheme.Show(_window, L("删除共享配置“", "Delete shared profile \"") + name + L("”？所有游戏的该配置选择都会清除，游戏 INI 不会改变。", "\"? Its selection will be cleared for every game. Game INI files are not changed."),
                L("删除配置", "Delete profile"), MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
                DeleteIniProfileCore(game, name);
        }

        // Noninteractive entry points share the exact same guards and operations as the dialog handlers.
        bool SaveIniProfile(string name) { return SaveIniProfileCore(CaptureIniGame(), name); }
        bool ImportIniProfile(string source, string name) { return ImportIniProfileCore(CaptureIniGame(), source, name); }
        bool RenameIniProfile(string newName) { return RenameIniProfileCore(CaptureIniGame(), _iniSelectedProfile, newName); }
        bool DeleteIniProfile() { return DeleteIniProfileCore(CaptureIniGame(), _iniSelectedProfile); }
        bool SaveIniProfileCore(IniGameIdentity game, string name)
        {
            if (!CanUseIniGame(game)) return false;
            try {
                var saved = _iniProfiles.Save(Path.Combine(game.Directory, "OptiScaler.ini"), name);
                _iniProfiles.SetSelection(game.Key, saved);
                ReloadIniProfiles(saved);
                SetIniStatus("saved", saved);
                return true;
            } catch (Exception ex) { IniFailure(ex); return false; }
        }
        bool ImportIniProfileCore(IniGameIdentity game, string source, string name)
        {
            if (!CanUseIniGame(game)) return false;
            try {
                var imported = _iniProfiles.Import(source, name);
                _iniProfiles.SetSelection(game.Key, imported);
                ReloadIniProfiles(imported);
                SetIniStatus("imported", imported);
                return true;
            } catch (Exception ex) { IniFailure(ex); return false; }
        }
        bool RenameIniProfileCore(IniGameIdentity game, string oldName, string newName)
        {
            if (!CanUseIniGame(game) || string.IsNullOrEmpty(oldName)) return false;
            try {
                _iniProfiles.Rename(oldName, newName);
                ReloadIniProfiles(_iniProfiles.GetSelection(game.Key));
                SetIniStatus("renamed", IniProfiles.CleanName(newName));
                return true;
            } catch (Exception ex) { IniFailure(ex); return false; }
        }
        bool DeleteIniProfileCore(IniGameIdentity game, string name)
        {
            if (!CanUseIniGame(game) || string.IsNullOrEmpty(name)) return false;
            try {
                _iniProfiles.Delete(name);
                ReloadIniProfiles(_iniProfiles.GetSelection(game.Key));
                SetIniStatus("deleted", name);
                return true;
            } catch (Exception ex) { IniFailure(ex); return false; }
        }
        bool ApplyIniProfile()
        {
            var game = CaptureIniGame();
            var name = _iniSelectedProfile;
            if (!CanUseIniGame(game)) return false;
            var locked = false;
            var busy = false;
            try
            {
                if (!Directory.Exists(game.Directory)) throw new IniProfileException("game");
                locked = TryLockTarget(game.Directory);
                if (!locked) { SetIniStatus("locked", ""); return false; }
                SetBusy(true, "ini-profile");
                busy = true;
                RefreshProfileActions();
                var backup = _iniProfiles.Apply(game.Directory, name);
                SetIniStatus("applied", Path.GetFileName(backup));
                return true;
            }
            catch (Exception ex) { IniFailure(ex); return false; }
            finally
            {
                if (locked) ReleaseLock();
                if (busy)
                {
                    SetBusy(false, "");
                    UpdatePrimaryButtonState();
                }
                RefreshProfileActions();
            }
        }
        void ReloadIniProfiles(string selected)
        {
            _iniProfileNames = _iniProfiles.List();
            _iniSelectedProfile = FindIniName(selected);
            PopulateIniProfiles();
            RefreshProfileActions();
        }
        void IniFailure(Exception error)
        {
            var specific = error as IniProfileException;
            SetIniStatus(specific == null ? "io" : specific.Code, "");
        }
        void SetIniStatus(string status, string detail)
        {
            _iniStatus = status;
            _iniStatusDetail = detail ?? "";
            RenderIniStatus();
        }
        void RenderIniStatus()
        {
            if (_iniProfileStatus == null) return;
            string text;
            switch (_iniStatus)
            {
                case "busy": text = L("操作正在进行，请稍候。", "An operation is in progress. Please wait."); break;
                case "locked": text = L("游戏文件夹已被另一个启动器锁定。", "The game folder is locked by another launcher."); break;
                case "changed": text = L("游戏选择已改变，操作已取消。", "Game selection changed; operation cancelled."); break;
                case "game": text = L("请先选择游戏文件夹。", "Select a game folder first."); break;
                case "metadata": text = L("无法读取配置记录，请检查配置记录文件。", "Cannot read profile records. Check the profile metadata file."); break;
                case "noini": text = L("游戏没有 OptiScaler.ini，请先安装。", "No game OptiScaler.ini. Install first."); break;
                case "noasi": text = L("游戏没有 dlssnr-amd.ini，请先安装 ASI 组件。", "No game dlssnr-amd.ini. Install the ASI component first."); break;
                case "asi-exported": text = L("已完整导出 dlssnr-amd.ini：", "Exported complete dlssnr-amd.ini: ") + _iniStatusDetail; break;
                case "exported": text = L("已导出 OptiScaler.ini：", "Exported OptiScaler.ini: ") + _iniStatusDetail; break;
                case "asi-imported": text = L("已完整导入 dlssnr-amd.ini，重启 NR 后生效。原文件备份：", "Imported complete dlssnr-amd.ini; restart NR. Original backup: ") + _iniStatusDetail; break;
                case "invalid": text = L("不是有效的 OptiScaler.ini。", "Not a valid OptiScaler.ini."); break;
                case "invalidasi": text = L("不是有效的 dlssnr-amd.ini。", "Not a valid dlssnr-amd.ini."); break;
                case "extension": text = L("只能导入 .ini 文件。", "Only .ini files can be imported."); break;
                case "source": text = L("导入文件不存在。", "The import file does not exist."); break;
                case "missing": text = L("配置已不存在，请重新选择游戏以刷新列表。", "Profile no longer exists. Reselect the game to refresh."); break;
                case "name": text = L("请输入有效的配置名称。", "Enter a valid profile name."); break;
                case "exists": text = L("已有同名配置，请使用其他名称。", "That profile name already exists. Choose another name."); break;
                case "pending": text = L("已保存选择；点击“应用”后修改游戏 INI。", "Selection saved; click Apply to change the game INI."); break;
                case "saved": text = L("已保存共享配置：", "Shared profile saved: ") + _iniStatusDetail; break;
                case "imported": text = L("已导入：", "Imported: ") + _iniStatusDetail + L("。点击“应用”切换。", ". Click Apply to switch."); break;
                case "renamed": text = L("已重命名：", "Renamed: ") + _iniStatusDetail; break;
                case "deleted": text = L("已删除共享配置：", "Shared profile deleted: ") + _iniStatusDetail; break;
                case "applied": text = L("已应用，请重启游戏。原 INI 备份：", "Applied; restart the game. Original INI backup: ") + _iniStatusDetail; break;
                case "share-imported": text = L("已换上同款画面，重启游戏后生效", "New look applied; restart the game."); break;
                case "share-error": text = _iniStatusDetail; break;
                case "unlocker": text = _iniStatusDetail; break;
                case "plugins-missing": text = L("未检测到插件目录，请先安装本工具", "Plugins folder not found; install this tool first."); break;
                case "io": text = L("配置操作失败，请检查文件权限与可用空间。", "Profile operation failed. Check file permissions and free space."); break;
                default: text = _iniProfiles == null ? L("配置库不可用。", "Profile library unavailable.")
                    : _selectedGame == null ? L("选择游戏以使用共享配置。", "Select a game to use shared profiles.")
                    : _iniProfileNames.Length == 0 && (_asiProfileCombo == null || _asiProfileCombo.Items.Count <= 1) ? L("暂无共享配置", "No shared profiles") : ""; break;
            }
            _iniProfileStatus.Text = text;
            _iniProfileStatus.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        }
    }
}
