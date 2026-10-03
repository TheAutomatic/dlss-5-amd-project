#pragma once
#include "Kind.h"
#include <string>

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// Startup hook policy: lmxxf, or convenience with lmxxf installed.
bool SubmissionHooksWanted();
// ProxyWrap is required for lmxxf same-frame QI. NrConvenience pre-opens it so a
// daniel-started session can later switch to lmxxf without a restart. Pure mode
// (NrConvenience=0) only wraps when lmxxf is actually selected.
bool ProxyWrapWanted();
// Latched startup policy: later menu changes require restart, including late hooks.
bool DanielGraphicsHooksWanted();
// Cache the on-disk install probe. Call after dropping/adding runtime files.
void InvalidateInstallProbe();
bool HasDanielInstalled();
bool HasLmxxfInstalled();
bool HasMochizukiInstalled();
std::string InstallIssue(Kind kind);
} // namespace DlssNr::Backend
