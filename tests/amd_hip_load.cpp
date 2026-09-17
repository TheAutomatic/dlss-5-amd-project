// Exercises the production HIP selection/validation flow without a GPU or AMD runtime.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/HipRuntimeLoad.h"
#include <cstdio>

using namespace AmdPreSr::HipRuntimeLoad;
static void Expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct FakeApi
{
    struct Attempt { std::wstring path; DWORD flags; };
    std::vector<Attempt> attempts;
    std::vector<std::wstring> environmentReads;
    std::wstring hipPath, path;
    size_t successAt = 0;
    const char* missing = "";
    inline static int enumResult = 0, devices = 2, propResult = 0, setResult = 0, setCalls = 0;
    inline static int selected = -1;
    inline static LUID wanted { 12, 34 };
    LoadedModule Load(const wchar_t* candidate, DWORD flags)
    {
        attempts.push_back({ candidate, flags });
        return attempts.size() == successAt ? LoadedModule { reinterpret_cast<HMODULE>(1), ERROR_SUCCESS }
                                            : LoadedModule { nullptr, static_cast<DWORD>(1000 + attempts.size()) };
    }
    std::wstring Environment(const wchar_t* name)
    {
        environmentReads.emplace_back(name);
        SetLastError(9999); // must not overwrite a preceding load error
        return std::wcscmp(name, L"HIP_PATH") == 0 ? hipPath : path;
    }
    std::wstring ModulePath(HMODULE) { return L"C:\\实际模块\\amdhip64_7.dll"; }
    static int Count(int* count) { *count = devices; return enumResult; }
    static int Properties(void* output, int ordinal)
    {
        const char name[] = "test adapter";
        std::memcpy(output, name, sizeof(name));
        const LUID luid = ordinal == 1 ? wanted : LUID { 0, 0 };
        std::memcpy(static_cast<char*>(output) + 272, &luid, sizeof(luid));
        return propResult;
    }
    static int Set(int ordinal) { selected = ordinal; ++setCalls; return setResult; }
    FARPROC Procedure(HMODULE, const char* name)
    {
        if (std::strcmp(name, missing) == 0)
            return nullptr;
        if (std::strcmp(name, "hipGetDeviceCount") == 0)
            return reinterpret_cast<FARPROC>(&Count);
        if (std::strcmp(name, "hipGetDevicePropertiesR0600") == 0)
            return reinterpret_cast<FARPROC>(&Properties);
        if (std::strcmp(name, "hipSetDevice") == 0)
            return reinterpret_cast<FARPROC>(&Set);
        return nullptr;
    }
    static void Reset()
    {
        enumResult = propResult = setResult = setCalls = 0;
        devices = 2;
        selected = -1;
    }
};

static void UnitTests()
{
    std::vector<std::string> messages;
    auto log = [&](const std::string& message) { messages.push_back(message); SetLastError(8888); };
    FakeApi api;
    api.successAt = 1;
    api.hipPath = L"C:\\other";
    auto runtime = Initialize(api, FakeApi::wanted, log);
    Expect(api.attempts.size() == 1 && api.environmentReads.empty(), "default success must not inspect environment");
    Expect(api.attempts[0].flags == LOAD_LIBRARY_SEARCH_DEFAULT_DIRS &&
           api.attempts[0].path == L"amdhip64_7.dll", "retain default load contract");
    Expect(runtime.device == 1 && runtime.setDevice == &FakeApi::Set && FakeApi::selected == 1,
           "select the matching adapter, not device zero");
    Expect(messages.front() == "HIP runtime: " + Utf8(L"C:\\实际模块\\amdhip64_7.dll") + " source=default",
           "log UTF-8 actual module path, not candidate spelling");

    api = {};
    api.hipPath = L"  \"C:\\HIP SDK\\..\\HIP SDK\"  ";
    api.path = L";relative;.;;C:driveRelative;\\rooted;\"C:/HIP SDK/bin/\";c:\\hip sdk\\BIN;"
               L" \"C:\\路径 空格\" ;C:\\another;\"C:\\with;semicolon\";bad\"quote;";
    api.successAt = 3;
    messages.clear();
    SelectModule(api, log);
    Expect(api.attempts.size() == 3, "skip duplicate and relative entries");
    Expect(api.attempts[1].path == L"C:\\HIP SDK\\bin\\amdhip64_7.dll" &&
           api.attempts[2].path == L"C:\\路径 空格\\amdhip64_7.dll", "HIP_PATH then PATH, unicode and quotes");
    Expect(api.attempts[1].flags == (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) &&
           api.attempts[2].flags == api.attempts[1].flags, "absolute candidates include same-directory dependencies");
    Expect(messages[0].find("win32Error=1001") != std::string::npos &&
           messages[1].find("win32Error=1002") != std::string::npos, "preserve original per-attempt errors");

    api = {};
    api.hipPath = L"relative SDK";
    api.path = L";;. ;C:relative;\\rooted;\"C:\\with;semicolon\";\\\\server\\share\\hip;D:/final";
    messages.clear();
    bool failed = false;
    try { SelectModule(api, log); }
    catch (const std::runtime_error& error)
    {
        failed = std::string(error.what()).find("Windows error=1004") != std::string::npos;
    }
    Expect(failed && api.attempts.size() == 4, "invalid HIP_PATH falls through; final error survives environment/log calls");
    Expect(api.attempts[1].path == L"C:\\with;semicolon\\amdhip64_7.dll" &&
           api.attempts[2].path == L"\\\\server\\share\\hip\\amdhip64_7.dll", "quoted semicolons and UNC paths");
    Expect(Directory(L"\"C:\\unfinished").empty() && Directory(L"C:\\bad\"quote").empty(), "reject malformed quotes");

    // Every validation failure occurs AFTER selecting one module. It must not
    // enter the remaining environment candidates or publish partial state.
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        FakeApi::Reset();
        api = {};
        api.successAt = 2;
        api.hipPath = L"C:\\SDK";
        api.path = L"D:\\different runtime";
        if (scenario == 0) api.missing = "hipGetDevicePropertiesR0600";
        if (scenario == 1) FakeApi::enumResult = 9;
        if (scenario == 2) FakeApi::devices = 0;
        if (scenario == 3) FakeApi::propResult = 8;
        if (scenario == 4) FakeApi::devices = 1; // no matching LUID
        if (scenario == 5) FakeApi::setResult = 7;
        Runtime published;
        failed = false;
        try { published = Initialize(api, FakeApi::wanted, log); }
        catch (const std::runtime_error&) { failed = true; }
        Expect(failed && api.attempts.size() == 2 && api.environmentReads.size() == 1,
               "API/enumeration/LUID/set failure must not switch modules");
        Expect(!published.setDevice && published.device == -1, "no partially initialized state escapes");
        Expect(FakeApi::setCalls == (scenario == 5 ? 1 : 0), "set device only after API/LUID validation");
    }
    FakeApi::Reset();
    std::puts("PASS: HIP default/fallback ordering, paths, errors, API/LUID failures and atomic publication");
}

struct FixtureApi : WindowsApi
{
    std::wstring sdk;
    bool failDefault = true;
    unsigned environmentReads = 0, attempts = 0;
    LoadedModule Load(const wchar_t* path, DWORD flags)
    {
        ++attempts;
        if (failDefault && std::wcscmp(path, L"amdhip64_7.dll") == 0)
            return { nullptr, ERROR_MOD_NOT_FOUND }; // never load the machine's real HIP in this test
        return WindowsApi::Load(path, flags);
    }
    std::wstring Environment(const wchar_t* name)
    {
        ++environmentReads;
        return std::wcscmp(name, L"HIP_PATH") == 0 ? sdk : L"";
    }
};

static void FixtureTest(const wchar_t* mode, const std::filesystem::path& fixture)
{
    WindowsApi windows;
    auto log = [](const std::string& message) { std::puts(message.c_str()); };
    FixtureApi api;
    api.sdk = fixture.parent_path().parent_path().native();
    if (std::wcscmp(mode, L"--dependency") == 0)
    {
        const auto without = windows.Load(fixture.c_str(), LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Expect(!without.module && without.error == ERROR_MOD_NOT_FOUND,
               "isolated dependency must not be in the default search directories");
        const auto loaded = SelectModule(api, log);
        const auto value = reinterpret_cast<int (*)()>(GetProcAddress(loaded, "hipFixtureValue"));
        Expect(value && value() == 42 && api.attempts == 2, "HIP_PATH loader must resolve fixture sibling dependency");
        std::puts("PASS: real Windows loader resolves same-directory dependency through DLL_LOAD_DIR");
    }
    else
    {
        const auto preloaded = windows.Load(fixture.c_str(), LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        Expect(preloaded.module != nullptr, "preload controlled HIP-name fixture");
        api.failDefault = false;
        api.sdk = L"C:\\this candidate must never be tried";
        Expect(SelectModule(api, log) == preloaded.module && api.environmentReads == 0 && api.attempts == 1,
               "default load preserves already-loaded same-name module before environment candidates");
        std::puts("PASS: real Windows loaded-module precedence is preserved");
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        if (argc == 3 && (std::wcscmp(argv[1], L"--dependency") == 0 || std::wcscmp(argv[1], L"--loaded") == 0))
            FixtureTest(argv[1], std::filesystem::absolute(argv[2]));
        else if (argc == 1)
            UnitTests();
        else
            throw std::runtime_error("Usage: amd_hip_load [--dependency|--loaded synthetic-fixture.dll]");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
