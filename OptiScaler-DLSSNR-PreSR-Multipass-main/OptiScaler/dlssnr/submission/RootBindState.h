#pragma once
#include <d3d12.h>
#include <cstdint>
#include <cstring>

namespace DlssNr::Submission
{
// Compact root-parameter seed for continuation (plan D residual).
// Mirrors GraphicsSnapshot intent without pulling amd/GraphicsSnap.
struct RootBindState
{
    static constexpr UINT kMaxParams = 64;
    static constexpr UINT kMaxConstants = 64;

    enum class EntryType : UINT8
    {
        None = 0,
        Table,
        CBV,
        SRV,
        UAV,
        Constants,
    };

    struct Entry
    {
        EntryType type = EntryType::None;
        bool set = false;
        D3D12_GPU_DESCRIPTOR_HANDLE table {};
        D3D12_GPU_VIRTUAL_ADDRESS gpuVa = 0;
        UINT numConstants = 0;
        UINT constants[kMaxConstants] {};
        UINT64 knownMask = 0; // bit i = DWORD i observed
    };

    Entry entries[kMaxParams] {};

    void Reset()
    {
        for (UINT i = 0; i < kMaxParams; ++i)
            entries[i] = Entry {};
    }

    // New root signature invalidates prior root args.
    void OnSignatureChanged() { Reset(); }

    bool OnTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h)
    {
        if (index >= kMaxParams)
            return false;
        Entry &e = entries[index];
        e = Entry {};
        e.type = EntryType::Table;
        e.set = true;
        e.table = h;
        return true;
    }

    bool OnGpuVa(UINT index, EntryType type, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        if (index >= kMaxParams)
            return false;
        if (type != EntryType::CBV && type != EntryType::SRV && type != EntryType::UAV)
            return false;
        Entry &e = entries[index];
        e = Entry {};
        e.type = type;
        e.set = true;
        e.gpuVa = va;
        return true;
    }

    bool OnConstants(UINT index, UINT num, const void *src, UINT destOffset)
    {
        if (index >= kMaxParams || !src || num == 0)
            return false;
        if (destOffset >= kMaxConstants || num > kMaxConstants - destOffset)
            return false;
        Entry &e = entries[index];
        if (!e.set || e.type != EntryType::Constants)
        {
            e = Entry {};
            e.type = EntryType::Constants;
            e.set = true;
        }
        const UINT *words = static_cast<const UINT *>(src);
        for (UINT i = 0; i < num; ++i)
        {
            e.constants[destOffset + i] = words[i];
            e.knownMask |= (UINT64 { 1 } << (destOffset + i));
        }
        if (destOffset + num > e.numConstants)
            e.numConstants = destOffset + num;
        return true;
    }

    bool OnSingleConstant(UINT index, UINT value, UINT destOffset)
    {
        return OnConstants(index, 1, &value, destOffset);
    }

    UINT BoundCount() const
    {
        UINT n = 0;
        for (UINT i = 0; i < kMaxParams; ++i)
            if (entries[i].set)
                ++n;
        return n;
    }

    void ApplyGraphics(ID3D12GraphicsCommandList *list) const
    {
        if (!list)
            return;
        for (UINT i = 0; i < kMaxParams; ++i)
        {
            const Entry &e = entries[i];
            if (!e.set)
                continue;
            switch (e.type)
            {
            case EntryType::Table:
                list->SetGraphicsRootDescriptorTable(i, e.table);
                break;
            case EntryType::CBV:
                list->SetGraphicsRootConstantBufferView(i, e.gpuVa);
                break;
            case EntryType::SRV:
                list->SetGraphicsRootShaderResourceView(i, e.gpuVa);
                break;
            case EntryType::UAV:
                list->SetGraphicsRootUnorderedAccessView(i, e.gpuVa);
                break;
            case EntryType::Constants:
                for (UINT k = 0; k < e.numConstants && k < kMaxConstants;)
                {
                    if (((e.knownMask >> k) & 1ull) == 0)
                    {
                        ++k;
                        continue;
                    }
                    UINT start = k;
                    UINT count = 0;
                    while (k < e.numConstants && k < kMaxConstants && ((e.knownMask >> k) & 1ull))
                    {
                        ++count;
                        ++k;
                    }
                    list->SetGraphicsRoot32BitConstants(i, count, &e.constants[start], start);
                }
                break;
            default:
                break;
            }
        }
    }

    void ApplyCompute(ID3D12GraphicsCommandList *list) const
    {
        if (!list)
            return;
        for (UINT i = 0; i < kMaxParams; ++i)
        {
            const Entry &e = entries[i];
            if (!e.set)
                continue;
            switch (e.type)
            {
            case EntryType::Table:
                list->SetComputeRootDescriptorTable(i, e.table);
                break;
            case EntryType::CBV:
                list->SetComputeRootConstantBufferView(i, e.gpuVa);
                break;
            case EntryType::SRV:
                list->SetComputeRootShaderResourceView(i, e.gpuVa);
                break;
            case EntryType::UAV:
                list->SetComputeRootUnorderedAccessView(i, e.gpuVa);
                break;
            case EntryType::Constants:
                for (UINT k = 0; k < e.numConstants && k < kMaxConstants;)
                {
                    if (((e.knownMask >> k) & 1ull) == 0)
                    {
                        ++k;
                        continue;
                    }
                    UINT start = k;
                    UINT count = 0;
                    while (k < e.numConstants && k < kMaxConstants && ((e.knownMask >> k) & 1ull))
                    {
                        ++count;
                        ++k;
                    }
                    list->SetComputeRoot32BitConstants(i, count, &e.constants[start], start);
                }
                break;
            default:
                break;
            }
        }
    }
};
} // namespace DlssNr::Submission