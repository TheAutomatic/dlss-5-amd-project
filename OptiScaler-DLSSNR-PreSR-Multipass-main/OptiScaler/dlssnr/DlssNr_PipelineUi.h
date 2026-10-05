#pragma once
// SPDX-License-Identifier: GPL-3.0-only
// Adapted from wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass v0.8.4 (8802b2b4),
// DlssNr_PipelineUi.h. Retains the node drawing/click navigation; this product
// supplies its three backend views. No renderer or Config writes here.
#include <imgui/imgui.h>
#include <menu/MenuUi.h>
#include <algorithm>
#include <array>
#include <string>

namespace DlssNr::PipelineUi
{
enum class Section { Input, Model, Output };
inline const char* SectionName(Section section)
{
    switch (section) {
    case Section::Input: return "Prepare NR input";
    case Section::Model: return "NR model";
    case Section::Output: return "Apply NR edit";
    }
    return "NR model";
}
struct View
{
    std::string backend, input, model, output;
    bool enabled = true;
    bool beforeSr = true;
};
inline void Draw(const View& view, Section& selected)
{
    ImGui::PushID("NR pipeline chart");
    MenuUi::TextWrapped(view.beforeSr ? "Game input -> %s NR -> Super Resolution" :
        "Game input -> Super Resolution -> %s NR", view.backend.c_str());
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const float nodeWidth = std::min(width, ImGui::GetFontSize() * 27.0f);
    const float gap = ImGui::GetFontSize() * 0.6f;
    const float padding = ImGui::GetStyle().FramePadding.x + 4.0f;
    const float wrapWidth = std::max(nodeWidth - padding * 2.0f, 1.0f);
    struct Node { const char* title; std::string detail; int section; };
    const std::array<Node, 3> nodes {{
        {MenuLocale::Translate("Prepare NR input"), MenuLocale::Translate(view.input.c_str()), int(Section::Input)},
        {MenuLocale::Translate("NR model"), MenuLocale::Translate(view.model.c_str()), int(Section::Model)},
        {MenuLocale::Translate("Apply NR edit"), MenuLocale::Translate(view.output.c_str()), int(Section::Output)}
    }};
    float height = 0;
    for (const auto& node : nodes)
        height = std::max(height, MenuUi::CalcTextSize(node.title, nullptr, false, wrapWidth).y +
            MenuUi::CalcTextSize(node.detail.c_str(), nullptr, false, wrapWidth).y + 12.0f);
    const float step = height + gap;
    auto* draw = ImGui::GetWindowDrawList();
    const float x = origin.x + (width - nodeWidth) * 0.5f;
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int i = 0; i < int(nodes.size()); ++i) {
        const auto& node = nodes[i];
        const ImVec2 at(x, origin.y + i * step);
        if (i) {
            const ImVec2 end(x + nodeWidth * 0.5f, at.y);
            draw->AddLine(ImVec2(end.x, at.y - gap), end, line, 1.5f);
            draw->AddTriangleFilled(end, ImVec2(end.x - 3, end.y - 4), ImVec2(end.x + 3, end.y - 4), line);
        }
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(i);
        const bool editable = node.section >= 0;
        bool hovered = false;
        if (editable) {
            if (ImGui::InvisibleButton("stage", ImVec2(nodeWidth, height))) selected = Section(node.section);
            hovered = ImGui::IsItemHovered();
        } else ImGui::Dummy(ImVec2(nodeWidth, height));
        const bool chosen = editable && node.section == int(selected);
        const auto background = chosen ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_ButtonHovered :
            editable ? ImGuiCol_Button : ImGuiCol_FrameBg;
        auto fill = ImGui::GetStyleColorVec4(background);
        if (node.section == int(Section::Model)) {
            const float b = std::max({fill.x, fill.y, fill.z});
            fill = ImVec4(b * .20f, b * .65f, b * .30f, fill.w);
        }
        if (!view.enabled && editable) fill.w *= .55f;
        draw->AddRectFilled(at, ImVec2(at.x + nodeWidth, at.y + height), ImGui::GetColorU32(fill),
            ImGui::GetStyle().FrameRounding);
        const auto title = MenuUi::CalcTextSize(node.title, nullptr, false, wrapWidth);
        const auto detail = MenuUi::CalcTextSize(node.detail.c_str(), nullptr, false, wrapWidth);
        const float y = at.y + (height - title.y - detail.y) * .5f;
        draw->AddText(nullptr, 0, ImVec2(at.x + (nodeWidth - title.x) * .5f, y),
            ImGui::GetColorU32(ImGuiCol_Text), node.title, nullptr, wrapWidth);
        draw->AddText(nullptr, 0, ImVec2(at.x + (nodeWidth - detail.x) * .5f, y + title.y),
            ImGui::GetColorU32(editable ? ImGuiCol_Text : ImGuiCol_TextDisabled), node.detail.c_str(), nullptr, wrapWidth);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + nodes.size() * step));
    ImGui::Dummy(ImVec2(width, 0));
    MenuUi::TextWrapped("%s", view.beforeSr ?
        "Then: Super Resolution -> game effects / HUD -> presentation" :
        "Then: game effects / HUD -> presentation (game-dependent)");
    ImGui::PopID();
}
// Short page buttons stay usable when the chart is collapsed and wrap in narrow columns.
inline void Navigation(Section& selected)
{
    for (int i = 0; i < 3; ++i) {
        const auto page = Section(i);
        static const char* labels[] = {"Input", "Model", "Output"};
        const char* label = labels[i];
        const float buttonWidth = MenuUi::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (i && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + buttonWidth <= right)
            ImGui::SameLine();
        const bool chosen = page == selected;
        if (chosen) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (MenuUi::Button(label)) selected = page;
        if (chosen) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) MenuUi::SetTooltip("%s", SectionName(page));
    }
}
}
