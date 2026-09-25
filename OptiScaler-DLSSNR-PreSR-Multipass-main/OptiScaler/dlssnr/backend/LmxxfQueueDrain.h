#pragma once

#include <d3d12.h>
#include <windows.h>

namespace DlssNr::Backend
{
// A successful return proves all work submitted before the signal has completed.
inline bool DrainQueue(ID3D12Device *device, ID3D12CommandQueue *queue, DWORD timeoutMs = 5000)
{
    if (!device || !queue)
        return false;
    ID3D12Fence *fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence)
        return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
    {
        fence->Release();
        return false;
    }
    constexpr UINT64 value = 1;
    const bool signaled = SUCCEEDED(queue->Signal(fence, value));
    const bool armed = signaled && SUCCEEDED(fence->SetEventOnCompletion(value, event));
    const bool completed = armed && WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0 &&
                           fence->GetCompletedValue() >= value && SUCCEEDED(device->GetDeviceRemovedReason());
    // A timed-out registration may still signal later. Keep both objects alive
    // when completion is unknown; callers abandon the affected session.
    if (!signaled || completed)
    {
        CloseHandle(event);
        fence->Release();
    }
    return completed;
}
} // namespace DlssNr::Backend
