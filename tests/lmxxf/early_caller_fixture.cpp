// Build under two DLL names to exercise the real return-address module filter.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

extern "C" __declspec(dllexport) __declspec(noinline) HRESULT WINAPI CreateFixtureList(
    ID3D12Device *device, ID3D12CommandAllocator *allocator, D3D12_COMMAND_LIST_TYPE type,
    BOOL closed, ID3D12GraphicsCommandList **out)
{
    if (closed)
    {
        ID3D12Device4 *d4 = nullptr;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&d4));
        if (FAILED(hr)) return hr;
        hr = d4->CreateCommandList1(0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(out));
        d4->Release();
        return hr;
    }
    // Keep the API return address in this DLL even in optimized test builds.
    volatile HRESULT hr = device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(out));
    return hr;
}
