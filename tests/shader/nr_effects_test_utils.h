#pragma once
#include "../lmxxf/lmxxf_gpu_test_utils.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/effects/NrOutputEffects.h"
#include <limits>
using namespace DlssNr;
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static float Half(UINT16 h)
{
    const float sign = h & 0x8000 ? -1.f : 1.f;
    const unsigned exponent = (h >> 10) & 31, fraction = h & 1023;
    return sign * (exponent ? std::ldexp(1.f + float(fraction)/1024.f, int(exponent)-15) : std::ldexp(float(fraction), -24));
}
static Ptr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h)
{
    D3D12_RESOURCE_DESC desc {}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=w; desc.Height=h; desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp {}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         nullptr,IID_PPV_ARGS(&resource)),"texture"); return resource;
}
struct Recording
{
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<Submission::CommandListProxy> proxy;
    ID3D12Resource* output = nullptr;
};
static Recording NewRecording(ID3D12Device* device)
{
    Recording out; Ptr<ID3D12GraphicsCommandList> real;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&out.allocator)),"allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,out.allocator.Get(),nullptr,IID_PPV_ARGS(&real)),"list");
    Submission::CommandListProxy* proxy=nullptr;
    Check(Submission::CommandListProxy::Create(device,out.allocator.Get(),real.Get(),&proxy),"proxy"); out.proxy.Attach(proxy);
    return out;
}
static void CheckPixels(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* output, float intensity)
{
    auto td=output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 bytes=0;
    device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC desc {}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
    desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_READBACK;Ptr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
    Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"copy allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"copy list");
    Effects::Barrier(list.Get(),output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src {},dst {};src.pResource=output;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Effects::Barrier(list.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Check(list->Close(),"copy close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);WaitQueue(device,queue);
    void* data=nullptr;D3D12_RANGE range {0,SIZE_T(bytes)};Check(readback->Map(0,&range,&data),"map");
    std::vector<unsigned char> expected(size_t(td.Width)*8);
    for(UINT y=0;y<td.Height;++y){FillRow(expected.data(),y,UINT(td.Width),td.Format);
        const auto* actual=reinterpret_cast<const UINT16*>(static_cast<unsigned char*>(data)+size_t(y)*fp.Footprint.RowPitch);
        const auto* base=reinterpret_cast<const UINT16*>(expected.data());
        for(UINT x=0;x<td.Width;++x)for(UINT c=0;c<4;++c){
            const float wanted=Half(base[x*4+c])*(c==3?1.f:1.f+intensity);
            Require(std::fabs(Half(actual[x*4+c])-wanted)<=(std::max)(.001f,std::fabs(wanted)*.001f),"numerical blend/alpha");
        }
    }
    D3D12_RANGE written {0,0};readback->Unmap(0,&written);
}
static bool NoLeases() { std::lock_guard lock(Submission::RecordingMutex()); return Effects::Global().leases.empty(); }

static void SelectEffectsAdapter(IDXGIFactory4* factory, IDXGIAdapter** out)
{
    char enabled[2] {};
    if (GetEnvironmentVariableA("NR_EFFECTS_HARDWARE", enabled, 2) && enabled[0] == '1') {
        for (UINT i = 0;; ++i) {
            Ptr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc {}; Check(adapter->GetDesc1(&desc), "adapter description");
            if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                Check(adapter->QueryInterface(IID_PPV_ARGS(out)), "AMD adapter"); std::puts("NR effects adapter: AMD hardware"); return;
            }
        }
        Require(false, "AMD hardware requested but unavailable");
    }
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(out)), "WARP");
}
