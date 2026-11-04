"""Rebuild fsrd_preprocess shaders: cso + generated header (listing prefix + byte array).

Mirrors the layout the existing precompiled headers use, so the checked-in artifacts stay
in the same shape: an `#if 0` DXIL listing for reference, then the cso bytes.
FSRDInputConv builds both the original and enabled additive variants so an edit to
their shared source cannot leave either conversion PSO with a stale artifact.
"""
import subprocess
import sys
import os
import argparse
import io
import tempfile
from concurrent.futures import ThreadPoolExecutor
from fsrd_toolchain import dxc

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PRE = "OptiScaler/shaders/fsrd_preprocess/precompile"
VERIFY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_fsrd_mirrors.py")

TARGETS = [
    'FSRDFloorSeed', 'FSRDFloor', 'FSRDInputConv', 'FSRDInputConvAdditive',
    'RRTraceAdditive', 'FSRDOutputComp', 'FSRDOutputCompLight',
    'FSRDOutputCompNoRecovery', 'FSRDOutputCompTileLight', 'FSRDOutputCompTileAnchor',
    'FSRDAlbedoTrustEvidence', 'FSRDAlbedoTrustPropagate', 'FSRDUpscalerDepth'
]


def publish_if_changed(path, data):
    if os.path.isfile(path):
        with open(path, 'rb') as stream:
            if stream.read() == data:
                return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=os.path.dirname(path), suffix='.tmp', delete=False) as stream:
            temporary = stream.name
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.remove(temporary)


def build(name, compiler):
    src = os.path.join(ROOT, PRE, name + ".hlsl")
    cso = os.path.join(ROOT, PRE, name + "_Shader.cso")
    hdr = os.path.join(ROOT, PRE, name + "_Shader.h")

    # A capture/probe may be reading the existing CSO. Compile elsewhere and
    # publish only a complete changed artifact, just as for the generated header.
    with tempfile.TemporaryDirectory(prefix=name + '_', dir=os.path.dirname(cso)) as temporary:
        compiled = os.path.join(temporary, name + '.cso')
        asm = os.path.join(temporary, name + '.asm')
        args = [str(compiler), "-T", "cs_6_2", "-E", "CSMain", "-enable-16bit-types", "-O3",
                "-Qstrip_debug", "-Qstrip_reflect", src, "-Fo", compiled, "-Fc", asm]
        res = subprocess.run(args, capture_output=True, text=True)
        if res.returncode != 0:
            print(res.stdout)
            print(res.stderr)
            raise SystemExit("dxc failed for " + name)

        with open(asm, "r", encoding="utf-8", errors="replace") as f:
            listing = "\n".join(
                line.rstrip() for line in f.read().replace("\r\n", "\n").splitlines()
            )
        with open(compiled, "rb") as f:
            data = f.read()

    with io.StringIO() as f:
        f.write("#if 0\n")
        f.write(listing)
        f.write("\n\n#endif\n\n")
        f.write("const unsigned char %s_cso[] = {\n    " % name)
        for i, b in enumerate(data):
            f.write("0x%02x" % b)
            if i < len(data) - 1:
                f.write(",")
                f.write("\n    " if (i + 1) % 12 == 0 else " ")
        f.write("\n};\n")
        header = f.getvalue().encode('utf-8')

    publish_if_changed(cso, data)
    publish_if_changed(hdr, header)

    print("%s: cso %d bytes -> %s" % (name, len(data), hdr))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name', nargs='?', default='all',
                        choices=TARGETS + ['all', '--all'])
    parser.add_argument('--all', dest='run_all', action='store_true',
                        help='Rebuild all 12 upstream shaders + canonical upscaler depth')
    parser.add_argument('--dxc', help='DXC executable; otherwise FSRD_DXC, PATH, or latest installed SDK')
    options = parser.parse_args()

    compiler = dxc(options.dxc)
    print('DXC:', compiler, flush=True)
    subprocess.run([str(compiler), '--version'], check=True)

    check = subprocess.run([sys.executable, VERIFY])
    if check.returncode != 0:
        raise SystemExit("mirror check failed; not compiling")

    if options.run_all or options.name in ('all', '--all'):
        names = TARGETS
    elif options.name == 'FSRDInputConv':
        names = ['FSRDInputConv', 'FSRDInputConvAdditive', 'RRTraceAdditive']
    elif options.name in ('FSRDOutputComp', 'FSRDOutputCompLight', 'FSRDOutputCompNoRecovery',
                          'FSRDOutputCompTileLight', 'FSRDOutputCompTileAnchor'):
        names = ['FSRDOutputComp', 'FSRDOutputCompLight', 'FSRDOutputCompNoRecovery',
                 'FSRDOutputCompTileLight', 'FSRDOutputCompTileAnchor']
    else:
        names = [options.name]

    with ThreadPoolExecutor(max_workers=min(len(names), os.cpu_count() or 4)) as pool:
        list(pool.map(lambda name: build(name, compiler), names))
