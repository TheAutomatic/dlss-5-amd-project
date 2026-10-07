// Exercise the shipped SPIR-V, without model weights: hue, unchanged SDR, alpha and dispatch bounds.
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <nrvk.hpp>

using Pixel = std::array<float, 4>;

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static float Decode(float x)
{
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}

static void SameHue(const Pixel& encoded, const Pixel& source)
{
    float decoded[3] = { Decode(encoded[0]), Decode(encoded[1]), Decode(encoded[2]) };
    const float a = std::max({ decoded[0], decoded[1], decoded[2] });
    const float b = std::max({ source[0], source[1], source[2] });
    for (int c = 0; c != 3; ++c)
        Require(std::abs(decoded[c] / a - source[c] / b) < 2e-5f, "HDR encoding changed RGB ratios");
}

struct Resources
{
    nrvk::Context& ctx;
    std::vector<nrvk::Context::Image> images;
    nrvk::Kernel kernel;
    explicit Resources(nrvk::Context& c) : ctx(c) { images.reserve(4); }
    ~Resources()
    {
        if (kernel.device) kernel.destroy();
        for (auto& im : images) ctx.destroy(im);
    }
    auto& Image(uint32_t w, uint32_t h, bool sampled = false)
    {
        images.push_back(ctx.image(w, h, VK_FORMAT_R32G32B32A32_SFLOAT, sampled));
        return images.back();
    }
    template<class Push> void Run(const Push& push, uint32_t width, uint32_t height)
    {
        ctx.one_shot([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.layout, 0, 1,
                                    &kernel.set, 0, nullptr);
            vkCmdPushConstants(cmd, kernel.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
        });
    }
};

static const Pixel colors[] = {
    { 4.f, 1.f, .25f, .375f }, { .25f, 4.f, 1.f, .5f }, { 1.f, .25f, 4.f, .625f },
    { 16.f, 4.f, 1.f, .75f }, { .18f, .09f, .045f, 1.f }, { 0.f, 0.f, 0.f, 0.f },
    { -1.f, .25f, .125f, .25f }
};

static void Encode(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w = 19, h = 11, validW = 17, validH = 9;
    Resources r(ctx);
    auto& proxy = r.Image(w, h);
    auto& keep = r.Image(w, h);
    r.kernel.create(ctx, (shaders / "runtime_encode.spv").string(), {}, 16, { &proxy, &keep });
    std::vector<Pixel> input(w * h), output(w * h), kept(w * h), sentinel(w * h, { -9.f, -9.f, -9.f, -9.f });
    for (size_t i = 0; i < input.size(); ++i) input[i] = colors[i % std::size(colors)];
    for (float white : { .25f, 1.f, 4.f, 0.f })
    {
        ctx.upload(proxy, input.data(), input.size() * sizeof(Pixel));
        ctx.upload(keep, sentinel.data(), sentinel.size() * sizeof(Pixel));
        struct { uint32_t w, h; float white, knee; } push { validW, validH, white, .75f };
        r.Run(push, w, h);
        ctx.download(proxy, output.data(), output.size() * sizeof(Pixel));
        ctx.download(keep, kept.data(), kept.size() * sizeof(Pixel));
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = y * w + x;
            if (x >= validW || y >= validH)
            {
                Require(output[i] == input[i] && kept[i] == sentinel[i], "encode wrote outside the valid extent");
                continue;
            }
            Require(kept[i] == input[i] && output[i][3] == input[i][3], "encode changed original/alpha");
            for (int c = 0; c != 3; ++c)
                Require(std::isfinite(output[i][c]) && output[i][c] >= 0.f && output[i][c] <= 1.000001f,
                        "encode produced invalid or unbounded output");
            Pixel nonnegative = input[i];
            for (int c = 0; c != 3; ++c) nonnegative[c] = std::max(0.f, nonnegative[c]);
            if (nonnegative[0] + nonnegative[1] + nonnegative[2] > 0.f) SameHue(output[i], nonnegative);
            // Below the knee and peak limit the operation remains the ordinary sRGB transfer.
            const float norm = std::max(white, 1e-4f);
            if (std::max({nonnegative[0], nonnegative[1], nonnegative[2]}) / norm <= .75f)
                for (int c = 0; c != 3; ++c)
                    Require(std::abs(Decode(output[i][c]) - nonnegative[c] / norm) < 2e-6f,
                            "encode changed ordinary brightness");
        }
    }
}

static void Transfer(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w = 19, h = 11;
    for (bool scaled : { false, true })
    {
        const uint32_t mw = scaled ? 2 : w, mh = scaled ? 2 : h;
        Resources r(ctx);
        auto& model = r.Image(mw, mh);
        auto& shown = r.Image(mw, mh, true);
        auto& keep = r.Image(w, h, true);
        auto& answer = r.Image(w, h);
        r.kernel.create(ctx, (shaders / "runtime_transfer.spv").string(), {}, 36,
                        { &model, &shown, &keep, &answer });
        for (const Pixel& source : colors)
        {
            if (source[0] < 0.f) continue; // transfer has its own gamut handling, outside this change
            Pixel proxy = source;
            // Any common positive normalization cancels in the no-edit transfer; peak-normalize HDR.
            const float peak = std::max({ 1.f, source[0], source[1], source[2] });
            for (int c = 0; c != 3; ++c)
            {
                const float v = source[c] / peak;
                proxy[c] = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
            }
            std::vector<Pixel> pixels(mw * mh, proxy), original(w * h, source), output(w * h);
            ctx.upload(model, pixels.data(), pixels.size() * sizeof(Pixel));
            ctx.upload(shown, pixels.data(), pixels.size() * sizeof(Pixel));
            ctx.upload(keep, original.data(), original.size() * sizeof(Pixel));
            struct { uint32_t w, h, mw, mh, passthrough; float detail, colour, guard, white; }
                push { w, h, mw, mh, 0, 1.f, 1.f, 4.f, 1.f };
            r.Run(push, w, h);
            ctx.download(answer, output.data(), output.size() * sizeof(Pixel));
            for (const Pixel& p : output)
            {
                Require(p[3] == source[3], "transfer changed alpha");
                for (int c = 0; c != 3; ++c)
                    Require(std::isfinite(p[c]) && std::abs(p[c] - source[c]) < .001f,
                            "no-edit transfer changed colour (full or reduced model size)");
            }
        }
    }
}

int main(int argc, char** argv)
{
    nrvk::Context ctx;
    try
    {
        Require(argc == 2, "usage: hdr_shader <shaders/runtime>");
        ctx.create();
        std::cout << "GPU: " << ctx.gpu_name << '\n';
        Encode(ctx, argv[1]);
        Transfer(ctx, argv[1]);
        ctx.destroy();
        std::cout << "PASS Mochizuki HDR encode/transfer: hue, SDR, alpha, bounds, scaled/unscaled\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        if (ctx.device) ctx.destroy();
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
