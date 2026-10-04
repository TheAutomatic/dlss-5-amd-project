#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/SrPlacement.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main()
{
    DlssNr::SrPlacement p;
    int frameA, frameB;
    for (bool requested : { true, false })
    {
        const bool before = p.Select(&frameA, true, requested);
        const bool after = p.Select(&frameA, false, !requested);
        assert(before == after); // menu change cannot alter an in-progress pair
        assert(int(before) + int(!after) == 1);
        assert(p.Select(&frameA, true, !requested) == !requested);
        assert(p.Select(&frameA, false, !requested) == !requested);
    }
    p.Select(&frameA, true, true); // failed SR: no After callback
    assert(!p.Select(&frameB, true, false));
    assert(!p.Select(&frameB, false, false));
    assert(!p.Select(&frameA, false, false)); // post-only caller uses its setting
    std::puts("SR placement: PASS (one stage per pair, setting changes, failed SR, post-only)");
}
