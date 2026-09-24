#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <string>
#include "../backend/Kind.h"
namespace DlssNr::AmdBridge
{
bool HasFiles();
bool HasDanielRuntime();
bool HasLmxxfRuntime();
// Active NR host kind (daniel or lmxxf). Call after changing Config::NrBackend.
DlssNr::Backend::Kind LiveBackendKind();
// Hot-switch: flip ProxyWrap, clear history, force warm-up. Both hosts stay alive.
void SyncBackendWithConfig();
// Install submission expansion before the first wrapped list is exposed. No runtime/HIP initialization.
bool EnsureSubmissionHook(ID3D12CommandQueue*);
bool Before(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*);
void Restore(NVSDK_NGX_Parameter*);
bool HasReplacement(NVSDK_NGX_Parameter*);
void InvalidateHistory();
void TraceContextRelease(unsigned int handle, bool after);
std::string Status();
bool GraphicsRestartNeeded(UINT activePasses);
// pass1 SHA name ("0.3.0" / "0.3.1" / …) or nullptr if missing/unknown.
// Cached for menu display until the DLL path, size, or write time changes.
const char* RuntimeName();
void UpdateConfirmedRenderQueue(ID3D12CommandQueue *q);
} // namespace DlssNr::AmdBridge
