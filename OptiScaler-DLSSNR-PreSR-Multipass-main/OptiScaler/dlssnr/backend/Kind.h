#pragma once
#include <string_view>

namespace DlssNr::Backend
{
enum class Kind
{
    Daniel,
    Lmxxf,
    Off,
};

// Case-insensitive. Missing, empty, "auto", and unknown values are Daniel.
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
    if (raw.empty() || eq(raw, "auto") || eq(raw, "daniel"))
        return Kind::Daniel;
    if (eq(raw, "off") || eq(raw, "none"))
        return Kind::Off;
    if (eq(raw, "lmxxf"))
        return Kind::Lmxxf;
    return Kind::Daniel;
}

// First increment: no LmxxfNrRuntime. Off stays Off; everything else is Daniel.
inline bool LmxxfWired() { return false; }

inline Kind ActiveKind(Kind requested)
{
    if (requested == Kind::Off)
        return Kind::Off;
    if (requested == Kind::Lmxxf && LmxxfWired())
        return Kind::Lmxxf;
    return Kind::Daniel;
}
} // namespace DlssNr::Backend
