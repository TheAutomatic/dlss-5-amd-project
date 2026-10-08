#pragma once
#include "MenuUi.h"

// Targeted adaptation of upstream 434b955's ScopedCard draw-channel layout.
// Does not create a second input context or change the renderer's resource ownership.
class MenuCard
{
    inline static thread_local MenuCard* current = nullptr;
    ImGuiWindow* window = nullptr;
    ImDrawListSplitter splitter;
    ImVec2 origin {}, padding {};
    float width = 0, oldWork = 0, oldContent = 0, startY = 0;
    bool titled = false;
public:
    MenuCard()
    {
        if (current) return;
        window = ImGui::GetCurrentWindow();
        const auto& style = ImGui::GetStyle();
        origin = window->DC.CursorPos;
        width = ImGui::GetContentRegionAvail().x;
        padding = {style.WindowPadding.x, style.WindowPadding.y * .75f};
        splitter.Split(window->DrawList, 2);
        splitter.SetCurrentChannel(window->DrawList, 1);
        window->DC.CursorPos.y += padding.y;
        ImGui::Indent(padding.x);
        oldWork = window->WorkRect.Max.x;
        oldContent = window->ContentRegionRect.Max.x;
        window->WorkRect.Max.x -= padding.x;
        window->ContentRegionRect.Max.x -= padding.x;
        startY = window->DC.CursorPos.y;
        current = this;
    }
    ~MenuCard()
    {
        if (!window) return;
        current = nullptr;
        ImGui::Unindent(padding.x);
        window->WorkRect.Max.x = oldWork;
        window->ContentRegionRect.Max.x = oldContent;
        if (window->DC.CursorPos.y <= startY)
        {
            splitter.Merge(window->DrawList);
            window->DC.CursorPos.y = origin.y;
            return;
        }
        const float bottom = window->DC.CursorPos.y - ImGui::GetStyle().ItemSpacing.y + padding.y;
        splitter.SetCurrentChannel(window->DrawList, 0);
        window->DrawList->AddRect(origin, {origin.x + width, bottom},
                                 ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetStyle().ChildRounding);
        splitter.Merge(window->DrawList);
        window->DC.CursorPos.y = bottom;
        ImGui::Dummy({0, 0});
    }
    MenuCard(const MenuCard&) = delete;
    MenuCard& operator=(const MenuCard&) = delete;
    static bool TryTitle(const char* text)
    {
        if (!current || current->titled || ImGui::GetCurrentWindow() != current->window ||
            current->window->DC.CursorPos.y > current->startY + ImGui::GetStyle().ItemSpacing.y * 2)
            return false;
        current->titled = true;
        MenuUi::TextDisabled("%s", text);
        ImGui::Spacing();
        return true;
    }
};

inline void MenuSectionTitle(const char* label)
{
    if (!MenuCard::TryTitle(label)) MenuUi::SeparatorText(label);
}
