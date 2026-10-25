#pragma once

#include "Quirks.h"

#include <functional>
#include <string>
#include <vector>
#include <string_view>

// Metadata preserves unset Auto/explicit INI semantics; never assign these to Config.
struct NrGamePolicy
{
    bool earlyExeWrap = false;
    bool earlyAnyModuleWrap = false;
    bool enhancedBarriers = false;
    bool earlyUnityPlayerWrap = false;
    // Per-game default for FGDLSSGRequiredReadyFrames when the user hasn't set it.
    int fgReadyFrames = 0;
};

inline NrGamePolicy GetNrGamePolicy(std::string_view exeName, bool unrealEngine = false)
{
    enum class Match { Exact, Contains, Unreal };
    struct Entry { std::string_view name; Match match; NrGamePolicy policy; };
    static constexpr Entry entries[] = {
        { "forza", Match::Contains, { true, true, true, false } },
        { "acmirage.exe", Match::Exact, { true, false, false, false } },
        // Dying Light (The Beast): the renderer rd3d12_x64_rwdi.dll creates boot DIRECT
        // command lists before the swapchain; the exe-only early whitelist never matches
        // them, they stay unwrapped forever, and NR bypasses every frame recorded into
        // them (linux2422: ~2.7% proxy-missing frames = the visible NR flicker).
        { "dyinglight", Match::Contains, { false, true, false, false } },
        // 逆水寒 / Justice, 燕云十六声 / Where Winds Meet (GacRunnerNG.exe): same boot-list
        // pattern as Dying Light — with defaults the renderer's pre-swapchain DIRECT lists
        // stay unwrapped and proxy_missing grows to ~10/5s window as the game rotates onto
        // them (visible NR flicker); the A/B field runs confirmed EarlyAnyModuleWrap removes
        // it. Self-diagnosis hint added in MochizukiBackend for games not yet in this table.
        // fgReadyFrames=30: XeFG already active during the entry phase can crash or corrupt
        // visuals there, so default to the 30-frame readiness gate when the user hasn't set
        // FGDLSSGRequiredReadyFrames.
        { "gacrunnerng.exe", Match::Exact, { false, true, false, false, 30 } },
        { "007firstlight.exe", Match::Exact, { true, false, false, false } },
        { "aniimo.exe", Match::Exact, { false, false, false, true } },
        { {}, Match::Unreal, { true, false, false, false } },
    };
    if (const auto slash = exeName.find_last_of("\\/"); slash != std::string_view::npos)
        exeName.remove_prefix(slash + 1);
    auto equalAt = [&](size_t offset, std::string_view text)
    {
        if (offset + text.size() > exeName.size()) return false;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(exeName[offset + i]);
            if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != text[i]) return false;
        }
        return true;
    };
    NrGamePolicy result;
    for (const auto& entry : entries)
    {
        bool matches = entry.match == Match::Unreal && unrealEngine;
        if (entry.match == Match::Exact)
            matches = exeName.size() == entry.name.size() && equalAt(0, entry.name);
        else if (entry.match == Match::Contains)
            for (size_t i = 0; !matches && i + entry.name.size() <= exeName.size(); ++i)
                matches = equalAt(i, entry.name);
        if (!matches) continue;
        result.earlyExeWrap |= entry.policy.earlyExeWrap;
        result.earlyAnyModuleWrap |= entry.policy.earlyAnyModuleWrap;
        result.enhancedBarriers |= entry.policy.enhancedBarriers;
        result.earlyUnityPlayerWrap |= entry.policy.earlyUnityPlayerWrap;
        if (entry.policy.fgReadyFrames > result.fgReadyFrames)
            result.fgReadyFrames = entry.policy.fgReadyFrames;
    }
    return result;
}

struct GameConfigContext
{
    bool isNvidia = false;
};

// A single "Setting = value" line of a game entry
struct ConfigDefault
{
    std::string text;
    std::function<bool()> apply; // Returns false when the user's config already sets this option
    bool (*condition)(const GameConfigContext&) = nullptr;
};

struct GameConfig
{
    std::vector<std::string> exes;   // Lowercase exe names
    std::vector<std::string> ueExes; // UE project names, matches <name>-win64-shipping.exe & -wingdk-shipping.exe
    std::vector<ConfigDefault> defaults;
    std::vector<GameQuirk> quirks;
};

// Applies the config defaults of every entry matching exeName and returns their quirks
flag_set<GameQuirk> ApplyGameConfigs(std::string exeName, const GameConfigContext& context);
