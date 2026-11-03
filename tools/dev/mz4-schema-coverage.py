"""List MZ4 coverage for every key in the release template, PBZ ini and writer."""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CS = ROOT / "tools/install/launcher/MochizukiShareCode.cs"
TEMPLATE = ROOT / "third_party/mochizuki-asi/payload/ini/dlssnr-amd-optiscaler-renodx.ini"
PBZ = Path(r"C:\Program Files (x86)\Steam\steamapps\common\PBZ\platform\steam\dlssnr-amd.ini")
WRITER = Path(r"C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\pe\nr_pe_config.cpp")

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

def main():
    global_keys, layer_keys = schema()
    missing = []
    rows = []
    for label, path in (("template", TEMPLATE), ("PBZ", PBZ)):
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
    print("| source | section | key | status | reason |")
    print("|---|---|---|---|---|")
    for row in rows: print("| " + " | ".join(row) + " |")
    print(f"\n{len(rows)} keys checked, {len(missing)} missing")
    return 1 if missing else 0

if __name__ == "__main__":
    raise SystemExit(main())
