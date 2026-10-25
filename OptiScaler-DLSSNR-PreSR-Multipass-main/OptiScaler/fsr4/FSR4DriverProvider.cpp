#include "pch.h"
#include "FSR4DriverProvider.h"

#include "FSR4Models.h"
#include "FSR4Presets.h"

#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>
#include <misc/IdentifyGpu.h>

#include <detours/detours.h>
#include <ffx_upscale.h>

namespace
{
struct PatchedDll
{
    HMODULE module;
    decltype(&GetModuleHandleW) o_GetModuleHandleW;
};

std::vector<PatchedDll> patchedDlls;
std::mutex patchedDllsMutex;

// Driver dll that gets itself for every FFX SDK dll it looks up while it sets its SDK version
std::atomic<HMODULE> primingDll = nullptr;

std::vector<FSR4DriverDll*>& DriverDlls()
{
    static std::vector<FSR4DriverDll*> dlls;
    return dlls;
}

HMODULE LoadDriverDll(const wchar_t* name, const GpuInformation& gpu)
{
    auto module = KernelBaseProxy::GetModuleHandleW_()(name);

    if (module != nullptr)
    {
        LOG_INFO(L"{} already in memory", name);
        return module;
    }

    // A copy next to Opti overrides the driver's. Only explicit paths are tried, a search by name can find an old
    // copy that a previous driver left in System32
    auto optiPath = std::filesystem::path(Config::Instance()->MainDllPath.value()) / name;

    if (std::filesystem::exists(optiPath))
    {
        module = NtdllProxy::LoadLibraryExW_Ldr(optiPath.c_str(), NULL, 0);

        if (module != nullptr)
        {
            LOG_INFO(L"{} loaded from {}", name, optiPath.wstring());
            return module;
        }
    }

    if (!gpu.driverStore.empty())
    {
        auto dllPath = gpu.driverStore / name;
        LOG_INFO(L"Trying {} for adapter {:08X}:{:08X} ({}) from {}", name,
                 static_cast<uint32_t>(gpu.luid.HighPart), gpu.luid.LowPart,
                 string_to_wstring(gpu.name), dllPath.wstring());
        module = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);
        if (module != nullptr)
        {
            LOG_INFO(L"{} loaded from {}", name, dllPath.wstring());
            return module;
        }
    }

    LOG_WARN(L"Failed to load {}", name);
    return nullptr;
}

// Only the driver dlls call it, through their patched imports
HMODULE WINAPI hkGetModuleHandleW(LPCWSTR lpModuleName)
{
    auto caller = Util::GetCallerModule(_ReturnAddress());
    HMODULE module = nullptr;

    if (caller != nullptr && caller == primingDll.load() && lpModuleName != nullptr &&
        _wcsnicmp(lpModuleName, L"amd_fidelityfx_", 15) == 0)
    {
        return caller;
    }
    {
        std::scoped_lock lock(patchedDllsMutex);

        auto patched = std::ranges::find(patchedDlls, caller, &PatchedDll::module);
        module = patched != patchedDlls.end() ? patched->o_GetModuleHandleW(lpModuleName)
                                              : KernelBaseProxy::GetModuleHandleW_()(lpModuleName);
    }

    if (module == nullptr)
        module = FSR4DriverDll::SdkModuleStandIn(lpModuleName, caller);

    return module;
}

BOOL CALLBACK PatchGetModuleHandleW(PVOID pContext, DWORD nOrdinal, LPCSTR pszFunc, PVOID* ppvFunc)
{
    if (pszFunc == nullptr || ppvFunc == nullptr || strcmp(pszFunc, "GetModuleHandleW") != 0)
        return TRUE;

    DWORD oldProtect = 0;
    if (!VirtualProtect(ppvFunc, sizeof(PVOID), PAGE_READWRITE, &oldProtect))
        return FALSE;

    static_cast<PatchedDll*>(pContext)->o_GetModuleHandleW = reinterpret_cast<decltype(&GetModuleHandleW)>(*ppvFunc);
    *ppvFunc = reinterpret_cast<PVOID>(&hkGetModuleHandleW);

    VirtualProtect(ppvFunc, sizeof(PVOID), oldProtect, &oldProtect);

    return FALSE;
}

} // namespace

FSR4DriverDll::FSR4DriverDll(const wchar_t* name, std::vector<std::wstring> sdkModules)
    : _name(name), _sdkModules(std::move(sdkModules))
{
    DriverDlls().push_back(this);
}

HMODULE FSR4DriverDll::Load(std::optional<LUID> actualLuid)
{
    if (_module != nullptr)
        return _module;

    auto gpu = actualLuid.has_value() ? GpuInformation {} : IdentifyGpu::getActiveGpu();
    if (actualLuid.has_value())
    {
        for (const auto& candidate : IdentifyGpu::getAllGpus())
            if (IsEqualLUID(candidate.luid, *actualLuid))
            {
                gpu = candidate;
                break;
            }
        // Recursive or failed DXGI discovery is retryable, unlike an actual DLL load attempt.
        if (gpu.driverStore.empty())
            return nullptr;
    }

    std::call_once(_loadOnce,
                   [this, &gpu]
                   {
                       auto module = LoadDriverDll(_name.c_str(), gpu);
                       if (module == nullptr)
                           return;

                       _updateFfxApiProvider = reinterpret_cast<PFN_UpdateFfxApiProvider>(
                           KernelBaseProxy::GetProcAddress_()(module, "UpdateFfxApiProvider"));
                       _updateFfxApiProviderEx = reinterpret_cast<PFN_UpdateFfxApiProviderEx>(
                           KernelBaseProxy::GetProcAddress_()(module, "UpdateFfxApiProviderEx"));

                       if (_updateFfxApiProvider == nullptr)
                       {
                           LOG_ERROR(L"Failed to get UpdateFfxApiProvider of {}", _name);
                           return;
                       }

                       if (!_sdkModules.empty())
                       {
                           PatchedDll patched { module, nullptr };
                           DetourEnumerateImportsEx(module, &patched, nullptr, PatchGetModuleHandleW);

                           if (patched.o_GetModuleHandleW != nullptr)
                           {
                               {
                                   std::scoped_lock lock(patchedDllsMutex);
                                   patchedDlls.push_back(patched);
                               }

                               // The driver keeps the SDK version of the first FFX dll it checks for the rest of
                               // the process. An old one, like the legacy amd_fidelityfx_dx12.dll that struct
                               // version 1 requests check, rules out frame generation and its swapchain. This
                               // request runs before any other and has the driver dll check itself, its file
                               // version counts as the newest SDK
                               ExternalProviderData data {};
                               data.structVersion = 2;
                               data.descType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;

                               // The DX12 and Vulkan providers own separate FSR4DriverDll objects whose
                               // first loads can overlap; serialize the whole set/call/clear transaction so
                               // one load can't clear or steal the other's marker (hkGetModuleHandleW would
                               // then miss the priming branch and an old game SDK could win the sticky
                               // SDK-version decision)
                               static std::mutex primingMutex;
                               std::scoped_lock primingLock(primingMutex);

                               primingDll = module;
                               auto result = UpdateFfxApiProvider(&data, ExternalProviderData::SizeFor(2));
                               primingDll = nullptr;

                               LOG_DEBUG(L"Set the SDK version of {}, result: 0x{:X}", _name, (UINT) result);
                           }
                           else
                           {
                               LOG_WARN(L"Couldn't patch GetModuleHandleW of {}", _name);
                           }
                       }

                       // Auto still needs DetectedModel support checks and preset correction. Only an explicit
                       // opt-out skips these code/vtable patches for anti-cheat-sensitive games.
                       if (!Config::Instance()->Fsr4DisableDriverHooks.value_or_default())
                       {
                           FSR4Presets::Hook(module);
                           FSR4Models::Hook(module);
                       }
                       else
                       {
                           LOG_WARN("FSR4 driver hooks disabled: automatic model selection and preset correction "
                                    "are disabled; model/preset overrides will not be applied");
                       }

                       _module = module;
                   });

    return _module;
}

std::optional<DriverProvider> FSR4DriverDll::QueryProvider(uint64_t descType,
                                                           std::initializer_list<uint32_t> structVersions,
                                                           ProviderOption* options) const
{
    if (_module == nullptr)
        return std::nullopt;

    for (auto structVersion : structVersions)
    {
        ExternalProviderData data {};
        data.structVersion = structVersion;
        data.descType = descType;

        auto result = UpdateFfxApiProvider(&data, ExternalProviderData::SizeFor(structVersion), options);

        if (SUCCEEDED(result) && data.provider.createContext != nullptr)
        {
            DriverProvider provider { data.provider, data.provider.versionName ? data.provider.versionName : "",
                                      structVersion >= 3 ? data.isSupported : nullptr };
            provider.functions.versionName = nullptr;

            LOG_INFO(L"{} provider for {:X}: {}, id: {:X}, struct version: {}", _name, descType,
                     string_to_wstring(provider.versionName), provider.functions.versionId, structVersion);

            return provider;
        }

        LOG_DEBUG(L"{} UpdateFfxApiProvider for {:X} with struct version {}, result: 0x{:X}", _name, descType,
                  structVersion, (UINT) result);
    }

    return std::nullopt;
}

HRESULT FSR4DriverDll::UpdateFfxApiProvider(void* data, uint32_t dataSizeInBytes, ProviderOption* options) const
{
    if (_updateFfxApiProvider == nullptr)
        return E_NOINTERFACE;

    auto owner = State::GetOwner();
    State::DisableChecks(owner);

    auto result = options != nullptr && _updateFfxApiProviderEx != nullptr
                      ? _updateFfxApiProviderEx(data, dataSizeInBytes, options)
                      : _updateFfxApiProvider(data, dataSizeInBytes);

    State::EnableChecks(owner);

    return result;
}

HMODULE FSR4DriverDll::SdkModuleStandIn(const wchar_t* moduleName, HMODULE caller)
{
    if (moduleName == nullptr || caller == nullptr)
        return nullptr;

    for (auto dll : DriverDlls())
    {
        if (dll->_module != caller)
            continue;

        for (const auto& sdkModule : dll->_sdkModules)
        {
            if (_wcsicmp(moduleName, sdkModule.c_str()) == 0)
            {
                LOG_DEBUG(L"Giving {} its own HMODULE for {}", dll->_name, moduleName);
                return dll->_module;
            }
        }
    }

    return nullptr;
}
