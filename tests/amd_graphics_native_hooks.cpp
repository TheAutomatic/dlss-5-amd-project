// CPU-only: exercise real Detours forwarding on two synthetic list implementations.
// No D3D device, GPU, author runtime, or game is used.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/NativeWaitHooks.h"
#include <limits>
#include <stdexcept>
#include <iostream>

using namespace AmdPreSr::GraphicsSnap;
using namespace AmdPreSr::GraphicsSnap::NativeWaitHooks;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct SyntheticList
{
    void** vtable;
    volatile LONG draws = 0, dispatches = 0;
    UINT args[4] {};
};

__declspec(noinline) static void WINAPI DrawOne(ID3D12GraphicsCommandList* raw, UINT a, UINT b, UINT c, UINT d)
{
    auto& list = *reinterpret_cast<SyntheticList*>(raw);
    InterlockedIncrement(&list.draws);
    list.args[0] = a; list.args[1] = b; list.args[2] = c; list.args[3] = d;
}
__declspec(noinline) static void WINAPI DrawTwo(ID3D12GraphicsCommandList* raw, UINT a, UINT b, UINT c, UINT d)
{
    auto& list = *reinterpret_cast<SyntheticList*>(raw);
    InterlockedExchangeAdd(&list.draws, 2);
    list.args[0] = a; list.args[1] = b; list.args[2] = c; list.args[3] = d;
}
__declspec(noinline) static void WINAPI DispatchOne(ID3D12GraphicsCommandList* raw, UINT x, UINT y, UINT z)
{
    auto& list = *reinterpret_cast<SyntheticList*>(raw);
    InterlockedIncrement(&list.dispatches);
    list.args[0] = x; list.args[1] = y; list.args[2] = z;
}
__declspec(noinline) static void WINAPI DispatchTwo(ID3D12GraphicsCommandList* raw, UINT x, UINT y, UINT z)
{
    auto& list = *reinterpret_cast<SyntheticList*>(raw);
    InterlockedExchangeAdd(&list.dispatches, 2);
    list.args[0] = x; list.args[1] = y; list.args[2] = z;
}

int main()
{
    try
    {
        Require(!Ensure(nullptr, 0).drawCovered, "null list must not appear covered");
        void* vt1[15] {}, *vt2[15] {};
        vt1[12] = reinterpret_cast<void*>(&DrawOne); vt1[14] = reinterpret_cast<void*>(&DispatchOne);
        vt2[12] = reinterpret_cast<void*>(&DrawTwo); vt2[14] = reinterpret_cast<void*>(&DispatchTwo);
        SyntheticList one { vt1 }, two { vt2 };
        auto* a = reinterpret_cast<ID3D12GraphicsCommandList*>(&one);
        auto* b = reinterpret_cast<ID3D12GraphicsCommandList*>(&two);
        const auto first = Ensure(a, 0), second = Ensure(b, 0);
        Require(first.drawCovered && first.dispatchCovered, "first real target pair must attach");
        Require(second.drawCovered && second.dispatchCovered, "second real target pair must attach");
        Require(first.drawTarget != second.drawTarget, "fixture must use distinct implementations");
        Require(Ensure(a, 0).drawCovered, "same target reuse must not stack hooks");
        {
            ScopedNativeDrawObservation scope(reinterpret_cast<uintptr_t>(a), 1,
                                               (std::numeric_limits<uintptr_t>::max)());
            a->DrawInstanced(3, 1, 17, 19);
            Require(one.draws == 1 && one.args[2] == 17 && one.args[3] == 19,
                    "draw original called once with unchanged arguments");
            a->Dispatch(2, 3, 4);
            Require(one.dispatches == 1 && one.args[0] == 2 && one.args[2] == 4,
                    "dispatch original called once with unchanged arguments");
            b->DrawInstanced(5, 6, 7, 8);
            b->Dispatch(5, 6, 7);
            Require(two.draws == 2 && two.dispatches == 2 && two.args[2] == 7,
                    "second target must use its own trampoline");
            Require(scope.observation.hookHits == 2 && scope.observation.count == 1,
                    "observe both implementations but only count the scoped list");
            Require(scope.observation.dispatchHook == 2 && scope.observation.dispatchWait == 1,
                    "dispatch observation keeps list filtering");
        }
        a->DrawInstanced(1, 2, 3, 4);
        Require(one.draws == 2, "unscoped game draw must still be forwarded");
        // A foreign transaction must survive the observer's failed installation.
        Require(DetourTransactionBegin() == NO_ERROR, "begin fixture transaction");
        // Use a distinct valid function with the matching ABI, never invoke it.
        auto& slot = NativeWaitHooks::Detail::drawSlots[7];
        slot.original = DrawOne;
        Require(NativeWaitHooks::Detail::Install(slot, NativeWaitHooks::Detail::Draw<7>) != NO_ERROR,
                "observer must not join another transaction");
        Require(DetourTransactionAbort() == NO_ERROR, "observer must not abort another transaction");
        std::cout << "native wait hook forwarding scenarios passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
