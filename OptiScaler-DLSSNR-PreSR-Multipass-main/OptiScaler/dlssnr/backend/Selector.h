#pragma once
#include "Kind.h"

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// True only when lmxxf is requested AND LmxxfWired(). Mutual exclusion vs graphics tracker.
bool SubmissionHooksWanted();
// ProxyWrap is required for lmxxf same-frame QI. NrConvenience pre-opens it so a
// daniel-started session can later switch to lmxxf without a restart. Pure mode
// (NrConvenience=0) only wraps when lmxxf is actually selected.
bool ProxyWrapWanted();
// Cache the on-disk install probe. Call after dropping/adding runtime files.
void InvalidateInstallProbe();
bool HasDanielInstalled();
bool HasLmxxfInstalled();
} // namespace DlssNr::Backend
