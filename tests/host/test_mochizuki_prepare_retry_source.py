from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PrepareRetryWiring(unittest.TestCase):
    def test_backend_uses_typed_disposition_and_preserves_real_breaker(self):
        source = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/MochizukiBackend.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("if (p->prepareFailures.Open())", source)
        self.assertIn("ClassifyMochizukiPrepare(prepareResult, reason)", source)
        self.assertIn("p->prepareFailures.Observe(outcome)", source)
        self.assertIn("outcome == MochizukiPrepareOutcome::VramWait", source)
        self.assertIn("waiting for VRAM (original frame)", source)
        self.assertNotIn("consecutivePrepareFailures", source)
        release = source[source.index("void MochizukiBackend::ReleaseSession("):]
        self.assertIn("p->prepareFailures.Reset()", release)


if __name__ == "__main__":
    unittest.main()
