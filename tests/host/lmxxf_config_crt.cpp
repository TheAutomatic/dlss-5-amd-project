// Build this file as a DLL and an EXE with /MT to exercise independent CRT caches.
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h"

#ifdef LMXXF_CONFIG_CRT_FIXTURE
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfProductionOptions.h"
extern "C" __declspec(dllexport) bool RuntimeSkips(const char *expected)
{
    auto options = LmxxfProductionOptions(1920, 1152, "modules", "assets");
    return options.skip_blocks == hip_reference::ParseSkipBlocks(expected);
}
extern "C" __declspec(dllexport) bool RuntimeSwin(unsigned w, unsigned h)
{
    return hip_reference::SwinRunCompatible(LmxxfProductionOptions(w, h, "modules", "assets"));
}
extern "C" __declspec(dllexport) bool RuntimeRejectsChainFinishSkip()
{
    try
    {
        LmxxfProductionOptions(1920, 1152, "modules", "assets");
        return false;
    }
    catch (const std::runtime_error &e)
    {
        return std::strstr(e.what(), "Skipping blocks 4 or 69") != nullptr;
    }
}

extern "C" __declspec(dllexport) bool RuntimeExcludesReferenceExperiments()
{
    _putenv_s("DLSS5_HIP_SUBMIT_PULSE", "1");
    _putenv_s("DLSS5_TEMPORAL_HISTORY_EXPERIMENT", "1");
    const auto options = LmxxfProductionOptions(1920, 1152, "modules", "assets");
    _putenv_s("DLSS5_HIP_SUBMIT_PULSE", "");
    _putenv_s("DLSS5_TEMPORAL_HISTORY_EXPERIMENT", "");
    return options.submit_pulse == 0 && !options.experimental_temporal && !options.temporal_feature_tap;
}

extern "C" __declspec(dllexport) const char *ReadRuntimeEnvironment(const char *key)
{
    return std::getenv(key);
}

extern "C" __declspec(dllexport) void SyncRuntimeEnvironment()
{
    CfgKey::SyncEnvAliasesFromProcess();
}
#else
namespace
{
void Require(bool ok, const char *message)
{
    if (!ok)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}

int main(int argc, char **argv)
{
    Require(argc == 2, "expected runtime fixture DLL path");
    for (const char *key : CfgKey::kKnown)
        if (CfgKey::EnvAlias(key))
            CfgKey::PutEnvAlias(key, true);

    const HMODULE dll = LoadLibraryA(argv[1]);
    Require(dll != nullptr, "could not load runtime fixture");
    const auto read = reinterpret_cast<const char *(*)(const char *)>(
        GetProcAddress(dll, "ReadRuntimeEnvironment"));
    const auto sync = reinterpret_cast<void (*)()>(GetProcAddress(dll, "SyncRuntimeEnvironment"));
    const auto skips = reinterpret_cast<bool (*)(const char *)>(GetProcAddress(dll, "RuntimeSkips"));
    const auto swin = reinterpret_cast<bool (*)(unsigned, unsigned)>(GetProcAddress(dll, "RuntimeSwin"));
    const auto rejects = reinterpret_cast<bool (*)()>(GetProcAddress(dll, "RuntimeRejectsChainFinishSkip"));
    Require(read && sync && skips && swin && rejects, "missing runtime fixture exports");
    auto expect = [&](const char *key, const char *value) {
        const char *actual = read(key);
        if (!actual || std::strcmp(actual, value) != 0)
        {
            std::fprintf(stderr, "%s: expected %s, got %s\n", key, value, actual ? actual : "(unset)");
            std::exit(1);
        }
    };

    // Prime the DLL cache, then prove each host boolean update crosses the CRT boundary.
    for (const char *key : CfgKey::kKnown)
    {
        if (!CfgKey::EnvAlias(key))
            continue;
        expect(key, "1");
        CfgKey::PutEnvAlias(key, false);
        expect(key, "1"); // Publishing to Win32 alone leaves the private CRT stale.
        sync();
        expect(key, "0");
        CfgKey::PutEnvAlias(key, true);
        sync();
        expect(key, "1");
    }

    // Exercise the string path used by network tier and reuse sliders as well.
    const char *stringKeys[] = {CfgKey::NetworkHeight, CfgKey::VitStream, CfgKey::VitReusePeriod,
                               CfgKey::VitReuseGlobal, CfgKey::VitReuseLocal, CfgKey::VitReuseImage};
    const char *values[] = {"720", "3", "8", "0.25", "1.5", "0.35"};
    for (size_t i = 0; i < sizeof stringKeys / sizeof *stringKeys; ++i)
    {
        CfgKey::PutEnvString(stringKeys[i], values[i]);
        sync();
        expect(stringKeys[i], values[i]);
    }
    CfgKey::PutEnvString(CfgKey::NetworkHeight, "auto");
    sync();
    expect(CfgKey::NetworkHeight, "auto");

    std::string normalized;
    Require(CfgKey::NormalizeSkipBlocks(" 46,42,43,42 ", normalized) && normalized == "42,43,46",
            "skip block normalization failed");
    Require(CfgKey::NormalizeSkipBlocks("31-33,42", normalized) && normalized == "31,32,33,42",
            "skip block range normalization failed");
    Require(CfgKey::NormalizeSkipBlocks("31–33, 45 - 46", normalized) && normalized == "31,32,33,45,46",
            "skip block dash/space normalization failed");
    Require(CfgKey::NormalizeSkipBlocks("31-37", normalized) && normalized == "31,32,33,34,35,36,37",
            "skip block 31-37 range expansion failed");
    for (const char *invalid : {"", "0", "39", "70", "1,,2", "1,", "-1", "4294967297", "1x",
                                "1-", "1--2", "38-40", "39-41", "35-31", "0-5", "68-70"})
        Require(!CfgKey::NormalizeSkipBlocks(invalid, normalized), "accepted invalid skip blocks");
    for (const char *value : {"1,2", "none", "auto", "bad"})
    {
        CfgKey::PutEnvString(CfgKey::SkipBlocks, value);
        sync();
        const char *expected = std::strcmp(value, "1,2") == 0 ? "1,2" :
                               ""; // 0.40 auto/invalid fall back to all 71 blocks.
        Require(skips(expected), "Runtime did not use the host skip block selection");
    }
    CfgKey::PutEnvString(CfgKey::SkipBlocks, "auto");
    sync();
    const auto excludes = reinterpret_cast<bool (*)()>(GetProcAddress(dll, "RuntimeExcludesReferenceExperiments"));
    Require(excludes && excludes(), "upstream reference experiments leaked into product defaults");
    CfgKey::PutEnvAlias(CfgKey::SwinRun, true);
    sync();
    Require(swin(1600, 960) && swin(1920, 1152), "Swin did not admit supported production tiers");
    Require(!swin(1280, 768), "Swin admitted unsupported tier");
    CfgKey::PutEnvAlias(CfgKey::SwinRun, false);
    sync();
    Require(!swin(1920, 1152), "Runtime ignored menu Swin disable");
    CfgKey::PutEnvAlias(CfgKey::SwinRun, true);
    CfgKey::PutEnvAlias(CfgKey::WaveOwned, false);
    sync();
    Require(!swin(1920, 1152), "Swin ignored wave-owned prerequisite");
    CfgKey::PutEnvAlias(CfgKey::WaveOwned, true);
    CfgKey::PutEnvString(CfgKey::SkipBlocks, "16");
    sync();
    Require(!swin(1920, 1152), "Swin ignored skipped internal block");
    for (bool waveOwned : {false, true})
    {
        CfgKey::PutEnvAlias(CfgKey::WaveOwned, waveOwned);
        for (const char *value : {"4", "69", "1,4", "42,43,46,69"})
        {
            CfgKey::PutEnvString(CfgKey::SkipBlocks, value);
            sync();
            Require(rejects(), "accepted FP8 raw chain with FP16-only skipped finish");
        }
    }
    // The full valid list exceeds the old PutEnvString fixed buffer once the key is added.
    std::string all;
    for (unsigned i = 1; i <= 69; ++i)
        if (i != 39)
        {
            if (!all.empty()) all += ',';
            all += std::to_string(i);
        }
    CfgKey::PutEnvString(CfgKey::SkipBlocks, all.c_str());
    sync();
    expect(CfgKey::SkipBlocks, all.c_str());
    Require(rejects(), "full list must retain 4/69 and reject the incompatible finish");
    CfgKey::PutEnvString(CfgKey::SkipBlocks, "42,43,46");
    sync();
    Require(skips("42,43,46"), "default skips rejected after unsupported selection");

    FreeLibrary(dll);
    std::puts("LMXXF_CONFIG_CRT_OK");
}
#endif
