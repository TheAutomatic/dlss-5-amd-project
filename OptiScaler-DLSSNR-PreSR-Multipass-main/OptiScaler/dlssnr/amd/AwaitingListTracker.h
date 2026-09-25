#pragma once
#include <d3d12.h>
#include <mutex>
#include <vector>
#include "../submission/CommandListProxy.h"

namespace DlssNr::AmdBridge
{

class AwaitingListTracker
{
    mutable std::mutex mutex_;
    std::vector<ID3D12GraphicsCommandList *> lists_;
    static constexpr size_t kMaxAwaitingLists = 16;

public:
    static bool ContainsTargetList(UINT n, ID3D12CommandList *const *c, ID3D12GraphicsCommandList *target)
    {
        if (!c || !target)
            return false;
        for (UINT i = 0; i < n; ++i)
        {
            if (c[i] == reinterpret_cast<ID3D12CommandList *>(target))
                return true;
            DlssNr::Submission::ILogicalCommandList *logical = nullptr;
            if (SUCCEEDED(c[i]->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                              reinterpret_cast<void **>(&logical))) && logical)
            {
                ID3D12CommandList *native = logical->UnsplitNativeList();
                const bool match = (native == reinterpret_cast<ID3D12CommandList *>(target));
                logical->Release();
                if (match)
                    return true;
            }
        }
        return false;
    }

    void Add(ID3D12GraphicsCommandList *cmd)
    {
        if (!cmd)
            return;
        std::lock_guard lock(mutex_);
        for (auto *existing : lists_)
            if (existing == cmd)
                return;
        if (lists_.size() >= kMaxAwaitingLists)
            lists_.erase(lists_.begin());
        lists_.push_back(cmd);
    }

    ID3D12GraphicsCommandList *MatchAndRemove(UINT n, ID3D12CommandList *const *c)
    {
        ID3D12GraphicsCommandList *matched = nullptr;
        std::lock_guard lock(mutex_);
        for (auto it = lists_.begin(); it != lists_.end(); )
        {
            if (ContainsTargetList(n, c, *it))
            {
                matched = *it;
                it = lists_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return matched;
    }

    size_t Count() const
    {
        std::lock_guard lock(mutex_);
        return lists_.size();
    }

    bool Contains(ID3D12GraphicsCommandList *cmd) const
    {
        std::lock_guard lock(mutex_);
        for (auto *existing : lists_)
            if (existing == cmd)
                return true;
        return false;
    }

    void Clear()
    {
        std::lock_guard lock(mutex_);
        lists_.clear();
    }
};

} // namespace DlssNr::AmdBridge
