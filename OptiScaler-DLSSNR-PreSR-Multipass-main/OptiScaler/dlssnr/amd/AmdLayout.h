#pragma once
#include <cstddef>
#include <cstdint>

namespace AmdPreSr
{
struct AmdLayout
{
    const char* name;
    std::size_t size;
    const unsigned char sha256[32];
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
    // See exports/a03-staging-state.md.
    std::uint32_t recreate;
    // Address of the mutex guarding Record. Record tries to take it on entry
    // and bails without attaching when it loses, while its worker holds it for
    // the whole of a job - which is what a refusal actually means. +0x4c is the
    // waiter count. 0 means the layout does not record it.
    std::uint32_t recordLock;
    // Diagnostic-only gates read by Record's normal path. Each is tested with
    // jns right before the pendingList attach, and a non-negative value sends
    // Record past the attach to a plain return. counter78 is bumped once per
    // call that gets past the packet check, so it says how far a call got.
    // 0.2.17's three are inferred from the same jns pair and the same +0x1C and
    // +0x10 spacing as 0.3.0's, not read off a 0.2.17 window - treat as unverified.
    std::uint32_t gate4c;
    std::uint32_t gate68;
    std::uint32_t counter78;
};

// 0.2.17 pass DLL, SHA256 bc97f3b0...
inline constexpr AmdLayout kAmd0217 {
    "0.2.17",
    7248384,
    {0xbc,0x97,0xf3,0xb0,0x67,0x18,0xe1,0x90,0x42,0xac,0xaf,0x22,0x7b,0xfe,0x15,0xd1,
     0xe4,0x3d,0x49,0x77,0xf9,0xdc,0x2e,0x39,0x99,0x4f,0xcc,0x51,0x14,0x45,0xff,0x4e},
    0x80e48, 0x19240, 0xf600, 0x9170, 0x12690, 0x8daf8,
    0x8cee8, 0x8cef0, 0x8cef8, 0x8d010, 0x8d018, 0x8d218, 0x8d21a,
    0x8d6c0, 0x8d6f4, 0x8d6f8, 0x8d724, 0x8d82c, 0x8d908, 0x8d914,
    0x8d9b0, 0x8d9b4, 0x8d9bc, 0x8d9bd, 0x8d9be, 0x8d9bf, 0x8d9c0,
    0x8d9d0, 0x8d9d4, 0x8d9d8, 0x8d9e0, 0x8d9e4, 0x8dad0,
    0x8daa8, 0x8da30, 0x8d8f4, 0x8d910, 0x8d920
};

// Alpha 0.3.0 version.dll. Fields from unique 0.2.17 instruction windows;
// Record 0x12640 from pendingList/jobId xchg owner; Notify+0x13 still calls trampoline.
inline constexpr AmdLayout kAmd03 {
    "0.3.0",
    7290880,
    {0x83,0x21,0xca,0xe7,0x28,0xd2,0x8c,0xb7,0x63,0x2d,0x0d,0x58,0xd3,0xd9,0x13,0xe9,
     0x11,0x32,0xbf,0x76,0x45,0xc1,0x26,0x50,0x56,0x98,0xfb,0xe4,0xcd,0x5a,0x01,0x38},
    0, 0x1fe80, 0x12640, 0x9460, 0x161e0, 0x97c70,
    0x96f68, 0x96f70, 0x96f78, 0x97090, 0x97098, 0x97298, 0x9729a,
    0x977a0, 0x977d4, 0x977d8, 0x97804, 0x97984, 0x97a60, 0x97a6c,
    0x97b10, 0x97b14, 0x97b1c, 0x97b1d, 0x97b1e, 0x97b1f, 0x97b20,
    0x97b30, 0x97b34, 0x97b38, 0x97b40, 0x97b44, 0x97c30,
    0x97c08, 0x97b90, 0x97a4c, 0x97a68, 0x97a78
};

inline constexpr const AmdLayout* kAmdLayouts[] = { &kAmd0217, &kAmd03 };
}
