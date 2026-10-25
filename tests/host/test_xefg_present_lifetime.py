"""Source contract for the borrowed-cache / owned Present retag boundary."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
XE = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg'


class PresentLifetime(unittest.TestCase):
    def test_retag_owns_reference_until_present_exit(self):
        source = (XE / 'XeFG_Dx12.cpp').read_text(encoding='utf-8-sig')
        header = (XE / 'XeFG_Dx12.h').read_text(encoding='utf-8-sig')
        dispatch = source.split('bool XeFG_Dx12::Dispatch(', 1)[1].split('void* XeFG_Dx12::FrameGenerationContext', 1)[0]
        retag = dispatch.split('Dx12Resource retagged;', 1)[1].split('if (!_noDistortionField', 1)[0]
        self.assertLess(retag.index('std::unique_lock'), retag.index('res.resource->AddRef();'))
        self.assertLess(retag.index('res.resource->AddRef();'), retag.index('res.validity ='))
        self.assertIn('presentResource.resource = res.resource;', retag)
        self.assertIn('presentResource.mutex = &_resourceMutex[fIndex];', retag)
        self.assertIn('retagged = res;', retag)
        self.assertIn('SetResource(&retagged);', retag)
        present = source.split('bool XeFG_Dx12::Present()', 1)[1].split('bool XeFG_Dx12::SetResource', 1)[0]
        self.assertLess(present.index('PresentResourceReference presentResource;'), present.index('return false;'))
        self.assertIn('return Dispatch(presentResource);', present)
        guard = header.split('struct PresentResourceReference', 1)[1].split('bool Dispatch(', 1)[0]
        self.assertIn('PresentResourceReference(const PresentResourceReference&) = delete;', guard)
        self.assertLess(guard.index('std::unique_lock'), guard.index('resource->Release();'))
        self.assertNotIn('_frameResources', guard)  # cache clear/replacement cannot orphan ownership
        set_resource = source.split('bool XeFG_Dx12::SetResource(', 1)[1]
        self.assertNotIn('AddRef(', set_resource)
        self.assertEqual(source.count('res.resource->AddRef();'), 1)


if __name__ == '__main__':
    unittest.main()
