using System;
using System.Collections.Generic;
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

        void EnsureSharedModelSources(string root)
        {
            if (string.IsNullOrEmpty(root) || !Directory.Exists(Path.Combine(root, "dlssnr-amd"))) return;
            try
            {
                var searchDirs = new System.Collections.Generic.List<string>();
                if (!string.IsNullOrEmpty(_settingsDir))
                    searchDirs.Add(Path.Combine(_settingsDir, "models"));
                var parent = Path.GetDirectoryName(root.TrimEnd('\\', '/'));
                if (!string.IsNullOrEmpty(parent))
                    searchDirs.Add(Path.Combine(parent, "weights"));
                foreach (var dllName in new[] { "nvngx_dlssd.dll", "nvngx_dlssnr.dll" })
                {
                    var dest = Path.Combine(root, dllName);
                    if (File.Exists(dest)) continue;
                    foreach (var dir in searchDirs)
                    {
                        if (string.IsNullOrEmpty(dir)) continue;
                        var cand = Path.Combine(dir, dllName);
                        if (!File.Exists(cand)) continue;
                        if (dllName == "nvngx_dlssd.dll" && !IsDlssd310_9_1(cand)) continue;
                        try { File.Copy(cand, dest, false); break; } catch { }
                    }
                }
            }
            catch { }
        }

        void SaveToSharedModelStore(string sourcePath)
        {
            if (string.IsNullOrEmpty(sourcePath) || string.IsNullOrEmpty(_settingsDir)) return;
            try
            {
                var modelsDir = Path.Combine(_settingsDir, "models");
                Directory.CreateDirectory(modelsDir);
                var dest = Path.Combine(modelsDir, Path.GetFileName(sourcePath));
                if (!string.Equals(Path.GetFullPath(sourcePath), Path.GetFullPath(dest), StringComparison.OrdinalIgnoreCase))
                    File.Copy(sourcePath, dest, true);
            }
            catch { }
        }

        bool HasValidOpenRrDlssd()
        {
            var root = ResolveImportRoot();
            if (!string.IsNullOrEmpty(root))
            {
                EnsureSharedModelSources(root);
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
            if (!string.IsNullOrEmpty(root) && Directory.Exists(Path.Combine(root, "opennr-rr")) && _allGames != null)
            {
                foreach (var g in _allGames)
                {
                    if (g == null || string.IsNullOrEmpty(g.Exe)) continue;
                    var gDir = Path.GetDirectoryName(g.Exe);
                    if (string.IsNullOrEmpty(gDir)) continue;
                    foreach (var rel in new[] { "nvngx_dlssd.dll", @"opennr-rr\nvngx_dlssd.dll" })
                    {
                        var cand = Path.Combine(gDir, rel);
                        if (IsDlssd310_9_1(cand))
                        {
                            try { File.Copy(cand, Path.Combine(root, "nvngx_dlssd.dll"), false); } catch { }
                            return true;
                        }
                    }
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
            if (_openRrToggle != null && _openRrToggle.IsChecked == true && !HasValidOpenRrDlssd())
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
            var enabled = _openRrToggle != null && _openRrToggle.IsChecked == true;
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
            {
                _openRrImportButton.Visibility = (enabled && !hasDlssd) ? Visibility.Visible : Visibility.Collapsed;
                _openRrImportButton.IsEnabled = PresentationIdle;
            }
            SetLabel("OpenRrDescription",
                hasDlssd
                    ? "在 RX 9000 (RDNA4) 显卡上原生运行 NVIDIA DLSS 光线重建网络（支持 D3D12 直录与 Vulkan）。已检测到 310.9.1.0 版 nvngx_dlssd.dll；取消勾选或非 RX 9000 显卡将自动使用内置 FSR 光线再生 (FSR-RR)。"
                    : "在 RX 9000 (RDNA4) 显卡上原生运行 NVIDIA DLSS 光线重建网络（支持 D3D12 直录与 Vulkan）。需导入一次 310.9.1.0 版 nvngx_dlssd.dll；取消勾选或非 RX 9000 显卡将自动使用内置 FSR 光线再生 (FSR-RR)。",
                hasDlssd
                    ? "Runs NVIDIA's DLSS Ray Reconstruction network natively on RX 9000 (RDNA4) GPUs (D3D12 & Vulkan). nvngx_dlssd.dll (310.9.1.0) is ready; unchecking or non-RX 9000 GPUs automatically use built-in FSR Ray Regeneration (FSR-RR)."
                    : "Runs NVIDIA's DLSS Ray Reconstruction network natively on RX 9000 (RDNA4) GPUs (D3D12 & Vulkan). Import nvngx_dlssd.dll (310.9.1.0) once; unchecking or non-RX 9000 GPUs automatically use built-in FSR Ray Regeneration (FSR-RR).");
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
                SaveToSharedModelStore(source);
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
                Filter = L("模型源文件|nvngx_dlssnr.dll;nvngx_dlssd.dll|DLL (*.dll)|*.dll", "Model sources|nvngx_dlssnr.dll;nvngx_dlssd.dll|DLL (*.dll)|*.dll"),
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
                SaveToSharedModelStore(source);
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
            new KeyValuePair<string, string[]>("fsr_vk", new[] { "amd_fidelityfx_vk.dll" })
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
            var mochiModel = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", @"dlssnr-amd\dlssnr.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SILVER_AARDWOLD.bin", @"dlssnr-amd\dlssnr.bin");
            var mochiSmall = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SMALL_FP8.bin", @"dlssnr-amd\dlssnr-small.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SMALL_FP8.bin", @"dlssnr-amd\dlssnr-small.bin");
            var mochiExperimental = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin", @"dlssnr-amd\dlssnr-small-experimental.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SMALL_FP8_EXPERIMENTAL.bin", @"dlssnr-amd\dlssnr-small-experimental.bin");
            var mochiInt4 = FindFirstNonEmpty(pkgRoot, @"dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin", @"dlssnr-amd\dlssnr-int4.bin")
                ?? FindFirstNonEmpty(gameDir, @"dlssnr-amd\CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin", @"dlssnr-amd\dlssnr-int4.bin");
            string mochiNv = null;
            if (!string.IsNullOrEmpty(modelSourceDll) && File.Exists(modelSourceDll)) mochiNv = modelSourceDll;
            else if (!string.IsNullOrEmpty(pkgRoot) && File.Exists(Path.Combine(pkgRoot, "nvngx_dlssnr.dll"))) mochiNv = Path.Combine(pkgRoot, "nvngx_dlssnr.dll");
            else if (!string.IsNullOrEmpty(gameDir) && File.Exists(Path.Combine(gameDir, "nvngx_dlssnr.dll"))) mochiNv = Path.Combine(gameDir, "nvngx_dlssnr.dll");

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

        bool TryFastInspect(GameInfo game, string fastProxy)
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
                foreach (var marker in new[] { "dlssnr_core.dll", "OptiScaler.asi", "dlssnr_amd.addon64" })
                    if (File.Exists(Path.Combine(gameDir, marker))) return false;
            }
            catch { return false; }

            var gen = ++_fastInspectGeneration;
            var sourceDll = _decisions != null ? _decisions.ModelSourceDll : null;
            EnsureSharedModelSources(pkgRoot);
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
                Backends = ComputeFastBackends(pkgRoot, gameDir, sourceDll)
            };
            if (_progress != null && (_session == null || !_session.IsRunning)) _progress.IsIndeterminate = false;
            if (_session == null || !_session.IsRunning) SetBusy(false, "");
            if (_statusLed != null) _statusLed.Fill = ReadyBrush;
            if (_statusText != null && (_session == null || !_session.IsRunning))
                _statusText.Text = L("成功", "success");
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
