// CPU-only native Style contract: no HIP API/device is created.
#include "third_party/lmxxf/src/native_hip_env_options.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h"
#include <cstdio>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static uint32_t bits(float value) {
    uint32_t out; std::memcpy(&out, &value, sizeof out); return out;
}
template<class F> static void rejects(F fn) {
    bool threw = false;
    try { fn(); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "invalid style/weights were accepted");
}
int main() {
    try {
        require(hip_reference::Options{}.model_style == 1, "legacy default changed");
        require(!std::strcmp(CfgKey::EnvAlias(CfgKey::ModelStyle), "DLSS5_MODEL_STYLE"), "wrong alias");
        bool known = false;
        for (const auto* key : CfgKey::kKnown) known |= !std::strcmp(key, CfgKey::ModelStyle);
        require(known, "Style is not owned by the INI");
        for (unsigned style = 0; style != 3; ++style) {
            _putenv_s("DLSS5_MODEL_STYLE", "2"); // External fallback must lose to the host.
            CfgKey::PutEnvString(CfgKey::ModelStyle, std::to_string(style).c_str());
            char processValue[4]{};
            require(GetEnvironmentVariableA("DLSS5_MODEL_STYLE", processValue, sizeof processValue) == 1,
                    "host selection was not published across CRTs");
            const std::string expected = std::to_string(style);
            require(expected == processValue, "wrong style published to Win32");
            // Simulate a stale independent CRT after the process environment changed.
            _putenv_s("DLSS5_MODEL_STYLE", std::to_string((style + 1) % 3).c_str());
            SetEnvironmentVariableA("DLSS5_MODEL_STYLE", expected.c_str());
            require(std::strcmp(std::getenv("DLSS5_MODEL_STYLE"), expected.c_str()) != 0,
                    "stale CRT fixture was not established");
            CfgKey::SyncEnvAliasesFromProcess();
            hip_reference::Options opt;
            NativeApplyHipEnvironment(opt, true);
            require(opt.model_style == style, "environment selection did not reach Options");
        }
        for (const char* invalid : {"-1", "3", "1.0", "01", "auto", " 0"}) {
            _putenv_s("DLSS5_MODEL_STYLE", invalid);
            rejects([] { hip_reference::Options opt; NativeApplyHipEnvironment(opt, true); });
        }
        _putenv_s("DLSS5_MODEL_STYLE", "");
        hip_reference::Options unset;
        NativeApplyHipEnvironment(unset, true);
        require(unset.model_style == 1, "missing override changed legacy default");

        // Include positive/negative zero, subnormal and normal FP16 coefficients.
        const uint16_t sample[] = {0, 0x8000, 1, 0x8001, 0x03ff, 0x83ff, 0x0400, 0x8400,
                                   0x2111, 0xa111, 0x3c00, 0xbc00, 0x4000, 0xc000, 0x7400, 0xf400};
        std::vector<float> original(8736);
        for (size_t i = 0; i < original.size(); ++i) original[i] = float(i % 23) / 32.f;
        for (size_t row = 0; row != 32; ++row) original[row * 16 + 6] = hip_reference::Half(sample[row % 16]);
        for (unsigned style = 0; style != 3; ++style) {
            auto folded = original;
            hip_reference::ApplyPrefixModelStyle(folded, style);
            if (style == 1)
                require(!std::memcmp(folded.data(), original.data(), original.size() * sizeof(float)),
                        "Natural did not retain original bytes");
            for (size_t i = 0; i < original.size(); ++i) {
                if (i >= 512 || i % 16 != 6) {
                    require(bits(folded[i]) == bits(original[i]), "non-Style coefficient changed");
                    continue;
                }
                const float feature = hip_reference::Half(hip_reference::ExactWeightHalf(float(style) / 128.f));
                const float oldWeight = hip_reference::Half(hip_reference::ExactWeightHalf(original[i]));
                const float newWeight = hip_reference::Half(hip_reference::ExactWeightHalf(folded[i]));
                require(bits(feature * oldWeight) == bits((1.f / 128.f) * newWeight),
                        "FP16 projection product or zero sign differs");
            }
            // Packing must preserve the entire prefix matrix, including signed zero.
            const auto prefix = folded;
            // Exact E4M3 tail fixture exercises the same C32 regions as PackedC32Weight.
            std::fill(folded.begin() + 512, folded.begin() + 8704, 0.5f);
            hip_reference::PackWeightRegions(folded, {{512,4096},{4608,4096}});
            require(!std::memcmp(folded.data(), prefix.data(), 512 * sizeof(float)), "C32 packing overwrote prefix");
        }
        auto bad = original;
        rejects([&] { hip_reference::ApplyPrefixModelStyle(bad, 3); });
        bad.resize(512);
        rejects([&] { hip_reference::ApplyPrefixModelStyle(bad, 0); });
        bad = original; bad[6] = 0.1f;
        rejects([&] { hip_reference::ApplyPrefixModelStyle(bad, 0); });
        bad = original; bad[6] = 65504.f;
        rejects([&] { hip_reference::ApplyPrefixModelStyle(bad, 2); });
        std::puts("lmxxf_model_style: PASS (CPU only)");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lmxxf_model_style: FAIL: %s\n", e.what());
        return 1;
    }
}
