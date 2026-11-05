using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
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
            if (_modelImportButton != null)
                _modelImportButton.Content = L("导入模型源文件…", "Import model source file…");
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
            if (backend.Hints == null) return false;
            foreach (var hint in backend.Hints)
                foreach (var needle in needles)
                    if (string.Equals(hint, needle, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        static bool IsDlssd310_9_1(string path)
        {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return false;
            try
            {
                var v = System.Diagnostics.FileVersionInfo.GetVersionInfo(path);
                return v != null && v.FileMajorPart == 310 && v.FileMinorPart == 9 && v.FileBuildPart == 1;
            }
            catch { return false; }
        }

        bool HasValidOpenRrDlssd()
        {
            var root = ResolveImportRoot();
            if (!string.IsNullOrEmpty(root))
            {
                if (IsDlssd310_9_1(Path.Combine(root, "nvngx_dlssd.dll"))) return true;
                if (IsDlssd310_9_1(Path.Combine(root, "opennr-rr", "nvngx_dlssd.dll"))) return true;
            }
            if (_selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe))
            {
                var gameDir = Path.GetDirectoryName(_selectedGame.Exe);
                if (!string.IsNullOrEmpty(gameDir))
                {
                    if (IsDlssd310_9_1(Path.Combine(gameDir, "opennr-rr", "nvngx_dlssd.dll"))) return true;
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
                wanted += (wanted.Length == 0 ? "" : " / ") + "nvngx_dlssnr.dll（Mochizuki 模型源，310.9.16 或 310.8.0）";
            if (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true && BackendHintsContain(FindBackend("lmxxf"), "lmxxf-weights", "lmxxf-original", "lmxxf-small"))
                wanted += (wanted.Length == 0 ? "" : " / ") + "nvngx_dlssnr.dll（lmxxf 权重源）";
            var daniel = FindBackend("daniel");
            if (_danielCheck != null && _danielCheck.IsChecked == true && BackendHintsContain(daniel, "daniel-weights", "daniel-source"))
                wanted += (wanted.Length == 0 ? "" : " / ") + "nvngx_dlssnr.dll 或 dlssnr_on_amd_weights.bin（Daniel 权重源）";
            else if (_danielCheck != null && _danielCheck.IsChecked == true && BackendHintsContain(daniel, "daniel-runtime"))
                wanted += (wanted.Length == 0 ? "" : " / ") + "dlssnr_on_amd_setup.exe 或 version.dll（Daniel 运行库）";
            if (_openRrToggle != null && _openRrToggle.IsChecked == true && !HasValidOpenRrDlssd())
                wanted += (wanted.Length == 0 ? "" : " / ") + L("nvngx_dlssd.dll（OpenRR 光线重建模型源，310.9.1.0）", "nvngx_dlssd.dll (OpenRR model source, 310.9.1.0)");
            if (wanted.Length == 0) return false;
            message = L("所选后端缺少：", "Selected backends are missing: ") + wanted +
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
            var enabled = _openRrToggle == null || _openRrToggle.IsChecked == true;
            var hasDlssd = HasValidOpenRrDlssd();
            if (!enabled)
            {
                _openRrStatusText.Text = L("已关闭 · 使用内置 FSR-RR", "Off · Uses built-in FSR-RR");
                _openRrStatusText.Foreground = MissingBrush;
                _openRrBadge.Background = MissingBg;
                _openRrBadge.BorderBrush = MissingBorder;
            }
            else if (hasDlssd)
            {
                _openRrStatusText.Text = L("已就绪 · nvngx_dlssd.dll 310.9.1", "Ready · nvngx_dlssd.dll 310.9.1");
                _openRrStatusText.Foreground = ReadyBrush;
                _openRrBadge.Background = ReadyBg;
                _openRrBadge.BorderBrush = ReadyBorder;
            }
            else
            {
                _openRrStatusText.Text = L("待导入 nvngx_dlssd.dll (310.9.1.0)", "Needs nvngx_dlssd.dll (310.9.1.0)");
                _openRrStatusText.Foreground = WarningBrush;
                _openRrBadge.Background = MissingBg;
                _openRrBadge.BorderBrush = WarningBrush;
            }
            if (_openRrImportButton != null)
                _openRrImportButton.IsEnabled = PresentationIdle;
        }

        void RefreshModelImportBar()
        {
            RefreshOpenRrCard();
            if (_modelImportBar == null) return;
            string message = null;
            var needs = _selectedGame != null && _selectedGame.HasExecutable && SelectedBackendNeedsImport(out message);
            var visible = needs;
            _modelImportBar.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (_modelImportText != null) _modelImportText.Text = message ?? "";
            if (_modelImportButton != null) _modelImportButton.IsEnabled = visible && PresentationIdle;
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
                if (_statusText != null) _statusText.Text = L("已导入 nvngx_dlssd.dll (310.9.1.0)，OpenRR 已就绪。", "Imported nvngx_dlssd.dll (310.9.1.0); OpenRR is ready.");
                RefreshModelImportBar();
                if (_selectedGame != null && _selectedGame.HasExecutable) InspectGame(_selectedGame);
            } catch (Exception ex) {
                DialogTheme.Show(_window, L("导入失败：", "Import failed: ") + ex.Message, _window.Title);
            }
        }

        void PickModelSourceFile()
        {
            if (IniBusyLike()) return;
            var directory = Path.GetDirectoryName(_selectedGame.Exe);
            var picker = new OpenFileDialog {
                Title = L("选择模型源文件（复制到启动器目录）", "Choose a model source file (copied into the launcher folder)"),
                Filter = L("模型源文件|nvngx_dlssnr.dll;nvngx_dlssd.dll;dlssnr_on_amd_setup.exe;dlssnr_on_amd_weights.bin|DLL (*.dll)|*.dll|EXE (*.exe)|*.exe|BIN (*.bin)|*.bin", "Model sources|nvngx_dlssnr.dll;nvngx_dlssd.dll;dlssnr_on_amd_setup.exe;dlssnr_on_amd_weights.bin|DLL (*.dll)|*.dll|EXE (*.exe)|*.exe|BIN (*.bin)|*.bin"),
                CheckFileExists = true, RestoreDirectory = true
            };
            if (!string.IsNullOrEmpty(directory)) picker.InitialDirectory = directory;
            if (picker.ShowDialog(_window) != true) return;
            try {
                var source = picker.FileName;
                // PE validation for DLL/EXE; weight BINs are plain data the installer hash-checks.
                if (!source.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
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
                if (_statusText != null) _statusText.Text = string.Format(L("已导入 {0}，正在重新检查后端…", "Imported {0}; rechecking backends…"), Path.GetFileName(source));
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
    }
}
