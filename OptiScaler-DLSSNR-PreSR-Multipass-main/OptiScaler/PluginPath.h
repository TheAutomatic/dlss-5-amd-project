#pragma once
#include <filesystem>

namespace PluginPath
{
// Keep relative plugin paths independent of the launcher's working directory.
// The caller publishes a volatile value without rewriting the saved INI setting.
inline std::filesystem::path Resolve(std::filesystem::path configured,
                                     const std::filesystem::path& exeDirectory,
                                     const std::filesystem::path& mainDllDirectory)
{
    if (!configured.empty())
    {
        if (configured.is_relative())
            configured = exeDirectory / configured;
        if (std::filesystem::is_directory(configured))
            return std::filesystem::absolute(configured).lexically_normal();
    }
    return (mainDllDirectory / L"plugins").lexically_normal();
}
}
