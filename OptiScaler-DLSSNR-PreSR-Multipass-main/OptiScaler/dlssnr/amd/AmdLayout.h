#pragma once
#include <cstddef>
#include <cstdint>

namespace AmdPreSr
{
// SHA256 as a fixed 32-byte digest. Prefer Sha256FromHex() over a raw
// {0x..} list: a hand-copied byte array that is one nibble short still
// compiles (the rest zero-fills) and then never matches at runtime.
struct Sha256
{
    unsigned char bytes[32];
};

constexpr unsigned char HexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<unsigned char>(c - 'A' + 10);
    return 0xFF;
}

constexpr bool IsHexDigit(char c) { return HexNibble(c) <= 0x0F; }

// Exactly 64 hex digits + NUL. Wrong length is a compile error, which is
// the guard for the 0.3.1 digest that was hand-copied one character short.
template <std::size_t N>
constexpr Sha256 Sha256FromHex(const char (&hex)[N])
{
    static_assert(N == 65, "SHA256 hex literal must be exactly 64 hex digits (plus NUL)");
    Sha256 out {};
    for (std::size_t i = 0; i < 32; ++i)
    {
        const char h = hex[i * 2];
        const char l = hex[i * 2 + 1];
        if (!IsHexDigit(h) || !IsHexDigit(l))
            return Sha256 {}; // invalid digit → all-zero digest (will not match)
        out.bytes[i] = static_cast<unsigned char>((HexNibble(h) << 4) | HexNibble(l));
    }
    return out;
}

struct AmdLayout
{
    const char* name;
    std::size_t size;
    Sha256 sha256;
    std::uint32_t d3dCompileIat; // 0 if the runtime has no D3DCompile import
    std::uint32_t init;
    std::uint32_t record;
    std::uint32_t notify;
    std::uint32_t shutdown;
    std::uint32_t trampoline;
    std::uint32_t device;
    std::uint32_t queue;
    std::uint32_t engine;
    std::uint32_t historyView;
    std::uint32_t historyValid;
    std::uint32_t initDone;
    std::uint32_t nativeFailure;
    std::uint32_t configuredInline;
    std::uint32_t jobDone;
    std::uint32_t timeoutCount;
    std::uint32_t watchdog;
    std::uint32_t interop;
    std::uint32_t pendingList;
    std::uint32_t jobId;
    std::uint32_t depthInverted;
    std::uint32_t explicitDepth;
    std::uint32_t enabled;
    std::uint32_t temporal;
    std::uint32_t fsrInputs;
    std::uint32_t depthPresent;
    std::uint32_t tonemap;
    std::uint32_t tone;
    std::uint32_t structure;
    std::uint32_t skin;
    std::uint32_t charMask;
    std::uint32_t toneChannels;
    std::uint32_t hipOrdinal;
    // Sticky "staging must be re-created" byte. Set when the runtime detects a
    // resize, a re-created upscaler context, or an INI change; cleared only
    // after it has drained the game's queue and joined its workers. Record
    // tests it as its first act, so 0 means "this call will not rebuild".
    std::uint32_t recreate;
    // Address of the mutex guarding Record. The verified 0.3.1 entry
    // acquires it through a blocking SRW-lock path. +0x4c is its ownership /
    // recursion count, not a waiter count or worker-busy indicator. It cannot
    // explain a refused Record by itself. 0 means the field is not mapped.
    std::uint32_t recordLock;
    // Diagnostic-only fields checked by Record's non-inline admission path.
    // These jns gates are bypassed by normal inline admission; gate68 also
    // stores the singleton job waiting for Notify. They are not independent
    // inline refusal reasons. counter78 advances after the packet check and
    // is only a coarse indication of how far a call got.
    // 0.2.17's three are inferred from the same jns pair and the same +0x1C and
    // +0x10 spacing as 0.3.0's, not read off a 0.2.17 window - treat as unverified.
    std::uint32_t gate4c;
    std::uint32_t gate68;
    std::uint32_t counter78;
    // 0.3.1 only: inline-wait spin implementation. 0 = compute dispatch spin;
    // non-zero = predicated 1-pixel graphics draws. The compute path also has
    // 0.3.1-specific slicing, so SpinDraw=0 is not identical to the 0.3.0 wait.
    // 0 means "runtime does not expose this flag".
    std::uint32_t spinDraw;
};

// 0.2.17 pass DLL, SHA256 bc97f3b0...
inline constexpr AmdLayout kAmd0217 {
    "0.2.17",
    7248384,
    Sha256FromHex("bc97f3b06718e19042acaf227bfe15d1e43d4977f9dc2e39994fcc511445ff4e"),
    0x80e48, 0x19240, 0xf600, 0x9170, 0x12690, 0x8daf8,
    0x8cee8, 0x8cef0, 0x8cef8, 0x8d010, 0x8d018, 0x8d218, 0x8d21a,
    0x8d6c0, 0x8d6f4, 0x8d6f8, 0x8d724, 0x8d82c, 0x8d908, 0x8d914,
    0x8d9b0, 0x8d9b4, 0x8d9bc, 0x8d9bd, 0x8d9be, 0x8d9bf, 0x8d9c0,
    0x8d9d0, 0x8d9d4, 0x8d9d8, 0x8d9e0, 0x8d9e4, 0x8dad0,
    0x8daa8, 0x8da30, 0x8d8f4, 0x8d910, 0x8d920,
    0
};

// Alpha 0.3.0 version.dll. Fields from unique 0.2.17 instruction windows;
// Record 0x12640 from pendingList/jobId xchg owner; Notify+0x13 still calls trampoline.
inline constexpr AmdLayout kAmd03 {
    "0.3.0",
    7290880,
    Sha256FromHex("8321cae728d28cb7632d0d58d3d913e91132bf7645c126505698fbe4cd5a0138"),
    0, 0x1fe80, 0x12640, 0x9460, 0x161e0, 0x97c70,
    0x96f68, 0x96f70, 0x96f78, 0x97090, 0x97098, 0x97298, 0x9729a,
    0x977a0, 0x977d4, 0x977d8, 0x97804, 0x97984, 0x97a60, 0x97a6c,
    0x97b10, 0x97b14, 0x97b1c, 0x97b1d, 0x97b1e, 0x97b1f, 0x97b20,
    0x97b30, 0x97b34, 0x97b38, 0x97b40, 0x97b44, 0x97c30,
    0x97c08, 0x97b90, 0x97a4c, 0x97a68, 0x97a78,
    0
};

// 0.3.1 version.dll (SHA b108d640). Mapped from 0.3.0 via unique instruction
// windows (analysis/map_a031_rva.py); Record/Notify/shutdown heads and the
// recreate sticky-bit xrefs match 0.3.0 role-for-role. Data section moved
// ~+0x3180 and .text grew — every RVA below is 0.3.1-specific.
inline constexpr AmdLayout kAmd031 {
    "0.3.1",
    7304192,
    Sha256FromHex("b108d6407eb7f094a4f9111edd778eee7b978b648d413a9fc7aeedfdd914c154"),
    0, 0x21720, 0x13540, 0x9720, 0x17150, 0x9ae68,
    0x9a0e8, 0x9a0f0, 0x9a100, 0x9a218, 0x9a220, 0x9a420, 0x9a422,
    0x9a928, 0x9a95c, 0x9a960, 0x9a98c, 0x9ab58, 0x9ac38, 0x9ac44,
    0x9ace8, 0x9acec, 0x9acf4, 0x9acf5, 0x9acf6, 0x9acf7, 0x9acf8,
    0x9ad08, 0x9ad0c, 0x9ad10, 0x9ad18, 0x9ad1c, 0x9ae08,
    0x9ade0, 0x9ad68, 0x9ac24, 0x9ac40, 0x9ac50,
    0x9ab14
};

inline constexpr const AmdLayout* kAmdLayouts[] = { &kAmd0217, &kAmd03, &kAmd031 };

// Compile-time sanity: the hex helper must land on the first/last digest byte
// of each known runtime. A wrong-length literal already fails Sha256FromHex;
// these catch a copy-paste that swapped two mid-string bytes.
static_assert(kAmd0217.sha256.bytes[0] == 0xbc && kAmd0217.sha256.bytes[31] == 0x4e);
static_assert(kAmd03.sha256.bytes[0] == 0x83 && kAmd03.sha256.bytes[31] == 0x38);
static_assert(kAmd031.sha256.bytes[0] == 0xb1 && kAmd031.sha256.bytes[31] == 0x54);
}
