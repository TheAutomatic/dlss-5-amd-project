#pragma once

#include <Windows.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AmdPreSr::HipRuntimeLoad
{
using SetDeviceFn = int (*)(int);
struct LoadedModule
{
    HMODULE module = nullptr;
    DWORD error = ERROR_SUCCESS;
};
struct Runtime
{
    SetDeviceFn setDevice = nullptr;
    int device = -1;
};

inline std::string Utf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (!size)
        return "<path conversion failed>";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// No process-wide DLL directory changes. Keep the selected HIP loaded for the
// process lifetime, including API/device-validation failures: A's imports must
// not subsequently bind a different runtime from the one B inspected.
struct WindowsApi
{
    LoadedModule Load(const wchar_t* name, DWORD flags) const
    {
        HMODULE module = LoadLibraryExW(name, nullptr, flags);
        const DWORD error = module ? ERROR_SUCCESS : GetLastError();
        return { module, error };
    }
    std::wstring Environment(const wchar_t* name) const
    {
        DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        while (size)
        {
            std::wstring value(size, L'\0');
            const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
            if (!written)
                return {};
            if (written < size)
            {
                value.resize(written);
                return value;
            }
            size = written; // includes terminator when environment grew
        }
        return {};
    }
    std::wstring ModulePath(HMODULE module) const
    {
        for (DWORD size = 512; size <= 32768; size *= 2)
        {
            std::wstring path(size, L'\0');
            const DWORD written = GetModuleFileNameW(module, path.data(), size);
            if (!written)
                return L"<module path unavailable>";
            if (written < size)
            {
                path.resize(written);
                return path;
            }
        }
        return L"<module path too long>";
    }
    FARPROC Procedure(HMODULE module, const char* name) const { return GetProcAddress(module, name); }
};

inline std::wstring Directory(std::wstring_view value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos)
        return {};
    value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    if (value.empty() || value.find(L'"') != std::wstring_view::npos)
        return {};
    std::filesystem::path path(value);
    if (!path.is_absolute()) // excludes drive-relative and rooted-without-drive paths
        return {};
    return path.lexically_normal().make_preferred().native();
}

inline std::vector<std::wstring> PathDirectories(std::wstring_view value)
{
    std::vector<std::wstring> result;
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i)
    {
        if (i < value.size() && value[i] == L'"')
            quoted = !quoted;
        if (i == value.size() || (value[i] == L';' && !quoted))
        {
            auto directory = Directory(value.substr(start, i - start));
            if (!directory.empty())
                result.push_back(std::move(directory));
            start = i + 1;
        }
    }
    return result;
}

template <typename Api, typename Log> HMODULE SelectModule(Api& api, Log&& log)
{
    DWORD lastError = ERROR_MOD_NOT_FOUND;
    auto attempt = [&](const wchar_t* candidate, const char* source, DWORD flags) {
        // Load captures GetLastError before path conversion, logging or another Win32 call.
        const auto loaded = api.Load(candidate, flags);
        if (loaded.module)
            log(std::string("HIP runtime: ") + Utf8(api.ModulePath(loaded.module)) + " source=" + source);
        else
        {
            lastError = loaded.error;
            log(std::string("HIP load failed: source=") + source + " candidate='" + Utf8(candidate) +
                "' win32Error=" + std::to_string(loaded.error));
        }
        return loaded.module;
    };

    if (auto module = attempt(L"amdhip64_7.dll", "default", LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        return module;

    std::vector<std::wstring> tried;
    auto explicitPath = [&](const std::filesystem::path& directory, const char* source) -> HMODULE {
        auto candidate = (directory / L"amdhip64_7.dll").lexically_normal().make_preferred().native();
        for (const auto& previous : tried)
            if (CompareStringOrdinal(previous.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL)
                return nullptr;
        tried.push_back(candidate);
        return attempt(candidate.c_str(), source, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    };
    const auto hipDirectory = Directory(api.Environment(L"HIP_PATH"));
    if (!hipDirectory.empty())
        if (auto module = explicitPath(std::filesystem::path(hipDirectory) / L"bin", "HIP_PATH"))
            return module;

    for (const auto& directory : PathDirectories(api.Environment(L"PATH")))
        if (auto module = explicitPath(directory, "PATH"))
            return module;

    throw std::runtime_error("Cannot load amdhip64_7.dll; Windows error=" + std::to_string(lastError) +
                             ". Install the compatible AMD HIP 7 runtime; HIP 6 alone is insufficient.");
}

template <typename Api, typename Log> Runtime Initialize(Api& api, const LUID& luid, Log&& log)
{
    const auto module = SelectModule(api, log);
    const auto count = reinterpret_cast<int (*)(int*)>(api.Procedure(module, "hipGetDeviceCount"));
    const auto props = reinterpret_cast<int (*)(void*, int)>(api.Procedure(module, "hipGetDevicePropertiesR0600"));
    const auto setDevice = reinterpret_cast<SetDeviceFn>(api.Procedure(module, "hipSetDevice"));
    if (!count || !props || !setDevice)
        throw std::runtime_error("HIP R0600 API unavailable");
    int n = 0;
    const int countResult = count(&n);
    if (countResult != 0 || n <= 0)
        throw std::runtime_error("HIP device enumeration failed: code=" + std::to_string(countResult) +
                                 " devices=" + std::to_string(n));
    int selected = -1;
    for (int i = 0; i < n; ++i)
    {
        // R0600 prefix: name[256], uuid[16], luid[8]. Oversized aligned storage.
        alignas(16) std::array<unsigned char, 8192> properties {};
        const int propResult = props(properties.data(), i);
        const char* name = reinterpret_cast<char*>(properties.data());
        const std::string adapterName(name, strnlen_s(name, 256));
        log("HIP candidate " + std::to_string(i) + " code=" + std::to_string(propResult) + " name=" + adapterName);
        if (propResult == 0 && std::memcmp(properties.data() + 272, &luid, sizeof(luid)) == 0)
        {
            selected = i;
            log("HIP adapter: " + adapterName);
            break;
        }
    }
    if (selected < 0)
        throw std::runtime_error("No HIP adapter matches D3D12 LUID");
    const int setResult = setDevice(selected);
    if (setResult != 0)
        throw std::runtime_error("HIP hipSetDevice failed: code=" + std::to_string(setResult) +
                                 " device=" + std::to_string(selected));
    return { setDevice, selected };
}
} // namespace AmdPreSr::HipRuntimeLoad
