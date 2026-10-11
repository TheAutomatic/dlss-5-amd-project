"""Build the pinned Linux SPIR-V inputs and verify their packaged ACO records."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'third_party/mochizuki'


def fnv64(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def record_hash(data):
    if len(data) < 96 or data[:8] != b'NRPAL001':
        raise ValueError('Invalid ACO record header')
    spv_hash, code_hash = struct.unpack_from('<QQ', data, 8)
    inline_count, code_size = struct.unpack_from('<II', data, 68)
    if inline_count > 32 or not code_size or code_size % 4 or len(data) != 96 + inline_count * 8 + code_size:
        raise ValueError('Invalid ACO record size')
    if any(data[76:96]) or fnv64(data[96 + inline_count * 8:]) != code_hash:
        raise ValueError('Invalid ACO record checksum/reserved fields')
    return spv_hash


def verify(bundle):
    shaders = sorted((bundle / 'shaders').glob('g_*.spv'))
    shaders += sorted((bundle / 'shaders/temporal').glob('temporal_*fp32*.spv'))
    if not shaders:
        raise ValueError('ACO network shaders are missing')
    required = set()
    for shader in shaders:
        digest = fnv64(shader.read_bytes())
        name = f'{digest:016x}.nrp'
        if record_hash((bundle / 'records' / name).read_bytes()) != digest:
            raise ValueError('ACO SPIR-V/record mismatch: ' + shader.name)
        required.add(name)
    records = {p.name for p in (bundle / 'records').glob('*.nrp')}
    if required != records:
        raise ValueError('ACO record inventory does not match the shader recipes')
    if not (bundle / 'shell-aliases.txt').is_file():
        raise ValueError('ACO template aliases are missing')
    return len(shaders), len(records)


def build(glslang, bundle):
    bundle = bundle.resolve()
    # This function replaces only its own generated asset directory under exports.
    if not bundle.is_relative_to(ROOT / 'exports') or bundle == ROOT / 'exports':
        raise ValueError('ACO build output must be a subdirectory of exports')
    if bundle.exists():
        shutil.rmtree(bundle)
    shader_dir = bundle / 'shaders'
    (shader_dir / 'temporal').mkdir(parents=True)
    table = json.loads((VENDOR / 'linux/shaders/rdna4/pipelines.json').read_text(encoding='utf-8'))

    def compile_shader(source, defines, output):
        subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.3',
                        '-I' + str(VENDOR / 'linux/shaders/rdna4/include'),
                        *('-D' + d for d in defines), str(source), '-o', str(output)],
                       check=True, stdout=subprocess.DEVNULL)

    for name, entry in table['pipelines'].items():
        compile_shader(VENDOR / 'linux/shaders/rdna4' / entry['source'], entry['defines'], shader_dir / f'g_{name}.spv')
    for name, entry in table['variants'].items():
        base = table['pipelines'][entry['base']]
        compile_shader(VENDOR / 'linux/shaders/rdna4' / base['source'], base['defines'] + entry['add'],
                       shader_dir / 'temporal' / f'{name}.spv')
    for name in ('motion_luma', 'motion_estimate'):
        compile_shader(VENDOR / 'linux/shaders/passes' / f'{name}.comp', [], shader_dir / 'temporal' / f'{name}.spv')
    for name, value in table['markers'].items():
        (shader_dir / name).write_text('\n'.join(value) + '\n' if isinstance(value, list) else value + '\n', encoding='utf-8')
    shutil.copy2(shader_dir / 'shader-constants.txt', shader_dir / 'temporal/shader-constants.txt')
    shutil.copytree(VENDOR / 'windows/data/aco/records', bundle / 'records')
    shutil.copy2(VENDOR / 'windows/data/aco/shell-aliases.txt', bundle / 'shell-aliases.txt')
    shaders, records = verify(bundle)
    print(f'ACO_BUNDLE_VERIFIED: {shaders} shaders, {records} records')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glslang', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.verify:
        print('ACO_BUNDLE_VERIFIED:', verify(args.out))
    elif args.glslang:
        build(args.glslang, args.out)
    else:
        parser.error('--glslang is required when building')
