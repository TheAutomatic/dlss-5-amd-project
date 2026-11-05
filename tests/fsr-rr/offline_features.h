#pragma once
#include <climits>
#include "upscalers/fsr31/FSRDProfiles.h"

// Small Config-shaped fixture holder: production profile/signal policy stays authoritative.
// No Config singleton/INI loading or second implementation of MakePlan is involved.
template<class T> struct FixturePreference
{
    T fallback;
    std::optional<T> explicitValue;
    FixturePreference(T value) : fallback(value) {}
    FixturePreference& operator=(T value) { explicitValue = value; return *this; }
    T value_or_default() const { return explicitValue.value_or(fallback); }
    void reset() { explicitValue.reset(); }
};
struct OfflineFeatures
{
    std::string name = "baseline-compatible";
    std::string guides = "existing";
    bool capture = false, failureRecovery = false;
    FixturePreference<int> FfxDenoiserProfile{-1};
    FixturePreference<int> FfxDenoiserDiffuseRoute{1}, FfxDenoiserSpecularRoute{2};
    FixturePreference<bool> FfxDenoiserDenoiseDiffuse{true}, FfxDenoiserDenoiseSpecular{true};
    FixturePreference<bool> FfxDenoiserUnsupportedAlbedoRecovery{false}, FfxDenoiserEstimateHitDistances{false};
    FixturePreference<bool> FfxDenoiserAlbedoBleedFixDiffuse{true};
    FixturePreference<bool> FfxDenoiserFloorEnabled{true}, FfxDenoiserFloorFastMode{false};
    FixturePreference<float> FfxDenoiserFloorRecovery{0.35f};
    FixturePreference<bool> FfxDenoiserFloorFlatRecovery{true}, FfxDenoiserFloorSpecularRecovery{false}, FfxDenoiserFloorDiffuseRecovery{false};
    FixturePreference<int> FfxDenoiserFloorFlatNoiseMethod{0}, FfxDenoiserFloorSpecularNoiseMethod{0}, FfxDenoiserFloorDiffuseNoiseMethod{0};
    FixturePreference<float> FfxDenoiserFloorHandoverAnchorClamp{4.0f}, FfxDenoiserFloorHandoverCorrelationMix{1.0f};
    FixturePreference<float> FfxDenoiserFloorLumaRecovery{1.0f}, FfxDenoiserFloorChromaRecovery{1.0f};
    FixturePreference<float> FfxDenoiserSpecularAlbedoDemodulation{1.0f}, FfxDenoiserDiffuseAlbedoModulation{1.0f};
    FixturePreference<float> FfxDenoiserAdditiveLightSplit{0.0f};

    bool CandidateOnly() const { return name != "baseline-compatible"; }
    uint32_t RecoveryMask() const
    {
        return (FfxDenoiserFloorFlatRecovery.value_or_default() ? 1u : 0u) |
               (FfxDenoiserFloorSpecularRecovery.value_or_default() ? 2u : 0u) |
               (FfxDenoiserFloorDiffuseRecovery.value_or_default() ? 4u : 0u);
    }
    uint32_t TemporalMask() const
    {
        return RecoveryMask() & ((FfxDenoiserFloorFlatNoiseMethod.value_or_default() == 1 ? 1u : 0u) |
            (FfxDenoiserFloorSpecularNoiseMethod.value_or_default() == 1 ? 2u : 0u) |
            (FfxDenoiserFloorDiffuseNoiseMethod.value_or_default() == 1 ? 4u : 0u));
    }
    nlohmann::json Json() const
    {
        nlohmann::json j;
#define RR_SETTING(field) j[#field] = field.value_or_default()
        RR_SETTING(FfxDenoiserProfile); RR_SETTING(FfxDenoiserDiffuseRoute); RR_SETTING(FfxDenoiserSpecularRoute);
        RR_SETTING(FfxDenoiserDenoiseDiffuse); RR_SETTING(FfxDenoiserDenoiseSpecular);
        RR_SETTING(FfxDenoiserUnsupportedAlbedoRecovery); RR_SETTING(FfxDenoiserEstimateHitDistances);
        RR_SETTING(FfxDenoiserAlbedoBleedFixDiffuse); RR_SETTING(FfxDenoiserFloorEnabled);
        RR_SETTING(FfxDenoiserFloorFastMode); RR_SETTING(FfxDenoiserFloorRecovery);
        RR_SETTING(FfxDenoiserFloorFlatRecovery); RR_SETTING(FfxDenoiserFloorSpecularRecovery); RR_SETTING(FfxDenoiserFloorDiffuseRecovery);
        RR_SETTING(FfxDenoiserFloorFlatNoiseMethod); RR_SETTING(FfxDenoiserFloorSpecularNoiseMethod); RR_SETTING(FfxDenoiserFloorDiffuseNoiseMethod);
        RR_SETTING(FfxDenoiserFloorHandoverAnchorClamp); RR_SETTING(FfxDenoiserFloorHandoverCorrelationMix);
        RR_SETTING(FfxDenoiserFloorLumaRecovery); RR_SETTING(FfxDenoiserFloorChromaRecovery);
        RR_SETTING(FfxDenoiserSpecularAlbedoDemodulation); RR_SETTING(FfxDenoiserDiffuseAlbedoModulation); RR_SETTING(FfxDenoiserAdditiveLightSplit);
#undef RR_SETTING
        j["effective_profile"] = FSRDProfiles::Code(FSRDProfiles::Detect(*this));
        j["RecoveryMask"] = RecoveryMask(); j["SpatialTemporalMask"] = TemporalMask();
        return j;
    }
    void Set(const std::string& key, const std::string& value)
    {
        if (key == "guides")
        {
            if (value != "both" && value != "none" && value != "specular" && value != "dropout" && value != "arrival")
                throw std::runtime_error("guides must be both|none|specular|dropout|arrival");
            guides = value; return;
        }
        if (key == "capture") { capture = value == "1"; return; }
        if (key == "failure_recovery") { failureRecovery = value == "1"; return; }
        const auto unit = [&] { size_t end = 0; float f = std::stof(value, &end);
            if (end != value.size() || !std::isfinite(f) || f < 0 || f > 1) throw std::runtime_error("unit setting outside [0,1]: " + key); return f; };
#define RR_BOOL(field) if (key == #field) { if (value != "0" && value != "1") throw std::runtime_error("boolean setting requires 0|1: " + key); field = value == "1"; return; }
#define RR_UNIT(field) if (key == #field) { field = unit(); return; }
#define RR_METHOD(field) if (key == #field) { if (value != "0" && value != "1") throw std::runtime_error("noise method requires 0|1"); field = value == "1" ? 1 : 0; return; }
        RR_BOOL(FfxDenoiserDenoiseDiffuse) RR_BOOL(FfxDenoiserDenoiseSpecular)
        RR_BOOL(FfxDenoiserUnsupportedAlbedoRecovery) RR_BOOL(FfxDenoiserEstimateHitDistances)
        RR_BOOL(FfxDenoiserAlbedoBleedFixDiffuse) RR_BOOL(FfxDenoiserFloorEnabled) RR_BOOL(FfxDenoiserFloorFastMode)
        RR_BOOL(FfxDenoiserFloorFlatRecovery) RR_BOOL(FfxDenoiserFloorSpecularRecovery) RR_BOOL(FfxDenoiserFloorDiffuseRecovery)
        RR_UNIT(FfxDenoiserFloorRecovery) RR_UNIT(FfxDenoiserFloorLumaRecovery) RR_UNIT(FfxDenoiserFloorChromaRecovery)
        RR_UNIT(FfxDenoiserSpecularAlbedoDemodulation) RR_UNIT(FfxDenoiserDiffuseAlbedoModulation) RR_UNIT(FfxDenoiserAdditiveLightSplit)
        RR_METHOD(FfxDenoiserFloorFlatNoiseMethod) RR_METHOD(FfxDenoiserFloorSpecularNoiseMethod) RR_METHOD(FfxDenoiserFloorDiffuseNoiseMethod)
#undef RR_BOOL
#undef RR_UNIT
#undef RR_METHOD
        if (key == "FfxDenoiserDiffuseRoute" || key == "FfxDenoiserSpecularRoute")
        {
            if (value != "auto" && value != "direct" && value != "indirect" && value != "split") throw std::runtime_error("invalid signal route");
            (key == "FfxDenoiserDiffuseRoute" ? FfxDenoiserDiffuseRoute : FfxDenoiserSpecularRoute) = FSRDSignals::RouteFromCode(value); return;
        }
        throw std::runtime_error("unknown offline feature setting: " + key);
    }
    static std::vector<std::string> Cases()
    {
        std::vector<std::string> cases;
        for (const char* d : FSRDSignals::RouteCodes)
            for (const char* s : FSRDSignals::RouteCodes) cases.push_back(std::string("routes-") + d + "-" + s);
        for (const char* l : {"diffuse", "specular"})
            for (const char* r : FSRDSignals::RouteCodes) cases.push_back(std::string("only-") + l + "-" + r);
        for (const char* c : {"albedo-specular", "albedo-full", "albedo-missing-diffuse", "albedo-dropout", "albedo-arrival",
            "estimate-off", "estimate-on", "profile-fast", "profile-balanced", "profile-quality", "floor-off", "floor-fast", "floor-full",
            "recovery-none", "recovery-flat", "recovery-specular", "recovery-diffuse", "noise-spatial", "noise-temporal",
            "luma-off", "chroma-off", "master-off", "modulation-half", "additive-default", "additive-zero", "additive-half", "additive-full",
            "albedo-incompatible-modulation", "albedo-incompatible-additive", "capture", "failure-recovery"}) cases.emplace_back(c);
        return cases;
    }
    void Select(const std::string& selected)
    {
        const auto cases = Cases();
        if (std::find(cases.begin(), cases.end(), selected) == cases.end()) throw std::runtime_error("unknown feature case: " + selected);
        name = selected; guides = "both";
        FSRDProfiles::Apply(*this, FSRDProfiles::Quality);
        FfxDenoiserDiffuseRoute = FSRDSignals::Auto; FfxDenoiserSpecularRoute = FSRDSignals::Auto;
        if (selected.rfind("routes-", 0) == 0)
        {
            const size_t split = selected.find('-', 7);
            Set("FfxDenoiserDiffuseRoute", selected.substr(7, split - 7)); Set("FfxDenoiserSpecularRoute", selected.substr(split + 1));
        }
        else if (selected.rfind("only-", 0) == 0)
        {
            const size_t split = selected.find('-', 5); const bool diffuse = selected.substr(5, split - 5) == "diffuse";
            FfxDenoiserDenoiseDiffuse = diffuse; FfxDenoiserDenoiseSpecular = !diffuse;
            Set(diffuse ? "FfxDenoiserDiffuseRoute" : "FfxDenoiserSpecularRoute", selected.substr(split + 1));
        }
        else if (selected.rfind("albedo-", 0) == 0)
        {
            FfxDenoiserUnsupportedAlbedoRecovery = true;
            FfxDenoiserAlbedoBleedFixDiffuse = selected != "albedo-specular";
            if (selected == "albedo-missing-diffuse") guides = "specular";
            if (selected == "albedo-dropout") guides = "dropout";
            if (selected == "albedo-arrival") guides = "arrival";
            if (selected == "albedo-incompatible-modulation") FfxDenoiserDiffuseAlbedoModulation = 0.5f;
            if (selected == "albedo-incompatible-additive") FfxDenoiserAdditiveLightSplit = 0.5f;
        }
        else if (selected == "estimate-off" || selected == "estimate-on") { guides = "none"; FfxDenoiserEstimateHitDistances = selected == "estimate-on"; }
        else if (selected == "profile-fast" || selected == "floor-off") FSRDProfiles::Apply(*this, FSRDProfiles::Fast);
        else if (selected == "profile-balanced" || selected == "floor-fast") FSRDProfiles::Apply(*this, FSRDProfiles::Balanced);
        else if (selected == "recovery-none" || selected == "recovery-flat" || selected == "recovery-specular" || selected == "recovery-diffuse")
        {
            FfxDenoiserFloorFlatRecovery = selected == "recovery-flat"; FfxDenoiserFloorSpecularRecovery = selected == "recovery-specular";
            FfxDenoiserFloorDiffuseRecovery = selected == "recovery-diffuse";
        }
        else if (selected == "noise-spatial" || selected == "noise-temporal")
        {
            const int method = selected == "noise-temporal" ? 1 : 0;
            FfxDenoiserFloorFlatNoiseMethod = method; FfxDenoiserFloorSpecularNoiseMethod = method; FfxDenoiserFloorDiffuseNoiseMethod = method;
        }
        else if (selected == "luma-off") FfxDenoiserFloorLumaRecovery = 0.0f;
        else if (selected == "chroma-off") FfxDenoiserFloorChromaRecovery = 0.0f;
        else if (selected == "master-off") FfxDenoiserFloorRecovery = 0.0f;
        else if (selected == "modulation-half") { FfxDenoiserSpecularAlbedoDemodulation = 0.5f; FfxDenoiserDiffuseAlbedoModulation = 0.5f; }
        else if (selected == "additive-zero") FfxDenoiserAdditiveLightSplit = 0.0f;
        else if (selected == "additive-half") FfxDenoiserAdditiveLightSplit = 0.5f;
        else if (selected == "additive-full" || selected == "capture") { FfxDenoiserAdditiveLightSplit = 1.0f; capture = selected == "capture"; }
        else if (selected == "failure-recovery") failureRecovery = true;
    }
};

template<class C> constexpr bool HasOfflineFeatures = requires(C& c) { c.ConfigureSignalResources(true, true, true, true); };
template<class C> void ConfigureOfflineFeatures(C& c, const OfflineFeatures& f)
{
    if constexpr (HasOfflineFeatures<C>)
    {
        const auto r = FSRDSignals::RequestFrom(f);
        if (!c.ConfigureSignalResources(r.diffuse == FSRDSignals::Split || r.albedoFix,
            r.specular == FSRDSignals::Split || r.albedoFix, r.albedoFix, r.albedoFix && r.fixDiffuse))
            throw std::runtime_error("ConfigureSignalResources rejected fixture layout");
    }
    else if (f.CandidateOnly()) throw std::runtime_error("candidate-only feature case cannot run against frozen two-signal converter");
}
template<class D> void ApplyOfflineConversion(D& d, const OfflineFeatures& f, const FSRDSignals::Plan& plan,
    bool diffuseGuide, ID3D12Resource* distance, uint32_t x, uint32_t y)
{
    if constexpr (requires { d.AdditiveLightSplit; d.DiffuseHitDistanceMode; })
    {
        d.FloorEnabled = f.FfxDenoiserFloorEnabled.value_or_default(); d.FloorFastMode = f.FfxDenoiserFloorFastMode.value_or_default();
        d.FloorDetailPreservation = f.FfxDenoiserFloorRecovery.value_or_default(); d.RecoveryMask = f.RecoveryMask();
        d.SpecularAlbedoDemodulation = f.FfxDenoiserSpecularAlbedoDemodulation.value_or_default();
        d.DiffuseAlbedoModulation = f.FfxDenoiserDiffuseAlbedoModulation.value_or_default(); d.AdditiveLightSplit = f.FfxDenoiserAdditiveLightSplit.value_or_default();
        d.Resources.InDiffuseHitDistance = diffuseGuide ? distance : nullptr; d.DiffuseHitDistanceMode = diffuseGuide ? 1 : 0;
        d.SpecularHitDistanceBase = {x, y}; d.DiffuseHitDistanceBase = {x, y}; d.InputBase4.z = x; d.InputBase4.w = y;
        const bool fix = plan.albedoFix && FSRDSignals::AlbedoFixAllowed(f);
        if (fix) d.Flags |= 1u << 28;
        if ((plan.mask & 5u) == 5u && !(fix && plan.diffuseCopy)) d.Flags |= 1u << 24;
        if ((plan.mask & 10u) == 10u && !fix) d.Flags |= 1u << 25;
        if (f.FfxDenoiserEstimateHitDistances.value_or_default()) d.Flags |= 1u << 26;
        if (f.FfxDenoiserEstimateHitDistances.value_or_default() || plan.diffuseCopy) d.Flags |= 1u << 27;
    }
}
template<class D> void ApplyOfflineComposition(D& d, const OfflineFeatures& f, const FSRDSignals::Plan& plan)
{
    if constexpr (requires { d.SpatialTemporalMask; d.UnsupportedAlbedoRecovery; })
    {
        const bool fix = plan.albedoFix && FSRDSignals::AlbedoFixAllowed(f);
        d.FloorDetailPreservation = f.FfxDenoiserFloorEnabled.value_or_default() ? f.FfxDenoiserFloorRecovery.value_or_default() : 0;
        d.RecoveryMask = f.RecoveryMask(); d.SpatialTemporalMask = f.TemporalMask();
        d.LumaRecovery = f.FfxDenoiserFloorLumaRecovery.value_or_default(); d.ChromaRecovery = f.FfxDenoiserFloorChromaRecovery.value_or_default();
        d.SpecularAlbedoDemodulation = f.FfxDenoiserSpecularAlbedoDemodulation.value_or_default();
        d.DiffuseAlbedoModulation = f.FfxDenoiserDiffuseAlbedoModulation.value_or_default();
        d.FloorHandoverAnchorClamp = f.FfxDenoiserFloorHandoverAnchorClamp.value_or_default();
        d.FloorHandoverCorrelationMix = f.FfxDenoiserFloorHandoverCorrelationMix.value_or_default(); d.UnsupportedAlbedoRecovery = fix ? 1.0f : 0.0f;
        if (!(plan.mask & 5u)) d.Flags |= 1u << 4;
        if (!(plan.mask & 10u)) d.Flags |= 1u << 5;
        if ((plan.mask & 5u) == 5u && !(fix && plan.diffuseCopy)) d.Flags |= 1u << 6;
        if ((plan.mask & 10u) == 10u && !fix) d.Flags |= 1u << 7;
        if (fix && plan.diffuseCopy) d.Flags |= 1u << 8;
    }
}
struct OfflineSignalChain
{
    ffxDispatchDescDenoiserDirectDiffuse diffuse{};
    ffxDispatchDescDenoiserIndirectSpecular specular{};
    ffxDispatchDescDenoiserDirectSpecular extraSpecular{};
    ffxDispatchDescDenoiserIndirectDiffuse extraDiffuse{};
    std::array<ffxDispatchDescHeader*, 4> headers{};
    size_t count = 0;
    template<class C> void Fill(C& c, ffxDispatchDescDenoiser& d, const OfflineFeatures& f, const FSRDSignals::Plan& plan)
    {
        c.GetSignals(d, diffuse, specular);
        diffuse.header.type = plan.mask & 1u ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE : FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
        specular.header.type = plan.mask & 8u ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR : FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
        if (plan.mask & 5u) headers[count++] = &diffuse.header; else d.diffuseAlbedo = {};
        if (plan.mask & 10u) headers[count++] = &specular.header; else d.specularAlbedo = {};
        if constexpr (HasOfflineFeatures<C>)
        {
            const bool fix = plan.albedoFix && FSRDSignals::AlbedoFixAllowed(f);
            if ((plan.mask & 10u) == 10u) { c.GetDirectSpecularSignal(extraSpecular, fix); headers[count++] = &extraSpecular.header; }
            if ((plan.mask & 5u) == 5u) { c.GetIndirectDiffuseSignal(extraDiffuse, fix && plan.diffuseCopy); headers[count++] = &extraDiffuse.header; }
        }
        std::sort(headers.begin(), headers.begin() + count, [](const auto* a, const auto* b) { return a->type < b->type; });
        d.header.pNext = count ? headers[0] : nullptr;
        for (size_t i = 0; i < count; ++i) headers[i]->pNext = i + 1 < count ? headers[i + 1] : nullptr;
    }
};
template<class C> bool RequestOfflineCapture(C&, bool request, uint32_t x = 0, uint32_t y = 0, uint32_t size = 16)
{
    if constexpr (requires { C::RequestAdditiveCapture(x, y, size); }) return !request || C::RequestAdditiveCapture(x, y, size);
    return !request;
}
template<class C, class Comp> void CompleteOfflineCapture(C& c, ID3D12GraphicsCommandList* cl,
    const ffxDispatchDescDenoiser& dd, const Comp& comp, float exposure, bool provided,
    float nearPlane, float farPlane, bool infiniteFar, bool inverted, bool encoded)
{
    if constexpr (HasOfflineFeatures<C>)
    {
        typename C::SRDepthDiagnostic depth{nearPlane, farPlane, infiniteFar, inverted, encoded};
        c.CompleteAdditiveCapture(cl, dd, comp, exposure, provided, depth);
    }
}
template<class C> std::string OfflineCaptureStatus(C&)
{
    if constexpr (requires { C::GetAdditiveCaptureStatus(); }) return C::GetAdditiveCaptureStatus();
    return "unsupported-frozen-converter";
}

// Streaming evidence hashes avoid retaining a second copy of all fixture frames.
class OfflineEvidenceHash
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
  public:
    OfflineEvidenceHash()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("cannot open offline evidence SHA256");
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
            throw std::runtime_error("cannot create offline evidence SHA256");
        }
    }
    ~OfflineEvidenceHash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void Add(const void* bytes, size_t size)
    {
        if (size > ULONG_MAX || (size && BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)), ULONG(size), 0) < 0))
            throw std::runtime_error("cannot hash offline evidence bytes");
    }
    template<class T> void Plane(const std::vector<T>& values)
    {
        const uint64_t count = values.size(); Add(&count, sizeof count); Add(values.data(), values.size() * sizeof(T));
    }
    std::string Finish()
    {
        std::array<UCHAR, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) < 0) throw std::runtime_error("cannot finish offline evidence SHA256");
        std::string hex; hex.reserve(64); const char* digits = "0123456789abcdef";
        for (UCHAR b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
        return hex;
    }
};
template<class C, class D> void ExerciseOfflineFailure(C& c, ID3D12GraphicsCommandList* cl, const D& valid)
{
    if constexpr (HasOfflineFeatures<C>)
    {
        D invalid = valid; invalid.RenderSize.x = 0;
        if (c.DispatchConversion(cl, invalid)) throw std::runtime_error("invalid conversion unexpectedly accepted");
        c.FinishCompositionHistory(false);
        if (c.GetUpscalerDepthInput() != nullptr) throw std::runtime_error("failed conversion retained encoded SR depth");
    }
}
template<class C> void ExerciseOfflineBypass(C& c, ID3D12GraphicsCommandList* cl)
{
    if constexpr (HasOfflineFeatures<C>)
    {
        c.TransitionDenoiserOutputsToRead(cl);
        c.FinishCompositionHistory(false);
    }
}

// Read the real request-driven export only after its production ticket completed.
// This records channel energy and authenticates every raw image against capture.json.
template<class C, class HashFile> nlohmann::json ReadOfflineCapture(C& c, const std::filesystem::path& destination, HashFile hashFile)
{
    const std::string status = OfflineCaptureStatus(c);
    constexpr const char* prefix = "Saved additive RRTrace: ";
    if (status.rfind(prefix, 0) != 0) throw std::runtime_error("additive capture did not export: " + status);
    const std::filesystem::path source = std::filesystem::u8path(status.substr(std::strlen(prefix)));
    std::ifstream metadata(source / "capture.json"); nlohmann::json j; metadata >> j;
    if (!metadata || j.value("schema", "") != "fsrd-additive-live-v2") throw std::runtime_error("invalid additive capture schema");
    std::filesystem::create_directories(destination);
    nlohmann::json evidence = {{"status", status}, {"source", source.string()}, {"capture_sha256", hashFile(source / "capture.json")}, {"images", nlohmann::json::array()}};
    const auto size = j.at("size"); const size_t pixels = size.at(0).get<size_t>() * size.at(1).get<size_t>();
    for (const auto& image : j.at("images"))
    {
        const std::string file = image.at("file").get<std::string>();
        if (std::filesystem::path(file).filename() != file) throw std::runtime_error("unsafe capture image path");
        const auto path = source / file; const auto sha = hashFile(path);
        if (sha != image.at("sha256").get<std::string>()) throw std::runtime_error("additive capture hash mismatch: " + file);
        const bool fp32 = image.at("format") == "RGBA32_FLOAT";
        const size_t bytes = pixels * 4 * (fp32 ? 4 : 2);
        if (std::filesystem::file_size(path) != bytes) throw std::runtime_error("additive capture image extent mismatch");
        nlohmann::json record = {{"file", file}, {"sha256", sha}, {"bytes", bytes}};
        if (fp32)
        {
            std::vector<float> values(pixels * 4); std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char*>(values.data()), std::streamsize(bytes));
            if (!input) throw std::runtime_error("cannot read additive capture channel");
            double energy = 0; uint64_t nonfinite = 0;
            for (size_t p = 0; p < pixels; ++p) for (size_t channel = 0; channel < 3; ++channel)
            {
                const float value = values[p * 4 + channel]; if (!std::isfinite(value)) ++nonfinite; else energy += value;
            }
            record["mean_rgb_sum"] = energy / double(pixels); record["nonfinite"] = nonfinite;
            if (nonfinite) throw std::runtime_error("nonfinite additive capture channel: " + file);
        }
        std::filesystem::copy_file(path, destination / file, std::filesystem::copy_options::overwrite_existing);
        evidence["images"].push_back(record);
    }
    std::filesystem::copy_file(source / "capture.json", destination / "capture.json", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(source / "conversion_constants.bin", destination / "conversion_constants.bin", std::filesystem::copy_options::overwrite_existing);
    evidence["comparison_scope"] = "production diagnostic strength0/1 channels plus configured actual composition; not RR A/B";
    return evidence;
}

template<class D> D OfflineDefaultAdditiveDescriptor(const D& explicitZero)
{
    D defaults{};
    D result = explicitZero;
    if constexpr (requires { result.AdditiveLightSplit; }) result.AdditiveLightSplit = defaults.AdditiveLightSplit;
    return result;
}
template<class C> void CopyOfflineRrOutputs(ID3D12GraphicsCommandList* cl, C& source, C& destination)
{
    // Exactly one real provider dispatch feeds both real composition paths.
    // Both converters just converted this frame, so these outputs are UAVs.
    ID3D12Resource* from[] = {source.GetDenoiserDiffuseOutput(), source.GetDenoiserSpecularOutput()};
    ID3D12Resource* to[] = {destination.GetDenoiserDiffuseOutput(), destination.GetDenoiserSpecularOutput()};
    D3D12_RESOURCE_BARRIER barriers[4]{};
    for (size_t i = 0; i < 2; ++i)
    {
        barriers[i * 2].Type = barriers[i * 2 + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i * 2].Transition = {from[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        barriers[i * 2 + 1].Transition = {to[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST};
    }
    cl->ResourceBarrier(4, barriers);
    for (size_t i = 0; i < 2; ++i) cl->CopyResource(to[i], from[i]);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cl->ResourceBarrier(4, barriers);
}

// Pinned Config.h + FSRDFeature_Dx12.cpp at 2c2f619baac889c7570b6fe6706dfe28bbf87f5a.
// Descriptor aggregate initialization is NOT Config initialization: two required
// conversion scalars and both composition handover scalars otherwise become zero.
template<class D> void ApplyFrozenProductionConversionDefaults(D& d)
{
    if constexpr (requires { d.FloorIsolation; d.RoughnessFloor; })
    {
        d.FloorIsolation = 1.0f; d.RoughnessFloor = 0.1f;
        d.FloorClampSmoothing = 0.0f; d.FloorRawBlend = 1.0f; d.FloorStructureGate = 1.0f;
        d.DemodDivisorFloor = 8e-3f; d.FloorHandoverMode = 1;
        d.FloorHandoverStrength = 1.0f; d.FloorHandoverDetail = 1.0f;
        d.FloorDetailBoost = 0.0f; d.FloorNormalSharpness = 16.0f;
        d.FloorAlbedoGuide = 1.0f; d.FloorLumSymmetry = 1.0f;
        d.FloorGrazingSharpness = 0.0f; d.FloorEnvelopeBias = 0.0f; d.FloorSoftMin = 0.0f;
        d.BiasMaskStrength = 1.0f; d.ResponsivityTrustThreshold = 0.0f; d.ResponsivityInvert = false;
    }
}
template<class D, class Comp> nlohmann::json OfflineEffectiveSettings(const OfflineFeatures& features)
{
    if constexpr (requires(D d) { d.FloorIsolation; })
    {
        D d{}; ApplyFrozenProductionConversionDefaults(d);
        Comp comp{}; comp.CorrelationBias = 1.0f;
        comp.FloorHandoverAnchorClamp = 2.0f; comp.FloorHandoverCorrelationMix = 1.0f;
        nlohmann::json j;
#define RR_FROZEN(field) j[#field] = d.field
        RR_FROZEN(FloorIsolation); RR_FROZEN(RoughnessFloor); RR_FROZEN(FloorClampSmoothing);
        RR_FROZEN(FloorRawBlend); RR_FROZEN(FloorStructureGate); RR_FROZEN(DemodDivisorFloor);
        RR_FROZEN(FloorHandoverMode); RR_FROZEN(FloorHandoverStrength); RR_FROZEN(FloorHandoverDetail);
        RR_FROZEN(FloorDetailBoost); RR_FROZEN(FloorNormalSharpness); RR_FROZEN(FloorAlbedoGuide);
        RR_FROZEN(FloorLumSymmetry); RR_FROZEN(FloorGrazingSharpness); RR_FROZEN(FloorEnvelopeBias);
        RR_FROZEN(FloorSoftMin); RR_FROZEN(BiasMaskStrength); RR_FROZEN(ResponsivityTrustThreshold); RR_FROZEN(ResponsivityInvert);
#undef RR_FROZEN
        j["CorrelationBias"] = comp.CorrelationBias;
        j["FloorHandoverAnchorClamp"] = comp.FloorHandoverAnchorClamp;
        j["FloorHandoverCorrelationMix"] = comp.FloorHandoverCorrelationMix;
        j["DiffuseRoute"] = features.FfxDenoiserDiffuseRoute.value_or_default();
        j["SpecularRoute"] = features.FfxDenoiserSpecularRoute.value_or_default();
        j["DiagnosticsEnabled"] = false;
        j["defaults_source"] = "Config.h and FSRDFeature_Dx12.cpp@2c2f619baac889c7570b6fe6706dfe28bbf87f5a";
        j["rr_six_key_policy"] = "queried-provider-defaults; intentionally not shipping tuned Config values";
        return j;
    }
    else
    {
        auto j = features.Json();
        j["rr_six_key_policy"] = "queried-provider-defaults; intentionally not shipping tuned Config values";
        return j;
    }
}
