// Random stage: seeded xorshift roll, CONSUMED AT THE STAGE-LOAD SEAM.
//
// WHY THIS LIVES AT THE LOAD SEAM AND NOT AT BATTLE START
// -------------------------------------------------------
// The old implementation rolled inside Netplay_StartBattle and poked the
// result into ADDR_SELECTED_STAGE (0x43010c). IDA/wwdecomp showed why that
// was always one match late: vs_round_function case 0/1 does
//
//     g_active_stage_id = g_selected_stage;      // 0x470040 = 0x43010c
//     LoadStageFile(g_selected_stage);           // reads 0x43010c HERE
//
// once per match at match init, and 0x43010c is otherwise written only by
// the cmdline / config / settings dialog -- never per match. Netplay_StartBattle
// runs AFTER that read (the ".stage" CreateFileA lands a few ms earlier), so
// the write landed in time for the NEXT match, not the current one. Net effect:
// match 1 of a session played the config stage and every later match played the
// roll made one match earlier. It looked like it worked because rematches did
// change stage.
//
// Consuming at the load seam instead fixes the ordering AND makes offline work
// for free: the hook is a plain game-function trampoline with no dependency on
// Netplay_StartBattle, which is unreachable when g_offline_mode is set.
//
// DETERMINISM
// -----------
// Both peers must advance the xorshift the same number of times. IDA xrefs
// confirm LoadStageFile @ 0x4041E0 has exactly three callers, all inside
// vs_round_function: two pass g_selected_stage (the VS and Team match-init
// paths, mutually exclusive, once per match) and one passes a per-character
// story-table value. Hook_LoadStageFile gates on g_game_mode_flag being VS(1)
// or Team(2), so the story call can never consume a roll. Rollback re-sim is
// excluded via g_is_rolling_back -- a re-simmed call passes through and reuses
// the stage already sitting in 0x43010c, so it stays idempotent.

#include <cstdint>
#include <cstdlib>
#include "../core/globals.h"
#include <SDL3/SDL_log.h>

namespace {

bool     s_seeded    = false;
uint32_t s_xs_a = 1812433254u, s_xs_b = 3713160357u,
         s_xs_c = 3109174145u, s_xs_d = 64984499u;
int      s_stage_min = 0;
int      s_stage_max = -1;      // -1 = random disabled
int      s_stage_count = -1;    // lazy scan of the game's real stage table

void SeedOnce() {
    if (s_seeded) return;
    s_seeded = true;

    const char* seed_env = std::getenv("FM2K_STAGE_RANDOM_SEED");
    const char* min_env  = std::getenv("FM2K_STAGE_RANDOM_MIN");
    const char* max_env  = std::getenv("FM2K_STAGE_RANDOM_MAX");
    // One-shot diagnostic: if the env vars aren't visible to the hook process,
    // random-stage silently does nothing (s_stage_max stays -1). Logging both
    // presence and value tells apart "host disabled it" from "env didn't
    // propagate into the game process".
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RandomStage: env SEED=%s MIN=%s MAX=%s",
        seed_env ? seed_env : "(unset)",
        min_env  ? min_env  : "(unset)",
        max_env  ? max_env  : "(unset)");
    if (!seed_env || !*seed_env) return;

    const uint32_t seed = (uint32_t)std::strtoul(seed_env, nullptr, 10);
    if (seed == 0) return;

    // Lilith's seeding scheme -- same constants so a shared seed produces the
    // same a[4] state on both peers.
    uint32_t s = seed;
    uint32_t* arr[4] = {&s_xs_a, &s_xs_b, &s_xs_c, &s_xs_d};
    for (int i = 0; i < 4; ++i) {
        s = 1812433253u * (s ^ (s >> 30)) + (uint32_t)i;
        *arr[i] = s;
    }
    s_stage_min = (min_env && *min_env) ? std::atoi(min_env) : 0;
    s_stage_max = (max_env && *max_env) ? std::atoi(max_env) : -1;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RandomStage: seeded (seed=%u min=%d max=%d)",
        (unsigned)seed, s_stage_min, s_stage_max);
}

// Clamp the range to the game's REAL stage list (Patrick, 2026-06-11: "stage
// range isn't game specific"). A stale or hand-edited range must never roll
// past the stage table: LoadStageFile sprintf's the filename from the 256-byte
// entry and an empty one throws a modal "GameStage Open error" box mid-match.
// Both peers scan the same table of the same game, so the clamp is identical
// on both sides and rolls stay deterministic.
void ClampToStageTableOnce() {
    if constexpr (FM2K::ADDR_STAGE_FILE_TABLE != 0) {
        if (s_stage_count < 0 && s_stage_max >= 0) {
            const char* tbl = (const char*)FM2K::ADDR_STAGE_FILE_TABLE;
            int n = 0;
            while (n < 100 && tbl[256 * n] != '\0') ++n;
            s_stage_count = n;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "RandomStage: game defines %d stage(s)", n);
        }
        if (s_stage_max >= 0 && s_stage_count >= 0) {
            if (s_stage_count == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "RandomStage: disabled -- stage table is empty");
                s_stage_max = -1;
            } else if (s_stage_max >= s_stage_count) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "RandomStage: range %d..%d exceeds the game's %d stages -- clamped",
                    s_stage_min, s_stage_max, s_stage_count);
                s_stage_max = s_stage_count - 1;
                if (s_stage_min > s_stage_max) s_stage_min = s_stage_max;
            }
        }
    }
}

}  // namespace

// Advance one xorshift128 step and return the rolled stage index, or
// 0xFFFFFFFF when random-stage is off / unusable. EVERY call consumes a roll,
// so callers must already have decided this is a real once-per-match stage
// load (see Hook_LoadStageFile's gating).
extern "C" uint32_t RandomStage_ConsumeForLoad() {
    SeedOnce();
    ClampToStageTableOnce();
    if (s_stage_max < 0 || s_stage_max < s_stage_min) return 0xFFFFFFFFu;

    const uint32_t t = s_xs_a ^ (s_xs_a << 11);
    s_xs_a = s_xs_b; s_xs_b = s_xs_c; s_xs_c = s_xs_d;
    s_xs_d = (s_xs_d ^ (s_xs_d >> 19)) ^ (t ^ (t >> 8));

    const uint32_t span  = (uint32_t)(s_stage_max - s_stage_min + 1);
    const uint32_t stage = (uint32_t)s_stage_min + (s_xs_d % span);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RandomStage: rolled=%u (range %d..%d)", stage, s_stage_min, s_stage_max);
    return stage;
}

// True when a seed arrived and the range is usable -- lets callers skip work
// without consuming a roll.
extern "C" bool RandomStage_IsEnabled() {
    SeedOnce();
    ClampToStageTableOnce();
    return s_stage_max >= 0 && s_stage_max >= s_stage_min;
}
