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
    assert(ParseKind("off") == Kind::Off);
    assert(ParseKind("Off") == Kind::Off);
    assert(ParseKind("none") == Kind::Off);
    assert(ParseKind("garbage") == Kind::Daniel);
    assert(!LmxxfWired());
    assert(ActiveKind(Kind::Daniel) == Kind::Daniel);
    assert(ActiveKind(Kind::Off) == Kind::Off);
    assert(ActiveKind(Kind::Lmxxf) == Kind::Daniel);
    std::cout << "nr_backend_selector: ok\n";
    return 0;
}
