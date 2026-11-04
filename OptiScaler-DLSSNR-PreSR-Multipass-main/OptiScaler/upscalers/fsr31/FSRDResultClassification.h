#pragma once
#include "FSRDRetryPolicy.h"
#include <ffx_api.h>

namespace FSRD
{
inline RRResult ClassifyRRApiFailure(ffxReturnCode_t result, bool dynamicInput, bool deviceLost) noexcept
{
    if (deviceLost)
        return RRResult::DeviceLost;
    switch (result)
    {
    case FFX_API_RETURN_OK:
        return RRResult::Success;
    case FFX_API_RETURN_NO_PROVIDER:
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return RRResult::UnsupportedProvider;
    case FFX_API_RETURN_ERROR_PARAMETER:
        // Per-frame resources and tuning can recover without replacing the context.
        // Creation/default-query descriptors are fixed by our provider ABI.
        return dynamicInput ? RRResult::RetryableInputFailure : RRResult::UnsupportedProvider;
    default:
        // Unspecified/runtime/allocation errors cannot establish context usability.
        return RRResult::NeedsRecreation;
    }
}
} // namespace FSRD
