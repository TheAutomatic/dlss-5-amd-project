#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/Dx12InteropDesc.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

int main()
{
    DXGI_SWAP_CHAIN_DESC1 base {};
    base.Width = 1920; base.Height = 1080;
    base.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    base.SampleDesc = {1, 0}; base.BufferCount = 1;
    base.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    base.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    base.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    for (const bool tearing : {false, true})
    {
        auto d = base;
        assert(Dx12InteropDesc::Prepare(d, tearing));
        assert(d.BufferCount == 2 && d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        assert(d.BufferUsage == DXGI_USAGE_RENDER_TARGET_OUTPUT);
        assert(d.Flags == (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        assert(d.Width == 1920 && d.Height == 1080);
    }
    for (int invalid = 0; invalid < 5; ++invalid)
    {
        auto d = base;
        if (invalid == 0) d.SampleDesc.Count = 4;
        if (invalid == 1) d.BufferCount = 17;
        if (invalid == 2) d.Stereo = TRUE;
        if (invalid == 3) d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (invalid == 4) d.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(99);
        const auto before = d;
        assert(!Dx12InteropDesc::Prepare(d, true));
        assert(std::memcmp(&d, &before, sizeof(d)) == 0);
    }
    for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM})
    {
        DXGI_SWAP_CHAIN_DESC d {};
        d.BufferDesc.Format = format; d.SampleDesc.Count = 1;
        d.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL; d.BufferCount = 16;
        assert(Dx12InteropDesc::Prepare(d, false));
        assert(d.Windowed && d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && d.BufferCount == 16);
        assert(d.BufferDesc.Format == format);
    }
    assert(!Dx12InteropDesc::SupportsTearing(nullptr));
    std::puts("DX12 interop descriptor: PASS");
}
