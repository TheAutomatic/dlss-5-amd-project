#pragma once

// dlssnr-amd.asi takes over Neural Rendering.
//
// The vendor ASI plugin runs NR itself. When it is loaded into this process,
// OptiScaler keeps its menu, Ray Reconstruction, Super Resolution and frame
// generation, but stops its own NR engine so two engines never touch the same
// frame.
//
// Detected from the loaded module list, never from files on disk: an installed
// but not loaded plugin is not a takeover, and a plugin loaded after startup
// (including a -loadlate .asi) is picked up by the throttled refresh. The state
// latches for the session; disabling the plugin takes effect on the next start,
// which avoids switching two engines on and off mid-frame.

#include <string>

namespace DlssNr::AsiTakeover
{
// Throttled module-list check. True while dlssnr-amd.asi is loaded.
bool Active();
// Full path of the loaded module, empty when not loaded.
std::wstring ModulePath();
// File version as "a.b.c.d", empty when unknown.
std::string ModuleVersion();
// Configured [NRControl] MenuKey from dlssnr-amd.ini ("End" by default; empty when explicitly disabled).
std::string MenuKey();
// Unthrottled re-read. Called after ASI loading and at startup.
bool Refresh();
// The single runtime gate for our own NR: the user asked for it and no ASI owns
// NR in this process. The stored preference is never rewritten.
bool NrRuntimeEnabled();
} // namespace DlssNr::AsiTakeover
