#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Submission;
using RootFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
static RootFn nativeRoot, proxyRoot;
static bool tracking = true;
static thread_local bool inLateHook = false;
static std::unordered_map<ID3D12GraphicsCommandList*, ID3D12RootSignature*> captured;
static ID3D12GraphicsCommandList* nativeReceiver;
static void Require(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void STDMETHODCALLTYPE NativeHook(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root)
{
    nativeReceiver = list;
    if (tracking && !inLateHook && root) captured.insert_or_assign(list, root);
    nativeRoot(list, root);
}
static void STDMETHODCALLTYPE LateHook(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root)
{
    inLateHook = true;
    if (tracking && root) captured.insert_or_assign(list, root);
    proxyRoot(list, root);
    inLateHook = false;
}

// Exercise real Detours trampolines on the production proxy. The host's legacy
// contract is caller-keyed capture, capture suppression inside SR/NR, and replay
// through the late trampoline with the same logical object after a split.
int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP");
    Require(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
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
    Require(SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)), "serialize");
    ComPtr<ID3D12RootSignature> gameRoot, passRoot;
    Require(SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&gameRoot))), "game root");
    parameter.Constants.Num32BitValues = 2;
    blob.Reset();
    Require(SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)), "serialize pass");
    Require(SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&passRoot))), "pass root");

    nativeRoot = reinterpret_cast<RootFn>((*reinterpret_cast<void***>(native.Get()))[30]);
    proxyRoot = reinterpret_cast<RootFn>((*reinterpret_cast<void***>(proxy.Get()))[30]);
    Require(nativeRoot != proxyRoot, "native and proxy have distinct call targets");
    Require(DetourTransactionBegin() == NO_ERROR, "begin attach");
    Require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "update thread");
    Require(DetourAttach(reinterpret_cast<void**>(&nativeRoot), NativeHook) == NO_ERROR, "attach native");
    Require(DetourAttach(reinterpret_cast<void**>(&proxyRoot), LateHook) == NO_ERROR, "attach proxy");
    Require(DetourTransactionCommit() == NO_ERROR, "commit attach");

    proxy->SetGraphicsRootSignature(gameRoot.Get());
    proxy->SetGraphicsRoot32BitConstant(0, 42, 0);
    Require(captured.contains(proxy.Get()) && captured.at(proxy.Get()) == gameRoot.Get(), "capture uses caller identity");
    Require(!captured.contains(native.Get()), "forwarded native capture is suppressed by late hook");
    Require(nativeReceiver == native.Get(), "proxy forwards native this");

    tracking = false;
    proxy->SetGraphicsRootSignature(passRoot.Get());
    Require(captured.at(proxy.Get()) == gameRoot.Get(), "pass does not replace saved game root");
    Require(SUCCEEDED(raw->SplitSegments()), "split");
    auto* continuation = GraphicsRecordingList(proxy.Get());
    Require(continuation != native.Get(), "new native continuation");
    Require(!captured.contains(continuation), "no native-map migration needed");
    proxyRoot(proxy.Get(), captured.at(proxy.Get()));
    Require(nativeReceiver == continuation, "late replay reaches current continuation");
    proxy->SetGraphicsRoot32BitConstant(0, 42, 0);
    Require(raw->CapturedGfxRootCount() == 1, "replay restores proxy seed as well as native binding");
    proxyRoot(proxy.Get(), passRoot.Get());
    Require(raw->CapturedGfxRootCount() == 0, "different replayed signature invalidates seed root arguments");
    Require(captured.at(proxy.Get()) == gameRoot.Get(), "replay preserves caller-keyed saved root");
    Require(SUCCEEDED(proxy->Close()), "close");

    Require(DetourTransactionBegin() == NO_ERROR, "begin detach");
    Require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "update detach thread");
    Require(DetourDetach(reinterpret_cast<void**>(&proxyRoot), LateHook) == NO_ERROR, "detach proxy");
    Require(DetourDetach(reinterpret_cast<void**>(&nativeRoot), NativeHook) == NO_ERROR, "detach native");
    Require(DetourTransactionCommit() == NO_ERROR, "commit detach");
    std::puts("PASS: legacy caller capture, native forwarding, split and late-trampoline replay");
}
