#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
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
    WaitForSingleObject(ev, 30000);
    CloseHandle(ev);
    fence->Release();
}

// Deterministic, non-constant pattern so an output hash can tell "same math" from
// "different math". RGB9E5 words use exponent 15 with varying 9-bit mantissas; that
// format has no Inf/NaN, so any bit pattern is a finite value.
static void FillRow(unsigned char *dst, UINT y, UINT w, bool rgb9e5)
{
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
        else
        {
            // 8 bytes per pixel: four FP16 channels (NOT four float32). Exponent 12..15
            // keeps every value finite; 0x3C00 is 1.0f in FP16.
            const UINT16 h[4] = {
                static_cast<UINT16>(((12u + (mR >> 7)) << 10) | (mR & 0x3FFu)),
                static_cast<UINT16>(((12u + (mG >> 7)) << 10) | (mG & 0x3FFu)),
                static_cast<UINT16>(((12u + (mB >> 7)) << 10) | (mB & 0x3FFu)),
                0x3C00u
            };
            std::memcpy(dst + size_t(x) * 8, h, sizeof h);
        }
    }
}

// Fills `tex` with FillRow. `tex` must start and end in NON_PIXEL_SHADER_RESOURCE.
static void UploadColorPattern(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *tex, bool rgb9e5)
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
        FillRow(base + size_t(y) * fp.Footprint.RowPitch, y, w, rgb9e5);
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
static uint64_t HashTexture(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *tex)
{
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        std::fprintf(stderr, "output_hash: output is not a texture; skipped\n");
        return 0;
    }
    const UINT w = static_cast<UINT>(td.Width), h = td.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT numRows = 0;
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, nullptr, &total);

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

    const UINT bytesPerPixel = (td.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) ? 4u : 8u;
    const UINT64 rowBytes = UINT64(w) * bytesPerPixel;
    uint64_t hsh = 1469598103934665603ull;
    void *mapped = nullptr;
    Check(rb->Map(0, nullptr, &mapped), "map readback");
    const auto *base = static_cast<const unsigned char *>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const unsigned char *row = base + size_t(y) * fp.Footprint.RowPitch;
        for (UINT64 i = 0; i < rowBytes; ++i)
        {
            hsh ^= row[i];
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
    bool queueMismatch = false, resize = false, rgb9e5 = false, outputHash = false, rejectFormats = false,
         useExposure = false;
    for (int i = 3; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--queue-mismatch"))
            queueMismatch = true;
        else if (!std::strcmp(argv[i], "--resize"))
            resize = true;
        else if (!std::strcmp(argv[i], "--rgb9e5"))
            rgb9e5 = outputHash = true;
        else if (!std::strcmp(argv[i], "--output-hash"))
            outputHash = true;
        else if (!std::strcmp(argv[i], "--reject-formats"))
            rejectFormats = true;
        else if (!std::strcmp(argv[i], "--exposure"))
            useExposure = outputHash = true;
        else
        {
            std::fprintf(stderr,
                         "usage: lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <assets_dir> "
                         "[--queue-mismatch|--resize] [--rgb9e5] [--output-hash] [--reject-formats]\n");
            return 2;
        }
    }
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <assets_dir> "
                     "[--queue-mismatch|--resize] [--rgb9e5] [--output-hash] [--reject-formats]\n");
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
    Require(caps.hip_ready == 0, "hip_ready stays 0");
    Require(caps.graph_supported == 0, "graph off");

    IDXGIFactory4 *factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            break;
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    Require(device != nullptr, "AMD D3D12 device");

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
    td.Width = 1920;
    td.Height = 1080;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = rgb9e5 ? DXGI_FORMAT_R9G9B9E5_SHAREDEXP : DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    // RE9's scene colour is RGB9E5 and is only ever read (SRV), so do not request a UAV.
    td.Flags = rgb9e5 ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *color = nullptr;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&color)),
          "color");
    if (outputHash)
        UploadColorPattern(device, submitQueue, color, rgb9e5);

    const std::wstring modules = Widen(argv[2]);
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modules.c_str();
    info.flags = queueMismatch ? LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK : 0;
    void *ctx = nullptr;
    Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create");
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
    frame.color_width = 1920;
    frame.color_height = 1080;
    frame.color = color;
    frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // An unsupported colour must be a retryable contract rejection, not a poisoned session.
    // Poisoning is what made RE9's RGB9E5 failure permanent and left no clue in the log.
    if (rejectFormats)
    {
        const DXGI_FORMAT kBadFormats[] = {DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           DXGI_FORMAT_R10G10B10A2_UNORM,
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
        // Oversized input with LmxxfFitLarge off is the documented limit; same contract.
        D3D12_RESOURCE_DESC bigDesc = td;
        bigDesc.Width = 2560;
        bigDesc.Height = 1440;
        ID3D12Resource *bigTex = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bigDesc,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&bigTex)),
              "oversized colour");
        LmxxfNrFrameInfo bigFrame = frame;
        bigFrame.color = bigTex;
        bigFrame.color_width = 2560;
        bigFrame.color_height = 1440;
        LmxxfNrJob bigJob {};
        bigJob.struct_size = sizeof(bigJob);
        const int32_t bigRc = api.PrepareFrame(ctx, &bigFrame, &bigJob);
        char bigErr[256] {};
        api.GetLastError(bigErr, sizeof bigErr);
        std::printf("reject 2560x1440 rc=%d err=%s\n", bigRc, bigErr);
        Require(bigRc == LMXXF_NR_INVALID_ARGUMENT, "oversized input -> INVALID_ARGUMENT");
        Require(std::strstr(bigErr, "outside admitted geometry") != nullptr, "geometry reason named");
        bigTex->Release();

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
        ed.Width = 2;
        ed.Height = 2;
        ed.DepthOrArraySize = ed.MipLevels = 1;
        ed.Format = DXGI_FORMAT_R32_FLOAT;
        ed.SampleDesc.Count = 1;
        ID3D12Resource *badExp = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ed,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              nullptr, IID_PPV_ARGS(&badExp)),
              "bad exposure");
        LmxxfNrFrameInfo badFrame = frame;
        badFrame.exposure = badExp;
        LmxxfNrJob badJob {};
        badJob.struct_size = sizeof(badJob);
        const int32_t badRc = api.PrepareFrame(ctx, &badFrame, &badJob);
        char badErr[256] {};
        api.GetLastError(badErr, sizeof badErr);
        std::printf("reject 2x2 exposure rc=%d err=%s\n", badRc, badErr);
        Require(badRc == LMXXF_NR_INVALID_ARGUMENT, "non-1x1 exposure -> INVALID_ARGUMENT");
        badExp->Release();

        ed.Width = ed.Height = 1;
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

    if (outputHash && outs == LMXXF_NR_OK && job.private_output)
        std::printf("output_hash=%016llx %ux%u\n",
                    static_cast<unsigned long long>(HashTexture(device, submitQueue,
                                                                 static_cast<ID3D12Resource *>(job.private_output))),
                    frame.color_width, frame.color_height);

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
                                         resize ? " (default-path resize teardown)" : "");
    return 0;
}
