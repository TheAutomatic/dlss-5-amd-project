"""Check ownership state reaches the bridge without game-colour transitions."""
from pathlib import Path
import unittest

PRODUCT = Path(__file__).resolve().parents[2] / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler"


class RrColourWiring(unittest.TestCase):
    def test_owned_contract_survives_all_host_layers(self):
        source = (PRODUCT / "shaders/dlssnr/DlssNr_Dx12.cpp").read_text(encoding="utf-8-sig")
        start = source.index("ID3D12Resource* EvaluateAfterDenoise(")
        end = source.index("return answer;", start)
        seam = source[start:end]
        self.assertIn("EvaluateInternal(cmdList, params, true, nullptr, false, 0, {}, kRrComposedColourState)", seam)
        self.assertIn("AmdBridge::Evaluate(cmdList, params, timingQueue, beforeUpscale, outputExtent, ownedColourState)", source)
        bridge = (PRODUCT / "dlssnr/amd/AmdBridge.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("f.colourState = ResolveNrColourState(beforeUpscale,", bridge)
        self.assertIn("        ownedColourState);", bridge)

    def test_rr_seam_does_not_borrow_the_titles_colour_hotfix(self):
        source = (PRODUCT / "upscalers/fsr31/FSRDFeature_Dx12.cpp").read_text(encoding="utf-8-sig")
        start = source.index("            ID3D12Resource* composed =")
        end = source.index("        isUpscalerReady = DispatchUpscaler", start)
        seam = source[start:end]
        self.assertIn("EvaluateAfterDenoise(InCommandList, InParameters, composed)", seam)
        self.assertNotIn("ResourceBarrier(InCommandList, composed,", seam)
        self.assertNotIn("cfg.ColorResourceBarrier", seam)


if __name__ == "__main__":
    unittest.main()
