#include "pch.h"
#include "DlssNr_PipelineUi.h"
#include "amd/AmdBridge.h"
#include "backend/Selector.h"
#include "submission/SubmissionHooks.h"
#include "AsiTakeover.h"
#include "DlssNr.h"

#include <Config.h>
#include <Util.h>
#include <State.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <optional>

namespace DlssNr
{

static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    MenuUi::FitSameLine(MenuUi::CalcTextSize("(?)").x);
    MenuUi::TextDisabled("(?)");

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        MenuUi::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static bool MochizukiOptionOnPage(std::string_view key, PipelineUi::Section page)
{
    using PipelineUi::Section;
    if (key == CfgKey::MochizukiModelScale || key == CfgKey::MochizukiWhitePoint ||
        key == CfgKey::MochizukiLinearInput || key == CfgKey::MochizukiDynamicResolution ||
        key == CfgKey::MochizukiPreprocess || key == CfgKey::MochizukiPreprocessExposure ||
        key == CfgKey::MochizukiPreprocessBiasEv || key == CfgKey::MochizukiPreprocessCurve ||
        key == CfgKey::MochizukiPreprocessContrast || key == CfgKey::MochizukiPreprocessSaturation)
        return page == Section::Input;
    if (key == CfgKey::MochizukiDetailStrength || key == CfgKey::MochizukiColourStrength ||
        key == CfgKey::MochizukiMaxRatio || key == CfgKey::MochizukiApplyModel ||
        key == CfgKey::MochizukiEnlargeMode || key == CfgKey::MochizukiClassicScaler ||
        key == CfgKey::MochizukiCompactTransfer)
        return page == Section::Output;
    return page == Section::Model;
}

struct AcoMenuChoice
{
    bool captured = false;
    bool session = false;
    bool shown = false;
};
static AcoMenuChoice& AcoMenu()
{
    static AcoMenuChoice choice;
    return choice;
}

static void ResetMochizuki(Config* config, int selectedGroup = -1,
    std::optional<PipelineUi::Section> page = std::nullopt)
{
    // ACO is a standalone startup switch; reset actions preserve both current and saved choices.
#define MZ_F(name, def, low, high, group, label, target) if ((selectedGroup < 0 || selectedGroup == group) && (!page || MochizukiOptionOnPage(CfgKey::name, *page))) config->name = std::optional<float>{};
#define MZ_U(name, def, low, high, group, label, target) if ((selectedGroup < 0 || selectedGroup == group) && (!page || MochizukiOptionOnPage(CfgKey::name, *page))) config->name = std::optional<uint32_t>{};
#define MZ_B(name, def, low, high, group, label, target) if (std::string_view(CfgKey::name) != CfgKey::MochizukiAcoMode && (selectedGroup < 0 || selectedGroup == group) && (!page || MochizukiOptionOnPage(CfgKey::name, *page))) config->name = std::optional<bool>{};
#include "backend/MochizukiOptions.inc"
#undef MZ_F
#undef MZ_U
#undef MZ_B
    if ((selectedGroup < 0 || selectedGroup == 4) &&
        (!page || *page == PipelineUi::Section::Input))
    {
        config->MochizukiWhitePointSource = std::optional<uint32_t> {};
    }
    if ((selectedGroup < 0 || selectedGroup == 1) &&
        (!page || *page == PipelineUi::Section::Model))
    {
        config->MochizukiUnlockPasses = std::optional<bool> {};
        config->MochizukiLayerMode = std::optional<uint32_t> {};
    }
    AmdBridge::InvalidateHistory();
}

static bool MochizukiChoice(const char* label, std::string_view key, int* value, int low, int high)
{
    (void)key;
    return MenuUi::SliderInt(label, value, low, high);
}

struct NrCompatibilityMenuBaseline
{
    std::optional<bool> earlyExe, earlyAny, enhancedBarriers;
};

static const NrCompatibilityMenuBaseline& CompatibilityMenuBaseline(Config* config)
{
    static const NrCompatibilityMenuBaseline initial {
        config->LmxxfEarlyExeWrap.value_for_config(), config->LmxxfEarlyAnyModuleWrap.value_for_config(),
        config->LmxxfAllowEnhancedBarriers.value_for_config()
    };
    return initial;
}

static void RenderNrCompatibilityNotice(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.20f, 1.0f));
    MenuUi::TextWrapped(text);
    ImGui::PopStyleColor();
}

static void RenderEnhancedBarrierSaveNotice(Config* config)
{
    if (config->LmxxfAllowEnhancedBarriers.value_for_config() != CompatibilityMenuBaseline(config).enhancedBarriers)
        RenderNrCompatibilityNotice("Applies immediately. Save Settings to keep this choice.");
}

static void RenderAllowEnhancedBarriers(Config* config)
{
    const char* names[] = {Config::ExePolicyEnhancedBarriers() ? "auto (current: on)" : "auto (current: off)", "force on", "force off"};
    int index = config->LmxxfAllowEnhancedBarriers.has_value() ? (*config->LmxxfAllowEnhancedBarriers ? 1 : 2) : 0;
    if (MenuUi::Combo("Allow enhanced barriers", &index, names, IM_ARRAYSIZE(names)))
    {
        if (index == 0) config->LmxxfAllowEnhancedBarriers.reset();
        else config->LmxxfAllowEnhancedBarriers = (index == 1);
        SetAllowEnhancedBarriers(config->AllowEnhancedBarriersEffective());
    }
}

static void RenderSplitQuerySplice(Config* config)
{
    const char* names[] = {Config::ExePolicySplitQuerySplice() ? "auto (current: on)" : "auto (current: off)", "force on", "force off"};
    int index = config->NrSplitQuerySplice.has_value() ? (*config->NrSplitQuerySplice ? 1 : 2) : 0;
    if (MenuUi::Combo("Split query splice", &index, names, IM_ARRAYSIZE(names)))
    {
        if (index == 0) config->NrSplitQuerySplice.reset();
        else config->NrSplitQuerySplice = (index == 1);
        SetSplitQuerySplice(config->SplitQuerySpliceEffective());
    }
    HelpMarker("Lets split command lists run when a non-timestamp query stays open across"
               "\nevaluation (otherwise rejected as open_query, e.g. pipeline-statistics scopes)."
               "\nAuto: on for Crimson Desert, off otherwise. Applies immediately."
               "\nINI: NrSplitQuerySplice.");
}

static void RenderEarlyCommandListWrap(Config* config)
{
    static const char* names[] = {"auto (game compatibility)", "force on", "force off"};
    int index = config->LmxxfEarlyExeWrap.has_value() ? (*config->LmxxfEarlyExeWrap ? 1 : 2) : 0;
    if (MenuUi::Combo("Early command-list wrap", &index, names, IM_ARRAYSIZE(names)))
    {
        if (index == 0) config->LmxxfEarlyExeWrap.reset();
        else config->LmxxfEarlyExeWrap = (index == 1);
    }
    HelpMarker("Wrap command lists created before the swapchain."
               "\nSave and restart to apply. INI: LmxxfEarlyExeWrap.");
    if (config->LmxxfEarlyExeWrap.value_for_config() != CompatibilityMenuBaseline(config).earlyExe)
        RenderNrCompatibilityNotice("Save Settings and restart the game to apply early wrapping.");
    const bool autoAny = GetNrGamePolicy(State::Instance().gameExe).earlyAnyModuleWrap;
    const char* anyNames[] = {autoAny ? "auto (current: on)" : "auto (current: off)", "force on", "force off"};
    int anyIndex = config->LmxxfEarlyAnyModuleWrap.has_value() ? (*config->LmxxfEarlyAnyModuleWrap ? 1 : 2) : 0;
    ImGui::Indent();
    if (MenuUi::Combo("Early wrap all modules (Streamline/overlays)", &anyIndex, anyNames, IM_ARRAYSIZE(anyNames)))
    {
        if (anyIndex == 0) config->LmxxfEarlyAnyModuleWrap.reset();
        else config->LmxxfEarlyAnyModuleWrap = (anyIndex == 1);
    }
    HelpMarker("Also wrap command lists created by any module before the swapchain is created."
               "\nSave and restart to apply. INI: LmxxfEarlyAnyModuleWrap.");
    if (config->LmxxfEarlyAnyModuleWrap.value_for_config() != CompatibilityMenuBaseline(config).earlyAny)
        RenderNrCompatibilityNotice("Save Settings and restart the game to apply early wrapping.");
    ImGui::Unindent();
}

static void RenderRecordingCompatibilitySettings(Config* config)
{
    RenderEarlyCommandListWrap(config);
    RenderAllowEnhancedBarriers(config);
    RenderEnhancedBarrierSaveNotice(config);
    RenderSplitQuerySplice(config);
}

static void RenderAsiTakeoverNotice()
{
    const auto path = DlssNr::AsiTakeover::ModulePath();
    const auto version = DlssNr::AsiTakeover::ModuleVersion();
    const auto menuKey = DlssNr::AsiTakeover::MenuKey();

    auto* window = ImGui::GetCurrentWindow();
    auto* draw = window->DrawList;
    const auto& style = ImGui::GetStyle();
    const float baseFont = ImGui::GetFontSize();
    const float padX = style.WindowPadding.x;
    const float padY = style.WindowPadding.y * 0.85f;
    const float accentWidth = std::max(4.0f, std::round(baseFont * 0.28f));

    const ImVec2 boxMin = window->DC.CursorPos;
    const float boxWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);

    window->DC.CursorPos.y += padY;
    const float indentX = accentWidth + padX;
    ImGui::Indent(indentX);
    const float oldWorkMaxX = window->WorkRect.Max.x;
    const float oldContentMaxX = window->ContentRegionRect.Max.x;
    window->WorkRect.Max.x -= padX;
    window->ContentRegionRect.Max.x -= padX;

    ImGui::PushFontSize(std::round(baseFont * 1.22f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.28f, 0.92f, 0.56f, 1.0f));
    MenuUi::TextWrapped("Mochizuki ASI plugin detected; NR is managed by the plugin (check NR Control for live status)");
    ImGui::PopStyleColor();
    ImGui::PopFontSize();

    ImGui::Spacing();

    if (!menuKey.empty())
    {
        ImGui::PushFontSize(std::round(baseFont * 1.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.88f, 0.32f, 1.0f));
        MenuUi::TextWrapped("Press the [ %s ] key on your keyboard to open the independent NR Control menu",
                            menuKey.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFontSize();

        ImGui::PushFontSize(std::round(baseFont * 1.05f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
        MenuUi::TextWrapped("(The End key is usually above the arrow keys next to Delete / PageDown; if you changed MenuKey in the launcher or dlssnr-amd.ini, press your configured key.)");
        ImGui::PopStyleColor();
        ImGui::PopFontSize();
    }
    else
    {
        ImGui::PushFontSize(std::round(baseFont * 1.20f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.88f, 0.32f, 1.0f));
        MenuUi::TextWrapped("NR Control menu hotkey is disabled in dlssnr-amd.ini ([NRControl] MenuKey is empty)");
        ImGui::PopStyleColor();
        ImGui::PopFontSize();

        ImGui::PushFontSize(std::round(baseFont * 1.05f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
        MenuUi::TextWrapped("(Set [NRControl] MenuKey = End in dlssnr-amd.ini or the launcher to re-enable the NR Control menu hotkey.)");
        ImGui::PopStyleColor();
        ImGui::PopFontSize();
    }

    ImGui::Spacing();

    MenuUi::TextWrapped("All NR model switching, quality parameters, and hotkeys are configured in that independent window.");

    if (!path.empty())
        MenuUi::TextDisabled("Module: %s", wstring_to_string(path).c_str());
    if (version.empty())
        MenuUi::TextDisabled("Version information not provided by the ASI");
    else
        MenuUi::TextDisabled("Version: %s", version.c_str());

    ImGui::Unindent(indentX);
    window->WorkRect.Max.x = oldWorkMaxX;
    window->ContentRegionRect.Max.x = oldContentMaxX;

    const float boxMaxY = window->DC.CursorPos.y - style.ItemSpacing.y + padY;
    const ImVec2 boxMax(boxMin.x + boxWidth, boxMaxY);

    draw->AddRectFilled(boxMin, ImVec2(boxMin.x + accentWidth, boxMaxY),
                        ImGui::GetColorU32(ImVec4(0.24f, 0.84f, 0.50f, 0.95f)),
                        style.FrameRounding, ImDrawFlags_RoundCornersLeft);
    draw->AddRect(boxMin, boxMax,
                  ImGui::GetColorU32(ImVec4(0.24f, 0.84f, 0.50f, 0.70f)),
                  style.FrameRounding, 0, 1.5f);

    window->DC.CursorPos.y = boxMaxY;
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.5f));
}

void RenderMenu(Config* config, float menuResScale)
{
    (void)menuResScale;
    const bool asiOwned = DlssNr::AsiTakeover::Active();
    if (asiOwned)
        RenderAsiTakeoverNotice();

    CompatibilityMenuBaseline(config);

    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (!asiOwned)
        {
            const auto issue = Backend::InstallIssue(Backend::Kind::Mochizuki);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .72f, .25f, 1.f));
            if (!issue.empty())
                MenuUi::TextWrapped("%s", issue.c_str());
            else
                MenuUi::TextWrapped("Mochizuki ASI plugin (dlssnr-amd.asi) is not loaded in this session. Enable Plugins.LoadAsiPlugins=true in OptiScaler.ini and restart the game.");
            ImGui::PopStyleColor();
        }
        else
        {
            MenuUi::TextWrapped("Neural Rendering is active via dlssnr-amd.asi. Use the NR Control menu to adjust models and parameters.");
        }

        if (MenuUi::TreeNode("Compatibility & Scheduling"))
        {
            RenderRecordingCompatibilitySettings(config);
            ImGui::TreePop();
        }
    }
}

} // namespace DlssNr
