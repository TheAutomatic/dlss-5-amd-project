"""GPU check: an ACO multipass first-pass export equals independent single-pass output."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
assets = ROOT / 'exports/mochizuki-runtime'
out = ROOT / 'exports/mochizuki-tests'
for scale in ('1', '.5'):
    outputs = []
    for passes in ('1', '3'):
        output = out / f'aco-first-pass-{scale}-{passes}.raw'
        command = [str(out / 'runtime.exe'), str(assets / 'MochizukiNrRuntime.dll'), str(assets),
                   '--profile', str(output), '512', '384', scale, passes, '0', '1', '0', '--aco']
        if passes != '1': command.append('--first-pass')
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace', timeout=300)
        if result.returncode:
            raise RuntimeError(result.stdout)
        outputs.append(output.read_bytes())
    if outputs[0] != outputs[1]:
        raise RuntimeError('First-pass export differs at scale ' + scale)
    print('ACO_FIRST_PASS_OK scale=' + scale, flush=True)
