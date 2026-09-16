#pragma once
#include <d3d12.h>

namespace AmdPreSr
{
// This project executes the list before Notify. Keep the original runtime's
// job-publication body, but do
// not execute the game's entire list again for each private runtime/pass.
// RuntimeHostLoad separately prevents the standalone hook bootstrap. Replacing
// this callback alone would not isolate A's Present/FFX hooks or INI reloads.
inline void STDMETHODCALLTYPE AlreadySubmitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) {}
}
