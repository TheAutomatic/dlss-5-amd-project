// Disk regression for the real Config.cpp + Util.cpp + CSimpleIni consumer.
// Every load/save phase runs in a new process: neither CustomOptional's session
// priority nor Config.cpp's static CSimpleIni can conceal a broken restart.
// The DLL identity is a real fixture module inside a fresh temporary directory;
// production Config's implicit constructor load never visits a user's INI.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Util.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRDProfiles.h"
#include <SimpleIni.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fsrd_ini_roundtrip:%d: %s\n", __LINE__, #condition); \
    throw std::runtime_error("INI regression assertion"); } } while (false)

static constexpr char FullIni[] = R"ini([FSR-RR]
Enabled=true
Profile=-1
UseAmdDefaults=true
DisocclusionThreshold=0.125
CrossBilateralNormalStrength=0.25
TemporalStabilityBias=0.75
MaxRadiance=12345
RadianceClipDeviation=17
GaussianKernelRelaxation=0.625
HardwareDepth=false
ProviderIndex=3
DebugViewLinearDepthMax=512
DebugViewViewport=2
InternalDebugViews=true
Diagnostics=true
DiffuseHitDistance=false
FloorEnabled=false
FloorFastMode=false
FloorRecovery=0
SpecularAlbedoDemodulation=0.5
DiffuseAlbedoModulation=0.75
AdditiveLightSplit=0.25
AlbedoBleedFix=true
AlbedoBleedFixDiffuse=false
FloorFlatRecovery=false
FloorSpecularRecovery=false
FloorDiffuseRecovery=true
FloorFlatNoiseMethod=2
FloorSpecularNoiseMethod=0
FloorDiffuseNoiseMethod=2
FloorLumaRecovery=0.25
FloorChromaRecovery=0.5
FloorHandoverCorrelationMix=0.75
FloorHandoverAnchorClamp=2.5
DemodDivisorFloor=0.015625
DenoiseDiffuse=false
DenoiseSpecular=true
EstimateHitDistances=true
GpuTimings=false
TaggedNormalRoughness=true
TaggedAmbientOcclusion=false
NormalsInViewSpace=true
UseTitleLinearDepth=true
ResponsivityTrustThreshold=0
ResponsivityInvert=true
BiasMaskStrength=0
DiffuseSignal=split
SpecularSignal=indirect
DebugMode=1099511627781
[Unrelated.User]
Keep=not-owned-by-RR
)ini";

static constexpr const char* RetiredKeys[] = {
    "CorrelationBias", "FloorIsolation", "RoughnessFloor", "FloorHandover",
    "FloorHandoverStrength", "FloorRawBlend", "FloorStructureGate", "FloorClampSmoothing",
    "FloorHandoverDetail", "DiffuseSignalType", "SpecularSignalType", "FloorDetailBoost",
    "FloorNormalSharpness", "FloorAlbedoGuide", "FloorLumSymmetry", "FloorGrazingSharpness",
    "FloorEnvelopeBias", "FloorSoftMin"
};

static void WriteIni(const fs::path& directory, const std::string& text)
{
    fs::create_directories(directory);
    std::ofstream stream(directory / L"OptiScaler.ini", std::ios::binary | std::ios::trunc);
    CHECK(stream.good());
    stream << text;
    stream.close();
    CHECK(stream.good());
}

static std::string Value(CSimpleIniA& ini, const char* key, const char* section = "FSR-RR")
{
    const char* value = ini.GetValue(section, key, nullptr);
    CHECK(value != nullptr);
    return value;
}

static void LoadDisk(CSimpleIniA& ini, const fs::path& directory)
{
    CHECK(ini.LoadFile((directory / L"OptiScaler.ini").c_str()) == SI_OK);
}

static void CheckFullConfig(Config& cfg)
{
#define FIELD(member, expected) CHECK(cfg.member.has_value() && cfg.member.value() == (expected))
    FIELD(FsrRrEnabled, true);
    FIELD(FfxDenoiserProfile, FSRDProfiles::Custom);
    FIELD(FfxDenoiserUseAmdDefaults, true);
    FIELD(FfxDenoiserDisocThreshold, 0.125f);
    FIELD(FfxDenoiserCrossBlNormStr, 0.25f);
    FIELD(FfxDenoiserStabilityBias, 0.75f);
    FIELD(FfxDenoiserMaxRadiance, 12345.0f);
    FIELD(FfxDenoiserRadianceClip, 17.0f);
    FIELD(FfxDenoiserGaussKernRelax, 0.625f);
    FIELD(FfxDenoiserHardwareDepth, false);
    FIELD(FfxDenoiserIndex, 3);
    FIELD(FfxDenoiserDebugDepthMax, 512.0f);
    FIELD(FfxDenoiserDebugViewport, 2);
    FIELD(FfxDenoiserInternalDebugViews, true);
    FIELD(FfxDenoiserDiagnostics, true);
    FIELD(FfxDenoiserDiffuseHitDistance, false);
    FIELD(FfxDenoiserFloorEnabled, false);
    FIELD(FfxDenoiserFloorFastMode, false);
    FIELD(FfxDenoiserFloorRecovery, 0.0f);
    FIELD(FfxDenoiserSpecularAlbedoDemodulation, 0.5f);
    FIELD(FfxDenoiserDiffuseAlbedoModulation, 0.75f);
    FIELD(FfxDenoiserAdditiveLightSplit, 0.25f);
    FIELD(FfxDenoiserUnsupportedAlbedoRecovery, true);
    FIELD(FfxDenoiserAlbedoBleedFixDiffuse, false);
    FIELD(FfxDenoiserFloorFlatRecovery, false);
    FIELD(FfxDenoiserFloorSpecularRecovery, false);
    FIELD(FfxDenoiserFloorDiffuseRecovery, true);
    FIELD(FfxDenoiserFloorFlatNoiseMethod, 2);
    FIELD(FfxDenoiserFloorSpecularNoiseMethod, 0);
    FIELD(FfxDenoiserFloorDiffuseNoiseMethod, 2);
    FIELD(FfxDenoiserFloorLumaRecovery, 0.25f);
    FIELD(FfxDenoiserFloorChromaRecovery, 0.5f);
    FIELD(FfxDenoiserFloorHandoverCorrelationMix, 0.75f);
    FIELD(FfxDenoiserFloorHandoverAnchorClamp, 2.5f);
    FIELD(FfxDenoiserDemodDivisorFloor, 0.015625f);
    FIELD(FfxDenoiserDenoiseDiffuse, false);
    FIELD(FfxDenoiserDenoiseSpecular, true);
    FIELD(FfxDenoiserEstimateHitDistances, true);
    FIELD(FfxDenoiserGpuTimings, false);
    FIELD(FfxDenoiserTaggedNormalRoughness, true);
    FIELD(FfxDenoiserTaggedAmbientOcclusion, false);
    FIELD(FfxDenoiserNormalsInViewSpace, true);
    FIELD(FfxDenoiserUseTitleLinearDepth, true);
    FIELD(FfxDenoiserResponsivityThreshold, 0.0f);
    FIELD(FfxDenoiserResponsivityInvert, true);
    FIELD(FfxDenoiserBiasMaskStrength, 0.0f);
    FIELD(FfxDenoiserDiffuseRoute, FSRDSignals::Split);
    FIELD(FfxDenoiserSpecularRoute, FSRDSignals::Indirect);
    FIELD(FfxDenoiserDebugMode, uint64_t{1099511627781});
#undef FIELD
    CHECK(cfg.GetFfxDenoiserProfile() == FSRDProfiles::Custom);
}

static void CheckResetOptionals(const Config& cfg)
{
#define UNSET(member) CHECK(!cfg.member.has_value())
    UNSET(FfxDenoiserProfile);
    UNSET(FfxDenoiserUseAmdDefaults);
    UNSET(FfxDenoiserDisocThreshold);
    UNSET(FfxDenoiserCrossBlNormStr);
    UNSET(FfxDenoiserStabilityBias);
    UNSET(FfxDenoiserMaxRadiance);
    UNSET(FfxDenoiserRadianceClip);
    UNSET(FfxDenoiserGaussKernRelax);
    UNSET(FfxDenoiserHardwareDepth);
    UNSET(FfxDenoiserIndex);
    UNSET(FfxDenoiserDebugDepthMax);
    UNSET(FfxDenoiserDebugViewport);
    UNSET(FfxDenoiserInternalDebugViews);
    UNSET(FfxDenoiserDiagnostics);
    UNSET(FfxDenoiserDiffuseHitDistance);
    UNSET(FfxDenoiserFloorEnabled);
    UNSET(FfxDenoiserFloorFastMode);
    UNSET(FfxDenoiserFloorRecovery);
    UNSET(FfxDenoiserSpecularAlbedoDemodulation);
    UNSET(FfxDenoiserDiffuseAlbedoModulation);
    UNSET(FfxDenoiserAdditiveLightSplit);
    UNSET(FfxDenoiserUnsupportedAlbedoRecovery);
    UNSET(FfxDenoiserAlbedoBleedFixDiffuse);
    UNSET(FfxDenoiserFloorFlatRecovery);
    UNSET(FfxDenoiserFloorSpecularRecovery);
    UNSET(FfxDenoiserFloorDiffuseRecovery);
    UNSET(FfxDenoiserFloorFlatNoiseMethod);
    UNSET(FfxDenoiserFloorSpecularNoiseMethod);
    UNSET(FfxDenoiserFloorDiffuseNoiseMethod);
    UNSET(FfxDenoiserFloorLumaRecovery);
    UNSET(FfxDenoiserFloorChromaRecovery);
    UNSET(FfxDenoiserFloorHandoverCorrelationMix);
    UNSET(FfxDenoiserFloorHandoverAnchorClamp);
    UNSET(FfxDenoiserDemodDivisorFloor);
    UNSET(FfxDenoiserDenoiseDiffuse);
    UNSET(FfxDenoiserDenoiseSpecular);
    UNSET(FfxDenoiserEstimateHitDistances);
    UNSET(FfxDenoiserGpuTimings);
    UNSET(FfxDenoiserTaggedNormalRoughness);
    UNSET(FfxDenoiserTaggedAmbientOcclusion);
    UNSET(FfxDenoiserNormalsInViewSpace);
    UNSET(FfxDenoiserUseTitleLinearDepth);
    UNSET(FfxDenoiserResponsivityThreshold);
    UNSET(FfxDenoiserResponsivityInvert);
    UNSET(FfxDenoiserBiasMaskStrength);
    UNSET(FfxDenoiserDiffuseRoute);
    UNSET(FfxDenoiserSpecularRoute);
    UNSET(FfxDenoiserDebugMode);
#undef UNSET
    CHECK(cfg.FsrRrEnabled.value() == true); // Separate master gate is not Reset.
}

static void CheckCanonicalDisk(const fs::path& directory, bool reset)
{
    CSimpleIniA expected, actual;
    CHECK(expected.LoadData(FullIni) == SI_OK);
    LoadDisk(actual, directory);
    CSimpleIniA::TNamesDepend keys, actualKeys;
    CHECK(expected.GetAllKeys("FSR-RR", keys));
    CHECK(actual.GetAllKeys("FSR-RR", actualKeys));
    CHECK(keys.size() == 49);
    CHECK(actualKeys.size() == keys.size());
    for (const auto& entry : keys)
    {
        const char* key = entry.pItem;
        CHECK(CfgKey::IsKnown(key));
        const std::string saved = Value(actual, key);
        if (reset)
        {
            const std::string wanted = std::strcmp(key, "Enabled") == 0 ? "true" :
                std::strcmp(key, "Profile") == 0 ? "1" : "auto";
            CHECK(saved == wanted);
        }
        else
        {
            const std::string wanted = Value(expected, key);
            // Numeric formatting is not the contract (SaveIni uses to_string).
            char *wantedEnd = nullptr, *savedEnd = nullptr;
            const double wantNumber = std::strtod(wanted.c_str(), &wantedEnd);
            const double savedNumber = std::strtod(saved.c_str(), &savedEnd);
            if (wantedEnd != wanted.c_str() && *wantedEnd == '\0')
                CHECK(savedEnd != saved.c_str() && *savedEnd == '\0' && savedNumber == wantNumber);
            else
                CHECK(saved == wanted);
        }
    }
    CHECK(Value(actual, "Keep", "Unrelated.User") == "not-owned-by-RR");
}

static void CheckProfile(Config& cfg, int profile)
{
    CHECK(cfg.GetFfxDenoiserProfile() == profile);
    CHECK(cfg.FfxDenoiserFloorEnabled.value() == (profile != FSRDProfiles::Fast));
    CHECK(cfg.FfxDenoiserFloorFastMode.value() == (profile == FSRDProfiles::Balanced));
    CHECK(cfg.FfxDenoiserFloorRecovery.value() == (profile == FSRDProfiles::Fast ? 0.0f : 1.0f));
    CHECK(cfg.FfxDenoiserFloorDiffuseRecovery.value() == (profile == FSRDProfiles::Quality));
}

static int Child(const fs::path& directory, const std::string& phase)
{
    spdlog::set_level(spdlog::level::off);
    // Set the real global consumed by Util.cpp before any Config exists.
    dllModule = LoadLibraryExW((directory / L"fsrd_ini_identity.dll").c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(dllModule != nullptr);
    CHECK(Util::DllPath() == directory / L"fsrd_ini_identity.dll");
    Config& cfg = *Config::Instance(); // Real constructor -> real Reload.

    if (phase.starts_with("routes-"))
    {
        const int diffuse = phase.at(7) - '0';
        const int specular = phase.at(9) - '0';
        CHECK(cfg.FfxDenoiserDiffuseRoute.value_or_default() == diffuse);
        CHECK(cfg.FfxDenoiserSpecularRoute.value_or_default() == specular);
        CSimpleIniA saved;
        LoadDisk(saved, directory);
        CHECK(Value(saved, "DiffuseSignal") == FSRDSignals::RouteCodes[diffuse]);
        CHECK(Value(saved, "SpecularSignal") == FSRDSignals::RouteCodes[specular]);
        CHECK(saved.GetValue("FSR-RR", "DiffuseSignalType", nullptr) == nullptr);
        CHECK(saved.GetValue("FSR-RR", "SpecularSignalType", nullptr) == nullptr);
    }
    else if (phase == "auto-diffuse")
    {
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Auto);
        CHECK(cfg.FfxDenoiserSpecularRoute.value() == FSRDSignals::Indirect);
        CHECK(cfg.SaveIni());
    }
    else if (phase == "auto-specular")
    {
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Direct);
        CHECK(cfg.FfxDenoiserSpecularRoute.value() == FSRDSignals::Auto);
        CHECK(cfg.SaveIni());
    }
    else if (phase == "legacy-diffuse")
    {
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Indirect);
        CHECK(!cfg.FfxDenoiserSpecularRoute.has_value());
        CHECK(cfg.SaveIni());
    }
    else if (phase == "legacy-specular")
    {
        CHECK(!cfg.FfxDenoiserDiffuseRoute.has_value());
        CHECK(cfg.FfxDenoiserSpecularRoute.value() == FSRDSignals::Direct);
        CHECK(cfg.SaveIni());
    }
    else if (phase == "sparse-quality")
    {
        CheckProfile(cfg, FSRDProfiles::Quality);
        CHECK(cfg.FfxDenoiserFloorFlatNoiseMethod.value() == 0);
        CHECK(cfg.FfxDenoiserFloorHandoverAnchorClamp.value() == 4.0f);
        CHECK(cfg.FfxDenoiserAdditiveLightSplit.value() == 0.0f);
        CHECK(cfg.SaveIni());
    }
    else if (phase == "explicit-zero" || phase == "explicit-restart")
    {
        CHECK(cfg.FfxDenoiserFloorEnabled.value() == false);
        CHECK(cfg.FfxDenoiserFloorRecovery.value() == 0.0f);
        CHECK(cfg.FfxDenoiserFloorDiffuseRecovery.value() == false);
        CHECK(cfg.FfxDenoiserFloorFastMode.value() == false);
        CHECK(cfg.FfxDenoiserFloorSpecularRecovery.value() == true);
        CHECK(cfg.FfxDenoiserFloorHandoverAnchorClamp.value() == 2.5f);
        CHECK(cfg.FfxDenoiserAdditiveLightSplit.value() == 0.5f);
        CHECK(cfg.GetFfxDenoiserProfile() == FSRDProfiles::Custom);
        CHECK(cfg.FsrRrEnabled.value() == false);
        CHECK(cfg.SaveIni());
        CSimpleIniA saved;
        LoadDisk(saved, directory);
        CHECK(Value(saved, "Profile") == "-1");
        CHECK(Value(saved, "FloorEnabled") == "false");
        CHECK(std::stof(Value(saved, "FloorRecovery")) == 0.0f);
        CHECK(Value(saved, "Enabled") == "false");
    }
    else if (phase == "apply")
    {
        CheckFullConfig(cfg);
        cfg.ApplyFfxDenoiserProfile(FSRDProfiles::Balanced);
        CheckProfile(cfg, FSRDProfiles::Balanced);
        CHECK(!cfg.FfxDenoiserFloorHandoverAnchorClamp.has_value());
        CHECK(!cfg.FfxDenoiserAdditiveLightSplit.has_value());
        CHECK(!cfg.FfxDenoiserSpecularAlbedoDemodulation.has_value());
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Split);
        CHECK(cfg.SaveIni());
        CSimpleIniA saved;
        LoadDisk(saved, directory);
        CHECK(Value(saved, "Profile") == "1");
        CHECK(Value(saved, "FloorHandoverAnchorClamp") == "auto");
        CHECK(Value(saved, "SpecularAlbedoDemodulation") == "auto");
    }
    else if (phase == "apply-restart")
    {
        CheckProfile(cfg, FSRDProfiles::Balanced);
        CHECK(cfg.FfxDenoiserFloorHandoverAnchorClamp.value() == 4.0f);
        CHECK(cfg.FfxDenoiserAdditiveLightSplit.value() == 0.0f);
        CHECK(cfg.FfxDenoiserSpecularAlbedoDemodulation.value() == 1.0f);
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Split);
        CHECK(cfg.FfxDenoiserDebugMode.value() == uint64_t{1099511627781});
        CHECK(cfg.SaveIni());
    }
    else if (phase == "retire" || phase == "retire-restart")
    {
        CheckProfile(cfg, FSRDProfiles::Balanced);
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Direct);
        CHECK(cfg.FfxDenoiserSpecularRoute.value() == FSRDSignals::Indirect);
        CHECK(cfg.SaveIni());
        CSimpleIniA saved;
        LoadDisk(saved, directory);
        for (size_t i = 0; i < std::size(RetiredKeys); ++i)
        {
            CHECK(saved.GetValue("FSR-RR", RetiredKeys[i], nullptr) == nullptr);
            const std::string expected = i == 9 ? "0" : i == 10 ? "1" : "legacy-" + std::to_string(i);
            CHECK(Value(saved, RetiredKeys[i], "FSR-RR.Retired") == expected);
        }
        CHECK(Value(saved, "ForeignSetting") == "keep-active");
    }
    else if (phase == "full" || phase == "full-restart")
    {
        CheckFullConfig(cfg);
        CHECK(cfg.SaveIni());
        CheckCanonicalDisk(directory, false);
    }
    else if (phase == "reset")
    {
        CheckFullConfig(cfg);
        CHECK(cfg.ResetFfxDenoiserSettings());
        CheckResetOptionals(cfg);
        CHECK(!cfg.ResetFfxDenoiserSettings());
        CHECK(cfg.SaveIni());
        CheckCanonicalDisk(directory, true);
    }
    else if (phase == "reset-restart")
    {
        CheckProfile(cfg, FSRDProfiles::Balanced);
        CHECK(cfg.FsrRrEnabled.value() == true);
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Auto);
        CHECK(cfg.FfxDenoiserSpecularRoute.value() == FSRDSignals::Auto);
        CHECK(!cfg.FfxDenoiserHardwareDepth.has_value());
        CHECK(!cfg.FfxDenoiserDebugMode.has_value());
        CHECK(!cfg.FfxDenoiserNormalsInViewSpace.has_value());
        CHECK(!cfg.FfxDenoiserTaggedNormalRoughness.has_value());
        CHECK(!cfg.FfxDenoiserUseAmdDefaults.has_value());
        CHECK(cfg.FfxDenoiserDebugMode.value_or_default() == 0);
        CHECK(cfg.FfxDenoiserBiasMaskStrength.value_or_default() == 1.0f);
        CHECK(cfg.FfxDenoiserDenoiseDiffuse.value_or_default() == true);
        CHECK(cfg.FfxDenoiserFloorHandoverAnchorClamp.value() == 4.0f);
    }
    else if (phase == "explicit-path")
    {
        // This public entry calls the same private Reload used by the constructor.
        // The initial fixture INI is empty of RR so session priority is not bypassed.
        CHECK(!cfg.FsrRrEnabled.has_value());
        CHECK(cfg.LoadFromPath((directory / L"explicit").c_str()));
        CHECK(cfg.FsrRrEnabled.value() == true);
        CHECK(cfg.FfxDenoiserDiffuseRoute.value() == FSRDSignals::Split);
        CHECK(cfg.SaveIni());
        CSimpleIniA initial, saved;
        LoadDisk(initial, directory);
        LoadDisk(saved, directory / L"explicit");
        CHECK(initial.GetValue("FSR-RR", "Enabled", nullptr) == nullptr);
        CHECK(Value(saved, "Enabled") == "true");
        CHECK(Value(saved, "DiffuseSignal") == "split");
    }
    else
        CHECK(false);
    return 0;
}

static std::wstring Quote(const std::wstring& text)
{
    CHECK(text.find(L'"') == std::wstring::npos);
    return L"\"" + text + L"\"";
}

static void RunPhase(const fs::path& exe, const fs::path& directory, const wchar_t* phase)
{
    std::wstring command = Quote(exe.wstring()) + L" --child " + Quote(directory.wstring()) + L" " + phase;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         directory.c_str(), &startup, &process));
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD result = 2;
    const BOOL gotExitCode = GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    CHECK(wait == WAIT_OBJECT_0 && gotExitCode && result == 0);
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--child")
            return Child(fs::path(argv[2]), wstring_to_string(argv[3]));
        CHECK(argc == 2);
        const fs::path identityFixture = fs::absolute(argv[1]);
        CHECK(fs::is_regular_file(identityFixture));
        wchar_t executable[32768]{};
        CHECK(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0);
        const fs::path exe(executable);
        const fs::path root = fs::temp_directory_path() /
            (L"fsrd actual ini \u6d4b\u8bd5 " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        CHECK(fs::create_directory(root));
        struct Cleanup
        {
            fs::path root;
            ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); }
        } cleanup{root};
        const auto setup = [&](const wchar_t* name, const std::string& text) {
            const auto directory = root / name;
            WriteIni(directory, text);
            CHECK(fs::copy_file(identityFixture, directory / L"fsrd_ini_identity.dll"));
            return directory;
        };
        const auto run = [&](const wchar_t* name, const std::string& text, const wchar_t* phase) {
            const auto directory = setup(name, text);
            RunPhase(exe, directory, phase);
            return directory;
        };
        const auto autoDiffuse = run(L"auto diffuse", "[FSR-RR]\nDiffuseSignal=AuTo\nDiffuseSignalType=1\nSpecularSignalType=1\n", L"auto-diffuse");
        RunPhase(exe, autoDiffuse, L"routes-0-2");
        const auto autoSpecular = run(L"auto specular", "[FSR-RR]\nSpecularSignal=auto\nDiffuseSignalType=0\nSpecularSignalType=1\n", L"auto-specular");
        RunPhase(exe, autoSpecular, L"routes-1-0");
        const auto legacyDiffuse = run(L"legacy diffuse", "[FSR-RR]\nDiffuseSignalType=1\n", L"legacy-diffuse");
        RunPhase(exe, legacyDiffuse, L"routes-2-0");
        const auto legacySpecular = run(L"legacy specular", "[FSR-RR]\nSpecularSignalType=0\n", L"legacy-specular");
        RunPhase(exe, legacySpecular, L"routes-0-1");
        const auto quality = run(L"quality", "[FSR-RR]\nProfile=2\n", L"sparse-quality");
        RunPhase(exe, quality, L"sparse-quality");
        const auto zero = setup(L"zero", "[FSR-RR]\nEnabled=false\nProfile=2\nFloorEnabled=false\nFloorRecovery=0\nFloorDiffuseRecovery=false\nFloorHandoverAnchorClamp=2.5\nAdditiveLightSplit=0.5\n");
        RunPhase(exe, zero, L"explicit-zero");
        RunPhase(exe, zero, L"explicit-restart");
        const auto applied = setup(L"apply", FullIni);
        RunPhase(exe, applied, L"apply");
        RunPhase(exe, applied, L"apply-restart");
        std::string retired = "[FSR-RR]\nProfile=1\nForeignSetting=keep-active\n";
        for (size_t i = 0; i < std::size(RetiredKeys); ++i)
            retired += std::string(RetiredKeys[i]) + "=" +
                (i == 9 ? "0" : i == 10 ? "1" : "legacy-" + std::to_string(i)) + "\n";
        const auto archived = setup(L"archive", retired);
        RunPhase(exe, archived, L"retire");
        RunPhase(exe, archived, L"retire-restart");
        const auto full = setup(L"full", FullIni);
        RunPhase(exe, full, L"full");
        RunPhase(exe, full, L"full-restart");
        RunPhase(exe, full, L"reset");
        RunPhase(exe, full, L"reset-restart");
        const auto explicitPath = setup(L"path", "[Unrelated.User]\nKeep=implicit-fixture-only\n");
        WriteIni(explicitPath / L"explicit", "[FSR-RR]\nEnabled=true\nDiffuseSignal=split\n");
        RunPhase(exe, explicitPath, L"explicit-path");
        std::puts("fsrd_ini_roundtrip: PASS (actual Config Reload/SaveIni, isolated disk restarts, 49 canonical keys, migration, profiles, archive, reset)");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "fsrd_ini_roundtrip: FAIL: %s\n", error.what());
        return 1;
    }
}
