// Real ImGui widgets, glyphs and WARP rendering; no game or visible window.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuUi.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuFont.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/font/Hack_Compressed.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_PipelineUi.h"
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
    for (bool hq : {false, true})
    {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr; io.DeltaTime = 1.f / 60;
    if (hq) io.Fonts->AddFontFromMemoryCompressedBase85TTF(hack_compressed_compressed_data_base85, 14);
    else io.Fonts->AddFontDefault();
    assert(MenuFont::MergeChinese(io.Fonts, chineseFont.data(), static_cast<int>(chineseFont.size())));
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
                    assert(height >= latinHeight * .95f && height <= latinHeight * 1.3f);
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
            MenuUi::TextWrapped("ViT adaptive reuse is unavailable while Temporal history is enabled. Your settings are retained."); within();
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
                    (std::string(hq ? "hq-" : "default-") + Code(lang) + "-" + std::to_string(int(scale)) + ".bmp"));
        }
        ++layouts;
    }
    ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext();
    }
    std::printf("Menu localization: PASS (%u font/language/width/scale layouts, mixed glyph metrics, footer, IDs, disabled controls, WARP)\n", layouts);
}
