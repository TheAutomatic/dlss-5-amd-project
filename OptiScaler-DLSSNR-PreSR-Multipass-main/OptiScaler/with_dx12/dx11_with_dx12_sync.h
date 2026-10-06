#pragma once
#include <mutex>
#include <shared_mutex>
#include <dxgi.h>
namespace Dx11wDx12Sync
{
inline std::shared_mutex& PresentResizeMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}
// Only confirmed window dimensions may resolve a zero request. Unknown state must
// take the real ResizeBuffers path (DirectComposition has no implied HWND).
inline bool Equivalent(const DXGI_SWAP_CHAIN_DESC& old, UINT count, UINT width, UINT height,
                       DXGI_FORMAT format, UINT flags, const SIZE* window)
{
    if (!width) { if (!window || window->cx <= 0) return false; width = UINT(window->cx); }
    if (!height) { if (!window || window->cy <= 0) return false; height = UINT(window->cy); }
    return old.BufferDesc.Width == width && old.BufferDesc.Height == height &&
           old.BufferCount == (count ? count : old.BufferCount) &&
           old.BufferDesc.Format == (format == DXGI_FORMAT_UNKNOWN ? old.BufferDesc.Format : format) &&
           old.Flags == flags;
}
inline bool Equivalent(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
                       DXGI_FORMAT format, UINT flags, HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!chain || FAILED(chain->GetDesc(&desc))) return false;
    RECT rect{};
    SIZE size{};
    const bool resolved = window && GetClientRect(window, &rect);
    if (resolved) size = {rect.right - rect.left, rect.bottom - rect.top};
    return Equivalent(desc, count, width, height, format, flags, resolved ? &size : nullptr);
}
// Both DXGI entry points share ordering and failure semantics. In particular a
// failed wait does not release any resources, and a companion failure must not
// overwrite a successful game ResizeBuffers result.
template<class Wait, class Release, class ResizeGame, class ResizeCompanion>
HRESULT ResizeTransaction(Wait wait, Release release, ResizeGame game, ResizeCompanion companion,
                          HRESULT& companionError)
{
    if (!wait()) return DXGI_ERROR_WAS_STILL_DRAWING;
    release();
    const HRESULT result = game();
    if (SUCCEEDED(result)) companionError = companion();
    return result;
}

}
