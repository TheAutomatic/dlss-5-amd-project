#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace DlssNr::Backend
{
inline bool SameDeviceIdentity(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    if (a == b) return true;
    Microsoft::WRL::ComPtr<IUnknown> aId, bId;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&aId))) && aId &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&bId))) && bId && aId.Get() == bId.Get();
}

// ownedChild MUST have been created by this session through expectedDevice.
// ReShade returns its wrapper from queue.GetDevice(), but creates native fences.
// Our own synchronization fence establishes the native device's provenance
// without private wrapper GUIDs, adapter-LUID guesses or accepting a foreign device.
inline bool IsRecordingDevice(IUnknown* expectedDevice, ID3D12DeviceChild* ownedChild, IUnknown* candidateDevice)
{
    if (!expectedDevice || !candidateDevice) return false;
    if (SameDeviceIdentity(expectedDevice, candidateDevice)) return true;
    Microsoft::WRL::ComPtr<IUnknown> nativeIdentity;
    return ownedChild && SUCCEEDED(ownedChild->GetDevice(IID_PPV_ARGS(&nativeIdentity))) &&
           SameDeviceIdentity(nativeIdentity.Get(), candidateDevice);
}
}
