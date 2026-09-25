"""Small, non-executable module bundles for entrypoint and integrity tests."""
from contextlib import contextmanager
import ctypes
import hashlib
import json
from pathlib import Path
import shutil


MODULE_NAMES = (
    'boundary-fast', 'boundary_reference', 'c32_fast', 'c32_fast_attention',
    'c32_fused_attention', 'c32_fused_ffn_attention-packed', 'c32_fused_ffn_attention',
    'c32_prefix_reference', 'c32_tiled', 'c32_wmma', 'deep_fast-packed', 'deep_fast',
    'deep_reference', 'deep_wmma', 'multihead-fast-packed',
    'multihead-fast-padded-wave-packed', 'multihead-fast-padded-wave', 'multihead-fast',
    'multihead-reference', 'multihead-tiled', 'multihead-wmma',
    'multihead_fused_attention', 'prefix_fast', 'wave-pointwise',
)
ARCHES = ('gfx1200', 'gfx1201')


def make_modules(directory, marker='fixture', runtime_manifest=True):
    directory = Path(directory)
    root_rows = []
    for arch in ARCHES:
        leaf = directory / arch
        leaf.mkdir(parents=True, exist_ok=True)
        rows, metadata = [], []
        for name in MODULE_NAMES:
            data = f'{marker} {arch} {name}; not GPU code'.encode('ascii')
            (leaf / (name + '.hsaco')).write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            rows.append(f'{digest}  {name}.hsaco')
            root_rows.append(f'{digest}  {arch}/{name}.hsaco')
            metadata.append(dict(target=arch, module=name, sha256=digest,
                                 sources='fixture.hip', defines='HIP_ISA_HALF 1'))
        (leaf / 'SHA256SUMS').write_text('\n'.join(rows) + '\n', encoding='utf-8')
        (leaf / 'modules.json').write_text(json.dumps(metadata), encoding='utf-8')
    (directory / 'SHA256SUMS').write_text('\n'.join(root_rows) + '\n', encoding='utf-8')
    if runtime_manifest:
        (directory / 'runtime-manifest.json').write_text(json.dumps(dict(
            schema=2, runtime_abi=1, targets=list(ARCHES), module_count=48,
            module_count_per_arch=24, upstream_commit='0' * 40)), encoding='utf-8')
    return directory


def damage_modules(directory, kind):
    """Faults preserve all unrelated evidence, including hashes where relevant."""
    directory = Path(directory)
    if kind == 'corrupt':
        (directory / 'gfx1200/c32_fast.hsaco').write_bytes(b'corrupt module')
    elif kind == 'missing-arch':
        target = directory / 'gfx1201'
        assert target.resolve().parent == directory.resolve() and not target.is_symlink()
        shutil.rmtree(target)
    elif kind in ('missing-root', 'missing-leaf', 'missing-metadata', 'missing-runtime'):
        relative = {'missing-root': 'SHA256SUMS', 'missing-leaf': 'gfx1201/SHA256SUMS',
                    'missing-metadata': 'gfx1200/modules.json',
                    'missing-runtime': 'runtime-manifest.json'}[kind]
        (directory / relative).unlink()
    elif kind in ('root-mismatch', 'leaf-mismatch', 'duplicate', 'traversal', 'incomplete-root'):
        path = directory / ('gfx1200/SHA256SUMS' if kind == 'leaf-mismatch' else 'SHA256SUMS')
        rows = path.read_text().splitlines()
        if kind == 'duplicate':
            rows.append(rows[0])
        elif kind == 'traversal':
            rows[0] = '0' * 64 + '  gfx1200/../escape.hsaco'
        elif kind == 'incomplete-root':
            rows = rows[:24]
        else:
            rows[0] = '0' * 64 + rows[0][64:]
        path.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    elif kind == 'rename':
        (directory / 'gfx1200/prefix_fast.hsaco').rename(directory / 'gfx1200/unknown.hsaco')
        for relative in ('SHA256SUMS', 'gfx1200/SHA256SUMS', 'gfx1200/modules.json'):
            path = directory / relative
            path.write_text(path.read_text().replace('prefix_fast', 'unknown'), encoding='utf-8')
    elif kind == 'wrong-metadata':
        path = directory / 'gfx1200/modules.json'
        data = json.loads(path.read_text())
        data[0]['target'] = 'gfx1201'
        path.write_text(json.dumps(data), encoding='utf-8')
    elif kind == 'flat':
        (directory / 'legacy.hsaco').write_bytes(b'legacy')
    else:
        raise ValueError(kind)


def snapshot_files(directory):
    return {p.relative_to(directory).as_posix(): p.read_bytes()
            for p in Path(directory).rglob('*') if p.is_file()}


@contextmanager
def locked_file(path, share=0, directory=False):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x80000000, share,
                                None, 3, 0x02000000 if directory else 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        kernel.CloseHandle(handle)
