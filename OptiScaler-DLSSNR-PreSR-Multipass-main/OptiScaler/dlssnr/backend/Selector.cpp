#include "pch.h"
#include "Selector.h"
#include <Config.h>

namespace DlssNr::Backend
{
Kind RequestedKind()
{
    const auto& cfg = *Config::Instance();
    if (!cfg.NrBackend.has_value())
        return Kind::Daniel;
    return ParseKind(cfg.NrBackend.value());
}

Kind ActiveKindFromConfig() { return ActiveKind(RequestedKind()); }

bool SubmissionHooksWanted()
{
    return LmxxfWired() && RequestedKind() == Kind::Lmxxf;
}
} // namespace DlssNr::Backend
