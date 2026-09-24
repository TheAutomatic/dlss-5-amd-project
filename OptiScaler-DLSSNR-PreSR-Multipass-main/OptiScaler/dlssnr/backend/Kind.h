#pragma once
#include <string_view>

namespace DlssNr::Backend
{
enum class Kind
{
    Daniel,
    Lmxxf,
};

// Case-insensitive. Missing, empty, "auto", "off", "none", and unknown are Daniel.
// There is no separate "off" host: Enable NR is the on/off switch.
inline Kind ParseKind(std::string_view raw)
{
    auto eq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            const auto lo = [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
            };
            if (lo(ca) != lo(cb))
                return false;
        }
        return true;
    };
    if (eq(raw, "lmxxf"))
        return Kind::Lmxxf;
    return Kind::Daniel;
}

// First increment: no LmxxfNrRuntime. Everything else is Daniel.
inline bool LmxxfWired() { return true; } // LOCAL E trial only — do not push/default

inline Kind ActiveKind(Kind requested)
{
    if (requested == Kind::Lmxxf && LmxxfWired())
        return Kind::Lmxxf;
    return Kind::Daniel;
}
} // namespace DlssNr::Backend
