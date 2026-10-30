// FSR-RR offline chain: minimal RR dispatch on real hardware, no game.
// Loads the product's denoiser DLL, creates an RR context, builds synthetic
// inputs, and dispatches the denoiser outside a game process.
//
// Modes:
//   default: correctness (assert return codes, finite output)
//   PERF_MODE: timing loop (P4 baseline matrix)

#ifdef PERF_MODE
#define CHAIN_NAME "perf_chain"
#else
#define CHAIN_NAME "offline_chain"
#endif

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <cassert>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#include <fsr-rr/ffx_denoiser.h>
#include <ffx_api.h>
#include <ffx_api_dx12.h>

// ---- Utility ----
template <typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    T* get() const { return p; }
    operator bool() const { return p != nullptr; }
};

static bool CheckHR(HRESULT hr, const char* msg)
{
    if (FAILED(hr)) { fprintf(stderr, "FATAL: %s (hr=0x%08X)\n", msg, (unsigned) hr); return false; }
    return true;
}

// ---- Synthetic input (P2-3) ----
enum class SceneType : int
{
    FlatPlane, Checkerboard, MovingSquares, OcclusionReveal,
    DarkScene, Highlights, ZeroRoughness, HitDistanceVar, Count
};

static const char* SceneName(SceneType t)
{
    static const char* names[] = { "flat_plane", "checkerboard", "moving_squares", "occlusion_reveal",
                                    "dark_scene", "highlights", "zero_roughness", "hit_distance_var" };
    return names[int(t)];
}

// Half-float conversion (round-to-nearest-even approximation)
static uint16_t F2H(float v)
{
    if (v <= 0) return 0;
    if (v >= 65504.f) return 0x7BFF;
    uint32_t bits; memcpy(&bits, &v, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exp = int32_t((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (bits >> 13) & 0x3FF;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7C00);
    return uint16_t(sign | (uint32_t(exp) << 10) | mant);
}

struct SynthTex
{
    std::vector<uint8_t> data;
    DXGI_FORMAT format;
    uint32_t bytesPerPixel;
    uint32_t width, height;

    void Allocate(DXGI_FORMAT fmt, uint32_t bpp, uint32_t w, uint32_t h)
    {
        format = fmt; bytesPerPixel = bpp; width = w; height = h;
        data.resize(size_t(w) * h * bpp);
    }
};

static void GenerateFrame(std::vector<SynthTex>& texs, SceneType scene, uint32_t frameIdx,
                          uint32_t w, uint32_t h)
{
    const float t = float(frameIdx);
    auto& color = texs[0];   // RGBA16F
    auto& depth = texs[1];   // R32F
    auto& motion = texs[2];  // RG16F
    auto& normals = texs[3]; // RGBA16F (world normal + roughness)

    auto* c = reinterpret_cast<uint16_t*>(color.data.data());
    auto* d = reinterpret_cast<float*>(depth.data.data());
    auto* m = reinterpret_cast<uint16_t*>(motion.data.data());
    auto* n = reinterpret_cast<uint16_t*>(normals.data.data());

    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            const size_t i4 = size_t(y) * w + x;
            const float fx = float(x) / w, fy = float(y) / h;
            float r = 0.5f, g = 0.5f, b = 0.5f;
            float dv = 1.0f - fy * 0.8f; // reversed Z
            float mvx = 0, mvy = 0;
            float nx = 0, ny = 0, nz = 1, rough = 0.5f;

            switch (scene)
            {
            case SceneType::FlatPlane: r = fx; g = fy; b = 0.3f; break;
            case SceneType::Checkerboard:
            {
                const int ch = ((x / 16) + (y / 16)) & 1;
                r = g = b = ch ? 1.0f : 0.0f;
            } break;
            case SceneType::MovingSquares:
            {
                const float sp = 0.001f + fx * 0.005f;
                const float ox = fmodf(t * sp + fx * 4, 1);
                const float oy = fmodf(t * sp * 0.7f + fy * 4, 1);
                const bool sq = ox > 0.3f && ox < 0.7f && oy > 0.3f && oy < 0.7f;
                r = g = b = sq ? 0.9f : 0.2f;
                mvx = sp * w * 0.5f; mvy = sp * h * 0.3f;
            } break;
            case SceneType::OcclusionReveal:
            {
                const float rv = sinf(t * 0.05f) * 0.5f + 0.5f;
                const bool occ = fx < rv;
                dv = occ ? 0.9f : 0.3f;
                r = g = b = occ ? 0.2f : 0.8f;
            } break;
            case SceneType::DarkScene: r = g = b = 0.001f; rough = 0.9f; break;
            case SceneType::Highlights:
                r = fx > 0.5f ? 8.0f : 0.1f; g = r * 0.8f; b = r * 0.6f;
                if (fx > 0.5f) dv = 0.95f;
                break;
            case SceneType::ZeroRoughness:
                r = fx; g = fy; b = 0.5f;
                rough = (fx > 0.3f && fx < 0.7f) ? 0.0f : 0.5f;
                break;
            case SceneType::HitDistanceVar: r = fy; g = 0.5f; b = 0.5f; dv = 1.0f - fy * 0.99f; break;
            default: break;
            }

            const size_t ic = i4 * 4, im = i4 * 2;
            c[ic] = F2H(r); c[ic+1] = F2H(g); c[ic+2] = F2H(b); c[ic+3] = F2H(1.0f);
            d[i4] = dv;
            m[im] = F2H(mvx); m[im+1] = F2H(mvy);
            n[ic] = F2H(nx); n[ic+1] = F2H(ny); n[ic+2] = F2H(nz); n[ic+3] = F2H(rough);
        }
    }
}

// ---- Denoiser DLL function pointers ----
struct DenoiserApi
{
    HMODULE dll = nullptr;
    ffxReturnCode_t (STDMETHODCALLTYPE *CreateContext)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*) = nullptr;
    ffxReturnCode_t (STDMETHODCALLTYPE *DestroyContext)(ffxContext*, ffxDestroyContextDesc*) = nullptr;
    ffxReturnCode_t (STDMETHODCALLTYPE *Dispatch)(ffxContext*, ffxCommandList, ffxDispatchDescHeader*) = nullptr;
    ffxReturnCode_t (STDMETHODCALLTYPE *Query)(ffxContext*, ffxQueryDescHeader*) = nullptr;

    bool Load(const std::wstring& path)
    {
        dll = LoadLibraryW(path.c_str());
        if (!dll) return false;
        CreateContext = (ffxReturnCode_t (STDMETHODCALLTYPE*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*))GetProcAddress(dll, "ffxCreateContext");
        DestroyContext = (ffxReturnCode_t (STDMETHODCALLTYPE*)(ffxContext*, ffxDestroyContextDesc*))GetProcAddress(dll, "ffxDestroyContext");
        Dispatch = (ffxReturnCode_t (STDMETHODCALLTYPE*)(ffxContext*, ffxCommandList, ffxDispatchDescHeader*))GetProcAddress(dll, "ffxDispatch");
        Query = (ffxReturnCode_t (STDMETHODCALLTYPE*)(ffxContext*, ffxQueryDescHeader*))GetProcAddress(dll, "ffxQuery");
        return CreateContext && DestroyContext && Dispatch && Query;
    }
};

// ---- Main ----
int main(int argc, char** argv)
{
    const uint32_t renderW = 2560, renderH = 1440;
#ifdef PERF_MODE
    const uint32_t warmupFrames = 120;
    const uint32_t measuredFrames = 600;
#else
    const uint32_t warmupFrames = 30;
    const uint32_t measuredFrames = 60;
#endif

    fprintf(stderr, "[%s] render=%ux%u warmup=%u measured=%u\n", CHAIN_NAME, renderW, renderH, warmupFrames, measuredFrames);

    // 1. Load denoiser DLL
    std::filesystem::path exeDir = std::filesystem::path(argv[0]).parent_path();
    std::vector<std::wstring> candidates = {
        (exeDir / L"..\\..\\..\\OptiScaler-DLSSNR-PreSR-Multipass-main\\external\\FidelityFX-SDK-v2\\Kits\\FidelityFX\\signedbin\\amd_fidelityfx_denoiser_dx12.dll").wstring(),
        L"amd_fidelityfx_denoiser_dx12.dll",
    };
    DenoiserApi api;
    bool loaded = false;
    for (const auto& c : candidates)
    {
        if (std::filesystem::exists(c) && api.Load(c))
        {
            loaded = true;
            fprintf(stderr, "Loaded: %ls\n", c.c_str());
            break;
        }
    }
    if (!loaded) { fprintf(stderr, "SKIP: denoiser DLL not found\n"); return 0; }

    // 2. Create device on AMD adapter
    ComPtr<IDXGIFactory6> factory;
    if (!CheckHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory.p)), "CreateDXGIFactory2")) return 1;

    ComPtr<IDXGIAdapter4> adapter;
    for (UINT i = 0;; i++)
    {
        ComPtr<IDXGIAdapter4> a;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a.p))))
            break;
        DXGI_ADAPTER_DESC3 desc; a->GetDesc3(&desc);
        fprintf(stderr, "Adapter %u: %ls (vendor %04X)\n", i, desc.Description, desc.VendorId);
        if (desc.VendorId == 0x1002) { adapter.p = a.p; a.p = nullptr; break; }
    }
    if (!adapter) { fprintf(stderr, "SKIP: no AMD adapter\n"); return 0; }

    ComPtr<ID3D12Device> device;
    if (!CheckHR(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device.p)), "D3D12CreateDevice")) return 1;
    fprintf(stderr, "Device created\n");

    // 3. Queue + command list
    D3D12_COMMAND_QUEUE_DESC qd = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> queue;
    if (!CheckHR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p)), "CreateCommandQueue")) return 1;

    ComPtr<ID3D12CommandAllocator> alloc;
    if (!CheckHR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p)), "CreateCommandAllocator")) return 1;

    ComPtr<ID3D12GraphicsCommandList> cl;
    if (!CheckHR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, IID_PPV_ARGS(&cl.p)), "CreateCommandList")) return 1;

    // 4. Upload heap for synthetic data
    auto MakeUpload = [&](size_t size, const wchar_t* name) -> ComPtr<ID3D12Resource> {
        D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = UINT64(size), .Height = 1,
                                   .DepthOrArraySize = 1, .MipLevels = 1,
                                   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        ComPtr<ID3D12Resource> res;
        if (!CheckHR(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res.p)), "CreateUploadBuffer")) return {};
        return res;
    };

    // Synthetic input textures (default heap, GPU only)
    auto MakeTexture = [&](DXGI_FORMAT fmt, const wchar_t* name) -> ComPtr<ID3D12Resource> {
        D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                   .Width = renderW, .Height = renderH,
                                   .DepthOrArraySize = 1, .MipLevels = 1, .Format = fmt,
                                   .SampleDesc = { 1, 0 } };
        ComPtr<ID3D12Resource> res;
        if (!CheckHR(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res.p)), "CreateTexture")) return {};
        return res;
    };

    ComPtr<ID3D12Resource> texColor = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"color");
    ComPtr<ID3D12Resource> texDepth = MakeTexture(DXGI_FORMAT_R32_FLOAT, L"depth");
    ComPtr<ID3D12Resource> texMotion = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, L"motion");
    ComPtr<ID3D12Resource> texNormals = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"normals");
    if (!texColor || !texDepth || !texMotion || !texNormals) return 1;

    // Synthetic data generators
    std::vector<SynthTex> synth(4);
    synth[0].Allocate(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, renderW, renderH);
    synth[1].Allocate(DXGI_FORMAT_R32_FLOAT, 4, renderW, renderH);
    synth[2].Allocate(DXGI_FORMAT_R16G16_FLOAT, 4, renderW, renderH);
    synth[3].Allocate(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, renderW, renderH);

    // Upload buffers
    auto uploadColor = MakeUpload(synth[0].data.size(), L"upload_color");
    auto uploadDepth = MakeUpload(synth[1].data.size(), L"upload_depth");
    auto uploadMotion = MakeUpload(synth[2].data.size(), L"upload_motion");
    auto uploadNormals = MakeUpload(synth[3].data.size(), L"upload_normals");
    if (!uploadColor || !uploadDepth || !uploadMotion || !uploadNormals) return 1;

    // 5. Create RR context (matching product defaults: DirectDiffuse + IndirectSpecular)
    const uint32_t kRenderCeilingAlign = 8;
    const auto ceil8 = [&](uint32_t v) { return (v + kRenderCeilingAlign - 1) / kRenderCeilingAlign * kRenderCeilingAlign; };

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = device.get();

    ffxCreateContextDescDenoiser createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    createDesc.header.pNext = &backendDesc.header;
    createDesc.version = FFX_DENOISER_VERSION;
    createDesc.maxRenderSize = { ceil8(renderW), ceil8(renderH) };
    createDesc.signalFlags = FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    createDesc.checkerboardSignalFlags = FFX_DENOISER_SIGNAL_NONE;
    createDesc.flags = 0;

    ffxContext ctx = nullptr;
    auto ret = api.CreateContext(&ctx, &createDesc.header, nullptr);
    if (ret != FFX_API_RETURN_OK)
    {
        fprintf(stderr, "FAIL: CreateContext returned 0x%08X\n", (unsigned) ret);
        return 1;
    }
    fprintf(stderr, "RR context created\n");

    // 6. Signal descriptors (chained to dispatch desc)
    ffxDispatchDescDenoiserDirectDiffuse directDiffuse = {};
    directDiffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    directDiffuse.signal.input = ffxApiGetResourceDX12(texColor.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    directDiffuse.signal.output = ffxApiGetResourceDX12(texColor.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescDenoiserIndirectSpecular indirectSpecular = {};
    indirectSpecular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    indirectSpecular.signal.input = ffxApiGetResourceDX12(texColor.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    indirectSpecular.signal.output = ffxApiGetResourceDX12(texColor.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    // Chain: DirectDiffuse -> IndirectSpecular (ordered by type, matching product)
    directDiffuse.header.pNext = &indirectSpecular.header;

    // 7. Dispatch loop
    SceneType scene = SceneType::FlatPlane;
    LARGE_INTEGER qpcFreq, qpcStart, qpcEnd;
    QueryPerformanceFrequency(&qpcFreq);

    for (uint32_t f = 0; f < warmupFrames + measuredFrames; f++)
    {
        // Regenerate synthetic input (alternating scenes in perf mode)
        if (f == warmupFrames) QueryPerformanceCounter(&qpcStart);

        GenerateFrame(synth, scene, f, renderW, renderH);

        // Upload synthetic data to GPU
        {
            // Map upload buffers, copy, unmap
            auto Upload = [&](ComPtr<ID3D12Resource>& buf, const SynthTex& tex) {
                void* mapped; D3D12_RANGE r{0,0};
                buf->Map(0, &r, &mapped);
                memcpy(mapped, tex.data.data(), tex.data.size());
                buf->Unmap(0, nullptr);
            };
            Upload(uploadColor, synth[0]);
            Upload(uploadDepth, synth[1]);
            Upload(uploadMotion, synth[2]);
            Upload(uploadNormals, synth[3]);
        }

        // Record: copy → dispatch → close → execute → wait
        alloc->Reset();
        cl->Reset(alloc.get(), nullptr);

        // Copy upload → texture
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {}, dstLoc = {};
        auto CopyTex = [&](ComPtr<ID3D12Resource>& dst, ComPtr<ID3D12Resource>& upload, const SynthTex& tex, UINT rowPitch) {
            srcLoc.pResource = upload.get(); srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = 0;
            srcLoc.PlacedFootprint.Footprint.Format = tex.format;
            srcLoc.PlacedFootprint.Footprint.Width = tex.width;
            srcLoc.PlacedFootprint.Footprint.Height = tex.height;
            srcLoc.PlacedFootprint.Footprint.Depth = 1;
            srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
            dstLoc.pResource = dst.get(); dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        };
        // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)
        const UINT pitchRGBA16F = (renderW * 8 + 255) & ~255;
        const UINT pitchR32F = (renderW * 4 + 255) & ~255;
        const UINT pitchRG16F = (renderW * 4 + 255) & ~255;
        CopyTex(texColor, uploadColor, synth[0], pitchRGBA16F);
        CopyTex(texDepth, uploadDepth, synth[1], pitchR32F);
        CopyTex(texMotion, uploadMotion, synth[2], pitchRG16F);
        CopyTex(texNormals, uploadNormals, synth[3], pitchRGBA16F);

        // Transition to compute read
        D3D12_RESOURCE_BARRIER barriers[4] = {};
        ID3D12Resource* resources[] = { texColor.get(), texDepth.get(), texMotion.get(), texNormals.get() };
        for (int i = 0; i < 4; i++)
        {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = resources[i];
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        cl->ResourceBarrier(4, barriers);
        cl->Close();
        queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&cl.p));

        // Fence wait
        static ComPtr<ID3D12Fence> fence;
        static UINT64 fenceValue = 0;
        if (!fence)
        {
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p));
        }
        fenceValue++;
        queue->Signal(fence.get(), fenceValue);
        while (fence->GetCompletedValue() < fenceValue)
            Sleep(0);

        // Dispatch denoiser
        ffxDispatchDescDenoiser desc = {};
        desc.commandList = cl.get();
        desc.motionVectorScale = { 1, 1, 1 };
        desc.cameraPositionDelta = { 0, 0, 0 };
        // Identity view/projection for synthetic test
        desc.view = { .row0 = {1,0,0,0}, .row1 = {0,1,0,0}, .row2 = {0,0,1,0}, .row3 = {0,0,0,1} };
        desc.projection = { .row0 = {1,0,0,0}, .row1 = {0,1,0,0}, .row2 = {0,0,1,0}, .row3 = {0,0,0,1} };
        desc.linearDepthBounds = { 0.2f, 65504.0f };
        desc.renderSize = { renderW, renderH };
        desc.frameIndex = f;
        desc.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;
        desc.header.pNext = &directDiffuse.header;

        ret = api.Dispatch(ctx, cl.get(), &desc.header);
        if (ret != FFX_API_RETURN_OK)
        {
            fprintf(stderr, "FAIL: Dispatch frame %u returned 0x%08X\n", f, (unsigned) ret);
            api.DestroyContext(&ctx, nullptr);
            return 1;
        }

        if (f % 60 == 0)
            fprintf(stderr, "  frame %u/%u dispatch OK\n", f, warmupFrames + measuredFrames);
    }

    QueryPerformanceCounter(&qpcEnd);
    const double totalSec = double(qpcEnd.QuadPart - qpcStart.QuadPart) / qpcFreq.QuadPart;
    const double fps = measuredFrames / totalSec;
    fprintf(stderr, "[%s] %u frames in %.3fs = %.1f fps\n", CHAIN_NAME, measuredFrames, totalSec, fps);

    // Cleanup
    api.DestroyContext(&ctx, nullptr);
    fprintf(stderr, "[%s] PASS\n", CHAIN_NAME);
    return 0;
}
