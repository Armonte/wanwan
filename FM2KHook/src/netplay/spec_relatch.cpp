// spec_relatch.cpp -- ROUND-LIMIT LATCH re-derive on the SPECTATOR/REPLAY
// plane. Sibling of 3297b25's player-side RederiveBattleEntryLatches
// (netplay_barriers.cpp), same shape, same traps, same kill switch.
//
// ============================ THE BUG =====================================
//
// vs_round_function latches g_round_limit (0x470048) from g_default_round
// (0x430124) at 0x4087DA when a sim crosses into battle, in the SAME call that
// writes g_game_mode = 3000. Everything downstream of that instant consumes the
// LATCH, not the source: ui_state_manager's two `for i < g_round_limit` pip
// loops (0x409DE1 / 0x409EC8) and the match-over predicate at 0x409710.
//
// A VIEWER can cross that instant before the host's settings have reached it.
// Measured, live, phase6_sweep_impl.md F4 (run r12, viewer S1, wanwan):
//
//   21:39:26.371  /F dispatch released -- the viewer's engine walks battle init
//                 (this is where 0x4087DA runs; g_default_round is still the
//                 viewer's own game.ini value, 3)
//   21:39:26.507  Netplay: Received HOST_CONFIG #1 (rounds=1) -> 0x430124 = 1
//   21:39:27.016  [CFG] plane=spec match=1 ... rounds=1 ... latch_rounds=3
//                        ^ SOURCE correct, LATCH stale, cfg_rx=2, digest agreed
//   21:39:27.027  snapshot applied
//   21:39:27.028  SpectatorNode: restored round cfg -- time=60 count=1
//   21:39:27.028  SpectatorNode: applied MATCH_START ...
//
// i.e. a ~136ms race between the viewer's own battle init and the first
// HOST_CONFIG. The host and guest of that same run stamped latch_rounds=1: this
// is a viewer-only residue of the class 3297b25 closed for players. The player
// fix cannot reach here -- hooks_update.cpp:243 returns before the battle-sync
// block when g_spectator_mode, and the spectator trampoline drives its own
// tick and never calls Netplay_IsBattleSynced.
//
// Blast radius on a viewer: +2 win pips per surplus round at battle frame 1 and
// a match-over predicate that disagrees with the host. Contained today only
// because the viewer follows the host's MATCH_END events rather than its own
// predicate -- i.e. a display / full-state divergence, not a player desync.
//
// ================== WHY THE SNAPSHOT WAS NOT ALREADY THE FIX ==============
//
// It half was, by accident, and that is exactly why this needed writing down.
// g_round_limit lives at 0x470048, INSIDE the saved GAME_STATE region
// { 0x470020, 0x220 } (savestate_internal.h), so SaveState_LoadFromBytes
// installs the host's value as a side effect of every deep-join / pool-resync
// apply. That incidental repair:
//   (a) is LATE -- in r12 it landed 11ms AFTER the viewer's engine had already
//       reached RSS_ACTIVE with the stale latch (the [CFG] stamp above);
//   (b) is ABSENT whenever the blob does not land: [POOLSYNC] OUTCOME=missed-
//       battle, the bounded hold expiring, or FM2K_SPEC_POOL_SYNC=0;
//   (c) does not exist AT ALL on the offline-replay plane, which never
//       subscribes and so is never PoolSync_Active().
// So the viewer's round limit was correct by luck of delivery. This makes it a
// derivation, from a value that is already in the ordered stream.
//
// ============================= THE WRITE ==================================
//
// At the MATCH_START op apply, immediately after the EXISTING restore of the
// source word:
//
//     *(uint32_t*)0x470048 = hdr_round_latch;   // g_round_limit <- h+89
//
// One word, only when ALL of: kill switch on, the header CARRIES the host's
// latch (h+89 != 0), that value is in 1..9, g_game_mode_flag == 1 (VS 1v1),
// and it differs from the live latch. Everything else is logging.
//
// THE WRITE SOURCE IS THE HOST'S LATCH, NOT THE HOST'S SOURCE WORD. This is
// the single most important line in this file, and the first version of this
// lane got it wrong. h+85 is the host's g_default_round -- a CONFIG SOURCE.
// The value the host's sim actually runs on is its g_round_limit (0x470048) --
// the LATCH. 3297b25 exists BECAUSE those two can disagree, and its own
// re-derive declines to write on three paths (no true settings agreement /
// mode_flag != 1 / implausible source), each of which leaves the host running
// latch L while h+85 advertises source S != L. Deriving a LATCH from a SOURCE
// is exactly the inversion this campaign refuted one plane over; doing it here
// would have installed S on the viewer and created a viewer-vs-host divergence
// that the pre-fix code did not have (the snapshot had been handing the viewer
// the host's L). So the host now stamps L itself at h+89
// (ReplayHeader::reserved[0], spec_host_events.cpp) and this is what we write.
// h+85 survives untouched for its existing consumer (the 0x430124 restore two
// lines above) and is carried here only to be PRINTED.
//
// h+89 == 0 means NOT CARRIED: a legacy .fm2krep, a producer older than this
// build, or a host whose own latch was outside 1..9. All three are refusals
// with a loud SKIPPED line, never a write -- the same not-a-real-value guard
// discipline the 0x430114 / 0x430124 restores already apply to their fields.
//
// ============== PRECEDES EVERY SPECTATOR-SIDE SAVE / CHECKSUM =============
// (code trace, not assertion -- the five legs the player fix's proof uses)
//
// 1. The write's only caller is the MATCH_START case of ApplySessionEvent
//    (spec_playback.cpp). Ops drain in the host's ordered stream order, and the
//    host appends MATCH_START inside Netplay_StartBattle (SpectatorNode_
//    OnMatchStart) BEFORE any OnFrameConfirmed of that battle, so MATCH_START(N)
//    precedes every INPUT of match N on the wire and therefore in the drain.
//    ApplySessionEvent ITSELF has TWO callers, not one: the ordered drain and
//    the seam results-overrun walk (spec_playback_seam.cpp:191), which applies
//    non-INPUT head ops while mode == 2000. The ordering leg survives (that
//    loop only consumes ops ahead of the INPUTs it discards), and the second
//    caller is in fact the path by which the pre-emptive mode<3000 apply
//    happens -- so it is named here rather than left out of the enumeration.
// 2. A viewer simulates a battle frame ONLY by popping an INPUT op
//    (SpectatorSimOneFrame -> SpectatorNode_PopFrameInputs). Every simulated
//    frame of match N therefore runs strictly after this write.
// 3. The viewer's checksums are computed inside that popped-frame path
//    (trampoline_spectator.cpp: [CINPUT] / [CHECKSUM] / [SPEC-FP]), so the very
//    first fenceposted frame of the match is already post-write. Verbatim from
//    the r12 log: "applied MATCH_START" then "[CHECKSUM] f=0" in the same
//    millisecond, in that order.
// 4. SaveState_Save NEVER RUNS ON A VIEWER. The only spectator drain that
//    saves is Netplay_ProcessSpectatorPhase (netplay_battle_phase.cpp:429) and
//    that function has no callers at all (recorded at spec_join_viewer.cpp:656);
//    the viewer's fingerprint is recomputed live by SaveState_CalculateFingerprint
//    (trampoline_spectator.cpp:240). So there is no viewer-side snapshot of
//    pre-write state for a load to bring back. CORRECTION TO AN EARLIER
//    VERSION OF THIS LEG: offline replay does NOT save either. It sets
//    g_spectator_mode = 1 (dllmain.cpp:700-704 -- is_offline_replay is a SUBSET
//    of spectator mode), runs RunSpectatorTick, and never creates a gekko
//    session, so the one automatic SaveState_Save site (the AdvanceEvent
//    handler) is unreachable there too. The leg is therefore easier than it was
//    written, not harder: no plane reached by this write has an automatic save
//    at all. (The determinism gate's RECORD pass IS a stress session and does
//    save -- which is why V4 is still run.)
// 5. THE SNAPSHOT APPLY IS THE ONE WRITER THAT CAN LAND AFTER US, AND IT CANNOT
//    REVERT US TO A STALE VALUE. SaveState_LoadFromBytes memcpys GAME_STATE,
//    which CONTAINS 0x470048, so a pool-resync / deep-join apply does overwrite
//    our word -- with the host's g_round_limit as captured by
//    SpectatorNode_StashSnapshot at the host's battle entry for THIS match.
//    THE CORRECTED FORM OF THIS LEG (the first version claimed two readings of
//    "the same host value" while actually naming two DIFFERENT words, h+85 =
//    the host's SOURCE vs the snapshot's 0x470048 = the host's LATCH -- they
//    coincide only when the host's own re-derive wrote, which it declines to do
//    on three paths):
//        h+89                 = *(uint32_t*)0x470048 read in
//                               SpectatorNode_OnMatchStart
//        the snapshot's word  = *(uint32_t*)0x470048 read by
//                               SpectatorNode_StashSnapshot -> SaveState_Save(0)
//    Both are the HOST'S LATCH, the same word, in the same call, with no store
//    to it in between: Netplay_StartBattle calls SpectatorNode_OnMatchStart and
//    then SpectatorNode_StashSnapshot, back to back (netplay_battle.cpp:444 and
//    :464). And both are POST the host's own RederiveBattleEntryLatches:
//    Netplay_IsBattleSynced() takes the g_battle_synced=true transition and
//    calls it (netplay_barriers.cpp:451), and BOTH of its call sites
//    (trampoline_battle.cpp:164, hooks_update.cpp:348) only call
//    Netplay_StartBattle after it returns true. So the two readings are now
//    genuinely the same value BY CONSTRUCTION, on every path including the
//    three where the host's re-derive refuses to write -- and BOTH
//    interleavings end at the host's round limit:
//        apply-then-MATCH_START  (observed, r12: 27.027 then 27.028)
//        MATCH_START-then-apply  (a blob arriving inside the bounded hold)
//    What would break this leg is a snapshot from a DIFFERENT match being
//    admitted; the admit rule forbids it (snapshot anchor == our consumed INPUT
//    position, plus the match_index on the request -- spec_pool_sync.cpp).
//    SpecRelatch_OnBattleFrameZero() asserts the outcome empirically anyway.
//
// =================== TRAP DISPOSITIONS (the player fix's four) ============
//
// (a) TEAM MODE -- SCOPED OUT WITH THE PREDICATE, identically. In
//     g_game_mode_flag == 2 the engine latches g_round_limit from
//     g_team_round_setting (0x470064) at 0x408797, not from g_default_round at
//     0x4087DA, so a re-derive from the round-count word would be wrong there.
//     Requires *(int32_t*)0x470058 == 1 and otherwise logs and returns. This
//     guard is NOT theoretical on the viewer plane: the Phase 6 rotation caught
//     DragonPuppy_version01a running with mode_flag=0 (1P/story) on all three
//     planes (finding F5), which lands in exactly this branch.
// (b) See the precedes-every-save proof above.
// (c) THE "AGREEMENT" TERM -- AND WHY THIS PLANE DOES NOT NEED ONE.
//     The player fix refuses to write unless the two peers' settings digests
//     are non-zero and equal, because its source (0x430124) is wire-supplied
//     and its barrier has escape hatches. This plane does not have that
//     problem, because it does not derive anything: h+89 IS the answer. The
//     host's agreement question was already asked and answered ON THE HOST, and
//     whatever the outcome -- wrote, or declined on any of the three refusal
//     paths -- 0x470048 afterwards holds the number the host's sim is going to
//     run this match on. Copying it is correct in all four cases; there is
//     nothing left for a viewer-side agreement term to protect.
//     (An earlier version of this comment argued "the existing code already
//     trusts h+85 enough to write the SOURCE with it, so writing the LATCH from
//     it cannot be less safe". That is precisely backwards and is recorded here
//     as the refuted claim: a source and a latch are not interchangeable, which
//     is the entire premise of 3297b25.)
//     Everything the header cannot vouch for is still a refusal, not a write:
//     h+89 == 0 (legacy producer / not carried / host latch out of range) and
//     anything above 9.
// (d) g_score_value (0x470050) is NOT touched, same as the player fix.
//
// DELIBERATE SCOPE DECISION, inherited verbatim: g_active_stage_id (0x470040)
// is DETECTED, NOT WRITTEN. The engine runs
// `g_active_stage_id = g_selected_stage; LoadStageFile(g_selected_stage);` back
// to back, so by the time we run the stage FILE for the stale id is already
// loaded; writing the id alone would leave the two halves disagreeing. The
// detector additionally requires game_mode >= 3000, because a viewer that is
// still walking character-select has not latched a stage for this match at all
// and its 0x470040 is last match's -- reporting that as a mismatch would be a
// false positive on every first match.
//
// SANITY BOUND 1..9: the engine's own settings dialog (settings_dialog_proc
// 0x4160F0) sets the 0x430124 spin control's UDM_SETRANGE to 0x00090001, and
// vs_round_function treats 0 as fatal at 0x408904 (error modal). Refusing is
// fail-closed = pre-fix behaviour, and loud.
//
// ============== WHY [SPEC-RELATCH] TRIP IS A SOUND FATAL TERM =============
//
// TRIP fires when g_round_limit at the viewer's first battle frame is not the
// value this match's MATCH_START header authorised. The harness treats it as
// FATAL, which is only defensible if every path that reaches it is a REAL
// viewer-vs-host divergence rather than an artefact. Enumerating the writers
// that can land between our write and frame zero:
//
//  W1 the snapshot apply (SaveState_LoadFromBytes, GAME_STATE memcpy).
//     Installs the host's 0x470048 for THIS match -- the same word h+89 was
//     read from, in the same host call, with no store in between (leg 5). It
//     can no longer differ. Before the h+89 amendment it could, and that was
//     the review's counterexample: a fatal red on a viewer that was tracking
//     the host correctly. That path is now closed at the source.
//  W2 the viewer's OWN engine battle init (0x4087DA), when our apply was
//     pre-emptive (mode < 3000). It re-latches from 0x430124 = h+85, the
//     host's SOURCE. If the host's own re-derive DECLINED, h+85 (S) != h+89
//     (L), so the viewer ends the window on S while the host runs L. That IS a
//     real divergence -- the viewer will draw the wrong pip count and disagree
//     with the host's match-over predicate -- and it is exactly what a fatal
//     term should catch. It is NOT this fix misbehaving; it is this fix
//     DETECTING a session the host already broke (the host log carries
//     [ROUNDS-RELATCH] SKIPPED on the same run). We do not "fix" it by also
//     writing 0x430124: that word is a config source with other consumers and
//     with the settings digest hanging off it.
//  W3 a later HOST_CONFIG re-writing 0x430124 inside the same pre-emptive
//     window, so W2 latches a different value. The host re-broadcasts
//     HOST_CONFIG from inside Netplay_StartBattle, immediately after
//     StashSnapshot -- i.e. it carries the settings of the SAME match whose
//     MATCH_START we just applied, so the re-latch lands on the same number.
//     For W3 to bite, the viewer would have to still be initialising match N
//     while match N+1's config arrives, which also means match N+1's
//     MATCH_START is in the drain re-arming the expectation. Narrow, not
//     observed, and diagnosable now that every line carries a match ordinal.
//  W4 the kill-switch-OFF arm. Routed to KILL-SWITCH ARM by the
//     !s_expect_from_fix test ordering, never to TRIP.
//  W5 refusals (h+89 == 0, mode_flag != 1, > 9). These do not arm the ON-arm
//     expectation at all, so they cannot TRIP.
//
// => every reachable TRIP is a genuine "this viewer is about to simulate the
// match on a round limit the host is not using". FATAL. The line to grep for
// attribution is the HOST log's [ROUNDS-RELATCH] SKIPPED, which names W2.
//
// MATCH ORDINAL: every verdict line carries match=%u, a per-process count of
// MATCH_START applies. It pairs 1:1 with [CFG]'s own per-process ordinal (both
// count this viewer's battles, in order, whichever of the two fires first for
// a given battle), and it exists so the harness's sample-point carve-out can
// excuse a stamped mismatch for THE MATCH IT ACTUALLY REPAIRED instead of
// laundering match 1's correction over match 5's stale stamp. Misalignment is
// fail-closed by construction: an ordinal that does not match leaves the fatal
// term standing.
//
// LOG BUDGET: at most two lines per match (the verdict + the frame-zero
// assertion), never per frame. Hook logging is SYNCHRONOUS.

#include "spec_relatch.h"

#include "netplay.h"           // Netplay_MatchSettingsDigest / HostConfigRxCount
#include "../core/globals.h"   // FM2K::kIsFM2K, FM2K::ADDR_GAME_MODE

#include <SDL3/SDL_log.h>
#include <cstdlib>

namespace {

// Latched addresses. Named locally rather than pulled from a header so this TU
// reads the same way netplay_barriers.cpp's re-derive does.
constexpr uintptr_t kRoundLimit    = 0x470048;  // g_round_limit     (LATCH)
constexpr uintptr_t kActiveStageId = 0x470040;  // g_active_stage_id (LATCH)
constexpr uintptr_t kDefaultRound  = 0x430124;  // g_default_round   (source)
constexpr uintptr_t kGameModeFlag  = 0x470058;  // 0=story 1=VS 2=team

constexpr uint32_t kMaxPlausibleRounds = 9;

// Set by the MATCH_START apply, consumed by the frame-zero tripwire.
bool     s_expect_armed  = false;
uint32_t s_expect_rounds = 0;
// Per-process count of MATCH_START applies. Stamped on every verdict line so
// the harness can key its sample-point excuse to ONE match. Pairs 1:1 with
// [CFG]'s s_cfg_match_ord (round_events.cpp), which counts the same battles.
uint32_t s_match_ord     = 0;
// false = armed by the kill-switch-OFF branch, i.e. we deliberately did NOT
// write and the readback is a MEASUREMENT of the pre-fix behaviour, not an
// assertion about ours. Keeping the readback armed in the OFF arm is what makes
// the red and the green arm the SAME instrument.
bool     s_expect_from_fix = false;

}  // namespace

bool RoundsRelatch_Enabled() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("FM2K_ROUNDS_RELATCH");
        if (v == nullptr || v[0] == '\0') {
            s_on = 1;                       // default ON -- this is the fix
        } else if ((v[0] == '0' || v[0] == '1') && v[1] == '\0') {
            s_on = v[0] - '0';              // strict: only "0" and "1" parse
        } else {
            s_on = 1;                       // fail SAFE (fix stays on), loudly
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[ROUNDS-RELATCH] FM2K_ROUNDS_RELATCH=\"%s\" is not \"0\" or "
                "\"1\" -- refusing to guess; keeping the re-derive ON", v);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[ROUNDS-RELATCH] contract: enabled=%d (FM2K_ROUNDS_RELATCH, "
            "default ON; ONE switch for BOTH planes -- the player battle-entry "
            "barrier re-derive [ROUNDS-RELATCH] and the viewer MATCH_START "
            "re-derive [SPEC-RELATCH]; =0 restores the pre-fix stale-latch "
            "behaviour on both for A/B only -- it re-opens a real desync)",
            s_on);
    }
    return s_on == 1;
}

namespace specnode {

void SpecRelatch_OnMatchStart(uint8_t hdr_round_latch, uint32_t hdr_round_count,
                              int32_t hdr_stage_id) {
    if constexpr (!FM2K::kIsFM2K) {
        (void)hdr_round_latch;
        (void)hdr_round_count;
        (void)hdr_stage_id;
        return;                 // FM95: none of these globals are mapped
    } else {
        s_expect_armed = false;
        ++s_match_ord;          // pairs with [CFG]'s per-process match ordinal

        const bool     on          = RoundsRelatch_Enabled();  // emits contract
        const uint32_t hdr_latch   = (uint32_t)hdr_round_latch;
        const uint32_t latch_round = *(const uint32_t*)kRoundLimit;
        const uint32_t src_round   = *(const uint32_t*)kDefaultRound;
        const int32_t  latch_stage = *(const int32_t*) kActiveStageId;
        const int32_t  mode_flag   = *(const int32_t*) kGameModeFlag;
        const uint32_t mode        = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
        // h+89 == 0 is the NOT-CARRIED sentinel and the ONLY authority gate.
        const bool carried = (hdr_latch != 0u);

        if (!on) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] DISABLED by kill switch -- match=%u "
                "latch_rounds=%u hdr_latch=%u hdr_rounds=%u cfg_rounds=%u "
                "mode=%u (a mismatch between the latch and hdr_latch here is "
                "the viewer stale-latch bug)",
                s_match_ord, latch_round, hdr_latch, hdr_round_count,
                src_round, mode);
            // Still arm the frame-zero READBACK when the header CARRIED the
            // host's latch: the OFF arm must be measured with the SAME
            // instrument, and against the SAME authority, as the ON arm, or the
            // red half of a red/green pair is only an absence of log lines.
            // Writes nothing -- see the tripwire.
            if (carried) {
                s_expect_armed    = true;
                s_expect_rounds   = hdr_latch;
                s_expect_from_fix = false;
            }
            return;
        }
        if (!carried) {
            // The header does not carry the host's LATCH. Three producers land
            // here, all fail-closed: a legacy .fm2krep or any stream from a
            // build older than the h+89 stamp (the whole 96-byte header is
            // memset to 0, so the field reads 0 by construction); and a host
            // whose own g_round_limit was outside 1..9 at stamp time. Not an
            // ERROR: this is a producer capability, not an anomaly. Note the
            // deliberate refusal to fall back on hdr_rounds (h+85) -- that is
            // the host's config SOURCE, and writing a LATCH from a SOURCE is
            // the exact inversion 3297b25 refutes; a stale-but-consistent
            // viewer beats a confidently-wrong one.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] SKIPPED -- match=%u: this MATCH_START does not "
                "carry the host's round-limit LATCH (h+89 = 0 = legacy/older "
                "producer, or the host's own latch was out of range). Leaving "
                "latch_rounds=%u alone (pre-fix behaviour); NOT falling back on "
                "hdr_rounds=%u, which is the host's config SOURCE (h+85) and "
                "not what its sim runs on. The viewer's limit is whatever its "
                "own battle init latched from cfg_rounds=%u",
                s_match_ord, latch_round, hdr_round_count, src_round);
            return;
        }
        if (mode_flag != 1) {
            // Trap (a). Reachable in the field: Phase 6 F5 (DragonPuppy).
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] SKIPPED -- match=%u: g_game_mode_flag=%d is not "
                "VS 1v1; team mode latches g_round_limit from "
                "g_team_round_setting (0x470064 @ 0x408797), so this viewer's "
                "own engine may not even be in the regime this re-derive was "
                "reasoned about. latch_rounds=%u hdr_latch=%u",
                s_match_ord, mode_flag, latch_round, hdr_latch);
            return;
        }
        if (hdr_latch > kMaxPlausibleRounds) {
            // Unreachable through the current producer (it stamps 0 rather than
            // an out-of-range byte) but kept as a consumer-side bound: this
            // value is wire-supplied and the gate must not depend on a remote
            // build being the one we think it is.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] REFUSED -- match=%u: hdr_latch=%u is not a "
                "plausible round limit (must be 1..%u; the engine's own check "
                "at 0x408904 treats 0 as fatal and pops an error modal). "
                "Leaving latch_rounds=%u alone (pre-fix behaviour). The value "
                "is host-supplied (MATCH_START h+89 = the host's g_round_limit "
                "post its own re-derive), so this line means the HOST announced "
                "a bad round limit, not that the re-derive is wrong.",
                s_match_ord, hdr_latch, kMaxPlausibleRounds, latch_round);
            return;
        }

        // Armed for the frame-zero assertion whether or not we had to write:
        // "the latch already agreed" is exactly as much a claim about the first
        // battle frame as "we corrected it".
        s_expect_armed    = true;
        s_expect_rounds   = hdr_latch;
        s_expect_from_fix = true;

        if (latch_round != hdr_latch) {
            *(uint32_t*)kRoundLimit = hdr_latch;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] CORRECTED match=%u g_round_limit %u -> %u at "
                "the MATCH_START apply (mode=%u, hdr_rounds=%u cfg_rounds=%u "
                "cfg=0x%08X cfg_rx=%u). The written value is the HOST'S LATCH "
                "(h+89), not its config source. mode>=3000 means this viewer "
                "had ALREADY latched its own game.ini round count for this "
                "match; mode<3000 means the engine has not latched yet and this "
                "write is pre-emptive -- the engine will re-latch from "
                "cfg_rounds, so if hdr_rounds != hdr_latch (the host's own "
                "re-derive declined) expect a TRIP at frame zero and read the "
                "HOST log's [ROUNDS-RELATCH] SKIPPED line. Without this the "
                "viewer would draw %u extra win pips and disagree with the "
                "host's match-over predicate.",
                s_match_ord, latch_round, hdr_latch, mode, hdr_round_count,
                src_round,
                Netplay_MatchSettingsDigest(), Netplay_HostConfigRxCount(),
                (latch_round > hdr_latch)
                    ? 2u * (latch_round - hdr_latch)
                    : 2u * (hdr_latch - latch_round));
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] ok match=%u latch_rounds=%u hdr_latch=%u "
                "hdr_rounds=%u mode=%u cfg_rx=%u (no correction needed)",
                s_match_ord, latch_round, hdr_latch, hdr_round_count, mode,
                Netplay_HostConfigRxCount());
        }

        // Stage: DETECT ONLY, and only once this viewer is actually in battle
        // (see the scope decision in the header comment).
        if (mode >= 3000u && mode < 4000u && latch_stage != hdr_stage_id) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] STAGE-LATCH MISMATCH match=%u latch_stage=%d "
                "hdr_stage=%d -- the same race stranded g_active_stage_id, and "
                "this viewer has ALREADY LOADED the stale stage file "
                "(g_active_stage_id = g_selected_stage; LoadStageFile(...) run "
                "back to back at battle init). NOT corrected here: writing the "
                "id without reloading the assets would only make the two halves "
                "disagree. Expect a stage-driven divergence this match.",
                s_match_ord, latch_stage, hdr_stage_id);
        }
    }
}

void SpecRelatch_OnBattleFrameZero() {
    if constexpr (!FM2K::kIsFM2K) {
        return;
    } else {
        if (!s_expect_armed) {
            return;   // no MATCH_START header applied for this match yet
        }
        s_expect_armed = false;
        const uint32_t live = *(const uint32_t*)kRoundLimit;
        if (live != s_expect_rounds && !s_expect_from_fix) {
            // KILL-SWITCH ARM. The re-derive was disabled, so this is the
            // pre-fix behaviour MEASURED at the instant it starts to matter:
            // the viewer is about to simulate this match with a round limit the
            // host is not using. This line is the red half of the red/green
            // pair; with the switch back at its default it becomes
            // "[SPEC-RELATCH] CORRECTED" plus "frame-zero ok".
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] KILL-SWITCH ARM match=%u -- g_round_limit is %u "
                "at this viewer's first battle frame but the host's MATCH_START "
                "header said %u. The re-derive is OFF (FM2K_ROUNDS_RELATCH=0), "
                "so this is the STALE LATCH the fix exists to repair, "
                "unrepaired: this viewer will draw %u extra win pips and "
                "disagree with the host's match-over predicate for the whole "
                "match.",
                s_match_ord, live, s_expect_rounds,
                (live > s_expect_rounds) ? 2u * (live - s_expect_rounds)
                                         : 2u * (s_expect_rounds - live));
        } else if (live != s_expect_rounds) {
            // Two writers can land between the MATCH_START apply and here, and
            // BOTH make this a real divergence rather than an instrument
            // artefact (full enumeration W1-W5 in the header comment):
            //   W1 SaveState_LoadFromBytes (pool-resync / deep-join), whose
            //      GAME_STATE memcpy covers 0x470048. Post-h+89 it carries the
            //      SAME word we wrote from, read in the same host call -- so if
            //      this fires through W1, proof leg 5 is wrong and an
            //      out-of-match snapshot was admitted.
            //   W2 the viewer's own engine battle init re-latching from
            //      0x430124 when our apply was pre-emptive. That reinstalls the
            //      host's SOURCE (h+85), which differs from its LATCH (h+89)
            //      only when the host's own re-derive declined -- i.e. the host
            //      log carries [ROUNDS-RELATCH] SKIPPED for this match.
            // Either way the viewer is about to simulate on a limit the host is
            // not using, which is why the harness treats this as FATAL.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] TRIP match=%u -- g_round_limit is %u at the "
                "viewer's first battle frame but this match's MATCH_START "
                "header authorised %u. Something between the two put a "
                "different value back. Suspect 1: a snapshot apply carrying "
                "another match's GAME_STATE. Suspect 2: this viewer's own "
                "battle init re-latched from cfg_rounds because the apply was "
                "pre-emptive AND the host's own re-derive had declined (grep "
                "the HOST log for [ROUNDS-RELATCH] SKIPPED). This viewer will "
                "draw the wrong number of win pips and disagree with the host's "
                "match-over predicate.",
                s_match_ord, live, s_expect_rounds);
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-RELATCH] frame-zero ok match=%u -- g_round_limit=%u "
                "matches this match's MATCH_START header latch (re-derive %s)",
                s_match_ord, live,
                s_expect_from_fix ? "ON" : "OFF -- this viewer did not race");
        }
    }
}

}  // namespace specnode
