using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace OptScalerNR.Launcher
{
    [DataContract]
    public sealed class ProxyProfileDocument
    {
        [DataMember(Name="schema")] public int Schema;
        [DataMember(Name="revision")] public long Revision;
        [DataMember(Name="reviewed")] public string Reviewed;
        [DataMember(Name="proxies")] public string[] Proxies;
        [DataMember(Name="profiles")] public ProxyProfile[] Profiles;
    }
    [DataContract]
    public sealed class ProxyProfile
    {
        [DataMember(Name="id")] public string Id;
        [DataMember(Name="names")] public string[] Names;
        [DataMember(Name="pathSegments")] public string[] PathSegments;
        [DataMember(Name="preferred")] public string Preferred;
        [DataMember(Name="alternatives")] public string[] Alternatives;
        [DataMember(Name="notesEn")] public string NotesEn;
        [DataMember(Name="notesZh")] public string NotesZh;
        [DataMember(Name="source")] public string Source;
        [DataMember(Name="sourceFormat")] public string SourceFormat;
        [DataMember(Name="upstreamVersion")] public string UpstreamVersion;
        [DataMember(Name="optiPatcher")] bool? _optiPatcher;
        public bool OptiPatcher
        {
            // Older bundled/cache documents preserve the Wiki column in generated notes.
            get { return _optiPatcher ?? ((NotesEn ?? "").IndexOf(" · OptiPatcher supported · ", StringComparison.Ordinal) >= 0); }
            set { _optiPatcher = value; }
        }
    }

    public sealed class ProxyRecommendations : IDisposable
    {
        const int Limit = 1024 * 1024;
        public const string WikiCompatibilityListUrl = "https://raw.githubusercontent.com/wiki/optiscaler/OptiScaler/Compatibility-List.md";
        public const string WikiCompatibilityListPageUrl = "https://github.com/optiscaler/OptiScaler/wiki/Compatibility-List";
        const char WorkingMark = '\u2705';     // check mark
        const char PartialMark = '\u2796';     // minus
        const char BrokenMark = '\u274C';      // cross mark
        const char OptiPatcherMark = '\u2728'; // sparkles
        const string DefaultProxy = "dxgi.dll";

        const int DeadlineMs = 25000; // Whole document must arrive within 25 s; otherwise the current recommendations stay in use.
        static readonly ProxyProfileDocument Seed = ReadSeed();
        public static readonly string[] ProxyNames = Seed.Proxies;
        static readonly Regex CommentExpression = new Regex("<!--.*?-->", RegexOptions.Singleline | RegexOptions.Compiled);
        static readonly Regex AttachmentExpression = new Regex(@"\[[^\]]*\]\(https://github\.com/user-attachments/[^)]*\)", RegexOptions.Compiled);
        static readonly Regex LinkExpression = new Regex(@"\[([^\]]*)\]\(((?:[^()]|\([^()]*\))+)\)", RegexOptions.Compiled);
        static readonly Regex ProxyExpression = new Regex(@"\b(dxgi|winmm|version|d3d12|winhttp|wininet|dbghelp)(\.dll)?\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
        static readonly Regex ShortFormLeadExpression = new Regex(@"(?:use|using|as|install|installing|rename|renamed|works|working|filename|filenames|name)\s+$", RegexOptions.IgnoreCase | RegexOptions.Compiled);
        static readonly Regex BridgeExpression = new Regex(@"^(?:[\s,`]*(?:with|on|in|is|are|does|and|or|to|the|game|mode|inputs?|outputs?|fg))*[\s,`]*$", RegexOptions.IgnoreCase | RegexOptions.Compiled);
        static readonly Regex SentenceExpression = new Regex(@"\.(?=[ \t])", RegexOptions.Compiled);
        static readonly Regex SeparatorCellExpression = new Regex(@"^:?-+:?$", RegexOptions.Compiled);
        static readonly string[] BeforeNegativeCues = {
            "do not use", "don't use", "dont use", "does not use", "doesn't use", "not use", "never use",
            "avoid using", "avoid", "cannot use", "can't use", "cant use", "cannot work", "can't work",
            "cannot hook", "can't hook", "couldn't", "unusable", "broken", "blocked", "banned",
            "incompatible", "unsupported", "instead of", "no need", "not needed"
        };
        static readonly string[] AfterNegativeCues = {
            "does not work", "doesn't work", "doesnt work", "not working", "won't work", "wont work",
            "crashes", "crash", "crashing", "is broken", "are broken", "unusable", "fails", "instead of",
            "cannot be used", "can't be used", "not supported", "does not function"
        };
        static readonly string[] LinuxCues = { "linux", "proton", "vkd3d", "dxvk", "\uD83D\uDC27" };
        static readonly string[] EditionSuffixes = {
            "definitiveedition", "remastered", "completeedition", "gotyedition", "deluxeedition",
            "enhancededition", "ultimatesedition", "directorscut", "gameoftheyearedition", "goty"
        };
        static readonly string[,] ChinesePhrases = {
            { "Online game, use at your own risk!", "网游，风险自负！" },
            { "online game, use at your own risk", "网游，风险自负" },
            { "use at your own risk", "风险自负" },
            { "Requires REFramework", "需要 REFramework" },
            { "requires REFramework", "需要 REFramework" },
            { "check the compatibility entry", "查看 Wiki 兼容条目" },
            { "Check the compatibility entry", "查看 Wiki 兼容条目" },
            { "check compatibility entry", "查看 Wiki 兼容条目" },
            { "check the compat entry", "查看 Wiki 兼容条目" },
            { "check Wiki compat entry", "查看 Wiki 兼容条目" },
            { "Click on the game's name for the detailed entry", "点击游戏名查看详细条目" },
            { "Create shortcut with -dx12 launch parameter", "创建快捷方式并附加 -dx12 启动参数" },
            { "Create shortcut and add a -dx12 launch parameter", "创建快捷方式并附加 -dx12 启动参数" },
            { "-dx12 launch parameter", "-dx12 启动参数" },
            { "-dx12 launch command", "-dx12 启动命令" },
            { "-dx12 launch option", "-dx12 启动参数" },
            { "launch option", "启动参数" },
            { "Launch executable with", "用以下参数启动游戏：" },
            { "launch game from", "请从以下主程序启动：" },
            { "force DX12", "强制 DX12" },
            { "DX12 cannot be forced", "无法强制 DX12" },
            { "Use DX12 mode", "使用 DX12 模式" },
            { "use DX12 mode", "使用 DX12 模式" },
            { "Run in DX12", "以 DX12 运行" },
            { "run in DX12", "以 DX12 运行" },
            { "Run in DirectX 12 mode", "以 DirectX 12 模式运行" },
            { "DX12 mode", "DX12 模式" },
            { "Install OptiScaler as", "将 OptiScaler 安装为" },
            { "install OptiScaler as", "将 OptiScaler 安装为" },
            { "Use OptiScaler as", "将 OptiScaler 用作" },
            { "use OptiScaler as", "将 OptiScaler 用作" },
            { "Use Opti as", "将 Opti 用作" },
            { "use Opti as", "将 Opti 用作" },
            { "Opti works as", "Opti 可用作" },
            { "seem to work fine as", "可正常用作" },
            { "Works with", "可用：" },
            { "works with", "可用：" },
            { "works as", "可用作" },
            { "renamed to", "重命名为" },
            { "to detect DLSS input", "以检测 DLSS 输入" },
            { "Recommended to disable spoofing", "建议关闭显卡伪装" },
            { "disable spoofing", "关闭显卡伪装" },
            { "spoofing isn't required", "无需显卡伪装" },
            { "Spoofing for DLSS inputs doesn't work", "DLSS 输入的伪装无效" },
            { "spoofing", "显卡伪装" },
            { "Signed Opti builds", "签名的 Opti 构建" },
            { "signed build", "签名版本" },
            { "unsigned builds", "未签名构建" },
            { "Make sure EAC is disabled", "请确保已禁用 EAC" },
            { "Disable EAC", "禁用 EAC" },
            { "disable EAC", "禁用 EAC" },
            { "anti-cheat", "反作弊" },
            { "anti cheat", "反作弊" },
            { "For Xbox Gamepass version", "XBOX GamePass 版" },
            { "for Xbox version", "XBOX 版" },
            { "Xbox version", "XBOX 版" },
            { "Gamepass version", "GamePass 版" },
            { "MS Store", "微软商店" },
            { "For Linux", "Linux 下" },
            { "On Linux", "Linux 下" },
            { "for Linux", "Linux 下" },
            { "Linux:", "Linux：" },
            { "black screens and flickering", "黑屏与闪烁" },
            { "black screen", "黑屏" },
            { "flickering", "闪烁" },
            { "shimmering", "闪烁" },
            { "ghosting", "拖影" },
            { "crashes", "崩溃" },
            { "crashing", "崩溃" },
            { "crash", "崩溃" },
            { "Non-Linear Color Space", "Non-Linear 色彩空间" },
            { "Non-Linear sRGB", "Non-Linear sRGB" },
            { "in-game", "游戏内" },
            { "game settings", "游戏设置" },
            { "overlay", "悬浮层" },
            { "Overlay", "悬浮层" },
            { "upscaler", "升采样器" },
            { "Upscaler", "升采样器" },
            { "inputs", "输入" },
            { "Inputs", "输入" },
            { "Known issues", "已知问题" },
            { "workaround", "临时方案" },
            { "Recommended", "建议" },
            { "recommended", "建议" },
            { "Requires", "需要" },
            { "requires", "需要" },
            { "in latest game update", "（最新版本起）" },
            { " or ", " 或 " }
        };

        readonly object _gate = new object();
        readonly string _directory;
        readonly string _url;
        ProxyProfileDocument _current = Seed;
        string _etag;
        bool _cached, _disposed;
        int _updating;
        HttpWebRequest _request;

        static ProxyProfileDocument ReadSeed()
        {
            using (var stream = typeof(ProxyRecommendations).Assembly.GetManifestResourceStream("OptScalerNR.Launcher.proxy-profiles.json"))
            using (var reader = new StreamReader(stream, Encoding.UTF8))
                return JsonUtil.Read<ProxyProfileDocument>(reader.ReadToEnd());
        }
        public ProxyRecommendations(string settingsDirectory) : this(settingsDirectory, null) { }
        public ProxyRecommendations(string settingsDirectory, string updateUrl)
        {
            Validate(Seed);
            _directory = Path.Combine(settingsDirectory, "compatibility");
            _url = !string.IsNullOrEmpty(updateUrl) ? updateUrl : Environment.GetEnvironmentVariable("OPTSCALERNR_WIKI_URL");
            if (string.IsNullOrEmpty(_url) || !Uri.IsWellFormedUriString(_url, UriKind.Absolute)) _url = WikiCompatibilityListUrl;
            try {
                var path = Path.Combine(_directory, "proxy-profiles.json");
                if (File.Exists(path) && new FileInfo(path).Length <= Limit) {
                    var doc = JsonUtil.Read<ProxyProfileDocument>(File.ReadAllText(path, Encoding.UTF8));
                    Validate(doc);
                    if (doc.Revision >= Seed.Revision) { _current = doc; _cached = true; }
                    var etag = Path.Combine(_directory, "etag.txt");
                    if (_cached && File.Exists(etag) && new FileInfo(etag).Length < 512) {
                        var value = File.ReadAllText(etag);
                        if (value.IndexOfAny(new[] {'\r','\n'}) < 0) _etag = value;
                    }
                }
            } catch { /* Invalid cached data never replaces the bundled document. */ }
        }
        public string Reviewed { get { lock (_gate) return _current.Reviewed; } }
        public bool UsingCache { get { lock (_gate) return _cached; } }
        public int Count { get { lock (_gate) return CountUnlocked(); } }
        public string UpdateUrl { get { return _url; } }
        int CountUnlocked() { return _current == null || _current.Profiles == null ? 0 : _current.Profiles.Length; }
        public static bool Supports(string name) { return Array.Exists(ProxyNames, p => string.Equals(p, name, StringComparison.OrdinalIgnoreCase)); }

        static void Validate(ProxyProfileDocument doc)
        {
            DateTime reviewed;
            if (doc == null || doc.Schema != 1 || doc.Revision < 1 || doc.Profiles == null || doc.Profiles.Length > 1500 ||
                !DateTime.TryParseExact(doc.Reviewed, "yyyy-MM-dd", System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.None, out reviewed) || doc.Proxies == null || doc.Proxies.Length != ProxyNames.Length)
                throw new InvalidDataException("Invalid recommendation document.");
            var proxies = new HashSet<string>(doc.Proxies, StringComparer.OrdinalIgnoreCase);
            foreach (var name in ProxyNames) if (!proxies.Contains(name)) throw new InvalidDataException("Unsupported proxy list.");
            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var p in doc.Profiles) {
                Uri uri;
                if (p == null || string.IsNullOrEmpty(p.Id) || !ids.Add(p.Id) || !Supports(p.Preferred) ||
                    p.Names == null || p.Names.Length > 32 || p.PathSegments == null || p.PathSegments.Length > 32 ||
                    p.Alternatives == null || p.Alternatives.Length > ProxyNames.Length ||
                    !Uri.TryCreate(p.Source, UriKind.Absolute, out uri) || uri.Scheme != "https" || uri.Host != "github.com" ||
                    (p.NotesEn ?? "").Length > 3000 || (p.NotesZh ?? "").Length > 3000)
                    throw new InvalidDataException("Invalid recommendation record.");
                foreach (var a in p.Alternatives) if (!Supports(a)) throw new InvalidDataException("Unsupported proxy.");
                foreach (var n in p.Names) if (string.IsNullOrWhiteSpace(n) || n.Length > 200) throw new InvalidDataException("Invalid game identity.");
                foreach (var n in p.PathSegments) if (string.IsNullOrWhiteSpace(n) || n.Length > 200 || n.IndexOfAny(new[] {'/','\\'}) >= 0)
                    throw new InvalidDataException("Invalid game path identity.");
                if (p.Names.Length == 0 && p.PathSegments.Length == 0) throw new InvalidDataException("Missing game identity.");
            }
        }
        string _lastMatchKey;
        long _lastMatchRevision;
        ProxyProfile _lastMatchProfile;

        public ProxyProfile Match(GameInfo game)
        {
            if (game == null) return null;
            ProxyProfileDocument doc;
            lock (_gate) doc = _current;
            if (doc == null || doc.Profiles == null) return null;
            var matchKey = (game.Name ?? "") + "\0" + (game.DisplayName ?? "") + "\0" + (game.Root ?? "") + "\0" + (game.Exe ?? "");
            lock (_gate)
            {
                if (_lastMatchRevision == doc.Revision && string.Equals(_lastMatchKey, matchKey, StringComparison.Ordinal))
                    return _lastMatchProfile;
            }
            var matched = MatchCore(doc, game);
            lock (_gate)
            {
                _lastMatchRevision = doc.Revision;
                _lastMatchKey = matchKey;
                _lastMatchProfile = matched;
            }
            return matched;
        }

        static ProxyProfile MatchCore(ProxyProfileDocument doc, GameInfo game)
        {
            var paths = ((game.Root ?? "") + "\\" + (game.Exe ?? "")).Split(new[] {'\\','/'}, StringSplitOptions.RemoveEmptyEntries);
            var primary = NormalizeIdentity(game.Name);
            var display = NormalizeIdentity(game.DisplayName);
            foreach (var profile in doc.Profiles)
                foreach (var name in profile.Names) {
                    if (string.Equals(game.Name, name, StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(game.DisplayName, name, StringComparison.OrdinalIgnoreCase)) return profile;
                    var key = NormalizeIdentity(name);
                    if (key.Length > 0 && (key == primary || key == display)) return profile;
                }
            var primaryStripped = StripEditionSuffix(primary);
            var displayStripped = StripEditionSuffix(display);
            if (primaryStripped.Length > 0 || displayStripped.Length > 0)
                foreach (var profile in doc.Profiles)
                    foreach (var name in profile.Names) {
                        var key = StripEditionSuffix(NormalizeIdentity(name));
                        if (key.Length > 0 && (key == primaryStripped || key == displayStripped)) return profile;
                    }
            foreach (var profile in doc.Profiles)
                foreach (var segment in profile.PathSegments) {
                    var segmentKey = NormalizeIdentity(segment);
                    var segmentStripped = StripEditionSuffix(segmentKey);
                    foreach (var actual in paths) {
                        if (string.Equals(actual, segment, StringComparison.OrdinalIgnoreCase)) return profile;
                        var trimmed = actual.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? actual.Substring(0, actual.Length - 4) : actual;
                        if (string.Equals(trimmed, segment, StringComparison.OrdinalIgnoreCase)) return profile;
                        var actualKey = NormalizeIdentity(actual);
                        if (segmentKey.Length > 1 && (segmentKey == actualKey || segmentKey == NormalizeIdentity(trimmed))) return profile;
                        if (segmentStripped.Length > 2 && segmentStripped == StripEditionSuffix(actualKey)) return profile;
                    }
                }
            return null;
        }

        /// <summary>
        /// Parses the OptiScaler Wiki compatibility Markdown (main table plus the linked
        /// upscaler-mod and Luma UE tables) into a validated profile document, merging the
        /// result over <paramref name="baseSeed"/> so curated aliases survive a live refresh.
        /// </summary>
        public static ProxyProfileDocument ParseWikiCompatibilityMarkdown(string markdown, ProxyProfileDocument baseSeed, string reviewedDate)
        {
            if (string.IsNullOrEmpty(markdown)) throw new InvalidDataException("Empty wiki document.");
            var seed = baseSeed ?? Seed;
            if (seed == null || seed.Proxies == null) throw new InvalidDataException("Missing recommendation baseline.");
            var reviewed = reviewedDate;
            if (string.IsNullOrEmpty(reviewed)) reviewed = DateTime.UtcNow.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
            long revision;
            if (!long.TryParse(reviewed.Replace("-", "") + "01", NumberStyles.None, CultureInfo.InvariantCulture, out revision) || revision < 1)
                revision = long.Parse(DateTime.UtcNow.ToString("yyyyMMdd", CultureInfo.InvariantCulture) + "01", CultureInfo.InvariantCulture);
            var profiles = new List<ProxyProfile>();
            var byName = new Dictionary<string, ProxyProfile>(StringComparer.OrdinalIgnoreCase);
            var byStripped = new Dictionary<string, ProxyProfile>(StringComparer.OrdinalIgnoreCase);
            var usedIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var previousWikiEntries = 0;
            if (seed.Profiles != null)
                foreach (var source in seed.Profiles) {
                    if (source == null || string.IsNullOrEmpty(source.Id)) continue;
                    // Machine-generated entries are rebuilt from the live rows below. Keeping them would
                    // preserve games the wiki has since removed or marked broken with their old advice.
                    if (string.Equals(source.SourceFormat, "wiki", StringComparison.OrdinalIgnoreCase)) { previousWikiEntries++; continue; }
                    if (!usedIds.Add(source.Id)) continue;
                    var profile = CloneProfile(source);
                    profiles.Add(profile);
                    foreach (var name in profile.Names) IndexIdentity(byName, byStripped, name, profile);
                    foreach (var segment in profile.PathSegments) IndexIdentity(byName, byStripped, segment, profile);
                }
            var rows = ReadWikiRows(markdown);
            // A page whose table layout changed parses to few rows; refuse it instead of dropping the seed.
            if (previousWikiEntries > 0 && rows.Count * 2 < previousWikiEntries)
                throw new InvalidDataException("Wiki table parsed to too few rows.");
            foreach (var row in rows) {
                var analysis = AnalyzeProxies(row.Notes);
                ProxyProfile existing;
                var titleKey = NormalizeIdentity(row.Title);
                if (!byName.TryGetValue(titleKey, out existing)) byStripped.TryGetValue(StripEditionSuffix(titleKey), out existing);
                if (existing != null) { MergeRowIntoProfile(existing, row, analysis); continue; }
                var created = ProfileFromRow(row, analysis, usedIds);
                profiles.Add(created);
                IndexIdentity(byName, byStripped, created.Names[0], created);
                if (created.PathSegments.Length > 0) IndexIdentity(byName, byStripped, created.PathSegments[0], created);
            }
            var document = new ProxyProfileDocument();
            document.Schema = 1;
            document.Revision = revision < seed.Revision ? seed.Revision : revision;
            document.Reviewed = reviewed;
            document.Proxies = seed.Proxies;
            document.Profiles = profiles.ToArray();
            return document;
        }

        sealed class WikiRow
        {
            public string Title, PageUrl, Inputs, Notes;
            public bool HasLink, OptiPatcher, ThirdPartySection, LumaSection;
            public char Compatibility;
        }
        sealed class WikiProxyAnalysis
        {
            public readonly List<string> Recommended = new List<string>(2);
            public readonly List<string> LinuxOnly = new List<string>(1);
        }

        static List<WikiRow> ReadWikiRows(string markdown)
        {
            var rows = new List<WikiRow>();
            var text = CommentExpression.Replace(markdown, "").Replace("\r\n", "\n").Replace('\r', '\n');
            var thirdParty = false; var luma = false;
            var notesColumn = -1; var patcherColumn = -1;
            foreach (var raw in text.Split('\n')) {
                var line = raw.Trim();
                if (line.Length == 0) continue;
                if (line[0] == '#') {
                    var heading = line.TrimStart('#').Trim();
                    thirdParty = heading.IndexOf("upscaler mods", StringComparison.OrdinalIgnoreCase) >= 0;
                    luma = heading.IndexOf("luma unreal", StringComparison.OrdinalIgnoreCase) >= 0;
                    continue;
                }
                if (line[0] != '|') continue;
                var cells = SplitTableRow(line);
                if (cells.Count == 0 || IsSeparatorRow(cells)) continue;
                if (IsHeaderRow(cells)) {
                    notesColumn = -1; patcherColumn = -1;
                    for (var i = 0; i < cells.Count; i++) {
                        if (notesColumn < 0 && cells[i].IndexOf("notes", StringComparison.OrdinalIgnoreCase) >= 0) notesColumn = i;
                        else if (cells[i].IndexOf("optipatcher", StringComparison.OrdinalIgnoreCase) >= 0) patcherColumn = i;
                    }
                    continue;
                }
                if (notesColumn < 2 || cells.Count <= notesColumn) continue;
                var row = ParseRow(cells, notesColumn, patcherColumn, thirdParty, luma);
                if (row != null) rows.Add(row);
            }
            return rows;
        }
        static List<string> SplitTableRow(string line)
        {
            var cells = new List<string>();
            var builder = new StringBuilder();
            var body = line.StartsWith("|", StringComparison.Ordinal) ? line.Substring(1) : line;
            for (var i = 0; i < body.Length; i++) {
                var ch = body[i];
                if (ch == '\\' && i + 1 < body.Length && body[i + 1] == '|') { builder.Append('|'); i++; continue; }
                if (ch == '|') { cells.Add(builder.ToString().Trim()); builder.Length = 0; continue; }
                builder.Append(ch);
            }
            var trailing = builder.ToString().Trim();
            if (trailing.Length > 0) cells.Add(trailing);
            else if (cells.Count > 0 && cells[cells.Count - 1].Length == 0) cells.RemoveAt(cells.Count - 1);
            return cells;
        }
        static bool IsSeparatorRow(List<string> cells)
        {
            var marked = false;
            foreach (var cell in cells) {
                if (cell.Length == 0) continue;
                if (!SeparatorCellExpression.IsMatch(cell)) return false;
                marked = true;
            }
            return marked;
        }
        static bool IsHeaderRow(List<string> cells)
        {
            var first = cells[0].Trim();
            if (!string.Equals(first, "Game", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(first, "GAME NAME", StringComparison.OrdinalIgnoreCase)) return false;
            foreach (var cell in cells)
                if (cell.IndexOf("notes", StringComparison.OrdinalIgnoreCase) >= 0) return true;
            return false;
        }
        static WikiRow ParseRow(List<string> cells, int notesColumn, int patcherColumn, bool thirdParty, bool luma)
        {
            var title = cells[0];
            var url = (string)null; var hasLink = false;
            var link = LinkExpression.Match(title);
            if (link.Success) {
                title = link.Groups[1].Value;
                var target = link.Groups[2].Value.Trim();
                if (target.Length > 0 && target[0] != '#') {
                    hasLink = true;
                    url = target.StartsWith("http", StringComparison.OrdinalIgnoreCase)
                        ? target : "https://github.com/optiscaler/OptiScaler/wiki/" + target;
                    // Validate() only accepts https://github.com sources; an external title link must not
                    // invalidate the whole refresh, the row falls back to the compatibility list page.
                    Uri parsed;
                    if (!Uri.TryCreate(url, UriKind.Absolute, out parsed) || parsed.Scheme != "https" || parsed.Host != "github.com") {
                        url = null; hasLink = false;
                    }
                }
            }
            title = CollapseWhitespace(title.Replace("**", "").Replace("`", ""));
            if (title.Length == 0) return null;
            var compat = cells.Count > 1 ? cells[1] : "";
            char mark;
            if (compat.IndexOf(WorkingMark) >= 0) mark = WorkingMark;
            else if (compat.IndexOf(PartialMark) >= 0) mark = PartialMark;
            else return null; // Broken (cross mark) and unmarked rows never receive a recommendation.
            if (compat.IndexOf(BrokenMark) >= 0) return null;
            var row = new WikiRow();
            row.Title = title;
            row.PageUrl = url;
            row.HasLink = hasLink;
            row.Compatibility = mark;
            row.Inputs = cells.Count > 2 ? CollapseWhitespace(cells[2].Replace("<br>", " ").Replace("<br/>", " ").Replace("<br />", " ")) : "";
            row.OptiPatcher = patcherColumn >= 0 && patcherColumn < cells.Count && cells[patcherColumn].IndexOf(OptiPatcherMark) >= 0;
            row.Notes = cells[notesColumn];
            row.ThirdPartySection = thirdParty;
            row.LumaSection = luma;
            return row;
        }
        static WikiProxyAnalysis AnalyzeProxies(string notes)
        {
            var result = new WikiProxyAnalysis();
            if (string.IsNullOrEmpty(notes)) return result;
            var text = notes.Replace("<br />", "\n").Replace("<br/>", "\n").Replace("<br>", "\n");
            // Sentence ends (". ") are separated without touching the dot inside "dxgi.dll".
            foreach (var sentence in SentenceExpression.Split(text))
                foreach (var clause in sentence.Split(new[] {'\n', ';', ','}, StringSplitOptions.RemoveEmptyEntries))
                    AnalyzeClause(clause, result);
            if (result.LinuxOnly.Count > 0 && result.Recommended.Count > 0)
                for (var i = result.LinuxOnly.Count - 1; i >= 0; i--)
                    foreach (var name in result.Recommended)
                        if (string.Equals(name, result.LinuxOnly[i], StringComparison.OrdinalIgnoreCase)) { result.LinuxOnly.RemoveAt(i); break; }
            return result;
        }
        static void AnalyzeClause(string clause, WikiProxyAnalysis result)
        {
            var matches = ProxyExpression.Matches(clause);
            if (matches.Count == 0) return;
            var linuxScope = false;
            foreach (var cue in LinuxCues) if (clause.IndexOf(cue, StringComparison.OrdinalIgnoreCase) >= 0) { linuxScope = true; break; }
            for (var m = 0; m < matches.Count; m++) {
                var match = matches[m];
                var before = clause.Substring(0, match.Index);
                var after = clause.Substring(match.Index + match.Length);
                if (!match.Groups[2].Success && !ShortFormLeadExpression.IsMatch(Tail(before, 24))) continue; // Dxgi=false flags are not proxy files.
                if (IsNegated(before, after)) continue;
                var name = match.Groups[1].Value.ToLowerInvariant() + ".dll";
                if (linuxScope) AddUnique(result.LinuxOnly, name, 8);
                else AddUnique(result.Recommended, name, 8);
            }
        }
        static bool IsNegated(string before, string after)
        {
            var window = Tail(before, 48);
            foreach (var cue in BeforeNegativeCues)
                if (window.IndexOf(cue, StringComparison.OrdinalIgnoreCase) >= 0) return true;
            var head = Head(after, 28);
            foreach (var cue in AfterNegativeCues) {
                var at = head.IndexOf(cue, StringComparison.OrdinalIgnoreCase);
                if (at >= 0 && BridgeExpression.IsMatch(head.Substring(0, at))) return true;
            }
            return false;
        }
        static ProxyProfile ProfileFromRow(WikiRow row, WikiProxyAnalysis analysis, HashSet<string> usedIds)
        {
            var profile = new ProxyProfile();
            profile.Id = UniqueId(Slug(row.Title), usedIds);
            profile.Names = new[] { row.Title };
            var segment = SanitizeSegment(row.Title);
            profile.PathSegments = segment.Length > 0 ? new[] { segment } : new string[0];
            ApplyRowGuidance(profile, row, analysis);
            return profile;
        }
        static void ApplyRowGuidance(ProxyProfile profile, WikiRow row, WikiProxyAnalysis analysis)
        {
            profile.Preferred = analysis.Recommended.Count > 0 ? analysis.Recommended[0] : DefaultProxy;
            profile.OptiPatcher = row.OptiPatcher;
            var alternatives = new List<string>(analysis.Recommended.Count);
            for (var i = 1; i < analysis.Recommended.Count && alternatives.Count < 4; i++) alternatives.Add(analysis.Recommended[i]);
            profile.Alternatives = alternatives.ToArray();
            profile.NotesEn = BuildNotesEn(row, analysis);
            profile.NotesZh = BuildNotesZh(row, analysis);
            profile.Source = row.HasLink ? row.PageUrl : WikiCompatibilityListPageUrl;
            profile.SourceFormat = "wiki";
            profile.UpstreamVersion = null;
        }
        static void MergeRowIntoProfile(ProxyProfile target, WikiRow row, WikiProxyAnalysis analysis)
        {
            var names = new List<string>(target.Names);
            if (AddUnique(names, row.Title, 32)) target.Names = names.ToArray();
            var segment = SanitizeSegment(row.Title);
            if (segment.Length > 0) {
                var segments = new List<string>(target.PathSegments);
                if (AddUnique(segments, segment, 32)) target.PathSegments = segments.ToArray();
            }
            target.OptiPatcher = row.OptiPatcher;
            if (string.Equals(target.SourceFormat, "wiki", StringComparison.OrdinalIgnoreCase)) {
                ApplyRowGuidance(target, row, analysis); // Machine-generated entries are fully regenerated.
            } else if (row.HasLink && string.Equals(target.Source, WikiCompatibilityListPageUrl, StringComparison.OrdinalIgnoreCase)) {
                target.Source = row.PageUrl; // Curated records keep their reviewed guidance but adopt the specific wiki page.
            }
        }
        static string BuildNotesEn(WikiRow row, WikiProxyAnalysis analysis)
        {
            var parts = new List<string>(6);
            parts.Add("OptiScaler Wiki: " + (row.Compatibility == PartialMark ? "Partial (see notes)" : "Working"));
            if (row.Inputs.Length > 0) parts.Add("Inputs: " + row.Inputs);
            if (row.OptiPatcher) parts.Add("OptiPatcher supported");
            parts.Add(analysis.Recommended.Count > 0
                ? "Proxy: " + string.Join(" or ", analysis.Recommended.ToArray())
                : "Proxy: default " + DefaultProxy);
            if (analysis.LinuxOnly.Count > 0) parts.Add("Linux proxy: " + string.Join(" or ", analysis.LinuxOnly.ToArray()));
            if (row.LumaSection) parts.Add("Luma UE mod required");
            else if (row.ThirdPartySection) parts.Add("3rd-party upscaler mod required");
            var text = string.Join(" \u00b7 ", parts.ToArray());
            var cleaned = CleanWikiNotes(row.Notes);
            if (cleaned.Length > 0) text = text + ". " + cleaned;
            return Truncate(text, 900);
        }
        static string BuildNotesZh(WikiRow row, WikiProxyAnalysis analysis)
        {
            var parts = new List<string>(6);
            parts.Add("OptiScaler Wiki：" + (row.Compatibility == PartialMark ? "部分可用（详见说明）" : "可用"));
            if (row.Inputs.Length > 0) parts.Add("输入：" + row.Inputs.Replace(", ", "\u3001"));
            if (row.OptiPatcher) parts.Add("支持 OptiPatcher");
            parts.Add(analysis.Recommended.Count > 0
                ? "建议代理：" + string.Join(" 或 ", analysis.Recommended.ToArray())
                : "未记录特殊代理，默认 " + DefaultProxy);
            if (analysis.LinuxOnly.Count > 0) parts.Add("Linux 代理：" + string.Join(" 或 ", analysis.LinuxOnly.ToArray()));
            if (row.LumaSection) parts.Add("需 Luma UE Mod");
            else if (row.ThirdPartySection) parts.Add("需第三方升采样 Mod");
            var text = string.Join(" \u00b7 ", parts.ToArray());
            var cleaned = CleanWikiNotes(row.Notes);
            if (cleaned.Length > 0) text = text + "。" + TranslateToChinese(cleaned);
            return Truncate(text, 900);
        }
        static string CleanWikiNotes(string notes)
        {
            if (string.IsNullOrEmpty(notes)) return "";
            var text = AttachmentExpression.Replace(notes, " ");
            text = LinkExpression.Replace(text, delegate(Match match) { return match.Groups[1].Value; });
            text = text.Replace("**", "").Replace("__", "").Replace("+++", "").Replace("<s>", "").Replace("</s>", "");
            text = text.Replace("<br />", "; ").Replace("<br/>", "; ").Replace("<br>", "; ");
            text = text.Replace("\uD83D\uDC27", "Linux: ");
            text = CollapseWhitespace(text);
            return Truncate(text.Trim(' ', ';', ',', '.', '\u3001', '\uff0c', '\uff1b'), 480);
        }
        static string TranslateToChinese(string text)
        {
            var result = text;
            for (var i = 0; i < ChinesePhrases.GetLength(0); i++)
                result = result.Replace(ChinesePhrases[i, 0], ChinesePhrases[i, 1]);
            return result;
        }
        static void IndexIdentity(Dictionary<string, ProxyProfile> byName, Dictionary<string, ProxyProfile> byStripped, string value, ProxyProfile profile)
        {
            var key = NormalizeIdentity(value);
            if (key.Length > 0 && !byName.ContainsKey(key)) byName.Add(key, profile);
            var stripped = StripEditionSuffix(key);
            if (stripped.Length > 0 && !byStripped.ContainsKey(stripped)) byStripped.Add(stripped, profile);
        }
        static ProxyProfile CloneProfile(ProxyProfile source)
        {
            var copy = new ProxyProfile();
            copy.Id = source.Id;
            copy.Names = (string[])source.Names.Clone();
            copy.PathSegments = source.PathSegments == null ? new string[0] : (string[])source.PathSegments.Clone();
            copy.Preferred = source.Preferred;
            copy.Alternatives = source.Alternatives == null ? new string[0] : (string[])source.Alternatives.Clone();
            copy.NotesEn = source.NotesEn;
            copy.NotesZh = source.NotesZh;
            copy.Source = source.Source;
            copy.SourceFormat = source.SourceFormat;
            copy.UpstreamVersion = source.UpstreamVersion;
            copy.OptiPatcher = source.OptiPatcher;
            return copy;
        }
        static bool AddUnique(List<string> list, string value, int cap)
        {
            if (string.IsNullOrEmpty(value) || list.Count >= cap) return false;
            foreach (var item in list) if (string.Equals(item, value, StringComparison.OrdinalIgnoreCase)) return false;
            list.Add(value);
            return true;
        }
        static string Slug(string title)
        {
            var builder = new StringBuilder();
            foreach (var ch in title.ToLowerInvariant()) if (char.IsLetterOrDigit(ch)) builder.Append(ch);
            var value = builder.ToString();
            return value.Length > 0 ? value : "game";
        }
        static string UniqueId(string baseId, HashSet<string> used)
        {
            if (used.Add(baseId)) return baseId;
            for (var i = 2; ; i++) {
                var candidate = baseId + "-" + i.ToString(CultureInfo.InvariantCulture);
                if (used.Add(candidate)) return candidate;
            }
        }
        static string SanitizeSegment(string title)
        {
            var builder = new StringBuilder(title.Length);
            foreach (var ch in title) {
                if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
                    ch == '<' || ch == '>' || ch == '|' || ch < ' ') continue;
                builder.Append(ch);
            }
            return CollapseWhitespace(builder.ToString());
        }
        static string NormalizeIdentity(string value)
        {
            if (string.IsNullOrEmpty(value)) return "";
            var builder = new StringBuilder(value.Length);
            foreach (var ch in value) if (char.IsLetterOrDigit(ch)) builder.Append(char.ToLowerInvariant(ch));
            return builder.ToString();
        }
        static string StripEditionSuffix(string identity)
        {
            if (string.IsNullOrEmpty(identity)) return "";
            foreach (var suffix in EditionSuffixes)
                if (identity.Length > suffix.Length && identity.EndsWith(suffix, StringComparison.Ordinal))
                    return identity.Substring(0, identity.Length - suffix.Length);
            return identity;
        }
        static string CollapseWhitespace(string value)
        {
            if (string.IsNullOrEmpty(value)) return "";
            var builder = new StringBuilder(value.Length);
            var space = true;
            foreach (var ch in value) {
                if (ch == ' ' || ch == '\t') {
                    if (!space) { builder.Append(' '); space = true; }
                    continue;
                }
                builder.Append(ch);
                space = false;
            }
            return builder.ToString().Trim(' ');
        }
        static string Truncate(string value, int maximum)
        {
            if (string.IsNullOrEmpty(value) || value.Length <= maximum) return value ?? "";
            var cut = value.Substring(0, maximum);
            var space = cut.LastIndexOf(' ');
            if (space > maximum / 2) cut = cut.Substring(0, space);
            return cut.TrimEnd(' ', ';', ',', '.') + "\u2026";
        }
        static string Tail(string value, int count) { return value.Length <= count ? value : value.Substring(value.Length - count); }
        static string Head(string value, int count) { return value.Length <= count ? value : value.Substring(0, count); }

        public async Task<string> RefreshAsync()
        {
            if (_disposed || Interlocked.Exchange(ref _updating, 1) != 0) return "busy";
            var timer = Stopwatch.StartNew();
            var deadline = new CancellationTokenSource();
            var work = Task.Run(() => {
                HttpWebRequest request;
                try {
                    // OPTSCALERNR_WIKI_URL may name a file:// or ftp:// URI that WebRequest maps to another type.
                    request = WebRequest.Create(_url) as HttpWebRequest;
                    if (request != null) {
                        request.UserAgent = "OptScalerNR-Launcher/1";
                        request.Timeout = request.ReadWriteTimeout = DeadlineMs;
                        request.AllowAutoRedirect = false;
                        if (!string.IsNullOrEmpty(_etag)) request.Headers[HttpRequestHeader.IfNoneMatch] = _etag;
                    }
                } catch (Exception) { request = null; }
                if (request == null) return "invalid";
                lock (_gate) {
                    if (_disposed) return "cancelled";
                    _request = request;
                }
                return Download(request, deadline.Token, timer);
            });
            try {
                var completed = await Task.WhenAny(work, Task.Delay(DeadlineMs)).ConfigureAwait(false);
                if (completed != work) {
                    deadline.Cancel();
                    lock (_gate) { if (_request != null) try { _request.Abort(); } catch { } }
                    return "timeout";
                }
                return await work.ConfigureAwait(false);
            } finally {
                deadline.Cancel();
                lock (_gate) _request = null;
                Interlocked.Exchange(ref _updating, 0);
                // The token remains valid for a DNS request that completes after cancellation.
                ReleaseWhenDone(work, deadline);
            }
        }
        static void ReleaseWhenDone(Task work, CancellationTokenSource source)
        { work.ContinueWith(t => { var ignored = t.Exception; source.Dispose(); }, TaskScheduler.Default); }
        static ProxyProfileDocument ParseDownloadedDocument(string text)
        {
            var trimmed = text.TrimStart();
            if (trimmed.Length == 0) throw new InvalidDataException("Empty recommendation payload.");
            if (trimmed[0] == '{') return JsonUtil.Read<ProxyProfileDocument>(text);
            if (trimmed[0] == '#' || text.IndexOf("| Game | Compatibility |", StringComparison.Ordinal) >= 0)
                return ParseWikiCompatibilityMarkdown(text, Seed, null);
            throw new InvalidDataException("Unrecognized recommendation payload.");
        }
        string Download(HttpWebRequest request, CancellationToken token, Stopwatch timer)
        {
            try {
                using (var response = (HttpWebResponse)request.GetResponse()) {
                    if (response.StatusCode == HttpStatusCode.NotModified) return "not-modified:" + CountUnlocked();
                    if (response.StatusCode != HttpStatusCode.OK || response.ContentLength > Limit) return "invalid";
                    using (var input = response.GetResponseStream())
                    using (var output = new MemoryStream()) {
                        var buffer = new byte[8192]; int read;
                        while ((read = input.Read(buffer, 0, buffer.Length)) > 0) {
                            if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= DeadlineMs) return "timeout";
                            if (output.Length + read > Limit) return "invalid";
                            output.Write(buffer, 0, read);
                        }
                        var text = new UTF8Encoding(false, true).GetString(output.ToArray());
                        var doc = ParseDownloadedDocument(text);
                        Validate(doc);
                        lock (_gate) {
                            if (_disposed || token.IsCancellationRequested || timer.ElapsedMilliseconds >= DeadlineMs) return "timeout";
                            if (doc.Revision < _current.Revision) return "not-modified:" + CountUnlocked();
                        }
                        // File I/O must not hold the lock used by UI readers. Serialize writers
                        // across launcher windows and never replace a newer cache with an older response.
                        string mutexName;
                        using (var sha = SHA256.Create()) mutexName = "Local\\OptScalerNR.Profiles." +
                            BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(_directory).ToLowerInvariant()))).Replace("-", "");
                        using (var mutex = new Mutex(false, mutexName)) {
                            bool acquired;
                            try { acquired = mutex.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
                            if (!acquired) return "busy";
                            try {
                            if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= DeadlineMs) return "timeout";
                            Directory.CreateDirectory(_directory);
                            var path = Path.Combine(_directory, "proxy-profiles.json");
                            if (File.Exists(path) && new FileInfo(path).Length <= Limit) {
                                try {
                                    var existing = JsonUtil.Read<ProxyProfileDocument>(File.ReadAllText(path, Encoding.UTF8));
                                    Validate(existing);
                                    if (existing.Revision > doc.Revision) return "not-modified:" + CountOf(existing);
                                } catch { /* A damaged old file can be replaced by validated data. */ }
                            }
                            var payload = JsonUtil.Write(doc);
                            var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                            try {
                                File.WriteAllText(temporary, payload, new UTF8Encoding(false));
                                if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= DeadlineMs) return "timeout";
                                if (File.Exists(path)) File.Replace(temporary, path, null); else File.Move(temporary, path);
                            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
                            var etag = response.Headers[HttpResponseHeader.ETag];
                            if (!string.IsNullOrEmpty(etag) && etag.Length < 512) File.WriteAllText(Path.Combine(_directory, "etag.txt"), etag);
                            lock (_gate) {
                                if (_disposed || token.IsCancellationRequested || timer.ElapsedMilliseconds >= DeadlineMs) return "timeout";
                                _etag = etag; _current = doc; _cached = true;
                            }
                            } finally { mutex.ReleaseMutex(); }
                        }
                        return "updated:" + CountOf(doc);
                    }
                }
            } catch (WebException ex) {
                var response = ex.Response as HttpWebResponse;
                if (response != null) { using (response) if (response.StatusCode == HttpStatusCode.NotModified) return "not-modified:" + CountUnlocked(); }
                return token.IsCancellationRequested ? "timeout" : "offline";
            } catch { return "invalid"; }
        }
        static int CountOf(ProxyProfileDocument doc) { return doc == null || doc.Profiles == null ? 0 : doc.Profiles.Length; }
        public void Dispose()
        {
            lock (_gate) { _disposed = true; if (_request != null) _request.Abort(); }
        }
    }
}
