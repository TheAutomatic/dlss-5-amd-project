#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Submission;
static void Require(bool ok, const char *what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, hr); std::exit(1); }
}
int main(int argc, char **argv)
{
    Require(argc == 2, "supply raygeneration DXIL library");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    else std::puts("NOTE: D3D12 debug layer unavailable");
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<ID3D12Device5> device;
    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device5> candidate;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate)))) continue;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options {};
        if (SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) &&
            options.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            device = candidate;
            std::printf("DXR adapter: %ls\n", desc.Description);
            break;
        }
    }
    if (!device) { std::puts("lmxxf_state_object: SKIP (no hardware DXR adapter)"); return 0; }
    ComPtr<ID3D12InfoQueue> info;
    device.As(&info);
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC rd {}; rd.NumParameters = 2; rd.pParameters = params;
    ComPtr<ID3DBlob> signature, errors;
    Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "serialize root");
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    std::ifstream file(argv[1], std::ios::binary);
    Require(file.good(), "DXIL file exists");
    std::vector<char> dxil((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Require(!dxil.empty(), "DXIL nonempty");
    D3D12_EXPORT_DESC exp {L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE};
    D3D12_DXIL_LIBRARY_DESC library {{dxil.data(), dxil.size()}, 1, &exp};
    D3D12_GLOBAL_ROOT_SIGNATURE global {root.Get()};
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig {0, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig {1};
    D3D12_STATE_SUBOBJECT sub[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}};
    D3D12_STATE_OBJECT_DESC sd {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 4, sub};
    ComPtr<ID3D12StateObject> state;
    Check(device->CreateStateObject(&sd, IID_PPV_ARGS(&state)), "raytracing state object");
    ComPtr<ID3D12StateObjectProperties> properties;
    Check(state.As(&properties), "state properties");
    const char *cs = "RWByteAddressBuffer output:register(u0); cbuffer P:register(b0){uint slot;uint value;}"
                     "[numthreads(1,1,1)]void main(){output.Store(slot*4,value+100);}";
    ComPtr<ID3DBlob> shader;
    Check(D3DCompile(cs, std::strlen(cs), nullptr, nullptr, nullptr, "main", "cs_5_1", 0, 0, &shader, &errors), "compute shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {}; pd.pRootSignature = root.Get();
    pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "compute PSO");
    auto buffer = [&](D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial, bool uav) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = heap;
        D3D12_RESOURCE_DESC bd {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (uav) bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, initial, nullptr, IID_PPV_ARGS(&r)), "buffer");
        return r;
    };
    auto table = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    void *mapped = nullptr;
    Check(table->Map(0, nullptr, &mapped), "table map");
    const void *identifier = properties->GetShaderIdentifier(L"RayGen");
    Require(identifier != nullptr, "raygen identifier");
    std::memcpy(mapped, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    table->Unmap(0, nullptr);
    D3D12_DISPATCH_RAYS_DESC rays {};
    rays.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.Width = rays.Height = rays.Depth = 1;
    D3D12_COMMAND_QUEUE_DESC qd {};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event");
    UINT64 serial = 0;
    // Repeat on the same proxy after Reset; also cover ClearState discarding an old RT binding.
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        ComPtr<ID3D12GraphicsCommandList> native;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&native)), "list");
        ComPtr<CommandListProxy> cmd;
        Check(CommandListProxy::Create(device.Get(), allocator.Get(), native.Get(), &cmd), "proxy");
        for (unsigned generation = 0; generation < 2; ++generation)
        {
            auto output = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            auto readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
            if (generation)
            {
                Check(allocator->Reset(), "allocator reset after GPU completion");
                Check(cmd->Reset(allocator.Get(), nullptr), "proxy reset");
                Require(!cmd->IsSplitIneligible(), "reset clears previous cut");
            }
            if (mode == 1) cmd->SetPipelineState(pso.Get());
            cmd->SetPipelineState1(state.Get());
            cmd->SetPipelineState1(state.Get()); // Repeated identity must retain a live reference.
            if (mode == 2) cmd->SetPipelineState(pso.Get());
            if (mode == 3) cmd->ClearState(pso.Get());
            cmd->SetComputeRootSignature(root.Get());
            cmd->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
            const UINT values[] = {0, 11 + generation};
            cmd->SetComputeRoot32BitConstants(1, 2, values, 0);
            if (mode < 2) cmd->DispatchRays(&rays); else cmd->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = output.Get(); cmd->ResourceBarrier(1, &barrier);
            Require(!cmd->IsSplitIneligible(), "recorded DXR commands remain splittable");
            Check(cmd->SplitSegments(), "split after pipeline/dispatch");
            // Rebind only the slot: state object/PSO, root UAV and value must survive the cut.
            cmd->SetComputeRoot32BitConstant(1, 1, 0);
            if (mode < 2) cmd->DispatchRays(&rays); else cmd->Dispatch(1, 1, 1);
            barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd->ResourceBarrier(1, &barrier);
            cmd->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 8);
            Check(cmd->Close(), "continuation close");
            Check(cmd->ExecuteOn(queue.Get()), "execute split pair");
            Check(queue->Signal(fence.Get(), ++serial), "signal");
            Check(fence->SetEventOnCompletion(serial, event), "completion event");
            Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU timeout");
            Check(device->GetDeviceRemovedReason(), "device healthy");
            Require(fence->GetCompletedValue() != UINT64_MAX, "valid completion");
            D3D12_RANGE range {0, 8};
            Check(readback->Map(0, &range, &mapped), "readback map");
            const auto *got = static_cast<const UINT *>(mapped);
            const UINT expected = values[1] + (mode < 2 ? 0 : 100);
            Require(got[0] == expected && got[1] == expected, "producer and continuation output match");
            range = {0, 0}; readback->Unmap(0, &range);
        }
    }
    // Lifetime checks use actual COM objects; no driver-invalid commands required.
    ContinuationState snapshot;
    snapshot.OnStateObject(state.Get()); snapshot.OnStateObject(state.Get());
    Require(snapshot.stateObject == state.Get() && snapshot.stateObjectLast, "state object retained");
    snapshot.OnPso(pso.Get()); Require(!snapshot.stateObjectLast, "PSO latest");
    snapshot.Reset(); Require(!snapshot.stateObject && !snapshot.pso, "reset releases pipelines");
    CommandListProxy guard;
    guard.BuildRaytracingAccelerationStructure(nullptr, 0, nullptr);
    Require(!guard.IsSplitIneligible(), "AS-build no longer blocks the cut (RT+NR coexist)");
    if (info)
    {
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; Check(info->GetMessage(i, nullptr, &size), "debug message size");
            std::vector<char> bytes(size);
            auto *msg = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            Check(info->GetMessage(i, msg, &size), "debug message");
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::fprintf(stderr, "%s\n", msg->pDescription);
                Require(false, "D3D12 debug error");
            }
        }
    }
    CloseHandle(event);
    std::puts("lmxxf_state_object: PASS (8 DXR/compute cuts, both binding orders, Reset/ClearState, GPU readback, AS-build split allowed)");
}
