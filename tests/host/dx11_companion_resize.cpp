#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/with_dx12/dx11_with_dx12_sync.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cassert>
#include <iostream>
#include <latch>
#include <thread>
using Microsoft::WRL::ComPtr;
int main()
{
    using Dx11wDx12Sync::Equivalent;
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Width=1280; d.BufferDesc.Height=720;
    d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferCount=2;
    SIZE current{1280,720}, changed{1920,1080}, minimized{0,0};
    assert(Equivalent(d,0,1280,720,DXGI_FORMAT_UNKNOWN,0,nullptr));
    assert(Equivalent(d,2,0,0,d.BufferDesc.Format,0,&current));
    assert(!Equivalent(d,2,0,0,d.BufferDesc.Format,0,&changed));
    assert(!Equivalent(d,2,0,720,d.BufferDesc.Format,0,nullptr));
    assert(!Equivalent(d,2,0,0,d.BufferDesc.Format,0,&minimized));
    assert(!Equivalent(d,3,1280,720,DXGI_FORMAT_UNKNOWN,0,nullptr));
    assert(!Equivalent(d,2,1280,720,DXGI_FORMAT_R16G16B16A16_FLOAT,0,nullptr));
    assert(!Equivalent(d,2,1280,720,DXGI_FORMAT_UNKNOWN,DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,nullptr));
    assert(!Equivalent(nullptr,0,0,0,DXGI_FORMAT_UNKNOWN,0,nullptr));
    // The shared production transaction is used by ResizeBuffers and ResizeBuffers1.
    for (int entry=0; entry<2; ++entry)
    {
        int releases=0, originals=0, companions=0;
        HRESULT fgError=S_OK;
        auto run=[&](bool wait, HRESULT game, HRESULT fg) {
            return Dx11wDx12Sync::ResizeTransaction([&]{return wait;},[&]{++releases;},
                [&]{++originals;return game;},[&]{++companions;return fg;},fgError);
        };
        assert(run(false,S_OK,S_OK)==DXGI_ERROR_WAS_STILL_DRAWING);
        assert(releases==0 && originals==0 && companions==0 && fgError==S_OK);
        assert(run(true,DXGI_ERROR_INVALID_CALL,S_OK)==DXGI_ERROR_INVALID_CALL);
        assert(releases==1 && originals==1 && companions==0);
        assert(run(true,S_OK,E_FAIL)==S_OK && fgError==E_FAIL);
        assert(run(true,S_OK,S_OK)==S_OK && fgError==S_OK);
    }
    // Controlled read/write exclusion, including recovery after a failed try_lock.
    for(int i=0;i<100;i++)
    {
        std::latch entered{1}, release{1};
        std::thread reader([&]{ std::shared_lock l(Dx11wDx12Sync::PresentResizeMutex()); entered.count_down(); release.wait(); });
        entered.wait();
        std::unique_lock writer(Dx11wDx12Sync::PresentResizeMutex(), std::try_to_lock);
        assert(!writer.owns_lock()); release.count_down(); reader.join();
        writer.lock(); assert(writer.owns_lock());
    }
    // Real software-device swapchain: a zero-size request tracks the HWND, not old desc.
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"NRResizeFixture";
    assert(RegisterClassW(&wc));
    HWND w=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,320,240,nullptr,nullptr,wc.hInstance,nullptr); assert(w);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ComPtr<IDXGIFactory2> factory; assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=320; desc.Height=240; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain; assert(SUCCEEDED(factory->CreateSwapChainForHwnd(device.Get(),w,&desc,nullptr,nullptr,&chain)));
    assert(Equivalent(chain.Get(),0,0,0,DXGI_FORMAT_UNKNOWN,0,w));
    assert(SetWindowPos(w,nullptr,0,0,640,480,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    assert(!Equivalent(chain.Get(),0,0,0,DXGI_FORMAT_UNKNOWN,0,w));
    assert(SUCCEEDED(chain->ResizeBuffers(0,0,0,DXGI_FORMAT_UNKNOWN,0)));
    assert(Equivalent(chain.Get(),0,0,0,DXGI_FORMAT_UNKNOWN,0,w));
    chain.Reset(); context.Reset(); device.Reset(); factory.Reset(); DestroyWindow(w);
    std::cout<<"DX11 companion resize: PASS (parameter matrix, present barrier, WARP zero-size HWND resize)\n";
}
