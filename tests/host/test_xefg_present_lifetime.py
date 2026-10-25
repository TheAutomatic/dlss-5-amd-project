"""Source contract for the borrowed-cache / owned Present retag boundary (XeFG, DLSSG, FSR-FG)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
FG = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen'
XE = FG / 'xefg'
DL = FG / 'dlssg'
FFX = FG / 'ffx'


class SharedGuard(unittest.TestCase):
    def test_shared_raii_owns_release_under_slot_mutex(self):
        header = (FG / 'IFGFeature_Dx12.h').read_text(encoding='utf-8-sig')
        guard = header.split('struct PresentResourceReference', 1)[1].split('class IFGFeature_Dx12', 1)[0]
        self.assertIn('PresentResourceReference(const PresentResourceReference&) = delete;', guard)
        self.assertLess(guard.index('resource->AddRef();'), guard.index('resource->Release();'))
        self.assertLess(guard.index('std::unique_lock'), guard.index('resource->Release();'))
        self.assertNotIn('_frameResources', guard)  # cache clear/replacement cannot orphan ownership


class XeFGPresentLifetime(unittest.TestCase):
    def test_retag_owns_reference_until_present_exit(self):
        source = (XE / 'XeFG_Dx12.cpp').read_text(encoding='utf-8-sig')
        dispatch = source.split('bool XeFG_Dx12::Dispatch(', 1)[1].split('void* XeFG_Dx12::FrameGenerationContext', 1)[0]
        for start in ('if (!_noHudless[fIndex])', 'if (!_noDistortionField[fIndex])'):
            retag = dispatch.split(start, 1)[1].split('Dx12Resource retagged;', 1)[1]
            self.assertLess(retag.index('std::unique_lock'), retag.index('presentResource.Acquire('))
            self.assertLess(retag.index('presentResource.Acquire('), retag.index('res.validity ='))
            self.assertIn('presentResource.Acquire(res.GetResource(),', retag)
            self.assertIn('retagged = res;', retag)
            self.assertIn('SetResource(&retagged);', retag)
        present = source.split('bool XeFG_Dx12::Present()', 1)[1].split('bool XeFG_Dx12::SetResource', 1)[0]
        self.assertLess(present.index('PresentResourceReference presentResource;'), present.index('return false;'))
        self.assertIn('return Dispatch(presentResource);', present)
        set_resource = source.split('bool XeFG_Dx12::SetResource(', 1)[1]
        self.assertNotIn('AddRef(', set_resource)
        self.assertNotIn('->AddRef(', source)  # ownership lives in the shared RAII only


class DLSSGPresentLifetime(unittest.TestCase):
    def test_both_retags_own_reference_until_present_exit(self):
        source = (DL / 'DLSSG_Dx12.cpp').read_text(encoding='utf-8-sig')
        dispatch = source.split('bool DLSSG_Dx12::Dispatch(', 1)[1].split('void* DLSSG_Dx12::FrameGenerationContext', 1)[0]
        for slot in ('HudlessColor', 'Distortion'):
            start = 'if (!_noHudless[fIndex])' if slot == 'HudlessColor' else 'if (!_noDistortionField[fIndex])'
            block = dispatch.split(start, 1)[1]
            block = block.split('Dx12Resource retagged;', 1)[1]
            self.assertLess(block.index('std::unique_lock'), block.index('presentResource.Acquire('))
            self.assertLess(block.index('presentResource.Acquire('), block.index('res.validity ='))
            self.assertIn('SetResource(&retagged);', block)
            self.assertIn('presentResource.Acquire(res.GetResource(),', block)
        present = source.split('bool DLSSG_Dx12::Present()', 1)[1].split('bool DLSSG_Dx12::SetResource', 1)[0]
        self.assertLess(present.index('PresentResourceReference presentResource;'), present.index('return false;'))
        self.assertIn('return Dispatch(presentResource);', present)
        set_resource = source.split('bool DLSSG_Dx12::SetResource(', 1)[1]
        self.assertNotIn('AddRef(', set_resource)  # initial cache stays borrowed


class FSRFGPresentLifetime(unittest.TestCase):
    def test_extended_borrowed_entries_are_owned_for_dispatch(self):
        source = (FFX / 'FSRFG_Dx12.cpp').read_text(encoding='utf-8-sig')
        dispatch = source.split('bool FSRFG_Dx12::Dispatch(', 1)[1].split('ffxReturnCode_t FSRFG_Dx12::DispatchCallback', 1)[0]
        acquire = dispatch.split('acquireIfExtended', 1)[1].split('if (!_noHudless', 1)[0]
        self.assertLess(acquire.index('std::unique_lock'), acquire.index('presentResource.Acquire('))
        self.assertLess(acquire.index('FG_ResourceValidity::UntilPresent'), acquire.index('presentResource.Acquire('))
        self.assertIn('FG_ResourceValidity::UntilPresentFromDispatch', acquire)
        self.assertIn('res.copy != nullptr', acquire)
        self.assertIn('presentResource.Acquire(res.GetResource(),', acquire)
        dispatch_rest = dispatch.split('acquireIfExtended', 1)[0] + dispatch.split('acquireIfExtended(FG_ResourceType::Distortion);', 1)[1]
        self.assertNotIn('AddRef(', dispatch_rest)
        self.assertIn('acquireIfExtended(FG_ResourceType::HudlessColor);', dispatch)
        self.assertIn('acquireIfExtended(FG_ResourceType::Distortion);', dispatch)
        present = source.split('bool FSRFG_Dx12::Present()', 1)[1].split('bool FSRFG_Dx12::SetResource', 1)[0]
        self.assertLess(present.index('DrainPresentResources()'), present.index('return false;'))
        self.assertIn('return Dispatch(_presentResources);', present)
        self.assertIn('_presentResources.Acquire(it->second.GetResource(),', present)
        self.assertNotIn('PresentResourceReference presentResource;', present)
        set_resource = source.split('bool FSRFG_Dx12::SetResource(', 1)[1]
        self.assertNotIn('AddRef(', set_resource)  # initial cache stays borrowed

    def test_ffx_retains_until_sdk_gpu_completion(self):
        source = (FFX / 'FSRFG_Dx12.cpp').read_text(encoding='utf-8-sig')
        header = (FFX / 'FSRFG_Dx12.h').read_text(encoding='utf-8-sig')
        self.assertIn('PresentResourceReference _presentResources;', header)
        drain = source.split('bool FSRFG_Dx12::DrainPresentResources()', 1)[1].split('bool FSRFG_Dx12::Dispatch(', 1)[0]
        self.assertIn('FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12', drain)
        self.assertLess(drain.index('D3D12_Dispatch'), drain.index('_presentResources.Reset();'))
        self.assertLess(drain.index('return false;'), drain.index('_presentResources.Reset();'))
        self.assertLess(drain.index('D3D12_Configure'), drain.index('_presentResources.Reset();'))
        self.assertIn('clear.swapChain = _swapChain;', drain)
        self.assertNotIn('clear.frameGenerationEnabled = true', drain)
        for method in ('DestroyFGContext', 'CreateContext', 'Activate', 'Deactivate', 'ReleaseObjects', 'ReleaseResourceCopies'):
            body = source.split('void FSRFG_Dx12::' + method + '(', 1)[1]
            self.assertIn('DrainPresentResources()', body.split('\n}', 1)[0])


if __name__ == '__main__':
    unittest.main()
