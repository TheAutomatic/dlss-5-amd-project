#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace DlssNr::Submission
{
// A credential for discarded CPU recordings. It does not keep the command list alive.
// Reset publishes a new generation only after the native Reset succeeded.
struct ListGenerationToken
{
    std::atomic<uint64_t> generation { 0 };
    std::atomic<bool> destroyed { false };
};

struct ListGenerationSnapshot
{
    std::shared_ptr<ListGenerationToken> token;
    uint64_t generation = 0;

    bool Discarded() const
    {
        return token && (token->destroyed.load(std::memory_order_acquire) ||
                         token->generation.load(std::memory_order_acquire) != generation);
    }
};
}
