#pragma once
// TEST19 only. Object-owned metadata survives pointer reuse correctly and never
// controls wrapping, submission, or resource lifetime.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace DlssNr::Submission::CreationDiagnostic
{
inline constexpr GUID kCreationGuid =
    {0xd50d37af,0xc7c4,0x4fa1,{0x94,0xaf,0x4e,0x12,0x8f,0x92,0x3d,0x22}};
enum Gate : uint32_t { Armed=1, ProxyWrap=2, EarlyExeWrap=4, OpenLists=8, Suppressed=16, EarlyUnityPlayerWrap=32 };
struct Stamp
{
    uint32_t version=1, bytes=sizeof(Stamp);
    uint64_t id=0, tick=0, callerOffset=0;
    uint32_t thread=0, type=0, api=0, gates=0, wrapped=0;
    GUID requestedInterface {};
    char callerModule[96] {};
};
inline std::atomic<uint64_t> nextId {0};

inline void Module(void *address, char (&name)[96], uint64_t &offset)
{
    HMODULE module=nullptr;
    std::strcpy(name,"unknown");offset=0;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address),&module)) return;
    offset=reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module);
    wchar_t path[1024] {};
    const DWORD n=GetModuleFileNameW(module,path,1024);
    if (!n || n>=1024) return;
    const wchar_t *base=path;
    for (const wchar_t *p=path;*p;++p) if (*p==L'\\' || *p==L'/') base=p+1;
    if (!WideCharToMultiByte(CP_UTF8,0,base,-1,name,96,nullptr,nullptr)) std::strcpy(name,"module-name-too-long");
}

inline Stamp Begin(void *caller, D3D12_COMMAND_LIST_TYPE type, unsigned api, REFIID riid, uint32_t gates)
{
    Stamp result;
    result.id=nextId.fetch_add(1,std::memory_order_relaxed)+1;
    result.tick=GetTickCount64();result.thread=GetCurrentThreadId();
    result.type=static_cast<uint32_t>(type);result.api=api;result.gates=gates;
    result.requestedInterface=riid;
    Module(caller,result.callerModule,result.callerOffset);
    return result;
}

inline void Attach(void *object, Stamp stamp, bool wrapped)
{
    if (!object) return;
    ID3D12GraphicsCommandList *list=nullptr;
    if (FAILED(static_cast<IUnknown *>(object)->QueryInterface(IID_PPV_ARGS(&list))) || !list) return;
    stamp.wrapped=wrapped?1u:0u;
    // Failure to write diagnostics must never change the Create result.
    list->SetPrivateData(kCreationGuid,sizeof(stamp),&stamp);
    list->Release();
}

inline bool Read(ID3D12GraphicsCommandList *list, Stamp &stamp)
{
    if (!list) return false;
    UINT bytes=sizeof(stamp);
    return SUCCEEDED(list->GetPrivateData(kCreationGuid,&bytes,&stamp)) &&
           bytes==sizeof(stamp) && stamp.version==1 && stamp.bytes==sizeof(stamp) &&
           stamp.callerModule[95]==0;
}
} // namespace DlssNr::Submission::CreationDiagnostic
