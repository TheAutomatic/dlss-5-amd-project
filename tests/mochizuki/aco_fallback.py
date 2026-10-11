"""GPU integration: absent/corrupt optional ACO assets rebuild a complete native graph."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
assets = ROOT / 'exports/mochizuki-runtime'
test = ROOT / 'exports/mochizuki-tests/runtime.exe'
with tempfile.TemporaryDirectory(prefix='aco-fallback-', dir=ROOT / 'exports/mochizuki-tests') as temp:
    fixture = Path(temp)
    data = fixture / 'dlssnr-amd'
    shutil.copytree(assets / 'dlssnr-amd/shaders', data / 'shaders')
    os.link(assets / 'dlssnr-amd/dlssnr.bin', data / 'dlssnr.bin')

    def run(label):
        result = subprocess.run([str(test), str(assets / 'MochizukiNrRuntime.dll'), str(fixture),
                                 '--startup', '', '256', '256', '--aco-fallback'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace', timeout=300)
        if result.returncode:
            raise RuntimeError(label + '\n' + result.stdout)
        print('ACO_FALLBACK_OK', label, flush=True)

    run('missing bundle')
    shutil.copytree(assets / 'dlssnr-amd/aco', data / 'aco')
    aliases = data / 'aco/shell-aliases.txt'
    original = aliases.read_bytes()
    aliases.unlink()
    run('missing aliases')
    aliases.write_bytes(original)
    # Corrupt every record so whichever kernel is built first must take the failure path.
    for record in (data / 'aco/records').glob('*.nrp'):
        record.write_bytes(b'corrupt test record')
    run('corrupt binary record')
