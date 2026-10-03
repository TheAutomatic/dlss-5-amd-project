#pragma once
#include "Kind.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace DlssNr::Backend {
inline bool NonemptyFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0 && !ec;
}
inline bool HasLmxxfWeightMarker(const std::filesystem::path& path) {
    return NonemptyFile(path / L"block0-ffn.f16") || NonemptyFile(path / L"block0-ffn.f32");
}
// Match the host's local-folder, hint and external-environment search order.
// A marker locates weights; it does not certify the full model's integrity.
inline std::filesystem::path FindLmxxfWeights(const std::filesystem::path& dir) {
    for (const auto* name : {L"native-game-tiled-assets", L"lmxxf-weights"})
        if (HasLmxxfWeightMarker(dir / name)) return dir / name;
    std::ifstream hint(dir / L"lmxxf-weights-dir.txt", std::ios::binary);
    std::string line;
    if (std::getline(hint, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        // Reject malformed UTF-8 before asking filesystem to construct a path.
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (count > 0) {
            std::wstring wide(count, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), wide.data(), count);
            auto path = std::filesystem::path(wide);
            if (!path.empty() && path.is_relative()) path = dir / path;
            if (!path.empty() && HasLmxxfWeightMarker(path)) return path;
        }
    }
    wchar_t env[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", env, 32768);
    if (n > 0 && n < 32768 && HasLmxxfWeightMarker(env)) return env;
    return {};
}
inline std::filesystem::path FindLmxxfModuleRoot(const std::filesystem::path& dir) {
    wchar_t env[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LMXXF_MODULES_DIR", env, 32768);
    if (n > 0 && n < 32768) return env;
    std::error_code ec;
    if (std::filesystem::exists(dir / L"lmxxf-modules", ec)) return dir / L"lmxxf-modules";
    return dir;
}
inline bool HasMochizukiModelHeader(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8]{};
    uint32_t count{};
    in.read(magic, sizeof magic);
    in.read(reinterpret_cast<char*>(&count), sizeof count);
    in.seekg(0, std::ios::end);
    return in && std::memcmp(magic, "NRMODEL1", 8) == 0 && count == 599 && in.tellg() > 16;
}
inline std::string ProbeInstallIssue(Kind kind, const std::filesystem::path& dir) {
    const auto* runtime = kind == Kind::Lmxxf ? L"LmxxfNrRuntime.dll" :
        kind == Kind::Mochizuki ? L"MochizukiNrRuntime.dll" : L"dlssnr_amd_pass1.dll";
    if (!NonemptyFile(dir / runtime)) return std::string(Name(kind)) + ": runtime missing or empty. Run Setup from the complete package.";
    if (kind == Kind::Lmxxf) {
        if (FindLmxxfWeights(dir).empty())
            return "lmxxf: weights missing. Install native-game-tiled-assets beside the game's OptiScaler DLL, then restart. Select the folder containing block0-ffn.f16/.f32, not its HIP subfolder.";
        const auto root = FindLmxxfModuleRoot(dir);
        bool hasManifest = false;
        for (const auto* sub : {L"", L"HIP", L"modules", L"native-game-tiled-assets/HIP"})
            hasManifest |= NonemptyFile(root / sub / L"SHA256SUMS");
        if (!hasManifest)
            return "lmxxf: module package missing. Reinstall the complete OptiScaler package with Setup.";
    } else if (kind == Kind::Mochizuki) {
        if (!NonemptyFile(dir / L"dlssnr-amd/dlssnr.bin"))
            return "mochizuki: model missing. Put nvngx_dlssnr.dll 310.8.0 beside Setup.bat and run Setup to extract the model, then restart the game.";
        if (!HasMochizukiModelHeader(dir / L"dlssnr-amd/dlssnr.bin"))
            return "mochizuki: model header invalid. Move dlssnr-amd/dlssnr.bin aside and rerun Setup to extract it again; the runtime checks full model integrity.";
        std::error_code ec;
        const auto shaders = dir / L"dlssnr-amd/shaders";
        if (!std::filesystem::is_directory(shaders, ec) || std::filesystem::is_empty(shaders, ec))
            return "mochizuki: shaders missing. Reinstall the complete OptiScaler package with Setup.";
    } else if (!NonemptyFile(dir / L"dlssnr_on_amd_weights.bin")) {
        return "daniel: model files missing. Run Setup with the Daniel installer and your nvngx_dlssnr.dll, then restart the game.";
    }
    return {};
}
} // namespace DlssNr::Backend
