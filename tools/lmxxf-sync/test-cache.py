"""Reuse a successful tooling suite only for identical inputs/environment in this UTC week."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
# These tests read production fixtures as well as the tools themselves. Include
# entire owned directories so additions/deletions cannot keep an old receipt valid.
INPUTS = (
    'tests/sync', 'tests/_lib', 'tests/install/test_local_package.py', 'tests/run-all.cmd',
    'tools/lmxxf-sync', 'tools/sync-lmxxf-upstream.ps1', 'tools/audit-lmxxf-enablements.py',
    'tools/lmxxf-module-package.ps1', 'tools/release', 'tools/build',
    'third_party/lmxxf', '.gitattributes', '.gitignore',
    PRODUCT + '/ConfigKeys.h', PRODUCT + '/dlssnr/backend/lmxxf_runtime',
)


def fingerprint(root=ROOT, now=None):
    now = now or datetime.now(timezone.utc)
    files = {}
    for relative in INPUTS:
        path = root / relative
        if not path.exists():
            raise RuntimeError('Missing tooling test input: ' + relative)
        for p in sorted(path.rglob('*')) if path.is_dir() else [path]:
            if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc':
                files[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    ps = os.environ.get('LMXXF_TEST_POWERSHELL', 'powershell')
    ps_version = subprocess.check_output([ps, '-NoProfile', '-Command',
        '$PSVersionTable.PSVersion.ToString()'], text=True).strip()
    git_version = subprocess.check_output(['git', '--version'], text=True).strip()
    environment = [platform.system(), platform.version(), platform.machine(),
                   sys.version, ps_version, git_version,
                   os.environ.get('VCToolsVersion', ''), os.environ.get('WindowsSDKVersion', '')]
    payload = {'schema': 1, 'files': files, 'environment': environment,
               'week': now.strftime('%G-W%V')}
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def valid(receipt, key):
    try:
        data = json.loads(receipt.read_text(encoding='utf-8'))
        return data.get('schema') == 1 and data.get('key') == key and data.get('result') == 'passed'
    except (OSError, ValueError, AttributeError):
        return False


def run(root=ROOT, force=False):
    receipt = root / 'exports/test-cache/sync.json'
    key = fingerprint(root)
    if not force and valid(receipt, key):
        print('sync: REUSED (matching inputs/environment; full suite passed this UTC week)', flush=True)
        return 0
    receipt.unlink(missing_ok=True)
    start = time.monotonic()
    code = subprocess.call(['cmd', '/d', '/c', 'tests\\sync\\run-uncached.cmd'], cwd=root)
    if code:
        return code
    if fingerprint(root) != key:
        print('FAIL: tooling test inputs/environment changed during validation', file=sys.stderr)
        return 1
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps({'schema': 1, 'key': key, 'result': 'passed',
        'completed_utc': datetime.now(timezone.utc).isoformat(),
        'seconds': round(time.monotonic() - start, 3)}, indent=2) + '\n', encoding='utf-8')
    print('sync: fresh PASS recorded', flush=True)
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('key', 'run'))
    parser.add_argument('--force', action='store_true', help='ignore prior success and run all tooling tests')
    args = parser.parse_args()
    if args.mode == 'key':
        print(fingerprint())
    else:
        sys.exit(run(force=args.force))
