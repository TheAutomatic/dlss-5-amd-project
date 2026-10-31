"""Structural checks of DX12 wiring, not a GPU execution test."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.cpp"


class SlotWiring(unittest.TestCase):
    def test_preflight_precedes_conversion_commands(self):
        source = SOURCE.read_text(encoding="utf-8-sig")
        body = source[source.index("    void DispatchConversion("):]
        first_command = body.index("TransitionDenoiserOutputsToRead(cmdList)")
        for reservation in ("m_floorSeedShader.Reserve(FloorSeed::kPasses)",
                            "m_floorFilterShader.Reserve(FloorFilter::kPasses)",
                            "m_convShader.Reserve(1)", "m_compShader.Reserve(2)"):
            self.assertLess(body.index(reservation), first_command)
        self.assertLess(body.index("m_compShader.Reserve(2)"), body.index("AddBarrier(cmdList"))

    def test_payload_addresses_survive_growth(self):
        source = SOURCE.read_text(encoding="utf-8-sig")
        self.assertNotIn("m_cbCurrentFrameIndex", source)
        self.assertNotIn("FSRD constant slot busy", source)
        self.assertIn("std::vector<std::unique_ptr<Slot>> m_slots", source)
        self.assertIn("std::shared_ptr<UploadPage> page", source)
        self.assertIn("slot.page->mapped + slot.offset", source)
        self.assertIn("FrameDescriptorHeap& currentHeap = slot.heap", source)
        self.assertIn("tracker.Retired(ticket)", source)
        self.assertIn("m_reservations.Commit(tracker.Record(cmdList))", source)
        self.assertIn("if (target > kMaxSlots)", source)
        self.assertNotIn("tracker.Wait(", source)


if __name__ == "__main__":
    unittest.main()
