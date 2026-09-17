"""Build a harmless synthetic PE fixture; no author DLL is read or executed.

The fixture has a minimal DllMain and a no-op worker at the addresses expected
by the 0.3.1 bootstrap filter. It deliberately is NOT an accepted runtime SHA:
only the runtime-load isolation unit test bypasses the production IdentifyRuntime gate.
"""
from pathlib import Path
import struct
import sys

out = bytearray(0x8600)


def write(offset, fmt, *values):
    struct.pack_into("<" + fmt, out, offset, *values)


def at(rva, data):
    offset = 0x400 + rva - 0x1000 if rva < 0x8800 else 0x7C00 + rva - 0x8D000
    out[offset:offset + len(data)] = data


out[:2] = b"MZ"
write(0x3C, "I", 0x80)
out[0x80:0x84] = b"PE\0\0"
write(0x84, "HHIIIHH", 0x8664, 2, 0, 0, 0, 0xF0, 0x2023)
opt = 0x98
write(opt, "HBBIII", 0x20B, 14, 0, 0x7800, 0xA00, 0)
write(opt + 16, "IIQII", 0x1000, 0x1000, 0x180000000, 0x1000, 0x200)
write(opt + 40, "HHHHHH", 6, 0, 0, 0, 6, 0)
write(opt + 56, "II", 0x8E000, 0x400)
write(opt + 68, "HHQQQQII", 2, 0x100, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
write(opt + 112 + 8, "II", 0x8D000, 40)  # import directory
write(opt + 112 + 12 * 8, "II", 0x8D788, 16)  # IAT
section = opt + 0xF0
out[section:section + 8] = b".text\0\0\0"
write(section + 8, "IIIIIIHHI", 0x8C000, 0x1000, 0x7800, 0x400, 0, 0, 0, 0, 0x60000020)
section += 40
out[section:section + 8] = b".idata\0\0"
write(section + 8, "IIIIIIHHI", 0xA00, 0x8D000, 0xA00, 0x7C00, 0, 0, 0, 0, 0xC0000040)

# Entry: do nothing unless DLL_PROCESS_ATTACH; prepare the stack arguments and
# jump to our own CreateThread call. No imports other than CreateThread.
entry = bytes.fromhex("83 fa 01 74 06 b8 01 00 00 00 c3 48 83 ec 38")
entry += bytes.fromhex("48 c7 44 24 28 00 00 00 00 c7 44 24 20 00 00 00 00")
entry += b"\xe9" + struct.pack("<i", 0x6D8F - (0x1000 + len(entry) + 5))
at(0x1000, entry)
if len(sys.argv) > 2 and sys.argv[2] == "--no-bootstrap":
    at(0x1000, bytes.fromhex("b8 01 00 00 00 c3"))
at(0x6D8F, bytes.fromhex("4c 8d 05 9a 18 00 00 31 c9 31 d2 45 31 c9 ff 15 e5 69 08 00"))
at(0x6DA3, bytes.fromhex("48 83 c4 38 b8 01 00 00 00 c3"))
# This no-op worker is harmless even if filtering regresses. This test must
# nevertheless reject that load because it did not observe one suppression.
at(0x8630, bytes.fromhex("55 41 57 41 56 41 54 56 57 53 48 81 ec 50 02 00 00 "
                       "48 81 c4 50 02 00 00 5b 5f 5e 41 5c 41 5e 41 5f 5d 31 c0 c3"))
at(0x8D000, struct.pack("<IIIII", 0x8D100, 0, 0, 0x8D1A0, 0x8D788))
at(0x8D100, struct.pack("<QQ", 0x8D180, 0))
at(0x8D788, struct.pack("<QQ", 0x8D180, 0))
at(0x8D180, b"\0\0CreateThread\0")
at(0x8D1A0, b"KERNEL32.dll\0")
target = Path(sys.argv[1])
target.write_bytes(out)
print("Synthetic bootstrap fixture:", target)
