"""Compile and run the MZ4 codec CPU tests (no WPF, no game files)."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CSC = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"


@unittest.skipUnless(os.name == "nt" and CSC.exists(), "requires Windows .NET Framework csc")
class Mz4ShareCodeTests(unittest.TestCase):
    def test_codec_round_trip_layers_and_old_prefixes(self):
        with tempfile.TemporaryDirectory(prefix="mz4-codec-") as temp:
            exe = Path(temp) / "mz4.exe"
            result = subprocess.run([
                str(CSC), "/nologo", "/target:exe", "/out:" + str(exe),
                str(REPO / "tests/install/mz4_share_code_test.cs"),
                str(REPO / "tools/install/launcher/MochizukiShareCode.cs")
            ], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn(b"PASS mz4 share code", run.stdout)


if __name__ == "__main__":
    unittest.main()
