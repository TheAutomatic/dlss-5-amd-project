#pragma once
#include <dxgi1_6.h>

// Targeted port of OptiScaler 97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed.
// Shared by detoured and wrapped factories. Only the private DX12 companion
// descriptor is normalized; the game's descriptor and composition path stay intact.
namespace Dx12InteropDesc
{
inline bool SupportsTearing(IDXGIFactory* factory)
{
    if (!factory) return false;
    IDXGIFactory5* factory5 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) || !factory5) return false;
    BOOL supported = FALSE;
    const auto hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported));
    factory5->Release();
    return SUCCEEDED(hr) && supported == TRUE;
}

inline bool PrepareFields(DXGI_FORMAT format, DXGI_SAMPLE_DESC& sample, UINT& count,
                          DXGI_SWAP_EFFECT& effect, DXGI_USAGE& usage, UINT& flags, bool tearing)
{
    // Do not reinterpret sRGB as linear UNORM without validating shared views.
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM)
        return false;
    if (sample.Count > 1 || count > 16) return false;
    switch (effect)
    {
    case DXGI_SWAP_EFFECT_DISCARD: effect = DXGI_SWAP_EFFECT_FLIP_DISCARD; break;
    case DXGI_SWAP_EFFECT_SEQUENTIAL: effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; break;
    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: break;
    default: return false;
    }
    if (count < 2) count = 2;
    sample = {1, 0};
    usage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    if (!tearing) flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return true;
}

inline bool Prepare(DXGI_SWAP_CHAIN_DESC& desc, bool tearing)
{
    auto candidate = desc;
    if (!PrepareFields(candidate.BufferDesc.Format, candidate.SampleDesc, candidate.BufferCount,
                       candidate.SwapEffect, candidate.BufferUsage, candidate.Flags, tearing)) return false;
    candidate.Windowed = TRUE; // Preserve our private interop presentation policy.
    desc = candidate;
    return true;
}

inline bool Prepare(DXGI_SWAP_CHAIN_DESC1& desc, bool tearing)
{
    auto candidate = desc;
    if (candidate.Stereo ||
        !PrepareFields(candidate.Format, candidate.SampleDesc, candidate.BufferCount,
                       candidate.SwapEffect, candidate.BufferUsage, candidate.Flags, tearing)) return false;
    desc = candidate;
    return true;
}
}
