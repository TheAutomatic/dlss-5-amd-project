"""Report what upstream ENABLES against what we enable.

Why this exists: syncing code is not the same as following the configuration. We once pulled
half a day of kernel work and shipped with all five of the author's HIP optimisation switches
off, because they live in scripts/hip-game-flags.txt rather than in any file we vendor. The same
trap exists at kernel build time: a speed-up can ship behind a `#define ... 0` and only be turned
on by a lab build script we never run.

Three kinds of switch exist upstream. Only two concern us:

  1. runtime Options      DLSS5_HIP_* in scripts/hip-game-flags.txt -> o.X via src/native_hip_network.h,
                          plus its `if(fast)` batch and src/LmxxfProductionOptions.h.
                          ** WE MATCH THIS BY HAND in LmxxfProductionOptions.h. **
  2. kernel build-time    HIP_* macros baked in by hip/build-modules.ps1's `defines` column.
                          We follow automatically (sync copies the recipe and rebuilds modules),
                          BUT a macro the recipe does not set is off for us even when the author
                          enables it in his own lab script. ** THIS TOOL REPORTS THOSE. **
  3. D3D12 network body   DLSS5_FP8_* / C32_* / SPLIT_* in src/native_c64.h etc. We deliberately do
                          not vendor that path (see third_party/lmxxf/UPSTREAM.md), so these are
                          not gaps. Reported as a count only.

Usage:  python tools/audit-lmxxf-enablements.py [upstream-clone-dir] [ref]
        Defaults: ../dlss5-on-amd-9070xt-porting and origin/main.
Exit code 0 always: a divergence can be deliberate (DLSS5_FIT_LARGE is, for us). Read the output.
"""
import io
import os
import re
import subprocess
import sys

def_up = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "..", "dlss5-on-amd-9070xt-porting")
UP = sys.argv[1] if len(sys.argv) > 1 else def_up
REF = sys.argv[2] if len(sys.argv) > 2 else "origin/main"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(ROOT, "OptiScaler-DLSSNR-PreSR-Multipass-main", "OptiScaler", "dlssnr",
                    "backend", "lmxxf_runtime", "LmxxfProductionOptions.h")


def up(path):
    return subprocess.check_output(["git", "-C", UP, "show", "%s:%s" % (REF, path)],
                                  stderr=subprocess.DEVNULL).decode("utf-8", "replace")


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read()


def true_fields(text):
    """Every o.X assigned by a statement whose chain ends in `= true`.

    Statement-based, not line-based: LmxxfProductionOptions.h chains a dozen fields over four
    lines, and line-based parsing counts only the last one - which reports real coverage as
    missing and is exactly the false confidence this tool exists to avoid.
    """
    out = set()
    nl = chr(10)
    code = text
    while "/*" in code:
        a = code.index("/*")
        b = code.find("*/", a + 2)
        code = code[:a] + " " + (code[b + 2:] if b >= 0 else "")
    code = "".join(ln.split("//", 1)[0] + nl for ln in code.split(nl))
    end_true = re.compile(r"=\s*true\s*$")
    field = re.compile(r"o\.([a-z0-9_]+)")
    for stmt in code.split(";"):
        if end_true.search(stmt.strip()):
            out.update(field.findall(stmt))
    return out


flags_on = {}
for f in ("scripts/hip-game-flags.txt", "scripts/hip-re9-flags.txt"):
    try:
        txt = up(f)
    except Exception:
        continue
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("DLSS5_") and "=" in line:
            k, v = line.split("=", 1)
            flags_on.setdefault(k.strip(), set()).add(v.strip())

net = up("src/native_hip_network.h")
flag_to_field = {}
for m in re.finditer(r'getenv\("(DLSS5_[A-Z0-9_]+)"\).{0,220}?o\.([a-z0-9_]+)\s*=\s*!strcmp', net, re.S):
    flag_to_field.setdefault(m.group(1), set()).add(m.group(2))

up_fields = true_fields(net) | true_fields(up("src/LmxxfProductionOptions.h"))
for flag, fields in flag_to_field.items():
    if "1" in flags_on.get(flag, set()):
        up_fields |= fields
our_fields = true_fields(read(OURS))

print("== 1. runtime Options (we match by hand in LmxxfProductionOptions.h) ==")
print("  upstream can enable %d fields, we enable %d" % (len(up_fields), len(our_fields)))
gap = sorted(up_fields - our_fields)
if gap:
    print("  MISSING here:")
    for g in gap:
        src = sorted(f for f, fs in flag_to_field.items() if g in fs and "1" in flags_on.get(f, set()))
        print("    o.%-24s <- %s" % (g, ", ".join(src) if src else "options file / if(fast) batch"))
else:
    print("  no gap")
extra = sorted(our_fields - up_fields)
if extra:
    print("  only we enable (confirm deliberate): o." + ", o.".join(extra))

print()
print("== 2. kernel build-time HIP_* (recipe is hip/build-modules.ps1) ==")
recipe = up("hip/build-modules.ps1")
recipe_defs = set(re.findall(r"HIP_[A-Z0-9_]+", recipe))
kernel_gates = set()
# Plain file names only: the recipe also mentions generated sources like "$x.generated.hip",
# and a loose pattern turns those into "hip/.generated.hip" lookups.
kernels = sorted(set(re.findall(r"'([A-Za-z0-9][A-Za-z0-9_.-]*[.]hip)'", recipe)))
for name in kernels:
    try:
        kernel_gates |= set(re.findall(r"#(?:if|ifdef|ifndef)\s+(HIP_[A-Z0-9_]+)", up("hip/" + name)))
    except Exception:
        pass
print("  recipe defines      : " + (", ".join(sorted(recipe_defs)) or "(none beyond the implicit ones)"))
# We also inject defines at build time through sync's $ModuleDefineOverrides, for gates the
# author enables in his own deployment scripts but not in his recipe. Count those as ours or
# the report keeps flagging a gap we already closed.
ours_extra = set()
_sync = io.open(os.path.join(ROOT, "tools", "sync-lmxxf-upstream.ps1"), encoding="utf-8", errors="replace").read()
_blk = re.search(r"\$ModuleDefineOverrides\s*=\s*@\{(.*?)\n\s*\}", _sync, re.S)
if _blk:
    ours_extra |= set(re.findall(r"'(HIP_[A-Z0-9_]+) \d+'", _blk.group(1)))
recipe_defs |= ours_extra

# Where does the author actually turn a gate ON? A gate with no "= 1" anywhere is an ablation or an
# experiment he never shipped, and noise. One set by a deployments/ script is a speed-up he runs in
# production that we silently miss - the trap this tool exists for.
enabled_at = {}
for path in subprocess.check_output(["git", "-C", UP, "ls-tree", "-r", "--name-only", REF,
                                     "--", "Development/deployments", "Development/HIP/experiments"],
                                    stderr=subprocess.DEVNULL).decode("utf-8", "replace").splitlines():
    try:
        body = up(path)
    except Exception:
        continue
    for g in re.findall(r"(HIP_[A-Z0-9_]+)\s*=\s*1", body):
        enabled_at.setdefault(g, set()).add(path)

# A gate whose #ifndef default is already on is not a gap: the recipe never mentions
# HIP_FFN_TRANSPOSED_TAIL and it is enabled regardless. Without this the report cried wolf on it.
kernel_default_on = set()
for name in kernels:
    try:
        src = up("hip/" + name)
    except Exception:
        continue
    for m in re.finditer(r"#ifndef\s+(HIP_[A-Z0-9_]+)[^\n]*\n#define\s+HIP_[A-Z0-9_]+\s+([01])", src):
        if m.group(2) == "1":
            kernel_default_on.add(m.group(1))

unbacked = sorted(g for g in kernel_gates if g not in recipe_defs and g not in kernel_default_on and g not in
                  ("HIP_ISA_HALF", "HIP_PREPACKED_WEIGHTS", "HIP_NATIVE_FP8_F", "HIP_NATIVE_RTZ",
                   "HIP_LDS_FENCE", "HIP_BRANCHLESS_F", "HIP_MH_RTZ_ISA"))
if kernel_default_on:
    print("  gates that default ON in the kernel (no recipe entry needed): " + ", ".join(sorted(kernel_default_on)))
shipped = [g for g in unbacked if g in enabled_at and any("deployments" in p for p in enabled_at[g])]
other = [g for g in unbacked if g not in shipped]

if shipped:
    print("  TURNED ON BY HIS DEPLOYMENT SCRIPTS BUT NOT BY OUR RECIPE  <-- we are shipping without these:")
    print("    (a define we inject via sync's ModuleDefineOverrides is NOT missing; only real gaps appear here)")
    for g in shipped:
        where = [p for p in sorted(enabled_at[g]) if "deployments" in p]
        kernels_using = [n for n in kernels
                         if re.search(r"#(?:if|ifdef|ifndef)\s+" + g + r"\b", up("hip/" + n))]
        print("    %-24s enabled in %s" % (g, ", ".join(where)))
        print("    %-24s gated in    %s" % ("", ", ".join(kernels_using)))
if other:
    print("  gated in kernels, never enabled anywhere we can see (ablations / dead experiments): %d"
          % len(other))
    print("    " + ", ".join(other))
if not unbacked:
    print("  every kernel gate is backed by the recipe")

print()
print("== 3. D3D12 network body (we do not vendor it - not a gap) ==")
body = [f for f in sorted(flags_on) if f.startswith(("DLSS5_FP8_", "DLSS5_C32_", "DLSS5_SPLIT_",
                                                     "DLSS5_POST70_", "DLSS5_PREBLOCK_", "DLSS5_DECODER_",
                                                     "DLSS5_TEST_", "DLSS5_FUSED_", "DLSS5_INLINE_",
                                                     "DLSS5_FAST_", "DLSS5_TILED_", "DLSS5_FFN_"))]
print("  %d flags live only in the excluded D3D12 path / test harness" % len(body))

print()
print("== 4. non-boolean switches (compare by value, not just on/off) ==")
for key in ("DLSS5_SKIP_BLOCKS", "DLSS5_NETWORK_HEIGHT", "DLSS5_FIT_LARGE", "DLSS5_CODEC_SRGB",
            "DLSS5_VIT_ADAPTIVE", "DLSS5_HIP_GRAPH"):
    vals = sorted(flags_on.get(key, set())) or ["(absent from his flags)"]
    print("  %-22s his flags -> %s" % (key, ",".join(vals)))
for line in read(OURS).splitlines():
    if "ParseSkipBlocks" in line:
        print("  ours hardcodes          : " + line.strip())
