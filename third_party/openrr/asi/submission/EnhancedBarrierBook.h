#pragma once
#include <d3d12.h>
#include <unordered_map>

namespace DlssNr::Submission
{
// Only uniform, completed whole-resource transitions are admitted for NR interop.
// Partial ranges and pending split barriers remain unsupported, rather than guessing layouts.
class EnhancedBarrierBook
{
public:
    struct State {
        bool texture = false, known = false;
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
        bool legacy = false;
        D3D12_BARRIER_SUBRESOURCE_RANGE range {};
        UINT64 offset = 0, size = 0;
        bool invalidated = false;
    };
private:
    std::unordered_map<ID3D12Resource*, State> states;
    std::unordered_map<DXGI_FORMAT, UINT8> planeCounts;
    bool unknownAfterAliasing = false;
    bool WholeTexture(const D3D12_TEXTURE_BARRIER& barrier) {
        const auto& range = barrier.Subresources;
        if (range.NumMipLevels == 0 && range.IndexOrFirstMipLevel == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) return true;
        if (range.IndexOrFirstMipLevel != 0) return false;
        const auto desc = barrier.pResource->GetDesc();
        const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
        if (range.NumMipLevels == 0) {
            if (desc.MipLevels != 1 || arrays != 1) return false;
        } else if (range.NumMipLevels != desc.MipLevels || range.FirstArraySlice != 0 ||
                   range.NumArraySlices != arrays || range.FirstPlane != 0) return false;
        auto known = planeCounts.find(desc.Format);
        if (known == planeCounts.end()) {
            ID3D12Device* device = nullptr; D3D12_FEATURE_DATA_FORMAT_INFO info {desc.Format, 0};
            if (FAILED(barrier.pResource->GetDevice(IID_PPV_ARGS(&device)))) return false;
            const auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info);
            device->Release();
            if (FAILED(hr) || !info.PlaneCount) return false;
            known = planeCounts.emplace(desc.Format, info.PlaneCount).first;
        }
        return range.NumMipLevels == 0 ? known->second == 1 : range.NumPlanes == known->second;
    }
public:
    void Reset() { states.clear(); planeCounts.clear(); unknownAfterAliasing = false; }
    void Forget(ID3D12Resource* resource) { states.erase(resource); }
    const State* Find(ID3D12Resource* resource) const {
        auto it = states.find(resource);
        if (it != states.end()) return &it->second;
        static const State unknown = [] { State state; state.invalidated = true; return state; }();
        return unknownAfterAliasing ? &unknown : nullptr;
    }
    void Restore(ID3D12Resource* resource, State state) { states[resource] = state; }
    // A lost observation is not evidence of legacy state. Preserve a tombstone
    // until a complete transition supplies a new contract on this recording.
    void Invalidate(ID3D12Resource* resource) {
        auto& state = states[resource]; state.known = state.legacy = false; state.invalidated = true;
    }
    void OnLegacyBarrier(const D3D12_RESOURCE_BARRIER& barrier) {
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (!barrier.Aliasing.pResourceBefore || !barrier.Aliasing.pResourceAfter) {
                unknownAfterAliasing = true;
                for (auto& entry : states) {
                    entry.second.known = entry.second.legacy = false; entry.second.invalidated = true;
                }
            } else {
                Invalidate(barrier.Aliasing.pResourceBefore);
                Invalidate(barrier.Aliasing.pResourceAfter);
            }
        } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barrier.Transition.pResource) {
            const auto& b = barrier.Transition;
            if (!Find(b.pResource)) return;
            auto& state = states[b.pResource];
            state.range = {}; state.range.IndexOrFirstMipLevel = b.Subresource;
            state.legacy = true;
            state.known = b.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                          !(barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
            if (state.known) state.invalidated = false;
        }
    }
    void Observe(UINT n, const D3D12_BARRIER_GROUP* groups) {
        for (UINT g = 0; groups && g < n; ++g) {
            const auto& group = groups[g];
            if (group.Type == D3D12_BARRIER_TYPE_TEXTURE) {
                for (UINT i = 0; i < group.NumBarriers; ++i) {
                    const auto& b = group.pTextureBarriers[i];
                    if (!b.pResource) continue;
                    const bool whole = WholeTexture(b);
                    states[b.pResource] = {true, whole && b.SyncBefore != D3D12_BARRIER_SYNC_SPLIT &&
                                          b.SyncAfter != D3D12_BARRIER_SYNC_SPLIT,
                                          b.LayoutAfter, b.AccessAfter, false, b.Subresources};
                }
            } else if (group.Type == D3D12_BARRIER_TYPE_BUFFER) {
                for (UINT i = 0; i < group.NumBarriers; ++i) {
                    const auto& b = group.pBufferBarriers[i];
                    if (!b.pResource) continue;
                    const bool whole = b.Offset == 0 && (b.Size == UINT64_MAX || b.Size >= b.pResource->GetDesc().Width);
                    states[b.pResource] = {false, whole && b.SyncBefore != D3D12_BARRIER_SYNC_SPLIT &&
                                          b.SyncAfter != D3D12_BARRIER_SYNC_SPLIT,
                                          D3D12_BARRIER_LAYOUT_UNDEFINED, b.AccessAfter, false, {}, b.Offset, b.Size};
                }
            }
        }
    }
};
}
