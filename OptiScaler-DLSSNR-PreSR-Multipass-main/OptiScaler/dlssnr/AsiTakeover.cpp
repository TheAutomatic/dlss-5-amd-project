#include "pch.h"

#include "AsiTakeover.h"

#include <Config.h>
#include <Util.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <tlhelp32.h>
#include <vector>

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
std::string g_menuKey {"End"};
unsigned long long g_menuKeyCheckedAt = 0;
std::atomic<unsigned long long> g_checkedAt {0};

std::string ReadConfiguredMenuKey(const std::wstring& asiPath)
{
    std::vector<std::filesystem::path> candidates;
    if (!asiPath.empty())
    {
        std::filesystem::path dir = std::filesystem::path(asiPath).parent_path();
        for (int i = 0; i < 3 && !dir.empty(); ++i, dir = dir.parent_path())
            candidates.push_back(dir / L"dlssnr-amd.ini");
    }
    candidates.push_back(Util::DllPath().parent_path() / L"dlssnr-amd.ini");
    candidates.push_back(Util::ExePath().parent_path() / L"dlssnr-amd.ini");

    auto trim = [](std::string_view sv) {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
            sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
            sv.remove_suffix(1);
        return sv;
    };

    for (const auto& iniPath : candidates)
    {
        std::ifstream in(iniPath, std::ios::binary);
        if (!in)
            continue;
        bool inNrControl = false;
        bool firstLine = true;
        std::string line;
        while (std::getline(in, line))
        {
            if (firstLine && line.compare(0, 3, "\xef\xbb\xbf") == 0)
                line.erase(0, 3);
            firstLine = false;
            auto sv = trim(line);
            if (sv.empty() || sv.front() == ';' || sv.front() == '#')
                continue;
            if (sv.front() == '[' && sv.back() == ']')
            {
                auto sec = trim(sv.substr(1, sv.size() - 2));
                inNrControl = sec.size() == 9 && _strnicmp(sec.data(), "NRControl", 9) == 0;
                continue;
            }
            if (!inNrControl)
                continue;
            const auto eq = sv.find('=');
            if (eq == std::string_view::npos)
                continue;
            auto key = trim(sv.substr(0, eq));
            if (key.size() == 7 && _strnicmp(key.data(), "MenuKey", 7) == 0)
            {
                auto val = trim(sv.substr(eq + 1));
                const auto semi = val.find(';');
                if (semi != std::string_view::npos)
                    val = trim(val.substr(0, semi));
                const auto hash = val.find('#');
                if (hash != std::string_view::npos)
                    val = trim(val.substr(0, hash));
                if (val.empty() ||
                    (val.size() == 4 && _strnicmp(val.data(), "None", 4) == 0) ||
                    (val.size() == 3 && _strnicmp(val.data(), "Off", 3) == 0) ||
                    val == "0")
                    return {};
                return std::string(val);
            }
        }
        return "End";
    }
    return "End";
}

std::string ReadModuleVersion(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size)
    {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        {
            struct Translation { WORD language, codepage; };
            Translation* translations = nullptr;
            UINT bytes = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<void**>(&translations), &bytes))
            {
                // Prefer the complete product string, including prerelease suffixes.
                for (const wchar_t* field : {L"ProductVersion", L"FileVersion"})
                    for (UINT i = 0; i < bytes / sizeof(Translation); ++i)
                    {
                        const auto key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}",
                            translations[i].language, translations[i].codepage, field);
                        wchar_t* value = nullptr;
                        UINT length = 0;
                        if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &length) ||
                            !value || !length)
                            continue;
                        std::wstring text(value, wcsnlen(value, length));
                        const auto begin = text.find_first_not_of(L" \t\r\n");
                        if (begin != std::wstring::npos)
                            return wstring_to_string(text.substr(begin, text.find_last_not_of(L" \t\r\n") - begin + 1));
                    }
            }
        }
    }
    version_t fixed {};
    if (Util::GetFileVersion(path, &fixed))
        return std::format("{}.{}.{}.{}", fixed.major, fixed.minor, fixed.patch, fixed.reserved);
    return {};
}

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
        version = ReadModuleVersion(path);
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
    if (active)
    {
        g_menuKey = ReadConfiguredMenuKey(path);
        g_menuKeyCheckedAt = GetTickCount64();
    }
    g_active = active;
    g_path = std::move(path);
    g_version = std::move(version);

    if (firstDetection)
        LOG_INFO("NR ownership: {} is loaded ({}); OptiScaler NR engine disabled for this session. "
                 "{}. Restart without the plugin to use the built-in engine.",
                 wstring_to_string(g_path),
                 g_version.empty() ? "version information not provided" : g_version,
                 g_menuKey.empty() ? "NR Control hotkey is disabled in dlssnr-amd.ini"
                                   : std::format("Open NR Control with {}", g_menuKey));
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

std::string MenuKey()
{
    if (!Active())
        return "End";
    const auto now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    if (now - g_menuKeyCheckedAt >= kRefreshIntervalMs)
    {
        g_menuKey = ReadConfiguredMenuKey(g_path);
        g_menuKeyCheckedAt = now;
    }
    return g_menuKey;
}

bool NrRuntimeEnabled()
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return false;
    return !Active();
}
} // namespace DlssNr::AsiTakeover
