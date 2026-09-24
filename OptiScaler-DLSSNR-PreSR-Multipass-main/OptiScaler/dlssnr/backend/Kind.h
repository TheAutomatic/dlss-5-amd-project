#pragma once
#include <string_view>

namespace DlssNr::Backend
{
enum class Kind
{
    Daniel,
    Lmxxf,
};

// What the ini asked for. Config load migrates legacy "off"/"none" to
// Enabled=false; any unrecognized value resolves as Auto.
enum class Request
{
    Auto,
    Daniel,
    Lmxxf,
};

inline bool KindEq(std::string_view a, std::string_view b)
{
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
}

inline Request ParseRequest(std::string_view raw)
{
    if (KindEq(raw, "lmxxf"))
        return Request::Lmxxf;
    if (KindEq(raw, "daniel"))
        return Request::Daniel;
    return Request::Auto;
}

// Case-insensitive. Auto/legacy values report Daniel; use ResolveInstalled for a host
// that can actually run.
inline Kind ParseKind(std::string_view raw)
{
    return ParseRequest(raw) == Request::Lmxxf ? Kind::Lmxxf : Kind::Daniel;
}

// First increment: no LmxxfNrRuntime. Everything else is Daniel.
inline bool LmxxfWired() { return true; } // LOCAL E trial only — do not push/default

// Pick the host that will actually run.
// Explicit request wins when its files are on disk; otherwise fall back to the
// other installed host. Auto takes whichever is installed (lmxxf if it is alone).
// When neither runtime is present the result is still a Kind so callers can name
// the missing file — HasFiles() stays false and the error is visible.
inline Kind ResolveInstalled(Request request, bool hasDaniel, bool hasLmxxf, bool lmxxfWired)
{
    const bool canLmxxf = hasLmxxf && lmxxfWired;
    if (request == Request::Auto)
    {
        if (canLmxxf && !hasDaniel)
            return Kind::Lmxxf;
        if (hasDaniel)
            return Kind::Daniel;
        if (canLmxxf)
            return Kind::Lmxxf;
        return Kind::Daniel;
    }
    if (request == Request::Lmxxf)
    {
        if (canLmxxf)
            return Kind::Lmxxf;
        if (hasDaniel)
            return Kind::Daniel;
        return Kind::Lmxxf;
    }
    // Request::Daniel
    if (hasDaniel)
        return Kind::Daniel;
    if (canLmxxf)
        return Kind::Lmxxf;
    return Kind::Daniel;
}
} // namespace DlssNr::Backend
