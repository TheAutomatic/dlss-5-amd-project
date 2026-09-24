#include "pch.h"
#include "Selector.h"
#include <Config.h>
#include <Util.h>
#include <filesystem>

namespace DlssNr::Backend
{
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
    std::error_code ec;
    const auto dir = Util::DllPath().parent_path();
    const bool hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
    const bool hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
    return ResolveInstalled(request, hasDaniel, hasLmxxf, LmxxfWired());
}

bool SubmissionHooksWanted()
{
    return LmxxfWired() && ActiveKindFromConfig() == Kind::Lmxxf;
}
} // namespace DlssNr::Backend
