using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        bool _diagnosticMode;
        static string NormalPackageRoot { get { return Path.GetFullPath(Path.GetDirectoryName(typeof(Program).Assembly.Location)); } }
        string _diagnosticPackageRoot = "";

        void InitDiagnostics()
        {
            ((Button)_window.FindName("DiagnosticModeButton")).Click += delegate {
                if (!CanChangePackageSource()) return;
                if (_diagnosticMode) { SetDiagnosticMode(false); return; }
                var answer = DialogTheme.Show(_window,
                    L("诊断模式允许使用其他安装包进行检测和安装，可能与当前启动器版本不同。普通安装无需开启。\n\n仅本次启动有效；关闭启动器后恢复普通模式。是否开启？",
                      "Diagnostic mode allows another package to supply inspection and installation files. Its version may differ from this launcher. Normal installation does not need this mode.\n\nIt lasts only for this session. Enable diagnostic mode?"),
                    L("开启诊断模式", "Enable diagnostic mode"), MessageBoxButton.YesNo, MessageBoxImage.Warning);
                if (answer == MessageBoxResult.Yes) SetDiagnosticMode(true);
            };
            ((Hyperlink)_window.FindName("AuthorLink")).RequestNavigate += delegate(object sender, System.Windows.Navigation.RequestNavigateEventArgs e) {
                OpenUrl("https://github.com/TheAutomatic/dlss-5-amd-project");
                e.Handled = true;
            };
        }

        bool CanChangePackageSource()
        {
            return !_writeBusy && _targetMutex == null && _folderScan == null &&
                (_session == null || _session.CanKill);
        }

        void SetDiagnosticMode(bool enabled)
        {
            if (!CanChangePackageSource() || _diagnosticMode == enabled) return;
            _diagnosticMode = enabled;
            // Enabling authorizes the remembered path; an empty path keeps the current package.
            _packageRoot = enabled && !string.IsNullOrWhiteSpace(_diagnosticPackageRoot)
                ? _diagnosticPackageRoot : NormalPackageRoot;
            PackageSourceChanged();
        }

        void SetDiagnosticPackageRoot(string path)
        {
            if (!_diagnosticMode || !CanChangePackageSource()) return;
            _diagnosticPackageRoot = (path ?? "").Trim();
            _packageRoot = string.IsNullOrEmpty(_diagnosticPackageRoot) ? NormalPackageRoot : _diagnosticPackageRoot;
            SaveSettings();
            PackageSourceChanged();
        }

        void PackageSourceChanged()
        {
            // An inspection is read-only and killable; never let its result revive the old source.
            if (_session != null) CancelReadSession();
            _resumeInstallAction = "";
            _decisions.ModelSourceDll = null;
            _decisions.MochizukiModels = null;
            _decisions.MochizukiNetworks = null;
            _decisions.MochizukiActiveModel = null;
            _decisions.LmxxfModels = null;
            _lastInspectResult = null;
            RefreshPackageComparisons(_packageRoot, true);
            RefreshDiagnosticModeUI();
            RefreshInspectionText();
            UpdatePrimaryButtonState();
            if (_selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
        }

        void RefreshDiagnosticModeUI()
        {
            SetLabel("DiagnosticModeButton", _diagnosticMode ? "关闭诊断模式" : "诊断模式…",
                _diagnosticMode ? "Disable diagnostic mode" : "Diagnostic mode…");
            var button = (Button)_window.FindName("DiagnosticModeButton");
            button.IsEnabled = CanChangePackageSource();
            var source = (FrameworkElement)_window.FindName("DiagnosticPackagePanel");
            source.Visibility = _diagnosticMode ? Visibility.Visible : Visibility.Collapsed;
            if (_packageRootBox != null) {
                _packageRootBox.IsEnabled = _diagnosticMode && CanChangePackageSource();
                if (!_packageRootBox.IsKeyboardFocusWithin) _packageRootBox.Text = _diagnosticPackageRoot;
            }
            ((Run)_window.FindName("AuthorCaption")).Text = L(" · 作者：", " · Author: ");
            SetLabel("PackageDesc", "仅诊断模式使用此目录；留空则使用当前安装包。关闭模式或重启后恢复普通来源。", "This folder is used only in diagnostic mode. Leave blank for the current package. Disabling the mode or restarting restores the normal source.");
            if (_packageBrowse != null) _packageBrowse.IsEnabled = _diagnosticMode && CanChangePackageSource();
            var indicator = (TextBlock)_window.FindName("DiagnosticModeIndicator");
            indicator.Text = L("诊断模式", "DIAGNOSTIC MODE");
            indicator.Visibility = _diagnosticMode ? Visibility.Visible : Visibility.Collapsed;
            string version = L("版本未知", "Unknown version");
            try {
                string file = Path.Combine(_packageRoot, "VERSION");
                if (File.Exists(file)) version = File.ReadAllText(file).Trim();
            } catch { }
            indicator.ToolTip = L("本次启动有效\n安装来源：", "Session only\nPackage source: ") + _packageRoot + "\n" + version;
        }

        bool HasLocalPackage()
        {
            try {
                return Path.IsPathRooted(_packageRoot) &&
                    File.Exists(Path.Combine(_packageRoot, "OptiScaler.dll")) &&
                    File.Exists(Path.Combine(_packageRoot, "launcher-worker.ps1")) &&
                    (File.Exists(Path.Combine(_packageRoot, "Setup.ps1")) ||
                     File.Exists(Path.Combine(_packageRoot, "install-amd-presr.ps1")));
            } catch (ArgumentException) { return false; }
              catch (NotSupportedException) { return false; }
              catch (PathTooLongException) { return false; }
        }
    }
}
