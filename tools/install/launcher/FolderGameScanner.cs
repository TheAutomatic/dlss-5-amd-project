using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace OptScalerNR.Launcher
{
    public sealed class ExeCandidate
    {
        public string Path, RelativePath, Architecture, Evidence;
        public string[] Apis;
        public bool Auxiliary;
        public int Rank;
        public string FileName { get { return System.IO.Path.GetFileName(Path); } }
    }
    public sealed class FolderScanResult
    {
        public ExeCandidate[] Candidates;
        public bool Partial;
        public int Directories, Executables;
    }
    public static class FolderGameScanner
    {
        sealed class ImageInfo
        {
            public string Architecture = "unknown";
            public HashSet<string> Direct = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public HashSet<string> Delayed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        }
        sealed class Section { public uint Va, Size, Raw; }
        static long Offset(List<Section> sections, uint rva, long length)
        {
            foreach (var s in sections) if (rva >= s.Va && (long)rva < (long)s.Va + s.Size) {
                var result = (long)s.Raw + rva - s.Va;
                if (result >= 0 && result < length) return result;
            }
            return -1;
        }
        static string Name(BinaryReader reader, long offset)
        {
            if (offset < 0 || offset >= reader.BaseStream.Length) return null;
            reader.BaseStream.Position = offset;
            var chars = new List<char>();
            for (var i=0; i<260 && reader.BaseStream.Position < reader.BaseStream.Length; i++) {
                var b = reader.ReadByte(); if (b == 0) break; chars.Add((char)b);
            }
            return new string(chars.ToArray()).ToLowerInvariant();
        }
        static ImageInfo ReadImage(string path)
        {
            var result = new ImageInfo();
            try {
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var reader = new BinaryReader(stream)) {
                    if (stream.Length < 64 || reader.ReadUInt16() != 0x5a4d) return result;
                    stream.Position = 60; var pe = reader.ReadUInt32();
                    if (pe < 64 || (long)pe + 24 > stream.Length) return result;
                    stream.Position = pe; if (reader.ReadUInt32() != 0x4550) return result;
                    var machine = reader.ReadUInt16(); var count = reader.ReadUInt16();
                    result.Architecture = machine == 0x8664 ? "x64" : machine == 0x14c ? "x86" : machine == 0xaa64 ? "arm64" : "unknown";
                    stream.Position = pe + 20; var size = reader.ReadUInt16();
                    long optional = pe + 24; stream.Position = optional; var magic = reader.ReadUInt16();
                    if ((magic != 0x20b && magic != 0x10b) || count > 96 || optional + size + count*40 > stream.Length) return result;
                    var directory = magic == 0x20b ? 112 : 96;
                    var sections = new List<Section>();
                    stream.Position = optional + size;
                    for (int i=0; i<count; i++) {
                        stream.Position += 8;
                        var virtualSize = reader.ReadUInt32(); var va = reader.ReadUInt32();
                        var rawSize = reader.ReadUInt32(); var raw = reader.ReadUInt32(); stream.Position += 16;
                        sections.Add(new Section { Va=va, Size=Math.Max(virtualSize,rawSize), Raw=raw });
                    }
                    foreach (var delayed in new[] {false,true}) {
                        var index = delayed ? 13 : 1;
                        if (size < directory + (index+1)*8) continue;
                        stream.Position = optional + directory + index*8;
                        var rva = reader.ReadUInt32(); if (rva == 0) continue;
                        var at = Offset(sections,rva,stream.Length); if (at < 0) continue;
                        int rowSize=delayed ? 32 : 20;
                        for (int i=0; i<256; i++) {
                            var row=at+i*rowSize; if (row+rowSize>stream.Length) break;
                            stream.Position=row; var first=reader.ReadUInt32();
                            stream.Position=row+(delayed ? 4 : 12); var nameRva=reader.ReadUInt32();
                            if (first==0 && nameRva==0) break;
                            if (delayed && (first & 1)==0) continue; // Legacy VA descriptors are not guessed.
                            var name=Name(reader,Offset(sections,nameRva,stream.Length));
                            if (!string.IsNullOrEmpty(name)) (delayed ? result.Delayed : result.Direct).Add(name);
                        }
                    }
                }
            } catch (IOException) { } catch (UnauthorizedAccessException) { } catch (ArgumentException) { }
            return result;
        }
        static string Api(string dll)
        {
            switch (dll.ToLowerInvariant()) {
                case "d3d11.dll": return "D3D11"; case "d3d12.dll": return "D3D12";
                case "dxgi.dll": return "DXGI"; case "vulkan-1.dll": return "Vulkan";
                case "d3d9.dll": return "D3D9"; case "opengl32.dll": return "OpenGL";
                default: return null;
            }
        }
        static bool Linked(string path)
        { try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; } catch { return true; } }
        static bool LinkedAncestor(string path)
        {
            while (!string.IsNullOrEmpty(path)) {
                if (Linked(path)) return true;
                path = System.IO.Path.GetDirectoryName(path);
            }
            return false;
        }
        internal static bool Auxiliary(string path)
        {
            var name=System.IO.Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
            if(new[] {"setup","install","unins","crash","unitycrash","vc_redist","dxsetup","easyanticheat","eac_","battleye","beclient","beservice","unrealcef","epicwebhelper","webview","cefsubprocess","person-worker","gamelaunchhelper","shadercompile","bugsplat","crs-","redprelauncher","larilauncher","2klauncher","activationui","cleanup","touchup","8za","7za","checkenv"}.Any(p=>name.StartsWith(p,StringComparison.Ordinal)))return true;
            return new[] {"prelauncher","launcher","crashreport","crashhandler","crs-handler","errorreporter","updater","patcher","uninstaller","benchmark","dedicatedserver"}.Any(s=>name.EndsWith(s,StringComparison.Ordinal));
        }
        static bool HasUpscalerAnchor(string exeDir)
        {
            if(string.IsNullOrEmpty(exeDir))return false;
            foreach(var marker in new[] {"amd-presr-install.txt","nvngx_dlss.dll","nvngx_dlssd.dll","nvngx_dlssg.dll","sl.interposer.dll","libxess.dll","libxess_fg.dll","amd_fidelityfx_dx12.dll","amd_fidelityfx_upscaler_dx12.dll","amd_fidelityfx_vk.dll"})
                if(File.Exists(System.IO.Path.Combine(exeDir,marker)))return true;
            return false;
        }
        static ExeCandidate Inspect(string path,string root, CancellationToken token,Stopwatch timer)
        {
            var image=ReadImage(path); var apis=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string evidence="none";
            foreach (var name in image.Direct) { var api=Api(name); if(api!=null) { apis.Add(api); evidence="direct"; } }
            foreach (var name in image.Delayed) { var api=Api(name); if(api!=null) { apis.Add(api); if(evidence=="none") evidence="delay"; } }
            // Search only bounded local dependencies, never the OS directories or arbitrary import paths.
            var queue=new Queue<Tuple<string,int>>(); queue.Enqueue(Tuple.Create(path,0));
            var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase); int files=0;
            while(queue.Count>0 && files<16 && !token.IsCancellationRequested && timer.ElapsedMilliseconds<10000) {
                var next=queue.Dequeue(); if(!seen.Add(next.Item1))continue; files++;
                var info=next.Item2==0 ? image : ReadImage(next.Item1);
                foreach(var name in info.Direct.Concat(info.Delayed)) {
                    var api=Api(name);
                    if(api!=null) { apis.Add(api); if(evidence=="none") evidence="dependency"; continue; }
                    if(next.Item2>=2 || System.IO.Path.GetFileName(name)!=name || !name.EndsWith(".dll",StringComparison.OrdinalIgnoreCase))continue;
                    foreach(var dir in new[] {System.IO.Path.GetDirectoryName(next.Item1),root,System.IO.Path.Combine(root,"Engine/Binaries/Win64"),System.IO.Path.Combine(root,"Binaries/Win64")}) {
                        var dll=System.IO.Path.Combine(dir,name);
                        if(File.Exists(dll) && !LinkedAncestor(dll)) { queue.Enqueue(Tuple.Create(dll,next.Item2+1)); break; }
                    }
                }
            }
            bool auxiliary=Auxiliary(path);
            var rel=path.Substring(root.Length).TrimStart('\\','/');
            int rank=(apis.Count==0 ? 50 : apis.All(a=>a=="DXGI") ? 20 : 0)+(auxiliary ? 100 : 0)+(image.Architecture=="x64" ? 0 : 200);
            if(path.IndexOf("shipping.exe",StringComparison.OrdinalIgnoreCase)>=0)rank-=3;
            if(rel.IndexOf("x64_dx12",StringComparison.OrdinalIgnoreCase)>=0 || rel.IndexOf("dx12",StringComparison.OrdinalIgnoreCase)>=0)rank-=4;
            if(HasUpscalerAnchor(System.IO.Path.GetDirectoryName(path)))rank-=10;
            return new ExeCandidate {Path=path,RelativePath=rel,Architecture=image.Architecture,
                Apis=apis.OrderBy(a=>a).ToArray(),Evidence=evidence,Auxiliary=auxiliary,Rank=rank};
        }
        sealed class State
        {
            public readonly object Gate=new object();
            public readonly List<ExeCandidate> Found=new List<ExeCandidate>();
            public int Directories,Executables;
            public bool Limited;
            public FolderScanResult Snapshot(bool partial) { lock(Gate) return new FolderScanResult {Candidates=Found.OrderBy(c=>c.Rank).ThenBy(c=>c.RelativePath,StringComparer.OrdinalIgnoreCase).ToArray(),Partial=partial||Limited,Directories=Directories,Executables=Executables}; }
        }
        sealed class DirectoryItem { public string Path; public int Depth,Priority; }
        static int Priority(string path)
        {
            var name=System.IO.Path.GetFileName(path).ToLowerInvariant();
            return new[] {"binaries","win64","wingdk","x64_dx12","x64","windowsnoeditor","client","bin","platform"}.Contains(name) ? 0 : name=="engine" || name=="content" ? 1 : 2;
        }
        public static async Task<FolderScanResult> ScanAsync(string directory,CancellationToken cancellation,Action<int,int> progress)
        {
            var absolute=System.IO.Path.GetFullPath(directory);
            var root=string.Equals(absolute,System.IO.Path.GetPathRoot(absolute),StringComparison.OrdinalIgnoreCase)?absolute:absolute.TrimEnd('\\','/');
            var state=new State(); var timer=Stopwatch.StartNew();
            var linked=CancellationTokenSource.CreateLinkedTokenSource(cancellation); var token=linked.Token;
            var work=Task.Run(()=> {
                if(!Directory.Exists(root) || LinkedAncestor(root))throw new IOException("Select an existing folder without directory links.");
                var todo=new List<DirectoryItem> {new DirectoryItem {Path=root}};
                var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                while(todo.Count>0 && !token.IsCancellationRequested && timer.ElapsedMilliseconds<10000) {
                    todo.Sort((a,b)=> {int p=a.Priority.CompareTo(b.Priority); return p!=0 ? p : a.Depth.CompareTo(b.Depth);});
                    var item=todo[0]; todo.RemoveAt(0);
                    if(!seen.Add(item.Path) || Linked(item.Path))continue;
                    lock(state.Gate)state.Directories++;
                    try {
                        foreach(var exe in Directory.EnumerateFiles(item.Path,"*.exe")) {
                            if(token.IsCancellationRequested || timer.ElapsedMilliseconds>=10000)break;
                            if(Linked(exe))continue;
                            lock(state.Gate) { state.Executables++; if(state.Executables>256) {state.Limited=true;return;} }
                            var candidate=Inspect(exe,root,token,timer);
                            lock(state.Gate)state.Found.Add(candidate);
                            if(progress!=null)progress(state.Directories,state.Found.Count);
                        }
                        if(item.Depth>=8) {lock(state.Gate)state.Limited=true;continue;}
                        foreach(var dir in Directory.EnumerateDirectories(item.Path)) {
                            if(token.IsCancellationRequested || timer.ElapsedMilliseconds>=10000)break;
                            var name=System.IO.Path.GetFileName(dir).ToLowerInvariant();
                            // Xbox installs put their actual executables under Content. Keep
                            // the root Content folder and identified Xbox packages searchable.
                            if(name=="content" && item.Depth>0 && !File.Exists(System.IO.Path.Combine(dir,"MicrosoftGame.Config")))continue;
                            if(new[] {"paks","shadercache",".git","node_modules","optiscaler","person-model","dlssnr-amd","redist","_commonredist","__installer"}.Contains(name))continue;
                            if(todo.Count+seen.Count>=10000) {lock(state.Gate)state.Limited=true;break;}
                            if(!Linked(dir))todo.Add(new DirectoryItem {Path=dir,Depth=item.Depth+1,Priority=Priority(dir)});
                        }
                    } catch(IOException) {lock(state.Gate)state.Limited=true;} catch(UnauthorizedAccessException) {lock(state.Gate)state.Limited=true;}
                }
                if(todo.Count>0)lock(state.Gate)state.Limited=true;
            });
            var winner=await Task.WhenAny(work,Task.Delay(10000,cancellation)).ConfigureAwait(false);
            if(winner==work) {try {await work.ConfigureAwait(false);}finally{linked.Dispose();} return state.Snapshot(false);}
            linked.Cancel(); ReleaseWhenDone(work,linked);
            return state.Snapshot(true);
        }
        static void ReleaseWhenDone(Task work,CancellationTokenSource source)
        {work.ContinueWith(t=> {var ignored=t.Exception;source.Dispose();},TaskScheduler.Default);}
    }
}
