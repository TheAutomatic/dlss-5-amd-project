#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/PluginModuleSlots.h"
#include <detours/detours.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
static PluginModuleSlots<4> registry;
template<size_t I> int Value()
{
    PluginModuleSlots<4>::Lease lease(registry.slots[I]);
    if (!lease) return -1;
    auto fn = reinterpret_cast<int(*)()>(registry.slots[I].original("value"));
    return fn ? fn() : -2;
}
template<size_t I> void* Resolve(const char* name)
{
    PluginModuleSlots<4>::Lease lease(registry.slots[I]);
    if (!lease || !name) return nullptr;
    if (std::strcmp(name, "value") == 0) return reinterpret_cast<void*>(&Value<I>);
    return registry.slots[I].original(name);
}
static PluginModuleSlots<4>::Resolver hooks[] = {Resolve<0>,Resolve<1>,Resolve<2>,Resolve<3>};
static auto Target(HMODULE m) { return reinterpret_cast<PluginModuleSlots<4>::Resolver>(GetProcAddress(m,"slGetPluginFunction")); }
static int Hook(HMODULE m, bool fail = false)
{
    return registry.Hook(0,m,Target(m),[fail](size_t i, auto& original) {
        if (fail) return false;
        assert(DetourTransactionBegin()==NO_ERROR);
        assert(DetourUpdateThread(GetCurrentThread())==NO_ERROR);
        assert(DetourAttach(reinterpret_cast<PVOID*>(&original),reinterpret_cast<PVOID>(hooks[i]))==NO_ERROR);
        return DetourTransactionCommit()==NO_ERROR;
    });
}
int main(int argc,char** argv)
{
    assert(argc==3 && registry.StartNotifications());
    auto a=LoadLibraryA(argv[1]), b=LoadLibraryA(argv[2]); assert(a && b && a!=b);
    assert(Hook(a)==0 && Hook(a)==0); // duplicate notification
    auto valueA=reinterpret_cast<int(*)()>(Target(a)("value"));
    assert(valueA && valueA()==11);
    assert(Hook(b,true)==-1 && valueA()==11); // rejected B retains A
    assert(Hook(b)==1);
    auto valueB=reinterpret_cast<int(*)()>(Target(b)("value"));
    assert(valueB()!=valueA() && valueB()==22);
    assert(Target(a)("missing")==nullptr);
    std::vector<std::thread> threads;
    for(int i=0;i<4;i++) threads.emplace_back([=]{ for(int j=0;j<1000;j++) { assert(valueA()==11); assert(valueB()==22); } });
    for(auto& t:threads)t.join();
    auto extra=LoadLibraryA(argv[1]); assert(extra==a);
    FreeLibrary(extra); assert(valueA()==11); // refcount decrement is not retirement
    {
        PluginModuleSlots<4>::Lease held(registry.slots[0]); assert(held);
        FreeLibrary(a); assert(valueA()==11); // active call owns the last module reference
    }
    assert(valueA()==-1 && !registry.slots[0].live.load());
    a=LoadLibraryA(argv[1]); assert(a && Hook(a)==2); // reload gets a fresh callback identity
    assert(valueA()==-1 && Value<2>()==11);
    FreeLibrary(a); FreeLibrary(b);
    assert(valueB()==-1 && !registry.Any(0));
    a=LoadLibraryA(argv[1]); assert(Hook(a)==3 && Value<3>()==11);
    b=LoadLibraryA(argv[2]); assert(Hook(b)==-1); // bounded capacity, original B still works
    assert(reinterpret_cast<int(*)()>(Target(b)("value"))()==22 && Value<3>()==11);
    FreeLibrary(a); FreeLibrary(b);
    std::cout<<"Streamline plugin slots: PASS (real DLLs, detours, unload, concurrent callbacks, retirement/capacity)\n";
}
