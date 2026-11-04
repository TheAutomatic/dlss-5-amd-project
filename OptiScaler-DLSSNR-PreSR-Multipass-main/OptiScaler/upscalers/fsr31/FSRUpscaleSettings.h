#pragma once

#include <ffx_upscale.h>

namespace FSR31
{
// This state belongs to one key on one context. Recreating the context must
// clear both the successfully applied value and the unsupported-descriptor flag.
struct UpscaleSettingState
{
    float desiredValue = 0.0f;
    float appliedValue = 0.0f;
    bool hasAppliedValue = false;
    bool unsupported = false;
    ffxReturnCode_t lastFailure = FFX_API_RETURN_OK;
};

struct UpscaleSettings
{
    UpscaleSettingState velocity;
    UpscaleSettingState reactiveScale;
    UpscaleSettingState shadingScale;
    UpscaleSettingState accAddPerFrame;
    UpscaleSettingState minDisOccAcc;

    void ResetForContext() { *this = {}; }
};

struct UpscaleSettingUpdate
{
    ffxReturnCode_t result = FFX_API_RETURN_OK;
    bool reportFailure = false;
};

template <typename Configure>
UpscaleSettingUpdate ApplyUpscaleSetting(ffxContext* context, UpscaleSettingState& state, float desiredValue,
                                        uint64_t key, Configure&& configure)
{
    state.desiredValue = desiredValue;
    if (state.unsupported)
        return {};

    if (state.hasAppliedValue && state.appliedValue == state.desiredValue)
    {
        state.lastFailure = FFX_API_RETURN_OK;
        return {};
    }

    ffxConfigureDescUpscaleKeyValue config {};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
    config.key = key;
    config.ptr = &state.desiredValue;

    const ffxReturnCode_t result = configure(context, &config.header);
    if (result == FFX_API_RETURN_OK)
    {
        state.appliedValue = state.desiredValue;
        state.hasAppliedValue = true;
        state.lastFailure = FFX_API_RETURN_OK;
        return {};
    }

    // The SDK maps effect errors (including an invalid key) to RUNTIME_ERROR,
    // so only UNKNOWN_DESCTYPE proves this context cannot configure the given
    // descriptor. PARAMETER/value errors and proxy NO_PROVIDER remain retryable.
    state.unsupported = result == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    const bool reportFailure = state.lastFailure != result;
    state.lastFailure = result;
    return { result, reportFailure };
}
} // namespace FSR31
