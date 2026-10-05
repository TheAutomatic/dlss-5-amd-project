#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/misc/VulkanCallScope.h"
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <thread>
#include <type_traits>

static void Check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}

template<class Scope> static void Nesting()
{
    static_assert(!std::is_copy_constructible_v<Scope> && !std::is_move_constructible_v<Scope>);
    Check(!Scope::Active(), "default creation remains unprotected");
    {
        Scope outer;
        try { Scope inner; throw 1; }
        catch (int) {}
        Check(Scope::Active(), "nested exception preserves outer protection");
    }
    Check(!Scope::Active(), "outer scope restores ordinary Vulkan creation");
}

template<class Scope> static void ConcurrentProbe()
{
    std::binary_semaphore entered(0), probeFinished(0), checked(0), leave(0), exited(0);
    std::jthread initializer([&]
    {
        Check(!Scope::Active(), "new initializer starts unprotected");
        {
            Scope privateRuntime;
            entered.release();
            probeFinished.acquire();
            Check(Scope::Active(), "menu probe cannot clear private initialization protection");
            checked.release();
            leave.acquire();
        }
        Check(!Scope::Active(), "initializer restores its own state");
        exited.release();
    });
    entered.acquire();
    Check(!Scope::Active(), "private initializer must not suppress a native Vulkan caller on another thread");
    { Scope menuProbe; { Scope nestedProbe; } Check(Scope::Active(), "nested menu probe"); }
    Check(!Scope::Active(), "menu probe exits independently");
    probeFinished.release();
    checked.acquire();
    {
        Scope secondProbe;
        leave.release();
        exited.acquire();
        Check(Scope::Active(), "initializer exit cannot clear a still-active probe");
    }
    Check(!Scope::Active(), "both threads return to ordinary creation");
}

int main()
{
    Nesting<ScopedSkipVulkanHooks>();
    Nesting<ScopedCreatingD3DDevice>();
    for (int i = 0; i < 100; ++i)
    {
        ConcurrentProbe<ScopedSkipVulkanHooks>();
        ConcurrentProbe<ScopedCreatingD3DDevice>();
    }
    {
        ScopedCreatingD3DDevice translation;
        Check(!ScopedSkipVulkanHooks::Active(), "translation marker does not broaden the hook bypass policy");
        { ScopedSkipVulkanHooks internal; Check(ScopedCreatingD3DDevice::Active(), "combined private initialization"); }
        Check(ScopedCreatingD3DDevice::Active(), "independent scope kinds");
    }
    {
        ScopedSkipVulkanHooks internal;
        Check(!ScopedCreatingD3DDevice::Active(), "hook bypass does not imply a D3D creation");
    }
    std::puts("Vulkan call scope: PASS (nested exceptions, 200 controlled thread interleavings, native caller isolation)");
}
