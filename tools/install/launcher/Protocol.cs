using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;

namespace OptScalerNR.Launcher
{
    [DataContract]
    public sealed class LauncherRequest
    {
        [DataMember(Name = "schema")] public int Schema = 1;
        [DataMember(Name = "requestId")] public string RequestId;
        [DataMember(Name = "action")] public string Action;
        [DataMember(Name = "gameExe")] public string GameExe;
        [DataMember(Name = "proxyChoice")] public string ProxyChoice;
        [DataMember(Name = "backendChoice")] public string BackendChoice;
        [DataMember(Name = "explicitDecisions")] public Decisions ExplicitDecisions;
        [DataMember(Name = "fingerprint")] public string Fingerprint;
        [DataMember(Name = "packageRoot")] public string PackageRoot;
        [DataMember(Name = "backends")] public string[] Backends;
        [DataMember(Name = "activeBackend")] public string ActiveBackend;
        [DataMember(Name = "language")] public string Language;
        [DataMember(Name = "compatibilityFingerprint")] public string CompatibilityFingerprint;
    }

    [DataContract]
    public sealed class Decisions
    {
        [DataMember(Name = "existingInstall")] public string ExistingInstall;
        [DataMember(Name = "existingIni")] public string ExistingIni;
        [DataMember(Name = "missingMochizukiModel")] public string MissingMochizukiModel;
        [DataMember(Name = "mochizukiModels")] public string MochizukiModels;
        [DataMember(Name = "mochizukiNetworks")] public string[] MochizukiNetworks;
        [DataMember(Name = "mochizukiActiveModel")] public string MochizukiActiveModel;
        [DataMember(Name = "lmxxfModels")] public string LmxxfModels;
        [DataMember(Name = "shortFileContinue")] public string ShortFileContinue;
        [DataMember(Name = "optiPatcher")] public string OptiPatcher;
        [DataMember(Name = "openRr")] public string OpenRr;
        [DataMember(Name = "foreignProxy")] public Dictionary<string, string> ForeignProxy;
        [DataMember(Name = "xessGroups")] public string[] XessGroups;
        [DataMember(Name = "modelSourceDll")] public string ModelSourceDll;
    }

    [DataContract]
    public sealed class LauncherResult
    {
        [DataMember(Name = "schema")] public int Schema;
        [DataMember(Name = "requestId")] public string RequestId;
        [DataMember(Name = "status")] public string Status;
        [DataMember(Name = "effectiveGameDir")] public string EffectiveGameDir;
        [DataMember(Name = "resolvedProxy")] public string ResolvedProxy;
        [DataMember(Name = "candidates")] public Candidate[] Candidates;
        [DataMember(Name = "conflicts")] public Conflict[] Conflicts;
        [DataMember(Name = "requiredDecisions")] public RequiredDecision[] RequiredDecisions;
        [DataMember(Name = "logPath")] public string LogPath;
        [DataMember(Name = "error")] public string Error;
        [DataMember(Name = "fingerprint")] public string Fingerprint;
        [DataMember(Name = "notes")] public string[] Notes;
        [DataMember(Name = "partial")] public bool Partial;
        [DataMember(Name = "recommend")] public string Recommend;
        [DataMember(Name = "games")] public GameInfo[] Games;
        [DataMember(Name = "backends")] public BackendInfo[] Backends;
        [DataMember(Name = "packageRoot")] public string PackageRoot;
        [DataMember(Name = "compatibility")] public XessCompatibility Compatibility;
        [DataMember(Name = "mochizukiInstall")] public MochizukiInstallInfo MochizukiInstall;
    }

    // A Mochizuki (DLSSNR-AMD) installation of its own found in the game folder (inspect is read-only).
    [DataContract]
    public sealed class MochizukiInstallInfo
    {
        [DataMember(Name = "detected")] public bool Detected;
        [DataMember(Name = "recorded")] public bool Recorded;
        [DataMember(Name = "files")] public string[] Files;
        [DataMember(Name = "proxy")] public string Proxy;
    }

    [DataContract]
    public sealed class XessCompatibility
    {
        [DataMember(Name = "restoreFiles")] public string[] RestoreFiles;
        [DataMember(Name = "groups")] public XessGroup[] Groups;
        [DataMember(Name = "active")] public bool Active;
        [DataMember(Name = "fingerprint")] public string Fingerprint;
        [DataMember(Name = "evidence")] public string[] Evidence;
        [DataMember(Name = "error")] public string Error;
    }

    [DataContract]
    public sealed class XessGroup
    {
        [DataMember(Name = "id")] public string Id;
        [DataMember(Name = "ready")] public bool Ready;
        [DataMember(Name = "different")] public bool Different;
        [DataMember(Name = "files")] public XessFile[] Files;
    }

    [DataContract]
    public sealed class XessFile
    {
        [DataMember(Name = "name")] public string Name;
        [DataMember(Name = "gameVersion")] public string GameVersion;
        [DataMember(Name = "packageVersion")] public string PackageVersion;
        [DataMember(Name = "state")] public string State;
    }

    [DataContract]
    public sealed class BackendInfo
    {
        [DataMember(Name = "id")] public string Id;
        [DataMember(Name = "runtimeState")] public string RuntimeState;
        [DataMember(Name = "modelState")] public string ModelState;
        [DataMember(Name = "originalState")] public string OriginalState;
        [DataMember(Name = "smallState")] public string SmallState;
        [DataMember(Name = "experimentalState")] public string ExperimentalState;
        [DataMember(Name = "int4State")] public string Int4State;
        [DataMember(Name = "preparationState")] public string PreparationState;
        [DataMember(Name = "enabled")] public bool Enabled;
        [DataMember(Name = "reasonCode")] public string ReasonCode;
        [DataMember(Name = "reason")] public string Reason;
        [DataMember(Name = "hints")] public string[] Hints;
        [DataMember(Name = "missingPackageFiles")] public string[] MissingPackageFiles;
    }

    [DataContract]
    public sealed class GameInfo
    {
        [DataMember(Name = "id")] public string Id;
        [DataMember(Name = "name")] public string Name;
        [DataMember(Name = "exe")] public string Exe;
        [DataMember(Name = "source")] public string Source;
        [DataMember(Name = "installed")] public bool Installed;
        [DataMember(Name = "cover")] public string Cover;
        [DataMember(Name = "hero")] public string Hero;
        [DataMember(Name = "root", EmitDefaultValue = false)] public string Root;
        [DataMember(Name = "customName", EmitDefaultValue = false)] public string CustomName;
        [DataMember(Name = "customCover", EmitDefaultValue = false)] public string CustomCover;
        [DataMember(Name = "exeOverride", EmitDefaultValue = false)] public bool? ExeOverride;
        public string DisplayName { get { return string.IsNullOrWhiteSpace(CustomName) ? Name : CustomName; } }
        public string DisplayCover { get { return !string.IsNullOrEmpty(CustomCover) && File.Exists(CustomCover) ? CustomCover : Cover; } }
        public string DisplayHero { get { return !string.IsNullOrEmpty(CustomCover) && File.Exists(CustomCover) ? CustomCover : Hero; } }
        public string RootDirectory { get { try { return !string.IsNullOrEmpty(Root) ? Root : !string.IsNullOrEmpty(Exe) ? Path.GetDirectoryName(Exe) : null; } catch { return null; } } }
        public bool HasExecutable { get { return !string.IsNullOrEmpty(Exe) && GamePaths.CanUse(Exe); } }
    }

    [DataContract]
    public sealed class Candidate
    {
        [DataMember(Name = "name")] public string Name;
        [DataMember(Name = "rank")] public int Rank;
        [DataMember(Name = "confidence")] public string Confidence;
        [DataMember(Name = "reason")] public string Reason;
    }

    [DataContract]
    public sealed class Conflict
    {
        [DataMember(Name = "name")] public string Name;
        [DataMember(Name = "kind")] public string Kind;
        [DataMember(Name = "detail")] public string Detail;
    }

    [DataContract]
    public sealed class RequiredDecision
    {
        [DataMember(Name = "id")] public string Id;
        [DataMember(Name = "file")] public string File;
        [DataMember(Name = "prompt")] public string Prompt;
        [DataMember(Name = "options")] public string[] Options;
    }

    public static class JsonUtil
    {
        static readonly object SerializerGate = new object();
        static readonly Dictionary<Type, DataContractJsonSerializer> Serializers = new Dictionary<Type, DataContractJsonSerializer>();

        static DataContractJsonSerializer Serializer(Type type)
        {
            lock (SerializerGate)
            {
                DataContractJsonSerializer ser;
                if (!Serializers.TryGetValue(type, out ser))
                {
                    ser = new DataContractJsonSerializer(type, new DataContractJsonSerializerSettings
                    {
                        UseSimpleDictionaryFormat = true
                    });
                    Serializers[type] = ser;
                }
                return ser;
            }
        }

        public static string Write<T>(T value)
        {
            var ser = Serializer(typeof(T));
            using (var ms = new MemoryStream())
            {
                ser.WriteObject(ms, value);
                return Encoding.UTF8.GetString(ms.ToArray());
            }
        }

        public static T Read<T>(string json)
        {
            var ser = Serializer(typeof(T));
            using (var ms = new MemoryStream(Encoding.UTF8.GetBytes(json)))
                return (T)ser.ReadObject(ms);
        }

        public static void WriteFile<T>(string path, T value)
        {
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, Write(value), new UTF8Encoding(false));
            File.Copy(tmp, path, true);
            File.Delete(tmp);
        }
    }
}
