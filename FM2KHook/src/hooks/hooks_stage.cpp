// hooks_stage.cpp -- FM2K LoadStageFile seam: replay-recorded stage, random stage, the reload guard, and the
// battle-entry barrier's agreed-stage reload. Split from hooks_vfs.cpp (installed there by InstallVfsHooks).
#include "hooks.h"
#include "hooks_internal.h"
#include "globals.h"
#include "netplay.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <SDL3/SDL_log.h>

extern "C" uint32_t RandomStage_ConsumeForLoad();   // advances the roll
extern "C" bool     RandomStage_IsEnabled();
extern "C" uint32_t SpectatorNode_ReplayStageForLoad();  // replay files: this match's stage

// FM2K random-stage seam. vs_round_function reads g_selected_stage (0x43010c)
// at match init and immediately hands it to LoadStageFile; overriding HERE is
// what makes the roll apply to THIS match instead of the next one, and it is
// reachable offline (the old roll site, Netplay_StartBattle, is not).
int (__cdecl* original_LoadStageFile)(int) = nullptr;

// TEST-ONLY lever FM2K_TEST_STAGE_SKEW=N (dark by default, acts on the GUEST only): the guest loads stage N at battle
// init whatever the host picked. That is what pkmncc v17 does in the wild (2026-11-09..12, ~10 sessions): each peer's
// g_selected_stage moves on its own right before the load, the host's pick reaches the guest only in the battle-entry
// HOST_CONFIG, and the guest had already loaded its own stage -> DESYNC f=1. Red/green for the barrier reload below.
static int StageSkewLever() {
    static int s_v = -2;
    if (s_v == -2) {
        const char* v = std::getenv("FM2K_TEST_STAGE_SKEW");
        s_v = (v && v[0]) ? std::atoi(v) : -1;
        if (s_v >= 0)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[STAGE-SKEW] TEST LEVER ARMED -- the guest loads stage %d at "
                        "battle init regardless of the host", s_v);
    }
    return s_v;
}

static int LoadStageCached(int stage_idx);

int __cdecl Hook_LoadStageFile(int stage_idx) {
    if constexpr (!FM2K::kIsFM95 && FM2K::ADDR_LOAD_STAGE_FILE != 0) {
        // Spectators and replay playback take their stage from MATCH_START
        // metadata (spec_playback.cpp) -- both run with g_spectator_mode set.
        // Rolling locally here would make the viewer/replay show a different
        // stage than the match did.
        //
        // Rollback re-sim must not consume a roll either: 0x43010c already
        // holds the applied stage, so passing through keeps re-sim idempotent.
        //
        // g_game_mode_flag gates out the story-mode call site (0x40882C),
        // which passes a per-character table value, not g_selected_stage.
        const uint32_t mode_flag = *(const uint32_t*)FM2K::ADDR_GAME_MODE_FLAG;
        const bool vs_or_team = (mode_flag == 1u || mode_flag == 2u);
        // Replay file: the recorded stage of the match being loaded (its
        // MATCH_START arrives after this load for match 2+ of a session).
        if (g_spectator_mode && !g_is_rolling_back && vs_or_team) {
            const uint32_t rec = SpectatorNode_ReplayStageForLoad();
            if (rec != 0xFFFFFFFFu && (int)rec != stage_idx) {
                *(uint32_t*)FM2K::ADDR_SELECTED_STAGE  = rec;
                *(uint32_t*)FM2K::ADDR_ACTIVE_STAGE_ID = rec;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Replay: LoadStageFile %d -> recorded stage %u", stage_idx, rec);
                stage_idx = (int)rec;
            }
        }
        if (g_player_index == 1 && !g_spectator_mode && !g_is_rolling_back && vs_or_team && StageSkewLever() >= 0) {
            const int skew = StageSkewLever();
            *(uint32_t*)FM2K::ADDR_SELECTED_STAGE  = (uint32_t)skew;
            *(uint32_t*)FM2K::ADDR_ACTIVE_STAGE_ID = (uint32_t)skew;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[STAGE-SKEW] LoadStageFile %d -> %d", stage_idx, skew);
            stage_idx = skew;
        }
        if (!g_spectator_mode && !g_is_rolling_back && vs_or_team &&
            RandomStage_IsEnabled()) {
            const uint32_t rolled = RandomStage_ConsumeForLoad();
            if (rolled != 0xFFFFFFFFu) {
                // The dispatcher already copied the OLD g_selected_stage into
                // g_active_stage_id one line above this call, so both have to
                // be re-stamped or the metadata/render disagree with the file
                // actually loaded.
                *(uint32_t*)FM2K::ADDR_SELECTED_STAGE  = rolled;
                *(uint32_t*)FM2K::ADDR_ACTIVE_STAGE_ID = rolled;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "RandomStage: LoadStageFile override %d -> %u (mode_flag=%u)",
                    stage_idx, rolled, (unsigned)mode_flag);
                stage_idx = (int)rolled;
            }
        }
    }
    return LoadStageCached(stage_idx);
}

// The load itself, behind the reload guard. Also the battle-entry barrier's reload (Hooks_ReloadAgreedStage).
static int LoadStageCached(int stage_idx) {
    // THE RELOAD GUARD. stage_file_loader has no equivalent of the early
    // return player_data_file_loader does: it unconditionally frees the
    // stage slot and then re-reads and re-parses the whole file at EVERY
    // battle init, same stage or not. Measured 2026-09-04, this is the
    // single worst frame of a session -- 226 to 461 ms of synchronous work
    // on the frame thread, every match. Only ~95 ms of that is the file
    // read; the rest is the per-record GlobalAlloc plus ReadFile parse
    // loop inside character_data_loader.
    //
    // WHY SKIPPING IS SAFE, traced rather than assumed, because the danger
    // is keeping buffers alive that something else frees:
    //   - ClearStageSlot (0x4041C0) has exactly TWO callers: this loader,
    //     and ShutdownAndFreeAllResources (0x4059F0).
    //   - ShutdownAndFreeAllResources has exactly ONE caller, WinMain at
    //     0x405EEE, i.e. process exit.
    //   So between matches nothing frees the stage slot, and a resident
    //   stage stays valid.
    //   - The slot lives at 0x445740 and is NOT inside any savestate
    //     region (game state 0x470020, input tracking 0x447EE0, object
    //     pool 0x4701E0, char slots 0x4D1D90). If the simulation mutated
    //     it, rollback would already be corrupting it, so it is read-only
    //     asset data and re-parsing produces the same bytes.
    //
    // The cache key includes 0x424744 because it selects ".stage.t" over
    // ".stage" -- the same index with that flag flipped is a DIFFERENT
    // file, and that flag is written from the window proc and the settings
    // dialog among others, so it can change under us.
    static int      s_cached_idx     = -1;
    static uint32_t s_cached_variant = 0xFFFFFFFFu;
    const uint32_t  variant = *(const uint32_t*)0x424744;

    if (stage_idx == s_cached_idx && variant == s_cached_variant) {
        // Already resident. The loader's own success state (its status
        // words at slot+8736 and slot+8740) is exactly what a previous
        // successful load left behind, so there is nothing to restamp.
        return 0;
    }

    // TEST-ONLY: stand in for a slow disk. A South American player's log on
    // 2026-09-05 shows this exact call taking ~4 s ("CreateFileA
    // 'Levelup Cave.stage'" at 22.378, next line at 26.352, with
    // "main pump stalled 3165ms" alongside). The frame thread is blocked for
    // the whole of it, so nothing is pumped, the peer sees dead air, and
    // GekkoNet's 5 s disconnect timeout ends a match that was perfectly
    // healthy. Reproducing that needed a stall we can place on demand,
    // because the real one only happens on hardware we do not have.
    if (const char* stall = std::getenv("FM2K_TEST_STAGE_LOAD_STALL_MS")) {
        const int ms = std::atoi(stall);
        if (ms > 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "TEST: stalling the frame thread %d ms inside the stage load",
                ms);
            Sleep((DWORD)ms);
        }
    }
    const int rc = original_LoadStageFile ? original_LoadStageFile(stage_idx) : 0;
    if (rc == 0) {
        s_cached_idx     = stage_idx;
        s_cached_variant = variant;
    } else {
        // A failed load leaves the slot cleared. Never cache that.
        s_cached_idx     = -1;
        s_cached_variant = 0xFFFFFFFFu;
    }
    return rc;
}

// Battle-entry barrier repair (netplay_barriers.cpp, FM2K_STAGE_BARRIER_RELOAD): this peer loaded a stage at battle
// init that is not the one both peers agreed on. vs_round_function's battle-init step does exactly
// `g_active_stage_id = g_selected_stage; LoadStageFile(g_selected_stage); player_data_file_loader x2; g_game_mode =
// 3000` (IDA 0x408756..0x4088B1) and nothing else reads the stage before the barrier releases, so redoing those two
// stage writes here, before the first battle frame, is the same state the engine would have built from the agreed id.
extern "C" int Hooks_ReloadAgreedStage(int stage_idx) {
    *(uint32_t*)FM2K::ADDR_SELECTED_STAGE  = (uint32_t)stage_idx;
    *(uint32_t*)FM2K::ADDR_ACTIVE_STAGE_ID = (uint32_t)stage_idx;
    return LoadStageCached(stage_idx);
}

