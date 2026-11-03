using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Button _mochiShareCodeButton;
        Button _styleOutButton;
        Button _styleInButton;

        void InitShareCode()
        {
            _mochiShareCodeButton = _window.FindName("MochiShareCodeButton") as Button;
            if (_mochiShareCodeButton != null) _mochiShareCodeButton.Click += delegate { ShowShareCode(false); };
            _styleOutButton = _window.FindName("StyleOutButton") as Button;
            if (_styleOutButton != null) _styleOutButton.Click += delegate { ShowShareCode(false); };
            _styleInButton = _window.FindName("StyleInButton") as Button;
            if (_styleInButton != null) _styleInButton.Click += delegate { ShowShareCode(true); };

        }

        void RefreshShareCode(bool enabled)
        {
            var title = _window.FindName("StyleShareTitle") as TextBlock;
            var sub = _window.FindName("StyleShareSub") as TextBlock;
            if (title != null) title.Text = L("画面风格分享", "Visual style sharing");
            if (sub != null)
                sub.Text = L("把你调好的画面风格变成一段分享码发给朋友，或粘贴别人的码换上同款画面",
                             "Turn your tuned look into a share code, or paste someone else's to wear it");
            if (_mochiShareCodeButton != null)
            {
                _mochiShareCodeButton.Content = SparkleLabel(L("风格码", "Style code"));
                _mochiShareCodeButton.ToolTip = L("导出/导入 Mochizuki 画面风格参数", "Export/import Mochizuki visual style parameters");
                _mochiShareCodeButton.IsEnabled = enabled;
            }
            if (_styleOutButton != null)
            {
                _styleOutButton.Content = L("晒出我的画面", "Share My Look");
                _styleOutButton.ToolTip = L("生成你当前画面风格的分享码（默认打开发出）", "Generate a share code for your current look (opens export)");
                _styleOutButton.IsEnabled = enabled;
            }
            if (_styleInButton != null)
            {
                _styleInButton.Content = L("换上同款画面", "Use a Shared Look");
                _styleInButton.ToolTip = L("粘贴别人的分享码，换上同款画面（默认打开导入）", "Paste a share code and wear that look (opens import)");
                _styleInButton.IsEnabled = enabled;
            }
        }

        // WPF renders emoji as monochrome glyphs (no color-font support), so the sparkle is a
        // vector path in emoji gold instead of the ✨ character
        object SparkleLabel(string text)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            var star = new System.Windows.Shapes.Path
            {
                Data = System.Windows.Media.Geometry.Parse("M 6 0 L 7.2 4.2 L 11.5 5.5 L 7.2 6.8 L 6 11 L 4.8 6.8 L 0.5 5.5 L 4.8 4.2 Z"),
                Fill = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0xF5, 0xC5, 0x18)),
                Stroke = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0xD4, 0x9A, 0x0A)),
                StrokeThickness = 0.4,
                Width = 11, Height = 11, Stretch = System.Windows.Media.Stretch.Uniform,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 4, 0)
            };
            var mini = new System.Windows.Shapes.Path
            {
                Data = System.Windows.Media.Geometry.Parse("M 2.6 0 L 3.1 1.5 L 4.6 2 L 3.1 2.5 L 2.6 4 L 2.1 2.5 L 0.6 2 L 2.1 1.5 Z"),
                Fill = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0xFF, 0xE0, 0x66)),
                Width = 4.5, Height = 4.5, Stretch = System.Windows.Media.Stretch.Uniform,
                VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(-9, 0, 0, 0)
            };
            var label = new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center };
            panel.Children.Add(star); panel.Children.Add(mini); panel.Children.Add(label);
            return panel;
        }

        static string ExtractCode(string text) { return MochizukiShareCode.ExtractCode(text); }

        string ShareError(Exception error)
        {
            var code = error as MochizukiShareCode.CodeException;
            if (code == null) return L("操作没成功，再试一次或检查文件权限。", "The operation failed. Try again or check file permissions.");
            switch (code.Reason) {
                case "old": return L("旧版风格码不适用于 ASI 版本，请在当前版本重新导出。",
                                      "Old style codes do not apply to the ASI version. Export again with this version.");
                case "prefix": return L("没认出画面分享码。确认复制了完整的分享内容（里面会有一段 MZ4 开头的字符）。",
                                        "No share code found. Make sure you copied the full share text (including the MZ4 part).");
                case "crc": return L("内容好像没复制全，少了一截。把整段分享内容重新复制一遍试试？",
                                     "Looks like part of the text was cut off. Try copying the whole thing again.");
                case "index": return L("这段分享码来自更新的版本，更新启动器后再试。", "This share code needs a newer launcher. Update first.");
                case "range": return L("这段分享码的内容有问题，可能没复制全。", "The share code looks incomplete.");
                case "duplicate": return L("这段分享码的内容有问题，可能没复制全。", "The share code looks incomplete.");
                case "encoding": return L("游戏设置文件不是常见的编码，无法读取。", "The game INI uses an unsupported encoding.");
                case "value": return L("这段分享码的内容有问题，可能没复制全。", "The share code looks incomplete.");
                case "unknown": return L("这段分享码包含当前安装包不支持的字段，请更新启动器。",
                                         "This share code uses settings this launcher does not support. Update the launcher.");
                case "layer": return L("分享码里的图层编号无效。", "The share code has an invalid layer number.");
                case "version": return L("分享码版本不受支持，请更新启动器。", "The share code version is not supported. Update the launcher.");
                default: return L("没认出画面分享码。确认复制了完整的分享内容。", "No share code recognized. Copy the full share text.");
            }
        }

        string ShareDisplayValue(string key, string value, bool chinese)
        {
            if (string.Equals(key, "Placement", StringComparison.OrdinalIgnoreCase)) {
                string v = (value ?? "").Trim().ToLowerInvariant();
                if (v == "before") return chinese ? "超分前 (before)" : "Before upscaler (before)";
                if (v == "after") return chinese ? "超分后 (after)" : "After upscaler (after)";
                if (v == "present") return chinese ? "最佳效果 (present)" : "Best result (present)";
            }
            return value;
        }

        string ShareGroup(string section, int layer, string key, bool chinese)
        {
            if (layer > 0) {
                string cat = ShareCategory(key, chinese);
                return (chinese ? "图层 " : "Layer ") + layer + (cat.Length > 0 ? " · " + cat : "");
            }
            if (string.Equals(section, "Network", StringComparison.OrdinalIgnoreCase))
                return chinese ? "网络" : "Network";
            if (string.Equals(section, "Preprocess", StringComparison.OrdinalIgnoreCase))
                return chinese ? "前处理" : "Preprocess";
            if (string.Equals(section, "RenoDX", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(section, "wilsjo2", StringComparison.OrdinalIgnoreCase))
                return (chinese ? "全局 · " : "Global · ") + ShareCategory(key, chinese);
            return "[" + section + "]";
        }

        string ShareCategory(string key, bool chinese)
        {
            if (key == "Processing" || key == "Placement" || key == "Network" || key == "Enabled")
                return chinese ? "处理" : "Processing";
            if (key.StartsWith("NRLook", StringComparison.OrdinalIgnoreCase)) return "Look";
            if (key.StartsWith("NRPass", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRFast", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("Pass", StringComparison.OrdinalIgnoreCase) ||
                key == "NRUnlockPasses" || key == "NRChainedHistory" || key == "NRNoiseEveryFrame" ||
                key == "Passes" || key == "UnlockPasses" || key == "NoiseEveryFrame" || key == "PRESET" ||
                key == "Preset" || key == "Style" || key == "Intensity")
                return "Pass";
            if (key.StartsWith("NRSdr", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRDisplay", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRCodec", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRSource", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRLinear", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRProxy", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRPaper", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRDiffuse", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRPQ", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRFeed", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRPresent", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRNorm", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRMVec", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRDepth", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NROutput", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("NRHook", StringComparison.OrdinalIgnoreCase))
                return chinese ? "HDR / 编码" : "HDR / Encoding";
            return chinese ? "风格" : "Style";
        }

        void RenderShareEntries(StackPanel host, IList<MochizukiShareCode.Entry> entries, bool chinese)
        {
            host.Children.Clear();
            string group = null;
            foreach (var entry in entries) {
                string next = ShareGroup(entry.Section, entry.Layer, entry.Key, chinese);
                if (group != next) {
                    group = next;
                    host.Children.Add(new TextBlock {
                        Text = next, FontWeight = FontWeights.SemiBold, FontSize = 11.5,
                        Foreground = TryFindResourceString("ThemeMuted"), Margin = new Thickness(0, 7, 0, 2)
                    });
                }
                string shown = ShareDisplayValue(entry.Key, entry.Value, chinese);
                if (entry.Reset) shown = ShareDisplayValue(entry.Key, entry.OldValue ?? entry.Value, chinese) + (chinese ? " → 默认" : " → default");
                else if (!string.IsNullOrEmpty(entry.OldValue)) shown = ShareDisplayValue(entry.Key, entry.OldValue, chinese) + " → " + shown;
                host.Children.Add(new TextBlock {
                    Text = entry.Key + " = " + shown,
                    TextWrapping = TextWrapping.Wrap, FontSize = 11.5,
                    Margin = new Thickness(10, 1, 0, 1), ToolTip = entry.Key + " = " + entry.Value,
                    Foreground = TryFindResourceString("ThemeInk")
                });
            }
        }

        void RenderShareDetails(StackPanel allHost, Button allToggle, ScrollViewer allScroll,
                                StackPanel diffHost, TextBlock diffHeader, Border diffBorder,
                                MochizukiShareCode.DecodedCode code, bool import, string path, bool chinese)
        {
            allScroll.Visibility = Visibility.Collapsed;
            diffBorder.Visibility = Visibility.Collapsed;
            if (code == null) {
                allHost.Children.Clear();
                diffHost.Children.Clear();
                allToggle.Tag = 0;
                allToggle.Content = L("全部参数 ▸", "All settings ▸");
                allToggle.IsEnabled = false;
                return;
            }
            var changed = new List<MochizukiShareCode.Entry>();
            if (import && File.Exists(path)) {
                try { changed = MochizukiShareCode.ChangedEntries(File.ReadAllBytes(path), code); } catch { }
            }
            var resets = new List<MochizukiShareCode.Entry>();
            var direct = new List<MochizukiShareCode.Entry>();
            foreach (var entry in changed) {
                if (entry.Reset) resets.Add(entry); else direct.Add(entry);
            }
            var allEntries = new List<MochizukiShareCode.Entry>(code.Entries);
            allEntries.AddRange(resets);
            allToggle.Tag = allEntries.Count;
            allToggle.IsEnabled = allEntries.Count > 0;
            allToggle.Content = (chinese ? "全部参数（" : "All settings (") + allEntries.Count + (chinese ? "） ▸" : ") ▸");
            RenderShareEntries(allHost, allEntries, chinese);
            if (!import || !File.Exists(path)) return;
            if (direct.Count == 0 && resets.Count == 0) {
                diffHost.Children.Clear();
                diffHeader.Text = L("与当前配置一致", "No changes from the current config");
                diffBorder.Visibility = Visibility.Visible;
                return;
            }
            string header = direct.Count > 0
                ? (chinese ? "与当前配置不同（" : "Changes from current config (") + direct.Count + (chinese ? " 项）" : ")")
                : (chinese ? "本次不改风格值" : "No style values overwritten");
            if (resets.Count > 0)
                header += (chinese ? " · 另有 " : " · plus ") + resets.Count + (chinese ? " 项恢复默认" : " restoring defaults");
            diffHeader.Text = header;
            var shownDirect = MochizukiShareCode.LimitEntries(direct, 30);
            if (direct.Count == 0 && resets.Count > 0) shownDirect = MochizukiShareCode.LimitEntries(resets, 30);
            RenderShareEntries(diffHost, shownDirect, chinese);
            int hidden = direct.Count + resets.Count - shownDirect.Count;
            if (hidden > 0) {
                diffHost.Children.Add(new TextBlock {
                    Text = (chinese ? "还有 " : "") + hidden + (chinese ? " 项，展开全部参数查看" : " more; open All settings"),
                    FontSize = 11, Foreground = TryFindResourceString("ThemeMuted"), Margin = new Thickness(10, 3, 0, 0)
                });
            }
            diffBorder.Visibility = Visibility.Visible;
        }

        static string[] RequiredModelFiles(MochizukiShareCode.DecodedCode code)
        {
            var files = new List<string>();
            foreach (var entry in code.Entries)
            {
                if (!string.Equals(entry.Key, "Network", StringComparison.OrdinalIgnoreCase) &&
                    !string.Equals(entry.Key, "Model", StringComparison.OrdinalIgnoreCase)) continue;
                var network = (entry.Value ?? "").Trim().ToUpperInvariant();
                string file = null;
                if (network == "CC_SMALL_FP8" || network == "SMALL") file = "dlssnr-small.bin";
                else if (network == "CC_SMALL_FP8_EXPERIMENTAL") file = "dlssnr-small-experimental.bin";
                else if (network == "CC_SILVER_AARDWOLD" || network == "ORIGINAL") file = "dlssnr.bin";
                else if (network == "CC_SILVER_AARDWOLD_INT4_MIXED") file = "dlssnr-int4.bin";
                if (file != null && !files.Contains(file)) files.Add(file);
            }
            return files.ToArray();
        }

        string MissingModelWarning(string gameDir, MochizukiShareCode.DecodedCode code)
        {
            var missing = new List<string>();
            foreach (var file in RequiredModelFiles(code))
                if (!File.Exists(Path.Combine(gameDir, "dlssnr-amd", file))) missing.Add(file);
            if (missing.Count == 0) return "";
            return L("\n缺少模型文件：", "\nMissing model files: ") + string.Join(", ", missing.ToArray()) +
                L("（不会自动替换网络；安装或生成后再套用）", " (the network is not changed here; install or generate it first)");
        }

        void ShowShareCode(bool import)
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            string path = Path.Combine(game.Directory, "dlssnr-amd.ini");
            string executable = _selectedGame.Exe;
            if (!File.Exists(path)) { SetIniStatus("noini", ""); return; }
            try {
                var dialog = DialogTheme.Create(_window, L("画面风格分享", "Visual style sharing"), 620);
                var panel = new StackPanel { Margin = new Thickness(22) };
                var modeRow = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 10) };
                var exportRadio = new RadioButton { Content = L("导出（发出我的画面）", "Export (share my look)"), GroupName = "ShareCodeMode", Margin = new Thickness(0, 0, 14, 0) };
                var importRadio = new RadioButton { Content = L("导入（换上别人的画面）", "Import (wear someone's look)"), GroupName = "ShareCodeMode" };
                modeRow.Children.Add(exportRadio); modeRow.Children.Add(importRadio);
                panel.Children.Add(modeRow);
                var hint = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) };
                panel.Children.Add(hint);
                var input = new TextBox
                {
                    TextWrapping = TextWrapping.Wrap, Height = 78, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    Background = TryFindResourceString("ThemeSurface"),
                    Foreground = TryFindResourceString("ThemeInk"),
                    BorderBrush = TryFindResourceString("ThemeBorder"),
                    Padding = new Thickness(6, 4, 6, 4)
                };
                panel.Children.Add(input);
                var includeGame = new CheckBox { Content = L("附带游戏名（朋友导入时会显示这段画面来自哪个游戏）", "Include game title (shown to whoever imports it)"), IsChecked = true, Margin = new Thickness(0, 10, 0, 0) };
                panel.Children.Add(includeGame);
                var sourceRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
                var pickIni = new Button { Content = L("从 ASI INI 读取…", "Read ASI INI…"), MinWidth = 110, Margin = new Thickness(0, 0, 8, 0), Padding = new Thickness(10, 3, 10, 3) };
                var sourceHint = new TextBlock { Foreground = TryFindResourceString("ThemeMuted"), FontSize = 11, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 330 };
                sourceHint.Text = L("默认：当前游戏的 dlssnr-amd.ini（ASI 配置）", "Default: the current game's dlssnr-amd.ini (ASI config)");
                sourceRow.Children.Add(pickIni); sourceRow.Children.Add(sourceHint);
                panel.Children.Add(sourceRow);
                var noteHint = new TextBlock { Text = L("备注（可选，随分享码一起发过去）：", "Note (optional, travels with the code):"), Foreground = TryFindResourceString("ThemeMuted"), FontSize = 11, Margin = new Thickness(0, 8, 0, 2) };
                var noteBox = new TextBox { Height = 26, Padding = new Thickness(6, 3, 6, 3), Background = TryFindResourceString("ThemeSurface"), Foreground = TryFindResourceString("ThemeInk"), BorderBrush = TryFindResourceString("ThemeBorder") };
                var notePlaceholder = L("给这段画面起个名字，比如：夜之城 冷蓝电影感", "Name this look, e.g. cold cinematic night");
                noteBox.Text = notePlaceholder; noteBox.Foreground = TryFindResourceString("ThemeMuted");
                bool noteDirty = false;
                noteBox.GotFocus += delegate { if (!noteDirty) { noteBox.Text = ""; noteBox.Foreground = TryFindResourceString("ThemeInk"); } };
                noteBox.LostFocus += delegate { if (!noteDirty && noteBox.Text.Length == 0) { noteBox.Text = notePlaceholder; noteBox.Foreground = TryFindResourceString("ThemeMuted"); } };
                panel.Children.Add(noteHint); panel.Children.Add(noteBox);
                var summary = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 10) };
                panel.Children.Add(summary);
                var diffHeader = new TextBlock { FontSize = 11, FontWeight = FontWeights.SemiBold };
                var diffHost = new StackPanel { Margin = new Thickness(0, 4, 0, 0) };
                var diffScroll = new ScrollViewer {
                    Content = diffHost, MaxHeight = 150,
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled
                };
                var diffBorder = new Border {
                    Background = TryFindResourceString("ThemeSurface"), BorderBrush = TryFindResourceString("ThemeBorder"),
                    BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8),
                    Padding = new Thickness(10, 7, 10, 7), Margin = new Thickness(0, 0, 0, 7),
                    Visibility = Visibility.Collapsed
                };
                var diffContent = new StackPanel();
                diffContent.Children.Add(diffHeader); diffContent.Children.Add(diffScroll);
                diffBorder.Child = diffContent;
                var allHost = new StackPanel();
                var allScroll = new ScrollViewer {
                    Content = allHost, MaxHeight = 180, Visibility = Visibility.Collapsed,
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                    Margin = new Thickness(0, 2, 0, 7)
                };
                var allToggle = new Button {
                    Content = L("全部参数 ▸", "All settings ▸"), Style = dialog.FindResource("GhostButton") as Style,
                    HorizontalAlignment = HorizontalAlignment.Left, Padding = new Thickness(9, 3, 9, 3),
                    Margin = new Thickness(0, 2, 0, 0)
                };
                allToggle.Click += delegate {
                    bool open = allScroll.Visibility != Visibility.Visible;
                    allScroll.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
                    bool zh = L("zh", "en") == "zh";
                    int count = allToggle.Tag is int ? (int)allToggle.Tag : 0;
                    allToggle.Content = count > 0
                        ? (zh ? "全部参数（" : "All settings (") + count + (zh ? "）" : ")") + (open ? " ▾" : " ▸")
                        : (zh ? "全部参数" : "All settings") + (open ? " ▾" : " ▸");
                };
                panel.Children.Add(diffBorder);
                panel.Children.Add(allToggle);
                panel.Children.Add(allScroll);
                byte[] exportedIni = null;
                string exportSourcePath = path;
                bool isExport = !import;
                Action renderMode = null;
                RoutedEventHandler switchMode = delegate {
                    var nowExport = exportRadio.IsChecked == true;
                    if (nowExport == isExport) return;
                    isExport = nowExport;
                    if (renderMode != null) renderMode();
                };
                exportRadio.Checked += switchMode; importRadio.Checked += switchMode;
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
                var close = new Button { Content = L("关闭", "Close"), MinWidth = 75, Margin = new Thickness(6) };
                close.Click += delegate { dialog.Close(); };
                var action = new Button { MinWidth = 100, Margin = new Thickness(6) };
                var apply = new Button { MinWidth = 110, Margin = new Thickness(6), IsEnabled = false, Visibility = Visibility.Collapsed };
                string validated = null, validatedGame = "";
                renderMode = delegate {
                    action.Content = isExport ? L("复制分享码", "Copy code") : L("解析", "Parse");
                    action.Style = (isExport ? dialog.FindResource("DialogPrimary") : dialog.FindResource("GhostButton")) as Style;
                    apply.Content = L("立即换上这套画面", "Apply this look");
                    apply.Style = dialog.FindResource("DialogPrimary") as Style;
                    apply.Visibility = isExport ? Visibility.Collapsed : Visibility.Visible;
                    apply.IsEnabled = false; validated = null; validatedGame = "";
                    includeGame.Visibility = isExport ? Visibility.Visible : Visibility.Collapsed;
                    sourceRow.Visibility = noteHint.Visibility = noteBox.Visibility =
                        isExport ? Visibility.Visible : Visibility.Collapsed;
                    hint.Text = isExport
                        ? L("你调好的画面风格会变成下面这段分享码（只含画面参数，不动其他设置）。复制后发给朋友或发到群里，TA 粘贴解析后就能用上一模一样的画面。",
                           "Your tuned look becomes the share code below (visual parameters only, nothing else changes). Send it to friends or chat; they paste, parse and wear your exact look.")
                        : L("粘贴别人分享的码（直接粘贴整段分享文本也行，会自动找出里面的码），解析预览后套用。",
                           "Paste a shared code (pasting the whole share text works too, the code inside is found automatically), parse, then apply.");
                    input.IsReadOnly = isExport;
                    summary.Text = "";
                    RenderShareDetails(allHost, allToggle, allScroll, diffHost, diffHeader, diffBorder, null, !isExport, path, false);
                    if (isExport) {
                        if (exportedIni == null) exportedIni = File.ReadAllBytes(exportSourcePath);
                        input.Text = MochizukiShareCode.Encode(exportedIni, includeGame.IsChecked == true ? executable : null,
                                                               noteDirty ? MochizukiShareCode.SanitizeNote(noteBox.Text) : null);
                        var decoded = MochizukiShareCode.Decode(input.Text);
                        summary.Text = MochizukiShareCode.Summary(decoded, L("zh", "en") == "zh");
                        RenderShareDetails(allHost, allToggle, allScroll, diffHost, diffHeader, diffBorder, decoded, false, path, L("zh", "en") == "zh");
                        if (MochizukiShareCode.NonDefaultCount(decoded) == 0)
                            summary.Text += L("\n当前全部为默认值：分享码有效，导入方应用后将恢复同样的默认画面。",
                                               "\nEverything is at defaults: the code is valid; importing it restores the same defaults.");
                    } else {
                        input.Text = "";
                    }
                };
                input.TextChanged += delegate {
                    if (!isExport) {
                        validated = null; validatedGame = ""; apply.IsEnabled = false; summary.Text = "";
                        var code = ExtractCode(input.Text);
                        if (code == null) return;
                        try {
                            var values = MochizukiShareCode.Decode(code);
                            string noteLine = values.Note.Length > 0 ? L("（备注：" + values.Note + "）\n", " (note: " + values.Note + ")\n") : "";
                            summary.Text = noteLine + MochizukiShareCode.Summary(values, L("zh", "en") == "zh") +
                                MissingModelWarning(game.Directory, values);
                            RenderShareDetails(allHost, allToggle, allScroll, diffHost, diffHeader, diffBorder, values, true, path, L("zh", "en") == "zh");
                            validated = code; validatedGame = values.GameTag; apply.IsEnabled = true;
                        } catch (Exception ex) { summary.Text = ShareError(ex); RenderShareDetails(allHost, allToggle, allScroll, diffHost, diffHeader, diffBorder, null, !isExport, path, false); }
                    }
                };
                includeGame.Checked += delegate { if (isExport && renderMode != null) renderMode(); };
                includeGame.Unchecked += delegate { if (isExport && renderMode != null) renderMode(); };
                noteBox.TextChanged += delegate {
                    if (!noteDirty && noteBox.Text == notePlaceholder) return;
                    noteDirty = true;
                    if (isExport && renderMode != null) renderMode();
                };
                pickIni.Click += delegate {
                    var dlg = new Microsoft.Win32.OpenFileDialog { Filter = L("INI 文件|*.ini|所有文件|*.*", "INI files|*.ini|All files|*.*"), Title = L("选择要导出的 dlssnr-amd.ini", "Choose the dlssnr-amd.ini to export") };
                    if (dlg.ShowDialog(dialog) == true)
                    {
                        try {
                            exportedIni = File.ReadAllBytes(dlg.FileName);
                            exportSourcePath = dlg.FileName;
                            sourceHint.Text = System.IO.Path.GetFileName(dlg.FileName);
                            if (isExport && renderMode != null) renderMode();
                        } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); }
                    }
                };
                action.Click += delegate {
                    try {
                        if (isExport) { Clipboard.SetText(input.Text); return; }
                        if (!CanUseIniGame(game)) return;
                        var code = ExtractCode(input.Text);
                        var values = MochizukiShareCode.Decode(code);
                        int difference = MochizukiShareCode.DifferenceCount(File.ReadAllBytes(path), code);
                        summary.Text = (values.Note.Length > 0 ? L("备注：", "Note: ") + values.Note + "\n" : "") +
                                       MochizukiShareCode.Summary(values, L("zh", "en") == "zh") + L("\n将更改 ", "\nWill change ") + difference + L(" 项画面设置（其余风格参数恢复默认）。", " settings (remaining visual parameters reset to defaults).") +
                                       MissingModelWarning(game.Directory, values);
                        RenderShareDetails(allHost, allToggle, allScroll, diffHost, diffHeader, diffBorder, values, true, path, L("zh", "en") == "zh");
                        validated = code; validatedGame = values.GameTag; apply.IsEnabled = true;
                    } catch (Exception ex) { validated = null; apply.IsEnabled = false; summary.Text = ShareError(ex); }
                };
                apply.Click += delegate {
                    if (validated == null || !CanUseIniGame(game)) return;
                    if (validatedGame.Length > 0 && !MochizukiShareCode.GameTagMatches(validatedGame, executable) &&
                        DialogTheme.Show(dialog,
                            L("这段画面来自「" + validatedGame + "」，和当前游戏不是同一个，还要换上吗？",
                              "This look was made for '" + validatedGame + "', not this game. Apply anyway?"),
                            L("游戏不一样", "Different game"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
                    if (ApplyShareCode(game, validated)) dialog.Close();
                };
                buttons.Children.Add(close); buttons.Children.Add(action); buttons.Children.Add(apply);
                DialogTheme.Primary(dialog, action); DialogTheme.SetContent(dialog, panel, buttons);

                exportRadio.IsChecked = !import; importRadio.IsChecked = import;
                renderMode();

                // Import mode: prefill only when the clipboard already holds a recognizable code —
                // never dump unrelated clipboard text into the box. Runs AFTER renderMode(),
                // whose import branch clears the input (and would wipe the prefill).
                if (import) {
                    try {
                        if (Clipboard.ContainsText()) {
                            var code = ExtractCode(Clipboard.GetText());
                            if (code != null) input.Text = code;
                        }
                    } catch { }
                }

                dialog.Loaded += delegate { if (!isExport) input.Focus(); };
                dialog.ShowDialog();
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); }
        }

        // Theme brush lookup for controls built in code (avoids the white system-default chrome)
        System.Windows.Media.Brush TryFindResourceString(string key)
        {
            var v = _window.TryFindResource(key);
            return v as System.Windows.Media.Brush ?? System.Windows.Media.Brushes.Transparent;
        }

        bool ApplyShareCode(IniGameIdentity game, string code)
        {
            if (!CanUseIniGame(game)) return false;
            bool locked = false, busy = false; string temporary = null;
            try {
                string path = Path.Combine(game.Directory, "dlssnr-amd.ini");
                if (!File.Exists(path)) { SetIniStatus("noini", ""); return false; }
                locked = TryLockTarget(game.Directory);
                if (!locked) { SetIniStatus("locked", ""); return false; }
                SetBusy(true, "share-code"); busy = true; RefreshProfileActions();
                byte[] bytes = MochizukiShareCode.Apply(File.ReadAllBytes(path), code);
                temporary = path + ".stage-" + Guid.NewGuid().ToString("N");
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
                string backup = path + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8);
                File.Replace(temporary, path, backup);
                SetIniStatus("share-imported", Path.GetFileName(backup)); return true;
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); return false; }
            finally {
                if (temporary != null && File.Exists(temporary)) { try { File.Delete(temporary); } catch { } }
                if (locked) ReleaseLock();
                if (busy) { SetBusy(false, ""); UpdatePrimaryButtonState(); }
                RefreshProfileActions();
            }
        }
    }
}
