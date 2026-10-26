#define NOMINMAX
#include <cassert>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/QueryStateBook.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/EnhancedBarrierBook.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionPolicy.h"
int main() {
    using namespace DlssNr::Submission;
    auto* heap = reinterpret_cast<ID3D12QueryHeap*>(uintptr_t(1));
    QueryStateBook queries;
    assert(queries.Begin(heap, D3D12_QUERY_TYPE_OCCLUSION, 7));
    assert(queries.Begin(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3));
    assert(!queries.Begin(heap, D3D12_QUERY_TYPE_OCCLUSION, 7));
    assert(!queries.CanSplit() && queries.OpenCount() == 2);
    unsigned observed = 0;
    queries.ForEachOpen([&](ID3D12QueryHeap* h, D3D12_QUERY_TYPE type, UINT index) {
        assert(h == heap);
        assert((type == D3D12_QUERY_TYPE_OCCLUSION && index == 7) ||
               (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS && index == 3)); ++observed;
    });
    assert(observed == 2 && !queries.CanSplit()); // Diagnostics do not end or restart queries.
    assert(queries.End(heap, D3D12_QUERY_TYPE_TIMESTAMP, 9) && queries.OpenCount() == 2);
    assert(!queries.End(heap, D3D12_QUERY_TYPE_OCCLUSION, 99));
    assert(queries.End(heap, D3D12_QUERY_TYPE_OCCLUSION, 7) && !queries.CanSplit());
    assert(queries.End(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3) && queries.CanSplit());
    EnhancedBarrierBook layouts;
    auto* texture = reinterpret_cast<ID3D12Resource*>(uintptr_t(2));
    D3D12_TEXTURE_BARRIER b {}; b.pResource = texture;
    b.LayoutAfter = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
    b.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    b.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_BARRIER_GROUP group {}; group.Type = D3D12_BARRIER_TYPE_TEXTURE; group.NumBarriers = 1; group.pTextureBarriers = &b;
    layouts.Observe(1, &group); assert(layouts.Find(texture)->known);
    const auto whole = *layouts.Find(texture);
    b.Subresources.IndexOrFirstMipLevel = 1; layouts.Observe(1, &group);
    assert(!layouts.Find(texture)->known); // A partial transition must invalidate uniform-layout admission.
    b.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.SyncAfter = D3D12_BARRIER_SYNC_SPLIT; layouts.Observe(1, &group);
    assert(!layouts.Find(texture)->known);
    layouts.Restore(texture, whole); assert(layouts.Find(texture)->known);
    b.SyncAfter = D3D12_BARRIER_SYNC_ALL; b.SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
    layouts.Observe(1, &group); assert(!layouts.Find(texture)->known);
    layouts.Restore(texture, whole);
    D3D12_RESOURCE_BARRIER legacy {}; legacy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    legacy.Transition = {texture, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    layouts.OnLegacyBarrier(legacy);
    assert(!layouts.Find(texture)->known); // One legacy subresource is not whole-resource evidence.
    legacy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    layouts.OnLegacyBarrier(legacy);
    assert(layouts.Find(texture)->known && layouts.Find(texture)->legacy);
    auto* other = reinterpret_cast<ID3D12Resource*>(uintptr_t(3));
    auto* unseen = reinterpret_cast<ID3D12Resource*>(uintptr_t(4));
    D3D12_RESOURCE_BARRIER alias {}; alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing = {texture, other}; layouts.OnLegacyBarrier(alias);
    assert(!layouts.Find(texture)->known && !layouts.Find(other)->known && !layouts.Find(unseen));
    alias.Aliasing = {nullptr, nullptr}; layouts.OnLegacyBarrier(alias);
    assert(layouts.Find(unseen) && !layouts.Find(unseen)->known); // Wildcard aliases also cover unseen inputs.
    layouts.OnLegacyBarrier(legacy);
    assert(layouts.Find(texture)->known && layouts.Find(texture)->legacy && !layouts.Find(other)->known);
    layouts.Restore(texture, whole); assert(layouts.Find(texture)->known && !layouts.Find(texture)->legacy);
    layouts.Reset(); assert(!layouts.Find(texture) && !layouts.Find(unseen)); // No state survives Reset/address reuse.
    layouts.Restore(texture, whole);
    layouts.Forget(texture); assert(!layouts.Find(texture));
    SetAllowEnhancedBarriers(true); assert(AllowEnhancedBarriers());
    SetAllowEnhancedBarriers(false); assert(!AllowEnhancedBarriers());
    SetSplitQuerySplice(true); assert(SplitQuerySpliceAllowed());
    SetSplitQuerySplice(false); assert(!SplitQuerySpliceAllowed());
    // Split query splice walks the open set twice (producer End pass, continuation Begin
    // pass); without mutation both passes must enumerate the same pairs.
    QueryStateBook splice;
    assert(splice.Begin(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3));
    assert(splice.Begin(heap, D3D12_QUERY_TYPE_OCCLUSION, 7));
    size_t first = 0, second = 0;
    splice.ForEachOpen([&](ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { ++first; });
    splice.ForEachOpen([&](ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { ++second; });
    assert(first == 2 && second == 2 && !splice.CanSplit());
    assert(splice.End(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3));
    assert(splice.End(heap, D3D12_QUERY_TYPE_OCCLUSION, 7) && splice.CanSplit());
    std::puts("NR admission PASS: query diagnostics preserve scopes; splice policy round-trips; partial/pending enhanced layouts fail closed");
}
