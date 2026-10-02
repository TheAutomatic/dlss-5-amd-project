#pragma once
#include <dlssnr/submission/CommandListProxy.h>

namespace RootRestoreIdentity
{
inline ID3D12GraphicsCommandList* Key(ID3D12GraphicsCommandList* list)
{
    return DlssNr::Submission::GraphicsRecordingList(list);
}

// The native hooks own the keys. A split changes the recording destination, but
// the state to restore still belongs to the producer from before the NR pass.
// Caller holds the map's lock. Copy before insertion (which may rehash).
template <class Map>
void Transfer(Map& states, ID3D12GraphicsCommandList* source, ID3D12GraphicsCommandList* destination)
{
    if (source == destination)
        return;
    const auto it = states.find(source);
    if (it == states.end())
        states.erase(destination);
    else
    {
        auto state = it->second;
        states.insert_or_assign(destination, std::move(state));
    }
}

// Replay through the proxy so its continuation seed also observes the restored
// bindings. Only a native object may be passed to a native Detours trampoline.
// Root signature tracking must be suppressed by the caller during replay.
template <class Hook, class Method, class... Args>
void Replay(ID3D12GraphicsCommandList* list, Hook hook, Method method, Args... args)
{
    if (Key(list) != list)
        (list->*method)(args...);
    else
        hook(list, args...);
}
}
