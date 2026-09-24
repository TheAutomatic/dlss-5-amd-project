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
// Kind of the host constructed for this process, or ActiveKindFromConfig() when none exists yet.
DlssNr::Backend::Kind LiveBackendKind();
// True when a host already exists and Config's NrBackend selects a different kind.
// The host is process-lifetime (Daniel HIP threads); switching needs a game restart.
bool BackendRestartNeeded();
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
