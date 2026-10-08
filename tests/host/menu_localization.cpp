// Real ImGui widgets, glyphs and WARP rendering; no game or visible window.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuUi.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuFont.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/font/Hack_Compressed.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_PipelineUi.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuNavigation.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuCard.h"
#include <imgui/imgui_impl_dx11.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <thread>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr) { assert(SUCCEEDED(hr)); }
static void SaveBmp(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                    const std::filesystem::path& path)
{
    D3D11_TEXTURE2D_DESC desc; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped;
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    BITMAPFILEHEADER file {}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
    BITMAPINFOHEADER info {}; info.biSize = sizeof(info); info.biWidth = desc.Width;
    info.biHeight = -static_cast<LONG>(desc.Height); info.biPlanes = 1; info.biBitCount = 32;
    std::ofstream out(path, std::ios::binary); assert(out);
    out.write(reinterpret_cast<char*>(&file), sizeof(file)); out.write(reinterpret_cast<char*>(&info), sizeof(info));
    for (UINT y = 0; y < desc.Height; ++y)
        out.write(static_cast<char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    assert(out.good());
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace MenuLocale;
    assert(Parse("") == Language::English && Parse("invalid") == Language::English);
    assert(Parse("en") == Language::English && Parse("zh-CN") == Language::SimplifiedChinese);
    current = Language::SimplifiedChinese;
    std::thread isolated([] { assert(current == Language::English); current = Language::English; });
    isolated.join(); assert(current == Language::SimplifiedChinese);
    assert(std::string(Translate("untranslated runtime detail")) == "untranslated runtime detail");

    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &context));
    std::ifstream fontFile("OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/font/NotoSansSC-Menu.ttf", std::ios::binary);
    std::vector<char> chineseFont((std::istreambuf_iterator<char>(fontFile)), {});
    assert(!chineseFont.empty());
    unsigned layouts = 0;
    for (int fontMode : {0, 1, 2}) // unified default, explicit custom Hack, non-HQ
    {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr; io.DeltaTime = 1.f / 60;
    if (fontMode == 0)
        assert(MenuFont::AddUnified(io.Fonts, chineseFont.data(), static_cast<int>(chineseFont.size()), 14));
    else {
        if (fontMode == 1) io.Fonts->AddFontFromMemoryCompressedBase85TTF(hack_compressed_compressed_data_base85, 14);
        else io.Fonts->AddFontDefault();
        assert(MenuFont::MergeChinese(io.Fonts, chineseFont.data(), static_cast<int>(chineseFont.size())));
    }
    assert(ImGui_ImplDX11_Init(device.Get(), context.Get()));
    for (const auto lang : {Language::English, Language::SimplifiedChinese})
    for (float scale : {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f})
    for (int width : {320, 480, 960, 1920})
    {
        current = lang;
        io.DisplaySize = ImVec2(static_cast<float>(width), 2600);
        ImGui::GetStyle() = ImGuiStyle(); ImGui::StyleColorsDark(); ImGui::GetStyle().ScaleAllSizes(scale);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            ImGui_ImplDX11_NewFrame(); ImGui::NewFrame(); ImGui::PushFontSize(14 * scale);
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Localization audit", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            auto within = [&](float decoration = 0.f) {
                const float right = ImGui::GetCurrentWindow()->WorkRect.Max.x + decoration;
                if (ImGui::GetItemRectMax().x > right + 1)
                    std::fprintf(stderr, "Overflow lang=%d scale=%g width=%d item=%g right=%g id=%u tree=%u radio=%u\n", int(lang), scale, width, ImGui::GetItemRectMax().x, right, GImGui->LastItemData.ID, MenuUi::GetID("Advanced preset hints (effect unverified)"), MenuUi::GetID("Depth Aware (RCAS)"));
                assert(ImGui::GetItemRectMax().x <= right + 1);
            };
            for (const char* original : {"Save Settings", "Reset this page##input", "Title###stable"})
            {
                const auto id = MenuUi::GetID(original);
                current = Language::English;
                assert(ImGui::GetID(Label(original).c_str()) == id);
                current = Language::SimplifiedChinese;
                assert(ImGui::GetID(Label(original).c_str()) == id);
                current = lang;
            }
            assert(MenuUi::GetID("foo") != MenuUi::GetID("##foo"));
            assert(MenuUi::GetID("#foo") != MenuUi::GetID("foo"));
            if (repeat == 0 && scale == 1 && width == 480)
            {
                for (const auto& entry : entries)
                    for (const char* p = entry.chinese; *p;)
                    {
                        unsigned int cp; const int n = ImTextCharFromUtf8(&cp, p, nullptr); assert(n > 0); p += n;
                        if (cp >= 0x2000) assert(ImGui::GetFontBaked()->FindGlyphNoFallback(static_cast<ImWchar>(cp)));
                    }
            }
            MenuUi::SeparatorText("DLSS Neural Rendering");
            if (fontMode == 0) {
                // Prevent a mixed-family regression, not merely similar bounding boxes.
                assert(ImGui::GetFont()->Sources.Size == 1);
                for (ImWchar cp : {ImWchar('N'), ImWchar('R'), ImWchar('0'), ImWchar(0x795e), ImWchar(0x6a21)}) {
                    const auto* glyph = ImGui::GetFontBaked()->FindGlyphNoFallback(cp);
                    assert(glyph && glyph->SourceIdx == 0);
                }
                const float digitWidth = ImGui::GetFontBaked()->FindGlyph('0')->AdvanceX;
                for (ImWchar cp = '1'; cp <= '9'; ++cp)
                    assert(std::abs(ImGui::GetFontBaked()->FindGlyph(cp)->AdvanceX - digitWidth) < .01f);
            }
            if (scale >= 1)
            {
                const auto* latin = ImGui::GetFontBaked()->FindGlyph('N');
                const float latinHeight = latin->Y1 - latin->Y0;
                const float latinCenter = (latin->Y0 + latin->Y1) * .5f;
                for (ImWchar codepoint : {ImWchar(0x795e), ImWchar(0x6a21), ImWchar(0x578b)})
                {
                    const auto* chinese = ImGui::GetFontBaked()->FindGlyphNoFallback(codepoint);
                    assert(chinese);
                    const float height = chinese->Y1 - chinese->Y0;
                    // A single Noto family has taller CJK faces than Latin caps.
                    // At 21px, integer rasterization yields 15px vs 11px; allow
                    // one rounding pixel without accepting undersized Chinese.
                    const float rounding = fontMode == 0 ? 1.f : 0.f;
                    assert(height >= latinHeight * .95f && height <= latinHeight * 1.3f + rounding);
                    assert(std::abs((chinese->Y0 + chinese->Y1) * .5f - latinCenter) <= 1.5f * scale);
                }
            }
            bool enabled = true;
            MenuUi::Checkbox("Enable NR", &enabled); within();
            ImGui::SameLine(); MenuUi::Checkbox("Allow backend hot switching (restart)", &enabled); within();
            assert(GImGui->LastItemData.ID == MenuUi::GetID("Allow backend hot switching (restart)"));
            int order = 0;
            MenuUi::Combo("Processing order", &order, "NR -> SR (default)\0SR -> NR (experimental)\0"); within();
            DlssNr::PipelineUi::View view {"lmxxf", "Native input resolution", "NR model", "Strength / colour"};
            auto page = DlssNr::PipelineUi::Section::Model;
            DlssNr::PipelineUi::Draw(view, page); within();
            DlssNr::PipelineUi::Navigation(page); within();
            MenuUi::Checkbox("Temporal history (anti-flicker)", &enabled); within();
            bool adaptive = false;
            ImGui::BeginDisabled(true);
            MenuUi::Checkbox("ViT adaptive reuse", &adaptive); within();
            assert(GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled);
            for (const char* label : {"Reuse global", "Reuse local", "Reuse image", "Reuse period"})
            {
                float value = 1;
                ImGui::SetNextItemWidth(300 * scale);
                MenuUi::SliderFloat(label, &value, 0.f, 2.f); within();
                assert(GImGui->LastItemData.ID == MenuUi::GetID(label));
                assert(GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled);
            }
            ImGui::EndDisabled(); assert(!adaptive);
            for (const char* label : {"Low-frequency gain", "Fine-detail gain",
                                      "Skin detail protection", "Edge detail protection"}) {
                float value = 1.f;
                ImGui::SetNextItemWidth(300 * scale);
                MenuUi::SliderFloat(label, &value, 0.f, 2.f); within();
                assert(GImGui->LastItemData.ID == MenuUi::GetID(label));
                ImGui::SameLine(); MenuUi::FitSameLine(MenuUi::CalcTextSize("(?)").x);
                MenuUi::TextDisabled("(?)"); within();
            }
            MenuUi::TextWrapped("ViT adaptive reuse is unavailable while Temporal history is enabled. Your settings are retained."); within();
            int stream = 3;
            MenuUi::Combo("ViT stream (exp)", &stream, "Off\0AV FP8\0Contract F16\0Both\0"); within();
            MenuUi::Checkbox("Override inherited controls", &enabled); within();
            MenuUi::Checkbox("Skin structure follows structure", &enabled); within();
            char skips[64] = "42,43,46";
            MenuUi::InputText("Extra skipped blocks in passes 2/3", skips, sizeof(skips)); within();
            assert(GImGui->LastItemData.ID == MenuUi::GetID("Extra skipped blocks in passes 2/3"));
            MenuUi::Button("Reset this page"); within(); ImGui::SameLine();
            MenuUi::Button("Save Settings"); within(); ImGui::SameLine(); MenuUi::Button("Close"); within();
            int language = lang == Language::English ? 0 : 1;
            const float footerWidth = MenuUi::LanguageSelectorWidth() + ImGui::GetStyle().ItemSpacing.x +
                MenuUi::CalcTextSize("Save Settings").x + 2 * ImGui::GetStyle().FramePadding.x;
            const bool footerFits = footerWidth <= ImGui::GetContentRegionAvail().x;
            MenuUi::LanguageSelector(&language); within();
            assert(GImGui->LastItemData.ID == MenuUi::GetID("Language"));
            const float comboTop = ImGui::GetItemRectMin().y;
            const float comboWidth = ImGui::GetItemRectSize().x;
            assert(comboWidth <= MenuUi::LanguageComboWidth() + 1);
            ImGui::SameLine(); MenuUi::Button("Save Settings"); within();
            if (footerFits) assert(ImGui::GetItemRectMin().y == comboTop);
            // Framed headers deliberately extend into the window padding.
            MenuUi::CollapsingHeader("Compatibility & Scheduling"); within(ImGui::GetCurrentWindow()->WindowPadding.x * .5f);
            if (MenuUi::TreeNode("Advanced preset hints (effect unverified)")) ImGui::TreePop();
            // Tree nodes deliberately enlarge the clickable area after the label.
            within(ImGui::GetStyle().ItemSpacing.x * 2);
            MenuUi::RadioButton("Depth Aware (RCAS)", false); within();
            MenuUi::Selectable("Advanced resolution"); within(ImGui::GetStyle().ItemSpacing.x * .5f);
            MenuUi::OpenPopup("New wait restart");
            if (MenuUi::BeginPopupModal("New wait restart", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            else assert(false && "localized popup identity must match its opener");
            ImGui::End(); ImGui::PopFontSize(); ImGui::Render();
            D3D11_TEXTURE2D_DESC desc {}; desc.Width = width; desc.Height = 2600; desc.MipLevels = 1;
            desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target; ComPtr<ID3D11RenderTargetView> rtv;
            Check(device->CreateTexture2D(&desc, nullptr, &target));
            Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
            ID3D11RenderTargetView* raw = rtv.Get(); context->OMSetRenderTargets(1, &raw, nullptr);
            const float clear[] = {.07f, .07f, .08f, 1}; context->ClearRenderTargetView(rtv.Get(), clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (repeat == 1 && width == 480 && (scale == 1 || scale == 2))
                SaveBmp(device.Get(), context.Get(), target.Get(), std::filesystem::path(argv[1]) /
                    (std::string(fontMode == 0 ? "unified-" : fontMode == 1 ? "custom-" : "legacy-") + Code(lang) + "-" + std::to_string(int(scale)) + ".bmp"));
        }
        ++layouts;
    }
    // The actual sidebar/card components: every page, both languages and compact/full layout.
    for (auto lang : {Language::English, Language::SimplifiedChinese})
    for (int width : {560, 1100})
    for (float scale : {1.f, 2.f})
    for (int selected = 0; selected < int(MenuNavigation::Page::Count); ++selected)
    for (int repeat = 0; repeat < 2; ++repeat)
    {
        current = lang;
        io.DisplaySize = {float(width), 780};
        ImGui_ImplDX11_NewFrame(); ImGui::NewFrame(); ImGui::PushFontSize(14 * scale);
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Navigation audit", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
        const float workRight = ImGui::GetCurrentWindow()->WorkRect.Max.x;
        auto page = MenuNavigation::Page(selected);
        int calls = 0;
        MenuNavigation::Render(page, scale, [&](MenuNavigation::Page actual) {
            assert(actual == MenuNavigation::Page(selected));
            assert(ImGui::GetContentRegionAvail().x > 200);
            if (ImGui::GetCurrentWindow()->WorkRect.Max.x > workRight)
                std::fprintf(stderr, "navigation overflow font=%d lang=%s width=%d scale=%.1f page=%d repeat=%d childWork=%.2f parentWork=%.2f inner=%.2f content=%.2f\n",
                    fontMode, Code(lang), width, scale, selected, repeat,
                    ImGui::GetCurrentWindow()->WorkRect.Max.x, workRight,
                    ImGui::GetCurrentWindow()->InnerRect.Max.x, ImGui::GetCurrentWindow()->ContentSize.x);
            assert(ImGui::GetCurrentWindow()->WorkRect.Max.x <= workRight);
            ++calls;
            const auto before = ImGui::GetCurrentWindow()->WorkRect;
            {
                MenuCard card;
                MenuSectionTitle("DLSS Neural Rendering");
                bool enabled = true; int passes = 2; float strength = 1;
                MenuUi::Checkbox("Enable NR", &enabled);
                DlssNr::PipelineUi::View view {"lmxxf", "Native input resolution", "NR model", "Strength / colour"};
                auto section = DlssNr::PipelineUi::Section::Model;
                DlssNr::PipelineUi::Draw(view, section);
                DlssNr::PipelineUi::Navigation(section);
                MenuUi::SliderInt("Passes", &passes, 1, 3);
                MenuUi::SliderFloat("Strength", &strength, 0, 2);
                MenuUi::Checkbox("Temporal history (anti-flicker)", &enabled);
                MenuUi::TextWrapped("ViT adaptive reuse is unavailable while Temporal history is enabled. Your settings are retained.");
            }
            const auto after = ImGui::GetCurrentWindow()->WorkRect;
            assert(before.Min.x == after.Min.x && before.Max.x == after.Max.x);
        });
        assert(calls == 1 && page == MenuNavigation::Page(selected));
        ImGui::End(); ImGui::PopFontSize(); ImGui::Render();
        if (fontMode == 0 && selected == 0 && repeat == 1 && scale == 1)
        {
            D3D11_TEXTURE2D_DESC desc {}; desc.Width = width; desc.Height = 780; desc.MipLevels = 1;
            desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target; ComPtr<ID3D11RenderTargetView> rtv;
            Check(device->CreateTexture2D(&desc, nullptr, &target));
            Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
            ID3D11RenderTargetView* raw = rtv.Get(); context->OMSetRenderTargets(1, &raw, nullptr);
            const float clear[] = {.07f, .07f, .08f, 1}; context->ClearRenderTargetView(rtv.Get(), clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            SaveBmp(device.Get(), context.Get(), target.Get(), std::filesystem::path(argv[1]) /
                ("navigation-" + std::string(Code(lang)) + "-" + std::to_string(width) + ".bmp"));
        }
    }
    ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext();
    }
    std::printf("Menu localization: PASS (%u font/language/width/scale layouts, mixed glyph metrics, footer, IDs, disabled controls, WARP)\n", layouts);
}
