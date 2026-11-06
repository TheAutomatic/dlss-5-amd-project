using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    static class GamePaths
    {
        const uint InvalidAttributes = 0xFFFFFFFF;
        const uint DirectoryFlag = 0x10;
        const uint ReparseFlag = 0x400;
        const int ErrorAccessDenied = 5;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFileAttributesW(string lpFileName);

        public static bool CanUse(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            uint attr = GetFileAttributesW(path);
            if (attr != InvalidAttributes) return (attr & DirectoryFlag) == 0;
            return Marshal.GetLastWin32Error() == ErrorAccessDenied;
        }

        public static string Normalize(string path)
        {
            if (string.IsNullOrEmpty(path)) return path;
            try { path = Path.GetFullPath(path); }
            catch { return path; }
            if (path.IndexOf("\\WindowsApps\\", StringComparison.OrdinalIgnoreCase) < 0)
                return path;
            var name = Path.GetFileName(path);
            if (string.IsNullOrEmpty(name)) return path;
            var mapped = FindUnderXboxGames(name);
            return mapped ?? path;
        }

        public static string PickExe(bool isZh = false, string initialDirectory = null)
        {
            try
            {
                var dlg = new OpenFileDialog();
                dlg.Title = isZh ? "选择游戏启动程序" : "Select game executable";
                dlg.Filter = isZh ? "游戏启动程序 (*.exe)|*.exe" : "Game executable (*.exe)|*.exe";
                dlg.CheckFileExists = false;
                dlg.CheckPathExists = true;
                dlg.DereferenceLinks = false;
                dlg.AddExtension = false;
                dlg.ValidateNames = false;
                if (!string.IsNullOrEmpty(initialDirectory) && Directory.Exists(initialDirectory))
                {
                    dlg.InitialDirectory = initialDirectory;
                    // Override the shell's remembered location for this specific operation.
                    dlg.FileName = Path.Combine(initialDirectory, "*.exe");
                    dlg.RestoreDirectory = true;
                }
                else if (Directory.Exists(@"C:\XboxGames"))
                    dlg.InitialDirectory = @"C:\XboxGames";
                if (dlg.ShowDialog() != true) return null;
                var fn = dlg.FileName;
                if (string.IsNullOrEmpty(fn)) return null;
                return Normalize(fn);
            }
            catch { return null; }
        }

        static string FindUnderXboxGames(string fileName)
        {
            var hits = new List<string>();
            foreach (var drive in Directory.GetLogicalDrives())
            {
                var root = Path.Combine(drive, "XboxGames");
                uint attr = GetFileAttributesW(root);
                if (attr == InvalidAttributes || (attr & DirectoryFlag) == 0) continue;
                Walk(root, fileName, 0, hits);
                if (hits.Count >= 32) break;
            }
            if (hits.Count == 0) return null;
            hits.Sort(CompareXboxHits);
            return hits[0];
        }

        static int CompareXboxHits(string a, string b)
        {
            int ra = XboxRank(a).CompareTo(XboxRank(b));
            if (ra != 0) return ra;
            return a.Length.CompareTo(b.Length);
        }

        static int XboxRank(string path)
        {
            if (path.IndexOf("\\WinGDK\\", StringComparison.OrdinalIgnoreCase) >= 0) return 0;
            if (path.IndexOf("\\Win64\\", StringComparison.OrdinalIgnoreCase) >= 0) return 1;
            if (path.IndexOf("\\Binaries\\", StringComparison.OrdinalIgnoreCase) >= 0) return 2;
            return 3;
        }

        static void Walk(string dir, string fileName, int depth, List<string> hits)
        {
            if (depth > 8 || hits.Count >= 32) return;
            string[] entries;
            try { entries = Directory.GetFileSystemEntries(dir); }
            catch { return; }
            foreach (var entry in entries)
            {
                uint attr = GetFileAttributesW(entry);
                if (attr == InvalidAttributes) continue;
                bool isDir = (attr & DirectoryFlag) != 0;
                bool reparse = (attr & ReparseFlag) != 0;
                if (isDir)
                {
                    if (reparse) continue;
                    Walk(entry, fileName, depth + 1, hits);
                    continue;
                }
                if (string.Equals(Path.GetFileName(entry), fileName, StringComparison.OrdinalIgnoreCase))
                    hits.Add(entry);
            }
        }
    }

    public static class PeProbe
    {
        static readonly string[] CandidateDlls = ProxyRecommendations.ProxyNames;
        static readonly Dictionary<string, Tuple<long, long, bool>> OptiDllCache =
            new Dictionary<string, Tuple<long, long, bool>>(StringComparer.OrdinalIgnoreCase);
        static readonly Dictionary<string, Tuple<long, long, HashSet<string>>> ExeImportCache =
            new Dictionary<string, Tuple<long, long, HashSet<string>>>(StringComparer.OrdinalIgnoreCase);

        static bool IsOptiScalerDll(string path)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists) return false;
                var full = info.FullName;
                var len = info.Length;
                var ticks = info.LastWriteTimeUtc.Ticks;
                lock (OptiDllCache)
                {
                    Tuple<long, long, bool> cached;
                    if (OptiDllCache.TryGetValue(full, out cached) && cached.Item1 == len && cached.Item2 == ticks)
                        return cached.Item3;
                }
                var vi = FileVersionInfo.GetVersionInfo(full);
                var isOpti = vi != null && (vi.ProductName == "OptiScaler" || vi.FileDescription == "OptiScaler");
                lock (OptiDllCache)
                {
                    if (OptiDllCache.Count > 128) OptiDllCache.Clear();
                    OptiDllCache[full] = Tuple.Create(len, ticks, isOpti);
                }
                return isOpti;
            }
            catch { return false; }
        }

        public static string QuickRecommend(string exePath)
        {
            if (string.IsNullOrEmpty(exePath) || !File.Exists(exePath)) return null;
            try
            {
                var dir = Path.GetDirectoryName(exePath);
                if (!string.IsNullOrEmpty(dir))
                {
                    foreach (var c in CandidateDlls)
                    {
                        var p = Path.Combine(dir, c);
                        if (IsOptiScalerDll(p)) return c;
                    }
                }

                HashSet<string> imports = null;
                var exeInfo = new FileInfo(exePath);
                var exeFull = exeInfo.FullName;
                var exeLen = exeInfo.Length;
                var exeTicks = exeInfo.LastWriteTimeUtc.Ticks;
                lock (ExeImportCache)
                {
                    Tuple<long, long, HashSet<string>> cached;
                    if (ExeImportCache.TryGetValue(exeFull, out cached) && cached.Item1 == exeLen && cached.Item2 == exeTicks)
                        imports = cached.Item3;
                }
                if (imports == null)
                {
                    imports = ReadImports(exeFull);
                    lock (ExeImportCache)
                    {
                        if (ExeImportCache.Count > 64) ExeImportCache.Clear();
                        ExeImportCache[exeFull] = Tuple.Create(exeLen, exeTicks, imports);
                    }
                }
                if (imports != null)
                {
                    foreach (var c in CandidateDlls)
                    {
                        if (imports.Contains(c)) return c;
                    }
                }
            }
            catch { }
            return null;
        }

        static HashSet<string> ReadImports(string exePath)
        {
            var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (var fs = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var br = new BinaryReader(fs))
            {
                if (fs.Length < 0x200) return null;
                if (br.ReadUInt16() != 0x5A4D) return null;
                fs.Seek(0x3C, SeekOrigin.Begin);
                var peOffset = br.ReadUInt32();
                if (peOffset + 0x100 > fs.Length) return null;
                fs.Seek(peOffset, SeekOrigin.Begin);
                if (br.ReadUInt32() != 0x00004550) return null;

                var machine = br.ReadUInt16();
                var numSections = br.ReadUInt16();
                fs.Seek(12, SeekOrigin.Current);
                var optHeaderSize = br.ReadUInt16();
                fs.Seek(2, SeekOrigin.Current);

                var optStart = fs.Position;
                var magic = br.ReadUInt16();
                if (magic != 0x20B) return null;

                fs.Seek(optStart + 112, SeekOrigin.Begin);
                var importRva = br.ReadUInt32();
                var importSize = br.ReadUInt32();
                if (importRva == 0 || importSize == 0) return result;

                fs.Seek(optStart + optHeaderSize, SeekOrigin.Begin);
                var sections = new List<PeSection>(numSections);
                for (int i = 0; i < numSections; i++)
                {
                    fs.Seek(8, SeekOrigin.Current);
                    var vSize = br.ReadUInt32();
                    var va = br.ReadUInt32();
                    var rawSize = br.ReadUInt32();
                    var rawPtr = br.ReadUInt32();
                    fs.Seek(16, SeekOrigin.Current);
                    sections.Add(new PeSection { VA = va, VSize = Math.Max(vSize, rawSize), Raw = rawPtr });
                }

                long importFileOffset = RvaToOffset(sections, importRva);
                if (importFileOffset < 0 || importFileOffset >= fs.Length) return result;

                fs.Seek(importFileOffset, SeekOrigin.Begin);
                for (int i = 0; i < 256; i++)
                {
                    if (fs.Position + 20 > fs.Length) break;
                    var originalFirstThunk = br.ReadUInt32();
                    var timeDateStamp = br.ReadUInt32();
                    var forwarderChain = br.ReadUInt32();
                    var nameRva = br.ReadUInt32();
                    var firstThunk = br.ReadUInt32();

                    if (nameRva == 0 && firstThunk == 0) break;

                    long nameOffset = RvaToOffset(sections, nameRva);
                    if (nameOffset > 0 && nameOffset < fs.Length)
                    {
                        var curPos = fs.Position;
                        fs.Seek(nameOffset, SeekOrigin.Begin);
                        var sb = new System.Text.StringBuilder();
                        int b;
                        while ((b = fs.ReadByte()) > 0 && sb.Length < 128)
                        {
                            sb.Append((char)b);
                        }
                        result.Add(sb.ToString().ToLowerInvariant());
                        fs.Seek(curPos, SeekOrigin.Begin);
                    }
                }
            }
            return result;
        }

        static long RvaToOffset(List<PeSection> sections, uint rva)
        {
            foreach (var s in sections)
            {
                if (rva >= s.VA && rva < s.VA + s.VSize)
                {
                    return s.Raw + (rva - s.VA);
                }
            }
            return -1;
        }

        struct PeSection
        {
            public uint VA;
            public uint VSize;
            public uint Raw;
        }
    }
}
