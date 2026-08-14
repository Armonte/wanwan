// netplay_battle_end.cpp -- Netplay_EndBattle: battle-session teardown +
// match-outcome capture. Split from netplay_battle.cpp; declared in netplay.h.
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "../hooks/seam_free_probe.h"  // [ENDSEAM-FREE] window close at the swap point
#include "control_channel.h"
#include "game_hash.h"
#include <algorithm>   // std::max for the rounds-cache merge (#63 lineage)
#include "input.h"
#include "savestate.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "upload_queue.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome
#include "../parity/parity_recorder.h"  // ParityRecorder::Close on harness auto-terminate
#include <SDL3/SDL_log.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include <cstdio>   // std::snprintf -- peer-suffixed replay filename
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctime>
#include <random>
#include <cstdio>
#include <cstring>
#include <atomic>

void Netplay_EndBattle() {
    // FM2K: neutralize afterimage references before the engine tears the
    // battle down (task #53). Each object's afterimage pool index (BYTE at
    // +0x151; sprite_rendering_engine reads it at 0x40cd15) chains render
    // into heap script pointers that the battle->CSS transition frees. A
    // rollback restore near the boundary (the catch-up burst after a
    // network stall) can resurrect the index after the free -- the next
    // render then walks a freed pointer (deterministic AV at 0x40cd47
    // under the CGNAT-rebind rig). The CSS init rebuilds the object pool
    // anyway; clearing the index here only closes the stale-render window.
    // Symmetric: both peers run EndBattle at the agreed swap frame.
    if constexpr (FM2K::kIsFM2K) {
        Fm2k_ClearAfterimageIndices();
    }
    // [ENDSEAM-FREE] window closes here: the battle-end swap point is the last
    // moment any SaveState_Load can reach a pre-902 frame, so a free after it
    // cannot be crossed by a rollback. Telemetry only.
    SeamFreeProbe_CloseWindow();
    // Capture match outcome BEFORE we destroy the session -- reading HP
    // at this point reflects the final state of the just-ended battle.
    // Outcome is from the local player's perspective; the launcher
    // forwards it as a `match_result` to the hub, which correlates
    // both peers' reports for stats. Only meaningful for actual
    // GekkoGameSessions (player vs player); spectate sessions and
    // stress runs skip the publish.
    // C7 -- capture winner / per-side round wins for both the launcher's
    // SharedMem outcome publish AND the SessionEvent MATCH_END payload that
    // ships to subscribers + replay files. Same data, two consumers.
    uint8_t  match_winner_idx  = 2;  // 0=P1, 1=P2, 2=draw / unknown
    uint8_t  match_rounds_p1   = 0;
    uint8_t  match_rounds_p2   = 0;
    if (g_session && g_session_kind == SessionKind::BATTLE) {
        FM2KMatchOutcome outcome = FM2K_MATCH_OUTCOME_NONE;
        if constexpr (FM2K::kIsFM2K) {
            // FM2K: decide by the engine's OWN round-win counters, like the
            // FM95 branch always has. The old HP-only read mislabeled every
            // timeout-won match as a DRAW ("both HP >0 = can't decide") --
            // the engine credits timeout rounds itself (higher HP wins the
            // round), so the counters are authoritative for the MATCH even
            // when the final round ends non-KO. Symptom in the wild AND in
            // harness logs: MATCH_END winner=DRAW rounds=2-0, hub records
            // scoring timeout wins as non-wins. HP stays as the tiebreak
            // for a genuinely level counter state (double-KO final round).
            //
            // Round counters (v0.2.21 probe-verified): per-char-slot field
            // at offset -0x18 from HP. 0 at match start, increments each
            // round the player wins. Hooks.cpp's `g_match_phase` /
            // `g_round_sub_state` labels at the same addresses are
            // misleading -- those are per-slot rounds-won, not phase fields.
            const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;
            const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;
            // Live counters get RESET by the match-over object's update
            // before we run (validated 2026-07-19: raw read 0-0 while the
            // ROUND_END cache correctly held 1-0 on a timeout-decided
            // match). Merge with the cached tally -- max per side, since
            // whichever source saw the deciding round holds the bigger
            // number and neither can overcount.
            uint8_t cr1 = 0, cr2 = 0;
            SpectatorNode_GetCachedRoundsWon(&cr1, &cr2);
            const uint32_t p1_wins = std::max<uint32_t>(
                *(uint32_t*)FM2K::ADDR_P1_ROUNDS_WON, cr1);
            const uint32_t p2_wins = std::max<uint32_t>(
                *(uint32_t*)FM2K::ADDR_P2_ROUNDS_WON, cr2);
            uint8_t widx = 2;
            if      (p1_wins > p2_wins)             widx = 0;
            else if (p2_wins > p1_wins)             widx = 1;
            else if (p1_hp > 0 && p2_hp == 0)       widx = 0;  // level counters: HP tiebreak
            else if (p2_hp > 0 && p1_hp == 0)       widx = 1;
            match_winner_idx = widx;
            outcome = (widx == 2) ? FM2K_MATCH_OUTCOME_DRAW
                    : ((int)widx == g_player_index) ? FM2K_MATCH_OUTCOME_SELF_WON
                                                    : FM2K_MATCH_OUTCOME_PEER_WON;
            match_rounds_p1 = (uint8_t)p1_wins;
            match_rounds_p2 = (uint8_t)p2_wins;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match outcome p1_hp=%u p2_hp=%u wins=%u-%u "
                "outcome=%d winner_idx=%u",
                p1_hp, p2_hp, p1_wins, p2_wins, (int)outcome, (unsigned)widx);
        } else {
            // FM95: round-win-counter-based outcome -- mirrors the
            // game's own decision in obj_match_result_state @ 0x410db0
            // case 4: whoever has more rounds won is the match winner.
            // These counters reset to 0 only at the START of a new
            // match (vs_round_function case 1), so by the time we land
            // here at session-stop they hold the final per-match
            // values. Doesn't depend on g_p_main_object_ptr being
            // valid (the per-object struct may have been torn down by
            // the time we get here on a peer-disconnect path).
            const uint32_t p1_wins = *(uint32_t*)FM2K::ADDR_P1_WIN_COUNTER;
            const uint32_t p2_wins = *(uint32_t*)FM2K::ADDR_P2_WIN_COUNTER;
            if (p1_wins > p2_wins) {
                outcome = (g_player_index == 0)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 0;
            } else if (p2_wins > p1_wins) {
                outcome = (g_player_index == 1)
                            ? FM2K_MATCH_OUTCOME_SELF_WON
                            : FM2K_MATCH_OUTCOME_PEER_WON;
                match_winner_idx = 1;
            } else {
                outcome = FM2K_MATCH_OUTCOME_DRAW;
                match_winner_idx = 2;
            }
            match_rounds_p1 = (uint8_t)p1_wins;
            match_rounds_p2 = (uint8_t)p2_wins;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match outcome p1_wins=%u p2_wins=%u outcome=%d",
                p1_wins, p2_wins, (int)outcome);
        }
        SharedMem_PublishMatchOutcome(outcome);
    }

    // Drain the confirmed-input flush BEFORE tearing the session down.
    // Under loss the confirmed horizon trails the live sim by RTT +
    // recovery (observed ~500 frames at 20% loss); destroying the session
    // strands every pending-confirm ring entry past the horizon, so the
    // recorded .fm2krep and the live spectator stream lose the MATCH TAIL
    // -- spectators froze at the host's last flushed frame and never saw
    // the match end (multi-match journey, 2026-06-11). Keep pumping the
    // battle phase (full event handling: corrections land, flush runs)
    // until the horizon catches the sim or the budget expires. Both peers
    // linger symmetrically -- they just exited the same battle-end swap
    // barrier.
    if (g_session && g_session_kind == SessionKind::BATTLE && !g_stress_mode) {
        const uint64_t drain_deadline = GetTickCount64() + 600;
        uint32_t last_logged = 0;
        while (GetTickCount64() < drain_deadline) {
            const int confirmed = gekko_confirmed_frame(g_session);
            if (g_netplay_frame == 0 ||
                confirmed >= (int)g_netplay_frame - 1) {
                break;  // every advanced frame is flushed
            }
            last_logged = (uint32_t)confirmed;
            Netplay_ProcessBattleInputPhase();
            Sleep(5);
        }
        const int final_confirmed = gekko_confirmed_frame(g_session);
        if (g_netplay_frame > 0 && final_confirmed < (int)g_netplay_frame - 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: EndBattle drain timed out -- confirmed=%d < last "
                "frame %u; stream/replay tail may be short",
                final_confirmed, g_netplay_frame - 1);
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: EndBattle drain complete (confirmed=%d, frames=%u)",
                final_confirmed, g_netplay_frame);
        }
        (void)last_logged;
    }

    if (g_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: Ending GekkoNet session (kind=%d)", (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
    }

    // (Legacy Replay::Replay_EndRecording call retired in v0.2.27 -- the
    // SpectatorNode_WriteCurrentBattleFile call below writes the v2
    // .fm2krep file that supersedes the legacy 96-byte-header format.)

    // Tell the spectator tree the match is over -- subscribers receive
    // MATCH_END and go idle until the next SpectatorNode_OnMatchStart.
    // C7: payload carries winner + per-side rounds + frames_total for
    // self-describing .fm2krep files. frames_total is computed inside
    // SpectatorNode_AppendMatchEnd via session-input-frame delta against
    // the most-recent MATCH_START -- host-side bookkeeping, no caller
    // input needed for that field.
    MatchEndPayload match_end_payload = {};
    match_end_payload.winner_idx     = match_winner_idx;
    match_end_payload.rounds_won_p1  = match_rounds_p1;
    match_end_payload.rounds_won_p2  = match_rounds_p2;
    match_end_payload.frames_total   = 0;  // filled inside Append
    SpectatorNode_OnMatchEnd(match_end_payload);

    // (Removed in v0.2.20: post-MATCH_END SpectatorNode_StashSnapshot. It
    // crashed users on the first 3000→2000 transition -- likely SaveState_Save
    // running its replay-diff scan against torn-down FM2K state after
    // gekko_destroy. CURRENT_MATCH-mode spectator joining mid-CSS still
    // receives the start-of-match snapshot from Netplay_StartBattle's
    // StashSnapshot -- they replay the prior match's frames, which is
    // suboptimal but correct. Phase 6 robustness pass can re-add a
    // between-match cache freshen with a JIT live-peek (no SaveState_Save)
    // path that doesn't trigger the replay-diff scan.)

    // Per-battle .fm2krep -- slice the SessionEvent log between the most
    // recent MATCH_START and the just-appended MATCH_END. Same on-disk
    // shape as .fm2kset (full session); is_battle_slice flag distinguishes.
    //
    // BOTH PLAYERS write their own local replay (host index 0 + guest
    // index 1). round_events.cpp now emits ROUND_START/END on both players,
    // so the guest's file carries round markers + round_offsets too -- the
    // guest is no longer left with no replay at all. The filename is
    // PEER-SUFFIXED (_p0 / _p1): on real netplay the two players are on
    // different machines so there's never a collision, but the local
    // 2-instance test harness has both writing the same replays/ dir in the
    // same wall-clock second -- the suffix stops the later writer clobbering
    // the other's file. Spectators (index 2) record via a separate path.
    if (g_player_index == 0 || g_player_index == 1) {
        char ts[80] = {};
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        char pattern[80];
        std::snprintf(pattern, sizeof(pattern),
            "replays/%%Y-%%m-%%d_%%H%%M%%S_p%d.fm2krep", g_player_index);
        std::strftime(ts, sizeof(ts), pattern, &tm_buf);
        CreateDirectoryA("replays", nullptr);
        SpectatorNode_WriteCurrentBattleFile(ts);

        // Set-file refresh (replay architecture finish, 2026-07-19): the
        // .fm2kset used to be written ONLY at process shutdown -- a crash,
        // a task-manager kill, or the harness's TerminateProcess meant the
        // session bundle never existed, which is why users only ever saw
        // per-game .fm2krep files. Rewrite the full session file after
        // EVERY match under a STABLE per-session name (first write picks
        // it; later matches overwrite the same file), so the set on disk
        // is always complete up to the last finished match. Shutdown's
        // write remains as the final tail flush. Cost: a full serialize of
        // session_events per match end -- a long session is a few MB, once
        // per multi-minute match, during the between-match seam.
        {
            static char s_set_path[128] = {};
            static uint64_t s_set_path_session = 0;
            const uint64_t sid = SpectatorNode_GetSessionId();
            if (s_set_path[0] == '\0' || s_set_path_session != sid) {
                char set_pattern[96];
                std::snprintf(set_pattern, sizeof(set_pattern),
                    "sessions/%%Y-%%m-%%d_%%H%%M%%S_%08x_p%d.fm2kset",
                    (uint32_t)(sid & 0xFFFFFFFFu), g_player_index);
                std::strftime(s_set_path, sizeof(s_set_path), set_pattern, &tm_buf);
                s_set_path_session = sid;
            }
            CreateDirectoryA("sessions", nullptr);
            SpectatorNode_WriteSessionFile(s_set_path);
        }
    }

    // Stop any pending SFX "desired" entries and clear the channel map so
    // the next battle rescans the channel table (handles character-load
    // changes between matches).
    SoundRollback::OnBattleEnd();

    // Match boundary for the rngtrace ring. Emits the one-line count summary
    // always and CLEARS the ring, so the next match starts empty; the ~1.4 MB
    // CSV itself is written only under FM2K_RNGTRACE. Before this, every match
    // end wrote up to 60000 fprintf lines on the game thread, from a ring that
    // was never cleared -- so the tail got longer with every match of a set,
    // on top of the up-to-600ms confirmed-input drain immediately above.
    SaveState_FlushRngTrace(g_player_index, "battle end");

    g_session_ready = false;
    g_simple_state = SimpleState::CONNECTED;

    // Reset CSS state for rematch
    g_css_active = false;
    g_css_synced = false;
    g_local_css_ready = false;
    g_remote_css_ready = false;
    // Stale-advance scrub: pre-rendezvous CSS frames consume
    // Netplay_GetCSSInput before the new session delivers anything. The
    // last advance pair of the PREVIOUS CSS session must not leak into
    // them -- each peer stops consuming its old session at its own flip
    // frame, so the leftovers can differ across peers and seed a CSS
    // divergence before the lockstep stream even starts.
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_frame         = 0;

    // Reset battle sync state for next battle (entry direction). Both gates
    // disarmed: the next CSS rendezvous re-arms BATTLE_ENTERING when the
    // new CSS GekkoSession comes up, and Netplay_StartBattle re-arms
    // BATTLE_END once the next battle session is created. Anything that
    // arrives between now and those points is stale carryover and gets
    // dropped at the handler.
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;
    g_battle_entry_armed      = false;
    // Rematch: the NEXT match must re-agree its settings from scratch (the
    // host may have changed the stage / round count between matches, and the
    // random-stage roll moves 0x43010c every match). Carrying the previous
    // battle's digest here would let a stale agreement satisfy the new gate.
    g_entry_remote_cfg_digest = 0;

    // Reset battle-end sync state -- fresh for the rematch's next return.
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;
    g_battle_end_armed           = false;
}
