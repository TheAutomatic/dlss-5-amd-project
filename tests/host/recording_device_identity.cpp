#include <windows.h>
#include <cassert>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/RecordingDeviceIdentity.h"

// Separate COM identities model a wrapper device and its native device. The
// session's own fence exposes the native identity; unrelated devices must fail.
struct Identity final : IUnknown
{
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};
struct Child final : ID3D12DeviceChild
{
    IUnknown* device;
    bool fail = false;
    explicit Child(IUnknown* d) : device(d) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { if (out) *out=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out) override {
        if (fail) { if (out) *out=nullptr; return E_FAIL; }
        return device->QueryInterface(iid,out);
    }
};
int main()
{
    using DlssNr::Backend::IsRecordingDevice;
    Identity wrapper, native, foreign;
    Child ownFence(&native);
    assert(IsRecordingDevice(&wrapper,&ownFence,&wrapper));
    assert(IsRecordingDevice(&wrapper,&ownFence,&native));
    assert(IsRecordingDevice(&native,&ownFence,&native));
    assert(!IsRecordingDevice(&wrapper,&ownFence,&foreign));
    assert(!IsRecordingDevice(&wrapper,nullptr,&native));
    assert(!IsRecordingDevice(nullptr,&ownFence,&native));
    assert(!IsRecordingDevice(&wrapper,&ownFence,nullptr));
    ownFence.fail=true;
    assert(!IsRecordingDevice(&wrapper,&ownFence,&native));
    assert(wrapper.refs==1 && native.refs==1 && foreign.refs==1);
    std::puts("recording device identity: wrapped/native accepted, foreign/unknown rejected, references balanced: PASS");
}
