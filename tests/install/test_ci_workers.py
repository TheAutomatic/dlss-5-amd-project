"""Run the real CI parent with stub builds: live logs, child exits and joining."""
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows and pwsh required')
class WorkerTests(unittest.TestCase):
    def exercise(self, failed):
        source = (ROOT / 'tools/build/build-ci.ps1').read_text(encoding='utf-8-sig')
        # Only replace expensive child work. Execute the actual parent unchanged.
        begin = source.index("if ($Worker -eq 'host') {")
        end = source.index('$workers = @()')
        stub = '''if ($Worker -ne 'all') {
    Write-Host "LIVE-$Worker"
    [Console]::Error.WriteLine("STDERR-$Worker")
    if ($Worker -eq 'host') { exit HOST_EXIT }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (!(Test-Path "$root/release-worker")) {
        if ([DateTime]::UtcNow -gt $deadline) { exit 9 }
        Start-Sleep -Milliseconds 100
    }
    Set-Content "$root/joined" 'completed'
    Write-Host 'TAIL-tests'
    exit 0
}
'''.replace('HOST_EXIT', '7' if failed else '0')
        with tempfile.TemporaryDirectory(prefix='ci workers ') as temp:
            root = Path(temp)
            script = root / 'tools/build/build-ci.ps1'
            script.parent.mkdir(parents=True)
            script.write_text(source[:begin] + stub + source[end:], encoding='utf-8')
            process = subprocess.Popen(['pwsh', '-NoProfile', '-File', str(script)],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding='utf-8', errors='replace')
            lines = queue.Queue()
            def collect():
                for line in process.stdout:
                    lines.put(line)
                lines.put(None)
            reader = threading.Thread(target=collect, daemon=True)
            reader.start()
            output = []
            try:
                while True:
                    line = lines.get(timeout=15)
                    self.assertIsNotNone(line, ''.join(output))
                    output.append(line)
                    if '[tests] LIVE-tests' in line:
                        break
                self.assertIsNone(process.poll(), 'logs must arrive while tests are still running')
            finally:
                (root / 'release-worker').touch()
                process.wait(timeout=30)
                reader.join(timeout=5)
                process.stdout.close()
            while not lines.empty():
                line = lines.get_nowait()
                if line is not None:
                    output.append(line)
            output = ''.join(output)
            self.assertEqual(process.returncode, 1 if failed else 0, output)
            self.assertTrue((root / 'joined').exists(), output)
            for text in ('[host] STDERR-host', '[tests] STDERR-tests', '[tests] TAIL-tests',
                         'CI WORKER host: exit=', 'CI WORKER tests: exit=0'):
                self.assertIn(text, output)
            self.assertEqual('HOST_AND_CI_PASSED' in output, not failed, output)

    def test_success_streams_before_exit_and_drains_both_logs(self):
        self.exercise(False)

    def test_failure_still_joins_other_worker_and_fails_parent(self):
        self.exercise(True)

    def test_test_worker_restores_windows_powershell_module_discovery(self):
        with tempfile.TemporaryDirectory(prefix='ci modules ') as temp:
            root = Path(temp)
            script = root / 'tools/build/build-ci.ps1'
            script.parent.mkdir(parents=True)
            shutil.copy2(ROOT / 'tools/build/build-ci.ps1', script)
            tests = root / 'tests'
            tests.mkdir()
            # Replace the expensive suite; retain the real worker environment and result handling.
            (tests / 'run-all.cmd').write_text('''@echo off
if defined PSModulePath exit /b 17
powershell -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $null=Get-Command Get-FileHash; Write-Output 'PS51_MODULE_DISCOVERY_OK'"
if errorlevel 1 exit /b 18
mkdir exports\\release-ci\\tests\\runtime
echo test-fixture>exports\\release-ci\\tests\\runtime\\LmxxfNrRuntime.dll
echo test-fixture>exports\\release-ci\\tests\\runtime-ci.sha256
exit /b 0
''', encoding='ascii')
            env = {key.upper(): value for key, value in os.environ.items()}
            env['PSMODULEPATH'] = str(root / 'incompatible-modules')
            result = subprocess.run(['pwsh', '-NoProfile', '-File', str(script), '-Worker', 'tests'],
                                    env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PS51_MODULE_DISCOVERY_OK', result.stdout)
            self.assertEqual((root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll').read_bytes().strip(), b'test-fixture')


if __name__ == '__main__':
    unittest.main(verbosity=2)
