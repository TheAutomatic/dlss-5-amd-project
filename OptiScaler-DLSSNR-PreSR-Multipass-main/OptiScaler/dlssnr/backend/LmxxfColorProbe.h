#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <string_view>

namespace DlssNr::Backend::LmxxfProbe
{
enum class Mode { Off, Original, CopyCurrent, StagingCurrent, StagingPrevious, Invalid, ProxyOriginal, SplitOriginal, CodecPassthrough };
inline Mode ParseMode(std::string_view value)
{
    if (value == "off") return Mode::Off;
    if (value == "original") return Mode::Original;
    if (value == "copy-current") return Mode::CopyCurrent;
    if (value == "staging-current") return Mode::StagingCurrent;
    if (value == "staging-previous") return Mode::StagingPrevious;
    if (value == "proxy-original") return Mode::ProxyOriginal;
    if (value == "split-original") return Mode::SplitOriginal;
    if (value == "codec-passthrough") return Mode::CodecPassthrough;
    return Mode::Invalid; // Never silently enable HIP on a misspelled diagnostic option.
}

inline bool NeedsOpenListProxy(Mode mode)
{
    return mode == Mode::Off || mode == Mode::ProxyOriginal || mode == Mode::SplitOriginal || mode == Mode::CodecPassthrough;
}

struct Evidence
{
    uint64_t evaluateId = 0;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *expectedColor = nullptr;
    bool copied = false;
    bool sampled = false;
    // Staging-specific fields
    Mode mode = Mode::Off;
    uint64_t epoch = 0;
    uint64_t sourceSequence = 0;  // For previous: which evaluate produced the source
    uint32_t age = 0;  // 0 = current, 1 = previous
    bool priming = false;
};
inline Evidence &CurrentEvidence() { static thread_local Evidence e; return e; }

// Diagnostic only. No Execute, Signal, Wait, allocator Reset, shader, or HIP call.
// Copy and SR consumer must be recorded on the SAME DIRECT list, with the consumer
// returning the private texture in NON_PIXEL_SHADER_RESOURCE (the FFX contract).
// A different list never shares an output. The application must serialize reuse
// of a given list; this probe cannot make invalid concurrent list execution safe.
class ColorCopy
{
    struct Entry
    {
        IUnknown *listIdentity = nullptr;
        ID3D12Resource *output = nullptr;
        UINT64 width = 0;
        UINT height = 0;
    };
    static constexpr size_t kMaxEntries = 32;
    static constexpr UINT64 kMaxBytes = 128ull * 1024 * 1024;
    std::array<Entry, kMaxEntries> entries {};
    size_t count = 0;
    UINT64 bytes = 0;
    const char *reason = "idle";

  public:
    ColorCopy() = default;
    ColorCopy(const ColorCopy &) = delete;
    ColorCopy &operator=(const ColorCopy &) = delete;
    // Intentionally keep bounded pins until process exit in the product. A completed
    // fence alone does not prove a closed native list cannot be executed again.
    // No unsubmitted/resize/timeout path is allowed to free these resources.
    ~ColorCopy() = default;
    const char *Reason() const { return reason; }
    size_t EntryCount() const { return count; }
    UINT64 AllocatedBytes() const { return bytes; }

    ID3D12Resource *Record(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                          ID3D12Resource *color, D3D12_RESOURCE_STATES state,
                          UINT validWidth, UINT validHeight)
    {
        reason = "invalid_input";
        if (!device || !cmd || !color || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return nullptr;
        const auto d = color->GetDesc();
        reason = "unsupported_description";
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.DepthOrArraySize != 1 ||
            d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0 ||
            d.Width == 0 || d.Height == 0 || d.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            d.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            return nullptr;
        reason = "invalid_extent";
        if (!validWidth || !validHeight || validWidth > d.Width || validHeight > d.Height)
            return nullptr;
        reason = "unsupported_state";
        if (state != D3D12_RESOURCE_STATE_COMMON && state != D3D12_RESOURCE_STATE_RENDER_TARGET &&
            state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS && state != D3D12_RESOURCE_STATE_COPY_SOURCE &&
            state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            state != (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
            return nullptr;

        IUnknown *identity = nullptr;
        reason = "identity_failed";
        if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
            return nullptr;
        ID3D12Resource *output = nullptr;
        for (size_t i = 0; i < count; ++i)
            if (entries[i].listIdentity == identity && entries[i].width == d.Width && entries[i].height == d.Height)
            {
                output = entries[i].output;
                break;
            }
        if (!output)
        {
            auto td = d;
            td.Alignment = 0;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            td.Flags = D3D12_RESOURCE_FLAG_NONE;
            const auto allocation = device->GetResourceAllocationInfo(0, 1, &td);
            reason = "pin_budget_exhausted";
            if (count == kMaxEntries || allocation.SizeInBytes == UINT64_MAX ||
                allocation.SizeInBytes > kMaxBytes - bytes)
            {
                identity->Release();
                return nullptr;
            }
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            reason = "allocation_failed";
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&output))))
            {
                identity->Release();
                return nullptr;
            }
            entries[count++] = {identity, output, d.Width, d.Height};
            bytes += allocation.SizeInBytes;
        }
        else
            identity->Release();

        // Every recording has identical before/after states, including first use,
        // Reset-without-Execute, closed-list replay and multiple Evaluate sites.
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER barriers[2] {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, read,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE};
        const UINT n = state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1u : 2u;
        cmd->ResourceBarrier(n, barriers);
        cmd->CopyResource(output, color); // Preserve allocation padding; valid extent is unchanged.
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.StateAfter = read;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateAfter = state;
        cmd->ResourceBarrier(n, barriers);
        reason = "recorded_not_submitted";
        return output;
    }

    // Harness/explicit teardown only: caller proves all GPU work complete AND all
    // recorded lists discarded/no future replay. The product deliberately never calls it.
    void ReleaseAfterGpuIdleAndDiscard()
    {
        for (size_t i = 0; i < count; ++i)
        {
            entries[i].output->Release();
            entries[i].listIdentity->Release();
            entries[i] = {};
        }
        count = 0;
        bytes = 0;
    }
};
} // namespace DlssNr::Backend::LmxxfProbe
