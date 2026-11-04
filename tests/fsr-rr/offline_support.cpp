// Standalone converter host support. In this executable the production converter is
// linked into the EXE, so its real module path is GetModuleFileNameW(nullptr), not an
// injected OptiScaler DLL. Keep capture paths anchored to that real module location.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace Util
{
std::filesystem::path DllPath()
{
    std::vector<wchar_t> name(512);
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, name.data(), DWORD(name.size()));
        if (length == 0)
            throw std::runtime_error("GetModuleFileNameW failed for offline converter host: " +
                                     std::to_string(GetLastError()));
        if (length < name.size())
            return std::filesystem::path(std::wstring(name.data(), length));
        if (name.size() >= 32768)
            throw std::runtime_error("offline converter host module path exceeds Windows path limit");
        name.resize(std::min<size_t>(name.size() * 2, 32768));
    }
}
} // namespace Util
