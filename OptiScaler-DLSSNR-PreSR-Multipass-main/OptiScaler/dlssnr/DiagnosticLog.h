#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace DlssNr::Diagnostics
{
// Call under the owner's lock. Time is supplied by the caller for deterministic tests.
class RepeatGate
{
    bool seen = false;
    uint64_t last = 0, identity = 0, suppressed = 0;
public:
    bool Allow(uint64_t now, uint64_t key = 0)
    {
        if (!seen || key != identity || now - last >= 5000)
        {
            seen = true;
            identity = key;
            last = now;
            return true;
        }
        ++suppressed;
        return false;
    }
    uint64_t TakeSuppressed() { const auto n = suppressed; suppressed = 0; return n; }
    bool Active() const { return seen; }
    void Reset() { seen = false; }
};

// One current file and one backup; failed diagnostics must not affect rendering.
inline void Append(const std::filesystem::path& path, std::string_view text,
                   uintmax_t limit = 2 * 1024 * 1024) noexcept
{
    static std::mutex mutex;
    try
    {
        std::lock_guard lock(mutex);
        if (!limit || text.size() + 1 > limit) return;
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > limit - text.size() - 1)
        {
            auto backup = path;
            backup += L".1";
            std::filesystem::remove(backup, ec);
            if (ec) return;
            std::filesystem::rename(path, backup, ec);
            if (ec) return;
        }
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.put('\n');
    }
    catch (...) {}
}
}
