"""Decode bounded F9 captures. Compare temporal changes within a stage/tile only.

Usage: python tools/diag/analyze-highlight.py capture.nrhl --out summary.csv
The final stage is NR's pre-SR output, not the game's presented/backbuffer image.
Tiles at different stages have different resolution/encoding and are not pixel-aligned.
"""
import argparse
import csv
import math
import struct
from pathlib import Path

HEADER = struct.Struct('<12I5fQ')
FIELDS = ('frame stage tile width height x y w h format exposure_source debug_view '
          'pre scale paper transfer color tick').split()
STAGES = ('game_input', 'encoded_input', 'neural_output', 'nr_composite', 'exposure')


def read_records(path):
    with open(path, 'rb') as f:
        if f.read(8) != b'NRHLV1\0\0':
            raise ValueError('Not an NRHLV1 capture')
        while raw := f.read(HEADER.size):
            if len(raw) != HEADER.size:
                raise ValueError('Truncated record header')
            r = dict(zip(FIELDS, HEADER.unpack(raw)))
            formats = {10: ('<4e', 8), 41: ('<f', 4), 54: ('<e', 2)}
            if r['format'] not in formats or r['stage'] >= len(STAGES):
                raise ValueError('Unsupported capture stage/format')
            if not (0 < r['w'] <= 32 and 0 < r['h'] <= 32 and
                    r['x'] + r['w'] <= r['width'] and r['y'] + r['h'] <= r['height']):
                raise ValueError('Invalid ROI bounds')
            fmt, bpp = formats[r['format']]
            payload = f.read(r['w'] * r['h'] * bpp)
            if len(payload) != r['w'] * r['h'] * bpp:
                raise ValueError('Truncated pixel payload')
            pixels = list(struct.iter_unpack(fmt, payload))
            values = [sum(c * weight for c, weight in zip(p[:3], (.2126, .7152, .0722)))
                      if len(p) == 4 else p[0] for p in pixels]
            yield r, values


def summaries(records):
    previous = {}
    for r, values in records:
        # Geometry changes break correspondence; never turn them into a flicker metric.
        key = tuple(r[k] for k in ('stage', 'tile', 'width', 'height', 'x', 'y', 'w', 'h', 'format'))
        old = previous.get(key)
        valid = sorted(v for v in values if math.isfinite(v))
        result = dict(r)
        result['stage'] = STAGES[r['stage']]
        result['nonfinite'] = len(values) - len(valid)
        result['mean'] = sum(valid) / len(valid) if valid else ''
        result['p95'] = valid[int((len(valid) - 1) * .95)] if valid else ''
        result['max'] = valid[-1] if valid else ''
        result['previous_frame'] = old[0] if old else ''
        result['mean_abs_delta'] = ''
        if old and r['frame'] == old[0] + 1:
            delta = [abs(a - b) for a, b in zip(values, old[1]) if math.isfinite(a) and math.isfinite(b)]
            result['mean_abs_delta'] = sum(delta) / len(delta) if delta else ''
        previous[key] = r['frame'], values
        yield result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    rows = list(summaries(read_records(args.capture)))
    if not rows:
        raise SystemExit('Capture contains no completed samples')
    with args.out.open('w', newline='', encoding='utf-8-sig') as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(f'{len(rows)} ROI records; {len({r["frame"] for r in rows})} sampled Evaluate calls -> {args.out}')
    print('Compare consecutive frames within each stage/tile. Camera motion and jitter also change pixels.')


if __name__ == '__main__':
    main()
