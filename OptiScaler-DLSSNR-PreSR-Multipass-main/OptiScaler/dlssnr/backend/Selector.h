#pragma once
#include "Kind.h"
#include <string>

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// Startup recording-hook policy for FSR-RR, NR effects and neural backends.
bool SubmissionHooksWanted();
// Latched only after device-initialization proxy hooks were successfully armed.
void MarkRecordingHooksPrepared();
bool RecordingHooksPrepared();
// FSR-RR and recorded NR effects require lifetime callbacks regardless of runtime
// installation. NR convenience additionally prepares live neural-backend switches.
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
