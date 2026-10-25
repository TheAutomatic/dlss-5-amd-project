"""PE import probe: x64, truncated, non-PE, and candidate ranking."""
from pathlib import Path
import json
import os
import struct
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
PS = Path(os.environ.get("SystemRoot", r"C:\Windows")) / (
    "System32/WindowsPowerShell/v1.0/powershell.exe"
)
PROBE = REPO / "tools/install/proxy-probe.ps1"


def write_pe(path: Path, machine=0x8664, magic=0x20B, imports=("dxgi.dll",), delay=(), trunc=None):
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)
    pe = bytearray()
    pe += b"PE\0\0"
    pe += struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, 240, 0x22)
    # PE32+ optional header (240 bytes), data dirs at +112
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, magic)
    struct.pack_into("<I", opt, 16, 0x1000)  # SizeOfCode dummy
    struct.pack_into("<I", opt, 108, 16)  # NumberOfRvaAndSizes
    # section
    sec = bytearray(40)
    sec[0:6] = b".rdata"
    struct.pack_into("<I", sec, 8, 0x200)  # vsize
    struct.pack_into("<I", sec, 12, 0x2000)  # va
    struct.pack_into("<I", sec, 16, 0x200)  # raw size
    struct.pack_into("<I", sec, 20, 64 + 4 + 20 + 240 + 40)  # raw ptr after headers + section table
    rdata = bytearray(0x200)
    # import desc at rva 0x2000
    struct.pack_into("<II", opt, 112 + 8, 0x2000, 20)
    name_off = 40
    for i, name in enumerate(imports):
        nb = name.encode("ascii") + b"\0"
        rdata[name_off : name_off + len(nb)] = nb
        struct.pack_into("<I", rdata, 20 * i + 12, 0x2000 + name_off)
        name_off += len(nb)
    if delay:
        delay_off = 0x80
        struct.pack_into("<I", opt, 96, 16)  # NumberOfRvaAndSizes on PE32+ is at 108; also set 96 unused
        struct.pack_into("<II", opt, 112 + 13 * 8, 0x2000 + delay_off, 64)
        for i, name in enumerate(delay):
            nb = name.encode("ascii") + b"\0"
            rdata[name_off : name_off + len(nb)] = nb
            struct.pack_into("<I", rdata, delay_off + 32 * i + 4, 0x2000 + name_off)
            name_off += len(nb)
    blob = dos + pe + opt + sec + rdata
    if trunc is not None:
        blob = blob[:trunc]
    path.write_bytes(blob)


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class ProxyProbeTests(unittest.TestCase):
    def run_probe(self, exe: Path):
        out = Path(tempfile.mkdtemp(prefix="probe-")) / "out.json"
        cmd = [
            str(PS),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(PROBE),
            "-ExePath",
            str(exe),
            "-OutJson",
            str(out),
        ]
        p = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return json.loads(out.read_text(encoding="utf-8"))

    def test_non_pe(self):
        d = Path(tempfile.mkdtemp(prefix="probe-bad-"))
        exe = d / "game.exe"
        exe.write_bytes(b"not a pe")
        data = self.run_probe(exe)
        self.assertFalse(data["pe"]["ok"])
        self.assertIsNone(data.get("recommend"))
        self.assertIn("自行尝试", data.get("hint") or "")

    def test_truncated(self):
        d = Path(tempfile.mkdtemp(prefix="probe-trunc-"))
        exe = d / "game.exe"
        write_pe(exe, trunc=40)
        data = self.run_probe(exe)
        self.assertIn(data["pe"]["error"], ("too-small", "truncated", "not-pe"))

    def test_x86_rejected(self):
        d = Path(tempfile.mkdtemp(prefix="probe-x86-"))
        exe = d / "game.exe"
        write_pe(exe, machine=0x14C)
        data = self.run_probe(exe)
        self.assertEqual(data["pe"]["arch"], "x86")
        self.assertFalse(data["pe"]["ok"])

    def test_cycle_local_dependency(self):
        d = Path(tempfile.mkdtemp(prefix="probe-cycle-"))
        a = d / "a.dll"
        b = d / "b.dll"
        write_pe(a, imports=("b.dll",))
        write_pe(b, imports=("a.dll", "dxgi.dll"))
        exe = d / "game.exe"
        write_pe(exe, imports=("a.dll",))
        data = self.run_probe(exe)
        self.assertGreaterEqual(data["walk"]["files"], 1)
        names = [c["name"] for c in data["candidates"]]
        self.assertIn("dxgi.dll", names)

    def test_unicode_and_special_path(self):
        root = Path(tempfile.mkdtemp(prefix="probe-path-"))
        d = root / "游戏 dir with spaces & 'quotes'"
        d.mkdir()
        exe = d / "game.exe"
        write_pe(exe, imports=("dxgi.dll",))
        data = self.run_probe(exe)
        # PowerShell expands aliases such as Actions' RUNNER~1 TEMP path.
        self.assertTrue(Path(data["gameDir"]).samefile(d))

    def test_import_dxgi_ranks_medium(self):
        d = Path(tempfile.mkdtemp(prefix="probe-imp-"))
        exe = d / "game.exe"
        write_pe(exe, imports=("dxgi.dll",))
        data = self.run_probe(exe)
        names = [c["name"] for c in data["candidates"]]
        self.assertEqual(names[0], "dxgi.dll")
        self.assertEqual(data["recommend"], "dxgi.dll")
        self.assertIn("dxgi.dll", data.get("hint") or "")
