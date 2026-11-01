using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        IniProfiles _iniProfiles;
        ComboBox _iniProfileCombo;
        TextBox _iniSearchBox;
        TextBlock _iniProfileStatus;
        Button _iniSaveButton, _iniQuickSaveButton, _iniImportButton, _iniRenameButton, _iniDeleteButton, _iniApplyButton, _iniFolderButton;
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
            _iniRenameButton = _window.FindName("IniRenameButton") as Button;
            _iniDeleteButton = _window.FindName("IniDeleteButton") as Button;
            _iniApplyButton = _window.FindName("IniApplyButton") as Button;
            _iniFolderButton = _window.FindName("IniFolderButton") as Button;
            InitShareCode();
            InitModelImport();
            InitUnlockerPlugin();
            try { _iniProfiles = new IniProfiles(_settingsDir); }
            catch (Exception ex) { IniFailure(ex); }
            if (_iniProfileCombo != null) _iniProfileCombo.SelectionChanged += delegate { IniSelectionChanged(); };
            if (_iniSearchBox != null) _iniSearchBox.TextChanged += delegate { if (!_iniUpdating) PopulateIniProfiles(); };
            if (_iniSaveButton != null) _iniSaveButton.Click += delegate { PromptSaveIniProfile(); };
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.Click += delegate {
                var game = CaptureIniGame();
                if (!CanUseIniGame(game)) return;
                SaveIniProfileCore(game, game.Name + L(" 保存配置", " profile"));
            };
            if (_iniImportButton != null) _iniImportButton.Click += delegate { PromptImportIniProfile(); };
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
            SetLabel("InstallOptionsHeading", "安装选项（下次安装或更新时生效）", "Install options (applied on the next install or update)");
            SetLabel("IniHeading", "OptiScaler.ini 共享配置（整文件）", "Shared OptiScaler.ini profiles (whole file)");
            SetLabel("IniSelectHint", "选择配置后点击“应用所选配置”。仅更改插件时请选择“当前游戏 INI”。", "Select a profile, then Apply selection. Choose Current game INI to apply only the plugin setting.");
            SetLabel("PluginHeading", "第三方插件", "Third-party plugins");
            if (_iniSaveButton != null) _iniSaveButton.Content = L("保存…", "Save…");
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.Content = L("快速保存", "Quick save");
            if (_iniImportButton != null) _iniImportButton.Content = L("导入…", "Import…");
            if (_iniRenameButton != null) _iniRenameButton.Content = L("重命名…", "Rename…");
            if (_iniDeleteButton != null) _iniDeleteButton.Content = L("删除", "Delete");
            if (_iniApplyButton != null) _iniApplyButton.Content = L("应用所选配置", "Apply selection");
            if (_iniFolderButton != null) _iniFolderButton.Content = L("配置文件夹", "Profiles folder");
            if (_iniSearchBox != null) _iniSearchBox.ToolTip = L("搜索共享配置", "Search shared profiles");
            RefreshUnlockerLanguage();
            PopulateIniProfiles();
            RenderIniStatus();
            RefreshProfileActions();
        }

        // Called synchronously immediately after _selectedGame is changed, including null selections.
        void SelectGameProfiles()
        {
            _iniSelectionGeneration++;
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
            if (!_iniInitialized) return;
            var available = _iniProfiles != null && !IniBusy();
            var game = CaptureIniGame();
            var hasGame = available && game != null;
            var hasIni = hasGame && File.Exists(Path.Combine(game.Directory, "OptiScaler.ini"));
            RefreshShareCode(game != null && !IniBusy() && File.Exists(Path.Combine(game.Directory, "OptiScaler.ini")));
            RefreshModelImportBar();
            var selected = hasGame && !string.IsNullOrEmpty(_iniSelectedProfile);
            if (_iniProfileCombo != null) _iniProfileCombo.IsEnabled = hasGame;
            if (_iniSearchBox != null) _iniSearchBox.IsEnabled = available;
            if (_iniSaveButton != null) _iniSaveButton.IsEnabled = hasIni;
            if (_iniQuickSaveButton != null) _iniQuickSaveButton.IsEnabled = hasIni;
            if (_iniImportButton != null) _iniImportButton.IsEnabled = hasGame;
            if (_iniRenameButton != null) _iniRenameButton.IsEnabled = selected;
            if (_iniDeleteButton != null) _iniDeleteButton.IsEnabled = selected;
            if (_iniApplyButton != null) _iniApplyButton.IsEnabled = hasIni;
            if (_iniFolderButton != null) _iniFolderButton.IsEnabled = available;
            // Single source of truth: the game-available gate AND the OptiPatcher pin are decided
            // together, so a profile-actions refresh can never re-enable a toggle the installer
            // would force back on.
            RefreshUnlockerPlugin();
        }

        bool IniBusy() { return _writeBusy || _session != null || _folderScan != null; }
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
                var current = new ComboBoxItem { Content = L("当前游戏 INI（仅插件设置）", "Current game INI (plugin setting only)"), Tag = "" };
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
                case "invalid": text = L("不是有效的 OptiScaler.ini。", "Not a valid OptiScaler.ini."); break;
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
                    : _iniProfileNames.Length == 0 ? L("暂无共享配置。可保存当前游戏 INI 或导入。", "No shared profiles yet. Save the current game INI or import one.")
                    : L("配置对所有游戏可见；选择与插件开关按游戏保存。", "Profiles are shared; selection and plugin setting are saved per game."); break;
            }
            _iniProfileStatus.Text = text;
        }
    }
}
