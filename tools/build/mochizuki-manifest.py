"""Bind the Mochizuki DLL/shaders to their exact source closure."""
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

ROOT = Path(__file__).resolve().parents[2]
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sources():
    paths = []
    product = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
    for folder in (ROOT / 'third_party/mochizuki/windows', ROOT / 'third_party/mochizuki/linux/shaders',
                   product / 'dlssnr/backend/mochizuki_runtime'):
        paths.extend(p for p in folder.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    paths.extend([product / 'dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h', product / 'dlssnr/NrPerformance.h'])
    paths.append(product / 'library/vulkan/vulkan-1.lib')
    paths.extend(ROOT / 'tools/build' / f for f in ('build-mochizuki-runtime.py', 'build-mochizuki-runtime.cmd', 'mochizuki-deps.py', 'mochizuki-manifest.py', 'mochizuki-aco.py'))
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(paths)}

def aco_paths():
    vendor = ROOT / 'third_party/mochizuki'
    table = json.loads((vendor / 'linux/shaders/rdna4/pipelines.json').read_text(encoding='utf-8'))
    names = ['shell-aliases.txt', 'shaders/temporal/shader-constants.txt',
             'shaders/temporal/motion_luma.spv', 'shaders/temporal/motion_estimate.spv']
    names.extend('shaders/g_' + name + '.spv' for name in table['pipelines'])
    names.extend('shaders/temporal/' + name + '.spv' for name in table['variants'])
    names.extend('shaders/' + name for name in table['markers'])
    records = sorted((vendor / 'windows/data/aco/records').glob('*.nrp'))
    if not records:
        raise ValueError('Vendored ACO records are missing')
    names.extend('records/' + p.name for p in records)
    return [Path('dlssnr-amd/aco') / name for name in names]


def artifacts(out):
    for relative in aco_paths():
        if not (out / relative).is_file():
            raise ValueError('Incomplete Mochizuki ACO bundle: ' + relative.as_posix())
    paths = [out / 'MochizukiNrRuntime.dll']
    paths.extend(p for p in (out / 'dlssnr-amd/shaders').rglob('*') if p.is_file())
    paths.extend(p for p in (out / 'dlssnr-amd/aco').rglob('*') if p.is_file())
    return {p.relative_to(out).as_posix(): digest(p) for p in sorted(paths)}

def cache_key():
    # Compiler/SDK are pinned by the build environment, not inferred from the DLL date.
    value = [sources(), platform.system(), platform.machine(),
             os.environ.get('VCToolsVersion', ''), os.environ.get('WindowsSDKVersion', ''),
             sys.version]
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def write(out):
    (out / 'build-manifest.json').write_text(json.dumps({'sources': sources(), 'artifacts': artifacts(out),
        'build_key': cache_key()}, indent=2)+'\n', encoding='utf-8')


def reusable(out):
    try:
        data = json.loads((out / 'build-manifest.json').read_text(encoding='utf-8'))
        if data.get('build_key') != cache_key():
            return False
        verify(out)
        return True
    except (OSError, ValueError, KeyError, AttributeError, TypeError, SystemExit):
        return False

def verify(out):
    manifest = json.loads((out / 'build-manifest.json').read_text(encoding='utf-8'))
    if manifest['sources'] != sources() or manifest['artifacts'] != artifacts(out):
        raise SystemExit('Mochizuki source/artifact mismatch: rebuild the runtime and shaders')
    print('MOCHIZUKI_BUILD_VERIFIED')

if __name__ == '__main__':
    if sys.argv[1] == '--cache-key':
        print(cache_key())
    else:
        verify(Path(sys.argv[1]).resolve())
