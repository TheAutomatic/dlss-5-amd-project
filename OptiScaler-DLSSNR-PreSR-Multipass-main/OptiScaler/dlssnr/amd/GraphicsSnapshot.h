#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <tuple>

// Pure CPU bookkeeping for AMD graphics-wait admission and restore.
// No D3D12 calls; production code maps real API types onto these PODs.
namespace AmdPreSr::GraphicsSnap
{

inline constexpr std::uint32_t kMaxRootParams = 64;
inline constexpr std::uint32_t kMaxRootConstants = 64;
inline constexpr std::uint32_t kMaxViewports = 16;
inline constexpr std::uint32_t kMaxScissors = 16;
inline constexpr std::uint32_t kMaxRTVs = 8;
inline constexpr std::uint32_t kMaxHeaps = 2;

enum class BindState : std::uint8_t
{
    Unknown,
    KnownUnset,
    KnownValue,
};

enum class RootEntryType : std::uint8_t
{
    Invalid,
    Table,
    Constant,
    Constants,
    CBV,
    SRV,
    UAV,
};

// Why this generation is not admissible for new wait (for logs).
enum class IneligibleWhy : std::uint8_t
{
    None,
    Bundle,
    Indirect,
    Query,
    Other,
};

struct RootEntry
{
    RootEntryType type = RootEntryType::Invalid;
    BindState state = BindState::Unknown;
    std::uint64_t gpuVa = 0;
    std::uint64_t table = 0;
    std::uint32_t constants[kMaxRootConstants] {};
    std::uint32_t numConstants = 0;
    std::uint32_t destOffset = 0;
    // Only observed DWORDs are replayed. Gaps in partial writes are not zeroes.
    std::uint64_t knownConstants = 0;
};

struct Viewport
{
    float topLeftX = 0, topLeftY = 0, width = 0, height = 0, minDepth = 0, maxDepth = 0;
};

struct ScissorRect
{
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

struct OmBinding
{
    BindState state = BindState::Unknown;
    std::uint32_t numRTVs = 0;
    bool singleHandleRange = false;
    std::uint64_t rtvHandles[kMaxRTVs] {};
    bool hasDsv = false;
    std::uint64_t dsvHandle = 0;
    // Copied CPU descriptors must remain alive as long as any live/frozen
    // snapshot references them, independently of other command lists.
    std::shared_ptr<void> owner;
};

struct Predication
{
    BindState state = BindState::Unknown;
    std::uint64_t resource = 0;
    std::uint64_t byteOffset = 0;
    std::uint32_t operation = 0;

    bool IsDisabled() const
    {
        if (state == BindState::KnownUnset)
            return true;
        return state == BindState::KnownValue && resource == 0;
    }
};

struct RootDomain
{
    BindState signatureState = BindState::Unknown;
    std::uint64_t signature = 0;
    RootEntry entries[kMaxRootParams] {};

    void ClearParams()
    {
        for (auto& e : entries)
            e = RootEntry {};
    }

    void ClearTables()
    {
        for (auto& e : entries)
            if (e.type == RootEntryType::Table)
                e = RootEntry {};
    }

    // Rebinding the same known signature preserves arguments (D3D12 rule).
    // Switching signatures, including to null, invalidates them.
    void SetSignature(std::uint64_t sig)
    {
        if (signatureState != BindState::Unknown && signature == sig)
            return;
        signature = sig;
        signatureState = sig ? BindState::KnownValue : BindState::KnownUnset;
        ClearParams();
    }

    void SetTable(std::uint32_t index, std::uint64_t handle)
    {
        if (index >= kMaxRootParams)
            return;
        auto& e = entries[index];
        e = RootEntry {};
        e.type = RootEntryType::Table;
        e.state = BindState::KnownValue;
        e.table = handle;
    }

    void SetGpuVa(std::uint32_t index, RootEntryType type, std::uint64_t va)
    {
        if (index >= kMaxRootParams)
            return;
        auto& e = entries[index];
        e = RootEntry {};
        e.type = type;
        e.state = BindState::KnownValue;
        e.gpuVa = va;
    }

    void SetConstant(std::uint32_t index, std::uint32_t value, std::uint32_t destOffset)
    {
        MergeConstants(index, &value, 1, destOffset);
    }

    // Partial writes must merge by DWORD offset, not replace the whole slot.
    bool MergeConstants(std::uint32_t index, const std::uint32_t* src, std::uint32_t count,
                        std::uint32_t destOffset)
    {
        if (index >= kMaxRootParams || !src || count == 0)
            return false;
        if (destOffset >= kMaxRootConstants || count > kMaxRootConstants - destOffset)
            return false;
        auto& e = entries[index];
        if (e.state != BindState::KnownValue || e.type != RootEntryType::Constants)
        {
            e = RootEntry {};
            e.type = RootEntryType::Constants;
            e.state = BindState::KnownValue;
        }
        std::memcpy(e.constants + destOffset, src, count * sizeof(std::uint32_t));
        for (std::uint32_t i = destOffset; i < destOffset + count; ++i)
            e.knownConstants |= std::uint64_t { 1 } << i;
        const auto end = destOffset + count;
        if (end > e.numConstants)
            e.numConstants = end;
        if (e.destOffset == 0 && destOffset != 0 && e.numConstants == end)
            e.destOffset = destOffset;
        return true;
    }
};

struct GraphicsSnapshot
{
    RootDomain compute;
    RootDomain graphics;
    BindState psoState = BindState::Unknown;
    std::uint64_t pso = 0;
    BindState viewportState = BindState::Unknown;
    std::uint32_t viewportCount = 0;
    Viewport viewports[kMaxViewports] {};
    BindState scissorState = BindState::Unknown;
    std::uint32_t scissorCount = 0;
    ScissorRect scissors[kMaxScissors] {};
    BindState topologyState = BindState::Unknown;
    std::uint32_t topology = 0;
    OmBinding om;
    Predication predication;
    // Shader-visible CBV/SRV/UAV + sampler heaps (D3D12 allows at most two).
    BindState heapState = BindState::Unknown;
    std::uint32_t heapCount = 0;
    std::uint64_t heaps[kMaxHeaps] {};
    bool renderPassActive = false;
    bool renderPassSuspended = false;
    bool queryActive = false;
    bool bundleOrIndirectSeen = false;
    IneligibleWhy ineligibleWhy = IneligibleWhy::None;

    void SetHeaps(std::uint32_t count, const std::uint64_t* handles)
    {
        // Heap order is immaterial: at most one heap of each shader-visible
        // type can be bound. Rebinding the same set preserves root tables.
        bool valid = count <= kMaxHeaps && (count == 0 || handles);
        for (std::uint32_t i = 0; valid && i < count; ++i)
            valid = handles[i] != 0;
        if (valid && count == 2 && handles[0] == handles[1])
            valid = false;
        const auto newState = !valid ? BindState::Unknown :
            (count ? BindState::KnownValue : BindState::KnownUnset);
        bool same = valid && heapState == newState && heapCount == count;
        if (same && count == 1)
            same = heaps[0] == handles[0];
        else if (same && count == 2)
            same = (heaps[0] == handles[0] && heaps[1] == handles[1]) ||
                   (heaps[0] == handles[1] && heaps[1] == handles[0]);
        if (!same)
        {
            compute.ClearTables();
            graphics.ClearTables();
        }
        heapState = newState;
        heapCount = valid ? count : 0;
        std::uint64_t nextHeaps[kMaxHeaps] {};
        for (std::uint32_t i = 0; i < heapCount; ++i)
            nextHeaps[i] = handles[i];
        for (std::uint32_t i = 0; i < kMaxHeaps; ++i)
            heaps[i] = nextHeaps[i];
    }

    void SetPso(std::uint64_t handle)
    {
        pso = handle;
        psoState = handle ? BindState::KnownValue : BindState::KnownUnset;
    }

    void SetViewports(const Viewport* v, std::uint32_t count)
    {
        if (!v || count == 0 || count > kMaxViewports)
        {
            viewportState = BindState::KnownUnset;
            viewportCount = 0;
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i)
            viewports[i] = v[i];
        viewportCount = count;
        viewportState = BindState::KnownValue;
    }

    void SetScissors(const ScissorRect* r, std::uint32_t count)
    {
        if (!r || count == 0 || count > kMaxScissors)
        {
            scissorState = BindState::KnownUnset;
            scissorCount = 0;
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i)
            scissors[i] = r[i];
        scissorCount = count;
        scissorState = BindState::KnownValue;
    }

    void SetTopology(std::uint32_t value)
    {
        topology = value;
        topologyState = BindState::KnownValue;
    }

    // The hook expands/copies every descriptor before reporting. A contiguous
    // input cannot be reconstructed here without a device descriptor increment.
    void SetRenderTargets(std::uint32_t numRTVs, const std::uint64_t* rtvHandles, bool singleHandleRange,
                          bool hasDsv, std::uint64_t dsvHandle, std::shared_ptr<void> owner = {})
    {
        om = OmBinding {};
        if (numRTVs == 0 && !hasDsv)
        {
            om.state = BindState::KnownUnset;
            return;
        }
        if (numRTVs > kMaxRTVs || (numRTVs && !rtvHandles) ||
            (singleHandleRange && numRTVs > 1) || (hasDsv && !dsvHandle))
        {
            om.state = BindState::Unknown;
            return;
        }
        om.numRTVs = numRTVs;
        om.singleHandleRange = false;
        om.hasDsv = hasDsv;
        om.dsvHandle = dsvHandle;
        if (rtvHandles && numRTVs)
        {
            for (std::uint32_t i = 0; i < numRTVs; ++i)
            {
                if (!rtvHandles[i])
                {
                    om = OmBinding {};
                    return;
                }
                om.rtvHandles[i] = rtvHandles[i];
            }
        }
        om.owner = std::move(owner);
        om.state = BindState::KnownValue;
    }

    void SetPredication(std::uint64_t resource, std::uint64_t byteOffset, std::uint32_t operation)
    {
        predication.resource = resource;
        predication.byteOffset = byteOffset;
        predication.operation = operation;
        predication.state = resource ? BindState::KnownValue : BindState::KnownUnset;
    }
};

// One command-list identity across Create / Reset / ClearState / Release.
struct ListTracker
{
    std::uint64_t listId = 0;
    std::uint32_t generation = 0;
    bool generationKnown = false;
    bool live = false;
    bool ineligible = false;
    GraphicsSnapshot snap;
    using QueryKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
    std::set<QueryKey> activeQueries;
    bool renderPassWillSuspend = false;

    // Direct command-list Create/Reset/ClearState start with no root
    // signatures, shader-visible heaps, OM bindings or predication.
    // These are known API defaults, not "never observed".
    void AdoptApiDefaults(std::uint64_t initialPso = 0)
    {
        snap = GraphicsSnapshot {};
        snap.compute.SetSignature(0);
        snap.graphics.SetSignature(0);
        snap.SetHeaps(0, nullptr);
        snap.predication.state = BindState::KnownUnset;
        snap.predication.resource = 0;
        snap.om.state = BindState::KnownUnset;
        snap.SetPso(initialPso);
        // Empty viewport/scissor/undefined topology are API defaults, not Unknown.
        snap.viewportState = BindState::KnownUnset;
        snap.viewportCount = 0;
        snap.scissorState = BindState::KnownUnset;
        snap.scissorCount = 0;
        snap.topologyState = BindState::KnownUnset;
        snap.topology = 0;
    }

    void OnCreate(std::uint64_t id, std::uint64_t initialPso = 0)
    {
        listId = id;
        generation = 1;
        generationKnown = true;
        live = true;
        ineligible = false;
        activeQueries.clear();
        renderPassWillSuspend = false;
        AdoptApiDefaults(initialPso);
    }

    // Only a successful Reset starts a new generation and adopts API defaults.
    bool OnReset(bool succeeded, std::uint64_t initialPso = 0)
    {
        if (!live || !succeeded)
            return false;
        ++generation;
        generationKnown = true;
        ineligible = false;
        activeQueries.clear();
        renderPassWillSuspend = false;
        AdoptApiDefaults(initialPso);
        return true;
    }

    // ClearState is not Reset: bindings become default, but active RP/query stay.
    void OnClearState(std::uint64_t initialPso = 0)
    {
        if (!live)
            return;
        const bool rp = snap.renderPassActive;
        const bool suspended = snap.renderPassSuspended;
        const bool q = snap.queryActive;
        const bool bad = snap.bundleOrIndirectSeen;
        const auto why = snap.ineligibleWhy;
        AdoptApiDefaults(initialPso);
        snap.renderPassActive = rp;
        snap.renderPassSuspended = suspended;
        snap.queryActive = q;
        snap.bundleOrIndirectSeen = bad;
        snap.ineligibleWhy = why;
    }

    void MarkIneligible(IneligibleWhy why = IneligibleWhy::Other)
    {
        if (live)
        {
            ineligible = true;
            snap.ineligibleWhy = why;
            if (why == IneligibleWhy::Bundle || why == IneligibleWhy::Indirect)
                snap.bundleOrIndirectSeen = true;
            if (why == IneligibleWhy::Query)
                snap.queryActive = true;
        }
    }

    void OnRelease()
    {
        live = false;
        listId = 0;
        generationKnown = false;
        ineligible = false;
        snap = GraphicsSnapshot {};
        activeQueries.clear();
        renderPassWillSuspend = false;
    }
};

struct AdmissionResult
{
    bool ok = false;
    const char* reason = "ok";
};

// Pure predicate over a frozen candidate. Caller must have generationKnown.
inline AdmissionResult CanAdmitGraphics(const GraphicsSnapshot& s, bool generationKnown, bool ineligible)
{
    if (!generationKnown)
        return { false, "unknown_generation" };
    if (ineligible || s.bundleOrIndirectSeen || s.renderPassActive || s.renderPassSuspended || s.queryActive)
    {
        if (s.queryActive || s.ineligibleWhy == IneligibleWhy::Query)
            return { false, "query_active" };
        if (s.ineligibleWhy == IneligibleWhy::Bundle)
            return { false, "execute_bundle" };
        if (s.ineligibleWhy == IneligibleWhy::Indirect)
            return { false, "execute_indirect" };
        if (s.renderPassSuspended)
            return { false, "render_pass_suspended" };
        if (s.renderPassActive)
            return { false, "render_pass" };
        return { false, "ineligible_generation" };
    }
    if (s.graphics.signatureState == BindState::Unknown)
        return { false, "graphics_root_unknown" };
    if (s.compute.signatureState == BindState::Unknown)
        return { false, "compute_root_unknown" };
    if (s.heapState == BindState::Unknown)
        return { false, "descriptor_heaps_unknown" };
    if (s.psoState != BindState::KnownValue)
        return { false, "pso_unknown_or_unset" };
    if (s.viewportState == BindState::Unknown)
        return { false, "viewport_unknown" };
    // Known-unset (API empty after Reset) is recoverable: restore RSSetViewports(0).
    if (s.scissorState == BindState::Unknown)
        return { false, "scissor_unknown" };
    if (s.topologyState == BindState::Unknown)
        return { false, "topology_unknown" };
    if (s.om.state != BindState::KnownValue && s.om.state != BindState::KnownUnset)
        return { false, "om_unknown" };
    if (!s.predication.IsDisabled())
        return { false, "predication_active_or_unknown" };
    return { true, "ok" };
}

// All admission gates for logs (not just the first failure).
inline void DescribeAdmissionGates(const GraphicsSnapshot& s, bool generationKnown, bool ineligible, char* buf,
                                   std::size_t bufSize)
{
    if (!buf || !bufSize)
        return;
    auto st = [](BindState v) {
        return v == BindState::KnownValue ? "V" : (v == BindState::KnownUnset ? "U" : "?");
    };
    std::snprintf(buf, bufSize,
                  "gen=%d inelig=%d gRoot=%s cRoot=%s heap=%s pso=%s vp=%s sc=%s topo=%s om=%s pred=%s rp=%d susp=%d q=%d why=%d",
                  generationKnown ? 1 : 0, ineligible ? 1 : 0, st(s.graphics.signatureState),
                  st(s.compute.signatureState), st(s.heapState), st(s.psoState), st(s.viewportState),
                  st(s.scissorState), st(s.topologyState), st(s.om.state),
                  s.predication.IsDisabled() ? "off" : "on", s.renderPassActive ? 1 : 0,
                  s.renderPassSuspended ? 1 : 0, s.queryActive ? 1 : 0, static_cast<int>(s.ineligibleWhy));
}

inline AdmissionResult CanAdmitGraphics(const ListTracker& t)
{
    return CanAdmitGraphics(t.snap, t.generationKnown, t.ineligible);
}

// Snapshot copy used by Freeze; restore replays this after A.Record.
inline bool TryFreeze(const ListTracker& t, GraphicsSnapshot& out)
{
    const auto admission = CanAdmitGraphics(t);
    if (!admission.ok)
        return false;
    out = t.snap;
    return true;
}

} // namespace AmdPreSr::GraphicsSnap
