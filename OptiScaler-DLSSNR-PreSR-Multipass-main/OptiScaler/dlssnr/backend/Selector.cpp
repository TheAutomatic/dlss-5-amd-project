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
    const auto requested = RequestedKind();
    if (!Config::Instance()->NrBackend.has_value() ||
        Config::Instance()->NrBackend.value() == "auto" ||
        Config::Instance()->NrBackend.value().empty())
    {
        std::error_code ec;
        const auto dir = Util::DllPath().parent_path();
        const bool hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
        const bool hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
        if (hasLmxxf && !hasDaniel && LmxxfWired())
            return Kind::Lmxxf;
    }
    return ActiveKind(requested);
}

bool SubmissionHooksWanted()
{
    return LmxxfWired() && ActiveKindFromConfig() == Kind::Lmxxf;
}
} // namespace DlssNr::Backend
