"""List MZ4 coverage for every key in the release template, PBZ ini and writer."""
import argparse, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CS = ROOT / "tools/install/launcher/MochizukiShareCode.cs"
TEMPLATE = ROOT / "third_party/mochizuki-asi/payload/ini/dlssnr-amd-optiscaler-renodx.ini"
DEFAULT_SOURCE = ROOT.parent / "DLSSNR-AMD-mochizuki"
WRITER = RENODX_KEYS = WILSJO2 = None

EXCLUDED = {
    ("Network", "Hotkey"): "target hotkey", ("Network", "Sound"): "target sound",
    ("Network", "Residency"): "memory policy",
    ("Network", "OSD"): "diagnostic OSD",
    ("Network", "Memory Guard"): "memory policy",
    ("Preprocess", "Hotkey"): "target hotkey", ("Preprocess", "Sound"): "target sound",
    ("RenoDX", "NRToggleKey"): "target hotkey", ("RenoDX", "NRScreenshotKey"): "target hotkey",
    ("RenoDX", "NRScreenshotSource"): "screenshot diagnostic", ("RenoDX", "NRScreenshotFormat"): "screenshot diagnostic",
    ("RenoDX", "NRScreenshotLayout"): "screenshot diagnostic", ("RenoDX", "NRScreenshotJpegQuality"): "screenshot diagnostic",
    ("RenoDX", "NRScreenshotMaxMB"): "screenshot diagnostic",
    ("HudFix", "ShowMask"): "debug mask", ("NRControl", "MenuKey"): "target hotkey",
    ("NRControl", "Language"): "launcher language",
}
LAYER_EXCLUDED = {
    "ToggleKey": "target hotkey", "NRToggleKey": "target hotkey", "NRPassesMode": "included",
}

def keys_from_array(text, name):
    m = re.search(r"static string\[\] " + name + r"\(\)\s*\{\s*return new\[\] \{(.*?)\};", text, re.S)
    return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()

def schema():
    text = CS.read_text(encoding="utf-8-sig")
    glob = set(re.findall(r'F\("([^"]+)",\s*"([^"]+)"', text))
    global_keys = {(s, k) for s, k in glob if s != "Layer"}
    m = re.search(r'foreach \(var key in new\[\] \{ ([^}]+) \}\)\s*\n\s*list\.Add\(F\("Preprocess", key', text)
    if m: global_keys |= {("Preprocess", k) for k in re.findall(r'"([^"]+)"', m.group(1))}
    layer_keys = {k for s, k in glob if s == "Layer"}
    m = re.search(r'foreach \(var key in new\[\] \{ ([^}]+) \}\)\s*\n\s*list\.Add\(F\("Layer", key', text)
    if m: layer_keys |= set(re.findall(r'"([^"]+)"', m.group(1)))
    reno = keys_from_array(text, "RenoDXKeys")
    wils = keys_from_array(text, "WilsjoKeys")
    global_keys |= {("RenoDX", k) for k in reno} | {("wilsjo2", k) for k in wils}
    layer_keys |= reno | wils
    m = re.search(r'static string\[\] DynamicPassKeys\(\).*?string\[\] fields = \{ (.*?) \};', text, re.S)
    if m:
        fields = re.findall(r'"([^"]+)"', m.group(1))
        for n in range(2, 31):
            for field in fields:
                if field == "Preset" and n > 3: continue
                key = "Pass" + str(n) + field
                global_keys.add(("wilsjo2", key)); layer_keys.add(key)
    return global_keys, layer_keys

def parse_ini(path):
    sections = []
    section = None
    if not path.is_file(): return sections
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        m = re.match(r"^\s*\[([^\]]+)\]", line)
        if m:
            section = m.group(1).strip(); sections.append((section, [])); continue
        m = re.match(r"^\s*([^=;#]+?)\s*=", line)
        if m and sections: sections[-1][1].append(m.group(1).strip())
    return sections

def writer_keys():
    if not WRITER.is_file(): return set()
    text = WRITER.read_text(encoding="utf-8", errors="replace")
    return {m.strip() for m in re.findall(r'"([A-Za-z][A-Za-z0-9 ]*)\s*=', text)}

def consumer_wilsjo():
    if not WILSJO2.is_file(): return set()
    text = WILSJO2.read_text(encoding="utf-8", errors="replace")
    keys = {k.lower() for k in re.findall(r'if \(key == "([^"]+)"\)', text)}
    m = re.search(r'static const char\* const known\[\] = \{(.*?)\};', text, re.S)
    if m: keys |= {k.lower() for k in re.findall(r'"([^"]+)"', m.group(1))}
    m = re.search(r'static const char\* const fields\[\] = \{(.*?)\};', text, re.S)
    fields = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    for n in range(2, 31):
        for field in fields:
            if field == "preset" and n > 3: continue
            keys.add(("pass" + str(n) + field).lower())
    return keys

def consumer_renodx():
    if not RENODX_KEYS.is_file(): return set()
    text = RENODX_KEYS.read_text(encoding="utf-8", errors="replace")
    return {k.lower() for k in re.findall(r'\{\s*"([A-Za-z0-9]+)"', text)}

def main(argv=None):
    global WRITER, RENODX_KEYS, WILSJO2
    parser = argparse.ArgumentParser(description="MZ4 consumer coverage")
    parser.add_argument("--source-root", default=str(DEFAULT_SOURCE),
                        help="DLSSNR-AMD source checkout (default: sibling DLSSNR-AMD-mochizuki)")
    parser.add_argument("--game-ini", default=None,
                        help="optional game dlssnr-amd.ini to include; omitted means not checked")
    args = parser.parse_args(argv)
    source = Path(args.source_root)
    WILSJO2 = source / "windows/src/core/nr_wilsjo2.cpp"
    RENODX_KEYS = source / "windows/src/core/nr_renodx_keys.hpp"
    WRITER = source / "windows/src/pe/nr_pe_config.cpp"
    for required in (WILSJO2, RENODX_KEYS, WRITER):
        if not required.is_file():
            print("ERROR: consumer source file missing: " + str(required), file=sys.stderr)
            return 2
    if not TEMPLATE.is_file():
        print("ERROR: release template missing: " + str(TEMPLATE), file=sys.stderr)
        return 2
    game = None
    if args.game_ini:
        game = Path(args.game_ini)
        if not game.is_file():
            print("ERROR: game ini not found: " + str(game), file=sys.stderr)
            return 2
    global_keys, layer_keys = schema()
    missing = []
    rows = []
    sources = [("template", TEMPLATE)]
    if game is not None: sources.append(("game", game))
    for label, path in sources:
        for section, keys in parse_ini(path):
            layer = re.fullmatch(r"Layer(\d+)", section, re.I)
            if layer:
                for key in keys:
                    if key in layer_keys: status, why = "included", ""
                    elif key in LAYER_EXCLUDED and LAYER_EXCLUDED[key] != "included": status, why = "excluded", LAYER_EXCLUDED[key]
                    else: status, why = "MISSING", "not in layer schema"; missing.append((label, section, key))
                    rows.append((label, section, key, status, why))
                continue
            for key in keys:
                if (section, key) in global_keys: status, why = "included", ""
                elif (section, key) in EXCLUDED: status, why = "excluded", EXCLUDED[(section, key)]
                elif section in ("Inspect", "Log", "Debug"):
                    status, why = "excluded", "inspection/log/diagnostic section"
                else: status, why = "MISSING", "not in global schema"; missing.append((label, section, key))
                rows.append((label, section, key, status, why))
    for key in sorted(writer_keys()):
        if key in ("DlssNr",): continue
        if any(k == key for _, k in global_keys) or key in layer_keys or key in LAYER_EXCLUDED: continue
        if key in ("Enabled", "Exposure", "ExposureBias", "Curve", "Contrast", "Saturation", "Inverse", "Model", "Placement", "Processing"): continue
        # Writer keys not present in the release template/PBZ are reported, not silently dropped.
        rows.append(("writer", "?", key, "source-only", "not present in pre1 template/PBZ; not claimed as pre1 support"))
    consumer = set()
    for key in sorted(consumer_wilsjo()):
        consumer.add(("wilsjo2", key))
        if any(sec == "wilsjo2" and str(k).lower() == key for sec, k in global_keys):
            rows.append(("consumer", "wilsjo2", key, "included", ""))
        else:
            missing.append(("consumer", "wilsjo2", key)); rows.append(("consumer", "wilsjo2", key, "MISSING", "consumer parser key not in schema"))
    for key in sorted(consumer_renodx()):
        consumer.add(("RenoDX", key))
        if any(str(k).lower() == key for sec, k in global_keys if sec == "RenoDX"):
            rows.append(("consumer", "RenoDX", key, "included", ""))
        elif (("RenoDX", key.title()) in EXCLUDED or ("RenoDX", key) in EXCLUDED or key in ("nrtogglekey", "nrscreenshotkey", "nrscreenshotsource", "nrscreenshotformat", "nrscreenshotlayout", "nrscreenshotjpegquality", "nrscreenshotmaxmb")):
            rows.append(("consumer", "RenoDX", key, "excluded", "target hotkey/screenshot"))
        else:
            missing.append(("consumer", "RenoDX", key)); rows.append(("consumer", "RenoDX", key, "MISSING", "consumer key not in schema"))
    print("| source | section | key | status | reason |")
    print("|---|---|---|---|---|")
    for row in rows: print("| " + " | ".join(row) + " |")
    print(f"\n{len(rows)} rows, {len(missing)} missing; consumer keys: wilsjo2={len(consumer_wilsjo())}, renodx={len(consumer_renodx())}")
    print("game ini: " + (str(game) if game is not None else "not checked (--game-ini not supplied)"))
    return 1 if missing else 0

if __name__ == "__main__":
    raise SystemExit(main())
