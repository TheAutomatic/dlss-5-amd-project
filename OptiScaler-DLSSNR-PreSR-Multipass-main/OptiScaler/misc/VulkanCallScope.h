#pragma once

// These describe the current synchronous call stack, not process-wide state.
// A menu/GPU probe must never clear another thread's private runtime protection,
// nor hide a native Vulkan game's concurrent creation. New worker threads must
// establish their own scope if they perform an internal device creation.
class ScopedSkipVulkanHooks
{
    inline static thread_local unsigned depth = 0;

  public:
    ScopedSkipVulkanHooks() noexcept { ++depth; }
    ~ScopedSkipVulkanHooks() { --depth; }
    ScopedSkipVulkanHooks(const ScopedSkipVulkanHooks&) = delete;
    ScopedSkipVulkanHooks& operator=(const ScopedSkipVulkanHooks&) = delete;
    static bool Active() noexcept { return depth != 0; }
};

// Kept distinct: a D3D translation call suppresses Vulkan spoofing, while an
// explicit SkipVulkanHooks scope also suppresses game device/overlay capture.
class ScopedCreatingD3DDevice
{
    inline static thread_local unsigned depth = 0;

  public:
    ScopedCreatingD3DDevice() noexcept { ++depth; }
    ~ScopedCreatingD3DDevice() { --depth; }
    ScopedCreatingD3DDevice(const ScopedCreatingD3DDevice&) = delete;
    ScopedCreatingD3DDevice& operator=(const ScopedCreatingD3DDevice&) = delete;
    static bool Active() noexcept { return depth != 0; }
};
