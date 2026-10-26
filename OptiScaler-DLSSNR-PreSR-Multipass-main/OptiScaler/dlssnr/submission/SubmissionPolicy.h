#pragma once
#include <atomic>

// Initialized by Config before game recordings, independently of backend creation.
inline std::atomic<bool> g_allowEnhancedBarriers { false };
inline void SetAllowEnhancedBarriers(bool on) { g_allowEnhancedBarriers.store(on, std::memory_order_release); }
inline bool AllowEnhancedBarriers() { return g_allowEnhancedBarriers.load(std::memory_order_acquire); }

// Opt-in split query splice. A non-timestamp query still open at the cut is ended on
// the producer half and reopened at the head of the continuation. The book keeps the
// entry open, so the game's own EndQuery (recorded after the cut) closes the reopened
// pair. The game's resolved query result then covers only the post-split half of the
// original scope: an engine reading those statistics sees them change. Default false.
inline std::atomic<bool> g_splitQuerySplice { false };
inline void SetSplitQuerySplice(bool on) { g_splitQuerySplice.store(on, std::memory_order_release); }
inline bool SplitQuerySpliceAllowed() { return g_splitQuerySplice.load(std::memory_order_acquire); }
