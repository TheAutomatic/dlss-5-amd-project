// Exercise the production DX11 helper implementations on Microsoft's WARP device.
// Config's test constructor keeps the real defaults without reading a game INI.
// No D3D interfaces, view creation, dispatch, or helper destructors are mocked.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/pch.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Util.h"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <wrl/client.h>

#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Common.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Dx11.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/rcas/RCAS_Common.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/rcas/RCAS_Dx11.cpp"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/gpu_time/GpuTime_Dx11.cpp"

Config::Config() = default;
Config* Config::Instance()
{
    static Config instance;
    return &instance;
}

void Util::GetDeviceRemovedReason(ID3D11Device*)
{
    throw std::runtime_error("WARP device removed during ownership regression");
}

namespace
{
using Microsoft::WRL::ComPtr;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

void Finish(ID3D11Device* device, ID3D11DeviceContext* context)
{
    context->ClearState();
    D3D11_QUERY_DESC desc {};
    desc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> completed;
    Check(device->CreateQuery(&desc, &completed), "Create completion query");
    context->End(completed.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    HRESULT result;
    while ((result = context->GetData(completed.Get(), nullptr, 0, 0)) == S_FALSE)
    {
        Require(std::chrono::steady_clock::now() < deadline, "WARP dispatch did not finish");
        std::this_thread::yield();
    }
    Check(result, "Wait for WARP dispatch");
}

ULONG References(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

struct Textures
{
    std::array<ComPtr<ID3D11Texture2D>, 4> resources;

    Textures(ID3D11Device* device, UINT size)
    {
        const DXGI_FORMAT formats[] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
        for (size_t i = 0; i < resources.size(); ++i)
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = size;
            desc.Height = size;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = formats[i];
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            Check(device->CreateTexture2D(&desc, nullptr, &resources[i]), "Create texture");
            // Keep a guard reference so an accidental extra Release reports a
            // deterministic count mismatch instead of dereferencing a freed COM object.
            resources[i]->AddRef();
        }
    }

    void VerifyAndReleaseGuards() const
    {
        const char* names[] = { "input", "motion vectors", "depth", "output" };
        bool valid = true;
        for (size_t i = 0; i < resources.size(); ++i)
        {
            const auto refs = References(resources[i].Get());
            if (refs != 2)
            {
                std::cerr << names[i] << ": expected 2 caller references, got " << refs << '\n';
                valid = false;
            }
        }
        // On failure leave guards alive until process exit to keep failure safe.
        Require(valid, "DX11 helper changed caller-owned resource references");
        for (const auto& resource : resources)
            resource->Release();
    }
};

void Dispatch(RCAS_Dx11& helper, ID3D11Device* device, ID3D11DeviceContext* context, const Textures& textures)
{
    RcasConstants constants {};
    constants.Sharpness = 0.2f;
    constants.MvScaleX = constants.MvScaleY = 1.0f;
    constants.CameraNear = 0.1f;
    constants.CameraFar = 1000.0f;
    constants.DepthIsLinear = true;
    Require(helper.Dispatch(device, context, textures.resources[0].Get(), textures.resources[1].Get(), constants,
                            textures.resources[3].Get(), textures.resources[2].Get()),
            "RCAS dispatch failed");
    Finish(device, context);
}
} // namespace

int main()
{
    try
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "D3D11CreateDevice(WARP)");
        Config::Instance()->UsePrecompiledShaders = true;
        State::Instance().isShuttingDown = false;

        // All paths exercise input/MV/output; depth-aware variants also exercise
        // the depth cache. Replace views, then destroy/rebuild the whole helper.
        const SharpenShader modes[] = { SharpenShader::RCAS, SharpenShader::DepthAware,
                                         SharpenShader::LocalContrastDepthAware };
        for (const auto mode : modes)
        {
            Config::Instance()->SharpnessShader = mode;
            for (int rebuild = 0; rebuild < 2; ++rebuild)
            {
                Textures first(device.Get(), 32);
                Textures replacement(device.Get(), 64);
                ComPtr<ID3D11Texture2D> ownedBuffer;
                {
                    RCAS_Dx11 helper("ownership-regression", device.Get());
                    Require(helper.IsInit(), "RCAS constructor failed");
                    Require(helper.CreateBufferResource(device.Get(), first.resources[0].Get()),
                            "Helper buffer allocation failed");
                    Dispatch(helper, device.Get(), context.Get(), first);
                    Dispatch(helper, device.Get(), context.Get(), replacement);
                    Require(helper.CreateBufferResource(device.Get(), replacement.resources[0].Get()),
                            "Helper buffer resize failed");
                    ownedBuffer = helper.Buffer();
                    Require(References(ownedBuffer.Get()) == 2, "Helper should own its allocated buffer");
                }
                Finish(device.Get(), context.Get());
                Require(References(ownedBuffer.Get()) == 1, "Helper leaked or over-released its allocated buffer");
                first.VerifyAndReleaseGuards();
                replacement.VerifyAndReleaseGuards();
            }
        }
        std::cout << "DX11 WARP ownership: all 3 sharpen modes, view replacement, buffer resize, and 6 helper "
                     "destroy/rebuild cycles passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
