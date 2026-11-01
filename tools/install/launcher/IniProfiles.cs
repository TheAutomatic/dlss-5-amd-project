using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.Serialization;
using System.Text;
using System.Text.RegularExpressions;

namespace OptScalerNR.Launcher
{
    public sealed class IniProfileException : IOException
    {
        public string Code { get; private set; }
        public IniProfileException(string code) : base(code) { Code = code; }
    }

    [DataContract]
    public sealed class IniProfileMetadata
    {
        [DataMember(Name = "selectedByGame")] public Dictionary<string, string> SelectedByGame = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    // Independent of launcher settings: constructing or reading this store never writes preferences.
    public sealed class IniProfiles
    {
        IniProfileMetadata _metadata;
        readonly string _settingsDirectory;
        public string RootDirectory { get; private set; }
        public string MetadataPath { get; private set; }

        public IniProfiles(string settingsDirectory)
        {
            _settingsDirectory = Path.GetFullPath(settingsDirectory);
            var appRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR");
            var defaultSettings = Path.Combine(appRoot, "Launcher");
            RootDirectory = string.Equals(_settingsDirectory.TrimEnd('\\', '/'), Path.GetFullPath(defaultSettings).TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase)
                ? Path.Combine(appRoot, "ini-profiles") : Path.Combine(_settingsDirectory, "ini-profiles");
            MetadataPath = Path.Combine(_settingsDirectory, "ini-profiles.json");
            _metadata = File.Exists(MetadataPath) ? JsonUtil.Read<IniProfileMetadata>(File.ReadAllText(MetadataPath, Encoding.UTF8)) : new IniProfileMetadata();
            if (_metadata == null) throw new IniProfileException("metadata");
            _metadata.SelectedByGame = _metadata.SelectedByGame == null ? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                : new Dictionary<string, string>(_metadata.SelectedByGame, StringComparer.OrdinalIgnoreCase);
        }

        public string[] List()
        {
            if (!Directory.Exists(RootDirectory)) return new string[0];
            var files = new DirectoryInfo(RootDirectory).GetFiles("*.ini", SearchOption.TopDirectoryOnly);
            Array.Sort(files, delegate(FileInfo a, FileInfo b) {
                var order = b.LastWriteTimeUtc.CompareTo(a.LastWriteTimeUtc);
                return order != 0 ? order : StringComparer.OrdinalIgnoreCase.Compare(a.Name, b.Name);
            });
            var names = new List<string>();
            foreach (var file in files)
                if ((file.Attributes & FileAttributes.ReparsePoint) == 0) names.Add(Path.GetFileNameWithoutExtension(file.Name));
            return names.ToArray();
        }

        public string GetSelection(string gameKey)
        {
            string value;
            return !string.IsNullOrEmpty(gameKey) && _metadata.SelectedByGame.TryGetValue(gameKey, out value) ? value ?? "" : "";
        }
        public void SetSelection(string gameKey, string name)
        {
            if (string.IsNullOrEmpty(gameKey)) throw new IniProfileException("game");
            if (!string.IsNullOrEmpty(name)) ExistingProfile(name);
            if (GetSelection(gameKey) == (name ?? "")) return;
            var updated = CopyMetadata();
            updated.SelectedByGame[gameKey] = name ?? "";
            Persist(updated);
        }
        public static string CleanName(string name)
        {
            var value = Regex.Replace((name ?? "").Trim(), "[\\\\/:*?\"<>|\\x00-\\x1f]", "_");
            value = Regex.Replace(value, @"\s+", " ");
            if (value.Length > 80) value = value.Substring(0, 80);
            value = value.Trim('.', ' ');
            if (value.Length == 0) throw new IniProfileException("name");
            var device = value.Split('.')[0];
            if (Regex.IsMatch(device, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$", RegexOptions.IgnoreCase)) value = "_" + value;
            return value;
        }

        string ProfilePath(string name)
        {
            if (string.IsNullOrWhiteSpace(name) || name != Path.GetFileName(name) || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.EndsWith(".") || name.EndsWith(" "))
                throw new IniProfileException("name");
            return Path.Combine(RootDirectory, name + ".ini");
        }
        string ExistingProfile(string name)
        {
            var path = ProfilePath(name);
            if (!File.Exists(path)) throw new IniProfileException("missing");
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IniProfileException("invalid");
            return path;
        }

        public string Save(string sourceIni, string name)
        {
            if (!File.Exists(sourceIni)) throw new IniProfileException("noini");
            return Add(File.ReadAllBytes(sourceIni), name);
        }
        public string Import(string sourceIni, string name)
        {
            if (!string.Equals(Path.GetExtension(sourceIni), ".ini", StringComparison.OrdinalIgnoreCase)) throw new IniProfileException("extension");
            if (!File.Exists(sourceIni)) throw new IniProfileException("source");
            var bytes = File.ReadAllBytes(sourceIni);
            Validate(Document.Read(bytes).Text);
            return Add(bytes, string.IsNullOrWhiteSpace(name) ? Path.GetFileNameWithoutExtension(sourceIni) : name);
        }
        string Add(byte[] bytes, string name)
        {
            var clean = CleanName(name);
            Directory.CreateDirectory(RootDirectory);
            var candidate = clean;
            for (var number = 2; File.Exists(ProfilePath(candidate)) || Directory.Exists(ProfilePath(candidate)); number++) candidate = clean + " (" + number.ToString(CultureInfo.InvariantCulture) + ")";
            // CreateNew never overwrites another profile, even across launcher instances.
            WriteNew(ProfilePath(candidate), bytes);
            return candidate;
        }
        public void Rename(string oldName, string newName)
        {
            var source = ExistingProfile(oldName);
            var clean = CleanName(newName);
            if (oldName == clean) return;
            var destination = ProfilePath(clean);
            var caseOnly = string.Equals(oldName, clean, StringComparison.OrdinalIgnoreCase);
            if (!caseOnly && (File.Exists(destination) || Directory.Exists(destination))) throw new IniProfileException("exists");
            var updated = CopyMetadata();
            foreach (var key in new List<string>(updated.SelectedByGame.Keys))
                if (string.Equals(updated.SelectedByGame[key], oldName, StringComparison.OrdinalIgnoreCase)) updated.SelectedByGame[key] = clean;
            if (caseOnly)
            {
                var temporary = source + ".rename-" + Guid.NewGuid().ToString("N");
                File.Move(source, temporary);
                try { File.Move(temporary, destination); }
                catch { File.Move(temporary, source); throw; }
            }
            else File.Move(source, destination);
            try { Persist(updated); }
            catch { File.Move(destination, source); throw; }
        }
        public void Delete(string name)
        {
            var source = ExistingProfile(name);
            var updated = CopyMetadata();
            foreach (var key in new List<string>(updated.SelectedByGame.Keys))
                if (string.Equals(updated.SelectedByGame[key], name, StringComparison.OrdinalIgnoreCase)) updated.SelectedByGame[key] = "";
            // Retain the bytes until metadata and deletion have both succeeded.
            var bytes = File.ReadAllBytes(source);
            File.Delete(source);
            try { Persist(updated); }
            catch { WriteNew(source, bytes); throw; }
        }

        // The controller owns the target mutex and write-busy lifecycle. No game writes occur elsewhere.
        public string Apply(string gameDirectory, string profileName)
        {
            var target = Path.Combine(gameDirectory, "OptiScaler.ini");
            if (!File.Exists(target)) throw new IniProfileException("noini");
            if ((File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0) throw new IniProfileException("invalid");
            var source = string.IsNullOrEmpty(profileName) ? target : ExistingProfile(profileName);
            var document = Document.Read(File.ReadAllBytes(source));
            Validate(document.Text);
            // ASI plugins are on by default by design (this product ships an ASI component),
            // so applying a profile rewrites the flag to true even over an explicit false.
            var bytes = document.WithPlugins(true);
            var temporary = target + ".stage-" + Guid.NewGuid().ToString("N");
            var backup = target + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
            for (var i = 2; File.Exists(backup); i++) backup = target + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture) + "-" + i.ToString(CultureInfo.InvariantCulture);
            try
            {
                WriteNew(temporary, bytes);
                // Same-volume Replace atomically commits the staged INI and preserves the exact original.
                // Do not fall back to delete/copy: on failure the existing game INI must remain intact.
                File.Replace(temporary, target, backup);
                return backup;
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        static void Validate(string text)
        {
            if (text.IndexOf('\0') >= 0 || !Regex.IsMatch(text, @"(?im)^\s*\[(OptiScaler|DlssNr)\][ \t]*(?:[;#][^\r\n]*)?\r?$") || !Regex.IsMatch(text, @"(?m)^\s*[^;#\[\r\n=]+=[^\r\n]*\r?$"))
                throw new IniProfileException("invalid");
        }
        static void WriteNew(string path, byte[] bytes)
        {
            var created = false;
            try
            {
                using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    created = true;
                    stream.Write(bytes, 0, bytes.Length);
                    stream.Flush(true);
                }
            }
            catch
            {
                if (created) File.Delete(path);
                throw;
            }
        }
        IniProfileMetadata CopyMetadata()
        {
            return new IniProfileMetadata {
                SelectedByGame = new Dictionary<string, string>(_metadata.SelectedByGame, StringComparer.OrdinalIgnoreCase),
            };
        }
        void Persist(IniProfileMetadata updated)
        {
            Directory.CreateDirectory(_settingsDirectory);
            var temporary = MetadataPath + ".stage-" + Guid.NewGuid().ToString("N");
            try
            {
                WriteNew(temporary, new UTF8Encoding(false).GetBytes(JsonUtil.Write(updated)));
                if (File.Exists(MetadataPath)) File.Replace(temporary, MetadataPath, null);
                else File.Move(temporary, MetadataPath);
                _metadata = updated;
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        internal sealed class Document
        {
            public string Text;
            Encoding _encoding;
            byte[] _preamble;
            public static Document Read(byte[] bytes)
            {
                Encoding encoding = new UTF8Encoding(false, true);
                var offset = 0;
                if (bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) offset = 3;
                else if (bytes.Length >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) { encoding = new UnicodeEncoding(false, false, true); offset = 2; }
                else if (bytes.Length >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) { encoding = new UnicodeEncoding(true, false, true); offset = 2; }
                string text;
                try { text = encoding.GetString(bytes, offset, bytes.Length - offset); }
                catch (DecoderFallbackException) { throw new IniProfileException("invalid"); }
                var preamble = new byte[offset];
                if (offset > 0) Buffer.BlockCopy(bytes, 0, preamble, 0, offset);
                return new Document { Text = text, _encoding = encoding, _preamble = preamble };
            }
            public byte[] WithPlugins(bool enabled)
            {
                var newline = Text.IndexOf("\r\n", StringComparison.Ordinal) >= 0 ? "\r\n" : Text.IndexOf('\n') >= 0 ? "\n" : Text.IndexOf('\r') >= 0 ? "\r" : Environment.NewLine;
                var lines = new List<string>(Regex.Split(Text, "\\r\\n|\\r|\\n"));
                var inPlugins = false;
                var header = -1;
                var found = false;
                var value = enabled ? "true" : "false";
                for (var i = 0; i < lines.Count; i++)
                {
                    var section = Regex.Match(lines[i], @"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$");
                    if (section.Success)
                    {
                        inPlugins = string.Equals(section.Groups[1].Value.Trim(), "Plugins", StringComparison.OrdinalIgnoreCase);
                        if (inPlugins) header = i;
                        continue;
                    }
                    if (!inPlugins) continue;
                    var key = Regex.Match(lines[i], @"^(\s*LoadAsiPlugins\s*=\s*)[^;#]*(.*)$", RegexOptions.IgnoreCase);
                    if (!key.Success) continue;
                    lines[i] = key.Groups[1].Value + value + (key.Groups[2].Value.Length > 0 ? " " + key.Groups[2].Value : "");
                    found = true;
                }
                if (!found)
                {
                    if (header >= 0) lines.Insert(header + 1, "LoadAsiPlugins=" + value);
                    else
                    {
                        if (lines.Count > 0 && lines[lines.Count - 1].Length != 0) lines.Add("");
                        lines.Add("[Plugins]");
                        lines.Add("LoadAsiPlugins=" + value);
                        lines.Add("");
                    }
                }
                var content = _encoding.GetBytes(string.Join(newline, lines.ToArray()));
                if (_preamble.Length == 0) return content;
                var result = new byte[_preamble.Length + content.Length];
                Buffer.BlockCopy(_preamble, 0, result, 0, _preamble.Length);
                Buffer.BlockCopy(content, 0, result, _preamble.Length, content.Length);
                return result;
            }
        }
    }
}
