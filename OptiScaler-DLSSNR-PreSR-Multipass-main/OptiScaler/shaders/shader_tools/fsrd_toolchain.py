"""Windows tool discovery shared by the FSRD shader/build/test entry points."""
from pathlib import Path
import os
import shutil
import subprocess

def executable(value, name):
    path = Path(value).expanduser() if value else None
    if path and path.is_file():
        return path
    resolved = shutil.which(str(value or name))
    if resolved:
        return Path(resolved)
    raise RuntimeError(f"Cannot find {name}: {value or 'not on PATH'}")

def dxc(explicit=None):
    override = explicit or os.environ.get('FSRD_DXC')
    if override or shutil.which('dxc.exe'):
        return executable(override, 'dxc.exe')
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                           r'SOFTWARE\Microsoft\Windows Kits\Installed Roots', 0,
                           winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as key:
            sdk = Path(winreg.QueryValueEx(key, 'KitsRoot10')[0])
        candidates = list((sdk/'bin').glob('10.*/x64/dxc.exe'))
        if candidates:
            return max(candidates, key=lambda p: tuple(map(int, p.parents[1].name.split('.'))))
    except Exception:
        pass
    fallback = Path(r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe")
    if fallback.is_file():
        return fallback
    raise RuntimeError('Install Windows SDK DXC or set FSRD_DXC.')
