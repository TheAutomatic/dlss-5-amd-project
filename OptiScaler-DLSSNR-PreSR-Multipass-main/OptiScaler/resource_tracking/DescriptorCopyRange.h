#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
namespace DescriptorTracking
{
// Lookup/cache lifetime is exactly one API call. Resource reads/writes retain
// their existing descriptor locks; inactive heaps are never reused by this cache.
template<class Lookup, class Copy>
void CopyRange(size_t count, size_t source, size_t destination, size_t increment, Lookup lookup, Copy copy)
{
    using Heap = decltype(lookup(source));
    if (count <= 1)
    {
        if (count)
        {
            auto src = source ? lookup(source) : Heap{};
            auto dst = lookup(destination);
            copy(src, source, dst, destination);
        }
        return;
    }
    Heap src, dst;
    auto resolve = [&](Heap& cached, size_t handle) {
        if (!cached || !cached->active.load(std::memory_order_acquire) ||
            handle < cached->cpuStart || handle >= cached->cpuEnd)
            cached = lookup(handle);
    };
    for (size_t i = 0; i < count; ++i)
    {
        const auto srcHandle = source ? source + i * increment : 0;
        const auto dstHandle = destination + i * increment;
        if (source) resolve(src, srcHandle);
        resolve(dst, dstHandle);
        copy(src, srcHandle, dst, dstHandle);
    }
}
}
