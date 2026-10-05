#include "nr_effects_test_utils.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/PostSr.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/SrOutputExtent.h"

struct OutputParameters
{
    unsigned int outWidth = 1920, outHeight = 1080;
    std::optional<unsigned int> dynamicWidth, dynamicHeight;
    NVSDK_NGX_Result Get(const char* key, unsigned int* value) const
    {
        const std::string_view name(key);
        if (name == NVSDK_NGX_Parameter_OutWidth) *value = outWidth;
        else if (name == NVSDK_NGX_Parameter_OutHeight) *value = outHeight;
        else if (name == "FSR.upscaleSize.width" && dynamicWidth) *value = *dynamicWidth;
        else if (name == "FSR.upscaleSize.height" && dynamicHeight) *value = *dynamicHeight;
        else return NVSDK_NGX_Result_Fail;
        return NVSDK_NGX_Result_Success;
    }
};

static void OutputExtentPolicy()
{
    OutputParameters params;
    auto extent = DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 3840, 2160);
    Require(extent && extent->width == 3840 && extent->height == 2160,
            "4K SR must not inherit 1080p optimal-settings results");
    extent = DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 4096, 2176);
    Require(extent && extent->width == 3840 && extent->height == 2160, "SR allocation padding is excluded");
    params.outWidth = params.outHeight = 0;
    extent = DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 4096, 2176);
    Require(extent && extent->width == 3840 && extent->height == 2160, "missing legacy dimensions are irrelevant");
    params.dynamicWidth = 2560; params.dynamicHeight = 1440;
    extent = DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 3840, 2160);
    Require(extent && extent->width == 2560 && extent->height == 1440, "per-frame FSR output extent");
    params.dynamicHeight.reset();
    Require(!DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 3840, 2160), "partial dynamic extent rejected");
    params.dynamicHeight = 5000;
    Require(!DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 3840, 2160), "oversized dynamic extent rejected");
    params.dynamicWidth.reset(); params.dynamicHeight.reset();
    Require(!DlssNr::ResolveSrOutputExtent(&params, {3840, 2160}, 1920, 1080), "feature extent must fit output");
    Require(!DlssNr::ResolveSrOutputExtent(&params, {3840, 0}, 3840, 2160), "partial feature extent rejected");
    extent = DlssNr::ResolveSrOutputExtent(&params, {}, 3840, 2160);
    Require(extent && extent->width == 3840 && extent->height == 2160, "untracked native output fallback");
    Require(!DlssNr::ResolveSrOutputExtent(&params, {}, 0, 2160), "missing output rejected");
}

static Ptr<ID3D12Resource> Tex(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT fmt)
{
    auto desc = Effects::Storage::Description(w, h);
    desc.Format = fmt;
    // The game's output need not support UAV writes; the adapter must copy back.
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> r;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, PostSr::Read, nullptr, IID_PPV_ARGS(&r)),
          "texture");
    return r;
}
static std::vector<unsigned char> Transfer(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12Resource* tex,
                                           const std::vector<unsigned char>* upload = nullptr)
{
    const auto td = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 rowBytes = 0, total = 0;
    d->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, &rowBytes, &total);
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
    Ptr<ID3D12Resource> buffer;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                     upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&buffer)),
          "transfer buffer");
    void* mapped = nullptr;
    if (upload)
    {
        Check(buffer->Map(0, nullptr, &mapped), "upload map");
        for (UINT y = 0; y < td.Height; ++y)
            std::memcpy(static_cast<char*>(mapped) + y * fp.Footprint.RowPitch, upload->data() + y * rowBytes,
                        size_t(rowBytes));
        buffer->Unmap(0, nullptr);
    }
    auto r = NewRecording(d);
    const auto state = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
    Effects::Barrier(r.proxy.Get(), tex, PostSr::Read, state);
    D3D12_TEXTURE_COPY_LOCATION a {}, b {};
    a.pResource = tex;
    a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    b.pResource = buffer.Get();
    b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    b.PlacedFootprint = fp;
    r.proxy->CopyTextureRegion(upload ? &a : &b, 0, 0, 0, upload ? &b : &a, nullptr);
    Effects::Barrier(r.proxy.Get(), tex, state, PostSr::Read);
    Check(r.proxy->Close(), "transfer close");
    Check(r.proxy->ExecuteOn(q), "transfer execute");
    WaitQueue(d, q);
    std::vector<unsigned char> result(size_t(rowBytes) * td.Height);
    if (!upload)
    {
        Check(buffer->Map(0, nullptr, &mapped), "readback map");
        for (UINT y = 0; y < td.Height; ++y)
            std::memcpy(result.data() + y * rowBytes, static_cast<char*>(mapped) + y * fp.Footprint.RowPitch,
                        size_t(rowBytes));
        buffer->Unmap(0, nullptr);
    }
    return result;
}
static Ptr<ID3D12Resource> Guide(ID3D12Device* d, ID3D12CommandQueue* q, UINT w, UINT h, UINT channels)
{
    auto r = Tex(d, w, h, channels == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32_FLOAT);
    std::vector<float> values(size_t(w) * h * channels);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            for (UINT c = 0; c < channels; ++c)
                values[(y * w + x) * channels + c] = float(x + 100 * y + 1000 * c);
    std::vector<unsigned char> data(values.size() * 4);
    std::memcpy(data.data(), values.data(), data.size());
    Transfer(d, q, r.Get(), &data);
    return r;
}
static void Retained4KRecordings(ID3D12Device* d, ID3D12CommandQueue* q)
{
    // Engines may retain many closed lists even when their GPU work has finished.
    // Real 4K allocations exercise the byte limit (the small pixel tests cannot).
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        PostSr::Reset();
        auto colour = Tex(d, 3840, 2160, format);
        auto motion = Guide(d, q, 16, 12, 2), depth = Guide(d, q, 16, 12, 1);
        std::vector<Recording> recordings;
        for (unsigned i = 0; i < 24; ++i)
        {
            auto r = NewRecording(d);
            AmdPreSr::Frame f;
            f.colour = colour.Get(); f.motion = motion.Get(); f.depth = depth.Get();
            f.width = 3840; f.height = 2160; f.motionWidth = 16; f.motionHeight = 12;
            std::string reason;
            auto lease = PostSr::Prepare(r.proxy.Get(), f, 16, 12, reason);
            if (!lease) std::fprintf(stderr, "4K retained recording %u: %s\n", i, reason.c_str());
            Require(bool(lease), "24 retained 4K recordings must not exhaust the adapter budget");
            Check(r.proxy->Close(), "retained 4K close");
            recordings.push_back(std::move(r));
        }
        Require(PostSr::Global().pool.size() == 1, "4K recordings share one scratch set");
        Require(PostSr::Global().pool.front()->bytes < 180ull * 1024 * 1024, "4K scratch stays bounded");
        // Discarded lists must release their descriptors and recording ownership.
        recordings.clear();
        PostSr::Poll();
        Require(PostSr::Global().leases.empty(), "retained 4K recordings released");
    }
    PostSr::Reset();
}
static void SharedScratchRecordings(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12CommandQueue* other,
                                    const AmdPreSr::Frame& base)
{
    PostSr::Reset();
    auto alternateMotion = Guide(d, q, 16, 12, 2);
    std::vector<float> alternateValues(16 * 12 * 2);
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 16; ++x)
            for (unsigned c = 0; c < 2; ++c)
                alternateValues[(y * 16 + x) * 2 + c] = float(x + 100 * y + 1000 * c + 5000);
    std::vector<unsigned char> data(alternateValues.size() * sizeof(float));
    std::memcpy(data.data(), alternateValues.data(), data.size());
    Transfer(d, q, alternateMotion.Get(), &data);
    std::vector<Recording> recordings;
    std::vector<Ptr<ID3D12Resource>> targets, results, capturedGuides;
    for (unsigned i = 0; i < 24; ++i)
    {
        auto target = Tex(d, 32, 24, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto result = Texture(d, 32, 24);
        auto capture = Tex(d, 32, 24, DXGI_FORMAT_R32G32_FLOAT);
        UploadColorPattern(d, q, target.Get());
        g_patternExponentShift = 1 + i % 2;
        UploadColorPattern(d, q, result.Get());
        g_patternExponentShift = 0;
        auto r = NewRecording(d);
        auto f = base;
        f.colour = target.Get();
        if (i % 2) f.motion = alternateMotion.Get();
        f.jitterX = i % 2 ? -.5f : .5f;
        std::string reason;
        auto lease = PostSr::Prepare(r.proxy.Get(), f, 16, 12, reason);
        Require(bool(lease), "retained recording prepares without frame drops");
        // Capture each invocation's guides before the scratch set is reused.
        Effects::Barrier(r.proxy.Get(), f.motion, PostSr::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Effects::Barrier(r.proxy.Get(), capture.Get(), PostSr::Read, D3D12_RESOURCE_STATE_COPY_DEST);
        r.proxy->CopyResource(capture.Get(), f.motion);
        Effects::Barrier(r.proxy.Get(), f.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, PostSr::Read);
        Effects::Barrier(r.proxy.Get(), capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, PostSr::Read);
        if (i % 2) Check(r.proxy->SplitSegments(), "shared scratch split");
        Require(PostSr::Finish(r.proxy.Get(), lease, result.Get(), PostSr::Read), "shared scratch finish");
        Check(r.proxy->Close(), "shared scratch close");
        recordings.push_back(std::move(r));
        targets.push_back(std::move(target)); results.push_back(std::move(result));
        capturedGuides.push_back(std::move(capture));
    }
    Require(PostSr::Global().pool.size() == 1, "distinct recordings share scratch");
    for (size_t i = 1; i < PostSr::Global().leases.size(); ++i)
        Require(PostSr::Global().leases[i]->heap != PostSr::Global().leases[0]->heap,
                "retained descriptors remain private");

    Ptr<ID3D12Fence> gate, reached;
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "scratch gate");
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&reached)), "scratch marker");
    Check(q->Wait(gate.Get(), 1), "hold first scratch writer");
    Check(recordings[0].proxy->ExecuteOn(q), "first scratch writer");
    Check(recordings[1].proxy->ExecuteOn(other), "different recording on other queue");
    Check(other->Signal(reached.Get(), 1), "scratch writer marker");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "scratch marker event");
    Check(reached->SetEventOnCompletion(1, event), "scratch completion event");
    Require(WaitForSingleObject(event, 100) == WAIT_TIMEOUT, "different recording waits for shared scratch");
    Check(gate->Signal(1), "release first scratch writer");
    WaitQueue(d, other);
    CloseHandle(event);
    for (unsigned i = 2; i < recordings.size(); ++i)
        Check(recordings[i].proxy->ExecuteOn(i % 2 ? other : q), "queued scratch writer");
    WaitQueue(d, q); WaitQueue(d, other);
    // Replaying old lists after recording newer ones must retain their original
    // source, target and guide bindings, including across the producer/NR split.
    for (int i = int(recordings.size()) - 1; i >= 0; --i)
        Check(recordings[i].proxy->ExecuteOn(i % 2 ? q : other), "reverse scratch replay");
    WaitQueue(d, q); WaitQueue(d, other);
    for (unsigned i = 0; i < recordings.size(); ++i)
    {
        auto actual = Transfer(d, q, targets[i].Get()), expected = Transfer(d, q, results[i].Get());
        auto guides = Transfer(d, q, capturedGuides[i].Get());
        for (unsigned y = 0; y < 24; ++y)
            for (unsigned x = 0; x < 32; ++x)
            {
                const size_t offset = (y * 32 + x) * 8;
                Require(std::memcmp(actual.data() + offset, expected.data() + offset, 6) == 0,
                        "shared scratch preserves each recording's result");
                const int sx = std::clamp(int(std::floor((x + .5f) * .5f + (i % 2 ? -.5f : .5f))), 0, 15);
                const int sy = std::clamp(int(std::floor((y + .5f) * .5f - .5f)), 0, 11);
                float m[2]; std::memcpy(m, guides.data() + offset, 8);
                Require(m[0] == sx + 100 * sy + (i % 2 ? 5000 : 0) && m[1] == m[0] + 1000,
                        "shared scratch preserves each recording's guide bindings and jitter");
            }
    }
    recordings.clear();
    PostSr::Poll();
    Require(PostSr::Global().leases.empty(), "shared scratch recording leases released");
    PostSr::Reset();
}
static void Multipass(ID3D12Device* d, ID3D12CommandQueue* q, const AmdPreSr::Frame& base)
{
    // A deterministic GPU stand-in for the backend's entire cascade. Each pass
    // must consume its predecessor; only the final result reaches SR Output.
    // This tests the adapter/recording boundary, not neural-network quality.
    constexpr char shader[] = R"(
Texture2D<float4> input:register(t0);
RWTexture2D<float4> output:register(u0);
cbuffer Params:register(b0){uint width,height;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 if(p.x<width&&p.y<height)output[p.xy]=float4(input.Load(int3(p.xy,0)).rgb*2,0);
}
)";
    D3D12_DESCRIPTOR_RANGE ranges[] { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
                                      { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 } };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, 2 };
    D3D12_ROOT_SIGNATURE_DESC rd { 2, params };
    Ptr<ID3DBlob> rootBlob, code, error;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &error), "cascade root blob");
    Check(d->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "cascade root");
    Check(NativeCompileShaderBlob(shader, sizeof shader - 1, "post-SR cascade test", nullptr, nullptr, "main", &code,
                                  &error),
          "cascade shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = root.Get();
    pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)), "cascade pipeline");
    // Both the inline and producer/continuation paths must preserve the whole
    // cascade, including when the user reduces the pass count again.
    for (bool split : { false, true })
        for (UINT passes : { 1u, 2u, 3u, 1u })
        {
            UploadColorPattern(d, q, base.colour);
            std::vector<Ptr<ID3D12Resource>> stages;
            Ptr<ID3D12DescriptorHeap> heap;
            D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passes * 2,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "cascade descriptors");
            const auto stride = d->GetDescriptorHandleIncrementSize(hd.Type);
            auto r = NewRecording(d);
            auto f = base;
            std::string reason;
            auto lease = PostSr::Prepare(r.proxy.Get(), f, 16, 12, reason);
            Require(bool(lease), "cascade prepare");
            auto previous = f.colour;
            for (UINT pass = 0; pass < passes; ++pass)
            {
                stages.push_back(Texture(d, f.width, f.height));
                auto next = stages.back().Get();
                auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += SIZE_T(pass * 2) * stride;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
                srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                d->CreateShaderResourceView(previous, &srv, cpu);
                cpu.ptr += stride;
                d->CreateUnorderedAccessView(next, nullptr, nullptr, cpu);
                if (split && pass == 0)
                    Check(r.proxy->SplitSegments(), "cascade producer split");
                Effects::Barrier(r.proxy.Get(), next, PostSr::Read, PostSr::Write);
                r.proxy->SetComputeRootSignature(root.Get());
                r.proxy->SetPipelineState(pipeline.Get());
                ID3D12DescriptorHeap* heaps[] { heap.Get() };
                r.proxy->SetDescriptorHeaps(1, heaps);
                auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += UINT64(pass * 2) * stride;
                r.proxy->SetComputeRootDescriptorTable(0, gpu);
                const UINT dims[] { f.width, f.height };
                r.proxy->SetComputeRoot32BitConstants(1, 2, dims, 0);
                r.proxy->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
                Effects::Barrier(r.proxy.Get(), next, PostSr::Write, PostSr::Read);
                previous = next;
            }
            Require(PostSr::Finish(r.proxy.Get(), lease, previous, PostSr::Read), "cascade finish");
            Check(r.proxy->Close(), "cascade close");
            // Replaying a closed recording must still start with the SR frame,
            // not stale private pass buffers from its previous execution.
            for (int replay = 0; replay < 2; ++replay)
            {
                UploadColorPattern(d, q, base.colour);
                Check(r.proxy->ExecuteOn(q), "cascade execute");
                WaitQueue(d, q);
                const auto bytes = Transfer(d, q, base.colour);
                const auto desc = base.colour->GetDesc();
                std::vector<unsigned char> original(size_t(desc.Width) * 8);
                for (UINT y = 0; y < desc.Height; ++y)
                {
                    FillRow(original.data(), y, UINT(desc.Width), desc.Format);
                    for (UINT x = 0; x < desc.Width; ++x)
                        for (UINT c = 0; c < 4; ++c)
                        {
                            UINT16 actual, source;
                            std::memcpy(&actual, bytes.data() + ((y * desc.Width + x) * 4 + c) * 2, 2);
                            std::memcpy(&source, original.data() + (x * 4 + c) * 2, 2);
                            const float expected =
                                Half(source) * (x < f.width && y < f.height && c < 3 ? float(1u << passes) : 1.f);
                            Require(Half(actual) == expected, "cascade final pass, alpha and padding");
                        }
                }
            }
            r.proxy.Reset();
            lease.reset();
            PostSr::Poll();
            Require(PostSr::Global().leases.empty(), "cascade recordings released");
        }
}
int main()
{
    OutputExtentPolicy();
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory;
    Ptr<IDXGIAdapter> adapter;
    Ptr<ID3D12Device> d;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    SelectEffectsAdapter(factory.Get(), &adapter);
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)), "device");
    Ptr<ID3D12CommandQueue> q, other;
    D3D12_COMMAND_QUEUE_DESC qd {};
    Check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "queue");
    Check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&other)), "other queue");
    Retained4KRecordings(d.Get(), q.Get());
    auto output = Tex(d.Get(), 35, 27, DXGI_FORMAT_R16G16B16A16_FLOAT), result = Texture(d.Get(), 35, 27);
    UploadColorPattern(d.Get(), q.Get(), output.Get());
    g_patternExponentShift = 1;
    UploadColorPattern(d.Get(), q.Get(), result.Get());
    g_patternExponentShift = 0;
    auto mv = Guide(d.Get(), q.Get(), 20, 16, 2), depth = Guide(d.Get(), q.Get(), 19, 15, 1);
    AmdPreSr::Frame base;
    base.colour = output.Get();
    base.motion = mv.Get();
    base.depth = depth.Get();
    // The same 2:1 display/render ratio as 4K/1080p, with allocation padding.
    // Exercise production dimension selection before GPU guide/copy/multipass tests.
    OutputParameters params;
    params.outWidth = 16; params.outHeight = 12;
    const auto extent = DlssNr::ResolveSrOutputExtent(&params, {32, 24}, 35, 27);
    Require(bool(extent), "resolve SR output before recording");
    base.width = extent->width;
    base.height = extent->height;
    base.motionWidth = 16;
    base.motionHeight = 12;
    base.motionScaleX = -16;
    base.motionScaleY = 12;
    base.jitterX = .5f;
    base.jitterY = -.5f;
    std::string reason;
    auto record = [&](bool finish = true, bool split = false)
    {
        auto r = NewRecording(d.Get());
        auto f = base;
        auto l = PostSr::Prepare(r.proxy.Get(), f, 16, 12, reason);
        if (!l)
            std::fprintf(stderr, "Prepare: %s\n", reason.c_str());
        Require(bool(l), "post prepare");
        Require(f.width == 32 && f.motionWidth == 32 && f.motionHeight == 24 && f.jitterX == 0 && f.jitterY == 0,
                "post grid");
        Require(f.motionScaleX == -32 && f.motionScaleY == 24 && f.colour == output.Get(),
                "MV units and colour identity");
        if (split)
            Check(r.proxy->SplitSegments(), "split at NR evaluate");
        if (finish)
            Require(PostSr::Finish(r.proxy.Get(), l, result.Get(), PostSr::Read), "post finish");
        Check(r.proxy->Close(), "post close");
        return r;
    };
    {
        auto discarded = record();
    }
    PostSr::Poll();
    Require(PostSr::Global().leases.empty(), "discarded recording released");
    auto first = record();
    auto lease = PostSr::Global().leases.back();
    PostSr::Reset();
    Check(first.proxy->ExecuteOn(q.Get()), "delayed submission after reset");
    WaitQueue(d.Get(), q.Get());
    const auto got = Transfer(d.Get(), q.Get(), output.Get());
    const auto want = Transfer(d.Get(), q.Get(), result.Get());
    std::vector<unsigned char> original(35 * 27 * 8);
    for (UINT y = 0; y < 27; ++y)
        FillRow(original.data() + y * 35 * 8, y, 35, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (UINT y = 0; y < 27; ++y)
        for (UINT x = 0; x < 35; ++x)
            for (UINT c = 0; c < 8; ++c)
            {
                const size_t i = (y * 35 + x) * 8 + c;
                Require(got[i] == ((x < 32 && y < 24 && c < 6) ? want[i] : original[i]),
                        "copy back preserves active extent and original alpha");
            }
    const auto motions = Transfer(d.Get(), q.Get(), lease->storage->motion.Get());
    const auto depths = Transfer(d.Get(), q.Get(), lease->storage->depth.Get());
    for (UINT y = 0; y < 24; ++y)
        for (UINT x = 0; x < 32; ++x)
        {
            const int sx = std::clamp(int(std::floor((x + .5f) * .5f + .5f)), 0, 15),
                      sy = std::clamp(int(std::floor((y + .5f) * .5f - .5f)), 0, 11);
            float m[2], z;
            std::memcpy(m, motions.data() + (y * 32 + x) * 8, 8);
            std::memcpy(&z, depths.data() + (y * 32 + x) * 4, 4);
            Require(m[0] == sx + 100 * sy && m[1] == sx + 100 * sy + 1000 && z == sx + 100 * sy,
                    "active guide mapping excludes padding and applies jitter");
        }
    // Replay preserves a recording's bindings; cross-queue scratch reuse must
    // wait for prior writers even when the recording itself is still retained.
    Ptr<ID3D12Fence> gate;
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
    Check(q->Wait(gate.Get(), 1), "gate queue");
    Check(first.proxy->ExecuteOn(q.Get()), "held replay");
    Check(first.proxy->ExecuteOn(other.Get()), "cross queue replay");
    Ptr<ID3D12Fence> reached;
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&reached)), "marker");
    Check(other->Signal(reached.Get(), 1), "marker signal");
    Require(reached->GetCompletedValue() == 0, "cross queue writer waits");
    Ptr<ID3D12CommandAllocator> fresh;
    Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fresh)), "fresh allocator");
    Check(first.proxy->Reset(fresh.Get(), nullptr), "pending reset");
    PostSr::Poll();
    Require(!PostSr::Global().leases.empty(), "pending buffers pinned");
    Check(gate->Signal(1), "release gate");
    WaitQueue(d.Get(), other.Get());
    PostSr::Poll();
    Require(PostSr::Global().leases.empty(), "completed invalidated buffers released");
    Check(first.proxy->Close(), "fresh close");
    first.proxy.Reset();
    lease.reset();
    auto bad = NewRecording(d.Get());
    auto f = base;
    f.width = 36;
    Require(!PostSr::Prepare(bad.proxy.Get(), f, 16, 12, reason), "invalid output extent rejected");
    f = base;
    f.motionWidth = 21;
    Require(!PostSr::Prepare(bad.proxy.Get(), f, 16, 12, reason), "invalid motion extent rejected");
    f = base;
    f.jitterX = std::numeric_limits<float>::quiet_NaN();
    Require(!PostSr::Prepare(bad.proxy.Get(), f, 16, 12, reason), "nonfinite jitter rejected");
    Check(bad.proxy->Close(), "empty close");
    bad.proxy.Reset();
    // lmxxf/Mochizuki split the list after consuming the prepared guides, then
    // write their result in the continuation. Exercise that real proxy seam.
    {
        auto split = record(true, true);
        Check(split.proxy->ExecuteOn(q.Get()), "split producer and continuation");
        WaitQueue(d.Get(), q.Get());
        Check(split.proxy->ExecuteOn(other.Get()), "split replay");
        WaitQueue(d.Get(), other.Get());
    }
    PostSr::Poll();
    Require(PostSr::Global().leases.empty(), "split recording ownership released");
    Multipass(d.Get(), q.Get(), base);
    SharedScratchRecordings(d.Get(), q.Get(), other.Get(), base);
    // HDR FP16 result -> common SR output formats. No UAV flag on game output.
    for (auto fmt : { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT,
                      DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM })
    {
        auto target = Tex(d.Get(), 32, 24, fmt);
        auto r = NewRecording(d.Get());
        f = base;
        f.colour = target.Get();
        auto l = PostSr::Prepare(r.proxy.Get(), f, 16, 12, reason);
        Require(bool(l), "format prepare");
        Require(PostSr::Finish(r.proxy.Get(), l, result.Get(), PostSr::Read), "format conversion");
        Check(r.proxy->Close(), "format close");
        Check(r.proxy->ExecuteOn(q.Get()), "format submit");
        WaitQueue(d.Get(), q.Get());
        auto bytes = Transfer(d.Get(), q.Get(), target.Get());
        Require(std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; }), "converted pixels");
    }
    PostSr::Reset();
    PostSr::Poll();
    Require(PostSr::Global().leases.empty(), "format recordings released");
    // Failed completion notification must never permit buffer reuse or replay.
    auto failed = record(false);
    auto failLease = PostSr::Global().leases.back();
    auto sibling = record(false);
    auto siblingLease = PostSr::Global().leases.back();
    Require(siblingLease->storage == failLease->storage, "failed proof fixture shares scratch");
    Check(failed.proxy->ExecuteOn(q.Get()), "failure fixture");
    WaitQueue(d.Get(), q.Get());
    Submission::RecordingExecution e { failLease->identity, 99, q.Get() };
    e.producerSubmitted = true;
    e.status = E_FAIL;
    failLease->Executed(e);
    Require(FAILED(failLease->BeforeExecute(e)), "unconfirmed replay rejected");
    e.identity = siblingLease->identity;
    Require(FAILED(siblingLease->BeforeExecute(e)), "unconfirmed shared scratch blocks sibling execution");
    // Fixture cleanup: actual work completed before injecting the failed proof.
    failLease->unconfirmed = false;
    failLease->storage->unconfirmed = false;
    failed.proxy.Reset();
    failLease.reset();
    sibling.proxy.Reset();
    siblingLease.reset();
    PostSr::Reset();
    PostSr::Poll();
    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(d.As(&info)))
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> data(size);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            Check(info->GetMessage(i, m, &size), "debug message");
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                std::fprintf(stderr, "%s\n", m->pDescription);
            Require(m->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "debug validation");
        }
    std::puts("Post-SR NR: PASS (1/2/3-pass cascade and replay, guides, units, formats, padding, alpha, discard, "
              "24 retained 4K lists, private descriptors, shared scratch cross-queue/reverse replay, pending Reset, "
              "failed proof)");
}
