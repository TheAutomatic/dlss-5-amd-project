"""Extract the supported Mochizuki model locally; never loads the NVIDIA DLL."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='User-supplied nvngx_dlssnr.dll 310.8.0, or its ZIP')
    parser.add_argument('output', type=Path, help='Destination dlssnr-amd/dlssnr.bin')
    parser.add_argument('--work', type=Path, help='Parent for temporary extraction files')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    vendor = here / 'model-tools'
    if not vendor.is_dir():
        vendor = here.parents[1] / 'third_party/mochizuki/linux/package/model-tools'
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or not source.is_file():
        parser.error('Source must be an existing DLL/ZIP distinct from the output')
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mochizuki-model-', dir=args.work) as tmp:
        work = Path(tmp)
        (work / 'graph').mkdir()
        shutil.copy2(vendor / 'descriptor.json', work / 'graph/descriptor.json')
        def run(script, *params, capture=False):
            return subprocess.run([sys.executable, '-X', 'utf8', str(vendor / script), *map(str, params)],
                                  cwd=vendor, check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                                  text=True, encoding='utf-8')
        result = run('inspect_nr.py', source, '--output', work / 'inventory', '--extract', capture=True)
        expected = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
        if json.loads(result.stdout)['dll_sha256'].lower() != expected:
            parser.error('The supported nvngx_dlssnr.dll 310.8.0 is required (SHA256 mismatch)')
        for script, folder, extra in [('unpack_swin_family.py', 'unpacked', ['--max-c', '256']),
                                      ('unpack_splitswin.py', 'unpacked-splitswin', []),
                                      ('unpack_vit.py', 'unpacked-vit', [])]:
            run(script, '--artifacts', work, '--output', work / folder, *extra)
        run('unpack_preblock.py', work / 'inventory/weights/block0.layer0.layer.bin', work / 'unpacked-preblock')
        run('unpack_postblock.py', work / 'inventory/weights/block70.layer0.layer.bin', work / 'unpacked-postblock')
        packed = work / 'dlssnr.bin'
        run('pack_model.py', '--root', work, '--list', vendor / 'model-files.txt', '--out', packed,
            '--verify', vendor / 'model-files.sha256')
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix='.mochizuki-', dir=output.parent, delete=False) as f:
            staged = Path(f.name)
        try:
            shutil.copyfile(packed, staged)
            staged.replace(output)
        finally:
            staged.unlink(missing_ok=True)
    print('MODEL_OK', output)

if __name__ == '__main__':
    main()
