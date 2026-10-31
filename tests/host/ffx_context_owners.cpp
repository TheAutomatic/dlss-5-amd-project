#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/proxies/FfxContextOwners.h"
#include <cassert>
#include <thread>
#include <vector>

struct Provider { int id; };

int main()
{
    FfxContextOwners<int, Provider> owners;
    Provider native {1}, packaged {2};
    assert(!owners.Find(1));
    owners.Insert(1, &native);
    assert(owners.Find(1) == &native);
    owners.Erase(1, &packaged); // a different provider cannot remove ownership
    assert(owners.Find(1) == &native);
    owners.Insert(1, &packaged);
    owners.Erase(1, &native); // stale completion cannot remove a new owner
    assert(owners.Find(1) == &packaged);
    owners.Erase(1, &packaged);
    assert(!owners.Find(1));

    owners.Insert(2, &native);
    auto failed = owners.Destroy(2, [&](Provider* provider) {
        assert(provider == &native);
        assert(owners.Find(2) == &native); // re-entry must not deadlock
        return -1;
    }, 0);
    assert(failed && *failed == -1 && owners.Find(2) == &native);
    auto retried = owners.Destroy(2, [&](Provider* provider) {
        assert(provider == &native);
        return 0;
    }, 0);
    assert(retried && *retried == 0 && !owners.Find(2));
    assert(!owners.Destroy(2, [](Provider*) { assert(false); return 0; }, 0));

    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker)
        threads.emplace_back([&, worker] {
            for (int i = 0; i < 2000; ++i)
            {
                const int handle = worker * 2000 + i + 10;
                owners.Insert(handle, &native);
                assert(owners.Find(handle) == &native);
                owners.Erase(handle, &native);
                assert(!owners.Find(handle));
            }
        });
    for (auto& thread : threads)
        thread.join();
}
