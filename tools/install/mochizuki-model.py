"""Extract supported Mochizuki models locally; never loads the NVIDIA DLL."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

DLL_3108 = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
DLL_3109 = '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f'

ORIGINAL_SHA256 = '2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd'
SMALL_SHA256 = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33'
ORIGINAL_ENTRIES = 599
SMALL_ENTRIES = 255
ORIGINAL_SIZE = 147756560
SMALL_SIZE = 113917248

PROFILES = {
    'original': {
        'resource': 'WEIGHTS_HT',
        'suffix': '',
        'post': 70,
        'filename': 'dlssnr.bin',
        'entries': ORIGINAL_ENTRIES,
        'full_sha256': ORIGINAL_SHA256,
        'full_size': ORIGINAL_SIZE,
    },
    'small': {
        'resource': 'WEIGHTS_CC_SMALL_FP8',
        'suffix': '-small',
        'post': 30,
        'filename': 'dlssnr-small.bin',
        'entries': SMALL_ENTRIES,
        'full_sha256': SMALL_SHA256,
        'full_size': SMALL_SIZE,
    }
}


def verify_model_pack(path, family=None, vendor=None):
    """Verify a model pack binary by full-file SHA256 or structural index + per-tensor SHA256."""
    p = Path(path)
    if not p.is_file():
        return False
    size = p.stat().st_size
    if size < 16:
        return False

    # Fast path: deterministic whole-file SHA256
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    actual_hash = h.hexdigest().lower()

    if family == 'original':
        if actual_hash == ORIGINAL_SHA256 and size == ORIGINAL_SIZE:
            return True
    elif family == 'small':
        if actual_hash == SMALL_SHA256 and size == SMALL_SIZE:
            return True
    elif family is None:
        if actual_hash == ORIGINAL_SHA256 and size == ORIGINAL_SIZE:
            return 'original'
        if actual_hash == SMALL_SHA256 and size == SMALL_SIZE:
            return 'small'

    # Fallback path: parse binary header and validate index
    with open(p, 'rb') as f:
        magic = f.read(8)
        if magic != b'NRMODEL1':
            return False
        count_bytes = f.read(4)
        reserved_bytes = f.read(4)
        if len(count_bytes) != 4 or len(reserved_bytes) != 4:
            return False
        count, reserved = struct.unpack('<II', count_bytes + reserved_bytes)
        if reserved != 0:
            return False

        detected_family = None
        if count == ORIGINAL_ENTRIES:
            detected_family = 'original'
        elif count == SMALL_ENTRIES:
            detected_family = 'small'
        else:
            return False

        if family and family != detected_family:
            return False

        entries = {}
        for _ in range(count):
            nb = f.read(4)
            if len(nb) != 4:
                return False
            n = struct.unpack('<I', nb)[0]
            if n == 0 or n > 4096:
                return False
            name_bytes = f.read(n)
            off_size = f.read(16)
            if len(name_bytes) != n or len(off_size) != 16:
                return False
            try:
                name = name_bytes.decode('utf-8')
            except UnicodeDecodeError:
                return False
            off, entry_size = struct.unpack('<QQ', off_size)
            if off + entry_size > size or off < 16:
                return False
            if name in entries:
                return False
            entries[name] = (off, entry_size)

        if not vendor:
            vendor = model_tools_dir()

        if not vendor:
            return False

        suffix = PROFILES[detected_family]['suffix']
        verify_file = Path(vendor) / f'model-files{suffix}.sha256'
        if not verify_file.is_file():
            return False

        expected_sums = {}
        for line in verify_file.read_text(encoding='utf-8').splitlines():
            if line.strip():
                eh, en = line.split(None, 1)
                expected_sums[en.strip()] = eh.strip().lower()
        if len(expected_sums) != count:
            return False
        for name, expected_h in expected_sums.items():
            if name not in entries:
                return False
            off, entry_size = entries[name]
            f.seek(off)
            blob = f.read(entry_size)
            if hashlib.sha256(blob).hexdigest().lower() != expected_h:
                return False

        return detected_family if family is None else True


def extract(source, output, vendor, models='both', temporary_parent=None):
    """Extract requested Mochizuki model(s) locally from user's supported DLL or ZIP."""
    if isinstance(models, Path) or (temporary_parent is None and models not in ('both', 'small', 'original')):
        temporary_parent = models
        models = 'both'

    if models not in ('both', 'small', 'original'):
        raise ValueError(f"Invalid models choice: {models}. Expected 'both', 'small', or 'original'.")

    output_path = Path(output)
    if output_path.is_dir() or output_path.suffix == '':
        orig_dest = output_path / 'dlssnr.bin'
        small_dest = output_path / 'dlssnr-small.bin'
    elif output_path.name.lower() == 'dlssnr-small.bin':
        small_dest = output_path
        orig_dest = output_path.with_name('dlssnr.bin')
    else:
        orig_dest = output_path
        small_dest = output_path.with_name('dlssnr-small.bin')

    with tempfile.TemporaryDirectory(prefix='mochizuki-model-', dir=temporary_parent) as tmp:
        work = Path(tmp)

        if zipfile.is_zipfile(source):
            with zipfile.ZipFile(source) as archive:
                entries = archive.infolist()
                if len(entries) > 10000:
                    raise ValueError("ZIP contains too many entries (limit: 10000)")
                total_uncompressed = 0
                matches = []
                for i in entries:
                    if ".." in i.filename or i.filename.startswith(("/", "\\")):
                        raise ValueError("Unsafe path traversal in ZIP archive")
                    total_uncompressed += i.file_size
                    if total_uncompressed > 2 * 1024 ** 3:
                        raise ValueError("Total uncompressed ZIP size exceeds 2 GiB limit")
                    if Path(i.filename).name.lower() == "nvngx_dlssnr.dll":
                        if i.file_size > 1024 ** 3:
                            raise ValueError("DLL exceeds 1 GiB inspection limit")
                        matches.append(i)
                if not matches:
                    raise ValueError("expected at least one nvngx_dlssnr.dll ZIP member")

                candidates = []
                seen_shas = {}
                for m in matches:
                    b = archive.read(m)
                    h = hashlib.sha256(b).hexdigest().lower()
                    if h not in seen_shas:
                        seen_shas[h] = (m.filename, b)
                        candidates.append((m.filename, b, h))

                qualifying = []
                for fname, b, h in candidates:
                    if h == DLL_3109:
                        qualifying.append((fname, b, h))
                    elif h == DLL_3108 and models == 'original':
                        qualifying.append((fname, b, h))

                if not qualifying:
                    if any(h == DLL_3108 for _, _, h in candidates) and models in ('both', 'small'):
                        raise ValueError(
                            'Original model only: this source cannot provide the new model.\n'
                            'To use the new model, provide nvngx_dlssnr.dll 310.9.16.0.\n'
                            'You may continue with the original model by explicitly selecting it.'
                        )
                    raise ValueError('nvngx_dlssnr.dll 310.9.16 or 310.8.0 is required (SHA256 mismatch)')

                if len(qualifying) == 1:
                    chosen_bytes = qualifying[0][1]
                elif models == 'original' and any(h == DLL_3109 for _, _, h in qualifying):
                    chosen_bytes = next(b for _, b, h in qualifying if h == DLL_3109)
                else:
                    cand_desc = ", ".join(f"{fn} ({h[:8]})" for fn, _, h in qualifying)
                    raise ValueError(f"ambiguous ZIP members matching requested models ({models}): {cand_desc}")

                chosen_dll = work / 'nvngx_dlssnr.dll'
                chosen_dll.write_bytes(chosen_bytes)
                source = chosen_dll

        def run(script, *params, capture=False):
            return subprocess.run([sys.executable, '-X', 'utf8', str(vendor / script), *map(str, params)],
                                  cwd=vendor, check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                                  text=True, encoding='utf-8')

        probe = run('inspect_nr.py', source, '--output', work / 'probe', '--extract-weights', capture=True)
        sha = json.loads(probe.stdout)['dll_sha256'].lower()
        if sha not in (DLL_3108, DLL_3109):
            raise ValueError('nvngx_dlssnr.dll 310.9.16 or 310.8.0 is required (SHA256 mismatch)')

        if sha == DLL_3108 and models in ('both', 'small'):
            raise ValueError(
                'Original model only: this source cannot provide the new model.\n'
                'To use the new model, provide nvngx_dlssnr.dll 310.9.16.0.\n'
                'You may continue with the original model by explicitly selecting it.'
            )

        models_to_pack = []
        if models in ('both', 'original'):
            models_to_pack.append(('WEIGHTS_HT', '', 70, orig_dest))
        if models in ('both', 'small'):
            models_to_pack.append(('WEIGHTS_CC_SMALL_FP8', '-small', 30, small_dest))

        packed_models = []
        # Validate every requested model before replacing any installed pack.
        for resource, suffix, post, destination in models_to_pack:
            model_work = work / resource
            (model_work / 'graph').mkdir(parents=True)
            shutil.copy2(vendor / ('descriptor' + suffix + '.json'), model_work / 'graph/descriptor.json')
            run('inspect_nr.py', source, '--output', model_work / 'inventory', '--extract-weights', '--resource', resource)
            for script, folder, extra in [('unpack_swin_family.py', 'unpacked', ['--max-c', '256']),
                                          ('unpack_splitswin.py', 'unpacked-splitswin', []),
                                          ('unpack_vit.py', 'unpacked-vit', [])]:
                run(script, '--artifacts', model_work, '--output', model_work / folder, *extra)
            run('unpack_preblock.py', model_work / 'inventory/weights/block0.layer0.layer.bin', model_work / 'unpacked-preblock')
            run('unpack_postblock.py', model_work / f'inventory/weights/block{post}.layer0.layer.bin', model_work / 'unpacked-postblock')
            packed = model_work / destination.name
            run('pack_model.py', '--root', model_work, '--list', vendor / ('model-files' + suffix + '.txt'),
                '--out', packed, '--verify', vendor / ('model-files' + suffix + '.sha256'))
            packed_models.append((packed, destination))

        staged_models = []
        backups = []
        preserve_backups = set()
        try:
            for packed, destination in packed_models:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(prefix='.mochizuki-', dir=destination.parent, delete=False) as f:
                    staged = Path(f.name)
                staged_models.append((staged, destination))
                shutil.copyfile(packed, staged)

            for _, destination in staged_models:
                dest_backup = None
                existed = destination.is_file()
                if existed:
                    with tempfile.NamedTemporaryFile(prefix=f'.bak-tx-{destination.name}-',
                                                     dir=destination.parent, delete=False) as f:
                        dest_backup = Path(f.name)
                    try:
                        shutil.copyfile(destination, dest_backup)
                    except Exception:
                        dest_backup.unlink(missing_ok=True)
                        raise
                backups.append((destination, dest_backup, existed))

            completed = []
            try:
                for staged, destination in staged_models:
                    staged.replace(destination)
                    completed.append(destination)
            except Exception as install_error:
                restore_errors = []
                for destination, dest_backup, existed in reversed(backups):
                    if destination in completed:
                        try:
                            if existed and dest_backup and dest_backup.is_file():
                                dest_backup.replace(destination)
                            elif existed:
                                raise OSError(f'Original backup is missing: {dest_backup}')
                            else:
                                destination.unlink(missing_ok=True)
                        except OSError as restore_error:
                            if dest_backup:
                                preserve_backups.add(dest_backup)
                            restore_errors.append(f'{destination}: {restore_error}; backup retained at {dest_backup}')
                if restore_errors:
                    raise RuntimeError('Model installation failed; restoration was incomplete.\n' +
                                       '\n'.join(restore_errors)) from install_error
                raise

            for _, destination in staged_models:
                print('MODEL_OK', destination)
        finally:
            for staged, _ in staged_models:
                staged.unlink(missing_ok=True)
            for _, dest_backup, _ in backups:
                if dest_backup and dest_backup not in preserve_backups:
                    dest_backup.unlink(missing_ok=True)


VENDOR_TOOL_MARKER = 'inspect_nr.py'


def model_tool_dirs():
    """Where the vendor model tools live, in the layouts we ship or develop in.

    The extractor itself is staged under model-tools/ in the package, so its own
    directory IS the tool directory there; the tools/install checkout keeps the
    vendor copy under third_party.
    """
    here = Path(__file__).resolve().parent
    return [
        here,
        here / 'model-tools',
        here.parent / 'model-tools',
        here.parents[1] / 'third_party/mochizuki/linux/package/model-tools',
    ]


def model_tools_dir():
    """First candidate that actually holds the vendor tools.

    Being a directory is not enough: tools/install is a directory in a source
    checkout but holds no inspect_nr.py, which silently selected it and made the
    Python extraction fail. Prefer a candidate with the marker file; fall back to
    the first existing directory only so the caller can report a missing tool.
    """
    candidates = model_tool_dirs()
    for candidate in candidates:
        if (candidate / VENDOR_TOOL_MARKER).is_file():
            return candidate
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path, help='User-supplied nvngx_dlssnr.dll 310.9.16 / 310.8.0, or its ZIP')
    parser.add_argument('output', nargs='?', type=Path, help='Destination dlssnr-amd/dlssnr.bin; new model is written beside it')
    parser.add_argument('--models', choices=['both', 'small', 'original'], help='Model set to extract: both, small, or original (required for extraction)')
    parser.add_argument('--verify-pack', type=Path, help='Verify an existing model pack file')
    parser.add_argument('--family', choices=['small', 'original'], help='Expected model family for --verify-pack')
    parser.add_argument('--work', type=Path, help='Parent for temporary extraction files')
    args = parser.parse_args()

    vendor = model_tools_dir()

    if args.verify_pack:
        res = verify_model_pack(args.verify_pack, family=args.family, vendor=vendor)
        if not res:
            sys.exit(f"VERIFY_FAILED: {args.verify_pack} is not a valid {args.family or ''} model pack")
        print(f"VERIFY_OK {args.verify_pack} family={res if isinstance(res, str) else args.family}")
        return

    if not args.source or not args.output:
        parser.error('the following arguments are required: source, output')
    if not args.models:
        parser.error('the following arguments are required: --models')

    source, output = args.source.resolve(), args.output.resolve()
    if source in (output, output.with_name('dlssnr-small.bin')) or not source.is_file():
        parser.error('Source must be an existing DLL/ZIP distinct from the output')
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
    try:
        extract(source, output, vendor, models=args.models, temporary_parent=args.work)
    except ValueError as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
