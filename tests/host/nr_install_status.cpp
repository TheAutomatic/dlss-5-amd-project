#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/InstallStatus.h"
#include <cassert>
#include <iostream>
using namespace DlssNr::Backend;
namespace fs = std::filesystem;
static void Write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(bytes.data(), bytes.size());
}
int main() {
    const auto dir = fs::temp_directory_path() / (L"nr-install-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", nullptr);
    SetEnvironmentVariableW(L"LMXXF_MODULES_DIR", nullptr);
    Write(dir / "LmxxfNrRuntime.dll", "runtime");
    fs::create_directories(dir / "native-game-tiled-assets");
    assert(FindLmxxfWeights(dir).empty());
    Write(dir / L"中文 weights/block0-ffn.f16", "weights");
    Write(dir / "lmxxf-weights-dir.txt", "\xef\xbb\xbf" "中文 weights\r\n");
    assert(FindLmxxfWeights(dir) == dir / L"中文 weights");
    assert(ProbeInstallIssue(Kind::Lmxxf, dir).find("module package missing") != std::string::npos);
    Write(dir / "external/HIP/SHA256SUMS", "manifest");
    SetEnvironmentVariableW(L"LMXXF_MODULES_DIR", (dir / "external").c_str());
    assert(FindLmxxfModuleRoot(dir) == dir / "external");
    assert(ProbeInstallIssue(Kind::Lmxxf, dir).empty());
    Write(dir / "lmxxf-weights-dir.txt", "\xff\xfeinvalid");
    assert(FindLmxxfWeights(dir).empty());
    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", (dir / L"中文 weights").c_str());
    assert(FindLmxxfWeights(dir) == dir / L"中文 weights");
    Write(dir / "dlssnr_amd_pass1.dll", "runtime");
    Write(dir / "nvngx_dlssnr.dll", "source only");
    assert(!ProbeInstallIssue(Kind::Daniel, dir).empty());
    Write(dir / "dlssnr_on_amd_weights.bin", "weights");
    assert(ProbeInstallIssue(Kind::Daniel, dir).empty());
    Write(dir / "MochizukiNrRuntime.dll", "runtime");
    Write(dir / "dlssnr-amd/dlssnr.bin", "corrupt");
    assert(ProbeInstallIssue(Kind::Mochizuki, dir).find("header invalid") != std::string::npos);
    Write(dir / "dlssnr-amd/dlssnr.bin", std::string("NRMODEL1\x57\x02\0\0payload", 19));
    assert(HasMochizukiModelHeader(dir / "dlssnr-amd/dlssnr.bin"));
    assert(ProbeInstallIssue(Kind::Mochizuki, dir).find("shaders missing") != std::string::npos);
    Write(dir / "dlssnr-amd/shaders/test.spv", "shader");
    assert(ProbeInstallIssue(Kind::Mochizuki, dir).empty());
    fs::remove_all(dir);
    std::cout << "PASS installation diagnostics and Unicode/shared path resolution\n";
}
