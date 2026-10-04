#include "nr_effects_test_utils.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/PostSr.h"

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
int main()
{
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
    base.width = 32;
    base.height = 24;
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
    // A native-size colour pass still changes the stage: replay must not alias
    // a later recording, and cross-queue reuse must wait for its own completion.
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
    Check(failed.proxy->ExecuteOn(q.Get()), "failure fixture");
    WaitQueue(d.Get(), q.Get());
    Submission::RecordingExecution e { failLease->identity, 99, q.Get() };
    e.producerSubmitted = true;
    e.status = E_FAIL;
    failLease->Executed(e);
    Require(FAILED(failLease->BeforeExecute(e)), "unconfirmed replay rejected");
    // Fixture cleanup: actual work completed before injecting the failed proof.
    failLease->unconfirmed = false;
    failed.proxy.Reset();
    failLease.reset();
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
    std::puts("Post-SR NR: PASS (guides, units, formats, padding, alpha, discard, pending Reset, cross-queue replay, "
              "failed proof)");
}
