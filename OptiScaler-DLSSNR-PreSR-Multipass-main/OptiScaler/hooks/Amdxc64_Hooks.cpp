#include "pch.h"
#include "Amdxc64_Hooks.h"
#include "RetryableDetour.h"

#include <fsr4/FSR4Upgrade.h>
#include <ffx_antilag2_dx12.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>
#include <low_latency/input/input_antilag2.h>
#include <misc/IdentifyGpu.h>
#include "Streamline_Hooks.h"

#pragma endregion

struct AmdExtD3DFactory : public IAmdExtD3DFactory
{
    bool linux = false;
    FSR4Support fsr4Support {};
    FSR4Support realFsr4Support {};
    bool fsr4ForcedSupport = false;

    HRESULT STDMETHODCALLTYPE CreateInterface(IUnknown* pOuter, REFIID riid, void** ppvObject) override
    {
        if (riid == __uuidof(IAmdExtD3DShaderIntrinsics))
        {
            if (Amdxc64Hooks::amdExtD3DShaderIntrinsics == nullptr)
                Amdxc64Hooks::amdExtD3DShaderIntrinsics = new AmdExtD3DShaderIntrinsics();

            *ppvObject = Amdxc64Hooks::amdExtD3DShaderIntrinsics;

            Amdxc64Hooks::o_amdExtD3DFactory->CreateInterface(pOuter, riid,
                                                              (void**) &Amdxc64Hooks::o_amdExtD3DShaderIntrinsics);

            LOG_INFO("Custom IAmdExtD3DShaderIntrinsics queried, returning custom AmdExtD3DShaderIntrinsics");

            return S_OK;
        }
        // TODO: Implementation too incomplete to always enable it
        else if (riid == __uuidof(IAmdExtD3DDevice8) && (linux || fsr4ForcedSupport))
        {
            if (Amdxc64Hooks::amdExtD3DDevice8 == nullptr)
            {
                Amdxc64Hooks::amdExtD3DDevice8 = new AmdExtD3DDevice8();
                Amdxc64Hooks::amdExtD3DDevice8->fsr4Support = fsr4Support;
                Amdxc64Hooks::amdExtD3DDevice8->realFsr4Support = realFsr4Support;
            }

            *ppvObject = Amdxc64Hooks::amdExtD3DDevice8;

            LOG_INFO("Custom IAmdExtD3DDevice8 queried, returning custom AmdExtD3DDevice8");

            return S_OK;
        }
        else if (Amdxc64Hooks::o_amdExtD3DFactory)
        {
            return Amdxc64Hooks::o_amdExtD3DFactory->CreateInterface(pOuter, riid, ppvObject);
        }

        return E_NOINTERFACE;
    }

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (Amdxc64Hooks::o_amdExtD3DFactory)
            return Amdxc64Hooks::o_amdExtD3DFactory->QueryInterface(riid, ppvObject);

        return E_NOTIMPL;
    }
    ULONG __stdcall AddRef(void) override
    {
        if (Amdxc64Hooks::o_amdExtD3DFactory)
            return Amdxc64Hooks::o_amdExtD3DFactory->AddRef();

        return 0;
    }
    ULONG __stdcall Release(void) override
    {
        if (Amdxc64Hooks::o_amdExtD3DFactory)
        {
            auto result = Amdxc64Hooks::o_amdExtD3DFactory->Release();

            if (result == 0)
                Amdxc64Hooks::o_amdExtD3DFactory = nullptr;

            return result;
        }

        return 0;
    }
};

namespace
{
HookInit::RetryableDetour<PFN_AmdExtD3DCreateInterface> createInterfaceHook;
}

void Amdxc64Hooks::Init()
{
    const auto result = createInterfaceHook.TryInstall([]() -> PFN_AmdExtD3DCreateInterface {
        auto module = KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll");
        // Preserve INI policy: RDNA2 custom DLL selection belongs to the loader.
        if (!module && !Config::Instance()->Fsr4DoNotLoadAmdxc64.value_or_default() &&
            !Config::Instance()->LoadCustomAmdxc64OnRdna2.value_or_default())
            module = NtdllProxy::LoadLibraryExW_Ldr(L"amdxc64.dll", nullptr, 0);
        if (!module) return nullptr; // A later driver load may retry.

        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(module), &pinned))
            return nullptr;
        return reinterpret_cast<PFN_AmdExtD3DCreateInterface>(
            KernelBaseProxy::GetProcAddress_()(pinned, "AmdExtD3DCreateInterface"));
    }, &hkAmdExtD3DCreateInterface);

    if (result.stage == HookInit::Stage::Commit && result.error == NO_ERROR)
        LOG_INFO("amdxc64.dll hook installed");
    else if (result.stage != HookInit::Stage::Busy && result.stage != HookInit::Stage::Resolve &&
             result.error != NO_ERROR)
        LOG_ERROR("amdxc64 hook failed at {}: {:X}; a later Init can retry",
                  HookInit::StageName(result.stage), result.error);
}

HRESULT STDMETHODCALLTYPE Amdxc64Hooks::hkAmdExtD3DCreateInterface(IUnknown* pOuter, REFIID riid, void** ppvObject)
{
    const auto original = createInterfaceHook.Original();
    if (!original)
    {
        if (ppvObject) *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    // We need to know D3D12 capabilities by now, one of them being FSR 4
    IdentifyGpu::updateD3d12Capabilities();

    const auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    const bool runFsr4Upgrade = primaryGpu.fsr4Support != FSR4Support::None;

    // Proton bleeding edge ships amdxc64 that is missing some required functions
    if (runFsr4Upgrade && riid == __uuidof(IAmdExtD3DFactory))
    {
        // Required for the custom AmdExtFfxApi, lack of it triggers visual glitches
        if (amdExtD3DFactory == nullptr)
        {
            amdExtD3DFactory = new AmdExtD3DFactory();
            amdExtD3DFactory->linux = State::Instance().isRunningOnLinux;
            amdExtD3DFactory->fsr4Support = primaryGpu.fsr4Support;
            amdExtD3DFactory->realFsr4Support = primaryGpu.realFsr4Support;
            amdExtD3DFactory->fsr4ForcedSupport = primaryGpu.fsr4ForcedSupport;
        }

        *ppvObject = amdExtD3DFactory;

        LOG_INFO("IAmdExtD3DFactory queried, returning custom AmdExtD3DFactory");

        if (o_amdExtD3DFactory == nullptr)
            original(pOuter, riid, (void**) &o_amdExtD3DFactory);

        return S_OK;
    }

    else if (runFsr4Upgrade && riid == __uuidof(IAmdExtFfxApi))
    {
        if (amdExtFfxApi == nullptr)
        {
            amdExtFfxApi = new AmdExtFfxApi();
            amdExtFfxApi->realFsr4Support = primaryGpu.realFsr4Support;
        }

        // Return custom one
        *ppvObject = amdExtFfxApi;

        LOG_INFO("IAmdExtFfxApi queried, returning custom AmdExtFfxApi");

        return S_OK;
    }

#ifdef LOW_LATENCY_INPUTS
    else if (riid == IID_IAmdExtAntiLagApi && giveGameAl2Proxy)
    {
        if (amdExtAntiLagApi == nullptr)
            amdExtAntiLagApi = new AmdExtAntiLagApi();

        // Return custom one
        *ppvObject = amdExtAntiLagApi;

        LOG_INFO("Providing the game with AL2 proxy, AL2 inputs should be avaliable");

        return S_OK;
    }
#else
    else if (riid == IID_IAmdExtAntiLagApi && giveGameAl2Proxy &&
             (StreamlineHooks::isReflexHooked() || State::Instance().activeFgInput == FGInput::NvngxFG ||
              State::Instance().activeFgInput == FGInput::DLSSG))
    {
        *ppvObject = nullptr;

        LOG_INFO("Killing native AL2");

        return S_OK;
    }
#endif

    return original(pOuter, riid, ppvObject);
}
