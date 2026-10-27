"""Rebuild one fsrd_preprocess shader: cso + generated header (listing prefix + byte array).

Mirrors the layout the existing precompiled headers use, so the checked-in artifacts stay
in the same shape: an `#if 0` DXIL listing for reference, then the cso bytes.
"""
import subprocess
import sys
import os

DXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PRE = "OptiScaler/shaders/fsrd_preprocess/precompile"
VERIFY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_fsrd_mirrors.py")


def build(name):
    src = os.path.join(ROOT, PRE, name + ".hlsl")
    cso = os.path.join(ROOT, PRE, name + "_Shader.cso")
    asm = os.path.join(ROOT, PRE, name + ".asm")
    hdr = os.path.join(ROOT, PRE, name + "_Shader.h")

    args = [DXC, "-T", "cs_6_2", "-E", "CSMain", "-enable-16bit-types", "-O3",
            "-Qstrip_debug", "-Qstrip_reflect", src, "-Fo", cso, "-Fc", asm]
    res = subprocess.run(args, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise SystemExit("dxc failed for " + name)

    with open(asm, "r", encoding="utf-8", errors="replace") as f:
        # DXC emits a few listing lines with trailing spaces. Normalize them here so
        # regenerated checked-in headers pass git diff --check deterministically.
        listing = "\n".join(
            line.rstrip() for line in f.read().replace("\r\n", "\n").splitlines()
        )
    os.remove(asm)

    with open(cso, "rb") as f:
        data = f.read()

    with open(hdr, "w", encoding="utf-8", newline="\n") as f:
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

    print("%s: cso %d bytes -> %s" % (name, len(data), hdr))


if __name__ == "__main__":
    # The verifier first: it fails in a second on a mirror that drifted, where the compiler
    # would say nothing and the shader would read the wrong parameter or resource.
    check = subprocess.run([sys.executable, VERIFY])
    if check.returncode != 0:
        raise SystemExit("mirror check failed; not compiling")
    build(sys.argv[1])
