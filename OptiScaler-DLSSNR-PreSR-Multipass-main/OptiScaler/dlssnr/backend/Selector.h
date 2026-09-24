#pragma once
#include "Kind.h"

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// True only when lmxxf is requested AND LmxxfWired(). Mutual exclusion vs graphics tracker.
bool SubmissionHooksWanted();
// Cache the on-disk install probe. Call after dropping/adding runtime files.
void InvalidateInstallProbe();
bool HasDanielInstalled();
bool HasLmxxfInstalled();
} // namespace DlssNr::Backend
