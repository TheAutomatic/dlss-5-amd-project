#include "pch.h"
#include "Selector.h"
#include "InstallStatus.h"
#include "../AsiTakeover.h"
#include <Config.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <upscalers/opennr_rr/OpenNrRrFeature_Dx12.h>
#include <filesystem>
#include <mutex>

namespace DlssNr::Backend
{
namespace
{
struct InstallProbe
{
    std::mutex mu;
    bool hasDaniel = false;
    bool hasLmxxf = false;
    bool hasMochizukiRuntime = false;
    bool hasAsiPlugin = false;
    bool hasOpenRr = false;
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
    return p.hasMochizukiRuntime || (DlssNr::AsiTakeover::Active() && p.hasAsiPlugin);
}

void RefreshProbeLocked(InstallProbe& p)
{
    std::error_code ec;
    const auto dir = Util::DllPath().parent_path();
    p.hasDaniel = false;
    p.hasLmxxf = false;
    p.hasMochizukiRuntime = false;
    p.hasAsiPlugin = HasAsiPlugin(dir);
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool gpuSupportsOpenRr =
        gpu.vendorId == VendorId::AMD &&
        (gpu.amdAsicFamily == 152 || gpu.amdHwGeneration == device_info::HwGeneration::kGfx12);
    const bool notBlocked = !OpenNrRr::Checked() || OpenNrRr::Reason().empty();
    p.hasOpenRr = gpuSupportsOpenRr && notBlocked &&
                  std::filesystem::exists(dir / L"OpenNrRrRuntime.dll", ec) &&
                  std::filesystem::exists(dir / L"opennr-rr" / L"shaders", ec) &&
                  OpenNrRr::DlssdFound();
    p.valid = true;
    p.tick = GetTickCount64();
}

void EnsureProbe(InstallProbe& p)
{
    const unsigned long long now = GetTickCount64();
    if (p.valid)
    {
        const unsigned long long ttl = p.hasAsiPlugin ? 1000ull : 100ull;
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
        return "mochizuki: dlssnr-amd.asi is not loaded in this session. Enable Plugins.LoadAsiPlugins and restart.";
    return g_probe.issues[index];
}

bool HasDanielInstalled() { return false; }
bool HasLmxxfInstalled() { return false; }

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
        return Kind::Mochizuki;
    return ParseKind(cfg.NrBackend.value());
}

Kind ActiveKindFromConfig()
{
    return Kind::Mochizuki;
}

void MarkRecordingHooksPrepared() { g_recordingHooksPrepared.store(true, std::memory_order_release); }
bool RecordingHooksPrepared() { return g_recordingHooksPrepared.load(std::memory_order_acquire); }

bool SubmissionHooksWanted()
{
    return ProxyWrapWanted();
}

bool DanielGraphicsHooksWanted()
{
    return false;
}

bool HasOpenRrInstalled()
{
    std::lock_guard lock(g_probe.mu);
    EnsureProbe(g_probe);
    return g_probe.hasOpenRr;
}

bool ProxyWrapWanted()
{
    return Config::Instance()->FsrRrEnabled.value_or_default() ||
           (Config::Instance()->OpenNrRrEnabled.value_or_default() && HasOpenRrInstalled());
}
} // namespace DlssNr::Backend
