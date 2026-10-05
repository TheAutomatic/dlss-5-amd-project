"""Rebuild the embedded OFL Chinese menu font from the pinned Noto Sans SC source.

Requires fonttools. Usage: python tools/build/build-menu-font.py --source path/to/NotoSansSC[wght].ttf
The source download/clone belongs outside product code; it is not needed for normal builds.
"""
import argparse
import hashlib
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[2]
MENU = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    expected = 'a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da'
    if hashlib.sha256(args.source.read_bytes()).hexdigest() != expected:
        parser.error('Source differs from the pinned Noto Sans SC font; review provenance before updating it.')
    # Include catalog text and language-selector text. Missing catalog glyphs are tested in host CI.
    text = (MENU / 'MenuStrings.inl').read_text(encoding='utf-8') + '简体中文'
    font = TTFont(args.source, recalcTimestamp=False)
    font = instantiateVariableFont(font, {'wght': 400}, inplace=True)
    options = subset.Options()
    options.recalc_timestamp = False
    options.name_IDs = ['*']
    sub = subset.Subsetter(options=options)
    sub.populate(unicodes=sorted({ord(c) for c in text if ord(c) >= 0x2000}))
    sub.subset(font)
    # Distinct family for this modified subset. Keep original copyright/license records.
    for record in font['name'].names:
        names = {1: 'OptScaler Menu Sans', 2: 'Regular', 3: 'OptScalerMenuSans-Regular',
                 4: 'OptScaler Menu Sans Regular', 6: 'OptScalerMenuSans-Regular',
                 16: 'OptScaler Menu Sans', 17: 'Regular'}
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    out = MENU / 'font/NotoSansSC-Menu.ttf'
    font.save(out)
    print(f'{out}: {out.stat().st_size} bytes, {len(font.getBestCmap())} glyphs')

if __name__ == '__main__':
    main()
