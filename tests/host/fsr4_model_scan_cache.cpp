// Source contract: the FSR4 menu queries the selectable models every frame, so the
// unhooked-module discovery (a multi-MB RTTI scan per provider) must be cached per
// module generation instead of rerunning rtti::FindVtable per frame. The real scan
// needs a live driver module, so this asserts the caching discipline on the source.
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

int main()
{
    std::ifstream file("OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/fsr4/FSR4Models.cpp");
    assert(file && "FSR4Models.cpp readable from repo root");
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    // The expensive scan runs only in the cached helper and in Hook (explicit selection)
    size_t scans = 0, pos = 0;
    while ((pos = source.find("rtti::FindVtable", pos)) != std::string::npos)
    {
        scans++;
        pos += 4;
    }
    assert(scans == 2 && "rtti::FindVtable is called only from the cached scan helper and Hook");

    size_t helper = source.find("const std::vector<FSR4Model>& ScannedModels(HMODULE module)");
    size_t selectable = source.find("FSR4Models::SelectableModels(HMODULE module)");
    size_t hasModel = source.find("FSR4Models::ModuleHasModel(HMODULE module, FSR4Model model)");
    assert(helper != std::string::npos && "cached scan helper exists");
    assert(selectable != std::string::npos && hasModel != std::string::npos);

    size_t selectableBodyEnd = source.find("FSR4Models::ActiveModel", selectable);
    std::string selectableBody = source.substr(selectable, selectableBodyEnd - selectable);
    assert(selectableBody.find("ScannedModels(module)") != std::string::npos &&
           "SelectableModels uses the cached scan on the unhooked path");
    assert(selectableBody.find("rtti::FindVtable") == std::string::npos &&
           "SelectableModels does not scan per frame");

    std::string hasModelBody = source.substr(hasModel, selectable - hasModel);
    assert(hasModelBody.find("ScannedModels(module)") != std::string::npos &&
           "ModuleHasModel uses the cached scan on the unhooked path");
    assert(hasModelBody.find("rtti::FindVtable") == std::string::npos &&
           "ModuleHasModel does not scan on the per-frame path");

    // The cache is invalidated by a changed module handle or a reloaded module file
    size_t helperBody = source.find("scanCache.module != module", helper);
    assert(helperBody != std::string::npos && "cache keys on the module handle");
    assert(source.find("dwLowDateTime") != std::string::npos &&
           source.find("dwHighDateTime") != std::string::npos &&
           "cache keys on the module file write time (reload detection)");

    std::cout << "fsr4_model_scan_cache: PASS\n";
    return 0;
}
