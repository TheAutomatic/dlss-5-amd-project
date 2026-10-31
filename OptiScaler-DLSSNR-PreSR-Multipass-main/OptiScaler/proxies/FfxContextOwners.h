#pragma once
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>

// Providers stay loaded for the process lifetime. Never hold this lock while
// calling provider code (which may re-enter the FFX switchboard).
template <class Handle, class Provider> class FfxContextOwners
{
    std::mutex mutex_;
    std::unordered_map<Handle, Provider*> owners_;

  public:
    template <class Call, class Success>
    std::optional<std::invoke_result_t<Call, Provider*>> Destroy(Handle handle, Call call, Success success)
    {
        auto* provider = Find(handle);
        if (!provider)
            return std::nullopt;
        const auto result = call(provider);
        if (result == success)
            Erase(handle, provider);
        return result;
    }
    void Insert(Handle handle, Provider* provider)
    {
        std::lock_guard lock(mutex_);
        owners_[handle] = provider;
    }
    Provider* Find(Handle handle)
    {
        std::lock_guard lock(mutex_);
        auto it = owners_.find(handle);
        return it == owners_.end() ? nullptr : it->second;
    }
    void Erase(Handle handle, Provider* provider)
    {
        std::lock_guard lock(mutex_);
        auto it = owners_.find(handle);
        if (it != owners_.end() && it->second == provider)
            owners_.erase(it);
    }
};
