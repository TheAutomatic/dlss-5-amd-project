#include "nr_effects_float_utils.h"

int main()
{
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory; Ptr<IDXGIAdapter> adapter; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    SelectEffectsAdapter(factory.Get(), &adapter);
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    Ptr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    Require(!ResidualSettings {nan, nan, nan, nan}.Active(), "non-finite settings become neutral");
    const auto bounds = ResidualSettings {-1, 8, -2, 3}.Bounded();
    Require(bounds.lowGain == 0 && bounds.detailGain == 2 && bounds.skinProtection == 0 && bounds.edgeProtection == 1,
            "host clamps settings before shader constants");

    // Partial workgroups, a one-pixel image, and an odd narrow image exercise
    // the shared-memory halo/barrier, including out-of-range lanes.
    for (auto [w, h] : {std::pair<UINT, UINT>{19, 13}, {1, 1}, {3, 17}})
    {
        auto original = FloatTexture(device.Get(), w, h), result = FloatTexture(device.Get(), w, h);
        const size_t n = size_t(w) * h, centre = size_t(h / 2) * w + w / 2;
        std::vector<Pixel> base(n, Pixel {.2f, .3f, .4f, .375f}), nr(n, Pixel {.3f, .4f, .5f, .9f});
        auto upload = [&] {
            Transfer(device.Get(), queue.Get(), original.Get(), &base);
            Transfer(device.Get(), queue.Get(), result.Get(), &nr);
        };
        auto record = [&](ResidualSettings settings, float intensity = 1.f) {
            auto recording = NewRecording(device.Get());
            recording.output = Effects::Record(recording.proxy.Get(), original.Get(), result.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, w, h, intensity, false, {}, {}, settings);
            Check(recording.proxy->Close(), "shape close");
            return recording;
        };
        auto run = [&](ResidualSettings settings, float intensity = 1.f) {
            auto recording = record(settings, intensity);
            Check(recording.proxy->ExecuteOn(queue.Get()), "shape submit");
            WaitQueue(device.Get(), queue.Get());
            auto pixels = Transfer(device.Get(), queue.Get(), recording.output);
            recording.proxy.Reset(); Effects::Reset(); Effects::Poll();
            Require(NoLeases(), "shaping releases completed resources");
            for (auto p : pixels) for (float v : p) Require(std::isfinite(v), "finite shaped output");
            return pixels;
        };
        auto approx = [](float a, float b) { return std::abs(a - b) <= (std::max)(.001f, std::abs(b) * .002f); };
        upload();
        {
            auto neutral = record({});
            Require(neutral.output == result.Get() && Effects::Global().storage.empty(), "neutral no-allocation path");
            auto zero = record({2, 0, 1, 1}, 0);
            Require(zero.output == original.Get(), "overall zero wins over shaping");
        }
        for (auto p : run({1.5f, .25f, 0, 0}))
            Require(approx(p[0], .35f) && approx(p[1], .45f) && approx(p[2], .55f) && p[3] == .375f,
                    "constant correction uses low gain only, retains original alpha");
        for (auto p : run({0, 0, 0, 0})) Require(approx(p[0], .2f), "zero gains keep original");
        // Settings belong to a recording, not a mutable descriptor or constant buffer.
        auto delayed = record({1.5f, .25f, 0, 0}), later = record({.5f, 1.5f, 1, 1});
        Effects::Reset();
        Check(later.proxy->ExecuteOn(queue.Get()), "later first");
        Check(delayed.proxy->ExecuteOn(queue.Get()), "delayed old settings");
        WaitQueue(device.Get(), queue.Get());
        Require(approx(Transfer(device.Get(), queue.Get(), delayed.output)[centre][0], .35f), "delayed constants retained");
        delayed.proxy.Reset(); later.proxy.Reset(); Effects::Poll();
        Require(NoLeases(), "delayed records retired");
        if (w == 1) continue;

        // A residual impulse on constant original: suppress fine changes, not
        // underlying image data. Neighbours contain only the low-frequency share.
        base.assign(n, Pixel {.2f, .2f, .2f, .375f}); nr = base;
        nr[centre] = {.6f, .6f, .6f, .9f}; upload();
        auto smooth = run({1, 0, 0, 0});
        Require(approx(smooth[centre][0], .3f), "binomial low-pass impulse: one quarter at centre");
        Require(approx(smooth[0][0], .2f), "distant original unchanged");
        // Scale with HDR exposure and preserve signed scRGB; no implicit sRGB
        // transfer or clamping to [0,1] is allowed in the spatial controls.
        for (auto& p : base) for (unsigned c = 0; c < 3; ++c) p[c] *= 16;
        for (auto& p : nr) for (unsigned c = 0; c < 3; ++c) p[c] *= 16;
        upload(); auto hdr = run({1, 0, 0, 0});
        Require(approx(hdr[centre][0], smooth[centre][0] * 16), "HDR exposure scale");
        base.assign(n, Pixel {-.5f, .25f, 2.f, .375f});
        nr.assign(n, Pixel {-.25f, .5f, 2.25f, .9f}); upload();
        auto signedRgb = run({1.5f, .5f, 1, 1});
        Require(approx(signedRgb[centre][0], -.125f) && approx(signedRgb[centre][2], 2.375f), "signed RGB and HDR kept");

        // Skin protection is explicitly a colour heuristic. Green content is
        // not skin, and the original warm-coloured image itself is untouched.
        base.assign(n, Pixel {.6f, .36f, .22f, .375f}); nr = base;
        nr[centre][0] += .4f; nr[centre][1] += .4f; nr[centre][2] += .4f; upload();
        const auto warm = run({1, 1, 1, 0});
        Require(warm[centre][0] < .8f && warm[centre][0] > .6f, "skin limits added fine detail");
        for (auto& p : base) for (unsigned c = 0; c < 3; ++c) p[c] *= 16;
        for (auto& p : nr) for (unsigned c = 0; c < 3; ++c) p[c] *= 16;
        upload(); const auto warmHdr = run({1, 1, 1, 0});
        Require(approx(warmHdr[centre][0], warm[centre][0] * 16), "skin exposure scale");
        base.assign(n, Pixel {.22f, .6f, .36f, .375f}); nr = base;
        nr[centre][0] += .4f; nr[centre][1] += .4f; nr[centre][2] += .4f; upload();
        Require(approx(run({1, 1, 1, 0})[centre][0], .62f), "green is not skin");
        nr = base; upload();
        const auto unchanged = run({2, 0, 1, 1});
        Require(approx(unchanged[centre][0], base[centre][0]), "zero residual stays zero with all controls");

        const UINT edgeX = w / 2 - 1; const size_t atEdge = size_t(h / 2) * w + edgeX;
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
            const float value = x < w / 2 ? .1f : 1.f;
            base[size_t(y) * w + x] = {value, value, value, .375f};
        }
        nr = base; nr[atEdge][0] += .5f; upload();
        auto edge = run({1, 1, 0, 1});
        Require(edge[atEdge][0] < .5f && edge[atEdge][0] >= .1f, "edge limits introduced halo");
        Require(approx(edge[atEdge+1][0], 1.f), "original bright edge remains");

        nr[centre][0] = nan; upload();
        auto invalid = run({1.5f, .5f, 1, 1});
        Require(invalid[centre][3] == .375f, "invalid result preserves original alpha");
        base.assign(n, Pixel {0, 0, 0, .375f}); nr = base; upload();
        Require(run({2, 0, 1, 1})[centre][0] == 0, "black has no division artefact");
    }
    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes); std::vector<char> data(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        Check(info->GetMessage(i, message, &bytes), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
        Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "debug validation");
    }
    std::puts("NR residual shaping: PASS (neutral, spectrum, skin, edges, HDR, odd sizes, recording lifetime)");
}
