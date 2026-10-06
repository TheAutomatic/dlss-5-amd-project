#pragma once
#include <imgui/imgui.h>

namespace MenuFont
{
// Noto's large ascender/descender box makes its regular CJK strokes look smaller
// next to Hack/Proggy at the same nominal size. Compensate the glyph face, not
// the UI scale, and share the base font's offset for mixed labels such as "NR 模型".
inline ImFont* MergeChinese(ImFontAtlas* atlas, void* data, int bytes)
{
    ImFont* base = atlas->Fonts.back();
    const float baseSize = base->Sources[0]->SizePixels;
    ImFontConfig config;
    config.MergeMode = true;
    config.FontDataOwnedByAtlas = false;
    config.GlyphOffset.y = base->Sources[0]->GlyphOffset.y;
    static const ImWchar ranges[] = {0x2000, 0x206F, 0x3000, 0x9FFF, 0xFF00, 0xFFEF, 0};
    return atlas->AddFontFromMemoryTTF(data, bytes, baseSize * 1.08f, &config, ranges);
}
}
