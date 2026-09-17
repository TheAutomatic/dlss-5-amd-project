// Capture descriptors emitted by the production helper; no device/GPU is required.
#include <pch.h>
#include <shaders/Shader_Dx12.h>
#include <array>
#include <cassert>
#include <cstdio>

#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Common.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Dx12.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/gpu_time/GpuTime_Dx12.cpp"

// Keep production defaults, without reading or writing a user's INI.
Config::Config() = default;
Config* Config::Instance()
{
    static Config instance;
    return &instance;
}

namespace
{
static_assert(sizeof(void*) == 8, "The COM ABI fixture requires x64");

struct DeviceCalls
{
    void** vtable;
    unsigned srvCalls = 0;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
};

void STDMETHODCALLTYPE UnexpectedCall() { std::abort(); }

// A real C++ interface override preserves the struct-return ABI of GetDesc.
class ResourceCalls : public ID3D12Resource
{
  public:
    D3D12_RESOURCE_DESC desc {};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { std::abort(); }
    ULONG STDMETHODCALLTYPE AddRef() override { std::abort(); }
    ULONG STDMETHODCALLTYPE Release() override { std::abort(); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE*, void**) override { std::abort(); }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE*) override { std::abort(); }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { std::abort(); }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override
    { std::abort(); }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override
    { std::abort(); }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS*) override
    { std::abort(); }
};

HRESULT STDMETHODCALLTYPE CreateQueryHeap(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void** output)
{
    // Disable the unrelated timer owned by Shader_Dx12.
    *output = nullptr;
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE CreateSrv(ID3D12Device* self, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    assert(handle.ptr == 1234);
    auto& calls = *reinterpret_cast<DeviceCalls*>(self);
    ++calls.srvCalls;
    calls.srv = *desc;
}

class ShaderProbe : public Shader_Dx12
{
  public:
    explicit ShaderProbe(ID3D12Device* device) : Shader_Dx12("SRV test", device) {}
    using Shader_Dx12::CreateShaderResourceView;
};
} // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    {
        GpuTime_Dx12 empty(nullptr);
        empty.Start(nullptr);
        empty.End(nullptr);
        assert(!empty.ReadGpuTime(nullptr).has_value());
    }
    std::array<void*, 44> deviceTable;
    deviceTable.fill(reinterpret_cast<void*>(&UnexpectedCall));
    deviceTable[18] = reinterpret_cast<void*>(&CreateSrv);
    deviceTable[39] = reinterpret_cast<void*>(&CreateQueryHeap);
    DeviceCalls calls { deviceTable.data() };
    auto* device = reinterpret_cast<ID3D12Device*>(&calls);

    ResourceCalls resource;
    resource.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.desc.Width = 1280;
    resource.desc.Height = 720;
    resource.desc.DepthOrArraySize = 1;
    resource.desc.MipLevels = 3;
    ID3D12Resource* texture = &resource;
    ShaderProbe shader(device);

    auto check = [&](DXGI_FORMAT resourceFormat, DXGI_FORMAT explicitFormat, DXGI_FORMAT expected)
    {
        resource.desc.Format = resourceFormat;
        const auto before = calls.srvCalls;
        shader.CreateShaderResourceView(device, texture, { 1234 }, explicitFormat);
        assert(calls.srvCalls == before + 1);
        assert(calls.srv.Format == expected);
        assert(calls.srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        if (resource.desc.DepthOrArraySize == 1)
        {
            assert(calls.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);
            assert(calls.srv.Texture2D.MipLevels == 3);
        }
        else
        {
            assert(calls.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
            assert(calls.srv.Texture2DArray.ArraySize == 2);
            assert(calls.srv.Texture2DArray.MipLevels == 3);
        }
    };

    check(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
          DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    check(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    check(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    check(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT);
    check(DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT);
    check(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
    check(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM);
    check(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    resource.desc.DepthOrArraySize = 2;
    check(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
          DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);

    resource.desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    const auto before = calls.srvCalls;
    bool rejected = false;
    try { shader.CreateShaderResourceView(device, texture, { 1234 }); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && calls.srvCalls == before);
    std::puts("shader_dx12_srv: 9 descriptor cases, DENY_SHADER_RESOURCE, and null-device GpuTime passed");
}
