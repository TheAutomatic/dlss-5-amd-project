"""Behavior test for the model-card dependency lock (real WPF CheckBox)."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CSC = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"


@unittest.skipUnless(os.name == "nt" and CSC.exists(), "requires Windows .NET Framework csc")
class DependencyLockTests(unittest.TestCase):
    def test_locked_parent_cannot_be_cleared_by_wpf_behavior(self):
        with tempfile.TemporaryDirectory(prefix="dependency-lock-") as temp:
            exe = Path(temp) / "dependency_lock.exe"
            fw = Path(os.environ["SystemRoot"]) / "Microsoft.NET/Framework64/v4.0.30319"
            wpf = fw / "WPF"
            result = subprocess.run([
                str(CSC), "/nologo", "/target:exe", "/out:" + str(exe),
                "/r:" + str(fw / "System.dll"),
                "/r:" + str(fw / "System.Core.dll"),
                "/r:" + str(fw / "System.Xaml.dll"),
                "/r:" + str(wpf / "WindowsBase.dll"),
                "/r:" + str(wpf / "PresentationCore.dll"),
                "/r:" + str(wpf / "PresentationFramework.dll"),
                str(REPO / "tests/install/dependency_lock_test.cs"),
                str(REPO / "tools/install/launcher/ModelDependencyLock.cs")
            ], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn(b"PASS dependency lock", run.stdout)


if __name__ == "__main__":
    unittest.main()
