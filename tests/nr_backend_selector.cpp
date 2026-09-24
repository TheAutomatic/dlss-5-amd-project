#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/Kind.h"
#include <cassert>
#include <iostream>

int main()
{
    using DlssNr::Backend::ActiveKind;
    using DlssNr::Backend::Kind;
    using DlssNr::Backend::LmxxfWired;
    using DlssNr::Backend::ParseKind;

    assert(ParseKind("") == Kind::Daniel);
    assert(ParseKind("auto") == Kind::Daniel);
    assert(ParseKind("daniel") == Kind::Daniel);
    assert(ParseKind("Daniel") == Kind::Daniel);
    assert(ParseKind("DANIEL") == Kind::Daniel);
    assert(ParseKind("lmxxf") == Kind::Lmxxf);
    assert(ParseKind("LMXXF") == Kind::Lmxxf);
    // Legacy "off"/"none" fall back to daniel; Enable NR is the on/off switch.
    assert(ParseKind("off") == Kind::Daniel);
    assert(ParseKind("Off") == Kind::Daniel);
    assert(ParseKind("none") == Kind::Daniel);
    assert(ParseKind("garbage") == Kind::Daniel);

    assert(ActiveKind(Kind::Daniel) == Kind::Daniel);
    if (LmxxfWired())
        assert(ActiveKind(Kind::Lmxxf) == Kind::Lmxxf);
    else
        assert(ActiveKind(Kind::Lmxxf) == Kind::Daniel);

    std::cout << "nr_backend_selector: ok (Wired=" << (LmxxfWired() ? "true" : "false") << ")\n";
    return 0;
}