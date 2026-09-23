#pragma once
#include <d3d12.h>
#include <functional>
#include <unordered_set>

namespace DlssNr::Submission
{
// A non-timestamp BeginQuery/EndQuery pair must stay on one native command list.
// Completed queries and timestamp EndQuery calls do not prevent a later cut.
class QueryStateBook
{
    struct Key
    {
        ID3D12QueryHeap *heap;
        D3D12_QUERY_TYPE type;
        UINT index;

        bool operator==(const Key &other) const
        {
            return heap == other.heap && type == other.type && index == other.index;
        }
    };

    struct Hash
    {
        size_t operator()(const Key &key) const
        {
            const size_t h = std::hash<ID3D12QueryHeap *>{}(key.heap);
            return h ^ (static_cast<size_t>(key.type) << 16) ^ static_cast<size_t>(key.index);
        }
    };

    std::unordered_set<Key, Hash> open;

  public:
    void Reset() { open.clear(); }

    bool Begin(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
        if (type == D3D12_QUERY_TYPE_TIMESTAMP)
            return false;
        return open.insert({heap, type, index}).second;
    }

    bool End(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
        if (type == D3D12_QUERY_TYPE_TIMESTAMP)
            return true;
        return open.erase({heap, type, index}) == 1;
    }

    bool CanSplit() const { return open.empty(); }
};
} // namespace DlssNr::Submission
