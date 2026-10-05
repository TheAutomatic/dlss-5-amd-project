"""Run the existing GPU round-trip test with stale shaders under the modules path.

This replaces its ordinary invocation, rather than adding another GPU matrix.
The installed package's runtime-adjacent shaders must win over old asset files.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    executable, runtime, modules, output = (Path(value).resolve() for value in sys.argv[1:])
    repo = Path(__file__).resolve().parents[2]
    output.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="shader-precedence-", dir=output)).resolve()
    if not stage.is_relative_to(output):
        raise RuntimeError("Shader test staging escaped its output directory")
    try:
        package = stage / "package"
        package.mkdir()
        staged_runtime = package / runtime.name
        shutil.copy2(runtime, staged_runtime)
        shaders = package / "shaders"
        shaders.mkdir()
        for source in (repo / "third_party/lmxxf/shaders").glob("*.hlsl"):
            shutil.copy2(source, shaders / source.name)
        staged_modules = package / "lmxxf-modules"
        shutil.copytree(modules, staged_modules, ignore=shutil.ignore_patterns("shader-cache"))
        stale = staged_modules / "shaders"
        stale.mkdir(exist_ok=True)
        # FindShaderDir historically used this marker alone and selected assets
        # before the package. Compilation must never reach this older copy.
        (stale / "native_codec_encode.hlsl").write_text(
            "#error STALE_ASSET_SHADER_SHADOWED_THE_PACKAGE\n", encoding="utf-8"
        )
        result = subprocess.run(
            [str(executable), str(staged_runtime), str(staged_modules), "--hip-passthrough"],
            cwd=repo, timeout=180,
        )
        if result.returncode:
            return result.returncode
        print("runtime shader precedence: PASS (packaged shaders override stale assets)", flush=True)
        return 0
    finally:
        # Only remove the unique directory created above, within the verified output.
        shutil.rmtree(stage)


if __name__ == "__main__":
    raise SystemExit(main())
