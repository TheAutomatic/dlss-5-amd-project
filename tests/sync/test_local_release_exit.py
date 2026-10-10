"""Exercise the actual release batch control flow with deterministic child exit codes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
if not (ROOT / 'VERSION').is_file():
    ROOT = Path.cwd()
STAGES = ('vcvarsall.bat', 'build-mochizuki-runtime.cmd', 'build-lmxxf-runtime.cmd', 'tests\\run-all.cmd', 'MSBuild.exe', 'build-person-worker.cmd')

class ReleaseExitTests(unittest.TestCase):
    def exercise(self, failing, code):
        source = (ROOT / 'tools/build/build-release-local.cmd').read_text(encoding='utf-8')
        lines=[]
        seen=set()
        for line in source.splitlines():
            command, separator, guard = line.partition(' || ')
            match = next((stage for stage in STAGES if stage in command and (command.lstrip().lower().startswith('call ') or command.lstrip().startswith(chr(34)))), None)
            if match:
                seen.add(match)
                lines.append('echo CALLED_'+match)
                lines.append('cmd /d /c exit /b '+str(code if match==failing else 0)+(separator+guard if separator else ''))
            else:
                lines.append(line)
        self.assertEqual(seen,set(STAGES))
        scratch=ROOT/'work/scratch'; scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='release-exit-',dir=scratch) as temporary:
            root=Path(temporary)
            script=root/'tools/build/build-release-local.cmd'; script.parent.mkdir(parents=True)
            script.write_text('\n'.join(lines)+'\n',encoding='utf-8')
            out=root/'exports/release-local'; (out/'tests').mkdir(parents=True)
            (out/'OptiScaler.dll').write_bytes(b'fixture host')
            (out/'LmxxfNrRuntime.dll').write_bytes(b'fixture runtime')
            (out/'tests/runtime-ci.sha256').write_text('fixture proof')
            return subprocess.run(['cmd','/d','/c',str(script)],capture_output=True,text=True,errors='replace',timeout=20)

    def test_all_success(self):
        result=self.exercise(None,0)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('BUILD_OK',result.stdout)

    def test_any_child_failure_stops_including_negative_exit(self):
        for stage in STAGES:
            for code in (-1,1,23):
                with self.subTest(stage=stage,code=code):
                    result=self.exercise(stage,code)
                    self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertNotIn('BUILD_OK',result.stdout)
                    for later in STAGES[STAGES.index(stage)+1:]:
                        self.assertNotIn('CALLED_'+later,result.stdout)

    def test_ci_mochizuki_guards_inside_batch_block(self):
        lines=(ROOT/'tests/run-all.cmd').read_text().splitlines()
        prefixes=('call tools\\build\\build-mochizuki-runtime.cmd',
                  'python -X utf8 tools\\build\\mochizuki-manifest.py')
        scratch=ROOT/'work/scratch'
        for prefix in prefixes:
            line=next(line.strip() for line in lines if line.strip().startswith(prefix))
            command,separator,guard=line.partition(' || ')
            self.assertTrue(separator, line)
            for code in (-1,1,0):
                with self.subTest(prefix=prefix,code=code), tempfile.TemporaryDirectory(dir=scratch) as temp:
                    script=Path(temp)/'guard.cmd'
                    script.write_text('@echo off\nif "1"=="1" (\ncmd /d /c exit /b '+str(code)+separator+guard+'\n)\necho REACHED\n',encoding='ascii')
                    result=subprocess.run(['cmd','/d','/c',str(script)],capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode==0,code==0,result.stdout)
                    self.assertEqual('REACHED' in result.stdout,code==0)

if __name__=='__main__':
    unittest.main()
