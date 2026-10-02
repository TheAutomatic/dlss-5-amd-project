#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/RootRestoreIdentity.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Submission;
static void Require(bool value, const char* label)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
static ID3D12GraphicsCommandList* trampolineTarget;
static void STDMETHODCALLTYPE NativeRoot(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root)
{
    trampolineTarget = list;
    list->SetGraphicsRootSignature(root);
}

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> native;
    Require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
    Require(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&native))), "native list");
    CommandListProxy* raw = nullptr;
    Require(SUCCEEDED(CommandListProxy::Create(device.Get(), allocator.Get(), native.Get(), &raw)), "proxy");
    ComPtr<ID3D12GraphicsCommandList> proxy;
    proxy.Attach(raw);

    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC desc {};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    ComPtr<ID3DBlob> blob, errors;
    Require(SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)), "serialize root");
    ComPtr<ID3D12RootSignature> root;
    Require(SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&root))), "root signature");
    proxy->SetGraphicsRootSignature(root.Get());
    proxy->SetGraphicsRoot32BitConstant(0, 42, 0);
    std::unordered_map<ID3D12GraphicsCommandList*, ID3D12RootSignature*> states;
    states[native.Get()] = root.Get(); // Native hooks observe the forwarded call.
    Require(!states.contains(proxy.Get()), "old proxy lookup reproduces missing state");
    Require(states.contains(RootRestoreIdentity::Key(proxy.Get())), "native identity admits proxy");
    Require(RootRestoreIdentity::Key(native.Get()) == native.Get(), "native identity unchanged");

    RootRestoreIdentity::Replay(native.Get(), NativeRoot, &ID3D12GraphicsCommandList::SetGraphicsRootSignature, root.Get());
    Require(trampolineTarget == native.Get(), "native trampoline receives native this");
    Require(SUCCEEDED(raw->SplitSegments()), "split");
    auto* continuation = RootRestoreIdentity::Key(proxy.Get());
    Require(continuation != native.Get(), "destination changes after split");
    Require(!states.contains(continuation), "continuation has no producer capture yet");
    RootRestoreIdentity::Transfer(states, native.Get(), continuation);
    Require(states.at(continuation) == root.Get(), "producer capture follows continuation");

    // Simulate NR changing the proxy's bindings, then restoring the game root.
    proxy->SetGraphicsRootSignature(nullptr);
    trampolineTarget = nullptr;
    RootRestoreIdentity::Replay(proxy.Get(), NativeRoot, &ID3D12GraphicsCommandList::SetGraphicsRootSignature,
                               states.at(continuation));
    Require(trampolineTarget == nullptr, "proxy is never passed to native trampoline");
    proxy->SetGraphicsRoot32BitConstant(0, 42, 0);
    Require(raw->CapturedGfxRootCount() == 1, "restored proxy seed accepts game root bindings");
    RootRestoreIdentity::Replay(proxy.Get(), NativeRoot, &ID3D12GraphicsCommandList::SetGraphicsRootSignature,
                               static_cast<ID3D12RootSignature*>(nullptr));
    Require(raw->CapturedGfxRootCount() == 0, "replay updates proxy seed as well as native state");

    states.erase(native.Get());
    RootRestoreIdentity::Transfer(states, native.Get(), continuation);
    Require(!states.contains(continuation), "missing source cannot reuse stale destination state");
    states[native.Get()] = root.Get();
    RootRestoreIdentity::Transfer(states, native.Get(), native.Get());
    Require(states.at(native.Get()) == root.Get(), "unsplit transfer is a no-op");
    Require(SUCCEEDED(proxy->Close()), "close");
    std::puts("PASS: root restore native/proxy identity, split transfer, replay and missing state");
}
