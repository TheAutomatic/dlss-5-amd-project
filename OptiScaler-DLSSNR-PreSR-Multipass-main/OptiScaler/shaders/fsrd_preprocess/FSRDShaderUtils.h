#pragma once
#include "State.h"
#include "../Shader_Dx12Utils.h"
#include <d3d12.h>
#include <numbers>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace FSRD
{
    constexpr UINT kMaxBarriers = 16;

    struct MipChainDesc
    {
        UINT MipCount;
        UINT MipSlice;
    };

    static inline void ThrowIfFailed(HRESULT hr, const char* msg)
    {
        if (FAILED(hr))
            throw std::runtime_error(msg);
    }
    
    /**
     * @brief Returns a non-owning view to a POD type as a byte array
     */
    template<typename T>
    std::span<const byte> GetAsByteSpan(const T& data) 
    {
        return { (const byte*) &data, sizeof(data) };
    }

    /**
     * @brief Rounds size to nearest multiple of 256 bytes for constant buffer alignment.
     */
    static inline UINT AlignTo256(UINT size) { return (size + 255) & ~255; }

    /**
     * @brief Maps typeless and depth formats into formats compatible with SRVs and UAVs
     */
    static inline DXGI_FORMAT GetViewFormat(DXGI_FORMAT defaultFormat)
    {
        switch (defaultFormat)
        {
        // Depth/Stencil Mappings
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

        // 128-bit Typeless
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;

        // 96-bit Typeless
        case DXGI_FORMAT_R32G32B32_TYPELESS:
            return DXGI_FORMAT_R32G32B32_FLOAT;

        // 64-bit Typeless
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

        // 32-bit Typeless
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM;

        // 16-bit Typeless
        case DXGI_FORMAT_R8G8_TYPELESS:
            return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;

        // 8-bit Typeless
        case DXGI_FORMAT_R8_TYPELESS:
            return DXGI_FORMAT_R8_UNORM;

        default:
            // Any format not explicitly caught here is likely already a fully typed format (or unsupported)
            return defaultFormat;
        }
    }

    /**
     * @brief Creates a new 2D texture with the given dimensions and format with support for unordered access.
     */
    static inline ComPtr<ID3D12Resource> CreateTexture2D(ID3D12Device* pDev, UINT width, UINT height,
                                                         DXGI_FORMAT format, LPCWSTR name,
                                                         D3D12_RESOURCE_STATES initialState, UINT mipLevels = 1)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = mipLevels;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
        ComPtr<ID3D12Resource> resource;

        ThrowIfFailed(pDev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource)),
                      "Failed to create internal texture");

        resource->SetName(name);
        return resource;
    }

    /**
     * @brief Adds a barrier for the given resource to the given array and increments the count if successful.
     */
    static inline void EnqueueBarrier(ID3D12Resource* pRes, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                                  std::span<D3D12_RESOURCE_BARRIER> barriers, int& bCount,
                                  UINT subresource = 0)
    {
        if (!pRes)
            return;

        // Callers hand this a fixed-size stack array (kMaxBarriers) as a span. An
        // output set that outgrows it would smash the stack rather than trip a
        // validation error, so refuse the write and make the cause visible.
        if (static_cast<size_t>(bCount) >= barriers.size())
        {
            LOG_ERROR("FSRD barrier array is full ({} entries); dropping a transition. "
                      "Raise FSRD::kMaxBarriers to match the largest resource set.",
                      barriers.size());
            return;
        }

        barriers[bCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[bCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[bCount].Transition.pResource = pRes;
        barriers[bCount].Transition.StateBefore = before;
        barriers[bCount].Transition.StateAfter = after;
        barriers[bCount].Transition.Subresource = subresource;
        bCount++;
    }

    /**
     * @brief Inserts barriers for the given resources on the command list that all share the samee before and after states.
     */
    static inline void AddBarriers(ID3D12GraphicsCommandList* cmdList, std::span<ID3D12Resource* const> resources,
                                   std::span<const UINT> mipLevels, D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barriers[kMaxBarriers] = {};
        int bCount = 0;

        for (int i = 0; i < resources.size(); i++)
        {
            if (!mipLevels.empty())
                EnqueueBarrier(resources[i], before, after, barriers, bCount, mipLevels[i]);
            else
                EnqueueBarrier(resources[i], before, after, barriers, bCount);
        }

        if (bCount > 0)
            cmdList->ResourceBarrier(bCount, barriers);
    }

    /**
     * @brief Inserts barriers for the given resources on the command list that all share the samee before and after states.
     */
    static inline void AddBarriers(ID3D12GraphicsCommandList* cmdList, std::span<ID3D12Resource* const> resources,
                                   D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        AddBarriers(cmdList, resources, {}, before, after);
    }

    /**
     * @brief Insert a barrier for the given resource on the command list.
     */
    static inline void AddBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* pRes,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) 
    {
        AddBarriers(cmdList, std::to_array({ pRes }), {}, before, after);
    }

    /**
     * @brief Insert a barrier for the given subresource on the command list.
     */
    static inline void AddBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* pRes, UINT mipLevel,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        AddBarriers(cmdList, std::to_array({ pRes }), std::to_array({ mipLevel }), before, after);
    }

    /**
     * @brief Creates a shader resource view for the given texture
     * @param mipSlice Mip level targeted by the view
     * @param mipCount Maximum number of mip levels
     */
    static inline void CreateSRV(ID3D12Device* pDev, D3D12_CPU_DESCRIPTOR_HANDLE destHandle, ID3D12Resource* pRes,
                                 UINT mipSlice = 0, UINT mipCount = 1)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = mipSlice;
        srvDesc.Texture2D.MipLevels = mipCount;

        if (pRes != nullptr)
        {
            srvDesc.Format = GetViewFormat(pRes->GetDesc().Format);
            pDev->CreateShaderResourceView(pRes, &srvDesc, destHandle);
        }
        else
        {
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            pDev->CreateShaderResourceView(nullptr, &srvDesc, destHandle);
        }
    }

    /**
     * @brief Creates a group of shader resource views for the given textures
     * @param inputs Resources targeted by the views
     * @param mips Optional mip level
     */
    static inline void CreateSRVs(ID3D12Device* pDev, FrameDescriptorHeap& heap,
                                  std::span<ID3D12Resource* const> inputs, std::span<const MipChainDesc> mips = {})
    {
        if (!mips.empty())
        {
            for (UINT i = 0; i < (UINT) inputs.size(); i++)
                CreateSRV(pDev, heap.GetSrvCPU(i), inputs[i], mips[i].MipSlice, mips[i].MipCount);
        }
        else
        {
            for (UINT i = 0; i < (UINT) inputs.size(); i++)
                CreateSRV(pDev, heap.GetSrvCPU(i), inputs[i]);
        }
    }

    /**
     * @brief Creates an unordered access view for a texture. Targets a given mip slice if specified.
     */
    static inline void CreateUAV(ID3D12Device* pDev, D3D12_CPU_DESCRIPTOR_HANDLE destHandle, ID3D12Resource* pRes,
                                 DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN, UINT mipSlice = 0)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        if (fmt == DXGI_FORMAT_UNKNOWN)
            fmt = (pRes != nullptr) ? GetViewFormat(pRes->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = fmt;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mipSlice;
        pDev->CreateUnorderedAccessView(pRes, nullptr, &uavDesc, destHandle);
    }

    /**
     * @brief Creates an unordered access view for a texture. Targets a given mip slice if specified.
     */
    static inline void CreateUAV(ID3D12Device* pDev, D3D12_CPU_DESCRIPTOR_HANDLE destHandle, ID3D12Resource* pRes, UINT mipSlice)
    {
        CreateUAV(pDev, destHandle, pRes, DXGI_FORMAT_UNKNOWN, mipSlice);
    }

    /**
     * @brief Creates a group of unordered access views for a set of textures. Targets a given mip slice if specified.
     */
    static inline void CreateUAVs(ID3D12Device* pDev, FrameDescriptorHeap& heap, std::span<ID3D12Resource*> resources,
                                  std::span<const UINT> mipSlices = {}, std::span<const DXGI_FORMAT> formats = {})
    {
        for (UINT i = 0; i < (UINT) resources.size(); i++)
        {
            DXGI_FORMAT fmt = !formats.empty() ? formats[i] : DXGI_FORMAT_UNKNOWN;

            if (!mipSlices.empty())
                CreateUAV(pDev, heap.GetUavCPU(i), resources[i], fmt, mipSlices[i]);
            else
                CreateUAV(pDev, heap.GetUavCPU(i), resources[i], fmt);
        }
    }

    /**
     * @brief Calculates normalized 1D Gaussian weights for a separable kernel.
     * Kernel size and radius are determined by the size of the span. Dst size must be odd.
     */
    static inline void GetGaussWeights(std::span<float> dst, float sigma = 1.0f)
    {
        const size_t radius = dst.size() / 2ull;
        const float denom = 2.0f * (sigma * sigma);
        float sum = 0.0f;

        for (size_t i = 0; i <= radius; i++)
        {
            // weight = e^(-x^2 / 2(sigma^2))
            const float x = float(i);
            const float val = std::exp(-(x * x) / denom);

            dst[radius + i] = val;
            dst[radius - i] = val;
            sum += 2.0f * val;
        }

        // Normalize
        sum -= dst[radius]; // Double counted center
        const float rcpSum = 1.0f / sum;

        for (UINT i = 0; i < dst.size(); i++)
            dst[i] *= rcpSum;
    }
}