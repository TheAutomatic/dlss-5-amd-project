#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

// Bounded, generation-stable callback identities. Never reuse a published slot:
// clients can retain our callback address after unloading the originating DLL.
// A dead detour trampoline cannot be detached by writing to its unmapped target.
// Keeping at most Capacity tombstones bounds both metadata and Detours storage.
template <size_t Capacity> class PluginModuleSlots
{
public:
    using Resolver = void* (*)(const char*);
    struct Slot
    {
        std::atomic<HMODULE> module{nullptr};
        std::atomic<bool> live{false};
        Resolver target = nullptr, original = nullptr;
        unsigned char patch[16]{};
        unsigned kind = 0;
    };
    std::array<Slot, Capacity> slots{};
    std::recursive_mutex attachMutex;
private:
    void* notificationCookie_ = nullptr;
    std::once_flag notificationOnce_;
public:
    // Registry must live as long as its detours (the host uses static storage).
    bool StartNotifications()
    {
        struct Data { ULONG flags; const void* fullName; const void* baseName; PVOID base; ULONG size; };
        using Notify = void (CALLBACK*)(ULONG, const Data*, void*);
        using Register = LONG (NTAPI*)(ULONG, Notify, void*, void**);
        std::call_once(notificationOnce_, [this] {
            auto reg = reinterpret_cast<Register>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
            if (reg)
                reg(0, [](ULONG reason, const Data* data, void* context) {
                    if (reason == 2 && data)
                        static_cast<PluginModuleSlots*>(context)->Unloaded(static_cast<HMODULE>(data->base));
                }, this, &notificationCookie_);
        });
        return notificationCookie_ != nullptr;
    }

    class Lease
    {
        HMODULE module_ = nullptr;
    public:
        explicit Lease(Slot& s)
        {
            // No application mutex while entering the Windows loader.
            if (!s.live.load(std::memory_order_acquire)) return;
            HMODULE held = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCWSTR>(s.target), &held)) return;
            if (s.live.load(std::memory_order_acquire) && held == s.module.load() &&
                std::memcmp(reinterpret_cast<const void*>(s.target), s.patch, sizeof(s.patch)) == 0)
                module_ = held;
            else
                FreeLibrary(held);
        }
        ~Lease() { if (module_) FreeLibrary(module_); }
        Lease(const Lease&) = delete;
        explicit operator bool() const { return module_ != nullptr; }
    };

    // Called by Ldr DLL notification at actual unmap, not FreeLibrary(refcount--).
    // Loader-lock safe: only fixed-size atomic operations, no logging/locks/APIs.
    void Unloaded(HMODULE module)
    {
        for (auto& s : slots)
            if (s.module.load(std::memory_order_relaxed) == module)
                s.live.store(false, std::memory_order_release);
    }

    template <class Attach> int Hook(unsigned kind, HMODULE module, Resolver target, Attach attach)
    {
        if (!module || !target) return -1;
        // Loads can be reentrant and can run under loader lock. Never wait here.
        std::unique_lock lock(attachMutex, std::try_to_lock);
        if (!lock) return -1;
        for (size_t i = 0; i < Capacity; ++i)
        {
            auto& s = slots[i];
            if (s.live.load() && s.kind == kind && s.module.load() == module && s.target == target)
                return static_cast<int>(i);
        }
        for (size_t i = 0; i < Capacity; ++i)
        {
            auto& s = slots[i];
            if (s.module.load()) continue; // published identities are never recycled
            s.kind = kind;
            s.target = s.original = target;
            // Reserve before attach so a reentrant load cannot consume this slot.
            s.module.store(module);
            if (!attach(i, s.original))
            {
                s.original = s.target = nullptr;
                s.module.store(nullptr); // unpublished failure can be retried
                return -1;
            }
            std::memcpy(s.patch, reinterpret_cast<const void*>(s.target), sizeof(s.patch));
            s.live.store(true, std::memory_order_release);
            return static_cast<int>(i);
        }
        return -1; // retain every existing hook; additional copies stay untouched
    }
    bool Any(unsigned kind) const
    {
        for (const auto& s : slots)
            if (s.live.load(std::memory_order_acquire) && s.kind == kind) return true;
        return false;
    }
};
