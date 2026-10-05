#pragma once
#include "CommandListProxy.h"
#include <string>
#include <wrl/client.h>

namespace DlssNr::Submission
{
struct SplitEligibility
{
    bool allowed = false;
    std::string reason = "not-our-proxy";
};

// Snapshot only: does not reserve a continuation or relax any split guard.
// The text must outlive the queried interface and later diagnostic queries.
inline SplitEligibility ReadSplitEligibility(ID3D12GraphicsCommandList *cmd)
{
    SplitEligibility result;
    Microsoft::WRL::ComPtr<ILogicalCommandList> logical;
    if (!cmd || FAILED(cmd->QueryInterface(__uuidof(ILogicalCommandList),
                                          reinterpret_cast<void **>(logical.GetAddressOf()))) || !logical)
        return result;
    result.allowed = !logical->IsSplitIneligible();
    const char *reason = logical->SplitRejectionReason();
    result.reason = reason ? reason : "unknown";
    return result;
}
}
