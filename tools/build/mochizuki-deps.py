"""Pinned build-only dependencies; no Vulkan SDK installation or machine changes."""
import hashlib
from pathlib import Path
import urllib.request
import zipfile

HEADER_PIN = 'e3b1eec08173d6b825cd3ac88c885a63b621504a'
GLSLANG_SHA = '06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce'

def fetch(url, path, expected=None):
    if not path.is_file():
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if expected and hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError('Build dependency SHA256 mismatch: ' + url)
        path.write_bytes(data)
    if expected and hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise RuntimeError('Cached build dependency SHA256 mismatch: ' + str(path))

def unpack(path, target):
    target = target.resolve()
    with zipfile.ZipFile(path) as archive:
        for item in archive.infolist():
            resolved = (target / item.filename).resolve()
            if not resolved.is_relative_to(target):
                raise RuntimeError('Archive member escapes dependency directory')
        archive.extractall(target)

def dependencies(repo):
    cache = repo / 'exports/mochizuki-toolchain'
    cache.mkdir(parents=True, exist_ok=True)
    headers = cache / ('Vulkan-Headers-' + HEADER_PIN)
    archive = cache / ('Vulkan-Headers-' + HEADER_PIN + '.zip')
    fetch('https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/' + HEADER_PIN, archive)
    if not (headers / 'include/vulkan/vulkan.h').is_file():
        unpack(archive, cache)
    glslang = cache / 'glslang/bin/glslang.exe'
    archive = cache / 'glslang-16.5.0.zip'
    fetch('https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip',
          archive, GLSLANG_SHA)
    if not glslang.is_file():
        unpack(archive, cache / 'glslang')
    return headers / 'include', glslang

if __name__ == '__main__':
    print(*dependencies(Path(__file__).resolve().parents[2]), sep='\n')
