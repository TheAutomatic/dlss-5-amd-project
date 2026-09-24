#include "pch.h"
#include "Selector.h"
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
    bool valid = false;
    unsigned long long tick = 0;
};
InstallProbe g_probe;

void RefreshProbeLocked(InstallProbe& p)
{
    std::error_code ec;
    const auto dir = Util::DllPath().parent_path();
    p.hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
    p.hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
    p.valid = true;
    p.tick = GetTickCount64();
}

void EnsureProbe(InstallProbe& p)
{
    const unsigned long long now = GetTickCount64();
    if (p.valid)
    {
        // Still missing both: the proxy path may appear late, so retry more often.
        const unsigned long long ttl = (p.hasDaniel || p.hasLmxxf) ? 1000ull : 100ull;
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

Kind RequestedKind()
{
    const auto& cfg = *Config::Instance();
    if (!cfg.NrBackend.has_value())
        return Kind::Daniel;
    return ParseKind(cfg.NrBackend.value());
}

Kind ActiveKindFromConfig()
{
    const auto& raw = Config::Instance()->NrBackend;
    const Request request =
        raw.has_value() ? ParseRequest(raw.value()) : Request::Auto;
    bool hasDaniel = false, hasLmxxf = false;
    {
        std::lock_guard lock(g_probe.mu);
        EnsureProbe(g_probe);
        hasDaniel = g_probe.hasDaniel;
        hasLmxxf = g_probe.hasLmxxf;
    }
    return ResolveInstalled(request, hasDaniel, hasLmxxf, LmxxfWired());
}

bool SubmissionHooksWanted()
{
    return LmxxfWired() && ActiveKindFromConfig() == Kind::Lmxxf;
}
} // namespace DlssNr::Backend
