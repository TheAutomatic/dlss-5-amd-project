#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace RRTraceAdditiveIO
{
inline void CopyPreSrRoi(ID3D12GraphicsCommandList* cmd, ID3D12Resource* source,
    ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
    UINT x, UINT y, UINT width, UINT height)
{
    if (!cmd || !source || !readback) throw std::runtime_error("Null paired copy resource");
    const auto desc=source->GetDesc();
    if (!width || !height || desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
        uint64_t(x)+width>desc.Width || uint64_t(y)+height>desc.Height ||
        footprint.Footprint.Format!=desc.Format || footprint.Footprint.Width!=width ||
        footprint.Footprint.Height!=height)
        throw std::runtime_error("Invalid paired output extent/format");
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_SOURCE};
    cmd->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src {},dst {};
    src.pResource=source; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
    D3D12_BOX box {x,y,0,x+width,y+height,1};
    cmd->CopyTextureRegion(&dst,0,0,0,&src,&box);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1,&barrier);
}

template<class T> inline std::string FloatArray(const T& value)
{
    static_assert(sizeof(T)%sizeof(float)==0);
    std::array<float,sizeof(T)/sizeof(float)> values;
    memcpy(values.data(),&value,sizeof(T));
    std::ostringstream out;
    out.imbue(std::locale::classic()); out<<std::setprecision(9)<<'[';
    for (size_t i=0;i<values.size();++i)
    {
        if (!std::isfinite(values[i])) throw std::runtime_error("Nonfinite capture metadata");
        if (i) out<<',';
        out<<values[i];
    }
    out<<']'; return out.str();
}
inline std::string Quote(std::string_view text)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    out << '"';
    return out.str();
}
inline std::string Sha256(std::span<const uint8_t> bytes)
{
    struct Handles
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } h;
    if (BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("RRTrace SHA256 provider failed");
    ULONG objectBytes = 0, written = 0;
    if (BCryptGetProperty(h.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                          sizeof(objectBytes), &written, 0) < 0)
        throw std::runtime_error("RRTrace SHA256 object length failed");
    std::vector<uint8_t> object(objectBytes);
    // Destroy hash before its caller-provided object storage goes out of scope.
    struct HashGuard
    {
        BCRYPT_HASH_HANDLE* hash;
        ~HashGuard() { if (*hash) { BCryptDestroyHash(*hash); *hash = nullptr; } }
    } hashGuard { &h.hash };
    if (BCryptCreateHash(h.algorithm, &h.hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
        throw std::runtime_error("RRTrace SHA256 create failed");
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const ULONG count = static_cast<ULONG>(std::min<size_t>(bytes.size()-offset, 1u<<28));
        if (BCryptHashData(h.hash, const_cast<PUCHAR>(bytes.data()+offset), count, 0) < 0)
            throw std::runtime_error("RRTrace SHA256 update failed");
        offset += count;
    }
    std::array<uint8_t, 32> digest {};
    if (BCryptFinishHash(h.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("RRTrace SHA256 finish failed");
    std::ostringstream out;
    for (uint8_t c : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
template<class T> inline std::string HashObject(const T& object)
{
    return Sha256({reinterpret_cast<const uint8_t*>(&object), sizeof(object)});
}
inline void WriteFile(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
}
inline void WriteText(const std::filesystem::path& path, const std::string& text)
{
    WriteFile(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}
}
