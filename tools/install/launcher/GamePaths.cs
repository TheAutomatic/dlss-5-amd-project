using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    static class GamePaths
    {
        const uint InvalidAttributes = 0xFFFFFFFF;
        const uint DirectoryFlag = 0x10;
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

        public static bool IsWindowsAppsPath(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            try { path = Path.GetFullPath(path); }
            catch { return false; }
            return (path + @"\").IndexOf(@"\WindowsApps\", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        public static string Normalize(string path)
        {
            if (string.IsNullOrEmpty(path) || IsWindowsAppsPath(path)) return null;
            try { return Path.GetFullPath(path); }
            catch { return null; }
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
                return dlg.FileName;
            }
            catch { return null; }
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
                int importOffset;
                if (machine == 0x014C && magic == 0x10B) importOffset = 104;
                else if (machine == 0x8664 && magic == 0x20B) importOffset = 120;
                else return null;
                if (optHeaderSize < importOffset + 8) return null;
                fs.Seek(optStart + importOffset, SeekOrigin.Begin);
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

        public static bool IsRtxRemixMinecraftDir(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return false;
            try
            {
                if (!Directory.Exists(dir)) return false;
                if (!File.Exists(Path.Combine(dir, "remixbridge.dll"))) return false;
                return File.Exists(Path.Combine(dir, "d3d9.dll")) ||
                       File.Exists(Path.Combine(dir, "rtxio.dll")) ||
                       File.Exists(Path.Combine(dir, "NRD.dll")) ||
                       File.Exists(Path.Combine(dir, "NRC_Vulkan.dll"));
            }
            catch { return false; }
        }

        public static bool IsRtxRemixMinecraftExe(string exePath)
        {
            if (string.IsNullOrEmpty(exePath)) return false;
            try
            {
                if (!string.Equals(Path.GetFileName(exePath), "remixbridge.dll", StringComparison.OrdinalIgnoreCase))
                    return false;
                return IsRtxRemixMinecraftDir(Path.GetDirectoryName(exePath));
            }
            catch { return false; }
        }

        public static string ResolveRtxRemixSdkDir(string pickedDir)
        {
            if (string.IsNullOrEmpty(pickedDir)) return null;
            try
            {
                var full = Path.GetFullPath(pickedDir).TrimEnd('\\', '/');
                if (!Directory.Exists(full)) return null;
                if (IsRtxRemixMinecraftDir(full)) return full;
                var candidates = new[]
                {
                    Path.Combine(full, "remix-sdk"),
                    Path.Combine(Path.Combine(full, ".minecraft"), "remix-sdk"),
                    Path.Combine(Path.Combine(full, "minecraft"), "remix-sdk")
                };
                foreach (var c in candidates)
                {
                    if (IsRtxRemixMinecraftDir(c)) return c;
                }
            }
            catch { }
            return null;
        }

        public static string TryFindPrismInstanceDir(string sdkDir)
        {
            if (string.IsNullOrEmpty(sdkDir)) return null;
            try
            {
                var sdkFull = Path.GetFullPath(sdkDir).TrimEnd('\\', '/');
                if (IsRtxRemixMinecraftDir(sdkFull))
                {
                    var mcDir = Path.GetDirectoryName(sdkFull);
                    if (!string.IsNullOrEmpty(mcDir))
                    {
                        var mcLeaf = Path.GetFileName(mcDir);
                        var instDir = Path.GetDirectoryName(mcDir);
                        if (!string.IsNullOrEmpty(instDir) &&
                            (string.Equals(mcLeaf, ".minecraft", StringComparison.OrdinalIgnoreCase) ||
                             string.Equals(mcLeaf, "minecraft", StringComparison.OrdinalIgnoreCase)))
                        {
                            return instDir;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        public static string TryReadPrismInstanceName(string sdkDir)
        {
            try
            {
                var instDir = TryFindPrismInstanceDir(sdkDir);
                if (string.IsNullOrEmpty(instDir)) return null;
                var cfg = Path.Combine(instDir, "instance.cfg");
                if (!File.Exists(cfg)) return null;
                foreach (var line in File.ReadAllLines(cfg, Encoding.UTF8))
                {
                    if (line == null) continue;
                    var trimmed = line.Trim();
                    if (trimmed.StartsWith("name=", StringComparison.OrdinalIgnoreCase))
                    {
                        var val = trimmed.Substring(5).Trim();
                        if (!string.IsNullOrEmpty(val)) return val;
                    }
                }
            }
            catch { }
            return null;
        }

        public static string TryFindPrismLauncherExe(string sdkDir, out string instanceDirName)
        {
            instanceDirName = null;
            try
            {
                var instDir = TryFindPrismInstanceDir(sdkDir);
                if (string.IsNullOrEmpty(instDir)) return null;
                instanceDirName = Path.GetFileName(instDir.TrimEnd('\\', '/'));
                var instancesDir = Path.GetDirectoryName(instDir);
                var launcherDir = string.IsNullOrEmpty(instancesDir) ? null : Path.GetDirectoryName(instancesDir);
                if (!string.IsNullOrEmpty(launcherDir))
                {
                    var exeCand = Path.Combine(launcherDir, "prismlauncher.exe");
                    if (File.Exists(exeCand)) return exeCand;
                }
            }
            catch { }
            return null;
        }

        public static PeTargetInfo AnalyzeTarget(string exePath)
        {
            var info = new PeTargetInfo
            {
                RecommendedMode = "optiscaler",
                RecommendedReshadeApi = "reshade-feeder",
                DetectedApi = "Unknown"
            };
            if (string.IsNullOrEmpty(exePath)) return info;
            try
            {
                var targetExe = exePath;
                var dir = Directory.Exists(exePath) ? exePath : Path.GetDirectoryName(exePath);
                if (IsRtxRemixMinecraftDir(dir))
                {
                    info.HasUpscaler = true;
                    info.DetectedApi = "Vulkan (RTX Remix)";
                    info.RecommendedMode = "rtx-remix-mc";
                    info.InstalledMode = DetectInstalledMode(dir);
                    return info;
                }
                if (Directory.Exists(exePath))
                {
                    targetExe = FindPrimaryExeInDir(exePath);
                }
                if (!string.IsNullOrEmpty(targetExe) && File.Exists(targetExe))
                {
                    var leaf = Path.GetFileName(targetExe) ?? "";
                    var leafLower = leaf.ToLowerInvariant();
                    var fullLower = targetExe.ToLowerInvariant();
                    if (leafLower == "obs64.exe" || leafLower == "mpv.exe" || leafLower == "potplayermini64.exe" ||
                        leafLower == "potplayer64.exe" || leafLower == "magpie.exe" || leafLower == "mpc-hc64.exe" ||
                        leafLower == "mpc-be64.exe" || leafLower == "vlc.exe" || leafLower == "losslessscaling.exe" ||
                        fullLower.IndexOf("\\obs-studio\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\potplayer\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\magpie\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\mpv\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\mpc-hc\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\mpc-be\\", StringComparison.Ordinal) >= 0 ||
                        fullLower.IndexOf("\\vlc\\", StringComparison.Ordinal) >= 0)
                    {
                        info.IsKnownStandaloneApp = true;
                    }

                    HashSet<string> imports = ReadImportsCached(targetExe);
                    bool hasD3d9 = false, hasD3d11 = false, hasD3d12 = false, hasDxgi = false, hasVulkan = false;
                    if (imports != null)
                    {
                        foreach (var rawImp in imports)
                        {
                            var imp = Path.GetFileName(rawImp ?? "").ToLowerInvariant();
                            if (imp == "d3d9.dll") hasD3d9 = true;
                            else if (imp == "d3d11.dll") hasD3d11 = true;
                            else if (imp == "d3d12.dll") hasD3d12 = true;
                            else if (imp == "dxgi.dll") hasDxgi = true;
                            else if (imp == "vulkan-1.dll") hasVulkan = true;
                            if (IsUpscalerDllName(imp)) info.HasUpscaler = true;
                        }
                    }

                    if (!info.HasUpscaler && !string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                    {
                        try
                        {
                            foreach (var dllPath in Directory.GetFiles(dir, "*.dll"))
                            {
                                if (IsUpscalerDllName(Path.GetFileName(dllPath)))
                                {
                                    info.HasUpscaler = true;
                                    break;
                                }
                            }
                        }
                        catch { }
                    }

                    if (hasVulkan && !(hasD3d11 || hasD3d12 || hasDxgi))
                    {
                        info.DetectedApi = "Vulkan";
                        info.RecommendedReshadeApi = "reshade-feeder-vulkan";
                    }
                    else if (hasD3d9 && !(hasD3d11 || hasD3d12 || hasDxgi || hasVulkan))
                    {
                        info.DetectedApi = "DX9";
                        info.RecommendedReshadeApi = "reshade-feeder-dx9";
                    }
                    else if (hasD3d11 || hasD3d12 || hasDxgi)
                    {
                        info.DetectedApi = "DX11/12";
                        info.RecommendedReshadeApi = "reshade-feeder";
                    }
                    else if (hasVulkan)
                    {
                        info.DetectedApi = "Vulkan";
                        info.RecommendedReshadeApi = "reshade-feeder-vulkan";
                    }

                    if (info.IsKnownStandaloneApp)
                        info.RecommendedMode = "standalone";
                    else if (hasD3d9 && !(hasD3d11 || hasD3d12 || hasDxgi || hasVulkan || info.HasUpscaler))
                        info.RecommendedMode = "reshade";
                    else
                        info.RecommendedMode = "optiscaler";
                }

                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                    info.InstalledMode = DetectInstalledMode(dir);
            }
            catch { }
            return info;
        }

        public static string DetectInstalledMode(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return null;
            try
            {
                var mark = Path.Combine(dir, "amd-presr-install.txt");
                if (File.Exists(mark))
                {
                    foreach (var raw in File.ReadAllLines(mark, Encoding.UTF8))
                    {
                        var line = (raw ?? "").TrimStart('\uFEFF').Trim();
                        if (line.StartsWith("installMode=", StringComparison.OrdinalIgnoreCase))
                        {
                            var val = line.Substring("installMode=".Length).Trim().ToLowerInvariant();
                            if (val == "optiscaler" || val == "rtx-remix-mc" || val == "standalone" ||
                                val == "reshade-feeder" || val == "reshade-feeder-dx9" || val == "reshade-feeder-vulkan")
                                return val;
                        }
                    }
                }
                if (IsRtxRemixMinecraftDir(dir) && IsOptiScalerDll(Path.Combine(dir, "OptiScaler.dll")))
                    return "rtx-remix-mc";
                if (File.Exists(Path.Combine(dir, "dlssnr_amd.addon64")) || File.Exists(Path.Combine(dir, "dlssnr_amd.addon32")))
                {
                    if (File.Exists(Path.Combine(dir, "d3d9.dll"))) return "reshade-feeder-dx9";
                    if (File.Exists(Path.Combine(dir, "vulkan-1.dll")) || File.Exists(Path.Combine(dir, "ReShade64.dll")) ||
                        File.Exists(Path.Combine(dir, "ReShade32.dll")))
                        return "reshade-feeder-vulkan";
                    return "reshade-feeder";
                }
                if (File.Exists(Path.Combine(dir, "dlssnr-amd.asi")) &&
                    File.Exists(Path.Combine(dir, @"vk-override\implicit_layer\dlssnr_standalone.json")) &&
                    !File.Exists(Path.Combine(dir, "ReShade64.dll")) &&
                    !File.Exists(Path.Combine(dir, "ReShade32.dll")))
                    return "standalone";
                foreach (var c in CandidateDlls)
                {
                    if (IsOptiScalerDll(Path.Combine(dir, c))) return "optiscaler";
                }
            }
            catch { }
            return null;
        }

        static HashSet<string> ReadImportsCached(string exePath)
        {
            var exeInfo = new FileInfo(exePath);
            var exeFull = exeInfo.FullName;
            var exeLen = exeInfo.Length;
            var exeTicks = exeInfo.LastWriteTimeUtc.Ticks;
            HashSet<string> imports = null;
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
            return imports;
        }

        public static int ReadGameBitness(string exePath)
        {
            try
            {
                using (var fs = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var br = new BinaryReader(fs))
                {
                    if (fs.Length < 64 || br.ReadUInt16() != 0x5A4D) return 0;
                    fs.Seek(0x3C, SeekOrigin.Begin);
                    var peOffset = br.ReadUInt32();
                    if (peOffset < 64 || peOffset > fs.Length - 26) return 0;
                    fs.Seek(peOffset, SeekOrigin.Begin);
                    if (br.ReadUInt32() != 0x00004550) return 0;
                    var machine = br.ReadUInt16();
                    fs.Position = peOffset + 20L;
                    var optionalSize = br.ReadUInt16();
                    if (peOffset + 24L + optionalSize > fs.Length || optionalSize < 2) return 0;
                    fs.Position = peOffset + 24L;
                    var magic = br.ReadUInt16();
                    if (machine == 0x014C && magic == 0x10B && optionalSize >= 96) return 32;
                    return machine == 0x8664 && magic == 0x20B && optionalSize >= 112 ? 64 : 0;
                }
            }
            catch { return 0; }
        }

        static string FindPrimaryExeInDir(string dir)
        {
            try
            {
                foreach (var f in Directory.GetFiles(dir, "*.exe"))
                {
                    var name = Path.GetFileName(f).ToLowerInvariant();
                    if (name.StartsWith("unins", StringComparison.Ordinal) ||
                        name == "nr_present_probe.exe" ||
                        name.IndexOf("crash", StringComparison.Ordinal) >= 0)
                        continue;
                    return f;
                }
            }
            catch { }
            return null;
        }

        static bool IsUpscalerDllName(string lowerLeaf)
        {
            if (string.IsNullOrEmpty(lowerLeaf)) return false;
            return lowerLeaf.StartsWith("nvngx_dlss", StringComparison.Ordinal) ||
                   lowerLeaf.StartsWith("sl.interposer", StringComparison.Ordinal) ||
                   lowerLeaf.StartsWith("libxess", StringComparison.Ordinal) ||
                   lowerLeaf.StartsWith("amd_fidelityfx_", StringComparison.Ordinal) ||
                   lowerLeaf.StartsWith("ffx_fsr", StringComparison.Ordinal) ||
                   lowerLeaf.StartsWith("ffx_backend_", StringComparison.Ordinal);
        }
    }

    public sealed class PeTargetInfo
    {
        public bool HasUpscaler;
        public string DetectedApi;
        public bool IsKnownStandaloneApp;
        public string RecommendedMode;
        public string RecommendedReshadeApi;
        public string InstalledMode;
    }

    public static class DlssnrLocalData
    {
        public static bool IsCurrentProcessElevated()
        {
            try
            {
                using (var id = WindowsIdentity.GetCurrent())
                {
                    return id != null && new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
                }
            }
            catch { return false; }
        }

        public static bool IsProtectedDirectory(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return false;
            var probe = Path.Combine(dir, ".nr-access-probe-" + Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                File.WriteAllText(probe, "ok");
                try { File.Delete(probe); } catch { }
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return true;
            }
            catch (SecurityException)
            {
                return true;
            }
            catch
            {
                return false;
            }
        }

        // Mirrors nr_path_win.hpp: the process names the tag, the preferred
        // write directory supplies its hash. Both operate on UTF-8 bytes.
        public static string ComputeAppStorageTag(string exePath, string preferredDir)
        {
            var stemRaw = Path.GetFileNameWithoutExtension(exePath) ?? "";
            var norm = preferredDir.Replace('/', '\\');
            while (norm.Length > 1 && norm[norm.Length - 1] == '\\' && !(norm.Length == 3 && norm[1] == ':'))
                norm = norm.Substring(0, norm.Length - 1);

            uint h = 2166136261u;
            var bytes = Encoding.UTF8.GetBytes(norm);
            for (int i = 0; i < bytes.Length; i++)
            {
                byte b = bytes[i];
                if (b >= (byte)'A' && b <= (byte)'Z') b = (byte)(b + 32);
                h ^= b;
                h *= 16777619u;
            }
            var sb = new StringBuilder();
            foreach (byte value in Encoding.UTF8.GetBytes(stemRaw))
            {
                if (sb.Length == 32) break;
                char c = (char)value;
                bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
                sb.Append(ok ? c : '_');
            }
            var stem = sb.Length > 0 ? sb.ToString() : "app";
            return stem + "_" + h.ToString("x8");
        }

        public static string ResolveAppDir(string gameDir, string exePath)
        {
            try
            {
                if (string.IsNullOrEmpty(gameDir) || string.IsNullOrEmpty(exePath)) return null;
                var localApp = Environment.GetEnvironmentVariable("LOCALAPPDATA");
                if (string.IsNullOrEmpty(localApp)) localApp = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(localApp)) return null;
                return Path.Combine(localApp, @"DLSSNR-AMD\apps", ComputeAppStorageTag(exePath, Path.GetFullPath(gameDir)));
            }
            catch { return null; }
        }

        public static string ResolveActiveDlssnrIni(string gameDir, string exePath)
        {
            var appDir = ResolveAppDir(gameDir, exePath);
            var appIni = !string.IsNullOrEmpty(appDir) ? Path.Combine(appDir, "dlssnr-amd.ini") : null;
            var gameIni = !string.IsNullOrEmpty(gameDir) ? Path.Combine(gameDir, "dlssnr-amd.ini") : null;
            if (!string.IsNullOrEmpty(gameDir) && IsProtectedDirectory(gameDir))
            {
                if (!string.IsNullOrEmpty(appIni) && File.Exists(appIni)) return appIni;
                return gameIni ?? appIni;
            }
            if (!string.IsNullOrEmpty(gameIni) && File.Exists(gameIni)) return gameIni;
            if (!string.IsNullOrEmpty(appIni) && File.Exists(appIni)) return appIni;
            return gameIni;
        }

        public static string[] GetModelReuseDirectories()
        {
            var list = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            Action<string> add = delegate(string p)
            {
                if (string.IsNullOrEmpty(p) || !Directory.Exists(p)) return;
                if (seen.Add(p)) list.Add(p);
            };
            try
            {
                var localApp = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(localApp)) return list.ToArray();
                var baseDir = Path.Combine(localApp, "DLSSNR-AMD");
                add(Path.Combine(baseDir, "models"));
                add(Path.Combine(baseDir, "dlssnr-amd"));
                add(baseDir);
                var appsDir = Path.Combine(baseDir, "apps");
                if (Directory.Exists(appsDir))
                {
                    foreach (var sub in Directory.GetDirectories(appsDir))
                    {
                        add(sub);
                        add(Path.Combine(sub, "dlssnr-amd"));
                    }
                }
            }
            catch { }
            return list.ToArray();
        }
    }
}
