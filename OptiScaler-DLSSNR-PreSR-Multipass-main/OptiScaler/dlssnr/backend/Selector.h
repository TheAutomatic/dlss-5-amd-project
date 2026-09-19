#pragma once
#include "Kind.h"

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// True only when lmxxf is requested AND LmxxfWired(). Mutual exclusion vs graphics tracker.
bool SubmissionHooksWanted();
} // namespace DlssNr::Backend
