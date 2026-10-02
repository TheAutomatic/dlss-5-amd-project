# Exercise the installer's actual C# helper without opening a picker or installing files.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Get-Content -LiteralPath (Join-Path $root 'tools/install-amd-presr.ps1') -Raw -Encoding UTF8
$match = [regex]::Match($source, '(?s)Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"\r?\n(.*?)\r?\n"@')
if (-not $match.Success) { throw 'Installer folder picker helper not found' }
$probe = @'
public static class FolderPickerDpiTest {
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern System.IntPtr GetThreadDpiAwarenessContext();
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern System.IntPtr SetThreadDpiAwarenessContext(System.IntPtr context);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern System.IntPtr GetWindowDpiAwarenessContext(System.IntPtr window);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool AreDpiAwarenessContextsEqual(System.IntPtr a, System.IntPtr b);
    static void Check(bool value, string message) { if (!value) throw new System.Exception(message); }
    public static void Run() {
        var original = SetThreadDpiAwarenessContext(new System.IntPtr(-1));
        Check(original != System.IntPtr.Zero, "set unaware test context");
        try {
            var before = GetThreadDpiAwarenessContext();
            try {
                using (AmdFolderPick.BeginDpiScope()) {
                    var expected = new System.IntPtr(-4);
                    Check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), expected), "picker thread must use per-monitor v2");
                    using (var owner = new System.Windows.Forms.Form()) {
                        Check(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(owner.Handle), expected), "owner must inherit per-monitor v2");
                    }
                    using (AmdFolderPick.BeginDpiScope()) { }
                    Check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), expected), "nested scope restores parent");
                    throw new System.OperationCanceledException();
                }
            } catch (System.OperationCanceledException) { }
            Check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), before), "scope restores after exception/cancel");
        } finally { SetThreadDpiAwarenessContext(original); }
        Check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), original), "test restores host context");
    }
}
'@
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition ($match.Groups[1].Value + "`n" + $probe)
[FolderPickerDpiTest]::Run()
Write-Host 'Folder picker DPI: PASS (thread, owner, nested scope, exception restore)'
