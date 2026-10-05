#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

namespace MenuLocale
{
enum class Language { English, SimplifiedChinese };
inline Language Parse(std::string_view value)
{
    return value == "zh" || value == "zh-cn" || value == "zh-CN" ? Language::SimplifiedChinese : Language::English;
}
inline const char* Code(Language value) { return value == Language::SimplifiedChinese ? "zh-CN" : "en"; }
// One snapshot per menu frame. No renderer/worker thread changes another thread's locale.
inline thread_local Language current = Language::English;
struct Entry { const char* english; const char* chinese; };
inline constexpr Entry entries[] = {
#include "MenuStrings.inl"
};
inline const char* Translate(const char* text)
{
    if (!text || current != Language::SimplifiedChinese) return text;
    static const auto catalog = [] {
        std::unordered_map<std::string_view, const char*> result;
        for (const auto& entry : entries) result.emplace(entry.english, entry.chinese);
        return result;
    }();
    const auto it = catalog.find(text);
    return it == catalog.end() ? text : it->second;
}
inline std::string IdSuffix(const char* original)
{
    const std::string_view full(original ? original : "");
    const auto marker = full.find("###");
    return marker == std::string_view::npos ? "###Menu/" + std::string(full) : std::string(full.substr(marker));
}
inline std::string Label(const char* original)
{
    if (!original) return {};
    const std::string_view full(original);
    const auto marker = full.find("##");
    const std::string visible(full.substr(0, marker));
    const char* translated = Translate(visible.c_str());
    // The ### bytes participate in ImGui's hash. Use the same suffix in BOTH
    // languages and wrapped layouts; never compare it to the raw English ID.
    return std::string(translated) + IdSuffix(original);
}
}
