#include "pch.h"

#include "AsiTakeover.h"

#include <Config.h>
#include <Util.h>

#include <atomic>
#include <mutex>
#include <tlhelp32.h>

namespace DlssNr::AsiTakeover
{
namespace
{
// The module the vendor package installs into OptiScaler\plugins. Match the
// loaded file name only; an unrelated .asi in the game folder must not own NR.
constexpr const wchar_t* kAsiFileName = L"dlssnr-amd.asi";

// Evaluation runs every frame; the module list is not worth a snapshot per call.
constexpr unsigned long long kRefreshIntervalMs = 500;

std::mutex g_mutex;
std::atomic<bool> g_active {false};
std::wstring g_path;
std::string g_version;
std::atomic<unsigned long long> g_checkedAt {0};

void ScanLocked()
{
    bool active = false;
    std::wstring path;
    std::string version;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W entry {};
        entry.dwSize = sizeof entry;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, kAsiFileName) == 0)
                {
                    active = true;
                    path = entry.szExePath;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (active)
    {
        version_t fileVersion {};
        if (Util::GetFileVersion(path, &fileVersion))
            version = std::format("{}.{}.{}.{}", fileVersion.major, fileVersion.minor, fileVersion.patch,
                                  fileVersion.reserved);
    }

    const bool firstDetection = active && !g_active;
    // Latch for the session: loading may precede every NR frame, and unloading
    // mid-session must not put a second engine on the same frames.
    if (g_active)
    {
        active = true;
        if (path.empty())
            path = g_path;
        if (version.empty())
            version = g_version;
    }
    g_active = active;
    g_path = std::move(path);
    g_version = std::move(version);

    if (firstDetection)
        LOG_INFO("NR ownership: {} is loaded ({}); OptiScaler NR engine disabled for this session. "
                 "Open NR Control with End. Restart without the plugin to use the built-in engine.",
                 wstring_to_string(g_path), g_version.empty() ? "version unknown" : g_version);
}
} // namespace

bool Refresh()
{
    std::lock_guard lock(g_mutex);
    ScanLocked();
    g_checkedAt.store(GetTickCount64(), std::memory_order_relaxed);
    return g_active;
}

bool Active()
{
    const auto now = GetTickCount64();
    if (g_active)
        return true;
    if (now - g_checkedAt.load(std::memory_order_relaxed) < kRefreshIntervalMs)
    {
        std::lock_guard lock(g_mutex);
        return g_active;
    }
    return Refresh();
}

std::wstring ModulePath()
{
    Active();
    std::lock_guard lock(g_mutex);
    return g_path;
}

std::string ModuleVersion()
{
    Active();
    std::lock_guard lock(g_mutex);
    return g_version;
}

bool NrRuntimeEnabled()
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return false;
    return !Active();
}
} // namespace DlssNr::AsiTakeover
