"""Scan text for encoding damage. Three different findings, and only one of them is mojibake.

  mojibake   The bytes of a UTF-8 punctuation character were decoded as GB18030 and
             saved again. U+9225 is that lead (UTF-8 E2 80). It is not U+9325; the
             two look the same. An ellipsis is E2 80 A6, and A6 then swallows the
             next byte, so "...the" becomes U+9225 plus a private-use character and
             the "t" is gone. Restore the punctuation and the swallowed byte.
             Do not embed the CJK character in this file: a previous copy saved the
             comparison literal as U+FFFD, and the check then matched nothing.

  U+FFFD     A replacement character is in the file. The original glyph is already
             gone. Report it. Do not invent a dash or a letter to put in its place.

  not UTF-8  A Windows-1252 punctuation byte (en dash, em dash, quotes). The text
             is normal. Transcode that byte to the same character in UTF-8.
             Do not delete it, and do not turn the en dash between Catmull and Rom
             into a hyphen. Decoding with errors="replace" is wrong here: it invents a
             U+FFFD the file does not contain.

U+FFFD inside external/ and analysis/ is a known fixture set and is not scanned.

Usage:  python tools/scan-mojibake.py [upstream-clone-dir] [upstream-ref]
        Defaults: ../dlss5-on-amd-9070xt-porting and feat/colour-contract-and-kernel-flags.
Exit code 0 always. Read the mojibake count.
"""
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "..", "dlss5-on-amd-9070xt-porting")
UPBRANCH = sys.argv[2] if len(sys.argv) > 2 else "feat/colour-contract-and-kernel-flags"
MEM = os.path.join(
    os.environ.get("USERPROFILE", ""),
    # Claude Code names a project's folder after its path with every non-alphanumeric turned into '-'.
    ".claude", "projects", re.sub(r"[^A-Za-z0-9]", "-", ROOT), "memory",
)
PR_BODY = os.path.join(ROOT, "exports", "upstream-pr9-body-20260924.md")

SKIP_DIR_NAMES = {".git", "external", "analysis", "node_modules"}

# Non-ASCII here is an ascii-policy note, separate from mojibake. Files that
# legitimately contain Chinese or an em dash are not in this list.
ASCII_ONLY = (
    "LmxxfNrRuntime.cpp", "LmxxfNrApi.h", "LmxxfProductionOptions.h",
    "native_input_geometry.h", "native_game_codec.h", "hip_d3d12_bridge.h",
    "lmxxf_nr_gpu.cpp", "lmxxf_bridge_zero_gpu.cpp", "lmxxf_nr_abi.cpp",
    "ContinuationState.h", "audit-lmxxf-enablements.py", "build-lmxxf-runtime.cmd",
    "scan-mojibake.py",
)

TEXT_EXT = (".cpp", ".h", ".ps1", ".py", ".cmd", ".md", ".c", ".hlsl", ".yml", ".json", ".ini")

CP1252_NAME = {
    0x91: "left single quote",
    0x92: "right single quote",
    0x93: "left double quote",
    0x94: "right double quote",
    0x95: "bullet",
    0x96: "en dash",
    0x97: "em dash",
}


def mojibake_hits(text):
    """Return (index, label) for signatures that cannot occur in healthy text."""
    hits = []
    i = 0
    n = len(text)
    while i < n:
        o = ord(text[i])
        if o == 0xFFFD:
            hits.append((i, "U+FFFD"))
        elif o == 0x9225:
            nxt = ord(text[i + 1]) if i + 1 < n else None
            if nxt is not None and 0xE000 <= nxt <= 0xF8FF:
                hits.append((i, "U+9225 U+%04X (punctuation swallowed the next byte)" % nxt))
                i += 2
                continue
            if nxt == 0x3F:
                hits.append((i, "U+9225 '?' (punctuation bytes that did not form a pair)"))
                i += 2
                continue
            hits.append((i, "U+9225 (GB18030 decode of UTF-8 E2 80)"))
        elif 0xE000 <= o <= 0xF8FF:
            hits.append((i, "U+%04X private-use" % o))
        elif o == 0x00E2 and i + 1 < n and ord(text[i + 1]) == 0x20AC:
            hits.append((i, "U+00E2 U+20AC (CP1252 decode of UTF-8 E2 80)"))
            i += 2
            continue
        elif o < 0x20 and text[i] not in "\t\n\r":
            hits.append((i, "control U+%04X" % o))
        i += 1
    return hits


def should_be_ascii(path):
    return any(path.replace("\\", "/").endswith(n) for n in ASCII_ONLY)


def snippet(text, index):
    start = max(0, index - 24)
    end = min(len(text), index + 24)
    return text[start:end].replace("\n", " ").replace("\r", " ").encode("unicode_escape").decode("ascii")


def report_text(rel, text, bad_box, ascii_box, invalid_box, invalid_note=None):
    if text is None:
        if invalid_note:
            invalid_box.append(rel)
            print("  BYTES  %s  %s" % (rel, invalid_note), flush=True)
        return
    hits = mojibake_hits(text)
    if hits:
        bad_box.append(rel)
        print("  BAD  %s" % rel, flush=True)
        shown = {}
        for index, label_hit in hits:
            shown.setdefault(label_hit, index)
        for label_hit, index in shown.items():
            count = sum(1 for _i, name in hits if name == label_hit)
            print("       %s x%d  %s" % (label_hit, count, snippet(text, index)), flush=True)
    elif should_be_ascii(rel):
        non_ascii = sum(1 for ch in text if ord(ch) > 0x7F)
        if non_ascii:
            ascii_box.append(rel)
            print("  ASCII  %s  non-ASCII x%d" % (rel, non_ascii), flush=True)


def scan_pairs(label, pairs):
    if pairs is None:
        print("== %s: skipped (path missing)" % label, flush=True)
        return 0
    bad_box = []
    ascii_box = []
    invalid_box = []
    total = 0
    for item in pairs:
        total += 1
        if len(item) == 3:
            rel, text, invalid_note = item
        else:
            rel, text = item
            invalid_note = None
        report_text(rel, text, bad_box, ascii_box, invalid_box, invalid_note)
    print("== %s: %d files, %d mojibake, %d not-utf8, %d ascii-policy" % (
        label, total, len(bad_box), len(invalid_box), len(ascii_box)), flush=True)
    return len(bad_box)


def skip_dir(path):
    parts = set(path.replace("\\", "/").split("/"))
    return bool(parts & SKIP_DIR_NAMES)


def classify_bytes(raw):
    """Return (text, invalid_note). text is None when the bytes are not UTF-8."""
    try:
        return raw.decode("utf-8"), None
    except UnicodeDecodeError as exc:
        byte = raw[exc.start]
        name = CP1252_NAME.get(byte)
        if name:
            note = "normal punctuation stored as Windows-1252 %s (byte 0x%02X at %d); transcode, do not delete" % (
                name, byte, exc.start)
        else:
            note = "not UTF-8: byte 0x%02X at %d" % (byte, exc.start)
        return None, note


def read_text(path):
    try:
        raw = io.open(path, "rb").read()
    except Exception:
        return None, None
    return classify_bytes(raw)


def disk_pairs(root, exts):
    if not os.path.isdir(root):
        return None
    out = []
    for dp, dirnames, fns in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_NAMES]
        if skip_dir(dp):
            continue
        for fn in fns:
            if fn.endswith(exts):
                path = os.path.join(dp, fn)
                text, note = read_text(path)
                out.append((path, text, note))
    return out


def file_pairs(path):
    if not os.path.isfile(path):
        return None
    text, note = read_text(path)
    return [(path, text, note)]


def git_pairs(repo, ref, exts):
    if not os.path.isdir(repo):
        return None
    try:
        names = subprocess.check_output(
            ["git", "-C", repo, "ls-tree", "-r", "--name-only", ref],
            stderr=subprocess.DEVNULL,
        ).decode("utf-8", "replace").splitlines()
    except Exception:
        return None
    names = [n for n in names if n.endswith(exts) and not skip_dir(n)]
    if not names:
        return []
    proc = subprocess.Popen(
        ["git", "-C", repo, "cat-file", "--batch"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    payload = "".join("%s:%s\n" % (ref, n) for n in names).encode("utf-8")
    stdout, _err = proc.communicate(payload)
    out = []
    pos = 0
    data = stdout
    for name in names:
        header_end = data.find(b"\n", pos)
        if header_end < 0:
            out.append((name, None))
            break
        header = data[pos:header_end].decode("utf-8", "replace")
        pos = header_end + 1
        if header.endswith("missing") or " " not in header:
            out.append((name, None))
            continue
        size = int(header.rsplit(" ", 1)[-1])
        blob = data[pos:pos + size]
        pos += size + 1
        text, note = classify_bytes(blob)
        out.append((name, text, note))
    return out


def main():
    total_bad = 0
    total_bad += scan_pairs("our repo working tree", disk_pairs(ROOT, TEXT_EXT))
    total_bad += scan_pairs("our repo HEAD", git_pairs(ROOT, "HEAD", TEXT_EXT))
    total_bad += scan_pairs("PR branch %s" % UPBRANCH, git_pairs(UP, UPBRANCH, TEXT_EXT))
    total_bad += scan_pairs("handoff", disk_pairs(os.path.join(ROOT, ".handoff"), (".md",)))
    total_bad += scan_pairs("memory", disk_pairs(MEM, (".md",)))
    total_bad += scan_pairs("PR body draft", file_pairs(PR_BODY))
    print("---", flush=True)
    print("TOTAL files with mojibake: %d" % total_bad, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
