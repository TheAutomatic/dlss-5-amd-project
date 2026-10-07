#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/PluginPath.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "expected fixture output directory");
        const auto root = fs::absolute(argv[1]).lexically_normal();
        const auto exe = root / "game" / "bin";
        const auto main = exe / "CustomDeps";
        const auto cwd = root / "launcher";
        for (const auto& dir : {exe, main, cwd, exe / "mods", cwd / "mods", root / "shared"})
            fs::create_directories(dir);
        fs::current_path(cwd);
        Require(PluginPath::Resolve("mods", exe, main) == exe / "mods", "relative plugins used launcher CWD");
        Require(PluginPath::Resolve("mods/../mods", exe, main) == exe / "mods", "relative path normalization");
        Require(PluginPath::Resolve(root / "shared", exe, main) == root / "shared", "absolute preference changed");
        Require(PluginPath::Resolve({}, exe, main) == main / "plugins", "auto must follow resolved OptiDllPath");
        Require(PluginPath::Resolve("missing", exe, main) == main / "plugins", "invalid path must keep host fallback");
        std::ofstream(exe / "file-path") << "not a directory";
        Require(PluginPath::Resolve("file-path", exe, main) == main / "plugins", "file accepted as plugin directory");
        fs::create_directories(exe / "created-by-setup");
        Require(PluginPath::Resolve("created-by-setup", exe, main) == exe / "created-by-setup", "new Setup directory not selected");
        std::cout << "plugin_path: PASS (relative, absolute, auto, invalid, newly installed; foreign CWD)\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
