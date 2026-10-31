#define NOMINMAX
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/NrColourState.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Hr(HRESULT hr) { if (FAILED(hr)) { std::printf("HRESULT %08lX\n", hr); throw std::runtime_error("DX12 failed"); } }

static void Warp()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    else std::puts("Debug layer unavailable; testing actual WARP recording and execution");
    ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    D3D12_COMMAND_QUEUE_DESC qdesc {};
    ComPtr<ID3D12CommandQueue> queue; Hr(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=16; desc.Height=16;
    desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap {}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> source,copy;
    const auto state=DlssNr::ResolveNrColourState(true,D3D12_RESOURCE_STATE_RENDER_TARGET,{},DlssNr::kRrComposedColourState);
    assert((state&D3D12_RESOURCE_STATE_RENDER_TARGET)==0);
    assert((desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)==0);
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&source)));
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&copy)));
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE completed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!completed) throw std::runtime_error("CreateEvent failed");
    for (unsigned frame=0; frame<2; ++frame)
    {
        if (frame) { Hr(allocator->Reset()); Hr(list->Reset(allocator.Get(),nullptr)); }
        // First call bypasses NR while discovering the queue; second records a copy.
        // Neither is allowed to inherit the UE RENDER_TARGET state for owned colour.
        if (frame)
        {
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=source.Get(); barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore=state; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1,&barrier);
            list->CopyResource(copy.Get(),source.Get());
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter=state;
            list->ResourceBarrier(1,&barrier);
        }
        Hr(list->Close()); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        Hr(queue->Signal(fence.Get(),frame+1)); Hr(fence->SetEventOnCompletion(frame+1,completed));
        if (WaitForSingleObject(completed,10000)!=WAIT_OBJECT_0) throw std::runtime_error("WARP fence timeout");
    }
    CloseHandle(completed);
    Hr(device->GetDeviceRemovedReason());
    if (info)
        for (UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        {
            SIZE_T size=0; Hr(info->GetMessage(i,nullptr,&size)); std::vector<unsigned char> bytes(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); Hr(info->GetMessage(i,message,&size));
            if (message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
                std::puts(message->pDescription); throw std::runtime_error("D3D12 validation error");
            }
        }
    std::puts("RR owned colour WARP: PASS (bypass + copy + actual retirement)");
}

int main(int argc,char** argv)
{
    using namespace DlssNr;
    assert(ResolveNrColourState(true,{},{})==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    assert(ResolveNrColourState(true,D3D12_RESOURCE_STATE_RENDER_TARGET,{})==D3D12_RESOURCE_STATE_RENDER_TARGET);
    assert(ResolveNrColourState(false,D3D12_RESOURCE_STATE_RENDER_TARGET,{})==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(ResolveNrColourState(false,{},D3D12_RESOURCE_STATE_COPY_SOURCE)==D3D12_RESOURCE_STATE_COPY_SOURCE);
    // Both explicit ini states and volatile UE defaults describe GAME resources.
    for (auto game : {D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS})
        assert(ResolveNrColourState(true,game,game,kRrComposedColourState)==kRrComposedColourState);
    std::puts("NR colour state CPU: PASS");
    if (argc>1 && std::strcmp(argv[1],"--warp")==0) Warp();
}
