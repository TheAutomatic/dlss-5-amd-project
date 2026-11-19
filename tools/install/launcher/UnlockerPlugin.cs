using System;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace OptScalerNR.Launcher
{
    internal static class UnlockerPlugin
    {
        public static string DestinationName(string source)
        {
            var extension = Path.GetExtension(source);
            if (!string.Equals(extension, ".asi", StringComparison.OrdinalIgnoreCase) && !string.Equals(extension, ".dll", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Select an .asi or .dll plugin.");
            return string.Equals(extension, ".dll", StringComparison.OrdinalIgnoreCase) ? Path.GetFileNameWithoutExtension(source) + ".asi" : Path.GetFileName(source);
        }

        public static void ValidatePe(string path)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var reader = new BinaryReader(stream))
            {
                if (stream.Length < 4096 || stream.Length > 100L * 1024 * 1024 || reader.ReadUInt16() != 0x5a4d)
                    throw new InvalidDataException("Expected a 4 KB–100 MB AMD64 PE plugin (MZ / PE signatures).");
                stream.Position = 0x3c;
                var offset = reader.ReadInt32();
                if (offset < 0x40 || offset > stream.Length - 24)
                    throw new InvalidDataException("Invalid PE header offset.");
                stream.Position = offset;
                if (reader.ReadUInt32() != 0x00004550 || reader.ReadUInt16() != 0x8664)
                    throw new InvalidDataException("Expected an AMD64 PE plugin (PE signature and machine 0x8664).");
            }
        }

        public static string ReadIni(string path)
        {
            return File.Exists(path) ? IniProfiles.Document.Read(File.ReadAllBytes(path)).Text : "";
        }

        // Match the installer's first-section / first-assignment path lookup.
        public static string Setting(string text, string sectionName, string key)
        {
            var inside = false;
            foreach (var line in Regex.Split(text, "\\r\\n|\\r|\\n"))
            {
                var section = Regex.Match(line, @"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$");
                if (section.Success)
                {
                    if (inside) break;
                    inside = string.Equals(section.Groups[1].Value.Trim(), sectionName, StringComparison.OrdinalIgnoreCase);
                }
                else if (inside)
                {
                    var match = Regex.Match(line, @"^\s*" + Regex.Escape(key) + @"\s*=(.*)$", RegexOptions.IgnoreCase);
                    if (match.Success) return match.Groups[1].Value.Trim();
                }
            }
            return "";
        }

        public static string LoadAsiValue(string text)
        {
            var value = Setting(text, "Plugins", "LoadAsiPlugins");
            var comment = value.IndexOfAny(new[] { ';', '#' });
            return (comment >= 0 ? value.Substring(0, comment) : value).Trim();
        }

        public static string DestinationDirectory(string gameDirectory, string iniText)
        {
            var fallback = Path.Combine(gameDirectory, "OptiScaler", "plugins");
            try
            {
                var configured = Setting(iniText, "Plugins", "Path");
                if (configured.Length > 0 && !string.Equals(configured, "auto", StringComparison.OrdinalIgnoreCase))
                {
                    var target = Path.GetFullPath(Path.IsPathRooted(configured) ? configured : Path.Combine(gameDirectory, configured));
                    return File.Exists(target) ? fallback : target;
                }
                var main = Setting(iniText, "Libraries", "OptiDllPath");
                if (main.Length == 0 || string.Equals(main, "auto", StringComparison.OrdinalIgnoreCase)) main = "OptiScaler";
                main = Path.GetFullPath(Path.IsPathRooted(main) ? main : Path.Combine(gameDirectory, main));
                if (!Directory.Exists(main)) main = gameDirectory;
                return Path.Combine(main, "plugins");
            }
            catch (ArgumentException) { return fallback; }
            catch (NotSupportedException) { return fallback; }
            catch (PathTooLongException) { return fallback; }
        }

        public static bool Identical(string first, string second)
        {
            using (var hash = SHA256.Create())
            using (var a = File.OpenRead(first))
            using (var b = File.OpenRead(second))
            {
                if (a.Length != b.Length) return false;
                var left = hash.ComputeHash(a);
                var right = hash.ComputeHash(b);
                for (var i = 0; i < left.Length; i++) if (left[i] != right[i]) return false;
                return true;
            }
        }

        static string BackupName(string target)
        {
            var name = target + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
            var candidate = name;
            for (var i = 2; File.Exists(candidate); i++) candidate = name + "-" + i.ToString(CultureInfo.InvariantCulture);
            return candidate;
        }

        // A sibling temporary file guarantees a same-volume atomic replacement. Never delete/copy over a live target.
        public static string CopyAtomic(string source, string target, bool replace)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            var temporary = target + ".stage-" + Guid.NewGuid().ToString("N");
            try
            {
                using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    input.CopyTo(output);
                    output.Flush(true);
                }
                ValidatePe(temporary);
                if (!replace) { File.Move(temporary, target); return ""; }
                var backup = BackupName(target);
                File.Copy(target, backup, false);
                File.Replace(temporary, target, null);
                return backup;
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        public static string EnableAsi(string iniPath)
        {
            var bytes = File.Exists(iniPath) ? File.ReadAllBytes(iniPath) : new byte[0];
            var updated = IniProfiles.Document.Read(bytes).WithPlugins(true);
            var temporary = iniPath + ".stage-" + Guid.NewGuid().ToString("N");
            try
            {
                using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    output.Write(updated, 0, updated.Length);
                    output.Flush(true);
                }
                if (!File.Exists(iniPath)) { File.Move(temporary, iniPath); return ""; }
                var backup = BackupName(iniPath);
                File.Replace(temporary, iniPath, backup);
                return backup;
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        public static bool HasMultipleUnlockers(string directory)
        {
            var count = 0;
            foreach (var file in Directory.GetFiles(directory, "*.asi", SearchOption.TopDirectoryOnly))
                if (Regex.IsMatch(Path.GetFileName(file), @"(mfg.*unlock|xefg.*unlock).*\.asi$", RegexOptions.IgnoreCase) && ++count >= 2) return true;
            return false;
        }
    }
}
