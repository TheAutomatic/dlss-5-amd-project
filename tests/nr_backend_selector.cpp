#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/Kind.h"
#include <cassert>
#include <iostream>

int main()
{
    using DlssNr::Backend::Kind;
    using DlssNr::Backend::LmxxfWired;
    using DlssNr::Backend::ParseKind;
    using DlssNr::Backend::ParseRequest;
    using DlssNr::Backend::Request;
    using DlssNr::Backend::ResolveInstalled;

    assert(ParseKind("") == Kind::Daniel);
    assert(ParseKind("auto") == Kind::Daniel);
    assert(ParseKind("daniel") == Kind::Daniel);
    assert(ParseKind("Daniel") == Kind::Daniel);
    assert(ParseKind("DANIEL") == Kind::Daniel);
    assert(ParseKind("lmxxf") == Kind::Lmxxf);
    assert(ParseKind("LMXXF") == Kind::Lmxxf);
    // Legacy "off"/"none"/unknown are Auto, not a third host.
    assert(ParseRequest("off") == Request::Auto);
    assert(ParseRequest("Off") == Request::Auto);
    assert(ParseRequest("none") == Request::Auto);
    assert(ParseRequest("garbage") == Request::Auto);
    assert(ParseRequest("") == Request::Auto);
    assert(ParseRequest("daniel") == Request::Daniel);
    assert(ParseRequest("lmxxf") == Request::Lmxxf);

    const bool wired = LmxxfWired();
    // Auto: prefer whichever is installed; lmxxf alone wins.
    assert(ResolveInstalled(Request::Auto, true, true, wired) == Kind::Daniel);
    assert(ResolveInstalled(Request::Auto, true, false, wired) == Kind::Daniel);
    assert(ResolveInstalled(Request::Auto, false, true, wired) == (wired ? Kind::Lmxxf : Kind::Daniel));
    assert(ResolveInstalled(Request::Auto, false, false, wired) == Kind::Daniel);

    // Explicit request honored when its files exist.
    assert(ResolveInstalled(Request::Daniel, true, true, wired) == Kind::Daniel);
    assert(ResolveInstalled(Request::Lmxxf, true, true, wired) == (wired ? Kind::Lmxxf : Kind::Daniel));
    assert(ResolveInstalled(Request::Lmxxf, false, true, wired) == (wired ? Kind::Lmxxf : Kind::Daniel));
    assert(ResolveInstalled(Request::Daniel, true, false, wired) == Kind::Daniel);

    // Fall back to the installed host when the request is missing its files.
    assert(ResolveInstalled(Request::Lmxxf, true, false, wired) == Kind::Daniel);
    assert(ResolveInstalled(Request::Daniel, false, true, wired) == (wired ? Kind::Lmxxf : Kind::Daniel));

    // Nothing installed: keep the request so the error can name the missing file.
    assert(ResolveInstalled(Request::Lmxxf, false, false, wired) == Kind::Lmxxf);
    assert(ResolveInstalled(Request::Daniel, false, false, wired) == Kind::Daniel);

    std::cout << "nr_backend_selector: ok (Wired=" << (wired ? "true" : "false") << ")\n";
    return 0;
}