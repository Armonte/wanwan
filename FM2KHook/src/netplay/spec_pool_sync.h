// spec_pool_sync.h -- MATCH-START POOL RESYNC (Phase 4c). Declarations only;
// the rationale, the measurement it closes and every safety argument live in
// spec_pool_sync.cpp's header comment. Deliberately its own header rather than
// riding spectator_node_internal.h, which is already over the 1000-line rule.
#pragma once
#include <cstdint>

namespace specnode {

// Kill-switch parse (FM2K_SPEC_POOL_SYNC, DEFAULT ON). Read once, logged once.
// Host and viewer both consult it; the harness forwards the variable to both.
bool PoolSync_Enabled();

// VIEWER ---------------------------------------------------------------------
// Active(): this viewer takes a fresh battle-entry snapshot at EVERY match
// boundary. False for offline replay, for a viewer whose join snapshot is still
// the thing in flight, and for a bounded deep joiner (which owns its own hold).
bool PoolSync_Active();
// Armed(): Active() and this match's snapshot has not landed (or timed out) yet.
// Gates the ApplyPendingSnapshot re-join guard's narrowing.
bool PoolSync_Armed();
// Per-match reset. Called from BOTH boundary ops: MATCH_END arms early (the
// host's push for the next battle reaches a lagging viewer before that
// battle's MATCH_START drains), MATCH_START re-arms and numbers the match.
void PoolSync_OnMatchEnd();
void PoolSync_OnMatchStart();
// Bounded battle-entry hold, from PopFrameInputs (after the deep-join hold).
bool PoolSync_ShouldHold();
// Hold supervision (one re-request per interval), from TickHealth.
void PoolSync_HoldTick(uint64_t now);
// Called from ApplyPendingSnapshot for EVERY successful apply.
void PoolSync_OnSnapshotApplied(uint32_t anchor);
// PHASE 4e (review A2.1). Gates the two PRE-4c fallbacks this feature exempts:
// the placeholder CSS drive arm (spec_recv.cpp) and the parked-snapshot freeze
// (spec_playback.cpp). Narrower than PoolSync_Active(): a viewer that re-JOINed
// mid-stream keeps both fallbacks until its next snapshot applies, because a
// re-anchored stream carries no char-select for it to mirror.
bool PoolSync_SuppressLegacyCssFallback();
// Latch the above. Called from HandleJoinAck when a GRANT lands on a viewer
// that already has a stream (i.e. a re-JOIN, not a fresh boot).
void PoolSync_OnViewerRejoin();
// PHASE 4e (review A3.2). True while this viewer is parked in the bounded
// battle-entry hold; the deep-join verdict uses it exactly as it uses
// pb_deep_join_await, so a forward anchor that lands on a held viewer is
// recognised as an unreachable battle instead of parking as WAIT.
bool PoolSync_Holding();
// ...and the release for that case: drop the hold NOW rather than at the 2500 ms
// budget, because the blob it was waiting for has been superseded.
void PoolSync_OnMissedBattle(uint32_t anchor, uint32_t consumed);

// HOST -----------------------------------------------------------------------
// Serve the per-battle snapshot to every bound subscriber, not only to the
// bounded deep joiners. Consulted by spec_deep_join.cpp's arm / push / request
// paths -- they already implement the pacing, the per-battle dedupe and the
// re-ship bookkeeping, so this is a widening of their audience, not a fork.
bool PoolSync_HostServes();

}  // namespace specnode
