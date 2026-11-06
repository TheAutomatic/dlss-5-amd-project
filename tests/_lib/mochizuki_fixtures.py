"""Small model-converter fixture; exercises the real installer/hash/commit path.

This does not emulate neural weights. Real graph conversion is covered by the
Mochizuki model/GPU verification against the upstream model hash.
"""
from pathlib import Path
import atexit
import hashlib
import os
import shutil
import subprocess
import tempfile

PAYLOAD = b'Mochizuki installer INT4 fixture'
_temporary = None


def extractor_tool(destination, with_small=True):
    """Official extractor stand-in: <source dll> <out dir> -> both model packs."""
    root = Path(tempfile.mkdtemp(prefix='mochi-extractor-fixture-'))
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    if True:
        source = root / 'extractor.cs'
        source.write_text("""using System;
using System.IO;
class Extractor {
    static int Main(string[] args) {
        if (args.Length != 2 || !File.Exists(args[0])) return 2;
        Directory.CreateDirectory(args[1]);
        var magic = System.Text.Encoding.ASCII.GetBytes("NRMODEL1");
        Write(Path.Combine(args[1], "CC_SILVER_AARDWOLD.bin"), magic, 599);
        var marker = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "extractor-small.marker");
        if (!File.Exists(marker) || File.ReadAllText(marker).Trim() != "0")
            Write(Path.Combine(args[1], "CC_SMALL_FP8.bin"), magic, 0);
        return 0;
    }
    static void Write(string path, byte[] magic, uint entries) {
        using (var w = new BinaryWriter(File.Create(path))) {
            w.Write(magic);
            w.Write(entries);
            w.Write(new byte[20]);
        }
    }
}""", encoding='utf-8')
        compiler = Path(os.environ['SystemRoot']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        subprocess.run([str(compiler), '/nologo', '/target:exe',
                        '/out:' + str(root / 'extractor.exe'), str(source)], check=True, capture_output=True)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / 'extractor.exe', destination / 'dlssnr_extract_model.exe')
    # The fixture reads this marker at run time, after the test reconfigures it.
    (destination / 'extractor-small.marker').write_text('1' if with_small else '0', encoding='ascii')


def int4_tools(destination):
    root = Path(tempfile.mkdtemp(prefix='mochi-converter-fixture-'))
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    if True:
        source = root / 'converter.cs'
        source.write_text('''using System;
using System.IO;
class Converter {
    static int Main(string[] args) {
        if (args.Length != 3 || !File.Exists(args[0]) ||
            !File.Exists(Path.Combine(args[1], "settings.txt"))) return 2;
        var input = File.ReadAllBytes(args[0]);
        if (input.Length < 12 || System.Text.Encoding.ASCII.GetString(input, 0, 8) != "NRMODEL1") return 3;
        File.WriteAllText(args[2], "Mochizuki installer INT4 fixture", new System.Text.UTF8Encoding(false));
        return 0;
    }
}''', encoding='utf-8')
        compiler = Path(os.environ['SystemRoot']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        subprocess.run([str(compiler), '/nologo', '/target:exe',
                        '/out:' + str(root / 'converter.exe'), str(source)], check=True, capture_output=True)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / 'converter.exe', destination / 'dlssnr-int4-weights.exe')
    (destination / 'dlssnr-int4.sha256').write_text(hashlib.sha256(PAYLOAD).hexdigest() + '\n', encoding='ascii')
