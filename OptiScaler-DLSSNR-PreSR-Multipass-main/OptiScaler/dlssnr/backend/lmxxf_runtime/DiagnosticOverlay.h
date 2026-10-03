#pragma once
#include <stdint.h>
#include <guiddef.h>

// Optional test runtime export. No session pointer crosses the rendering/UI threads.
struct NrDiagnosticOverlay {
    uint32_t size=sizeof(NrDiagnosticOverlay), version=2, visible=0, state=0;
    uint32_t elapsedMs=0, durationMs=20000, width=0, height=0, edge=64;
    uint32_t pixels=0, fullPairs=0, missingMask=0, sourceFormat=0, viewFormat=0;
    uint64_t updated=0;
    float centerX=.5f, centerY=.5f; // Display coordinates, not intermediate texture coordinates.
    uint32_t sourceYFlipped=0;
};
inline float NrCaptureSourceY(float displayY, bool flipped) { return flipped ? 1.f-displayY : displayY; }
using NrGetDiagnosticOverlay = int (__cdecl*)(NrDiagnosticOverlay*, uint32_t);

// Evidence from the game's RTV creation, stored on that exact D3D resource.
// This is observational; it never changes the view used by NR.
inline constexpr GUID NrObservedRtvFormatGuid =
    {0x12930719,0x68a2,0x4091,{0x8e,0x2a,0x2c,0xaf,0x0c,0x44,0x71,0x95}};
