#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    Upscaler featureKey = Upscaler::Reset; // Backend the feature was created with - used in DX12
    NVSDK_NGX_Feature featureID = NVSDK_NGX_Feature_SuperSampling;
    NVSDK_NGX_Parameter* createParams = nullptr;
    bool createParamsOwned = false;
    // What the old feature ran at, kept for a switch whose createParams is the game's own block: the game refills
    // that block every evaluate, so the sizes are written again just before the new feature reads them.
    unsigned int createWidth = 0, createHeight = 0, createOutWidth = 0, createOutHeight = 0;
    int createFlags = 0, createQuality = 0;
    int changeBackendCounter = 0;
    UINT64 lastEvalFrame = 0;
};
