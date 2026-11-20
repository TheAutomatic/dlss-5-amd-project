using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;

namespace OptScalerNR.Launcher
{
    public static class MochizukiHotkeysIni
    {
        public static readonly string[] TemplateFileNames =
        {
            "dlssnr-amd-optiscaler.ini",
            "dlssnr-amd-optiscaler-renodx.ini",
            "dlssnr-amd-standalone.ini",
            "dlssnr-amd-addon.ini"
        };

        public static string CanonicalizeHotkey(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return "";
            var trimmed = raw.Trim();
            if (trimmed.Length == 0) return "";

            bool ctrl = false, shift = false, alt = false;
            string keyToken = null;
            var parts = trimmed.Split('+');
            for (int i = 0; i < parts.Length; i++)
            {
                var p = parts[i].Trim();
                if (p.Length == 0) return null;
                var low = p.ToLowerInvariant();
                if (low == "ctrl" || low == "control") ctrl = true;
                else if (low == "shift") shift = true;
                else if (low == "alt") alt = true;
                else if (keyToken != null) return null;
                else
                {
                    keyToken = NormalizePrimaryToken(low);
                    if (keyToken == null) return null;
                }
            }
            if (keyToken == null) return null;
            return (ctrl ? "Ctrl+" : "") + (shift ? "Shift+" : "") + (alt ? "Alt+" : "") + keyToken;
        }

        static string NormalizePrimaryToken(string low)
        {
            if (low.Length >= 2 && low[0] == 'f')
            {
                int n;
                if (int.TryParse(low.Substring(1), out n) && n >= 1 && n <= 24)
                    return "F" + n.ToString();
                return null;
            }
            if (low.Length == 1)
            {
                char c = low[0];
                if (c >= 'a' && c <= 'z') return char.ToUpperInvariant(c).ToString();
                if (c >= '0' && c <= '9') return c.ToString();
                return null;
            }
            switch (low)
            {
                case "insert": return "Insert";
                case "delete": return "Delete";
                case "home": return "Home";
                case "end": return "End";
                case "pageup": return "PageUp";
                case "pagedown": return "PageDown";
                case "pause": return "Pause";
                case "scrolllock": return "ScrollLock";
                default: return null;
            }
        }

        public static string VkToMochizukiWord(int vk)
        {
            if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x70 + 1).ToString();
            if (vk >= 'A' && vk <= 'Z') return ((char)vk).ToString();
            if (vk >= '0' && vk <= '9') return ((char)vk).ToString();
            switch (vk)
            {
                case 0x2D: return "Insert";
                case 0x2E: return "Delete";
                case 0x24: return "Home";
                case 0x23: return "End";
                case 0x21: return "PageUp";
                case 0x22: return "PageDown";
                case 0x13: return "Pause";
                case 0x91: return "ScrollLock";
                default: return "";
            }
        }

        public static string WpfKeyToPrimaryToken(Key key)
        {
            if (key >= Key.F1 && key <= Key.F24)
                return "F" + ((int)(key - Key.F1) + 1).ToString();
            if (key >= Key.A && key <= Key.Z)
                return ((char)('A' + (int)(key - Key.A))).ToString();
            if (key >= Key.D0 && key <= Key.D9)
                return ((char)('0' + (int)(key - Key.D0))).ToString();
            switch (key)
            {
                case Key.Insert: return "Insert";
                case Key.Delete: return "Delete";
                case Key.Home: return "Home";
                case Key.End: return "End";
                case Key.Prior: return "PageUp";
                case Key.Next: return "PageDown";
                case Key.Pause: return "Pause";
                case Key.Scroll: return "ScrollLock";
                default: return null;
            }
        }

        public static bool IsModifierKey(Key key)
        {
            return key == Key.LeftCtrl || key == Key.RightCtrl ||
                   key == Key.LeftShift || key == Key.RightShift ||
                   key == Key.LeftAlt || key == Key.RightAlt ||
                   key == Key.LWin || key == Key.RWin;
        }

        public static bool TryFormatWpfKey(KeyEventArgs e, out string formatted, out bool isModifierOnly, out bool isUnsupported)
        {
            formatted = null;
            isModifierOnly = false;
            isUnsupported = false;

            Key raw = (e.Key == Key.System) ? e.SystemKey : e.Key;
            if (raw == Key.DeadCharProcessed) raw = e.DeadCharProcessedKey;

            var mods = Keyboard.Modifiers;
            if ((mods & ModifierKeys.Windows) != 0 || raw == Key.LWin || raw == Key.RWin)
            {
                isUnsupported = true;
                return false;
            }
            if (IsModifierKey(raw))
            {
                isModifierOnly = true;
                return false;
            }

            string token = WpfKeyToPrimaryToken(raw);
            if (token == null)
            {
                isUnsupported = true;
                return false;
            }

            bool ctrl = (mods & ModifierKeys.Control) != 0;
            bool shift = (mods & ModifierKeys.Shift) != 0;
            bool alt = (mods & ModifierKeys.Alt) != 0;
            formatted = (ctrl ? "Ctrl+" : "") + (shift ? "Shift+" : "") + (alt ? "Alt+" : "") + token;
            return true;
        }

        public static string FormatHeldModifiers(ModifierKeys mods)
        {
            var sb = new StringBuilder();
            if ((mods & ModifierKeys.Control) != 0) sb.Append("Ctrl+");
            if ((mods & ModifierKeys.Shift) != 0) sb.Append("Shift+");
            if ((mods & ModifierKeys.Alt) != 0) sb.Append("Alt+");
            return sb.ToString();
        }

        static string StripComment(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            for (int i = 0; i < s.Length; i++)
            {
                if (s[i] == ';' || s[i] == '#') return s.Substring(0, i);
            }
            return s;
        }

        public static bool HasIniSection(string text, string section)
        {
            if (string.IsNullOrEmpty(text)) return false;
            using (var sr = new StringReader(text))
            {
                string line;
                while ((line = sr.ReadLine()) != null)
                {
                    var t = line.Trim().TrimStart('\uFEFF');
                    if (t.StartsWith("[") && t.EndsWith("]"))
                    {
                        var secName = t.Substring(1, t.Length - 2).Trim();
                        if (string.Equals(secName, section, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                }
            }
            return false;
        }

        public static string ReadIniKey(string text, string section, string key)
        {
            if (string.IsNullOrEmpty(text)) return null;
            string currentSec = "";
            using (var sr = new StringReader(text))
            {
                string rawLine;
                while ((rawLine = sr.ReadLine()) != null)
                {
                    var t = StripComment(rawLine).Trim().TrimStart('\uFEFF');
                    if (t.Length == 0) continue;
                    if (t[0] == '[' && t[t.Length - 1] == ']')
                    {
                        currentSec = t.Substring(1, t.Length - 2).Trim();
                        continue;
                    }
                    if (!string.Equals(currentSec, section, StringComparison.OrdinalIgnoreCase)) continue;
                    int eq = t.IndexOf('=');
                    if (eq <= 0) continue;
                    var k = t.Substring(0, eq).Trim();
                    if (!string.Equals(k, key, StringComparison.OrdinalIgnoreCase)) continue;
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
            try
            {
                var raw = ReadIniKey(File.ReadAllText(iniPath, Encoding.UTF8), "Menu", "ShortcutKey");
                if (string.IsNullOrEmpty(raw)) return "Insert";
                var trimmed = raw.Trim();
                if (string.Equals(trimmed, "auto", StringComparison.OrdinalIgnoreCase)) return "Insert";
                if (string.Equals(trimmed, "false", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(trimmed, "none", StringComparison.OrdinalIgnoreCase) ||
                    trimmed == "0" || trimmed == "-1")
                    return "";
                int vk;
                if (trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                {
                    if (int.TryParse(trimmed.Substring(2), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out vk))
                        return VkToMochizukiWord(vk);
                }
                else if (int.TryParse(trimmed, out vk))
                {
                    return VkToMochizukiWord(vk);
                }
                var norm = CanonicalizeHotkey(trimmed);
                return norm ?? "Insert";
            }
            catch { return "Insert"; }
        }

        public static string UpdateIniText(string original, IDictionary<string, string> updates, bool onlyExistingSections, bool skipNrControlWhenMissing)
        {
            string nl = (original != null && original.IndexOf("\r\n", StringComparison.Ordinal) >= 0) ? "\r\n" : "\n";
            bool trailingNewline = string.IsNullOrEmpty(original) || original.EndsWith("\n", StringComparison.Ordinal);

            var lines = new List<string>();
            if (!string.IsNullOrEmpty(original))
            {
                var split = original.Replace("\r\n", "\n").Split('\n');
                int count = split.Length;
                if (count > 0 && split[count - 1].Length == 0 && trailingNewline) count--;
                for (int i = 0; i < count; i++)
                {
                    var ln = split[i];
                    if (i == 0 && ln.Length > 0 && ln[0] == '\uFEFF') ln = ln.Substring(1);
                    lines.Add(ln);
                }
            }

            var orderedUpdates = new List<KeyValuePair<string, string>>(updates);
            var handled = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            var secRanges = new Dictionary<string, int[]>(StringComparer.OrdinalIgnoreCase);
            string curSec = "";
            int curStart = -1;
            int curLastNonEmpty = -1;

            for (int i = 0; i < lines.Count; i++)
            {
                var rawLine = lines[i];
                var noComment = StripComment(rawLine).Trim();
                if (noComment.Length > 0 && noComment[0] == '[' && noComment[noComment.Length - 1] == ']')
                {
                    if (curSec.Length > 0 && !secRanges.ContainsKey(curSec))
                        secRanges[curSec] = new[] { curStart, curLastNonEmpty >= curStart ? curLastNonEmpty : curStart };
                    curSec = noComment.Substring(1, noComment.Length - 2).Trim();
                    curStart = i;
                    curLastNonEmpty = i;
                    continue;
                }
                if (rawLine.Trim().Length > 0 && curSec.Length > 0)
                    curLastNonEmpty = i;

                if (curSec.Length == 0 || noComment.Length == 0) continue;
                int eq = noComment.IndexOf('=');
                if (eq <= 0) continue;
                var k = noComment.Substring(0, eq).Trim();
                var mapKey = curSec + "|" + k;
                string newVal;
                if (updates.TryGetValue(mapKey, out newVal))
                {
                    string canonicalKey = k;
                    for (int u = 0; u < orderedUpdates.Count; u++)
                    {
                        if (string.Equals(orderedUpdates[u].Key, mapKey, StringComparison.OrdinalIgnoreCase))
                        {
                            int pipe = orderedUpdates[u].Key.IndexOf('|');
                            canonicalKey = orderedUpdates[u].Key.Substring(pipe + 1);
                            break;
                        }
                    }
                    lines[i] = string.IsNullOrEmpty(newVal) ? (canonicalKey + " =") : (canonicalKey + " = " + newVal);
                    handled.Add(mapKey);
                }
            }
            if (curSec.Length > 0 && !secRanges.ContainsKey(curSec))
                secRanges[curSec] = new[] { curStart, curLastNonEmpty >= curStart ? curLastNonEmpty : curStart };

            // Collect missing keys per section so insertions shift indices predictably.
            var insertions = new List<Tuple<int, string>>();
            var missingSections = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            var missingSectionOrder = new List<string>();

            for (int u = 0; u < orderedUpdates.Count; u++)
            {
                var mapKey = orderedUpdates[u].Key;
                if (handled.Contains(mapKey)) continue;
                int pipe = mapKey.IndexOf('|');
                var sec = mapKey.Substring(0, pipe);
                var key = mapKey.Substring(pipe + 1);
                var val = orderedUpdates[u].Value;
                var lineText = string.IsNullOrEmpty(val) ? (key + " =") : (key + " = " + val);

                int[] range;
                if (secRanges.TryGetValue(sec, out range))
                {
                    insertions.Add(Tuple.Create(range[1] + 1, lineText));
                }
                else
                {
                    if (onlyExistingSections) continue;
                    if (skipNrControlWhenMissing && string.Equals(sec, "NRControl", StringComparison.OrdinalIgnoreCase)) continue;
                    List<string> list;
                    if (!missingSections.TryGetValue(sec, out list))
                    {
                        list = new List<string>();
                        missingSections[sec] = list;
                        missingSectionOrder.Add(sec);
                    }
                    list.Add(lineText);
                }
            }

            if (insertions.Count > 0)
            {
                insertions.Sort(delegate(Tuple<int, string> a, Tuple<int, string> b) { return b.Item1.CompareTo(a.Item1); });
                for (int i = 0; i < insertions.Count; i++)
                    lines.Insert(insertions[i].Item1, insertions[i].Item2);
            }

            for (int i = 0; i < missingSectionOrder.Count; i++)
            {
                var sec = missingSectionOrder[i];
                if (lines.Count > 0 && lines[lines.Count - 1].Trim().Length > 0) lines.Add("");
                lines.Add("[" + sec + "]");
                var items = missingSections[sec];
                for (int j = 0; j < items.Count; j++) lines.Add(items[j]);
            }

            var joined = string.Join(nl, lines.ToArray());
            if (trailingNewline && joined.Length > 0) joined += nl;
            return joined;
        }

        public static void UpdateIniFile(string path, IDictionary<string, string> updates, bool onlyExistingSections, bool skipNrControlWhenMissing)
        {
            var original = File.ReadAllText(path, new UTF8Encoding(false));
            var updated = UpdateIniText(original, updates, onlyExistingSections, skipNrControlWhenMissing);
            File.WriteAllText(path, updated, new UTF8Encoding(false));
        }
    }

    public sealed partial class MainController
    {
        sealed class HotkeySpec
        {
            public string Section;
            public string Key;
            public string DefaultValue;
            public string TitleZh;
            public string TitleEn;
            public string DescZh;
            public string DescEn;
            public string MapKey { get { return Section + "|" + Key; } }
        }

        static readonly HotkeySpec[] MochiHotkeySpecs =
        {
            new HotkeySpec
            {
                Section = "NRControl", Key = "MenuKey", DefaultValue = "End",
                TitleZh = "打开 / 关闭 NR Control 菜单", TitleEn = "Open / Close NR Control",
                DescZh = "[NRControl] MenuKey · 默认 End（ReShade 模式不适用）",
                DescEn = "[NRControl] MenuKey · default End (not used in ReShade mode)"
            },
            new HotkeySpec
            {
                Section = "RenoDX", Key = "NRToggleKey", DefaultValue = "F6",
                TitleZh = "开关神经渲染（本局生效）", TitleEn = "Toggle Neural Rendering",
                DescZh = "[RenoDX] NRToggleKey · 默认 F6（切换开 / 关对比）",
                DescEn = "[RenoDX] NRToggleKey · default F6 (session toggle)"
            },
            new HotkeySpec
            {
                Section = "RenoDX", Key = "NRScreenshotKey", DefaultValue = "F5",
                TitleZh = "对比截图（关 / 开 NR 各一张）", TitleEn = "Before / After Screenshot",
                DescZh = "[RenoDX] NRScreenshotKey · 默认 F5（保存至 NR Screenshots）",
                DescEn = "[RenoDX] NRScreenshotKey · default F5 (saves to NR Screenshots)"
            },
            new HotkeySpec
            {
                Section = "Network", Key = "Hotkey", DefaultValue = "Ctrl+F11",
                TitleZh = "切换网络模型", TitleEn = "Cycle Network Model",
                DescZh = "[Network] Hotkey · 默认 Ctrl+F11（轮切可用模型）",
                DescEn = "[Network] Hotkey · default Ctrl+F11 (cycles models)"
            },
            new HotkeySpec
            {
                Section = "Preprocess", Key = "Hotkey", DefaultValue = "Ctrl+F10",
                TitleZh = "开关预处理（本局生效）", TitleEn = "Toggle Preprocess",
                DescZh = "[Preprocess] Hotkey · 默认 Ctrl+F10（色彩 / 曝光前处理）",
                DescEn = "[Preprocess] Hotkey · default Ctrl+F10 (color / exposure)"
            }
        };

        Button _mochiHotkeysButton;

        void InitMochiHotkeys()
        {
            _mochiHotkeysButton = _window.FindName("MochiHotkeysButton") as Button;
            if (_mochiHotkeysButton != null)
                _mochiHotkeysButton.Click += delegate { ShowMochiHotkeysDialog(); };
        }

        void RefreshMochiHotkeys(bool enabled)
        {
            if (_mochiHotkeysButton == null) return;
            _mochiHotkeysButton.Content = KeyboardLabel(L("快捷键", "Hotkeys"));
            _mochiHotkeysButton.ToolTip = L(
                "在启动器中修改 Mochizuki 当前游戏或默认模板的快捷键",
                "Edit Mochizuki hotkeys for the current game or launcher templates");
            _mochiHotkeysButton.IsEnabled = enabled;
        }

        object KeyboardLabel(string text)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            var icon = new System.Windows.Shapes.Path
            {
                Data = Geometry.Parse("M 1,2 L 13,2 L 13,10 L 1,10 Z M 3,4.3 L 4.3,4.3 M 5.8,4.3 L 7.1,4.3 M 8.6,4.3 L 9.9,4.3 M 11,4.3 L 11.4,4.3 M 3,6.4 L 4.3,6.4 M 5.8,6.4 L 7.1,6.4 M 8.6,6.4 L 11.4,6.4 M 4.2,8.4 L 9.8,8.4"),
                StrokeThickness = 1.1,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                Width = 13, Height = 10,
                Stretch = Stretch.Uniform,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 4, 0)
            };
            icon.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "ThemeInk");
            var label = new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center };
            panel.Children.Add(icon);
            panel.Children.Add(label);
            return panel;
        }

        string[] FindLauncherTemplateInis()
        {
            var roots = new List<string>();
            var importRoot = ResolveImportRoot();
            if (!string.IsNullOrEmpty(importRoot) && Directory.Exists(importRoot)) roots.Add(importRoot);
            if (!string.IsNullOrEmpty(_packageRoot) && Directory.Exists(_packageRoot) &&
                !roots.Exists(delegate(string r) { return string.Equals(r, _packageRoot, StringComparison.OrdinalIgnoreCase); }))
                roots.Add(_packageRoot);

            var found = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int r = 0; r < roots.Count; r++)
            {
                var root = roots[r];
                var searchDirs = new[] { root, Path.Combine(root, "ini") };
                for (int d = 0; d < searchDirs.Length; d++)
                {
                    if (!Directory.Exists(searchDirs[d])) continue;
                    for (int t = 0; t < MochizukiHotkeysIni.TemplateFileNames.Length; t++)
                    {
                        var p = Path.Combine(searchDirs[d], MochizukiHotkeysIni.TemplateFileNames[t]);
                        if (File.Exists(p))
                        {
                            var full = Path.GetFullPath(p);
                            if (seen.Add(full)) found.Add(full);
                        }
                    }
                }
            }
            return found.ToArray();
        }

        static string[] LoadHotkeysFromFile(string path, string fallbackPath)
        {
            var values = new string[MochiHotkeySpecs.Length];
            string primaryText = null;
            string fallbackText = null;
            try { if (!string.IsNullOrEmpty(path) && File.Exists(path)) primaryText = File.ReadAllText(path, Encoding.UTF8); } catch { }
            try { if (!string.IsNullOrEmpty(fallbackPath) && File.Exists(fallbackPath)) fallbackText = File.ReadAllText(fallbackPath, Encoding.UTF8); } catch { }

            for (int i = 0; i < MochiHotkeySpecs.Length; i++)
            {
                var spec = MochiHotkeySpecs[i];
                string raw = primaryText != null ? MochizukiHotkeysIni.ReadIniKey(primaryText, spec.Section, spec.Key) : null;
                if (raw == null && fallbackText != null)
                    raw = MochizukiHotkeysIni.ReadIniKey(fallbackText, spec.Section, spec.Key);
                if (raw == null)
                    values[i] = spec.DefaultValue;
                else
                {
                    var norm = MochizukiHotkeysIni.CanonicalizeHotkey(raw);
                    values[i] = norm ?? spec.DefaultValue;
                }
            }
            return values;
        }

        static UIElement BuildKeycapContent(string hotkey, string placeholder, bool conflict, bool capturing, bool hasModsPreview)
        {
            if (capturing)
            {
                var sp = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                var tb = new TextBlock
                {
                    Text = placeholder,
                    FontSize = 11.5,
                    FontWeight = hasModsPreview ? FontWeights.SemiBold : FontWeights.Normal,
                    VerticalAlignment = VerticalAlignment.Center
                };
                tb.SetResourceReference(TextBlock.ForegroundProperty, "ThemeAccent");
                sp.Children.Add(tb);
                return sp;
            }
            if (string.IsNullOrEmpty(hotkey))
            {
                var emptyText = new TextBlock
                {
                    Text = placeholder,
                    FontSize = 11.5,
                    VerticalAlignment = VerticalAlignment.Center,
                    HorizontalAlignment = HorizontalAlignment.Center
                };
                emptyText.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
                return emptyText;
            }

            var tokens = hotkey.Split('+');
            var panel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            for (int i = 0; i < tokens.Length; i++)
            {
                if (i > 0)
                {
                    var plus = new TextBlock
                    {
                        Text = "+",
                        FontSize = 10.5,
                        Margin = new Thickness(3, 0, 3, 1),
                        VerticalAlignment = VerticalAlignment.Center
                    };
                    plus.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
                    panel.Children.Add(plus);
                }
                var cap = new Border
                {
                    CornerRadius = new CornerRadius(5),
                    BorderThickness = new Thickness(1, 1, 1, 2.2),
                    Padding = new Thickness(7, 2, 7, 2),
                    VerticalAlignment = VerticalAlignment.Center,
                    SnapsToDevicePixels = true
                };
                cap.SetResourceReference(Border.BackgroundProperty, conflict ? "ThemeWarningBg" : "ThemeSurface");
                cap.SetResourceReference(Border.BorderBrushProperty, conflict ? "ThemeWarning" : "ThemeBorder");
                var keyText = new TextBlock
                {
                    Text = tokens[i],
                    FontSize = 11.5,
                    FontWeight = FontWeights.SemiBold,
                    VerticalAlignment = VerticalAlignment.Center
                };
                keyText.SetResourceReference(TextBlock.ForegroundProperty, conflict ? "ThemeWarning" : "ThemeInk");
                cap.Child = keyText;
                panel.Children.Add(cap);
            }
            return panel;
        }

        static ControlTemplate CreateKeycapSlotTemplate()
        {
            var template = new ControlTemplate(typeof(Button));
            var grid = new FrameworkElementFactory(typeof(Grid));
            var root = new FrameworkElementFactory(typeof(Border));
            root.Name = "Slot";
            root.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BackgroundProperty));
            root.SetValue(Border.BorderBrushProperty, new TemplateBindingExtension(Control.BorderBrushProperty));
            root.SetValue(Border.BorderThicknessProperty, new TemplateBindingExtension(Control.BorderThicknessProperty));
            root.SetValue(Border.CornerRadiusProperty, new CornerRadius(8));
            root.SetValue(Border.PaddingProperty, new TemplateBindingExtension(Control.PaddingProperty));
            root.SetValue(UIElement.SnapsToDevicePixelsProperty, true);

            var presenter = new FrameworkElementFactory(typeof(ContentPresenter));
            presenter.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Center);
            presenter.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
            root.AppendChild(presenter);

            var hover = new FrameworkElementFactory(typeof(Border));
            hover.Name = "HoverRing";
            hover.SetValue(Border.CornerRadiusProperty, new CornerRadius(8));
            hover.SetValue(Border.BackgroundProperty, new SolidColorBrush(Color.FromArgb(0x14, 0x80, 0x8A, 0x9E)));
            hover.SetValue(UIElement.IsHitTestVisibleProperty, false);
            hover.SetValue(UIElement.OpacityProperty, 0.0);

            grid.AppendChild(root);
            grid.AppendChild(hover);

            var trig = new Trigger { Property = UIElement.IsMouseOverProperty, Value = true };
            trig.Setters.Add(new Setter(UIElement.OpacityProperty, 1.0, "HoverRing"));
            template.Triggers.Add(trig);

            template.VisualTree = grid;
            return template;
        }

        void ShowMochiHotkeysDialog()
        {
            if (IniBusy()) return;

            var gameDir = (_selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Exe)) ? Path.GetDirectoryName(_selectedGame.Exe) : null;
            var exePath = _selectedGame != null ? _selectedGame.Exe : null;
            var gameIniPath = !string.IsNullOrEmpty(gameDir) ? DlssnrLocalData.ResolveActiveDlssnrIni(gameDir, exePath) : null;
            bool hasGameIni = !string.IsNullOrEmpty(gameIniPath) && File.Exists(gameIniPath);
            var templatePaths = FindLauncherTemplateInis();
            bool hasTemplates = templatePaths.Length > 0;
            string primaryTemplate = hasTemplates ? templatePaths[0] : null;
            string optiMenuKey = hasGameIni ? MochizukiHotkeysIni.ReadOptiScalerMenuKey(gameDir) : "Insert";

            if (!hasGameIni && !hasTemplates)
            {
                DialogTheme.Show(_window,
                    L("未找到当前游戏的 dlssnr-amd.ini，也未在启动器目录中找到默认模板文件。",
                      "Neither a current game's dlssnr-amd.ini nor launcher template INI files were found."),
                    L("Mochizuki 快捷键", "Mochizuki Hotkeys"),
                    MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            var win = DialogTheme.Create(_window, L("Mochizuki 快捷键设置", "Mochizuki Hotkeys"), 630);
            DialogTheme.SetSuppressEscapeClose(win, false);

            var currentValues = LoadHotkeysFromFile(hasGameIni ? gameIniPath : primaryTemplate, primaryTemplate);
            bool loadedFromGame = hasGameIni;
            int capturingIndex = -1;
            string transientNote = null;

            var body = new StackPanel { Margin = new Thickness(20, 14, 20, 12) };
            var intro = new TextBlock
            {
                Text = L("点击右侧按键框后直接按下组合键即可录入（支持 Ctrl / Shift / Alt + F1–F24 / A–Z / 0–9 / Insert / Delete / Home / End / PageUp / PageDown 等），按 Esc 取消录入。",
                         "Click any hotkey slot and press a key combination (Ctrl / Shift / Alt + F1–F24, A–Z, 0–9, Insert, Delete, Home, End, PageUp, PageDown, etc.). Press Esc to cancel capture."),
                TextWrapping = TextWrapping.Wrap,
                FontSize = 11.5,
                Margin = new Thickness(0, 0, 0, 12)
            };
            intro.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
            body.Children.Add(intro);

            // Scope selection grid (2 ModelCard checkboxes)
            var scopeGrid = new UniformGrid { Columns = 2, Margin = new Thickness(-4, 0, -4, 6) };
            var gameScopeCheck = new CheckBox
            {
                Style = win.FindResource("ModelCard") as Style,
                Margin = new Thickness(4, 0, 4, 0),
                IsChecked = hasGameIni,
                IsEnabled = hasGameIni
            };
            if (!hasGameIni) gameScopeCheck.Tag = "unavailable";

            var gameScopeStack = new StackPanel();
            var gameScopeTitle = new TextBlock
            {
                Text = L("当前游戏", "Current game"),
                FontWeight = FontWeights.SemiBold,
                FontSize = 12.5
            };
            gameScopeTitle.SetResourceReference(TextBlock.ForegroundProperty, "ThemeInk");
            var gameScopeSub = new TextBlock
            {
                Text = hasGameIni
                    ? ((_selectedGame != null ? _selectedGame.DisplayName : "") + L(" · 修改 dlssnr-amd.ini 并同步默认副本", " · Updates dlssnr-amd.ini & default copy"))
                    : (_selectedGame != null
                        ? L("当前游戏尚未安装 Mochizuki", "Mochizuki not installed in this game")
                        : L("未选择游戏", "No game selected")),
                FontSize = 10.5,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 2, 0, 0)
            };
            gameScopeSub.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
            gameScopeStack.Children.Add(gameScopeTitle);
            gameScopeStack.Children.Add(gameScopeSub);
            gameScopeCheck.Content = gameScopeStack;

            var tplScopeCheck = new CheckBox
            {
                Style = win.FindResource("ModelCard") as Style,
                Margin = new Thickness(4, 0, 4, 0),
                IsChecked = !hasGameIni && hasTemplates,
                IsEnabled = hasTemplates
            };
            if (!hasTemplates) tplScopeCheck.Tag = "unavailable";

            var tplScopeStack = new StackPanel();
            var tplScopeTitle = new TextBlock
            {
                Text = L("启动器默认模板", "Launcher templates"),
                FontWeight = FontWeights.SemiBold,
                FontSize = 12.5
            };
            tplScopeTitle.SetResourceReference(TextBlock.ForegroundProperty, "ThemeInk");
            var tplScopeSub = new TextBlock
            {
                Text = hasTemplates
                    ? string.Format(L("同步更新 {0} 份模板（后续安装 / 覆盖配置生效）", "Updates {0} templates (used on future installs)"), templatePaths.Length)
                    : L("未找到启动器模板文件", "No template INI files found"),
                FontSize = 10.5,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 2, 0, 0)
            };
            tplScopeSub.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
            tplScopeStack.Children.Add(tplScopeTitle);
            tplScopeStack.Children.Add(tplScopeSub);
            tplScopeCheck.Content = tplScopeStack;

            scopeGrid.Children.Add(gameScopeCheck);
            scopeGrid.Children.Add(tplScopeCheck);
            body.Children.Add(scopeGrid);

            // Source indicator & quick load strip
            var sourceBar = new DockPanel { Margin = new Thickness(0, 4, 0, 10) };
            var sourceBadge = new Border
            {
                CornerRadius = new CornerRadius(9),
                BorderThickness = new Thickness(1),
                Padding = new Thickness(8, 2, 8, 2),
                VerticalAlignment = VerticalAlignment.Center
            };
            sourceBadge.SetResourceReference(Border.BackgroundProperty, "ThemeSubtle");
            sourceBadge.SetResourceReference(Border.BorderBrushProperty, "ThemeBorder");
            var sourceBadgeText = new TextBlock { FontSize = 10.5 };
            sourceBadgeText.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
            sourceBadge.Child = sourceBadgeText;

            var quickLoadPanel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            Button loadGameBtn = null;
            Button loadTplBtn = null;
            if (hasGameIni && hasTemplates)
            {
                loadGameBtn = new Button
                {
                    Content = L("读取游戏配置", "Load game keys"),
                    Style = win.FindResource("GhostButton") as Style,
                    Height = 24, MinHeight = 24, FontSize = 10.5, Padding = new Thickness(9, 0, 9, 0),
                    Margin = new Thickness(0, 0, 6, 0)
                };
                loadTplBtn = new Button
                {
                    Content = L("读取模板值", "Load template keys"),
                    Style = win.FindResource("GhostButton") as Style,
                    Height = 24, MinHeight = 24, FontSize = 10.5, Padding = new Thickness(9, 0, 9, 0)
                };
                quickLoadPanel.Children.Add(loadGameBtn);
                quickLoadPanel.Children.Add(loadTplBtn);
            }
            DockPanel.SetDock(quickLoadPanel, Dock.Right);
            sourceBar.Children.Add(quickLoadPanel);
            sourceBar.Children.Add(sourceBadge);
            body.Children.Add(sourceBar);

            // Rounded hotkey table card
            var listBorder = new Border
            {
                CornerRadius = new CornerRadius(12),
                BorderThickness = new Thickness(1),
                SnapsToDevicePixels = true
            };
            listBorder.SetResourceReference(Border.BackgroundProperty, "ThemeSubtle");
            listBorder.SetResourceReference(Border.BorderBrushProperty, "ThemeBorder");
            var listStack = new StackPanel();
            listBorder.Child = listStack;
            body.Children.Add(listBorder);

            var keyButtons = new Button[MochiHotkeySpecs.Length];
            var clearButtons = new Button[MochiHotkeySpecs.Length];
            var slotTemplate = CreateKeycapSlotTemplate();

            for (int i = 0; i < MochiHotkeySpecs.Length; i++)
            {
                if (i > 0)
                {
                    var div = new Border { Height = 1 };
                    div.SetResourceReference(Border.BackgroundProperty, "ThemeBorder");
                    listStack.Children.Add(div);
                }
                var spec = MochiHotkeySpecs[i];
                var row = new DockPanel { Margin = new Thickness(14, 10, 14, 10) };

                var rightControls = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 0, 0) };
                var capBtn = new Button
                {
                    Template = slotTemplate,
                    MinWidth = 158,
                    Height = 32,
                    MinHeight = 32,
                    Padding = new Thickness(10, 2, 10, 2),
                    BorderThickness = new Thickness(1),
                    Cursor = Cursors.Hand,
                    Focusable = false,
                    ToolTip = L("点击后按下新的组合键；按 Esc 取消录入", "Click then press a hotkey; press Esc to cancel")
                };
                var clrBtn = new Button
                {
                    Content = "✕",
                    Style = win.FindResource("GhostButton") as Style,
                    Width = 28,
                    MinWidth = 28,
                    Height = 32,
                    MinHeight = 32,
                    FontSize = 10.5,
                    Padding = new Thickness(0),
                    Margin = new Thickness(6, 0, 0, 0),
                    Focusable = false,
                    ToolTip = L("清空（禁用此快捷键）", "Clear (disable this hotkey)")
                };
                rightControls.Children.Add(capBtn);
                rightControls.Children.Add(clrBtn);
                DockPanel.SetDock(rightControls, Dock.Right);
                row.Children.Add(rightControls);

                var leftInfo = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                var titleText = new TextBlock
                {
                    Text = L(spec.TitleZh, spec.TitleEn),
                    FontWeight = FontWeights.SemiBold,
                    FontSize = 13
                };
                titleText.SetResourceReference(TextBlock.ForegroundProperty, "ThemeInk");
                var subText = new TextBlock
                {
                    Text = L(spec.DescZh, spec.DescEn),
                    FontSize = 10.5,
                    Margin = new Thickness(0, 2, 0, 0),
                    TextTrimming = TextTrimming.CharacterEllipsis
                };
                subText.SetResourceReference(TextBlock.ForegroundProperty, "ThemeMuted");
                leftInfo.Children.Add(titleText);
                leftInfo.Children.Add(subText);
                row.Children.Add(leftInfo);

                keyButtons[i] = capBtn;
                clearButtons[i] = clrBtn;
                listStack.Children.Add(row);
            }

            // Warning / feedback banner
            var warnBanner = new Border
            {
                CornerRadius = new CornerRadius(10),
                BorderThickness = new Thickness(1),
                Padding = new Thickness(12, 8, 12, 8),
                Margin = new Thickness(0, 10, 0, 0),
                Visibility = Visibility.Collapsed
            };
            warnBanner.SetResourceReference(Border.BackgroundProperty, "ThemeWarningBg");
            warnBanner.SetResourceReference(Border.BorderBrushProperty, "ThemeWarning");
            var warnText = new TextBlock { FontSize = 11.5, TextWrapping = TextWrapping.Wrap };
            warnText.SetResourceReference(TextBlock.ForegroundProperty, "ThemeWarning");
            warnBanner.Child = warnText;
            body.Children.Add(warnBanner);

            // Footer
            var footer = new DockPanel();
            var resetBtn = new Button
            {
                Content = L("恢复默认", "Reset defaults"),
                Style = win.FindResource("GhostButton") as Style,
                Padding = new Thickness(14, 0, 14, 0),
                Height = 36, MinHeight = 36
            };
            var rightBtns = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var cancelBtn = new Button
            {
                Content = L("取消", "Cancel"),
                Style = win.FindResource("GhostButton") as Style,
                MinWidth = 86, Height = 36, MinHeight = 36, Margin = new Thickness(0, 0, 8, 0)
            };
            var applyBtn = new Button
            {
                Content = L("应用并保存", "Apply & Save"),
                MinWidth = 116, Height = 36, MinHeight = 36
            };
            DialogTheme.Primary(win, applyBtn);
            rightBtns.Children.Add(cancelBtn);
            rightBtns.Children.Add(applyBtn);
            DockPanel.SetDock(rightBtns, Dock.Right);
            footer.Children.Add(rightBtns);
            footer.Children.Add(resetBtn);

            Action refreshUi = delegate
            {
                sourceBadgeText.Text = loadedFromGame
                    ? L("当前已载入：游戏内 dlssnr-amd.ini", "Loaded: Current game dlssnr-amd.ini")
                    : L("当前已载入：启动器默认模板", "Loaded: Launcher templates");

                var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < currentValues.Length; i++)
                {
                    if (!string.IsNullOrEmpty(currentValues[i]))
                    {
                        int c;
                        counts.TryGetValue(currentValues[i], out c);
                        counts[currentValues[i]] = c + 1;
                    }
                }

                bool menuEmpty = string.IsNullOrEmpty(currentValues[0]);
                bool menuOptiConflict = !menuEmpty && !string.IsNullOrEmpty(optiMenuKey) &&
                    string.Equals(currentValues[0], optiMenuKey, StringComparison.OrdinalIgnoreCase);

                var conflicts = new bool[currentValues.Length];
                var dupList = new List<string>();
                for (int i = 0; i < currentValues.Length; i++)
                {
                    int c = 0;
                    if (!string.IsNullOrEmpty(currentValues[i]) && counts.TryGetValue(currentValues[i], out c) && c > 1)
                    {
                        conflicts[i] = true;
                        if (!dupList.Contains(currentValues[i])) dupList.Add(currentValues[i]);
                    }
                    if (i == 0 && menuOptiConflict) conflicts[i] = true;

                    bool isCap = (capturingIndex == i);
                    string preview = "";
                    bool hasMods = false;
                    if (isCap)
                    {
                        var held = MochizukiHotkeysIni.FormatHeldModifiers(Keyboard.Modifiers);
                        hasMods = held.Length > 0;
                        preview = hasMods
                            ? (held + " …")
                            : L("请按下按键… (Esc 取消)", "Press a key… (Esc cancels)");
                    }
                    else
                    {
                        preview = string.IsNullOrEmpty(currentValues[i]) ? L("未绑定（禁用）", "Unbound (disabled)") : currentValues[i];
                    }

                    keyButtons[i].Content = BuildKeycapContent(currentValues[i], preview, conflicts[i], isCap, hasMods);
                    if (isCap)
                    {
                        keyButtons[i].SetResourceReference(Control.BorderBrushProperty, "ThemeAccent");
                        keyButtons[i].BorderThickness = new Thickness(1.8);
                        keyButtons[i].Background = new SolidColorBrush(Color.FromArgb(0x1E, 0xE3, 0x12, 0x2B));
                    }
                    else if (conflicts[i])
                    {
                        keyButtons[i].SetResourceReference(Control.BorderBrushProperty, "ThemeWarning");
                        keyButtons[i].BorderThickness = new Thickness(1.2);
                        keyButtons[i].SetResourceReference(Control.BackgroundProperty, "ThemeWarningBg");
                    }
                    else
                    {
                        keyButtons[i].SetResourceReference(Control.BorderBrushProperty, "ThemeBorder");
                        keyButtons[i].BorderThickness = new Thickness(1);
                        keyButtons[i].SetResourceReference(Control.BackgroundProperty, "ThemeSurface");
                    }
                    clearButtons[i].IsEnabled = !isCap && !string.IsNullOrEmpty(currentValues[i]);
                }

                var notices = new List<string>();
                if (!string.IsNullOrEmpty(transientNote)) notices.Add(transientNote);
                if (dupList.Count > 0)
                    notices.Add(string.Format(L("存在重复快捷键：{0}，请改为不同按键避免互抢。", "Duplicate hotkey(s): {0}. Assign distinct keys."), string.Join(" / ", dupList.ToArray())));
                if (menuOptiConflict)
                    notices.Add(string.Format(L("菜单键与 OptiScaler 当前菜单键（{0}）相同，按键时可能会同时触发。", "NR Control menu key matches OptiScaler's menu key ({0}), which may collide in game."), optiMenuKey));
                if (menuEmpty)
                    notices.Add(L("已清空 NR Control 菜单键：游戏内将无法通过快捷键打开调节面板（可随时在启动器重新设置）。", "NR Control MenuKey is cleared: no hotkey will open the panel in game."));

                if (notices.Count > 0)
                {
                    warnText.Text = string.Join("\n", notices.ToArray());
                    warnBanner.Visibility = Visibility.Visible;
                }
                else
                {
                    warnBanner.Visibility = Visibility.Collapsed;
                }

                bool hasTarget = (gameScopeCheck.IsChecked == true && hasGameIni) || (tplScopeCheck.IsChecked == true && hasTemplates);
                applyBtn.IsEnabled = hasTarget;
                applyBtn.IsDefault = (capturingIndex < 0);
                DialogTheme.SetSuppressEscapeClose(win, capturingIndex >= 0);
            };

            Action stopCapture = delegate
            {
                if (capturingIndex >= 0)
                {
                    capturingIndex = -1;
                    refreshUi();
                }
            };

            for (int i = 0; i < MochiHotkeySpecs.Length; i++)
            {
                int idx = i;
                keyButtons[idx].Click += delegate
                {
                    transientNote = null;
                    capturingIndex = (capturingIndex == idx) ? -1 : idx;
                    refreshUi();
                };
                clearButtons[idx].Click += delegate
                {
                    transientNote = null;
                    capturingIndex = -1;
                    currentValues[idx] = "";
                    refreshUi();
                };
            }

            if (loadGameBtn != null)
            {
                loadGameBtn.Click += delegate
                {
                    transientNote = null;
                    capturingIndex = -1;
                    currentValues = LoadHotkeysFromFile(gameIniPath, primaryTemplate);
                    loadedFromGame = true;
                    refreshUi();
                };
            }
            if (loadTplBtn != null)
            {
                loadTplBtn.Click += delegate
                {
                    transientNote = null;
                    capturingIndex = -1;
                    currentValues = LoadHotkeysFromFile(primaryTemplate, null);
                    loadedFromGame = false;
                    refreshUi();
                };
            }

            resetBtn.Click += delegate
            {
                transientNote = null;
                capturingIndex = -1;
                for (int i = 0; i < MochiHotkeySpecs.Length; i++)
                    currentValues[i] = MochiHotkeySpecs[i].DefaultValue;
                refreshUi();
            };

            gameScopeCheck.Checked += delegate { refreshUi(); };
            gameScopeCheck.Unchecked += delegate { refreshUi(); };
            tplScopeCheck.Checked += delegate { refreshUi(); };
            tplScopeCheck.Unchecked += delegate { refreshUi(); };

            win.PreviewKeyDown += delegate(object sender, KeyEventArgs e)
            {
                if (capturingIndex < 0) return;
                Key raw = (e.Key == Key.System) ? e.SystemKey : e.Key;
                if (raw == Key.Escape)
                {
                    capturingIndex = -1;
                    transientNote = null;
                    refreshUi();
                    e.Handled = true;
                    return;
                }
                string formatted;
                bool isModOnly;
                bool isUnsupported;
                if (MochizukiHotkeysIni.TryFormatWpfKey(e, out formatted, out isModOnly, out isUnsupported))
                {
                    currentValues[capturingIndex] = formatted;
                    capturingIndex = -1;
                    transientNote = null;
                    refreshUi();
                }
                else if (isModOnly)
                {
                    refreshUi();
                }
                else if (isUnsupported)
                {
                    transientNote = string.Format(
                        L("不支持按键 “{0}”：Mochizuki 仅支持 Ctrl / Shift / Alt + F1–F24、A–Z、0–9、Insert、Delete、Home、End、PageUp、PageDown、Pause、ScrollLock。",
                          "Unsupported key \"{0}\": Mochizuki accepts Ctrl / Shift / Alt + F1–F24, A–Z, 0–9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock."),
                        raw.ToString());
                    refreshUi();
                }
                e.Handled = true;
            };

            win.PreviewKeyUp += delegate(object sender, KeyEventArgs e)
            {
                if (capturingIndex >= 0) refreshUi();
            };

            win.Deactivated += delegate { stopCapture(); };

            cancelBtn.Click += delegate { win.Close(); };

            applyBtn.Click += delegate
            {
                stopCapture();
                var updates = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < MochiHotkeySpecs.Length; i++)
                    updates[MochiHotkeySpecs[i].MapKey] = currentValues[i] ?? "";

                bool applyGame = gameScopeCheck.IsChecked == true && hasGameIni;
                bool applyTpl = tplScopeCheck.IsChecked == true && hasTemplates;
                if (!applyGame && !applyTpl) return;

                try
                {
                    int tplCount = 0;
                    if (applyGame)
                    {
                        bool skipNrCtrl = !MochizukiHotkeysIni.HasIniSection(File.ReadAllText(gameIniPath, Encoding.UTF8), "NRControl");
                        MochizukiHotkeysIni.UpdateIniFile(gameIniPath, updates, false, skipNrCtrl);
                        var defaultCopy = Path.Combine(gameDir, @"dlssnr-amd\dlssnr-amd.default.ini");
                        if (File.Exists(defaultCopy))
                        {
                            bool skipDefaultNrCtrl = !MochizukiHotkeysIni.HasIniSection(File.ReadAllText(defaultCopy, Encoding.UTF8), "NRControl");
                            MochizukiHotkeysIni.UpdateIniFile(defaultCopy, updates, false, skipDefaultNrCtrl);
                        }
                    }
                    if (applyTpl)
                    {
                        for (int t = 0; t < templatePaths.Length; t++)
                        {
                            MochizukiHotkeysIni.UpdateIniFile(templatePaths[t], updates, true, true);
                            tplCount++;
                        }
                    }

                    if (_statusText != null)
                    {
                        if (applyGame && applyTpl)
                            _statusText.Text = string.Format(L("已更新当前游戏 dlssnr-amd.ini 及 {0} 份启动器模板快捷键。", "Updated current game dlssnr-amd.ini and {0} launcher templates."), tplCount);
                        else if (applyGame)
                            _statusText.Text = L("已更新当前游戏 dlssnr-amd.ini 快捷键。", "Updated current game dlssnr-amd.ini hotkeys.");
                        else
                            _statusText.Text = string.Format(L("已更新 {0} 份启动器默认模板快捷键。", "Updated {0} launcher hotkey templates."), tplCount);
                    }
                    win.DialogResult = true;
                }
                catch (UnauthorizedAccessException)
                {
                    DialogTheme.Show(win,
                        L("没有权限写入目标配置文件（游戏或启动器目录可能位于 Program Files 等受保护位置）。请以管理员身份运行启动器后再试。",
                          "Permission denied writing the configuration file. Run the launcher as Administrator and try again."),
                        win.Title, MessageBoxButton.OK, MessageBoxImage.Error);
                }
                catch (Exception ex)
                {
                    DialogTheme.Show(win,
                        L("保存快捷键失败：", "Failed to save hotkeys: ") + ex.Message,
                        win.Title, MessageBoxButton.OK, MessageBoxImage.Error);
                }
            };

            refreshUi();
            DialogTheme.SetContent(win, body, footer);
            win.ShowDialog();
        }
    }
}
