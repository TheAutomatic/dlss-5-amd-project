using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;

namespace OptScalerNR.Launcher
{
    public static class MochizukiHotkeysIni
    {
        public static readonly string[] TemplateFileNames =
        {
            "dlssnr-amd-optiscaler.ini", "dlssnr-amd-optiscaler-renodx.ini",
            "dlssnr-amd-standalone.ini", "dlssnr-amd-addon.ini"
        };

        static readonly char[] CommentChars = { ';', '#' };
        static readonly int[] SpecialVks = { 0x2D, 0x2E, 0x24, 0x23, 0x21, 0x22, 0x13, 0x91 };
        static readonly string[] SpecialNames = { "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Pause", "ScrollLock" };

        public static string VkToMochizukiWord(int vk)
        {
            if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x70 + 1);
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return ((char)vk).ToString();
            int idx = Array.IndexOf(SpecialVks, vk);
            return idx >= 0 ? SpecialNames[idx] : "";
        }

        public static string WpfKeyToPrimaryToken(Key key)
        {
            var word = VkToMochizukiWord(KeyInterop.VirtualKeyFromKey(key));
            return word.Length > 0 ? word : null;
        }

        static string NormalizePrimaryToken(string low)
        {
            int n;
            if (low.Length == 1) {
                char c = char.ToUpperInvariant(low[0]);
                return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c.ToString() : null;
            }
            if (low.Length >= 2 && low[0] == 'f' &&
                int.TryParse(low.Substring(1), NumberStyles.None, CultureInfo.InvariantCulture, out n) && n >= 1 && n <= 24)
                return "F" + n;
            for (int i = 0; i < SpecialNames.Length; i++)
                if (string.Equals(SpecialNames[i], low, StringComparison.OrdinalIgnoreCase)) return SpecialNames[i];
            return null;
        }

        public static string CanonicalizeHotkey(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return "";
            var trimmed = raw.Trim();
            if (trimmed.Length == 0) return "";
            bool ctrl = false, shift = false, alt = false;
            string keyToken = null;
            var parts = trimmed.Split('+');
            for (int i = 0; i < parts.Length; i++) {
                var low = parts[i].Trim().ToLowerInvariant();
                if (low.Length == 0) return null;
                if (low == "ctrl" || low == "control") ctrl = true;
                else if (low == "shift") shift = true;
                else if (low == "alt") alt = true;
                else if (keyToken != null || (keyToken = NormalizePrimaryToken(low)) == null) return null;
            }
            return keyToken == null ? null : ((ctrl ? "Ctrl+" : "") + (shift ? "Shift+" : "") + (alt ? "Alt+" : "") + keyToken);
        }

        public static bool IsModifierKey(Key key)
        {
            return key == Key.LeftCtrl || key == Key.RightCtrl || key == Key.LeftShift || key == Key.RightShift ||
                   key == Key.LeftAlt || key == Key.RightAlt || key == Key.LWin || key == Key.RWin;
        }

        public static string FormatHeldModifiers(ModifierKeys mods)
        {
            return (((mods & ModifierKeys.Control) != 0) ? "Ctrl+" : "") +
                   (((mods & ModifierKeys.Shift) != 0) ? "Shift+" : "") +
                   (((mods & ModifierKeys.Alt) != 0) ? "Alt+" : "");
        }

        public static bool TryFormatWpfKey(KeyEventArgs e, out string formatted, out bool isModifierOnly, out bool isUnsupported)
        {
            formatted = null; isModifierOnly = false; isUnsupported = false;
            Key raw = (e.Key == Key.System) ? e.SystemKey : e.Key;
            if (raw == Key.DeadCharProcessed) raw = e.DeadCharProcessedKey;
            var mods = Keyboard.Modifiers;
            if ((mods & ModifierKeys.Windows) != 0 || raw == Key.LWin || raw == Key.RWin) { isUnsupported = true; return false; }
            if (IsModifierKey(raw)) { isModifierOnly = true; return false; }
            string token = WpfKeyToPrimaryToken(raw);
            if (token == null) { isUnsupported = true; return false; }
            formatted = FormatHeldModifiers(mods) + token;
            return true;
        }

        static string StripComment(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            int i = s.IndexOfAny(CommentChars);
            return i >= 0 ? s.Substring(0, i) : s;
        }

        static string ParseSection(string t)
        {
            return t.Length >= 2 && t[0] == '[' && t[t.Length - 1] == ']' ? t.Substring(1, t.Length - 2).Trim() : null;
        }

        public static bool HasIniSection(string text, string section)
        {
            return !string.IsNullOrEmpty(section) && ReadIniKey(text, section, null) != null;
        }

        public static string ReadIniKey(string text, string section, string key)
        {
            if (string.IsNullOrEmpty(text)) return null;
            string curSec = "";
            using (var sr = new StringReader(text)) {
                string line;
                while ((line = sr.ReadLine()) != null) {
                    var t = StripComment(line).Trim().TrimStart('\uFEFF').Trim();
                    if (t.Length == 0) continue;
                    var sec = ParseSection(t);
                    if (sec != null) {
                        curSec = sec;
                        if (key == null && string.Equals(curSec, section, StringComparison.OrdinalIgnoreCase)) return "";
                        continue;
                    }
                    if (key == null || !string.Equals(curSec, section, StringComparison.OrdinalIgnoreCase)) continue;
                    int eq = t.IndexOf('=');
                    if (eq > 0 && string.Equals(t.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase))
                        return t.Substring(eq + 1).Trim();
                }
            }
            return null;
        }

        public static string ReadOptiScalerMenuKey(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir)) return "";
            var iniPath = Path.Combine(gameDir, "OptiScaler.ini");
            if (!File.Exists(iniPath)) return "Insert";
            try {
                var raw = ReadIniKey(File.ReadAllText(iniPath, Encoding.UTF8), "Menu", "ShortcutKey");
                if (string.IsNullOrEmpty(raw)) return "Insert";
                var t = raw.Trim();
                if (string.Equals(t, "auto", StringComparison.OrdinalIgnoreCase)) return "Insert";
                if (string.Equals(t, "false", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(t, "none", StringComparison.OrdinalIgnoreCase) || t == "0" || t == "-1") return "";
                int vk;
                if (t.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
                    int.TryParse(t.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out vk))
                    return VkToMochizukiWord(vk);
                if (int.TryParse(t, out vk)) return VkToMochizukiWord(vk);
                return CanonicalizeHotkey(t) ?? "Insert";
            } catch { return "Insert"; }
        }

        public static string UpdateIniText(string original, IDictionary<string, string> updates, bool onlyExistingSections, bool skipNrControlWhenMissing)
        {
            string nl = (original != null && original.IndexOf("\r\n", StringComparison.Ordinal) >= 0) ? "\r\n" : "\n";
            bool trailingNl = string.IsNullOrEmpty(original) || original.EndsWith("\n", StringComparison.Ordinal);
            var lines = new List<string>();
            if (!string.IsNullOrEmpty(original)) {
                var split = original.Replace("\r\n", "\n").Split('\n');
                int count = (split.Length > 0 && split[split.Length - 1].Length == 0 && trailingNl) ? split.Length - 1 : split.Length;
                for (int i = 0; i < count; i++) lines.Add(i == 0 ? split[i].TrimStart('\uFEFF') : split[i]);
            }

            var order = new List<string>();
            var formatted = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in updates) {
                int pipe = kv.Key.IndexOf('|');
                if (pipe <= 0) continue;
                var key = kv.Key.Substring(pipe + 1);
                order.Add(kv.Key);
                formatted[kv.Key] = string.IsNullOrEmpty(kv.Value) ? (key + " =") : (key + " = " + kv.Value);
            }

            var handled = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var secEnd = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            string curSec = "";
            bool trackSec = false;
            for (int i = 0; i < lines.Count; i++) {
                var noComment = StripComment(lines[i]).Trim();
                var sec = ParseSection(noComment);
                if (sec != null) {
                    curSec = sec;
                    trackSec = !secEnd.ContainsKey(curSec);
                    if (trackSec) secEnd[curSec] = i;
                    continue;
                }
                if (curSec.Length == 0) continue;
                if (trackSec && lines[i].Trim().Length > 0) secEnd[curSec] = i;
                int eq = noComment.IndexOf('=');
                if (eq <= 0) continue;
                var mapKey = curSec + "|" + noComment.Substring(0, eq).Trim();
                string repl;
                if (formatted.TryGetValue(mapKey, out repl)) { lines[i] = repl; handled.Add(mapKey); }
            }

            var insertAt = new Dictionary<int, List<string>>();
            var missingSec = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            var missingOrder = new List<string>();
            for (int i = 0; i < order.Count; i++) {
                var mapKey = order[i];
                if (handled.Contains(mapKey)) continue;
                var sec = mapKey.Substring(0, mapKey.IndexOf('|'));
                int endIdx;
                if (secEnd.TryGetValue(sec, out endIdx)) {
                    List<string> bucket;
                    if (!insertAt.TryGetValue(endIdx + 1, out bucket)) insertAt[endIdx + 1] = bucket = new List<string>();
                    bucket.Add(formatted[mapKey]);
                } else if (!onlyExistingSections && !(skipNrControlWhenMissing && string.Equals(sec, "NRControl", StringComparison.OrdinalIgnoreCase))) {
                    List<string> bucket;
                    if (!missingSec.TryGetValue(sec, out bucket)) { missingSec[sec] = bucket = new List<string>(); missingOrder.Add(sec); }
                    bucket.Add(formatted[mapKey]);
                }
            }

            var positions = new List<int>(insertAt.Keys);
            positions.Sort();
            for (int i = positions.Count - 1; i >= 0; i--) lines.InsertRange(positions[i], insertAt[positions[i]]);
            for (int i = 0; i < missingOrder.Count; i++) {
                if (lines.Count > 0 && lines[lines.Count - 1].Trim().Length > 0) lines.Add("");
                lines.Add("[" + missingOrder[i] + "]");
                lines.AddRange(missingSec[missingOrder[i]]);
            }

            var joined = string.Join(nl, lines.ToArray());
            return (trailingNl && joined.Length > 0) ? (joined + nl) : joined;
        }

        public static void UpdateIniFile(string path, IDictionary<string, string> updates, bool onlyExistingSections, bool skipNrControlWhenMissing)
        {
            var utf8 = new UTF8Encoding(false);
            File.WriteAllText(path, UpdateIniText(File.ReadAllText(path, utf8), updates, onlyExistingSections, skipNrControlWhenMissing), utf8);
        }
    }

    public sealed partial class MainController
    {
        sealed class HotkeySpec
        {
            public readonly string Section, Key, DefaultValue, TitleZh, TitleEn, DescZh, DescEn;
            public string MapKey { get { return Section + "|" + Key; } }
            public HotkeySpec(string sec, string key, string def, string tZh, string tEn, string dZh, string dEn)
            { Section = sec; Key = key; DefaultValue = def; TitleZh = tZh; TitleEn = tEn; DescZh = dZh; DescEn = dEn; }
        }

        static readonly HotkeySpec[] MochiHotkeySpecs =
        {
            new HotkeySpec("NRControl", "MenuKey", "End", "打开 / 关闭 NR Control 菜单", "Open / Close NR Control",
                "[NRControl] MenuKey · 默认 End（ReShade 模式不适用）", "[NRControl] MenuKey · default End (not used in ReShade mode)"),
            new HotkeySpec("RenoDX", "NRToggleKey", "F6", "开关神经渲染（本局生效）", "Toggle Neural Rendering",
                "[RenoDX] NRToggleKey · 默认 F6（切换开 / 关对比）", "[RenoDX] NRToggleKey · default F6 (session toggle)"),
            new HotkeySpec("RenoDX", "NRScreenshotKey", "F5", "对比截图（关 / 开 NR 各一张）", "Before / After Screenshot",
                "[RenoDX] NRScreenshotKey · 默认 F5（保存至 NR Screenshots）", "[RenoDX] NRScreenshotKey · default F5 (saves to NR Screenshots)"),
            new HotkeySpec("Network", "Hotkey", "Ctrl+F11", "切换网络模型", "Cycle Network Model",
                "[Network] Hotkey · 默认 Ctrl+F11（轮切可用模型）", "[Network] Hotkey · default Ctrl+F11 (cycles models)"),
            new HotkeySpec("Preprocess", "Hotkey", "Ctrl+F10", "开关预处理（本局生效）", "Toggle Preprocess",
                "[Preprocess] Hotkey · 默认 Ctrl+F10（色彩 / 曝光前处理）", "[Preprocess] Hotkey · default Ctrl+F10 (color / exposure)")
        };

        Button _mochiHotkeysButton;
        StackPanel _mochiHotkeyHint, _mochiHotkeyHintCaps;
        TextBlock _mochiHotkeyHintPrefix, _mochiHotkeyHintSuffix;
        StackPanel _openRrHotkeyHint, _openRrHotkeyHintCaps;
        TextBlock _openRrHotkeyHintPrefix, _openRrHotkeyHintSuffix;

        void InitMochiHotkeys()
        {
            _mochiHotkeysButton = _window.FindName("MochiHotkeysButton") as Button;
            if (_mochiHotkeysButton != null) _mochiHotkeysButton.Click += delegate { ShowMochiHotkeysDialog(); };
            _mochiHotkeyHint = _window.FindName("MochiHotkeyHint") as StackPanel;
            _mochiHotkeyHintPrefix = _window.FindName("MochiHotkeyHintPrefix") as TextBlock;
            _mochiHotkeyHintCaps = _window.FindName("MochiHotkeyHintCaps") as StackPanel;
            _mochiHotkeyHintSuffix = _window.FindName("MochiHotkeyHintSuffix") as TextBlock;
            _openRrHotkeyHint = _window.FindName("OpenRrHotkeyHint") as StackPanel;
            _openRrHotkeyHintPrefix = _window.FindName("OpenRrHotkeyHintPrefix") as TextBlock;
            _openRrHotkeyHintCaps = _window.FindName("OpenRrHotkeyHintCaps") as StackPanel;
            _openRrHotkeyHintSuffix = _window.FindName("OpenRrHotkeyHintSuffix") as TextBlock;
        }

        void RefreshMochiHotkeys(bool enabled)
        {
            if (_mochiHotkeysButton != null) {
                _mochiHotkeysButton.Content = KeyboardLabel(L("快捷键", "Hotkeys"));
                _mochiHotkeysButton.ToolTip = L("在启动器中修改 Mochizuki 当前游戏或默认模板的快捷键", "Edit Mochizuki hotkeys for the current game or launcher templates");
                _mochiHotkeysButton.IsEnabled = enabled;
            }
        }

        string[] ResolveActiveMochiHotkeys()
        {
            var exePath = _selectedGame != null ? _selectedGame.Exe : null;
            var gameDir = !string.IsNullOrEmpty(exePath) ? Path.GetDirectoryName(exePath) : null;
            var gameIniPath = !string.IsNullOrEmpty(gameDir) ? DlssnrLocalData.ResolveActiveDlssnrIni(gameDir, exePath) : null;
            return LoadHotkeysFromFile(gameIniPath, FindActiveTemplateIni());
        }

        string ResolveActiveOptiMenuKey()
        {
            var exePath = _selectedGame != null ? _selectedGame.Exe : null;
            var gameDir = !string.IsNullOrEmpty(exePath) ? Path.GetDirectoryName(exePath) : null;
            return !string.IsNullOrEmpty(gameDir) ? MochizukiHotkeysIni.ReadOptiScalerMenuKey(gameDir) : "Insert";
        }

        static readonly FontFamily KeycapFont = new FontFamily("Cascadia Mono, Consolas, Segoe UI");

        static void BuildInlineKeycaps(Panel host, string hotkey)
        {
            if (host == null) return;
            host.Children.Clear();
            if (string.IsNullOrEmpty(hotkey)) return;
            var tokens = hotkey.Split('+');
            for (int i = 0; i < tokens.Length; i++) {
                if (i > 0) {
                    var plus = HotkeyText("+", 10, "ThemeMuted", false, false);
                    plus.Margin = new Thickness(2, 0, 2, 0);
                    plus.VerticalAlignment = VerticalAlignment.Center;
                    host.Children.Add(plus);
                }
                var tb = HotkeyText(tokens[i], 10.5, "ThemeInk", true, false);
                tb.FontFamily = KeycapFont;
                tb.VerticalAlignment = VerticalAlignment.Center;
                var cap = ThemedBorder(4, "ThemeSubtle", "ThemeBorder", tb);
                cap.BorderThickness = new Thickness(1, 1, 1, 1.5);
                cap.Padding = new Thickness(5, 0.5, 5, 1);
                cap.VerticalAlignment = VerticalAlignment.Center;
                cap.Margin = new Thickness(i == 0 ? 4 : 0, 0, i == tokens.Length - 1 ? 4 : 0, 0);
                host.Children.Add(cap);
            }
        }

        void RefreshMochiHotkeyHint()
        {
            if (_mochiHotkeyHint == null) return;
            if (!IsBackendSelected("mochizuki")) {
                _mochiHotkeyHint.Visibility = Visibility.Collapsed;
                return;
            }
            var mochiValues = ResolveActiveMochiHotkeys();
            if (GetSelectedBaseMode() == "reshade") {
                var toggleKey = mochiValues.Length > 1 ? mochiValues[1] : "";
                if (string.IsNullOrEmpty(toggleKey)) {
                    _mochiHotkeyHint.Visibility = Visibility.Collapsed;
                } else {
                    _mochiHotkeyHintPrefix.Text = L("游戏内按", "Press");
                    BuildInlineKeycaps(_mochiHotkeyHintCaps, toggleKey);
                    _mochiHotkeyHintSuffix.Text = L("开关 NR", "to toggle NR");
                    _mochiHotkeyHint.ToolTip = L("ReShade 模式下按 Home 打开 ReShade 菜单，可在右上角「快捷键」中修改热键", "In ReShade mode, press Home for ReShade; edit NR hotkeys via Hotkeys.");
                    _mochiHotkeyHint.Visibility = Visibility.Visible;
                }
            } else {
                var menuKey = mochiValues.Length > 0 ? mochiValues[0] : "";
                if (string.IsNullOrEmpty(menuKey)) {
                    _mochiHotkeyHintPrefix.Text = L("菜单键未绑定", "Menu key unbound");
                    BuildInlineKeycaps(_mochiHotkeyHintCaps, "");
                    _mochiHotkeyHintSuffix.Text = "";
                    _mochiHotkeyHint.ToolTip = L("可在右上角「快捷键」中重新绑定 NR Control 菜单键", "Rebind the NR Control menu key via Hotkeys.");
                    _mochiHotkeyHint.Visibility = Visibility.Visible;
                } else {
                    _mochiHotkeyHintPrefix.Text = L("游戏内按", "Press");
                    BuildInlineKeycaps(_mochiHotkeyHintCaps, menuKey);
                    _mochiHotkeyHintSuffix.Text = L("打开面板", "for menu");
                    _mochiHotkeyHint.ToolTip = L("游戏内按此键打开 / 关闭 Mochizuki NR Control 面板（可在右上角「快捷键」修改）", "Press in game to open/close Mochizuki NR Control (edit via Hotkeys).");
                    _mochiHotkeyHint.Visibility = Visibility.Visible;
                }
            }
        }

        void RefreshOpenRrHotkeyHint(bool openRrReady)
        {
            if (_openRrHotkeyHint == null) return;
            var optiKey = openRrReady ? ResolveActiveOptiMenuKey() : "";
            if (string.IsNullOrEmpty(optiKey)) {
                _openRrHotkeyHint.Visibility = Visibility.Collapsed;
            } else {
                _openRrHotkeyHintPrefix.Text = L("随", "Opens with");
                BuildInlineKeycaps(_openRrHotkeyHintCaps, optiKey);
                _openRrHotkeyHintSuffix.Text = L("打开", "menu");
                _openRrHotkeyHint.ToolTip = L("OpenRR 面板会停靠在 OptiScaler 菜单内，随此键同步打开", "OpenRR docks inside the OptiScaler menu and opens with this key.");
                _openRrHotkeyHint.Visibility = Visibility.Visible;
            }
        }

        void SetInstallCompleteStatus(bool updated)
        {
            var baseMode = GetSelectedBaseMode();
            bool isOpti = IsOptiScalerSelectedMode();
            bool isReShade = baseMode == "reshade";
            bool mochiOn = IsBackendSelected("mochizuki") || !isOpti;
            bool openRrOn = isOpti && !IsOpenRrUnsupportedLocked() && _openRrToggle != null && _openRrToggle.IsChecked == true;
            var mochiKeys = mochiOn ? ResolveActiveMochiHotkeys() : null;
            var menuKey = mochiOn ? mochiKeys[0] : "";
            var toggleKey = mochiOn ? mochiKeys[1] : "";
            var optiKey = isOpti ? ResolveActiveOptiMenuKey() : "";
            var zhParts = new List<string>();
            var enParts = new List<string>();
            if (isOpti && !string.IsNullOrEmpty(optiKey)) {
                zhParts.Add(openRrOn ? ("按 " + optiKey + " 打开 OptiScaler / OpenRR") : ("按 " + optiKey + " 打开 OptiScaler"));
                enParts.Add(openRrOn ? ("press " + optiKey + " for OptiScaler / OpenRR") : ("press " + optiKey + " for OptiScaler"));
            }
            if (isReShade) {
                if (!string.IsNullOrEmpty(toggleKey)) {
                    zhParts.Add("按 " + toggleKey + " 开关 NR（Home 打开 ReShade）");
                    enParts.Add("press " + toggleKey + " to toggle NR (Home for ReShade)");
                } else {
                    zhParts.Add("按 Home 打开 ReShade");
                    enParts.Add("press Home for ReShade");
                }
            } else if (mochiOn && !string.IsNullOrEmpty(menuKey)) {
                zhParts.Add("按 " + menuKey + " 打开 Mochizuki 面板");
                enParts.Add("press " + menuKey + " for Mochizuki");
            }
            string zh = updated ? "更新完成" : "安装完成";
            string en = updated ? "Update complete" : "Installation complete";
            if (zhParts.Count > 0) zh += " · 游戏内" + string.Join("，", zhParts.ToArray());
            if (enParts.Count > 0) en += " · In game: " + string.Join(", ", enParts.ToArray());
            SetStatus(zh, en);
        }

        static object KeyboardLabel(string text)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            var icon = new System.Windows.Shapes.Path
            {
                Data = Geometry.Parse("M 1,2 L 13,2 L 13,10 L 1,10 Z M 3,4.3 L 4.3,4.3 M 5.8,4.3 L 7.1,4.3 M 8.6,4.3 L 9.9,4.3 M 11,4.3 L 11.4,4.3 M 3,6.4 L 4.3,6.4 M 5.8,6.4 L 7.1,6.4 M 8.6,6.4 L 11.4,6.4 M 4.2,8.4 L 9.8,8.4"),
                StrokeThickness = 1.1, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
                Width = 13, Height = 10, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 4, 0)
            };
            icon.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "ThemeInk");
            panel.Children.Add(icon);
            panel.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
            return panel;
        }

        string FindActiveTemplateIni()
        {
            var root = ResolveImportRoot();
            var mode = GetSelectedBaseMode();
            string[] names;
            if (mode == "reshade") names = new[] { "dlssnr-amd-addon.ini" };
            else if (mode == "standalone") names = new[] { "dlssnr-amd-standalone.ini" };
            else if (GetSelectedRenoDx() == "on") names = new[] { "dlssnr-amd-optiscaler-renodx.ini", "dlssnr-amd-optiscaler.ini" };
            else names = new[] { "dlssnr-amd-optiscaler.ini" };
            foreach (var name in names) {
                var path = Path.Combine(root, "ini", name);
                if (File.Exists(path)) return path;
                path = Path.Combine(root, name);
                if (File.Exists(path)) return path;
            }
            return null;
        }

        string[] FindLauncherTemplateInis()
        {
            var roots = new[] { ResolveImportRoot(), _packageRoot };
            var found = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int r = 0; r < roots.Length; r++) {
                if (string.IsNullOrEmpty(roots[r]) || !Directory.Exists(roots[r])) continue;
                var dirs = new[] { roots[r], Path.Combine(roots[r], "ini") };
                for (int d = 0; d < dirs.Length; d++) {
                    if (!Directory.Exists(dirs[d])) continue;
                    for (int t = 0; t < MochizukiHotkeysIni.TemplateFileNames.Length; t++) {
                        var p = Path.Combine(dirs[d], MochizukiHotkeysIni.TemplateFileNames[t]);
                        if (File.Exists(p) && seen.Add(Path.GetFullPath(p))) found.Add(Path.GetFullPath(p));
                    }
                }
            }
            return found.ToArray();
        }

        static string[] LoadHotkeysFromFile(string path, string fallbackPath)
        {
            string primary = null, fallback = null;
            try { if (!string.IsNullOrEmpty(path) && File.Exists(path)) primary = File.ReadAllText(path, Encoding.UTF8); } catch { }
            try { if (!string.IsNullOrEmpty(fallbackPath) && File.Exists(fallbackPath)) fallback = File.ReadAllText(fallbackPath, Encoding.UTF8); } catch { }
            var values = new string[MochiHotkeySpecs.Length];
            for (int i = 0; i < MochiHotkeySpecs.Length; i++) {
                var spec = MochiHotkeySpecs[i];
                var raw = (primary != null ? MochizukiHotkeysIni.ReadIniKey(primary, spec.Section, spec.Key) : null)
                    ?? (fallback != null ? MochizukiHotkeysIni.ReadIniKey(fallback, spec.Section, spec.Key) : null);
                values[i] = raw == null ? spec.DefaultValue : (MochizukiHotkeysIni.CanonicalizeHotkey(raw) ?? spec.DefaultValue);
            }
            return values;
        }

        static TextBlock HotkeyText(string text, double size, string brushKey, bool bold, bool wrap)
        {
            var tb = new TextBlock { Text = text, FontSize = size, FontWeight = bold ? FontWeights.SemiBold : FontWeights.Normal, TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap };
            tb.SetResourceReference(TextBlock.ForegroundProperty, brushKey);
            return tb;
        }

        static Border ThemedBorder(double radius, string bgKey, string borderKey, UIElement child)
        {
            var b = new Border { CornerRadius = new CornerRadius(radius), BorderThickness = new Thickness(1), SnapsToDevicePixels = true, Child = child };
            b.SetResourceReference(Border.BackgroundProperty, bgKey);
            b.SetResourceReference(Border.BorderBrushProperty, borderKey);
            return b;
        }

        static CheckBox BuildScopeCard(Window win, string title, string subtitle, bool isChecked, bool isEnabled)
        {
            var stack = new StackPanel();
            stack.Children.Add(HotkeyText(title, 12.5, "ThemeInk", true, false));
            var sub = HotkeyText(subtitle, 10.5, "ThemeMuted", false, true);
            sub.Margin = new Thickness(0, 2, 0, 0);
            stack.Children.Add(sub);
            var cb = new CheckBox { Style = win.FindResource("ModelCard") as Style, Margin = new Thickness(4, 0, 4, 0), IsChecked = isChecked, IsEnabled = isEnabled, Content = stack };
            if (!isEnabled) cb.Tag = "unavailable";
            return cb;
        }

        static ControlTemplate CreateKeycapSlotTemplate()
        {
            const string xaml =
                "<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='{x:Type Button}'>" +
                "<Grid><Border x:Name='Slot' Background='{TemplateBinding Background}' BorderBrush='{TemplateBinding BorderBrush}' BorderThickness='{TemplateBinding BorderThickness}' CornerRadius='8' Padding='{TemplateBinding Padding}' SnapsToDevicePixels='True'>" +
                "<ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/></Border>" +
                "<Border x:Name='HoverRing' CornerRadius='8' Background='#14808A9E' IsHitTestVisible='False' Opacity='0'/></Grid>" +
                "<ControlTemplate.Triggers><Trigger Property='IsMouseOver' Value='True'><Setter TargetName='HoverRing' Property='Opacity' Value='1'/></Trigger></ControlTemplate.Triggers></ControlTemplate>";
            return (ControlTemplate)XamlReader.Parse(xaml);
        }

        static UIElement BuildKeycapContent(string hotkey, string placeholder, bool conflict, bool capturing, bool hasModsPreview)
        {
            if (capturing || string.IsNullOrEmpty(hotkey)) {
                var tb = HotkeyText(placeholder, 11.5, capturing ? "ThemeAccent" : "ThemeMuted", capturing && hasModsPreview, false);
                tb.HorizontalAlignment = HorizontalAlignment.Center; tb.VerticalAlignment = VerticalAlignment.Center;
                return tb;
            }
            var tokens = hotkey.Split('+');
            var panel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            for (int i = 0; i < tokens.Length; i++) {
                if (i > 0) {
                    var plus = HotkeyText("+", 10.5, "ThemeMuted", false, false);
                    plus.Margin = new Thickness(3, 0, 3, 1); plus.VerticalAlignment = VerticalAlignment.Center;
                    panel.Children.Add(plus);
                }
                var keyText = HotkeyText(tokens[i], 11.5, conflict ? "ThemeWarning" : "ThemeInk", true, false);
                keyText.VerticalAlignment = VerticalAlignment.Center;
                var cap = ThemedBorder(5, conflict ? "ThemeWarningBg" : "ThemeSurface", conflict ? "ThemeWarning" : "ThemeBorder", keyText);
                cap.BorderThickness = new Thickness(1, 1, 1, 2.2); cap.Padding = new Thickness(7, 2, 7, 2); cap.VerticalAlignment = VerticalAlignment.Center;
                panel.Children.Add(cap);
            }
            return panel;
        }

        DockPanel BuildHotkeyRow(Style ghostStyle, ControlTemplate slotTemplate, HotkeySpec spec, out Button capBtn, out Button clrBtn)
        {
            var row = new DockPanel { Margin = new Thickness(14, 10, 14, 10) };
            capBtn = new Button { Template = slotTemplate, MinWidth = 158, Height = 32, MinHeight = 32, Padding = new Thickness(10, 2, 10, 2), BorderThickness = new Thickness(1), Cursor = Cursors.Hand, Focusable = false, ToolTip = L("点击后按下新的组合键；按 Esc 取消录入", "Click then press a hotkey; press Esc to cancel") };
            clrBtn = new Button { Content = "✕", Style = ghostStyle, Width = 28, MinWidth = 28, Height = 32, MinHeight = 32, FontSize = 10.5, Padding = new Thickness(0), Margin = new Thickness(6, 0, 0, 0), Focusable = false, ToolTip = L("清空（禁用此快捷键）", "Clear (disable this hotkey)") };
            var right = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 0, 0) };
            right.Children.Add(capBtn); right.Children.Add(clrBtn);
            DockPanel.SetDock(right, Dock.Right); row.Children.Add(right);

            var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            left.Children.Add(HotkeyText(L(spec.TitleZh, spec.TitleEn), 13, "ThemeInk", true, false));
            var sub = HotkeyText(L(spec.DescZh, spec.DescEn), 10.5, "ThemeMuted", false, false);
            sub.Margin = new Thickness(0, 2, 0, 0); sub.TextTrimming = TextTrimming.CharacterEllipsis;
            left.Children.Add(sub); row.Children.Add(left);
            return row;
        }

        bool SaveMochiHotkeys(Window win, string gameDir, string gameIniPath, string[] templatePaths, string[] currentValues, bool applyGame, bool applyTpl)
        {
            var updates = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < MochiHotkeySpecs.Length; i++) updates[MochiHotkeySpecs[i].MapKey] = currentValues[i] ?? "";
            try {
                if (applyGame) {
                    MochizukiHotkeysIni.UpdateIniFile(gameIniPath, updates, false, true);
                    var defaultCopy = Path.Combine(gameDir, @"dlssnr-amd\dlssnr-amd.default.ini");
                    if (File.Exists(defaultCopy)) MochizukiHotkeysIni.UpdateIniFile(defaultCopy, updates, false, true);
                }
                if (applyTpl)
                    for (int t = 0; t < templatePaths.Length; t++) MochizukiHotkeysIni.UpdateIniFile(templatePaths[t], updates, true, true);
                if (applyGame && applyTpl)
                    SetStatus(string.Format("已更新当前游戏 dlssnr-amd.ini 及 {0} 份启动器模板快捷键。", templatePaths.Length),
                              string.Format("Updated current game dlssnr-amd.ini and {0} launcher templates.", templatePaths.Length));
                else if (applyGame)
                    SetStatus("已更新当前游戏 dlssnr-amd.ini 快捷键。", "Updated current game dlssnr-amd.ini hotkeys.");
                else
                    SetStatus(string.Format("已更新 {0} 份启动器默认模板快捷键。", templatePaths.Length),
                              string.Format("Updated {0} launcher hotkey templates.", templatePaths.Length));
                return true;
            } catch (UnauthorizedAccessException) {
                DialogTheme.Show(win, L("没有权限写入目标配置文件（游戏或启动器目录可能位于 Program Files 等受保护位置）。请以管理员身份运行启动器后再试。", "Permission denied writing the configuration file. Run the launcher as Administrator and try again."), win.Title, MessageBoxButton.OK, MessageBoxImage.Error);
            } catch (Exception ex) {
                DialogTheme.Show(win, L("保存快捷键失败：", "Failed to save hotkeys: ") + ex.Message, win.Title, MessageBoxButton.OK, MessageBoxImage.Error);
            }
            return false;
        }

        void ShowMochiHotkeysDialog()
        {
            if (IniBusy()) return;
            var exePath = _selectedGame != null ? _selectedGame.Exe : null;
            var gameDir = !string.IsNullOrEmpty(exePath) ? Path.GetDirectoryName(exePath) : null;
            var gameIniPath = !string.IsNullOrEmpty(gameDir) ? DlssnrLocalData.ResolveActiveDlssnrIni(gameDir, exePath) : null;
            bool hasGameIni = !string.IsNullOrEmpty(gameIniPath) && File.Exists(gameIniPath);
            var templatePaths = FindLauncherTemplateInis();
            bool hasTemplates = templatePaths.Length > 0;
            string primaryTemplate = FindActiveTemplateIni();
            string optiMenuKey = hasGameIni ? MochizukiHotkeysIni.ReadOptiScalerMenuKey(gameDir) : "Insert";

            if (!hasGameIni && !hasTemplates) {
                DialogTheme.Show(_window, L("未找到当前游戏的 dlssnr-amd.ini，也未在启动器目录中找到默认模板文件。", "Neither a current game's dlssnr-amd.ini nor launcher template INI files were found."), L("Mochizuki 快捷键", "Mochizuki Hotkeys"), MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            var win = DialogTheme.Create(_window, L("Mochizuki 快捷键设置", "Mochizuki Hotkeys"), 630);
            DialogTheme.SetSuppressEscapeClose(win, false);
            var ghostStyle = win.FindResource("GhostButton") as Style;
            var currentValues = LoadHotkeysFromFile(gameIniPath, primaryTemplate);
            int capturingIndex = -1;
            string transientNote = null;

            var body = new StackPanel { Margin = new Thickness(20, 14, 20, 12) };
            var intro = HotkeyText(L("点击右侧按键框后直接按下组合键即可录入（支持 Ctrl / Shift / Alt + F1–F24 / A–Z / 0–9 / Insert / Delete / Home / End / PageUp / PageDown 等），按 Esc 取消录入。", "Click any hotkey slot and press a key combination (Ctrl / Shift / Alt + F1–F24, A–Z, 0–9, Insert, Delete, Home, End, PageUp, PageDown, etc.). Press Esc to cancel capture."), 11.5, "ThemeMuted", false, true);
            intro.Margin = new Thickness(0, 0, 0, 12); body.Children.Add(intro);

            var scopeGrid = new UniformGrid { Columns = 2, Margin = new Thickness(-4, 0, -4, 12) };
            var gameSub = hasGameIni
                ? ((_selectedGame != null ? _selectedGame.DisplayName : "") + L(" · 修改 dlssnr-amd.ini 并同步默认副本", " · Updates dlssnr-amd.ini & default copy"))
                : (_selectedGame != null ? L("当前游戏尚未安装 Mochizuki", "Mochizuki not installed in this game") : L("未选择游戏", "No game selected"));
            var tplSub = hasTemplates
                ? string.Format(L("同步更新 {0} 份模板（后续安装 / 覆盖配置生效）", "Updates {0} templates (used on future installs)"), templatePaths.Length)
                : L("未找到启动器模板文件", "No template INI files found");
            var gameScopeCheck = BuildScopeCard(win, L("当前游戏", "Current game"), gameSub, hasGameIni, hasGameIni);
            var tplScopeCheck = BuildScopeCard(win, L("启动器默认模板", "Launcher templates"), tplSub, !hasGameIni && hasTemplates, hasTemplates);
            scopeGrid.Children.Add(gameScopeCheck); scopeGrid.Children.Add(tplScopeCheck); body.Children.Add(scopeGrid);

            var listStack = new StackPanel();
            body.Children.Add(ThemedBorder(12, "ThemeSubtle", "ThemeBorder", listStack));
            var keyButtons = new Button[MochiHotkeySpecs.Length];
            var clearButtons = new Button[MochiHotkeySpecs.Length];
            var slotTemplate = CreateKeycapSlotTemplate();
            for (int i = 0; i < MochiHotkeySpecs.Length; i++) {
                if (i > 0) { var div = new Border { Height = 1 }; div.SetResourceReference(Border.BackgroundProperty, "ThemeHairline"); listStack.Children.Add(div); }
                Button capBtn, clrBtn;
                listStack.Children.Add(BuildHotkeyRow(ghostStyle, slotTemplate, MochiHotkeySpecs[i], out capBtn, out clrBtn));
                keyButtons[i] = capBtn; clearButtons[i] = clrBtn;
            }

            var warnText = HotkeyText("", 11.5, "ThemeWarning", false, true);
            var warnBanner = ThemedBorder(10, "ThemeWarningBg", "ThemeWarning", warnText);
            warnBanner.Padding = new Thickness(12, 8, 12, 8); warnBanner.Margin = new Thickness(0, 10, 0, 0); warnBanner.Visibility = Visibility.Collapsed;
            body.Children.Add(warnBanner);

            var footer = new DockPanel();
            var resetBtn = new Button { Content = L("恢复默认", "Reset defaults"), Style = ghostStyle, Padding = new Thickness(14, 0, 14, 0), Height = 36, MinHeight = 36 };
            var cancelBtn = new Button { Content = L("取消", "Cancel"), Style = ghostStyle, MinWidth = 86, Height = 36, MinHeight = 36, Margin = new Thickness(0, 0, 8, 0) };
            var applyBtn = new Button { Content = L("应用并保存", "Apply & Save"), MinWidth = 116, Height = 36, MinHeight = 36 };
            DialogTheme.Primary(win, applyBtn);
            var rightBtns = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            rightBtns.Children.Add(cancelBtn); rightBtns.Children.Add(applyBtn);
            DockPanel.SetDock(rightBtns, Dock.Right); footer.Children.Add(rightBtns); footer.Children.Add(resetBtn);

            Action refreshUi = delegate {
                var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < currentValues.Length; i++) {
                    if (string.IsNullOrEmpty(currentValues[i])) continue;
                    int c; counts.TryGetValue(currentValues[i], out c); counts[currentValues[i]] = c + 1;
                }
                bool menuEmpty = string.IsNullOrEmpty(currentValues[0]);
                bool menuOptiConflict = !menuEmpty && !string.IsNullOrEmpty(optiMenuKey) && string.Equals(currentValues[0], optiMenuKey, StringComparison.OrdinalIgnoreCase);
                var dupList = new List<string>();
                for (int i = 0; i < currentValues.Length; i++) {
                    int c = 0;
                    bool conflict = !string.IsNullOrEmpty(currentValues[i]) && counts.TryGetValue(currentValues[i], out c) && c > 1;
                    if (conflict && !dupList.Contains(currentValues[i])) dupList.Add(currentValues[i]);
                    if (i == 0 && menuOptiConflict) conflict = true;
                    bool isCap = (capturingIndex == i);
                    var held = isCap ? MochizukiHotkeysIni.FormatHeldModifiers(Keyboard.Modifiers) : "";
                    string preview = isCap
                        ? (held.Length > 0 ? (held + " …") : L("请按下按键… (Esc 取消)", "Press a key… (Esc cancels)"))
                        : (string.IsNullOrEmpty(currentValues[i]) ? L("未绑定（禁用）", "Unbound (disabled)") : currentValues[i]);
                    keyButtons[i].Content = BuildKeycapContent(currentValues[i], preview, conflict, isCap, held.Length > 0);
                    if (isCap) {
                        keyButtons[i].SetResourceReference(Control.BorderBrushProperty, "ThemeAccent");
                        keyButtons[i].BorderThickness = new Thickness(1.5);
                        keyButtons[i].SetResourceReference(Control.BackgroundProperty, "ThemeAccentSoft");
                    } else {
                        keyButtons[i].SetResourceReference(Control.BorderBrushProperty, conflict ? "ThemeWarning" : "ThemeBorder");
                        keyButtons[i].BorderThickness = new Thickness(conflict ? 1.2 : 1);
                        keyButtons[i].SetResourceReference(Control.BackgroundProperty, conflict ? "ThemeWarningBg" : "ThemeSurface");
                    }
                    clearButtons[i].IsEnabled = !isCap && !string.IsNullOrEmpty(currentValues[i]);
                }

                var notices = new List<string>();
                if (!string.IsNullOrEmpty(transientNote)) notices.Add(transientNote);
                if (dupList.Count > 0) notices.Add(string.Format(L("存在重复快捷键：{0}，请改为不同按键避免互抢。", "Duplicate hotkey(s): {0}. Assign distinct keys."), string.Join(" / ", dupList.ToArray())));
                if (menuOptiConflict) notices.Add(string.Format(L("菜单键与 OptiScaler 当前菜单键（{0}）相同，按键时可能会同时触发。", "NR Control menu key matches OptiScaler's menu key ({0}), which may collide in game."), optiMenuKey));
                if (menuEmpty) notices.Add(L("已清空 NR Control 菜单键：游戏内将无法通过快捷键打开调节面板（可随时在启动器重新设置）。", "NR Control MenuKey is cleared: no hotkey will open the panel in game."));
                warnText.Text = string.Join("\n", notices.ToArray());
                warnBanner.Visibility = notices.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                applyBtn.IsEnabled = (gameScopeCheck.IsChecked == true && hasGameIni) || (tplScopeCheck.IsChecked == true && hasTemplates);
                applyBtn.IsDefault = (capturingIndex < 0);
                DialogTheme.SetSuppressEscapeClose(win, capturingIndex >= 0);
            };

            Action stopCapture = delegate { if (capturingIndex >= 0) { capturingIndex = -1; refreshUi(); } };
            for (int i = 0; i < MochiHotkeySpecs.Length; i++) {
                int idx = i;
                keyButtons[idx].Click += delegate { transientNote = null; capturingIndex = (capturingIndex == idx) ? -1 : idx; refreshUi(); };
                clearButtons[idx].Click += delegate { transientNote = null; capturingIndex = -1; currentValues[idx] = ""; refreshUi(); };
            }
            resetBtn.Click += delegate { transientNote = null; capturingIndex = -1; for (int i = 0; i < MochiHotkeySpecs.Length; i++) currentValues[i] = MochiHotkeySpecs[i].DefaultValue; refreshUi(); };
            RoutedEventHandler onScope = delegate { refreshUi(); };
            gameScopeCheck.Checked += onScope; gameScopeCheck.Unchecked += onScope;
            tplScopeCheck.Checked += onScope; tplScopeCheck.Unchecked += onScope;

            win.PreviewKeyDown += delegate(object sender, KeyEventArgs e) {
                if (capturingIndex < 0) return;
                Key raw = (e.Key == Key.System) ? e.SystemKey : e.Key;
                if (raw == Key.Escape) { capturingIndex = -1; transientNote = null; refreshUi(); e.Handled = true; return; }
                string formatted; bool isModOnly, isUnsupported;
                if (MochizukiHotkeysIni.TryFormatWpfKey(e, out formatted, out isModOnly, out isUnsupported)) {
                    currentValues[capturingIndex] = formatted; capturingIndex = -1; transientNote = null; refreshUi();
                } else if (isModOnly) {
                    refreshUi();
                } else if (isUnsupported) {
                    transientNote = string.Format(L("不支持按键 “{0}”：Mochizuki 仅支持 Ctrl / Shift / Alt + F1–F24、A–Z、0–9、Insert、Delete、Home、End、PageUp、PageDown、Pause、ScrollLock。", "Unsupported key \"{0}\": Mochizuki accepts Ctrl / Shift / Alt + F1–F24, A–Z, 0–9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock."), raw.ToString());
                    refreshUi();
                }
                e.Handled = true;
            };
            win.PreviewKeyUp += delegate { if (capturingIndex >= 0) refreshUi(); };
            win.Deactivated += delegate { stopCapture(); };
            cancelBtn.Click += delegate { win.Close(); };
            applyBtn.Click += delegate {
                stopCapture();
                bool applyGame = gameScopeCheck.IsChecked == true && hasGameIni;
                bool applyTpl = tplScopeCheck.IsChecked == true && hasTemplates;
                if ((applyGame || applyTpl) && SaveMochiHotkeys(win, gameDir, gameIniPath, templatePaths, currentValues, applyGame, applyTpl)) {
                    RefreshMochiHotkeyHint();
                    win.DialogResult = true;
                }
            };

            refreshUi();
            DialogTheme.SetContent(win, body, footer);
            win.ShowDialog();
        }
    }
}
