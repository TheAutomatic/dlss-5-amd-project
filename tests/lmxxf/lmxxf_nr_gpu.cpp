#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

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
    WaitForSingleObject(ev, 30000);
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

int main(int argc, char **argv)
{
    bool queueMismatch = false, resize = false, rgb9e5 = false, r10g10b10a2 = false, autoExposure = false,
         scale16 = false, outputHash = false, rejectFormats = false,
         useExposure = false, badExposure = false, ultrawide = false, subrect = false, temporalTest = false,
         temporalGuides = false, nativeTemporalTest = false, blockedProducer = false;
    int test17Mode=-1;
    for (int i = 3; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--native-temporal"))
            nativeTemporalTest = temporalTest = outputHash = true;
        else if (!std::strcmp(argv[i], "--blocked-producer")) {
            blockedProducer = true;
            _putenv_s("DLSS5_VIT_ADAPTIVE", "1");
            _putenv_s("DLSS5_VIT_ADAPTIVE_LOG", "");
            _putenv_s("DLSS5_VIT_REUSE_HOTKEY", "0");
        }
        else if (!std::strcmp(argv[i], "--queue-mismatch"))
            queueMismatch = true;
        else if (!std::strcmp(argv[i], "--resize"))
            resize = true;
        else if (!std::strcmp(argv[i], "--rgb9e5"))
            rgb9e5 = outputHash = true;
        else if (!std::strcmp(argv[i], "--r10g10b10a2"))
            r10g10b10a2 = outputHash = true;
        else if (!std::strcmp(argv[i], "--output-hash"))
            outputHash = true;
        else if (!std::strcmp(argv[i], "--reject-formats"))
            rejectFormats = true;
        else if (!std::strcmp(argv[i], "--exposure"))
            useExposure = outputHash = true;
        else if (!std::strcmp(argv[i], "--exposure-bad"))
            useExposure = badExposure = outputHash = true;
        else if (!std::strcmp(argv[i], "--ultrawide"))
            ultrawide = outputHash = true;
        else if (!std::strcmp(argv[i], "--subrect"))
            subrect = outputHash = true;
        else if (!std::strcmp(argv[i], "--auto-exposure"))
            autoExposure = outputHash = true;
        else if (!std::strcmp(argv[i], "--scale16"))
            scale16 = outputHash = true;
        else if (!std::strcmp(argv[i], "--temporal"))
            temporalTest = outputHash = true;
        else if (!std::strcmp(argv[i], "--temporal-guides"))
            temporalGuides = temporalTest = outputHash = true;
        else if (!std::strcmp(argv[i], "--test17-soft"))
            test17Mode=1,outputHash=true;
        else if (!std::strcmp(argv[i], "--test17-identity"))
            test17Mode=2,outputHash=true;
        else
        {
            std::fprintf(stderr,
                         "usage: lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <assets_dir> "
                         "[--queue-mismatch|--resize] [--rgb9e5|--r10g10b10a2] [--output-hash] [--reject-formats] [--ultrawide] "
                         "[--exposure|--exposure-bad] [--subrect] [--auto-exposure] [--scale16]\n");
            return 2;
        }
    }
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <assets_dir> "
                     "[--queue-mismatch|--resize] [--rgb9e5|--r10g10b10a2] [--output-hash] [--reject-formats] [--ultrawide] "
                         "[--exposure|--exposure-bad] [--subrect] [--auto-exposure] [--scale16]\n");
        return 2;
    }

    HMODULE dll = LoadLibraryW(Widen(argv[1]).c_str());
    Require(dll != nullptr, "LoadLibraryW");
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi *)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "GetProcAddress");

    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    Require(getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "GetApi");
    LmxxfNrCapabilities caps {};
    caps.struct_size = sizeof(caps);
    Require(api.QueryCapabilities(&caps) == LMXXF_NR_OK, "QueryCapabilities");

    Require(caps.graph_supported == 0, "graph off");

    IDXGIFactory4 *factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    // The first AMD device is often the 780M iGPU. The gfx1201 modules only match the
    // discrete card, so pick the AMD adapter with the most dedicated memory.
    SIZE_T bestMemory = 0;
    DXGI_ADAPTER_DESC1 chosen {};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        const bool amd = desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        if (amd && desc.DedicatedVideoMemory > bestMemory)
        {
            ID3D12Device *candidate = nullptr;
            if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate))))
            {
                if (device)
                    device->Release();
                device = candidate;
                chosen = desc;
                bestMemory = desc.DedicatedVideoMemory;
            }
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    Require(device != nullptr, "AMD D3D12 device");
    std::printf("adapter=%ls dedicated_mib=%llu\n", chosen.Description,
                static_cast<unsigned long long>(chosen.DedicatedVideoMemory / (1024ull * 1024ull)));

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ID3D12CommandQueue *queue2 = nullptr;
    if (queueMismatch)
        Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue2)), "second queue");
    ID3D12CommandQueue *submitQueue = queueMismatch ? queue2 : queue;
    ID3D12CommandAllocator *alloc = nullptr;
    ID3D12CommandAllocator *outAlloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    ID3D12GraphicsCommandList *list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // 2024x848 is 3440x1440 at DLSS Quality 1: 1.72M pixels, under the 1920x1080 budget of
    // 2.07M, but wider than 1920. Admission is by pixel budget precisely so this is allowed.
    td.Width = ultrawide ? 2024 : 1920;
    td.Height = ultrawide ? 848 : 1080;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = rgb9e5        ? DXGI_FORMAT_R9G9B9E5_SHAREDEXP
                : r10g10b10a2 ? DXGI_FORMAT_R10G10B10A2_UNORM
                              : DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    // RE9's scene colour is RGB9E5 and is only ever read (SRV), so do not request a UAV.
    td.Flags = rgb9e5 ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *color = nullptr;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&color)),
          "color");
    if (outputHash)
    {
        g_patternExponentShift = scale16 ? 4u : 0u;
        UploadColorPattern(device, submitQueue, color);
    }

    const std::wstring modules = Widen(argv[2]);
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modules.c_str();
    info.flags = queueMismatch ? LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK : 0;
    void *ctx = nullptr;
    Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create");
    if(test17Mode>=0){
        auto select=reinterpret_cast<int32_t(*)(void*,uint32_t)>(GetProcAddress(dll,"LmxxfNrTest17Select"));
        Require(select&&select(ctx,uint32_t(test17Mode))==LMXXF_NR_OK,"test17 runtime mode selected");
    }
    const int32_t prep = api.PrepareSession(ctx);
    if (prep != LMXXF_NR_OK)
    {
        char err[256] {};
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "PrepareSession rc=%d err=%s\n", prep, err);
        Require(false, "PrepareSession");
    }

    LmxxfNrFrameInfo frame {};
    frame.struct_size = sizeof(frame);
    frame.color_width = static_cast<UINT>(td.Width);
    frame.color_height = td.Height;
    // --subrect: the Color buffer is larger than the render area the game actually uses (NGX
    // subrect). The codec is built at the ALLOCATION size, so the two disagree on every frame.
    if (subrect)
    {
        frame.color_width = 1280;
        frame.color_height = 720;
    }
    frame.color = color;
    frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // No exposure texture: the runtime meters the colour. With --scale16 the network must see the
    // same input, so the output is exactly 16x the unscaled run and hashes equal after /16.
    if (autoExposure)
        frame.flags |= LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE;
    frame.paper_white = 1.0f;
    // An unsupported colour must be a retryable contract rejection, not a poisoned session.
    // Poisoning is what made RE9's RGB9E5 failure permanent and left no clue in the log.
    if (rejectFormats)
    {
        // R10G10B10A2 left this list when the runtime started accepting it (Horizon); --r10g10b10a2
        // runs it end to end instead. R8G8 stands in as a 4-byte format the codec cannot read.
        const DXGI_FORMAT kBadFormats[] = {DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           DXGI_FORMAT_R8G8_UNORM,
                                           DXGI_FORMAT_R32_FLOAT};
        for (DXGI_FORMAT fmt : kBadFormats)
        {
            D3D12_RESOURCE_DESC badDesc = td;
            badDesc.Format = fmt;
            badDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource *badTex = nullptr;
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &badDesc,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  nullptr, IID_PPV_ARGS(&badTex)),
                  "unsupported-format colour");
            LmxxfNrFrameInfo badFrame = frame;
            badFrame.color = badTex;
            LmxxfNrJob badJob {};
            badJob.struct_size = sizeof(badJob);
            const int32_t rc = api.PrepareFrame(ctx, &badFrame, &badJob);
            char badErr[256] {};
            api.GetLastError(badErr, sizeof badErr);
            std::printf("reject fmt=%u rc=%d err=%s\n", unsigned(fmt), rc, badErr);
            Require(rc == LMXXF_NR_INVALID_ARGUMENT, "unsupported format -> INVALID_ARGUMENT");
            Require(std::strstr(badErr, "fmt=") != nullptr, "rejection names the format");
            badTex->Release();
        }
        // Geometry with LmxxfFitLarge off is the documented limit; same contract. 2560x1440 is over
        // the pixel budget. The other two are within it but would be downsampled too far: 2600x720
        // is past the 2560 width cap, 1440x1440 is taller than 1080.
        const struct { UINT64 w; UINT h; } kBadShapes[] = {{2560, 1440}, {2600, 720}, {1440, 1440}};
        for (const auto &shape : kBadShapes)
        {
            D3D12_RESOURCE_DESC bigDesc = td;
            bigDesc.Width = shape.w;
            bigDesc.Height = shape.h;
            ID3D12Resource *bigTex = nullptr;
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bigDesc,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  nullptr, IID_PPV_ARGS(&bigTex)),
                  "oversized colour");
            LmxxfNrFrameInfo bigFrame = frame;
            bigFrame.color = bigTex;
            bigFrame.color_width = static_cast<uint32_t>(shape.w);
            bigFrame.color_height = shape.h;
            LmxxfNrJob bigJob {};
            bigJob.struct_size = sizeof(bigJob);
            const int32_t bigRc = api.PrepareFrame(ctx, &bigFrame, &bigJob);
            char bigErr[256] {};
            api.GetLastError(bigErr, sizeof bigErr);
            std::printf("reject %llux%u rc=%d err=%s\n", static_cast<unsigned long long>(shape.w), shape.h,
                        bigRc, bigErr);
            Require(bigRc == LMXXF_NR_INVALID_ARGUMENT, "inadmissible geometry -> INVALID_ARGUMENT");
            Require(std::strstr(bigErr, "outside admitted geometry") != nullptr, "geometry reason named");
            bigTex->Release();
        }

        // A host built against ABI v1 sends the smaller struct and has no exposure fields.
        // That must still run: the exposure fields are an ABI growth, not a new requirement.
        LmxxfNrFrameInfo v1Frame = frame;
        v1Frame.struct_size = LMXXF_NR_FRAME_INFO_V1_SIZE;
        v1Frame.exposure = nullptr;
        LmxxfNrJob v1Job {};
        v1Job.struct_size = sizeof(v1Job);
        const int32_t v1Rc = api.PrepareFrame(ctx, &v1Frame, &v1Job);
        std::printf("v1 frame struct_size=%u rc=%d\n", unsigned(LMXXF_NR_FRAME_INFO_V1_SIZE), v1Rc);
        Require(v1Rc == LMXXF_NR_OK && v1Job.handle != nullptr, "ABI v1 struct_size still runs");
        Require(api.CancelUnsubmitted(ctx, v1Job.handle) == LMXXF_NR_OK, "cancel v1 frame");
    }

    // Exposure reaches the codec through a 1x1 scale texture plus two scalars. The output hash
    // must differ from the no-exposure run, which is what shows it got as far as the shader.
    ID3D12Resource *exposureTex = nullptr;
    if (useExposure)
    {
        D3D12_RESOURCE_DESC ed {};
        ed.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        // --exposure-bad hands the codec a 2x2 scale. That is unusable (it samples Texture2D<float>
        // at (0,0)) and must cost the frame its exposure, not reject the frame itself.
        ed.Width = badExposure ? 2u : 1u;
        ed.Height = badExposure ? 2u : 1u;
        ed.DepthOrArraySize = ed.MipLevels = 1;
        ed.Format = DXGI_FORMAT_R32_FLOAT;
        ed.SampleDesc.Count = 1;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&exposureTex)),
              "exposure");
        D3D12_HEAP_PROPERTIES up {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 4;
        bd.Height = 1;
        bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *expUpload = nullptr;
        Check(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&expUpload)),
              "exposure upload");
        void *expMapped = nullptr;
        Check(expUpload->Map(0, nullptr, &expMapped), "map exposure upload");
        *static_cast<float *>(expMapped) = 0.25f;
        expUpload->Unmap(0, nullptr);
        ID3D12CommandAllocator *expAlloc = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&expAlloc)), "exp alloc");
        ID3D12GraphicsCommandList *expCl = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, expAlloc, nullptr, IID_PPV_ARGS(&expCl)),
              "exp list");
        D3D12_RESOURCE_BARRIER expToCopy {};
        expToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        expToCopy.Transition = {exposureTex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
        expCl->ResourceBarrier(1, &expToCopy);
        D3D12_TEXTURE_COPY_LOCATION expDst {}, expSrc {};
        expDst.pResource = exposureTex;
        expDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        expSrc.pResource = expUpload;
        expSrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        expSrc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        expSrc.PlacedFootprint.Footprint.Width = 1;
        expSrc.PlacedFootprint.Footprint.Height = 1;
        expSrc.PlacedFootprint.Footprint.Depth = 1;
        expSrc.PlacedFootprint.Footprint.RowPitch = 256;
        expCl->CopyTextureRegion(&expDst, 0, 0, 0, &expSrc, nullptr);
        D3D12_RESOURCE_BARRIER expToSrv = expToCopy;
        expToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        expToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        expCl->ResourceBarrier(1, &expToSrv);
        Check(expCl->Close(), "close exp list");
        ID3D12CommandList *expLs[] = {expCl};
        submitQueue->ExecuteCommandLists(1, expLs);
        WaitQueue(device, submitQueue);
        expCl->Release();
        expAlloc->Release();
        expUpload->Release();

        frame.exposure = exposureTex;
        frame.exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.pre_exposure = 2.0f;
        frame.exposure_scale = 0.5f;
    }

    if(temporalTest)
    {
        ID3D12Resource *motion=nullptr,*depth=nullptr;
        auto gd=td;gd.Format=DXGI_FORMAT_R32G32_FLOAT;gd.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(temporalGuides) {gd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;gd.MipLevels=2;}
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&gd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr,IID_PPV_ARGS(&motion)),"temporal motion");
        gd.Format=DXGI_FORMAT_R32_FLOAT;
        if(temporalGuides) {gd.Format=DXGI_FORMAT_R32G8X24_TYPELESS;gd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;}
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&gd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr,IID_PPV_ARGS(&depth)),"temporal depth");
        frame.motion=motion;frame.depth=depth;
        frame.motion_width=frame.color_width;frame.motion_height=frame.color_height;
        frame.motion_scale_x=frame.motion_scale_y=1;
        frame.motion_state=frame.depth_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if(temporalGuides) {
            ID3D12DescriptorHeap *heap=nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
            Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"temporal DSV heap");
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            const auto handle=heap->GetCPUDescriptorHandleForHeapStart();device->CreateDepthStencilView(depth,&dv,handle);
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={depth,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE};
            list->ResourceBarrier(1,&b);list->ClearDepthStencilView(handle,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,.5f,173,0,nullptr);
            b.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1,&b);Check(list->Close(),"temporal depth close");
            ID3D12CommandList *init[]={list};queue->ExecuteCommandLists(1,init);WaitQueue(device,queue);heap->Release();
        }
        const UINT guides=LMXXF_NR_TEMPORAL_INPUTS_VALID|LMXXF_NR_TEMPORAL_DEPTH_INVERTED;
        frame.temporal_flags=guides;
        UINT run=0, nextNativeSeed=0;
        const auto execute=[&](const char *expected) -> uint64_t {
            ++run;++frame.evaluate_sequence;
            ID3D12CommandAllocator *pa=nullptr,*ca=nullptr;
            ID3D12GraphicsCommandList *pc=nullptr,*cc=nullptr;
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&pa)),"temporal producer alloc");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&ca)),"temporal consumer alloc");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,pa,nullptr,IID_PPV_ARGS(&pc)),"temporal producer");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,ca,nullptr,IID_PPV_ARGS(&cc)),"temporal consumer");
            LmxxfNrJob tj{};tj.struct_size=sizeof(tj);
            const auto ok=[&](int rc,const char *step){if(rc!=LMXXF_NR_OK){char why[256]{};api.GetLastError(why,sizeof why);std::fprintf(stderr,"temporal %s rc=%d %s\n",step,rc,why);}Require(rc==LMXXF_NR_OK,step);};
            ok(api.PrepareFrame(ctx,&frame,&tj),"temporal PrepareFrame");
            char state[2048]{};api.GetStatus(ctx,state,sizeof state);
            std::printf("temporal run=%u %s\n",run,state);
            Require(std::strstr(state,expected)!=nullptr,"temporal effective state");
            if(nativeTemporalTest) {
                Require(std::strstr(state,"nativePost=1")!=nullptr,"native test runtime required");
                const char *seedText=std::strstr(state,"historySeed=");Require(seedText!=nullptr,"native seed diagnostic");
                const bool model=(frame.temporal_flags&LMXXF_NR_TEMPORAL_MODEL_HISTORY)!=0;
                const bool priming=std::strstr(state,"temporal=priming")!=nullptr;
                const bool active=std::strstr(state,"temporal=active")!=nullptr;
                if(priming||!active)nextNativeSeed=0;
                const unsigned wanted=model&&(priming||active)?nextNativeSeed++:1;
                Require(std::strtoul(seedText+12,nullptr,10)==wanted,"native seed progresses and resets with model history");
            }
            if(temporalGuides && frame.struct_size==sizeof(frame) && frame.motion==motion && frame.depth==depth) {
                Require(std::strstr(state,"/fmt2/dim3/array1/mips2/samples1/flags0")!=nullptr,"motion descriptor diagnostics");
                Require(std::strstr(state,"/fmt19/dim3/array1/mips2/samples1/flags2")!=nullptr,"depth descriptor diagnostics");
            }
            ok(api.RecordInputs(ctx,tj.handle,pc),"temporal inputs");Check(pc->Close(),"temporal producer close");
            // Same contract as the product: record the consumer before EnqueueHip.
            ok(api.RecordOutputs(ctx,tj.handle,cc),"temporal outputs");Check(cc->Close(),"temporal consumer close");
            ID3D12Fence *gate=nullptr;
            HANDLE enqueueDone=nullptr;
            std::thread releaseGate;
            bool enqueueBlocked=false;
            // After warmup, a CPU submission must not require producer GPU
            // completion. The watchdog releases even a broken runtime so the
            // regression fails cleanly without leaving the GPU queue blocked.
            if(blockedProducer && run>1) {
                Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"producer gate");
                Check(submitQueue->Wait(gate,1),"block producer GPU");
                enqueueDone=CreateEventW(nullptr,TRUE,FALSE,nullptr);Require(enqueueDone!=nullptr,"enqueue event");
                releaseGate=std::thread([&]{
                    enqueueBlocked=WaitForSingleObject(enqueueDone,2000)!=WAIT_OBJECT_0;
                    Check(gate->Signal(1),"release producer gate");
                });
            }
            ID3D12CommandList *ps[]={pc},*cs[]={cc};submitQueue->ExecuteCommandLists(1,ps);
            const auto enqueueStart=std::chrono::steady_clock::now();
            const int enqueueRc=api.EnqueueHip(ctx,tj.handle,submitQueue);
            const double enqueueMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-enqueueStart).count();
            if(gate) {SetEvent(enqueueDone);releaseGate.join();CloseHandle(enqueueDone);}
            std::printf("enqueue run=%u blocked=%u cpu_ms=%.3f\n",run,unsigned(enqueueBlocked),enqueueMs);
            ok(enqueueRc,"temporal enqueue");submitQueue->ExecuteCommandLists(1,cs);
            ok(api.Retire(ctx,tj.handle),"temporal retire");WaitQueue(device,submitQueue);
            if(gate)gate->Release();
            const auto hash=HashTexture(device,submitQueue,static_cast<ID3D12Resource*>(tj.private_output));
            std::printf("temporal_hash run=%u value=%016llx\n",run,static_cast<unsigned long long>(hash));
            Require(!enqueueBlocked,"EnqueueHip must return while producer GPU is blocked");
            pc->Release();cc->Release();pa->Release();ca->Release();return hash;
        };
        const auto baseline=execute("temporal=off");
        frame.temporal_flags=guides|LMXXF_NR_TEMPORAL_MODEL_HISTORY;
        const auto primingHash=execute("temporal=priming");
        if(!nativeTemporalTest)Require(primingHash==baseline,"history priming preserves baseline pixels");
        const auto historyHash=execute("temporal=active modelHistory=1");
        Require(historyHash!=baseline,"history reaches network math");
        execute("temporal=active modelHistory=1");
        frame.temporal_flags|=LMXXF_NR_TEMPORAL_RESET;
        Require(execute("temporal=priming")==primingHash,"reset restores first model frame");
        frame.temporal_flags=guides|LMXXF_NR_TEMPORAL_MODEL_HISTORY;execute("temporal=active modelHistory=1");
        frame.temporal_flags=guides;frame.output_smoothing=.25f;
        Require(execute("temporal=priming")==baseline,"smoothing-only priming preserves baseline");
        execute("temporal=active modelHistory=0 smoothing=0.25");
        frame.temporal_flags|=LMXXF_NR_TEMPORAL_MODEL_HISTORY;execute("temporal=priming");execute("temporal=active modelHistory=1 smoothing=0.25");
        frame.temporal_flags=guides;frame.output_smoothing=0;
        Require(execute("temporal=off")==baseline,"off restores baseline after both modes");
        frame.temporal_flags|=LMXXF_NR_TEMPORAL_MODEL_HISTORY;execute("temporal=priming");
        Require(api.ResetHistory(ctx)==LMXXF_NR_OK,"ResetHistory API");
        Require(execute("temporal=priming")==primingHash,"ResetHistory drops prior frame");
        frame.evaluate_sequence+=3;execute("temporal=priming");execute("temporal=active modelHistory=1");
        frame.motion=nullptr;Require(execute("temporal=missing-guides")==baseline,"missing motion preserves baseline");
        frame.motion=motion;execute("temporal=priming");execute("temporal=active modelHistory=1");
        if(temporalGuides) {
            ID3D12Resource *badMotion=nullptr;
            gd.Format=DXGI_FORMAT_R8G8_UNORM;gd.Flags=D3D12_RESOURCE_FLAG_NONE;
            Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&gd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr,IID_PPV_ARGS(&badMotion)),"unsupported temporal motion");
            frame.motion=badMotion;
            Require(execute("temporal=motion-format")==baseline,"unsupported motion reports reason and keeps baseline");
            badMotion->Release();
            gd.Format=DXGI_FORMAT_R32G32_FLOAT;gd.DepthOrArraySize=2;
            Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&gd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr,IID_PPV_ARGS(&badMotion)),"array temporal motion");
            frame.motion=badMotion;
            Require(execute("temporal=motion-array")==baseline,"unsupported texture array reports exact reason");
            badMotion->Release();frame.motion=motion;execute("temporal=priming");execute("temporal=active modelHistory=1");
        }
        for(UINT size:{LMXXF_NR_FRAME_INFO_V1_SIZE,LMXXF_NR_FRAME_INFO_EXPOSURE_SIZE,LMXXF_NR_FRAME_INFO_PAPER_WHITE_SIZE}) {
            frame.struct_size=size;Require(execute("temporal=legacy-frame")==baseline,"legacy frame ignores temporal tail");
        }
        frame.struct_size=sizeof(frame);
        Require(api.Destroy(ctx)==LMXXF_NR_OK,"temporal destroy");motion->Release();depth->Release();
        color->Release();list->Release();alloc->Release();queue->Release();device->Release();
        FreeLibrary(dll);
        std::printf("temporal integrated: PASS (%u frames, baseline=%016llx history=%016llx)\n",run,
                    static_cast<unsigned long long>(baseline),static_cast<unsigned long long>(historyHash));
        return 0;
    }

    LmxxfNrJob job {};
    job.struct_size = sizeof(job);
    const int32_t pfr = api.PrepareFrame(ctx, &frame, &job);
    if (pfr != LMXXF_NR_OK)
    {
        char err[256] {};
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "PrepareFrame rc=%d err=%s\n", pfr, err);
        Require(false, "PrepareFrame");
    }
    Require(job.private_output != nullptr, "private_output");
    Require(api.RecordInputs(ctx, job.handle, list) == LMXXF_NR_OK, "RecordInputs");
    Check(list->Close(), "close producer");
    ID3D12CommandList *lists[] = {list};
    submitQueue->ExecuteCommandLists(1, lists);
    const int32_t hip = api.EnqueueHip(ctx, job.handle, submitQueue);
    char err[256] {};
    api.GetLastError(err, sizeof err);
    std::printf("EnqueueHip rc=%d last_error=%s\n", hip, err);
    Require(queueMismatch ? (hip == LMXXF_NR_OK && std::strstr(err, "output zeroed"))
                          : (hip == LMXXF_NR_OK || hip == LMXXF_NR_UNAVAILABLE),
            "EnqueueHip result");

    // EnqueueHip only schedules GPU work; wait before Reset of the same allocator.
    {
        ID3D12Fence *fence = nullptr;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Check(submitQueue->Signal(fence, 1), "signal");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(ev != nullptr, "event");
        Check(fence->SetEventOnCompletion(1, ev), "set event");
        WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        fence->Release();
    }
    // Prefer a fresh allocator for outputs so producer storage is never Reset early.
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&outAlloc)), "out alloc");
    Check(list->Reset(outAlloc, nullptr), "reset list on out alloc");
    const int32_t outs = api.RecordOutputs(ctx, job.handle, list);
    Require(outs == LMXXF_NR_OK || outs == LMXXF_NR_FAILED, "RecordOutputs called");
    if (outs == LMXXF_NR_OK)
    {
        Check(list->Close(), "close outputs");
        submitQueue->ExecuteCommandLists(1, lists);
        Check(api.Retire(ctx, job.handle) == LMXXF_NR_OK, "Retire");
    }

    if (queueMismatch)
        Require(outs == LMXXF_NR_OK, "RecordOutputs after zero fallback");

    if (subrect)
    {
        // A subrect smaller than its buffer must NOT rebuild the codec chain. The old check
        // compared the remembered SUBRECT against the ALLOCATION, so this second frame - same
        // subrect, same buffer - would have taken geoChanged and paid a full rebuild.
        char stBefore[256] {}, stAfter[256] {};
        if (api.GetStatus)
            api.GetStatus(ctx, stBefore, sizeof stBefore);
        LmxxfNrFrameInfo again = frame;
        LmxxfNrJob againJob {};
        againJob.struct_size = sizeof(againJob);
        const int32_t againRc = api.PrepareFrame(ctx, &again, &againJob);
        std::printf("subrect second frame rc=%d\n", againRc);
        Require(againRc == LMXXF_NR_OK && againJob.handle != nullptr, "second subrect frame runs");
        // GetLastError BEFORE GetStatus: GetStatus clears the last-error slot.
        char why[320] {};
        if (api.GetLastError)
            api.GetLastError(why, sizeof why);
        std::printf("subrect rebuild reason: %s\n", why);
        if (api.GetStatus)
            api.GetStatus(ctx, stAfter, sizeof stAfter);
        auto recreates = [](const char *st) -> long {
            const char *p = std::strstr(st, "recreates=");
            return p ? std::strtol(p + 10, nullptr, 10) : -1;
        };
        std::printf("recreates before=%ld after=%ld (%s)\n", recreates(stBefore), recreates(stAfter), stAfter);
        Require(recreates(stBefore) == recreates(stAfter),
                "subrect != allocation must not rebuild the codec chain");
        Require(api.CancelUnsubmitted(ctx, againJob.handle) == LMXXF_NR_OK, "cancel second subrect frame");
    }

    if(test17Mode==2&&job.private_output){
        Require(HashTexture(device,submitQueue,static_cast<ID3D12Resource*>(job.private_output))==
                HashTexture(device,submitQueue,static_cast<ID3D12Resource*>(frame.color)),"test17 identity exact original after HIP execution");
        std::printf("test17 identity: original preserved with inference executed\n");
    }

    if (useExposure && !badExposure)
    {
        // The runtime must bind its own stable copy, not the game's texture. Handing it a NEW
        // allocation of the same format has to be free: the codec bakes the exposure SRV at
        // Create, so a pointer-sensitivity here would rebuild the whole chain every frame. The
        // observable is GetStatus's recreate count, which must not move.
        char stBefore[256] {}, stAfter[256] {};
        if (api.GetStatus)
            api.GetStatus(ctx, stBefore, sizeof stBefore);
        D3D12_RESOURCE_DESC ed = exposureTex->GetDesc();
        ID3D12Resource *rotated = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&rotated)),
              "rotated exposure");
        LmxxfNrFrameInfo rotFrame = frame;
        rotFrame.exposure = rotated;
        LmxxfNrJob rotJob {};
        rotJob.struct_size = sizeof(rotJob);
        const int32_t rotRc = api.PrepareFrame(ctx, &rotFrame, &rotJob);
        std::printf("exposure pointer rotation rc=%d\n", rotRc);
        Require(rotRc == LMXXF_NR_OK && rotJob.handle != nullptr, "new exposure allocation still runs");
        if (api.GetStatus)
            api.GetStatus(ctx, stAfter, sizeof stAfter);
        auto recreates = [](const char *st) -> long {
            const char *p = std::strstr(st, "recreates=");
            return p ? std::strtol(p + 10, nullptr, 10) : -1;
        };
        std::printf("recreates before=%ld after=%ld (%s)\n", recreates(stBefore), recreates(stAfter), stAfter);
        Require(recreates(stBefore) == recreates(stAfter),
                "a new exposure allocation must not rebuild the codec chain");
        Require(api.CancelUnsubmitted(ctx, rotJob.handle) == LMXXF_NR_OK, "cancel rotated frame");
        rotated->Release();
    }

    if (outputHash && outs == LMXXF_NR_OK && job.private_output)
        std::printf("output_hash=%016llx %ux%u\n",
                    static_cast<unsigned long long>(HashTexture(device, submitQueue,
                                                                 static_cast<ID3D12Resource *>(job.private_output),
                                                                 scale16 ? 4u : 0u)),
                    frame.color_width, frame.color_height);

    if (useExposure && !badExposure)
    {
        // A change of the exposure FORMAT is the one case that must rebuild: the stable copy has
        // to match its source for CopyTextureRegion. The old copy is still bound by the live
        // codecs and was read by the frame above, so the runtime may only free it after its
        // drain. This runs that path (R32 -> R16) and then checks the new binding is stable.
        auto recreates = [&]() -> long {
            char st[256] {};
            if (api.GetStatus)
                api.GetStatus(ctx, st, sizeof st);
            const char *p = std::strstr(st, "recreates=");
            return p ? std::strtol(p + 10, nullptr, 10) : -1;
        };
        D3D12_RESOURCE_DESC ed = exposureTex->GetDesc();
        ed.Format = DXGI_FORMAT_R16_FLOAT;
        ID3D12Resource *halfExposure = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&halfExposure)),
              "R16 exposure");
        const long before = recreates();
        for (int pass = 0; pass < 2; ++pass)
        {
            LmxxfNrFrameInfo halfFrame = frame;
            halfFrame.exposure = halfExposure;
            LmxxfNrJob halfJob {};
            halfJob.struct_size = sizeof(halfJob);
            const int32_t halfRc = api.PrepareFrame(ctx, &halfFrame, &halfJob);
            const long now = recreates();
            std::printf("exposure format change pass %d rc=%d recreates %ld->%ld\n", pass, halfRc, before, now);
            Require(halfRc == LMXXF_NR_OK && halfJob.handle != nullptr, "exposure format change still runs");
            Require(now == before + 1, pass == 0 ? "exposure format change rebuilds once"
                                                 : "same format afterwards does not rebuild again");
            Require(api.CancelUnsubmitted(ctx, halfJob.handle) == LMXXF_NR_OK, "cancel format-change frame");
        }
        halfExposure->Release();

        // Exercise actual submissions across game -> auto -> manual -> game
        // exposure changes. A binding change may rebuild codecs, never the model.
        auto bridgeCreates = [&]() -> long {
            char st[768] {};
            Require(api.GetStatus(ctx, st, sizeof(st)) == LMXXF_NR_OK, "exposure status");
            std::printf("exposure transition status: %s\n", st);
            const char *p = std::strstr(st, "bridgeCreates=");
            return p ? std::strtol(p + 14, nullptr, 10) : -1;
        };
        Require(bridgeCreates() == 1, "R32/R16 exposure changes retain the original model");
        uint64_t exposureHashes[3] {};
        for (unsigned pass = 0; pass < 6; ++pass)
        {
            Require(api.Drain(ctx) == LMXXF_NR_OK, "drain previous exposure frame");
            UploadColorPattern(device, submitQueue, color);
            Check(alloc->Reset(), "exposure producer allocator reset");
            Check(list->Reset(alloc, nullptr), "exposure producer list reset");
            LmxxfNrFrameInfo changing = frame;
            changing.exposure = pass % 3 == 0 ? exposureTex : nullptr;
            changing.flags = pass % 3 == 1 ? LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE : 0;
            LmxxfNrJob changingJob {}; changingJob.struct_size = sizeof(changingJob);
            Require(api.PrepareFrame(ctx, &changing, &changingJob) == LMXXF_NR_OK, "prepare exposure transition");
            Require(bridgeCreates() == 1, "exposure transition must not reload/warm model");
            Require(api.RecordInputs(ctx, changingJob.handle, list) == LMXXF_NR_OK, "exposure inputs");
            Check(list->Close(), "exposure producer close");
            submitQueue->ExecuteCommandLists(1, lists);
            Require(api.EnqueueHip(ctx, changingJob.handle, submitQueue) == LMXXF_NR_OK, "exposure enqueue");
            char why[256] {}; api.GetLastError(why, sizeof(why));
            Require(!std::strstr(why, "output zeroed"), "no recovery output during exposure transition");
            WaitQueue(device, submitQueue);
            Check(outAlloc->Reset(), "exposure output allocator reset");
            Check(list->Reset(outAlloc, nullptr), "exposure output reset");
            Require(api.RecordOutputs(ctx, changingJob.handle, list) == LMXXF_NR_OK, "exposure outputs");
            Check(list->Close(), "exposure output close");
            submitQueue->ExecuteCommandLists(1, lists);
            Require(api.Retire(ctx, changingJob.handle) == LMXXF_NR_OK, "exposure retire");
            Require(api.Drain(ctx) == LMXXF_NR_OK, "exposure GPU completion");
            Check(device->GetDeviceRemovedReason(), "exposure device healthy");
            const uint64_t hash = HashTexture(device, submitQueue,
                                             static_cast<ID3D12Resource *>(changingJob.private_output));
            if (pass < 3) exposureHashes[pass] = hash;
            else Require(hash == exposureHashes[pass % 3], "returning to exposure mode preserves pixel output");
            std::printf("exposure transition pass=%u output_hash=%016llx\n", pass,
                        static_cast<unsigned long long>(hash));
        }
    }
    ID3D12Resource *resizedColor = nullptr;
    if (resize)
    {
        td.Width = 1600;
        td.Height = 900;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&resizedColor)), "resized color");
        frame.color_width = 1600;
        frame.color_height = 900;
        frame.color = resizedColor;
        LmxxfNrJob resizedJob {};
        resizedJob.struct_size = sizeof(resizedJob);
        const int32_t resizeRc = api.PrepareFrame(ctx, &frame, &resizedJob);
        if (resizeRc != LMXXF_NR_OK)
        {
            char resizeErr[256] {};
            api.GetLastError(resizeErr, sizeof resizeErr);
            std::fprintf(stderr, "resized PrepareFrame rc=%d err=%s\n", resizeRc, resizeErr);
        }
        Require(resizeRc == LMXXF_NR_OK && resizedJob.private_output != nullptr,
                "resized PrepareFrame after default-path teardown");
        char resizeStatus[768] {};
        Require(api.GetStatus(ctx, resizeStatus, sizeof(resizeStatus)) == LMXXF_NR_OK, "resize status");
        Require(std::strstr(resizeStatus, "bridgeCreates=2 ") != nullptr,
                "geometry change still rebuilds the model");
        Require(api.CancelUnsubmitted(ctx, resizedJob.handle) == LMXXF_NR_OK,
                "cancel unsubmitted resized frame");
    }

    Require(api.Destroy(ctx) == LMXXF_NR_OK, "Destroy");
    if (exposureTex)
        exposureTex->Release();
    if (resizedColor)
        resizedColor->Release();
    color->Release();
    list->Release();
    if (outAlloc)
        outAlloc->Release();
    alloc->Release();
    if (queue2)
        queue2->Release();
    queue->Release();
    device->Release();
    if (adapter)
        adapter->Release();
    FreeLibrary(dll);
    std::printf("lmxxf_nr_gpu: ok%s\n", queueMismatch ? " (queue mismatch fallback)" :
                                         resize ? " (default-path resize teardown)" :
                                         ultrawide ? " (2024x848 ultrawide admitted)" : "");
    return 0;
}
