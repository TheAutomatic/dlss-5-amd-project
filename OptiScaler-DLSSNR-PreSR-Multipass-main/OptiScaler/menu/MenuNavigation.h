#pragma once
#include "MenuUi.h"
#include <algorithm>
#include <iterator>

namespace MenuNavigation
{
enum class Page { NR, Upscaling, FrameGen, Latency, Textures, Compatibility, Appearance, Status, Count };
struct Entry { const char* group; const char* label; const char* description; };
inline constexpr Entry entries[] = {
    {"Rendering", "Neural Rendering", "Backend, processing order, passes and NR output."},
    {"Rendering", "Upscaling", "Upscaler, sharpness and output resolution."},
    {"Rendering", "Frame Generation", "Frame generation source, output and pacing."},
    {"Rendering", "Latency & FPS", "Frame limiter, low latency and V-Sync."},
    {"System", "Textures", "Mipmap bias, filtering and magnifier."},
    {"System", "Compatibility", "Input flags, resource barriers and game workarounds."},
    {"System", "Menu", "Theme, scale, overlay and keybinds."},
    {"System", "Help / Status", "Detected components, active inputs and logging."},
};
static_assert(std::size(entries) == int(Page::Count));

// Adapted from upstream RenderMainMenuTabs. IDs never depend on translated text.
// A compact selector keeps the content usable on narrow windows/high DPI.
template<class Draw> void Render(Page& page, float scale, Draw draw)
{
    int selected = std::clamp(int(page), 0, int(Page::Count) - 1);
    const auto& style = ImGui::GetStyle();
    float sidebar = 120 * scale;
    for (const auto& entry : entries)
        sidebar = std::max(sidebar, MenuUi::CalcTextSize(entry.label).x + 28 * scale + style.WindowPadding.x * 2);
    const bool compact = ImGui::GetContentRegionAvail().x < sidebar + 440 * scale;
    if (compact)
    {
        const char* labels[std::size(entries)];
        for (size_t i = 0; i < std::size(entries); ++i) labels[i] = entries[i].label;
        ImGui::SetNextItemWidth(-1);
        MenuUi::Combo("##MenuPage", &selected, labels, int(std::size(entries)));
    }
    else
    {
        if (ImGui::BeginChild("##MenuSidebar", {sidebar, 0}, ImGuiChildFlags_Borders))
        {
            const char* lastGroup = nullptr;
            for (int i = 0; i < int(Page::Count); ++i)
            {
                if (lastGroup != entries[i].group)
                {
                    if (lastGroup) ImGui::Spacing();
                    MenuUi::TextDisabled("%s", entries[i].group);
                    lastGroup = entries[i].group;
                }
                ImGui::PushID(i);
                const float height = ImGui::GetFrameHeight() * 1.35f;
                if (ImGui::Selectable("##Page", selected == i, 0, {0, height})) selected = i;
                const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                auto list = ImGui::GetWindowDrawList();
                if (selected == i)
                    list->AddRectFilled(lo, {lo.x + 3 * scale, hi.y}, ImGui::GetColorU32(ImGuiCol_CheckMark));
                list->AddText({lo.x + 10 * scale, lo.y + (height - ImGui::GetTextLineHeight()) * .5f},
                              ImGui::GetColorU32(ImGuiCol_Text), MenuLocale::Translate(entries[i].label));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    page = Page(selected);
    ImGui::PushID(selected);
    // Controls wrap to the current viewport. A horizontal scrollbar preserves last
    // frame's wide WorkRect after resize and defeats responsive label wrapping.
    if (ImGui::BeginChild("##MenuPageBody", {0, 0}))
    {
        MenuUi::TextWrapped("%s", entries[selected].description);
        ImGui::Spacing();
        draw(page);
    }
    ImGui::EndChild();
    ImGui::PopID();
}
}
