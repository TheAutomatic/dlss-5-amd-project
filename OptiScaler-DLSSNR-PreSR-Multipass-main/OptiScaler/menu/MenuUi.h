#pragma once
#include "MenuLocalization.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <utility>
#include <vector>
#include <cstring>

// Local display adapters: never pass translated values back to Config or the model.
namespace MenuUi
{
using MenuLocale::Translate;
inline ImGuiID GetID(const char* label) { return ImGui::GetID(MenuLocale::IdSuffix(label).c_str()); }
inline void OpenPopup(const char* label, ImGuiPopupFlags flags = 0)
{ ImGui::OpenPopup(GetID(label), flags); }
template<class T> inline T Argument(T value) { return value; }
inline const char* Argument(const char* value) { return Translate(value); }
inline const char* Argument(char* value) { return Translate(value); }

template<class... Args> inline void Text(const char* format, Args... args)
{
    if constexpr (sizeof...(args) == 0) ImGui::TextWrapped("%s", Translate(format));
    else ImGui::TextWrapped(Translate(format), Argument(args)...);
}
template<class... Args> inline void TextWrapped(const char* format, Args... args) { Text(format, args...); }
inline void TextUnformatted(const char* text, const char* end = nullptr)
{
    const std::string value = end ? std::string(text, end) : std::string(text ? text : "");
    ImGui::TextWrapped("%s", Translate(value.c_str()));
}
template<class... Args> inline void TextColored(const ImVec4& color, const char* format, Args... args)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    Text(format, args...);
    ImGui::PopStyleColor();
}
template<class... Args> inline void TextDisabled(const char* format, Args... args)
{
    TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), format, args...);
}
template<class... Args> inline void SetTooltip(const char* format, Args... args)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    Text(format, args...);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
inline ImVec2 CalcTextSize(const char* text, const char* end = nullptr, bool hide = false, float wrap = -1)
{
    const std::string value = end ? std::string(text, end) : std::string(text ? text : "");
    return ImGui::CalcTextSize(Translate(value.c_str()), nullptr, hide, wrap);
}
inline void SeparatorText(const char* label) { ImGui::SeparatorText(Translate(label)); }

inline void FitSameLine(float width)
{
    // A SameLine() chosen with an English label must not force translated text
    // outside the column. Do not change the order or add a new settings container.
    if (ImGui::GetCurrentWindow()->DC.IsSameLine && width > ImGui::GetContentRegionAvail().x)
        ImGui::NewLine();
}
inline std::string WrapLabel(const char* original, float width)
{
    const std::string_view full(original);
    const std::string visible(full.substr(0, full.find("##")));
    const char* text = Translate(visible.c_str());
    if (ImGui::CalcTextSize(text).x <= width || visible.empty()) return MenuLocale::Label(original);
    width = std::max(ImGui::GetFontSize(), width);
    std::string wrapped;
    const char* end = text + std::strlen(text);
    for (const char* p = text; p < end;)
    {
        const char* next = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), p, end, width);
        if (next <= p)
        {
            unsigned int codepoint;
            next = p + std::max(1, ImTextCharFromUtf8(&codepoint, p, end));
        }
        wrapped.append(p, next);
        p = next;
        while (p < end && (*p == ' ' || *p == '\n')) ++p;
        if (p < end) wrapped += '\n';
    }
    return wrapped + MenuLocale::IdSuffix(original);
}
inline std::string ValueLabel(const char* original)
{
    const auto label = MenuLocale::Label(original);
    const float textWidth = ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
    const float requestedWidth = ImGui::CalcItemWidth();
    const float minimum = ImGui::GetFontSize() * 6;
    FitSameLine(minimum + textWidth + ImGui::GetStyle().ItemInnerSpacing.x);
    const float available = ImGui::GetContentRegionAvail().x;
    if (textWidth > 0 && minimum + textWidth + ImGui::GetStyle().ItemInnerSpacing.x > available)
    {
        const auto visible = label.substr(0, label.find("##"));
        ImGui::TextWrapped("%s", visible.c_str());
        ImGui::SetNextItemWidth(std::max(1.0f, std::min(requestedWidth, ImGui::GetContentRegionAvail().x)));
        return MenuLocale::IdSuffix(original);
    }
    ImGui::SetNextItemWidth(std::max(1.0f, std::min(requestedWidth,
        available - (textWidth > 0 ? textWidth + ImGui::GetStyle().ItemInnerSpacing.x : 0))));
    return label;
}
inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    const auto display = MenuLocale::Label(label);
    const float width = size.x > 0 ? size.x : ImGui::CalcTextSize(display.c_str(), nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x;
    FitSameLine(width);
    const auto wrapped = WrapLabel(label, (size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x) -
                                          2 * ImGui::GetStyle().FramePadding.x);
    return ImGui::Button(wrapped.c_str(), size);
}
inline bool SmallButton(const char* label)
{
    const auto display = MenuLocale::Label(label);
    FitSameLine(ImGui::CalcTextSize(display.c_str(), nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x);
    return ImGui::SmallButton(WrapLabel(label, ImGui::GetContentRegionAvail().x -
                                       2 * ImGui::GetStyle().FramePadding.x).c_str());
}
inline bool Checkbox(const char* label, bool* value)
{
    const auto display = MenuLocale::Label(label);
    FitSameLine(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                ImGui::CalcTextSize(display.c_str(), nullptr, true).x);
    return ImGui::Checkbox(WrapLabel(label, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                    ImGui::GetStyle().ItemInnerSpacing.x).c_str(), value);
}

#define MENU_LABEL_ADAPTER(name) \
    template<class... Args> inline auto name(const char* label, Args&&... args) \
    { return ImGui::name(MenuLocale::Label(label).c_str(), std::forward<Args>(args)...); }
MENU_LABEL_ADAPTER(Begin)
MENU_LABEL_ADAPTER(BeginPopupModal)
MENU_LABEL_ADAPTER(TextLinkOpenURL)
#undef MENU_LABEL_ADAPTER

#define MENU_WRAP_ADAPTER(name) \
    template<class... Args> inline auto name(const char* label, Args&&... args) \
    { \
        FitSameLine(ImGui::CalcTextSize(MenuLocale::Label(label).c_str(), nullptr, true).x + ImGui::GetFrameHeight()); \
        return ImGui::name(WrapLabel(label, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - \
            ImGui::GetStyle().ItemInnerSpacing.x).c_str(), std::forward<Args>(args)...); \
    }
MENU_WRAP_ADAPTER(CheckboxFlags)
MENU_WRAP_ADAPTER(RadioButton)
MENU_WRAP_ADAPTER(Selectable)
MENU_WRAP_ADAPTER(CollapsingHeader)
MENU_WRAP_ADAPTER(TreeNode)
MENU_WRAP_ADAPTER(TreeNodeEx)
#undef MENU_WRAP_ADAPTER

#define MENU_VALUE_ADAPTER(name) \
    template<class... Args> inline auto name(const char* label, Args&&... args) \
    { return ImGui::name(ValueLabel(label).c_str(), std::forward<Args>(args)...); }
MENU_VALUE_ADAPTER(SliderFloat)
MENU_VALUE_ADAPTER(SliderInt)
MENU_VALUE_ADAPTER(InputFloat)
MENU_VALUE_ADAPTER(InputInt)
MENU_VALUE_ADAPTER(InputText)
MENU_VALUE_ADAPTER(ColorEdit3)
#undef MENU_VALUE_ADAPTER

inline bool InputScalar(const char* label, ImGuiDataType type, void* value, const void* step = nullptr,
                        const void* fast = nullptr, const char* format = nullptr, ImGuiInputTextFlags flags = 0)
{
    return ImGui::InputScalar(ValueLabel(label).c_str(), type, value, step, fast, format, flags);
}

inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0)
{
    return ImGui::BeginCombo(ValueLabel(label).c_str(), Translate(preview), flags);
}
inline bool Combo(const char* label, int* selected, const char* const items[], int count, int height = -1)
{
    std::vector<const char*> translated;
    translated.reserve(count);
    for (int i = 0; i < count; ++i) translated.push_back(Translate(items[i]));
    return ImGui::Combo(ValueLabel(label).c_str(), selected, translated.data(), count, height);
}
inline bool Combo(const char* label, int* selected, const char* items, int height = -1)
{
    std::vector<const char*> values;
    for (auto p = items; *p; p += std::strlen(p) + 1) values.push_back(p);
    return Combo(label, selected, values.data(), static_cast<int>(values.size()), height);
}
}
