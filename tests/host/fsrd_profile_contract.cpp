#define NOMINMAX
// Include real production Config and FSRDProfiles.
// Define Config::Config() = default to instantiate a real Config instance
// on the stack without loading INI files or running DLL initialization.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRDProfiles.h"
#include <any>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

Config::Config() = default;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fsrd_profile_contract:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (false)

using namespace FSRDProfiles;

static void ProfileFillAndExplicitZeroPreservation()
{
    // 1. Sparse load fill: FillUnspecified populates only unconfigured fields.
    Config sparse;
    FillUnspecified(sparse, Quality);
    CHECK(sparse.FfxDenoiserFloorEnabled.value() == true);
    CHECK(sparse.FfxDenoiserFloorFastMode.value() == false);
    CHECK(sparse.FfxDenoiserFloorRecovery.value() == 1.0f);
    CHECK(sparse.FfxDenoiserFloorDiffuseRecovery.value() == true);
    CHECK(sparse.FfxDenoiserFloorHandoverAnchorClamp.value() == 4.0f);
    CHECK(sparse.FfxDenoiserAdditiveLightSplit.value() == 0.0f);

    // 2. Explicit false and zero: FillUnspecified must never overwrite user values.
    Config explicitCfg;
    explicitCfg.FfxDenoiserFloorEnabled = false;
    explicitCfg.FfxDenoiserFloorRecovery = 0.0f;
    explicitCfg.FfxDenoiserFloorDiffuseRecovery = false;
    explicitCfg.FfxDenoiserAdditiveLightSplit = 0.5f;

    FillUnspecified(explicitCfg, Quality);
    CHECK(explicitCfg.FfxDenoiserFloorEnabled.value() == false);
    CHECK(explicitCfg.FfxDenoiserFloorRecovery.value() == 0.0f);
    CHECK(explicitCfg.FfxDenoiserFloorDiffuseRecovery.value() == false);
    CHECK(explicitCfg.FfxDenoiserAdditiveLightSplit.value() == 0.5f);
    // Unspecified fields were still filled
    CHECK(explicitCfg.FfxDenoiserFloorSpecularRecovery.value() == true);
    CHECK(explicitCfg.FfxDenoiserFloorHandoverAnchorClamp.value() == 4.0f);

    // Conflicting explicit values degrade detected profile to Custom.
    CHECK(Detect(explicitCfg) == Custom);
    CHECK(HasExplicitPolicyOverride(explicitCfg, Quality));
    CHECK(HasExplicitAnchorOverride(explicitCfg));
}

static void InteractiveApplyOverridesAndResets()
{
    // Interactive Apply is deliberately authoritative: it sets profile policies
    // unconditionally and resets anchor/modulation overrides to Auto.
    Config cfg;
    cfg.FfxDenoiserFloorEnabled = false;
    cfg.FfxDenoiserFloorRecovery = 0.0f;
    cfg.FfxDenoiserFloorHandoverAnchorClamp = 2.0f;
    cfg.FfxDenoiserAdditiveLightSplit = 0.75f;
    cfg.FfxDenoiserDiffuseRoute = FSRDSignals::Split;

    CHECK(Apply(cfg, Balanced) == Balanced);
    CHECK(cfg.FfxDenoiserProfile.value() == Balanced);
    CHECK(cfg.FfxDenoiserFloorEnabled.value() == true);
    CHECK(cfg.FfxDenoiserFloorFastMode.value() == true);
    CHECK(cfg.FfxDenoiserFloorRecovery.value() == 1.0f);
    CHECK(!cfg.FfxDenoiserFloorHandoverAnchorClamp.has_value()); // Reset to Auto
    CHECK(!cfg.FfxDenoiserAdditiveLightSplit.has_value());       // Reset to Auto
    CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Split); // Route unchanged
    CHECK(Detect(cfg) == Balanced);
    CHECK(!HasExplicitPolicyOverride(cfg, Balanced));
    CHECK(!HasExplicitAnchorOverride(cfg));
}

static void SaveAndReloadPersistence()
{
    std::map<std::string, std::any> store;
    auto writer = [&](const char* key, auto opt) {
        if (opt.has_value()) store[key] = *opt;
        else store.erase(key);
    };
    auto reader = [&](const char* key, auto typeId) -> std::optional<typename decltype(typeId)::type> {
        using T = typename decltype(typeId)::type;
        auto it = store.find(key);
        if (it != store.end()) return std::any_cast<T>(it->second);
        return std::nullopt;
    };

    // Scenario A: User saved explicit FloorEnabled=false and Recovery=0 with Profile=Quality.
    // Save writes Detect(cfg) which is Custom, preserving individual manual switches.
    {
        Config cfg;
        cfg.FfxDenoiserProfile = Quality;
        cfg.FfxDenoiserFloorEnabled = false;
        cfg.FfxDenoiserFloorRecovery = 0.0f;
        cfg.FfxDenoiserFloorSpecularRecovery = true;
        cfg.FfxDenoiserFloorHandoverAnchorClamp = 3.5f;
        Save(cfg, writer);
        CHECK(std::any_cast<int>(store[CfgKey::RrProfile]) == Custom);
        CHECK(std::any_cast<bool>(store[CfgKey::RrFloorEnabled]) == false);
        CHECK(std::any_cast<float>(store[CfgKey::RrFloorRecovery]) == 0.0f);
    }
    // Reload into a fresh configuration:
    {
        Config reloaded;
        Load(reloaded, reader);
        CHECK(reloaded.FfxDenoiserProfile.value() == Custom);
        CHECK(reloaded.FfxDenoiserFloorEnabled.value() == false);
        CHECK(reloaded.FfxDenoiserFloorRecovery.value() == 0.0f);
        CHECK(reloaded.FfxDenoiserFloorSpecularRecovery.value() == true);
        CHECK(reloaded.FfxDenoiserFloorHandoverAnchorClamp.value() == 3.5f);
    }

    // Scenario B: Interactive Clean Profile Save & Reload.
    store.clear();
    {
        Config cfg;
        Apply(cfg, Quality);
        Save(cfg, writer);
        CHECK(std::any_cast<int>(store[CfgKey::RrProfile]) == Quality);
    }
    {
        Config reloaded;
        Load(reloaded, reader);
        CHECK(reloaded.FfxDenoiserProfile.value() == Quality);
        // FillUnspecified filled the Quality defaults on load
        CHECK(reloaded.FfxDenoiserFloorEnabled.value() == true);
        CHECK(reloaded.FfxDenoiserFloorFastMode.value() == false);
        CHECK(reloaded.FfxDenoiserFloorRecovery.value() == 1.0f);
        CHECK(reloaded.FfxDenoiserFloorDiffuseRecovery.value() == true);
        CHECK(Detect(reloaded) == Quality);
    }

    // Scenario C: Legacy signal type migration on Load.
    store.clear();
    store["DiffuseSignalType"] = 0;   // 0 -> Direct
    store["SpecularSignalType"] = 1;  // 1 -> Indirect
    {
        Config legacyLoaded;
        Load(legacyLoaded, reader);
        CHECK(legacyLoaded.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Direct);
        CHECK(legacyLoaded.FfxDenoiserSpecularRoute.value() == FSRDSignals::Indirect);
    }

    // Scenario D: Canonical route 'auto' wins over old DiffuseSignalType,
    // while an unconfigured specular route migrates old SpecularSignalType.
    store.clear();
    store[CfgKey::RrDiffuseSignal] = std::string("auto");
    store["DiffuseSignalType"] = 1;  // Legacy Indirect
    store["SpecularSignalType"] = 1; // Legacy Indirect
    {
        Config mixed;
        Load(mixed, reader);
        CHECK(mixed.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Auto);
        CHECK(mixed.FfxDenoiserSpecularRoute.value() == FSRDSignals::Indirect);
    }
}

int main()
{
    ProfileFillAndExplicitZeroPreservation();
    InteractiveApplyOverridesAndResets();
    SaveAndReloadPersistence();
    std::puts("fsrd_profile_contract: PASS (production Config instance profile, explicit preference, fill and save/reload)");
}
