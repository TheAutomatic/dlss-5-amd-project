#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "third_party/lmxxf/Development/HIP/hip_api.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

// Exercise the production GPU decision/finish kernels. The CPU oracle is the
// cache contract (bit equality + finiteness), not a copy of the wave reduction.
using U = unsigned;
struct Fixture {
    hip_probe::Api api{6};
    hip_probe::Handle module{}, stream{}, statsFn{}, decideFn{}, finishFn{};
    static constexpr U n = 48, count = n * 1024;
    void *x{}, *anchor{}, *full{}, *saved{}, *stats{}, *state{}, *out{}, *gain{}, *image{}, *imageAnchor{};
    std::vector<float> input = std::vector<float>(count, 1.f), result = std::vector<float>(count);
    std::vector<float> expectedAnchor, expectedOutput;
    bool valid = false;
    U tests = 0;
    Fixture(const char* path) {
        api.Check(api.hipInit(0), "init");
        int devices = 0, selected = -1;
        api.Check(api.hipGetDeviceCount(&devices), "devices");
        for (int d = 0; d < devices; ++d) {
            hip_probe::DevicePropertiesR0600 p{};
            api.Check(api.hipGetDevicePropertiesR0600(&p, d), "properties");
            if (std::string(p.gcnArchName).rfind("gfx120", 0) == 0) { selected = d; break; }
        }
        if (selected < 0) throw std::runtime_error("gfx1200/1201 required");
        api.Check(api.hipSetDevice(selected), "device");
        api.Check(api.LoadModule(&module, path), "module");
        api.Check(api.hipStreamCreate(&stream), "stream");
        api.Check(api.hipModuleGetFunction(&statsFn, module, "reuse_token_stats"), "stats kernel");
        api.Check(api.hipModuleGetFunction(&decideFn, module, "reuse_decide_exact"), "exact kernel");
        api.Check(api.hipModuleGetFunction(&finishFn, module, "reuse_finish"), "finish kernel");
        for (void** ptr : {&x, &anchor, &full, &saved, &stats, &out, &gain, &image, &imageAnchor})
            api.Check(api.hipMalloc(ptr, count * 4), "allocation");
        api.Check(api.hipMalloc(&state, 32), "state allocation");
        std::vector<float> initial(count, 37.f);
        for (void* ptr : {anchor, saved, gain, image, imageAnchor})
            api.Check(api.hipMemcpy(ptr, initial.data(), count * 4, 1), "initialize");
        reset();
    }
    ~Fixture() {
        api.hipStreamSynchronize(stream);
        for (void* ptr : {x, anchor, full, saved, stats, state, out, gain, image, imageAnchor}) api.hipFree(ptr);
        api.hipModuleUnload(module); api.hipStreamDestroy(stream);
    }
    void reset() { api.Check(api.hipMemsetAsync(state, 0, 32, stream), "reset"); valid = false; }
    template<class... Args> void launch(hip_probe::Handle fn, U groups, Args... args) {
        void* argv[] = {static_cast<void*>(&args)...};
        api.Check(api.hipModuleLaunchKernel(fn, groups, 1, 1, 32, 1, 1, 0, stream, argv, nullptr), "launch");
    }
    void step(const char* name, U reason) {
        bool same = valid && memcmp(input.data(), expectedAnchor.data(), count * 4) == 0;
        if (same) for (float v : input) if (!std::isfinite(v)) { same = false; break; }
        // Different deterministic full output each frame detects accidental full
        // selection on a cache hit and stale output/anchor writes on a miss.
        for (U i = 0; i < count; ++i) result[i] = float((i + tests * 97) % 4096) * .125f;
        api.Check(api.hipMemcpy(x, input.data(), count * 4, 1), "input");
        api.Check(api.hipMemcpy(full, result.data(), count * 4, 1), "full output");
        launch(statsFn, n, x, anchor, state, stats, n, U(1), U(1), U(16));
        launch(decideFn, 1, stats, state, n);
        launch(finishFn, count / 256, x, full, anchor, saved, gain, state, out, count, image, imageAnchor, U(3));
        api.Check(api.hipStreamSynchronize(stream), "complete");
        std::array<U, 8> words{};
        api.Check(api.hipMemcpy(words.data(), state, 32, 2), "state");
        if (bool(words[0]) != same || words[6] != reason)
            throw std::runtime_error(std::string(name) + " decision reason=" + std::to_string(words[6]));
        if (!same) { expectedAnchor = input; expectedOutput = result; valid = true; }
        for (auto check : {std::make_pair(out, &expectedOutput), std::make_pair(saved, &expectedOutput), std::make_pair(anchor, &expectedAnchor)}) {
            api.Check(api.hipMemcpy(result.data(), check.first, count * 4, 2), "readback");
            if (memcmp(result.data(), check.second->data(), count * 4)) throw std::runtime_error(std::string(name) + " output/anchor mismatch");
        }
        ++tests;
    }
};
int main(int argc, char** argv) { try {
    if (argc != 2) throw std::runtime_error("expected explicit deep_fast module path");
    Fixture f(argv[1]);
    f.step("cold", 1);
    for (U i = 0; i < 24; ++i) f.step("identical beyond period", 8);
    U bit = 0x3f800001; memcpy(&f.input[0], &bit, 4); f.step("single bit", 9); f.step("new anchor", 8);
    f.input.back() = 2.f; f.step("padded token last lane", 9);
    f.input[1024 * 32 + 31] = 3.f; f.step("second reduction iteration", 9);
    f.input[1] = 0.f; f.step("zero", 9);
    f.input[1] = -0.f; f.step("signed zero", 9); f.step("identical signed zero", 8);
    f.input.back() = std::numeric_limits<float>::quiet_NaN(); f.step("NaN", 4); f.step("identical NaN", 4);
    f.input.back() = 1.f; f.step("nonfinite anchor", 4); f.step("recovered finite", 8);
    f.input[31] = std::numeric_limits<float>::infinity(); f.step("infinity", 4); f.step("identical infinity", 4);
    f.input[31] = 1.f; f.reset(); f.step("reset", 1); f.step("after reset", 8);
    printf("PASS exact reuse %u GPU cases; bit-identical output and anchors\n", f.tests);
    return 0;
} catch (const std::exception& e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; } }
