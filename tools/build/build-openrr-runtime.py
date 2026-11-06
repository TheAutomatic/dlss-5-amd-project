"""Build OpenNrRrRuntime.dll and its 49 SPIR-V compute shaders using the repository toolchain."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('mochizuki_deps', Path(__file__).with_name('mochizuki-deps.py'))
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)

BASE_SPVS = [
    'ingest', 'pack', 'selftest', 'selftest_tex', 'pooled_history2',
    'ae', 'downsample', 'downsample_tl', 'post_tile', 'dfn_export', 'dfn_import',
]

FLOW_KERNELS = [
    'enc_stage_96_128_128', 'enc_stage_128_128_160', 'enc_stage_160_256_160',
    'enc_stage_160_256_0', 'dec_up_160_256_160', 'dec_dfn_160_256',
    'dec_up_128_128_160', 'dec_dfn_128_128', 'dec_up_96_128_128', 'dec_dfn_96_128',
]


def compile_shader(glslang: Path, shader_inc: Path, src: Path, out_spv: Path, defines: list[str]) -> None:
    cmd = [
        str(glslang),
        '-V',
        '--target-env', 'vulkan1.3',
        *[f'-D{d}' for d in defines],
        f'-I{shader_inc}',
        '-o', str(out_spv),
        str(src),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def parse_fastmode_defs(cmake_path: Path) -> dict[str, list[str]]:
    defs: dict[str, list[str]] = {}
    if not cmake_path.is_file():
        return defs
    for line in cmake_path.read_text(encoding='utf-8').splitlines():
        m = re.match(r'^\s*set\(\s*LT_DEFS_(\w+)\s+([^)]+)\)', line)
        if m:
            defs[m.group(1)] = m.group(2).split()
    return defs


def build_shaders(glslang: Path, shaders_dir: Path, spv_out_dir: Path) -> list[Path]:
    spv_out_dir.mkdir(parents=True, exist_ok=True)
    shader_inc = shaders_dir / 'include'
    lt_defs_map = parse_fastmode_defs(shaders_dir.parent / 'fastmode.cmake')
    built: list[Path] = []

    def lite_spv(out_name: str, src: Path, defines: list[str]) -> None:
        dst = spv_out_dir / f'{out_name}_lt.spv'
        compile_shader(glslang, shader_inc, src, dst, defines)
        built.append(dst)

    for name in BASE_SPVS:
        src = shaders_dir / f'{name}.comp'
        dst = spv_out_dir / f'{name}.spv'
        compile_shader(glslang, shader_inc, src, dst, [])
        built.append(dst)

    for src in sorted(shaders_dir.glob('*.comp')):
        name = src.stem
        if name in lt_defs_map:
            ltd = lt_defs_map[name]
            if name == 'enc0_attr':
                lite_spv('enc0_attr_f', src, ['ENC_F32=1', *ltd])
                lite_spv('enc0_attr_f_w', src, ['ENC_F32=1', 'ATTR_WIDE=1', *ltd])
                lite_spv('d3d_enc0_attr_f', src, ['D3D_GUIDES=1', 'ENC_F32=1', *ltd])
                lite_spv('d3d_enc0_attr_f_w', src, ['D3D_GUIDES=1', 'ENC_F32=1', 'ATTR_WIDE=1', *ltd])
            elif name in ('dfn_export', 'dfn_import', 'pooled_history2'):
                lite_spv(name, src, ltd)
            elif name in ('downsample', 'downsample_tl'):
                lite_spv(name, src, ltd)
                lite_spv(f'd3d_{name}', src, ['D3D_TEX=1', 'D3D_UAV=1', *ltd])
            else:
                lite_spv(f'{name}_f', src, ['ENC_F32=1', *ltd])
                if name == 'enc0_stage_32_64':
                    lite_spv(f'{name}_f_w', src, ['ENC_F32=1', 'ATTR_WIDE=1', *ltd])
                if name in FLOW_KERNELS:
                    slot = FLOW_KERNELS.index(name)
                    fw = 1 if slot > 0 else 0
                    lite_spv(f'{name}_fl', src, ['ENC_F32=1', 'FLOW=1', f'FLOW_SLOT={slot}', f'FLOW_WAIT={fw}', *ltd])

        if re.match(r'^(enc_stage_|enc0_stage_|dec_)', name):
            dst_f = spv_out_dir / f'{name}_f.spv'
            compile_shader(glslang, shader_inc, src, dst_f, ['ENC_F32=1'])
            built.append(dst_f)
            if name in FLOW_KERNELS:
                slot = FLOW_KERNELS.index(name)
                fw = 1 if slot > 0 else 0
                dst_fl = spv_out_dir / f'{name}_fl.spv'
                compile_shader(
                    glslang, shader_inc, src, dst_fl,
                    ['ENC_F32=1', 'FLOW=1', f'FLOW_SLOT={slot}', f'FLOW_WAIT={fw}'],
                )
                built.append(dst_fl)

        if name in ('ingest', 'pack', 'downsample', 'downsample_tl'):
            dst_d3d = spv_out_dir / f'd3d_{name}.spv'
            compile_shader(glslang, shader_inc, src, dst_d3d, ['D3D_TEX=1', 'D3D_UAV=1'])
            built.append(dst_d3d)

        if name in ('ae', 'enc0_attr'):
            out_name = 'd3d_ae.spv' if name == 'ae' else 'd3d_enc0_attr_f.spv'
            dst_d3d = spv_out_dir / out_name
            compile_shader(glslang, shader_inc, src, dst_d3d, ['D3D_GUIDES=1', 'ENC_F32=1'])
            built.append(dst_d3d)

        if name in ('enc0_attr', 'post', 'post_dlt'):
            dst_f = spv_out_dir / f'{name}_f.spv'
            compile_shader(glslang, shader_inc, src, dst_f, ['ENC_F32=1'])
            built.append(dst_f)

        if name in ('enc0_attr', 'enc0_stage_32_64'):
            dst_fw = spv_out_dir / f'{name}_f_w.spv'
            compile_shader(glslang, shader_inc, src, dst_fw, ['ENC_F32=1', 'ATTR_WIDE=1'])
            built.append(dst_fw)
            if name == 'enc0_attr':
                dst_d3d_fw = spv_out_dir / 'd3d_enc0_attr_f_w.spv'
                compile_shader(glslang, shader_inc, src, dst_d3d_fw, ['D3D_GUIDES=1', 'ENC_F32=1', 'ATTR_WIDE=1'])
                built.append(dst_d3d_fw)

    return built


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', nargs='?', default=None)
    parser.add_argument('--skip-shaders', action='store_true')
    args = parser.parse_args()

    out = (repo / args.out).resolve() if args.out else (repo / 'exports/openrr-runtime').resolve()
    if not out.is_relative_to((repo / 'exports').resolve()):
        parser.error('Build output must be under this workspace exports directory')

    obj = out / 'obj'
    obj.mkdir(parents=True, exist_ok=True)

    vk_headers, glslang = deps.dependencies(repo)
    up = repo / 'third_party/openrr'
    product = repo / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'

    includes = [
        vk_headers,
        up / 'gpu/include',
        up / 'gpu/src',
        up / 'runtime/include',
        up / 'runtime/src',
    ]
    cflags = [
        '/nologo', '/std:c++20', '/O2', '/EHsc', '/MT', '/utf-8',
        '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR',
        '/W4', '/permissive-', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS', '/DNDEBUG',
    ]
    sources = [
        up / 'gpu/src/vk.cpp',
        up / 'gpu/src/d3d12.cpp',
        up / 'gpu/src/kernels.cpp',
        up / 'gpu/src/weights.cpp',
        up / 'gpu/src/engine.cpp',
        up / 'runtime/src/runtime.cpp',
        up / 'runtime/src/vk_host.cpp',
    ]

    objects: list[str] = []
    for src in sources:
        target = obj / f'{src.stem}.obj'
        subprocess.run(
            ['cl', *cflags, *[f'/I{p}' for p in includes], '/c', str(src), f'/Fo{target}'],
            check=True,
        )
        objects.append(str(target))

    rc_src = up / 'runtime/src/version.rc'
    res_out = obj / 'version.res'
    subprocess.run(
        ['rc', '/nologo', *[f'/I{p}' for p in includes], f'/fo{res_out}', str(rc_src)],
        check=True,
    )
    objects.append(str(res_out))

    dll_out = out / 'OpenNrRrRuntime.dll'
    subprocess.run(
        [
            'link', '/nologo', '/DLL', f'/OUT:{dll_out}', *objects,
            str(product / 'library/vulkan/vulkan-1.lib'),
            'd3d12.lib', 'dxgi.lib', 'bcrypt.lib',
        ],
        check=True,
    )

    spv_dir = out / 'opennr-rr/shaders'
    if not args.skip_shaders:
        built = build_shaders(glslang, up / 'gpu/shaders', spv_dir)
        print(f'OPENRR_SHADERS_OK {len(built)} shaders in {spv_dir}')
    print('OPENRR_BUILD_OK', dll_out)


if __name__ == '__main__':
    main()
