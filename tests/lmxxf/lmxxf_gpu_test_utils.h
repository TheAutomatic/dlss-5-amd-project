#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static std::wstring Widen(const char *s) { return std::wstring(s, s + std::strlen(s)); }

static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static void WaitQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "wait fence");
    Check(queue->Signal(fence, 1), "wait signal");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(ev != nullptr, "wait event");
    Check(fence->SetEventOnCompletion(1, ev), "wait set event");
    Require(WaitForSingleObject(ev, 30000) == WAIT_OBJECT_0 &&
            fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= 1 &&
            SUCCEEDED(device->GetDeviceRemovedReason()), "confirmed queue completion");
    CloseHandle(ev);
    fence->Release();
}

// Deterministic, non-constant pattern so an output hash can tell "same math" from
// "different math". RGB9E5 words use exponent 15 with varying 9-bit mantissas; that
// format has no Inf/NaN, so any bit pattern is a finite value. R10G10B10A2 is UNORM, so
// the 10-bit channels are the same mantissa pattern widened to 10 bits.
// --scale16 raises every FP16 exponent by 4: an exact x16 of the same pattern.
static UINT g_patternExponentShift = 0;

static void FillRow(unsigned char *dst, UINT y, UINT w, DXGI_FORMAT format)
{
    const bool rgb9e5 = format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    const bool r10g10b10a2 = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    for (UINT x = 0; x < w; ++x)
    {
        const UINT mR = (x * 37u + y * 11u) & 0x1FFu;
        const UINT mG = (x * 53u + y * 29u) & 0x1FFu;
        const UINT mB = (x * 97u + y * 7u) & 0x1FFu;
        if (rgb9e5)
        {
            // 4 bytes per pixel: three 9-bit mantissas plus a shared 5-bit exponent.
            const UINT word = mR | (mG << 9) | (mB << 18) | (15u << 27);
            std::memcpy(dst + size_t(x) * 4, &word, 4);
        }
        else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            const float rgba[] = {mR / 512.f, mG / 512.f, mB / 512.f, 1.f};
            std::memcpy(dst + size_t(x) * sizeof rgba, rgba, sizeof rgba);
        }
        else if (r10g10b10a2)
        {
            // 4 bytes per pixel: 10-bit R, G, B and a 2-bit alpha of 3 (opaque).
            const UINT word = (mR << 1) | ((mG << 1) << 10) | ((mB << 1) << 20) | (3u << 30);
            std::memcpy(dst + size_t(x) * 4, &word, 4);
        }
        else
        {
            // 8 bytes per pixel: four FP16 channels (NOT four float32). Exponent 12..15
            // keeps every value finite; 0x3C00 is 1.0f in FP16.
            const UINT16 h[4] = {
                static_cast<UINT16>(((12u + g_patternExponentShift + (mR >> 7)) << 10) | (mR & 0x3FFu)),
                static_cast<UINT16>(((12u + g_patternExponentShift + (mG >> 7)) << 10) | (mG & 0x3FFu)),
                static_cast<UINT16>(((12u + g_patternExponentShift + (mB >> 7)) << 10) | (mB & 0x3FFu)),
                0x3C00u
            };
            std::memcpy(dst + size_t(x) * 8, h, sizeof h);
        }
    }
}

// Fills `tex` with FillRow. `tex` must start and end in NON_PIXEL_SHADER_RESOURCE.
static void UploadColorPattern(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *tex)
{
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    const UINT w = static_cast<UINT>(td.Width), h = td.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT numRows = 0;
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, nullptr, &total);

    D3D12_HEAP_PROPERTIES up {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *upload = nullptr;
    Check(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&upload)),
          "pattern upload buffer");
    void *mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped), "map pattern upload");
    auto *base = static_cast<unsigned char *>(mapped);
    for (UINT y = 0; y < h; ++y)
        FillRow(base + size_t(y) * fp.Footprint.RowPitch, y, w, td.Format);
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator *al = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "pattern alloc");
    ID3D12GraphicsCommandList *cl = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)), "pattern list");
    D3D12_RESOURCE_BARRIER toCopy {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
    cl->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dstLoc {}, srcLoc {};
    dstLoc.pResource = tex;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER toSrv = toCopy;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &toSrv);
    Check(cl->Close(), "close pattern list");
    ID3D12CommandList *ls[] = { cl };
    queue->ExecuteCommandLists(1, ls);
    WaitQueue(device, queue);
    cl->Release();
    al->Release();
    upload->Release();
}

// FNV-1a over the valid bytes of each row (row padding is uninitialized and would
// make the hash unstable). NativeGameCodec::Record leaves its output in
// NON_PIXEL_SHADER_RESOURCE.
// exponentShift > 0 divides every normal FP16 value by 2^shift before hashing, so a run on an
// exactly scaled input can be compared bit for bit with the unscaled one.
static uint64_t HashTexture(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *tex,
                            UINT exponentShift = 0)
{
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        std::fprintf(stderr, "output_hash: output is not a texture; skipped\n");
        return 0;
    }
    const UINT h = td.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT numRows = 0;
    // rowBytes is the footprint's own count of valid bytes per row, so every output format
    // hashes correctly; guessing bytes-per-pixel over-read the last row of a 4-byte format.
    UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &total);

    D3D12_HEAP_PROPERTIES rbh {};
    rbh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    Check(device->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&rb)),
          "readback buffer");

    ID3D12CommandAllocator *al = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "hash alloc");
    ID3D12GraphicsCommandList *cl = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)), "hash list");
    D3D12_RESOURCE_BARRIER toCopy {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cl->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dstLoc {}, srcLoc {};
    dstLoc.pResource = rb;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = fp;
    srcLoc.pResource = tex;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER toSrv = toCopy;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &toSrv);
    Check(cl->Close(), "close hash list");
    ID3D12CommandList *ls[] = { cl };
    queue->ExecuteCommandLists(1, ls);
    WaitQueue(device, queue);

    uint64_t hsh = 1469598103934665603ull;
    void *mapped = nullptr;
    Check(rb->Map(0, nullptr, &mapped), "map readback");
    const auto *base = static_cast<const unsigned char *>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const unsigned char *row = base + size_t(y) * fp.Footprint.RowPitch;
        for (UINT64 i = 0; i < rowBytes; ++i)
        {
            unsigned char byte = row[i];
            // RGB only: the decoder stores the source alpha unscaled.
            if (exponentShift && td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && ((i >> 1) & 3) != 3)
            {
                UINT16 half = 0;
                std::memcpy(&half, row + (i & ~UINT64(1)), 2);
                const UINT e = (half >> 10) & 0x1Fu;
                if (e > exponentShift && e < 31)
                    half = static_cast<UINT16>(half - (exponentShift << 10));
                byte = (i & 1) ? static_cast<unsigned char>(half >> 8) : static_cast<unsigned char>(half);
            }
            hsh ^= byte;
            hsh *= 1099511628211ull;
        }
    }
    rb->Unmap(0, nullptr);
    cl->Release();
    al->Release();
    rb->Release();
    return hsh;
}

// FMA A/B accounting: write valid row bytes (no pitch padding) so two runs can be differenced.
static void DumpTexture(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *tex, const char *path)
{
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        std::fprintf(stderr, "dump: output is not a texture; skipped\n");
        return;
    }
    const UINT h = td.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT numRows = 0;
    UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES rbh {};
    rbh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    Check(device->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&rb)),
          "dump readback");
    ID3D12CommandAllocator *al = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "dump alloc");
    ID3D12GraphicsCommandList *cl = nullptr;
    Check(device->CreateCommandList(1, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)), "dump list");
    D3D12_RESOURCE_BARRIER toCopy {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cl->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dstLoc {}, srcLoc {};
    dstLoc.pResource = rb;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = fp;
    srcLoc.pResource = tex;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER toSrv = toCopy;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &toSrv);
    Check(cl->Close(), "close dump list");
    ID3D12CommandList *ls[] = { cl };
    queue->ExecuteCommandLists(1, ls);
    WaitQueue(device, queue);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
    {
        std::fprintf(stderr, "dump: cannot open %s\n", path);
    }
    else
    {
        // header: width height rowBytes height (little-endian uint32 x4), then rows of rowBytes
        const unsigned hdr[4] = { (unsigned)td.Width, h, (unsigned)rowBytes, h };
        fwrite(hdr, 4, 4, f);
        void *mapped = nullptr;
        Check(rb->Map(0, nullptr, &mapped), "map dump");
        const auto *base = static_cast<const unsigned char *>(mapped);
        for (UINT y = 0; y < h; ++y)
            fwrite(base + size_t(y) * fp.Footprint.RowPitch, 1, (size_t)rowBytes, f);
        rb->Unmap(0, nullptr);
        fclose(f);
        std::printf("dump: %s %ux%u rowBytes=%llu\n", path, (unsigned)td.Width, h,
                    static_cast<unsigned long long>(rowBytes));
    }
    cl->Release();
    al->Release();
    rb->Release();
}
