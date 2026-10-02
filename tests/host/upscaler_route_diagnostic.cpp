#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/UpscalerRouteDiagnostic.h"
#include <cassert>
#include <iostream>

int main()
{
    using Event = UpscalerRouteDiagnostic::Event;
    UpscalerRouteDiagnostic missing;
    for (uint64_t t = 0; t < 30000; t += 100)
        assert(missing.Observe(t, true, false) == Event::None);
    assert(missing.Observe(30000, true, false) == Event::MissingRoute);
    for (uint64_t t = 30100; t < 100000; t += 100)
        assert(missing.Observe(t, (t % 200) == 0, false) == Event::None);
    assert(missing.Observe(100000, true, true) == Event::RouteObserved);
    assert(missing.Observe(200000, true, true) == Event::None);
    assert(missing.Observe(300000, true, false) == Event::None);

    UpscalerRouteDiagnostic loading;
    assert(loading.Observe(0, true, false) == Event::None);
    assert(loading.Observe(120000, true, false) == Event::None);
    for (uint64_t t = 120100; t < 150000; t += 100)
        assert(loading.Observe(t, true, false) == Event::None);
    assert(loading.Observe(150000, true, false) == Event::MissingRoute);

    UpscalerRouteDiagnostic disabled;
    for (uint64_t t = 0; t <= 60000; t += 100)
        assert(disabled.Observe(t, false, false) == Event::None);
    assert(disabled.Observe(60100, true, false) == Event::None);
    assert(disabled.Observe(60200, true, true) == Event::None);
    for (uint64_t t = 60300; t < 120000; t += 100)
        assert(disabled.Observe(t, true, false) == Event::None);
    std::cout << "upscaler route diagnostic: PASS\n";
}
