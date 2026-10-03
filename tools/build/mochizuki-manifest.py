"""Bind the Mochizuki DLL/shaders to their exact source closure."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sources():
    paths = []
    product = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
    for folder in (ROOT / 'third_party/mochizuki/windows', product / 'dlssnr/backend/mochizuki_runtime'):
        paths.extend(p for p in folder.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    paths.extend([product / 'dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h', product / 'dlssnr/NrPerformance.h'])
    paths.extend(ROOT / 'tools/build' / f for f in ('build-mochizuki-runtime.py', 'build-mochizuki-runtime.cmd', 'mochizuki-deps.py', 'mochizuki-manifest.py'))
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(paths)}

def artifacts(out):
    paths = [out / 'MochizukiNrRuntime.dll']
    paths.extend(p for p in (out / 'dlssnr-amd/shaders').rglob('*') if p.is_file())
    return {p.relative_to(out).as_posix(): digest(p) for p in sorted(paths)}

def write(out):
    (out / 'build-manifest.json').write_text(json.dumps({'sources': sources(), 'artifacts': artifacts(out)}, indent=2)+'\n', encoding='utf-8')

def verify(out):
    manifest = json.loads((out / 'build-manifest.json').read_text(encoding='utf-8'))
    if manifest['sources'] != sources() or manifest['artifacts'] != artifacts(out):
        raise SystemExit('Mochizuki source/artifact mismatch: rebuild the runtime and shaders')
    print('MOCHIZUKI_BUILD_VERIFIED')

if __name__ == '__main__':
    verify(Path(sys.argv[1]).resolve())
