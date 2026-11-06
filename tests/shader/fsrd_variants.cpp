// Direct-input production FSRDUpscalerDepth CSO dispatch/readback on WARP.
// Checks finite reversed-Z near/far, signed intermediate, and invalid zero depth.
#include <pch.h>
#include "lmxxf_gpu_test_utils.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cstdint>

#include <shaders/fsrd_preprocess/FSRDShaderData.h>

#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/fsrd_preprocess/precompile/FSRDUpscalerDepth_Shader.h"

using Microsoft::WRL::ComPtr;

namespace
{
struct GpuContext
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;

    void Flush()
    {
        Check(list->Close(), "Close list");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        ++fenceValue;
        Check(queue->Signal(fence.Get(), fenceValue), "Signal fence");
        if (fence->GetCompletedValue() < fenceValue)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Require(event != nullptr, "CreateEvent");
            Check(fence->SetEventOnCompletion(fenceValue, event), "SetEvent");
            Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "bounded wait for GPU execution");
            CloseHandle(event);
        }
        Require(SUCCEEDED(device->GetDeviceRemovedReason()), "Device healthy after execution");
        Check(allocator->Reset(), "Reset allocator");
        Check(list->Reset(allocator.Get(), nullptr), "Reset list");
    }
};

GpuContext CreateContext()
{
    GpuContext ctx;
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI Factory");
    ComPtr<IDXGIAdapter> adapter;
    factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    if (!adapter)
        factory->EnumAdapters(0, &adapter);
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx.device)), "D3D12 Device");

    D3D12_COMMAND_QUEUE_DESC qDesc { D3D12_COMMAND_LIST_TYPE_DIRECT };
    Check(ctx.device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&ctx.queue)), "CommandQueue");
    Check(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.allocator)), "Allocator");
    Check(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx.allocator.Get(), nullptr, IID_PPV_ARGS(&ctx.list)), "List");
    Check(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.fence)), "Fence");
    return ctx;
}

ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, UINT width, UINT height,
                                    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
{
    D3D12_HEAP_PROPERTIES heapProps { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> res;
    Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                         IID_PPV_ARGS(&res)), "CreateTexture");
    return res;
}

ComPtr<ID3D12Resource> CreateReadback(ID3D12Device* device, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heapProps { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&res)), "CreateReadback");
    return res;
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 bytes, const void* data)
{
    D3D12_HEAP_PROPERTIES heapProps { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (bytes + 255) & ~255ull;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    Check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&res)), "CreateUploadBuffer");
    if (data)
    {
        void* mapped = nullptr;
        Check(res->Map(0, nullptr, &mapped), "Map UploadBuffer");
        memcpy(mapped, data, bytes);
        res->Unmap(0, nullptr);
    }
    return res;
}


std::vector<uint8_t> ReadbackTexture(GpuContext& ctx, ID3D12Resource* tex, UINT width, UINT height, DXGI_FORMAT format)
{
    const UINT pixelSize = (format == DXGI_FORMAT_R32G32B32A32_FLOAT || format == DXGI_FORMAT_R32G32B32A32_UINT) ? 16 :
                           (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT) ? 8 : 4;
    const UINT rowPitch = (width * pixelSize + 255) & ~255;
    auto rb = CreateReadback(ctx.device.Get(), rowPitch * height);

    D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION srcLoc {}, dstLoc {};
    srcLoc.pResource = tex;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.pResource = rb.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Footprint.Width = width;
    dstLoc.PlacedFootprint.Footprint.Height = height;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
    dstLoc.PlacedFootprint.Footprint.Format = format;
    ctx.list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.list->ResourceBarrier(1, &toUav);
    ctx.Flush();

    std::vector<uint8_t> data(size_t(width) * pixelSize * height);
    void* mapped = nullptr;
    Check(rb->Map(0, nullptr, &mapped), "Map Readback");
    for (UINT y = 0; y < height; ++y)
        memcpy(data.data() + size_t(y) * width * pixelSize,
               static_cast<const uint8_t*>(mapped) + size_t(y) * rowPitch, size_t(width) * pixelSize);
    rb->Unmap(0, nullptr);
    return data;
}


void TestUpscalerDepthEncoding(GpuContext& ctx)
{
    std::cout << "[FSRD_GPU] Testing canonical SR depth shader dispatch on real WARP..." << std::endl;
    ComPtr<ID3D12RootSignature> rootSig;
    Check(ctx.device->CreateRootSignature(0, FSRDUpscalerDepth_cso, sizeof(FSRDUpscalerDepth_cso), IID_PPV_ARGS(&rootSig)), "RootSig Depth");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = { FSRDUpscalerDepth_cso, sizeof(FSRDUpscalerDepth_cso) };
    ComPtr<ID3D12PipelineState> pso;
    Check(ctx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "PSO Depth");

    const UINT width = 33, height = 17; // non-power-of-two odd tile
    auto depthOut = CreateTexture(ctx.device.Get(), width, height, DXGI_FORMAT_R32_FLOAT);
    auto depthIn = CreateTexture(ctx.device.Get(), width, height, DXGI_FORMAT_R32_FLOAT);

    const UINT rowPitch = (width * 4 + 255) & ~255;
    std::vector<float> inputDepths(width * height);
    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            if (x == 0 && y == 0) inputDepths[y * width + x] = 1.0f; // near plane
            else if (x == 1 && y == 0) inputDepths[y * width + x] = 1000.0f; // far plane
            else if (x == 2 && y == 0) inputDepths[y * width + x] = -10.0f; // signed depth L=10
            else inputDepths[y * width + x] = 0.0f; // invalid -> maps to far
        }
    }

    auto uploadBuf = CreateUploadBuffer(ctx.device.Get(), rowPitch * height, nullptr);
    void* mappedUpload = nullptr;
    Check(uploadBuf->Map(0, nullptr, &mappedUpload), "Map Depth Upload");
    for (UINT y = 0; y < height; ++y)
        memcpy(static_cast<byte*>(mappedUpload) + y * rowPitch, &inputDepths[y * width], width * sizeof(float));
    uploadBuf->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER bIn = CD3DX12_RESOURCE_BARRIER::Transition(depthIn.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx.list->ResourceBarrier(1, &bIn);
    D3D12_TEXTURE_COPY_LOCATION srcCopy {}, dstCopy {};
    srcCopy.pResource = uploadBuf.Get();
    srcCopy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCopy.PlacedFootprint.Footprint.Width = width;
    srcCopy.PlacedFootprint.Footprint.Height = height;
    srcCopy.PlacedFootprint.Footprint.Depth = 1;
    srcCopy.PlacedFootprint.Footprint.RowPitch = rowPitch;
    srcCopy.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dstCopy.pResource = depthIn.Get();
    dstCopy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ctx.list->CopyTextureRegion(&dstCopy, 0, 0, 0, &srcCopy, nullptr);
    D3D12_RESOURCE_BARRIER bInSrv = CD3DX12_RESOURCE_BARRIER::Transition(depthIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.list->ResourceBarrier(1, &bInSrv);

    // Constant buffer: near=1, far=1000, reversed-Z (InvertedOutput)
    FSRD::UpscalerDepth::Constants constants {};
    constants.RenderSize = { float(width), float(height), 1.0f / width, 1.0f / height };
    constants.NearPlane = 1.0f;
    constants.FarPlane = 1000.0f;
    constants.Flags = uint32_t(FSRD::UpscalerDepth::Flags::InvertedOutput);
    auto cb = CreateUploadBuffer(ctx.device.Get(), sizeof(constants), &constants);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(ctx.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Heap");

    const UINT inc = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    ctx.device->CreateShaderResourceView(depthIn.Get(), &srvDesc, cpu);

    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ctx.device->CreateUnorderedAccessView(depthOut.Get(), nullptr, &uavDesc, cpu);

    ctx.list->SetPipelineState(pso.Get());
    ctx.list->SetComputeRootSignature(rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { heap.Get() };
    ctx.list->SetDescriptorHeaps(1, heaps);
    ctx.list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    ctx.list->SetComputeRootDescriptorTable(1, gpu);
    auto gpuUav = gpu;
    gpuUav.ptr += inc;
    ctx.list->SetComputeRootDescriptorTable(2, gpuUav);
    ctx.list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    auto readbackData = ReadbackTexture(ctx, depthOut.Get(), width, height, DXGI_FORMAT_R32_FLOAT);
    const auto* row0 = reinterpret_cast<const float*>(readbackData.data());

    // Numerical assertions:
    Require(std::abs(row0[0] - 1.0f) < 1e-4f, "Near plane encodes to 1.0 in reversed-Z");
    Require(std::abs(row0[1] - 0.0f) < 1e-4f, "Far plane encodes to 0.0 in reversed-Z");
    Require(std::abs(row0[2] - (0.099f / 0.999f)) < 1e-4f, "Signed depth L=10 encodes to exact intermediate value");
    Require(std::abs(row0[3] - 0.0f) < 1e-4f, "Invalid depth L=0 encodes to far plane (0.0)");

    std::cout << "[FSRD_GPU] Canonical SR depth execution and exact numeric readbacks verified." << std::endl;
}


}

int main()
{
    try
    {
        GpuContext ctx = CreateContext();
        TestUpscalerDepthEncoding(ctx);
        std::cout << "fsrd_variants: direct-depth encoding PASS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "fsrd_variants: EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
}
