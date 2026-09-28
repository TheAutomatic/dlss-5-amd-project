#pragma once

// Test-runtime only. No product setting or inference change. Build explicitly with
// LMXXF_NR_HIGHLIGHT_DIAGNOSTICS; ordinary release builds contain no capture calls.
#ifdef LMXXF_NR_HIGHLIGHT_DIAGNOSTICS
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <array>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>

namespace HighlightDiagnostics
{
#pragma pack(push, 1)
struct Record
{
    uint32_t frame, stage, tile, width, height, x, y, w, h, format, exposureSource, debugView;
    float pre, scale, paper, transfer, color;
    uint64_t tick;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 76, "capture wire format");

class Capture
{
    static constexpr UINT kFrames = 64, kTiles = 5, kEdge = 32;
    static constexpr UINT kTileBytes = 256 * kEdge;
    static constexpr UINT kBytes = (4 * kTiles + 1) * kTileBytes;
    struct Tile { Record record {}; UINT offset = 0, bpp = 0; };
    struct Slot
    {
        ID3D12Resource *buffer = nullptr;
        std::vector<Tile> tiles;
        bool output = false, submitted = false, done = false;
    };
    std::array<Slot, kFrames> slots {};
    ID3D12Fence *fence = nullptr;
    int current = -1;
    UINT used = 0, sequence = 0;
    bool previousKey = false, triggered = false, sealed = false;
    std::vector<unsigned char> bytes {'N','R','H','L','V','1',0,0};
    std::shared_ptr<std::atomic<int>> saved = std::make_shared<std::atomic<int>>(0);
    std::thread writer;
    std::atomic<int> phase {0}; // Publishes the immutable path to GetStatus.
    wchar_t path[MAX_PATH] {};
    Record meta {};

    static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, 0, before, after};
        list->ResourceBarrier(1, &b);
    }
    void Save()
    {
        if (sealed || !triggered) return;
        sealed = true;
        phase.store(2);
        auto state = saved;
        std::wstring destination(path);
        // All GPU reads are already complete. The worker owns only CPU bytes;
        // disk I/O and file close never run on the render thread.
        try
        {
            writer = std::thread([data = std::move(bytes), destination, state]() {
                FILE *f = nullptr;
                if (_wfopen_s(&f, destination.c_str(), L"wb") || !f)
                { state->store(-1); return; }
                const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
                const bool closed = fclose(f) == 0;
                state->store(ok && closed ? 1 : -1);
            });
        }
        catch (...) { state->store(-1); }
    }

  public:
    Capture() = default;
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;
    // Session destruction must join before the host can unload the runtime DLL.
    // Ordinary frames never join, and the worker never holds GPU resources.
    ~Capture() { if (writer.joinable()) writer.join(); }
    const wchar_t *Path() const { return path; }
    // No destructor release: the Session explicitly proves GPU idle before Release.
    // Its fail-closed path intentionally retains these bounded GPU allocations.
    void Begin(ID3D12Device *device, Record metadata, bool keyDown)
    {
        Poll();
        current = -1;
        ++sequence;
        const bool pressed = keyDown && !previousKey;
        previousKey = keyDown;
        if (pressed && !triggered)
        {
            triggered = true;
            wchar_t dir[MAX_PATH] {};
            const DWORD n = GetTempPathW(MAX_PATH, dir);
            if (!n || n >= MAX_PATH || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            { sealed = true; saved->store(-1); return; }
            swprintf_s(path, L"%slmxxf-highlight-%lu-%llu.nrhl", dir, GetCurrentProcessId(), GetTickCount64());
            bytes.reserve(kFrames * kBytes);
            phase.store(1);
        }
        if (!triggered || sealed || used == kFrames || !fence) return;
        Slot &slot = slots[used];
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kBytes;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.buffer))))
        { used = kFrames; return; }
        slot.tiles.reserve(4 * kTiles + 1);
        current = static_cast<int>(used++);
        meta = metadata; meta.frame = sequence; meta.tick = GetTickCount64();
    }

    void Copy(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
              D3D12_RESOURCE_STATES state, UINT stage, UINT originX, UINT originY, UINT width, UINT height)
    {
        if (current < 0 || !resource || stage > 4) return;
        const auto d = resource->GetDesc();
        const bool exposure = stage == 4;
        const UINT bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 :
                         exposure && d.Format == DXGI_FORMAT_R32_FLOAT ? 4 :
                         exposure && d.Format == DXGI_FORMAT_R16_FLOAT ? 2 : 0;
        if (!bpp || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 ||
            d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1 || !width || !height ||
            uint64_t(originX) + width > d.Width || uint64_t(originY) + height > d.Height) return;
        Slot &slot = slots[current];
        Barrier(list, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const UINT centers[kTiles][2] = {{2,2},{1,1},{3,1},{1,3},{3,3}};
        for (UINT tile = 0; tile < (exposure ? 1u : kTiles); ++tile)
        {
            Tile t {}; t.record = meta; t.bpp = bpp;
            auto &r = t.record;
            r.stage = stage; r.tile = tile; r.width = static_cast<UINT>(d.Width); r.height = d.Height;
            r.w = (std::min)(exposure ? 1u : kEdge, width);
            r.h = (std::min)(exposure ? 1u : kEdge, height);
            r.x = originX + (std::min)(width - r.w, (width - r.w) * centers[tile][0] / 4);
            r.y = originY + (std::min)(height - r.h, (height - r.h) * centers[tile][1] / 4);
            r.format = d.Format;
            t.offset = (stage * kTiles + tile) * kTileBytes;
            D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
            src.pResource = resource; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = slot.buffer; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = t.offset;
            dst.PlacedFootprint.Footprint = {d.Format, r.w, r.h, 1, 256};
            D3D12_BOX box {r.x, r.y, 0, r.x + r.w, r.y + r.h, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            slot.tiles.push_back(t);
        }
        Barrier(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    void OutputsRecorded() { if (current >= 0) slots[current].output = true; }
    void Submitted(ID3D12CommandQueue *queue)
    {
        if (current < 0) return;
        Slot &s = slots[current];
        // Only Retire, after the actual consumer submission, may certify all stages.
        if (s.output && !s.submitted && queue && SUCCEEDED(queue->Signal(fence, current + 1)))
            s.submitted = true;
        current = -1;
    }
    void Cancel() { current = -1; } // Never read or recycle an unsubmitted slot.
    void Poll()
    {
        if (!fence || sealed) return;
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return; // Device removal is not completion.
        bool pending = false;
        for (UINT i = 0; i < used; ++i)
        {
            auto &s = slots[i];
            if (!s.submitted || s.done) continue;
            if (completed < i + 1) { pending = true; continue; }
            unsigned char *p = nullptr;
            D3D12_RANGE range {0, kBytes};
            if (SUCCEEDED(s.buffer->Map(0, &range, reinterpret_cast<void **>(&p))))
            {
                for (const auto &t : s.tiles)
                {
                    const auto *header = reinterpret_cast<const unsigned char *>(&t.record);
                    bytes.insert(bytes.end(), header, header + sizeof(Record));
                    for (UINT y = 0; y < t.record.h; ++y)
                        bytes.insert(bytes.end(), p + t.offset + y * 256,
                                     p + t.offset + y * 256 + t.record.w * t.bpp);
                }
                D3D12_RANGE noWrite {0, 0}; s.buffer->Unmap(0, &noWrite);
            }
            s.done = true;
        }
        if (used == kFrames && current < 0 && !pending) Save();
    }
    void ReleaseAfterGpuIdle()
    {
        Poll(); Save();
        for (auto &s : slots) if (s.buffer) { s.buffer->Release(); s.buffer = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
    }
    std::string Status() const
    {
        char file[MAX_PATH * 3] {};
        const int stage = phase.load();
        if (stage) WideCharToMultiByte(CP_UTF8, 0, path, -1, file, sizeof(file), nullptr, nullptr);
        const int result = saved->load();
        return std::string(" highlight=") + (result < 0 ? "failed" : result > 0 ? "saved" :
            stage == 2 ? "writing" : stage == 1 ? "capturing" : "F9-ready") + " file=" + file;
    }
};
}
#endif
