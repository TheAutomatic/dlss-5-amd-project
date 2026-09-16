#pragma once
#include <cstdint>
#include <cstring>

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

struct RootEntry
{
    RootEntryType type = RootEntryType::Invalid;
    BindState state = BindState::Unknown;
    std::uint64_t gpuVa = 0;
    std::uint64_t table = 0;
    std::uint32_t constants[kMaxRootConstants] {};
    std::uint32_t numConstants = 0;
    std::uint32_t destOffset = 0;
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

    // Any signature write invalidates previously bound arguments (D3D12 rule).
    // Null is a legal known-unset signature.
    void SetSignature(std::uint64_t sig)
    {
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
    bool queryActive = false;
    bool bundleOrIndirectSeen = false;

    void SetHeaps(std::uint32_t count, const std::uint64_t* handles)
    {
        heapCount = 0;
        if (!handles || count == 0)
        {
            heapState = BindState::KnownUnset;
            return;
        }
        if (count > kMaxHeaps)
        {
            heapState = BindState::Unknown;
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i)
            heaps[i] = handles[i];
        heapCount = count;
        heapState = BindState::KnownValue;
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

    // RTsSingleHandleToDescriptorRange expands a contiguous range at restore time
    // using the device increment; here we only record the API arguments.
    void SetRenderTargets(std::uint32_t numRTVs, const std::uint64_t* rtvHandles, bool singleHandleRange,
                          bool hasDsv, std::uint64_t dsvHandle)
    {
        om = OmBinding {};
        if (numRTVs == 0 && !hasDsv)
        {
            om.state = BindState::KnownUnset;
            return;
        }
        if (numRTVs > kMaxRTVs)
        {
            om.state = BindState::Unknown;
            return;
        }
        om.numRTVs = numRTVs;
        om.singleHandleRange = singleHandleRange;
        om.hasDsv = hasDsv;
        om.dsvHandle = dsvHandle;
        if (rtvHandles && numRTVs)
        {
            if (singleHandleRange)
                om.rtvHandles[0] = rtvHandles[0];
            else
                for (std::uint32_t i = 0; i < numRTVs; ++i)
                    om.rtvHandles[i] = rtvHandles[i];
        }
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

    // D3D12 Create/Reset leave predication disabled and OM empty; those are
    // known API defaults, not "never observed".
    void AdoptApiDefaults()
    {
        snap = GraphicsSnapshot {};
        snap.predication.state = BindState::KnownUnset;
        snap.predication.resource = 0;
        snap.om.state = BindState::KnownUnset;
    }

    void OnCreate(std::uint64_t id)
    {
        listId = id;
        generation = 1;
        generationKnown = true;
        live = true;
        ineligible = false;
        AdoptApiDefaults();
    }

    // Only a successful Reset starts a new generation and adopts API defaults.
    bool OnReset(bool succeeded)
    {
        if (!live || !succeeded)
            return false;
        ++generation;
        ineligible = false;
        AdoptApiDefaults();
        return true;
    }

    // ClearState is not Reset: bindings become default, but active RP/query stay.
    void OnClearState()
    {
        if (!live)
            return;
        const bool rp = snap.renderPassActive;
        const bool q = snap.queryActive;
        const bool bad = snap.bundleOrIndirectSeen;
        snap = GraphicsSnapshot {};
        snap.predication.state = BindState::KnownUnset;
        snap.renderPassActive = rp;
        snap.queryActive = q;
        snap.bundleOrIndirectSeen = bad;
    }

    void MarkIneligible()
    {
        if (live)
            ineligible = true;
    }

    void OnRelease()
    {
        live = false;
        listId = 0;
        generationKnown = false;
        ineligible = false;
        snap = GraphicsSnapshot {};
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
    if (ineligible || s.bundleOrIndirectSeen || s.renderPassActive || s.queryActive)
        return { false, "ineligible_generation" };
    if (s.graphics.signatureState == BindState::Unknown)
        return { false, "graphics_root_unknown" };
    if (s.psoState != BindState::KnownValue)
        return { false, "pso_unknown_or_unset" };
    if (s.viewportState != BindState::KnownValue)
        return { false, "viewport_unknown" };
    if (s.scissorState != BindState::KnownValue)
        return { false, "scissor_unknown" };
    if (s.topologyState != BindState::KnownValue)
        return { false, "topology_unknown" };
    if (s.om.state != BindState::KnownValue && s.om.state != BindState::KnownUnset)
        return { false, "om_unknown" };
    if (!s.predication.IsDisabled())
        return { false, "predication_active_or_unknown" };
    return { true, "ok" };
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
