// Build this file as a DLL and an EXE with /MT to exercise independent CRT caches.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h"

#ifdef LMXXF_CONFIG_CRT_FIXTURE
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
    Require(read && sync, "missing runtime fixture exports");
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

    FreeLibrary(dll);
    std::puts("LMXXF_CONFIG_CRT_OK");
}
#endif
