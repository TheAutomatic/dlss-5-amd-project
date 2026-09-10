#pragma once
#include <d3d12.h>

namespace AmdPreSr
{
// B executes the list before Notify. Keep A's job-publication body, but do
// not execute the game's entire list again for each private runtime/pass.
// A non-null 0x8daf8 also prevents A's ECL hook setup (pinned RVA 0x892a).
inline void STDMETHODCALLTYPE AlreadySubmitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) {}
}
