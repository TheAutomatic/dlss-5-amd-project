#include "pch.h"
#include "Selector.h"
#include "InstallStatus.h"
#include "../AsiTakeover.h"
#include "../NrEffectsSettings.h"
#include <Config.h>
#include <Util.h>
#include <filesystem>
#include <mutex>

namespace DlssNr::Backend
{
namespace
{
// Disk probe is not free on the render/submit path. Old ActiveKindFromConfig only
// touched disk for auto; this runs on Before/Execute too, so cache the probe.
struct InstallProbe
{
    std::mutex mu;
    bool hasDaniel = false;
    bool hasLmxxf = false;
    bool hasMochizukiRuntime = false;
    bool hasAsiPlugin = false;
    bool valid = false;
    unsigned long long tick = 0;
    std::string issues[3];
    unsigned long long issueTick[3]{};
    bool issueValid[3]{};
};
InstallProbe g_probe;
std::atomic<bool> g_recordingHooksPrepared { false };

bool EffectiveHasMochizuki(const InstallProbe& p)
{
    // The ASI branch counts as installed only when loaded in this process; if the
    // user disabled ASI loading (e.g. LoadAsiPlugins=false) without legacy
    // MochizukiNrRuntime.dll, ResolveInstalled must fall back to lmxxf/daniel.
    return p.hasMochizukiRuntime || (DlssNr::AsiTakeover::Active() && p.hasAsiPlugin);
}

void RefreshProbeLocked(InstallProbe& p)
{
    std::error_code ec;
    const auto dir = Util::DllPath().parent_path();
    p.hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
    p.hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
    p.hasMochizukiRuntime = std::filesystem::exists(dir / L"MochizukiNrRuntime.dll", ec);
    p.hasAsiPlugin = HasAsiPlugin(dir);
    p.valid = true;
    p.tick = GetTickCount64();
}

void EnsureProbe(InstallProbe& p)
{
    const unsigned long long now = GetTickCount64();
    if (p.valid)
    {
        // Still missing both: the proxy path may appear late, so retry more often.
        const unsigned long long ttl =
            (p.hasDaniel || p.hasLmxxf || p.hasMochizukiRuntime || p.hasAsiPlugin) ? 1000ull : 100ull;
        if (now - p.tick < ttl)
            return;
    }
    RefreshProbeLocked(p);
}
} // namespace

void InvalidateInstallProbe()
{
    std::lock_guard lock(g_probe.mu);
    g_probe.valid = false;
    for (auto& valid : g_probe.issueValid) valid = false;
}

std::string InstallIssue(Kind kind)
{
    std::lock_guard lock(g_probe.mu);
    EnsureProbe(g_probe);
    const auto index = static_cast<int>(kind);
    const auto now = GetTickCount64();
    if (!g_probe.issueValid[index] || now - g_probe.issueTick[index] >= 1000ull) {
        g_probe.issues[index] = ProbeInstallIssue(kind, Util::DllPath().parent_path());
        g_probe.issueTick[index] = now;
        g_probe.issueValid[index] = true;
    }
    if (kind == Kind::Mochizuki && g_probe.issues[index].empty() && !EffectiveHasMochizuki(g_probe))
        return "mochizuki: dlssnr-amd.asi is not loaded in this session. Enable Plugins.LoadAsiPlugins and restart, or select another backend.";
    return g_probe.issues[index];
}

bool HasDanielInstalled()
{
    std::lock_guard lock(g_probe.mu);
    EnsureProbe(g_probe);
    return g_probe.hasDaniel;
}

bool HasLmxxfInstalled()
{
    std::lock_guard lock(g_probe.mu);
    EnsureProbe(g_probe);
    return g_probe.hasLmxxf;
}

bool HasMochizukiInstalled()
{
    std::lock_guard lock(g_probe.mu);
    EnsureProbe(g_probe);
    return EffectiveHasMochizuki(g_probe);
}

Kind RequestedKind()
{
    const auto& cfg = *Config::Instance();
    std::lock_guard nrBackendLock(cfg.NrBackendMutex);
    if (!cfg.NrBackend.has_value())
        return Kind::Daniel;
    return ParseKind(cfg.NrBackend.value());
}

Kind ActiveKindFromConfig()
{
    Request request;
    {
        const auto& cfg = *Config::Instance();
        std::lock_guard nrBackendLock(cfg.NrBackendMutex);
        const auto& raw = cfg.NrBackend;
        request = raw.has_value() ? ParseRequest(raw.value()) : Request::Auto;
    }
    bool hasDaniel = false, hasLmxxf = false, hasMochizuki = false;
    {
        std::lock_guard lock(g_probe.mu);
        EnsureProbe(g_probe);
        hasDaniel = g_probe.hasDaniel;
        hasLmxxf = g_probe.hasLmxxf;
        hasMochizuki = EffectiveHasMochizuki(g_probe);
    }
    return ResolveInstalled(request, hasDaniel, hasLmxxf, LmxxfWired(), hasMochizuki);
}

void MarkRecordingHooksPrepared() { g_recordingHooksPrepared.store(true, std::memory_order_release); }
bool RecordingHooksPrepared() { return g_recordingHooksPrepared.load(std::memory_order_acquire); }

bool SubmissionHooksWanted()
{
    // Must NOT depend on DlssNrEnabled: device hooks are installed once at
    // CreateDevice. If we skip ArmCreate when NR starts off, enabling NR
    // in-game has no submission path and the pass cannot start.
    return ProxyWrapWanted();
}

bool DanielGraphicsHooksWanted()
{
    if (DlssNr::AsiTakeover::Active())
        return false;
    static const bool wanted = PrepareDanielGraphicsAtStartup(ActiveKindFromConfig(),
        Config::Instance()->NrConvenience.value_or_default() != 0, HasDanielInstalled(),
        Config::Instance()->AmdGraphicsWait.value_or_default() != 0);
    return wanted;
}

bool ProxyWrapWanted()
{
    // RR owns recorded depth/debug targets and postprocessing even with NR off,
    // Daniel pure mode, or no NR runtime installed. It needs the same observable
    // command-list lifetime independently of the selected neural backend.
    if (Config::Instance()->FsrRrEnabled.value_or_default())
        return true;
    // When dlssnr-amd.asi owns NR for this session, the built-in NR engine is
    // disabled and the ASI hooks D3D12 command lists, queues and swapchains itself.
    if (DlssNr::AsiTakeover::Active())
        return false;
    // Common effects need recording lifetime callbacks even with a single backend.
    // This requests only the proxy; it never instantiates the inactive backend.
    if (!Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
        Config::Instance()->NrStabilizerEnabled.value_or_default() ||
        DlssNr::OverallIntensity(Config::Instance()->NrOverallIntensity.value_or_default()) != 1.0f ||
        DlssNr::ResidualSettings {Config::Instance()->NrResidualLowGain.value_or_default(),
            Config::Instance()->NrResidualDetailGain.value_or_default(),
            Config::Instance()->NrResidualSkinProtection.value_or_default(),
            Config::Instance()->NrResidualEdgeProtection.value_or_default()}.Active())
        return true;
    if (!LmxxfWired())
        return false;
    return PrepareSubmissionAtStartup(ActiveKindFromConfig(),
        Config::Instance()->NrConvenience.value_or_default() != 0, HasLmxxfInstalled() || HasMochizukiInstalled());
}
} // namespace DlssNr::Backend
