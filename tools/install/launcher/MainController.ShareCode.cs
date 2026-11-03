using System;
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

        // MZ code inside arbitrary pasted text (share text with a pitch line, chat quotes, anything)
        static readonly Regex CodeInText = new Regex(@"MZ[1-4]:[^;\r\n]*;[0-9A-Fa-f]{4}", RegexOptions.Compiled);
        static string ExtractCode(string text)
        {
            if (text == null) return null;
            var match = CodeInText.Match(text);
            return match.Success ? match.Value : null;
        }

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
                var modeRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 10) };
                var exportRadio = new RadioButton { Content = L("导出（发出我的画面）", "Export (share my look)"), GroupName = "ShareCodeMode", Margin = new Thickness(0, 0, 14, 0) };
                var importRadio = new RadioButton { Content = L("导入（换上别人的画面）", "Import (wear someone's look)"), GroupName = "ShareCodeMode" };
                modeRow.Children.Add(exportRadio); modeRow.Children.Add(importRadio);
                panel.Children.Add(modeRow);
                var hint = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) };
                panel.Children.Add(hint);
                var input = new TextBox
                {
                    TextWrapping = TextWrapping.Wrap, Height = 130, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
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
                    apply.Content = L("立即换上这套画面", "Apply this look");
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
                    if (isExport) {
                        if (exportedIni == null) exportedIni = File.ReadAllBytes(exportSourcePath);
                        input.Text = MochizukiShareCode.Encode(exportedIni, includeGame.IsChecked == true ? executable : null,
                                                               noteDirty ? MochizukiShareCode.SanitizeNote(noteBox.Text) : null);
                        var decoded = MochizukiShareCode.Decode(input.Text);
                        summary.Text = MochizukiShareCode.Summary(decoded, L("zh", "en") == "zh");
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
                            string tag = values.GameTag.Length > 0 ? "【" + values.GameTag + "】" : "";
                            string noteLine = values.Note.Length > 0 ? L("（备注：" + values.Note + "）", " (note: " + values.Note + ")") : "";
                            summary.Text = noteLine + MochizukiShareCode.Summary(values, L("zh", "en") == "zh");
                            validated = code; validatedGame = values.GameTag; apply.IsEnabled = true;
                        } catch (Exception ex) { summary.Text = ShareError(ex); }
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
                                       MochizukiShareCode.Summary(values, L("zh", "en") == "zh") + L("\n将更改 ", "\nWill change ") + difference + L(" 项画面设置（其余风格参数恢复默认）。", " settings (remaining visual parameters reset to defaults).");
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
                panel.Children.Add(buttons);
                DialogTheme.Primary(dialog, action); DialogTheme.SetContent(dialog, panel);

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
