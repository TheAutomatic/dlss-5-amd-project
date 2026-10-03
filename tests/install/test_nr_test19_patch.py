import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
PS=Path(os.environ.get('SystemRoot','C:/Windows'))/'System32/WindowsPowerShell/v1.0/powershell.exe'

@unittest.skipUnless(os.name=='nt','Windows installer')
class PatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.fixture=Path(cls.tmp.name)/'fixture.dll'
        code='using System.Reflection; [assembly: AssemblyProduct("OptiScaler")] public class Fixture {}'
        ps="Add-Type -TypeDefinition '"+code+"' -OutputAssembly '"+str(cls.fixture)+"' -OutputType Library"
        subprocess.run([str(PS),'-NoProfile','-Command',ps],check=True,capture_output=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        t=tempfile.TemporaryDirectory();self.addCleanup(t.cleanup);self.root=Path(t.name);self.pkg=self.root/'patch';self.game=self.root/'game';self.pkg.mkdir();self.game.mkdir()
        self.script=self.pkg/'Apply.ps1';shutil.copyfile(ROOT/'tools/install/apply-nr-test19.ps1',self.script)
        self.names=['OptiScaler.dll','LmxxfNrRuntime.dll','shaders/native_codec_encode.hlsl','shaders/native_codec_decode.hlsl','NR-test19-collect.ps1','NR-test19-collect.cmd']
        sums=[]
        for name in self.names:
            p=self.pkg/'payload'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(self.fixture.read_bytes() if name=='OptiScaler.dll' else ('new '+name).encode())
            sums.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+name)
        (self.pkg/'PAYLOAD-SHA256.txt').write_text('\n'.join(sums))
        (self.game/'dxgi.dll').write_bytes(self.fixture.read_bytes());(self.game/'LmxxfNrRuntime.dll').write_bytes(b'old runtime')
        (self.game/'OptiScaler.ini').write_bytes(b'custom ini');(self.game/'native-game-tiled-assets').mkdir();(self.game/'native-game-tiled-assets/weights').write_bytes(b'keep weights')
    def run_patch(self):
        return subprocess.run([str(PS),'-NoProfile','-ExecutionPolicy','Bypass','-File',str(self.script),'-GameDirectory',str(self.game)],capture_output=True,timeout=30)
    def test_install_and_repeat_preserve_user_files(self):
        for _ in range(2):
            result=self.run_patch();self.assertEqual(result.returncode,0,result.stdout)
            for name in self.names:self.assertEqual((self.pkg/'payload'/name).read_bytes(),(self.game/('dxgi.dll' if name=='OptiScaler.dll' else name)).read_bytes())
            self.assertEqual((self.game/'OptiScaler.ini').read_bytes(),b'custom ini');self.assertEqual((self.game/'native-game-tiled-assets/weights').read_bytes(),b'keep weights')
        self.assertEqual(len(list(self.game.glob('backup-NR-test19-*'))),2)
    def test_corruption_refused_before_changes(self):
        (self.pkg/'payload/LmxxfNrRuntime.dll').write_bytes(b'corrupt');self.assertNotEqual(self.run_patch().returncode,0)
        self.assertEqual((self.game/'LmxxfNrRuntime.dll').read_bytes(),b'old runtime');self.assertFalse(list(self.game.glob('backup-*')))
    def test_other_proxy_is_not_overwritten(self):
        (self.game/'dxgi.dll').write_bytes(b'other proxy');self.assertNotEqual(self.run_patch().returncode,0);self.assertEqual((self.game/'dxgi.dll').read_bytes(),b'other proxy')
    def test_write_failure_restores_previous_files(self):
        source=self.script.read_text(encoding='utf-8');source=source.replace('$changed.Add($item);[IO.File]::Copy',"$changed.Add($item);if($item.Source -eq 'shaders/native_codec_decode.hlsl'){throw 'injected write failure'};[IO.File]::Copy")
        self.script.write_text(source,encoding='utf-8');self.assertNotEqual(self.run_patch().returncode,0)
        self.assertEqual((self.game/'LmxxfNrRuntime.dll').read_bytes(),b'old runtime');self.assertEqual((self.game/'dxgi.dll').read_bytes(),self.fixture.read_bytes());self.assertFalse((self.game/'shaders/native_codec_encode.hlsl').exists())

if __name__=='__main__':unittest.main()
