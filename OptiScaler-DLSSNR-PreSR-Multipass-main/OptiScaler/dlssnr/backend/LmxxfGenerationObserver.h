#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace DlssNr::Backend::LmxxfProbe {

// Lightweight observer for command list generation (Reset) and submission events.
// Independent of the full AmdPreSr::GraphicsTracker; only tracks what the staging
// probe needs. Thread-safe: all methods may be called from any thread.
class GenerationObserver {
public:
    struct ListState {
        uint64_t generation = 0;     // Incremented on each successful Reset
        bool closedSinceLastReset = false;  // Set when submitted, cleared on Reset
        uint64_t lastSubmitFenceValue = 0;
    };

    // Called from hkCommandListReset. Only increments generation on success.
    void OnReset(IUnknown* listIdentity, bool succeeded) {
        if (!listIdentity) return;
        IUnknown* identity = nullptr;
        if (SUCCEEDED(listIdentity->QueryInterface(IID_PPV_ARGS(&identity)))) {
            std::lock_guard<std::mutex> lock(mu_);
            auto& state = states_[identity];
            if (succeeded) {
                state.generation++;
                state.closedSinceLastReset = false;
            }
            identity->Release();
        }
    }

    // Called from AmdBridge::ExecuteBatch after executeOriginal returns.
    // Records the submit and fence value for each list in the batch.
    void OnSubmitted(UINT count, ID3D12CommandList* const* lists, uint64_t fenceValue) {
        if (!lists) return;
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            IUnknown* identity = nullptr;
            if (SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&identity)))) {
                std::lock_guard<std::mutex> lock(mu_);
                auto& state = states_[identity];
                state.closedSinceLastReset = true;
                state.lastSubmitFenceValue = fenceValue;
                identity->Release();
            }
        }
    }

    // Query the current generation for a list identity. Returns 0 if unknown.
    uint64_t GetGeneration(IUnknown* listIdentity) const {
        if (!listIdentity) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = states_.find(listIdentity);
        if (it != states_.end()) {
            return it->second.generation;
        }
        return 0;
    }

    // Query whether a list has been submitted since its last Reset.
    bool IsSubmittedSinceReset(IUnknown* listIdentity) const {
        if (!listIdentity) return false;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = states_.find(listIdentity);
        if (it != states_.end()) {
            return it->second.closedSinceLastReset;
        }
        return false;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<IUnknown*, ListState> states_;
};

// Process-wide singleton. The staging probe and hooks both use this.
inline GenerationObserver& GlobalGenerationObserver() {
    static GenerationObserver instance;
    return instance;
}

} // namespace DlssNr::Backend::LmxxfProbe
