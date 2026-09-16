#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <string>
namespace DlssNr::AmdBridge
{
bool HasFiles();
bool Before(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*);
void Restore(NVSDK_NGX_Parameter*);
bool HasReplacement(NVSDK_NGX_Parameter*);
void InvalidateHistory();
void TraceContextRelease(unsigned int handle, bool after);
std::string Status();
// pass1 SHA name ("0.3.0" / "0.3.1" / …) or nullptr if missing/unknown.
// Cached for menu display until the DLL path, size, or write time changes.
const char* RuntimeName();
} // namespace DlssNr::AmdBridge
