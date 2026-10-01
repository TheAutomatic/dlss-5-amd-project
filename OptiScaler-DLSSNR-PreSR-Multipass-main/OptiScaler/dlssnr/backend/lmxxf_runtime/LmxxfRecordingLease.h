#pragma once
#include <windows.h>
#include <d3d12.h>
#include <memory>
#include <new>
#include <vector>

namespace LmxxfRuntime
{
// Used only for destructors that may synchronize HIP. The callback's code and
// CRT remain loaded until Windows returns from the callback.
template<class T> void DeferredDelete(T* object) noexcept
{
    if (!object) return;
    struct Payload
    {
        T* object;
        HMODULE module;
        static void CALLBACK Run(PTP_CALLBACK_INSTANCE instance, void* context)
        {
            auto* payload = static_cast<Payload*>(context);
            FreeLibraryWhenCallbackReturns(instance, payload->module);
            delete payload->object;
            delete payload;
        }
    };
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&Payload::Run), &module))
    {
        OutputDebugStringA("lmxxf: retaining asynchronous deletion (module pin failed)\n");
        return;
    }
    auto* payload = new(std::nothrow) Payload {object, module};
    if (payload && TrySubmitThreadpoolCallback(&Payload::Run, payload, nullptr)) return;
    delete payload;
    FreeLibrary(module);
    OutputDebugStringA("lmxxf: retaining asynchronous deletion (callback scheduling failed)\n");
}

// A certificate is shared by the Job and its resource chain. References are
// released only after confirmed completion, never merely because a newer fence
// was signaled. Its fence pointer came from a borrowed submission callback.
struct RecordingCompletion
{
    ID3D12Fence* fence;
    ID3D12CommandQueue* queue;
    UINT64 value;
    RecordingCompletion(ID3D12Fence* f, ID3D12CommandQueue* q, UINT64 v)
        : fence(f), queue(q), value(v) { fence->AddRef(); queue->AddRef(); }
    ~RecordingCompletion() { fence->Release(); queue->Release(); }
    RecordingCompletion(const RecordingCompletion&) = delete;
    RecordingCompletion& operator=(const RecordingCompletion&) = delete;
    bool Complete() const
    {
        const UINT64 done = fence->GetCompletedValue();
        return done != UINT64_MAX && done >= value;
    }
};

struct RecordingPins
{
    std::vector<IUnknown*> objects;
    RecordingPins() = default;
    RecordingPins(const RecordingPins&) = delete;
    RecordingPins& operator=(const RecordingPins&) = delete;
    ~RecordingPins() { for (auto* p : objects) p->Release(); }
    void Add(IUnknown* p) { if (p) { objects.push_back(p); p->AddRef(); } }
};
} // namespace LmxxfRuntime
