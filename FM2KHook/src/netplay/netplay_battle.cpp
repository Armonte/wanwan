// Netplay battle session lifecycle: Netplay_StartBattle / StartStressBattle /
// EndBattle (GekkoNet session create/teardown for rollback). Extracted VERBATIM
// from netplay.cpp; shares state via netplay_internal.h.
// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "control_channel.h"
#include "game_hash.h"
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
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctime>
#include <random>
#include <cstdio>
#include <cstring>
#include <atomic>

// Battle RNG seed. Canonical pin is 0x12345678 (deterministic for tests).
// FM2K_TEST_BATTLE_SEED overrides it so we can drive a DIFFERENT match -- and,
// since stage selection is RNG-driven at the CSS->battle transition, a
// different STAGE -- on host + guest + spectator alike (all read the same env
// and the host broadcasts PIN_RNG to the spectator), to verify they still
// agree. Cached once per process.
uint32_t Netplay_TestBattleSeed() {
    static uint32_t s = 0;
    if (!s) {
        const char* e = std::getenv("FM2K_TEST_BATTLE_SEED");
        s = (e && *e) ? (uint32_t)std::strtoul(e, nullptr, 0) : 0x12345678u;
    }
    return s;
}

// GEKKONET SESSION - Battle Mode (rollback)
// =============================================================================

bool Netplay_StartBattle() {
    // Tear down the CSS lockstep session before standing up the battle
    // session. Sequenced so g_session is null between the two — the
    // multiplex adapter is shared (session_magic demuxes), but only one
    // session is alive at a time.
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    if (g_session) {
        return true;  // Already in some other session (battle or stress).
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Starting battle GekkoSession");

    // Capture per-match character IDs + resolved .player filenames
    // while the CSS-side state is still live. Held in shared mem so
    // the launcher can include them in the hub `match_result` payload
    // after Netplay_EndBattle wipes CSS. Addresses come from
    // FM2K_Integration.h (launcher-side header) — inlined here because
    // the hook DLL has its own minimal FM2K:: namespace in globals.h
    // that doesn't pull in the full Integration header.
    //
    // Roster table at 0x435474 (`g_char_slot_data`): array of 256-byte
    // CP932 (Shift-JIS) filename strings, indexed by the same char_id
    // that lives at P1/P2_CHARACTER_ID_ADDR. Convert CP932 → UTF-8
    // via Win32 before publishing so JP names survive round-tripping
    // through the JSON hub protocol intact.
    {
        // Engine-aware via globals.h: FM2K reads CCCaster-style address pair
        // at 0x470180/4 with the 258-slot g_char_slot_data table at 0x435474;
        // FM95 reads its per-player CSS struct at 0x432720/0x432730 with the
        // 50-slot g_player_file_name_array at 0x463CF0.
        constexpr uintptr_t kP1CharIdAddr  = FM2K::ADDR_P1_SELECTED_CHAR;
        constexpr uintptr_t kP2CharIdAddr  = FM2K::ADDR_P2_SELECTED_CHAR;
        constexpr uintptr_t kCharSlotData  = FM2K::ADDR_CHAR_FILENAME_TABLE;
        constexpr size_t    kSlotStride    = FM2K::CHAR_FILENAME_STRIDE;
        constexpr size_t    kSlotCount     = FM2K::CHAR_FILENAME_COUNT;

        const uint32_t p1_char = *(const uint32_t*)kP1CharIdAddr;
        const uint32_t p2_char = *(const uint32_t*)kP2CharIdAddr;

        auto resolve_name = [&](uint32_t id, char* out, size_t out_cap) {
            out[0] = '\0';
            if (id >= kSlotCount) return;
            const char* cp932 =
                reinterpret_cast<const char*>(kCharSlotData + id * kSlotStride);
            // Bound the read at the slot stride so a missing NUL never
            // walks off into the next slot's data.
            const int len_932 = (int)strnlen(cp932, kSlotStride);
            if (len_932 == 0) return;

            wchar_t wbuf[kSlotStride];
            int wlen = MultiByteToWideChar(932, 0, cp932, len_932,
                                           wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
            if (wlen <= 0) return;
            int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                            out, (int)(out_cap - 1),
                                            nullptr, nullptr);
            if (u8len <= 0) { out[0] = '\0'; return; }
            out[u8len] = '\0';
        };

        char p1_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        char p2_name[FM2K_MATCH_CHAR_NAME_MAX] = {};
        resolve_name(p1_char, p1_name, sizeof(p1_name));
        resolve_name(p2_char, p2_name, sizeof(p2_name));

        SharedMem_PublishMatchChars(p1_char, p2_char, p1_name, p2_name);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: match chars p1=%u(\"%s\") p2=%u(\"%s\")",
            p1_char, p1_name, p2_char, p2_name);
    }

    // Snapshot the actual battle stage_id BEFORE the random-stage block
    // touches ADDR_SELECTED_STAGE. By this point in Netplay_StartBattle
    // vs_round_function has already loaded the stage file (the
    // "<name>.stage" CreateFileA log line a few ms earlier), so the
    // value at ADDR_SELECTED_STAGE right now is the one that's actually
    // playing this match. The random-stage block below writes a fresh
    // value for the *next* match, but that value is what MATCH_START
    // used to record — leading to "battle on stage X, replay says Y"
    // mismatches reported on 2026-05-09. Capture pre-roll, use that
    // for both MATCH_START and SharedMem_PublishMatchStage.
    const uint32_t mstage_id_pre_roll =
        (FM2K::ADDR_SELECTED_STAGE != 0)
            ? *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE
            : 0u;

    // Random stage (#56 — Lilithport-style seeded xorshift). The
    // launcher hands us a host-generated seed via FM2K_STAGE_RANDOM_SEED
    // when both peers agree on random stage. We re-seed once per
    // process from that env var (g_xorshift_seeded), then advance one
    // step per Netplay_StartBattle and write the resulting index to
    // FM2K's stage memory. Both peers run the same xorshift sequence
    // from the same seed, so rematches keep rolling identically with
    // zero per-rematch wire traffic.
    //
    // KNOWN ISSUE: the write here lands AFTER vs_round_function has
    // already loaded the stage file for the current match, so the roll
    // does nothing for THIS battle. Only takes effect if the cached
    // value happens to influence a subsequent stage read — which in
    // current FM2K builds it doesn't. The random-stage feature is
    // slated for replacement by an explicit lobby/game-settings stage
    // selector, so this isn't being fixed in place.
    {
        constexpr uintptr_t kSelectedStageAddr = FM2K::ADDR_SELECTED_STAGE;
        static bool      g_xorshift_seeded = false;
        static uint32_t  g_xs_a = 1812433254u, g_xs_b = 3713160357u,
                         g_xs_c = 3109174145u, g_xs_d = 64984499u;
        static int       g_stage_min = 0;
        static int       g_stage_max = -1;   // -1 = random disabled

        if (!g_xorshift_seeded) {
            g_xorshift_seeded = true;
            const char* seed_env = std::getenv("FM2K_STAGE_RANDOM_SEED");
            const char* min_env  = std::getenv("FM2K_STAGE_RANDOM_MIN");
            const char* max_env  = std::getenv("FM2K_STAGE_RANDOM_MAX");
            // One-shot diagnostic: if the env vars aren't visible to
            // the hook process, random-stage silently does nothing
            // (g_stage_max stays -1) and every match plays on whatever
            // stage_id the CSS cursor happens to be on. Logging both
            // presence + value lets us tell apart "host disabled" vs
            // "env didn't propagate to game process".
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random-stage env: SEED=%s MIN=%s MAX=%s",
                seed_env ? seed_env : "(unset)",
                min_env  ? min_env  : "(unset)",
                max_env  ? max_env  : "(unset)");
            if (seed_env && *seed_env) {
                uint32_t seed = (uint32_t)std::strtoul(seed_env, nullptr, 10);
                if (seed != 0) {
                    // Lilith's seeding scheme — same constants so a
                    // shared seed produces the same a[4] state on both
                    // peers (cross-implementation parity if anyone ever
                    // wants Lilith-FM2K interop, though we don't ship
                    // that today).
                    uint32_t s = seed;
                    uint32_t* arr[4] = {&g_xs_a, &g_xs_b, &g_xs_c, &g_xs_d};
                    for (int i = 0; i < 4; ++i) {
                        s = 1812433253u * (s ^ (s >> 30)) + (uint32_t)i;
                        *arr[i] = s;
                    }
                    g_stage_min = (min_env && *min_env)
                        ? std::atoi(min_env) : 0;
                    g_stage_max = (max_env && *max_env)
                        ? std::atoi(max_env) : -1;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: random-stage seeded (seed=%u min=%d max=%d)",
                        (unsigned)seed, g_stage_min, g_stage_max);
                }
            }
        }

        // Clamp the range to the game's REAL stage list (Patrick,
        // 2026-06-11: "stage range isn't game specific"). The launcher's
        // range is a per-game setting now, but a stale or hand-edited
        // value must still never roll an index past the stage table:
        // LoadStageFile sprintf's the filename from the 256-byte entry
        // and an empty one throws a modal "GameStage Open error" box
        // mid-match. Both peers scan the same table of the same game,
        // so the clamp is identical on both sides and rolls stay
        // deterministic. Scanned once, lazily (battle start = game data
        // long since loaded).
        if constexpr (FM2K::ADDR_STAGE_FILE_TABLE != 0) {
            static int s_stage_count = -1;
            if (s_stage_count < 0 && g_stage_max >= 0) {
                const char* tbl = (const char*)FM2K::ADDR_STAGE_FILE_TABLE;
                int n = 0;
                while (n < 100 && tbl[256 * n] != '\0') ++n;
                s_stage_count = n;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: random-stage: game defines %d stage(s)", n);
            }
            if (g_stage_max >= 0 && s_stage_count >= 0) {
                if (s_stage_count == 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: random-stage disabled -- stage table is "
                        "empty");
                    g_stage_max = -1;
                } else if (g_stage_max >= s_stage_count) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: random-stage range %d..%d exceeds the "
                        "game's %d stages -- clamped",
                        g_stage_min, g_stage_max, s_stage_count);
                    g_stage_max = s_stage_count - 1;
                    if (g_stage_min > g_stage_max) g_stage_min = g_stage_max;
                }
            }
        }

        if (g_stage_max >= g_stage_min && g_stage_max >= 0) {
            // Advance one step. xorshift128 — identical to Lilith's
            // RandomStage() arrival-no-args branch (stdafx.cpp:670).
            uint32_t t  = g_xs_a ^ (g_xs_a << 11);
            g_xs_a = g_xs_b; g_xs_b = g_xs_c; g_xs_c = g_xs_d;
            g_xs_d = (g_xs_d ^ (g_xs_d >> 19)) ^ (t ^ (t >> 8));
            const uint32_t span = (uint32_t)(g_stage_max - g_stage_min + 1);
            const uint32_t roll = g_xs_d % span;
            const uint32_t stage = (uint32_t)g_stage_min + roll;
            *(uint32_t*)kSelectedStageAddr = stage;
            // Also stash for FM95's LoadStageFile_alt hook — vs/story
            // mode reads its stage_id from a per-character table at
            // call time, so writing to ADDR_SELECTED_STAGE alone only
            // covers practice mode. The hook reads g_pending_random_stage
            // and overrides arg0 when non-FFFFFFFF.
            g_pending_random_stage = stage;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: random stage rolled=%u (range %d..%d)",
                stage, g_stage_min, g_stage_max);
        }

        // Stage_id capture for the hub match_result payload. Uses the
        // pre-roll snapshot taken at function entry — that's the stage
        // file vs_round_function already loaded for THIS match. Reading
        // ADDR_SELECTED_STAGE here would pick up whatever the random
        // block just wrote (for the next match), producing a record
        // that doesn't match what players actually saw on screen.
        // FM95 has no documented selected-stage scalar yet
        // (ADDR_SELECTED_STAGE == 0); publish unknown.
        if constexpr (FM2K::ADDR_SELECTED_STAGE != 0) {
            SharedMem_PublishMatchStage(mstage_id_pre_roll);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: match stage_id=%u", mstage_id_pre_roll);
        } else {
            SharedMem_PublishMatchStage(0xFFFFFFFFu);
        }
    }

    // Reset CSS state when entering battle
    g_css_active        = false;
    g_css_synced        = false;
    g_local_css_ready   = false;
    g_remote_css_ready  = false;
    g_css_advance_ready = false;

    // Per-player input delay (#24 -- Melancholy/Spooder, bug bumbler).
    //
    // TWO paths:
    //  * MANUAL (FM2K_LOCAL_DELAY set): the value is authoritative and PURELY
    //    LOCAL -- run verbatim, never pulled up to the peer's. Each player owns
    //    their delay and the two sides may run different values. This is safe:
    //    GekkoNet injects local input as frame+delay and transmits it
    //    frame-tagged, so the peer needs no knowledge of our delay (input.cpp
    //    AddInput(frame + _input_delay, ...); advantage uses GetMinLocalDelay()).
    //    A lower-than-warranted delay just costs more rollback / teleportation
    //    on the chooser's side -- their trade-off, not the opponent's problem.
    //
    //  * AUTO (no override): peers exchange a DELAY_PROPOSAL over the control
    //    channel through CSS (ControlChannel_SendDelayProposal on the ping
    //    cadence) and adopt max(local candidate, remote candidate). max is
    //    commutative, so both AUTO peers land on the SAME smooth delay -- the
    //    right default when neither side expressed a preference. (History: peers
    //    once computed delay independently and drifted apart on jittery links;
    //    the shared-max fixed that. It does NOT override a manual choice.)
    //
    // ControlChannel computes the local candidate from measured RTT per the
    // delay mode (FM2K_DELAY_MODE: avg = mean RTT, peak = worst RTT) at FM2K's
    // 100 Hz, where 10 ms is one frame budget:
    //   candidate = clamp(ceil(one_way_ms / 10), 2, 15)
    //
    // The AUTO value is pinned for the connection lifetime
    // (g_session_delay_cached) so it stays stable across every CSS-> battle
    // transition. A manual override bypasses the cache so it can flip mid-session.
    bool has_manual_delay = false;
    int  manual_delay     = 0;
    if (const char* env = std::getenv("FM2K_LOCAL_DELAY"); env && env[0]) {
        int v = std::atoi(env);
        // No small clamp: a player owns their local delay. The only ceiling is
        // GekkoNet's input ring (DEFAULT_BUFF_SIZE=128) -- stay clear of it.
        if (v >= 0 && v <= MAX_LOCAL_DELAY) { has_manual_delay = true; manual_delay = v; }
    }
    const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
    const uint32_t rtt_worst_ms = ControlChannel_GetWorstRttMs();
    const int local_cand  = ControlChannel_GetLocalDelayCandidate();
    const int remote_cand = ControlChannel_GetRemoteDelayCandidate();

    int local_delay;
    enum DelaySource { DS_MANUAL, DS_COMPUTED, DS_CACHED } delay_source;
    if (has_manual_delay) {
        // The user's explicit local delay is authoritative and PURELY LOCAL --
        // never pulled up to the peer's value. Asymmetric delay is safe:
        // GekkoNet tags each local input with frame+delay and transmits it
        // frame-tagged, so the peer needs no knowledge of our delay. Choosing a
        // lower delay than the link warrants simply means more rollback /
        // teleportation on our side -- the player's own trade-off. Bypasses the
        // negotiation cache so it can be flipped mid-session.
        local_delay  = manual_delay;
        delay_source = DS_MANUAL;
    } else if (g_session_delay_cache_valid) {
        local_delay  = g_session_delay_cached;
        delay_source = DS_CACHED;
    } else {
        // Auto path (no manual override): size from RTT and adopt
        // max(local, remote) so both auto peers land on the same smooth delay.
        // local_cand is -1 only when no RTT has been measured -- fall back to
        // the conservative CSS delay.
        const int lc = (local_cand < 0) ? CSS_LOCAL_DELAY : local_cand;
        local_delay  = (remote_cand > lc) ? remote_cand : lc;
        delay_source = DS_COMPUTED;
        g_session_delay_cached      = local_delay;
        g_session_delay_cache_valid = true;
        // Fresh worst-RTT window for the next match's diagnostics.
        ControlChannel_ResetWorstRttMs();
    }

    // prediction_window: how far GekkoNet will speculatively rewind
    // before stalling. Default 16 at 100 FPS gives ~160 ms wall-clock
    // budget (matches the GekkoNet OnlineSession reference's 10-frame
    // budget at 60 FPS). Free CPU at steady state — only costs work
    // *during* a recovery from a deep rollback, which is rare.
    //
    // FM2K_PREDICTION_WINDOW env override accepts 0..64. Set 8 to
    // bisect a regression against the pre-2026-05-18 default.
    int prediction_window = 16;
    if (const char* env = std::getenv("FM2K_PREDICTION_WINDOW"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 64) prediction_window = v;
    }
    g_pred_window = prediction_window;

    const char* source_str =
        delay_source == DS_MANUAL   ? "manual" :
        delay_source == DS_CACHED   ? "negotiated (cached)" :
                                      "negotiated";
    const char* mode_str =
        ControlChannel_GetDelayMode() == 1 ? "peak" : "avg";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: RTT mean=%ums worst=%ums -> delay candidate "
        "local=%d remote=%d, local_delay=%d (%s, mode=%s), "
        "prediction_window=%d",
        rtt_mean_ms, rtt_worst_ms, local_cand, remote_cand,
        local_delay, source_str, mode_str, prediction_window);

    GekkoConfig config = {};
    config.num_players = 2;
    // Up to 4 spectators per battle session. spectator_delay = 0 disables
    // GekkoNet's spectator pause-buffer (spectator_session.cpp:216-218
    // short-circuits ShouldDelaySpectator). With > 0 the spectator's
    // local sim stays paused until min_received - current >= delay, but
    // current also advances each tick post-unpause — for low-latency
    // LAN/localhost setups the buffer never converges and spectator stays
    // paused forever. Re-enable jitter buffering once we have measured
    // cross-internet ping data.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;
    // Late-joiner backfill — see CSS-session comment + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = prediction_window;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::BATTLE;
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from the actually-learned peer sockaddr.
    // g_remote_addr was written at Netplay_Init from the env var (often stale, e.g.
    // "127.0.0.1:7001" for a host before the client connects). The adapter formats
    // inbound packet addresses from the real sockaddr ("ip:port"), so the string we
    // give gekko_add_actor must match that same formatting or every packet is dropped.
    if (const sockaddr_in* learned = NetSocket_GetRemoteAddr()) {
        if (learned->sin_port != 0) {  // sin_port aliases sin6_port (offset 2)
            // Family-aware canonical actor string (v4 byte-identical to the old
            // form; v6 "[..]:port") -- MUST match the recv stamp.
            std::string actor = NetSocket_GetRemoteActorString();
            snprintf(g_remote_addr, sizeof(g_remote_addr), "%s", actor.c_str());
        }
    }

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, local_delay);
            g_local_delay = local_delay;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added local player at slot %d (delay=%d)", i, local_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    AddSubscribedSpectatorsToSession();

    // Runahead: speculatively advance `runahead` extra frames PAST confirmed
    // input EVERY tick, re-simulating them, to hide the VISUAL lag of input
    // delay. The catch: that's `1 + runahead` full sim passes per wall-clock
    // frame, ALWAYS (not just on rollback). At 100 FPS the per-frame budget
    // is only 10 ms, and a lot of 2DFM players are on weak hardware where
    // even plain rollback strains — runahead on top routinely blows the
    // budget and drops frames. So:
    //
    //   DEFAULT OFF. Runahead is opt-in, not automatic.
    //
    // It's a latency-feel nicety, not a correctness feature — turning it off
    // costs only visible input lag (which the input delay already covers),
    // never desync. F8 toggles it on to `runahead_pref` (= local_delay, the
    // value that exactly cancels the delay's visual lag) for anyone who wants
    // it and has the CPU headroom. FM2K_RUNAHEAD env force-pins 0..15.
    int runahead_pref = local_delay;            // the "on" value F8 enables
    if (runahead_pref < 0)  runahead_pref = 0;
    if (runahead_pref > 15) runahead_pref = 15; // GekkoNet runahead is a u8 cap
    int runahead = 0;                           // DEFAULT OFF (see above)
    if (const char* env = std::getenv("FM2K_RUNAHEAD"); env && env[0]) {
        int v = std::atoi(env);
        if (v >= 0 && v <= 15) {
            runahead = v;
            if (v > 0) runahead_pref = v;       // env-on also sets the F8 target
        }
    }
    g_runahead_user_pref = runahead_pref;
    g_runahead_active.store(runahead, std::memory_order_release);
    gekko_set_runahead(g_session, (unsigned char)runahead);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: prediction_window=%d runahead=%d (DEFAULT OFF; F8 toggles "
        "0<->%d, local_delay=%d)",
        prediction_window, runahead, runahead_pref, local_delay);

    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = Netplay_TestBattleSeed();
    SpectatorNode_AppendPinRng(Netplay_TestBattleSeed());
    SaveState_Init();
    SaveState_DoInitialSync();  // eager pre-AdvEvent reset (was lazy)
    SoundRollback::Init();
    // Battle-start init order matters: PIN_RNG → RESET_INPUT_STATE → SOUND_INIT
    // mirrors what host's local sim is doing this frame. The RESET_INPUT_STATE
    // op corresponds to SaveState_Save's first-call buf_idx + edge state +
    // history-rings reset (savestate.cpp:223-237) which fires when the first
    // GekkoNet AdvanceEvent triggers a Save, not here. We append it now so
    // it lands BEFORE the first INPUT batch of the match — the spectator
    // applies it at the same logical-frame boundary the host clears state at.
    SpectatorNode_AppendResetInputState();
    SpectatorNode_AppendSoundInit();

    // Per-battle replay capture is owned by SpectatorNode (v2 .fm2krep
    // file format). The legacy Replay::Replay_BeginRecording call here was
    // retired in v0.2.27 — its 96-byte ReplayHeader + raw ReplayFrame[]
    // body has been a strict subset of the v2 SessionEvent stream since
    // C6 landed. Initial RNG seed + state hash are captured below for
    // SpectatorNode_OnMatchStart, which inlines them into the
    // MATCH_START event's 96-byte payload (byte-compatible with the old
    // ReplayHeader so the wire schema stays stable).
    const uint32_t initial_seed = Netplay_TestBattleSeed();
    const uint32_t initial_state_hash =
        SaveState_GetRegionChecksums().gameplay_fingerprint;

    // C7 — emit the host's session_id once at the very first match of the
    // connection. Generated lazily on first call: high 32 bits = unix epoch
    // seconds (so files sort chronologically + collisions are bounded to
    // intra-second), low 32 bits = a random nonce for uniqueness even within
    // the same second across machines. Subsequent matches in the same
    // session reuse the cached value via SpectatorNode_GetSessionId().
    if (g_player_index == 0 && SpectatorNode_GetSessionId() == 0) {
        const uint64_t epoch = (uint64_t)std::time(nullptr);
        std::random_device rd;
        const uint64_t nonce = ((uint64_t)rd() << 32) | (uint64_t)rd();
        const uint64_t session_id = (epoch << 32) ^ nonce;
        SpectatorNode_AppendSessionId(session_id);
    }

    // Notify the spectator tree: start of a new match, push INITIAL_MATCH to
    // any currently-subscribed viewers so they reset and follow this match.
    // C6: chars/stage read from the same addresses SharedMem_PublishMatchChars
    // / PublishMatchStage above use. Cast to uint8 — char/stage IDs are well
    // under 256 (FM2K rosters cap at 50, stages at ~50).
    //
    // Color pick: read from each player's char-slot record at slot+0xE00B
    // (ADDR_CHARSLOT0_COLOR_PICK + CHARSLOT_STRIDE * slot). 1v1 VS mode
    // pins slot[0]=P1, slot[1]=P2 (set by AssignPlayerColor's call site
    // in game_state_manager). v0.2.33 and earlier hardcoded 0 here based
    // on a misread that FM2K had no palette select — but the bit-mask
    // ladder in AssignPlayerColor maps attack buttons 1..5 to colors
    // 1..5. With 0 in MATCH_START, css_autoconfirm's per-player target
    // bit always fell back to bit 4 (color 0) on replay/spec, so every
    // playback showed palette 0 regardless of what was originally picked.
    const uint8_t mp1_char = static_cast<uint8_t>(
        *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR);
    const uint8_t mp2_char = static_cast<uint8_t>(
        *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR);
    uint8_t mp1_color = 0;
    uint8_t mp2_color = 0;
    if constexpr (FM2K::ADDR_CHARSLOT0_COLOR_PICK != 0) {
        mp1_color = static_cast<uint8_t>(
            *(const uint32_t*)FM2K::ADDR_CHARSLOT0_COLOR_PICK);
        mp2_color = static_cast<uint8_t>(
            *(const uint32_t*)(FM2K::ADDR_CHARSLOT0_COLOR_PICK + FM2K::CHARSLOT_STRIDE));
    }
    const uint8_t mstage_id = static_cast<uint8_t>(mstage_id_pre_roll);
    SpectatorNode_OnMatchStart(
        /*game_hash*/         0,
        /*initial_rng_seed*/  initial_seed,
        /*initial_state_hash*/initial_state_hash,
        /*p1_char*/mp1_char, /*p1_color*/mp1_color,
        /*p2_char*/mp2_char, /*p2_color*/mp2_color,
        /*stage_id*/mstage_id);

    // C3.5 — reset the intra-match round counter so the first ROUND_START
    // emitted from vs_round_function lands as round_idx=1.
    extern void RoundEvents_OnMatchStart();
    RoundEvents_OnMatchStart();

    // Task #18 phase 2: capture a SaveState snapshot RIGHT NOW so a
    // CURRENT_MATCH-mode spectator joining mid-set can SaveState_Load
    // directly to this match's start instead of replaying every previous
    // battle. Order matters: must run AFTER OnMatchStart (which appends
    // the MATCH_START SessionEvent — we want our snapshot's input_frame
    // anchor to count from after that op) and AFTER the pin/init sites
    // above (snapshot reflects the canonical pristine match-start state).
    SpectatorNode_StashSnapshot();

    // Re-broadcast host config so any setting changes (host clicked a
    // different stage between matches, switched SOCD mode, etc.) propagate
    // to client + spectators before this match's first sim frame.
    Netplay_BroadcastHostConfig();

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;  // monotonic dedup gate, reset per battle
    ResetConfirmRing();
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    // Battle session is up. Disarm BATTLE_ENTERING (we're past the signaling
    // window — any further arrivals are duplicates / late echoes and should
    // be dropped, otherwise the rate-limited echo path keeps both peers in
    // a forever-ping-pong that competes with GekkoNet's own sync handshake).
    // Arm BATTLE_END for the eventual return-to-CSS swap.
    g_battle_entry_armed = false;
    g_battle_end_armed   = true;
    g_end_epoch = NextBarrierEpoch();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: GekkoNet battle session created (runahead=%d, prediction_window=%d)",
        g_runahead_active.load(std::memory_order_acquire), g_pred_window);
    return true;
}

// Single-instance determinism test: GekkoStressSession with both players local
// and no network adapter. GekkoNet rewinds every check_distance frames and
// compares checksums, flagging any sim divergence as a desync event.
bool Netplay_StartStressBattle() {
    if (g_session) {
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Starting GekkoStressSession (single-instance determinism test)");

    GekkoConfig config = {};
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 8;
    config.input_size = sizeof(uint16_t);
    config.state_size = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving = false;
    // StressSession-specific: force a rollback every check_distance frames
    // so GekkoNet re-simulates from a saved state and compares checksums
    // against the original advance. Any mismatch fires GekkoDesyncDetected.
    //
    // 0 = no forced rollback. With check_distance=10 the replay self-test
    // produces a record→replay divergence at frame 764 (5× run-bisected
    // confirmation): host's forward sim records inputs that, replayed
    // through the engine without rollback, diverge from the host's own
    // recorded parity stream. With check_distance=0 the same 1500-frame
    // test passes 100% (all aligned frames identical). Means: SaveState
    // is missing some piece of state that PGI+UG mutates — after rollback
    // Load + re-PGI+UG, the post-re-sim state differs from the original
    // forward state, and subsequent forward advances drift from then on.
    //
    // Production netplay rollbacks are predictive (1-3 frames deep), so
    // the accumulated drift over 76 stress rollback cycles is much
    // larger than what a real game would hit, but the underlying
    // savestate-completeness bug is real and should be fixed. Task #34
    // tracks the hunt; until then check_distance=0 to keep the replay
    // self-test green for shipping. Real-netplay savestate correctness
    // is verified by per-frame checksum compares against the peer when
    // gekko rollback fires for genuine prediction misses.
    //
    // FM2K_CHECK_DISTANCE env overrides for task #34 bisects: e.g.
    // FM2K_CHECK_DISTANCE=10 python3 tools/replay_selftest.py ...
    // (the harness forwards it through the cmd.exe env block).
    config.check_distance = 0;
    if (const char* env = std::getenv("FM2K_CHECK_DISTANCE"); env && env[0]) {
        config.check_distance = std::atoi(env);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay: FM2K_CHECK_DISTANCE override -> %d", config.check_distance);
    }

    // StressSession mode: ignores network, forces rollbacks from a single instance.
    gekko_create(&g_session, GekkoStressSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::STRESS;
    SpectatorNode_ClearGekkoSpectatorTracking();

    // Both actors local. No adapter set -> no network calls.
    //
    // local_delay=0 in stress mode. Real netplay sets delay>=1 to hide
    // network latency, but stress mode has no peer — there's nothing to
    // hide. With delay=1, gekko buffers each tick's added input and
    // delivers it on the FOLLOWING confirmed advance, so host's frame K
    // sim consumes autoplay(K-1). The .fm2krep recorder writes INPUT[K] =
    // the autoplay value added on tick K (one-shot per AdvanceEvent
    // capture, not delay-corrected). Replay reads INPUT[K] sequentially
    // and feeds it to frame K's PGI — so replay's frame K consumes
    // autoplay(K) while host's consumed autoplay(K-1). Same value at the
    // INPUT[K] slot, but the engine sim aligns INPUT[K-1] (host) vs
    // INPUT[K] (replay) into "frame K", producing an off-by-one engine
    // input divergence that surfaces around frame 65 (the first frame
    // where the autoplay pattern emits a non-zero p1 value that the
    // motion-check engine cares about). With delay=0, host's frame K
    // consumes autoplay(K) too — symmetric with replay.
    for (int i = 0; i < 2; i++) {
        gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
        gekko_set_local_delay(g_session, i, 0);
    }
    g_local_delay = 0;

    // Deterministic initial RNG seed (matches Netplay_StartBattle).
    *(uint32_t*)FM2K::ADDR_RANDOM_SEED = Netplay_TestBattleSeed();
    SaveState_Init();
    SaveState_DoInitialSync();  // eager pre-AdvEvent reset
    SoundRollback::Init();
    // Initialize SpectatorNode in offline-record mode so the stress run
    // produces a self-replayable .fm2krep file on auto-terminate. Without
    // this, MATCH_START never reaches session_events and
    // SpectatorNode_WriteCurrentBattleFile returns false.
    SpectatorNode_Init();
    // Emit the same pre-MATCH_START init sequence as real netplay
    // (Netplay_StartBattle): PIN_RNG → RESET_INPUT_STATE → SOUND_INIT.
    // Without these in the recorded slice, the replay-side engine never
    // gets its rng reset to 0x12345678 — it stays at whatever the C
    // runtime / boot sequence left there — and the post-sim rng on
    // frame 0 diverges from the record (~ that's the literal record vs
    // replay rng split the harness diff caught at frame 0).
    SpectatorNode_AppendPinRng(Netplay_TestBattleSeed());
    SpectatorNode_AppendResetInputState();
    SpectatorNode_AppendSoundInit();
    // Emit MATCH_START with zeroed CSS metadata (boot-to-battle bypasses
    // CSS; char/color/stage IDs are whatever was in default memory).
    // The replay-self-test driver only needs the events to be slice-able,
    // not for the metadata to be accurate.
    SpectatorNode_OnMatchStart(
        /*game_hash*/         0,
        /*initial_rng_seed*/  Netplay_TestBattleSeed(),
        /*initial_state_hash*/0,
        /*p1_char*/0, /*p1_color*/0,
        /*p2_char*/0, /*p2_color*/0,
        /*stage_id*/0);

    g_simple_state = SimpleState::BATTLE;
    g_session_ready = true;   // no handshake needed for stress
    g_netplay_frame = 0;
    g_highest_recorded_frame = 0;
    ResetConfirmRing();
    g_rollback_count = 0;
    g_last_rollback_frame = 0;
    g_desync_count = 0;
    g_last_desync_log_tick = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: GekkoStressSession created (check_distance=%d, prediction_window=%d)",
        config.check_distance, config.input_prediction_window);
    return true;
}

