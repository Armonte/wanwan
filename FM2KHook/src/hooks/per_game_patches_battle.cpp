// per_game_patches_battle.cpp -- FM2K AI-field writes (story-init) + boot-to-battle
// dispatcher + PerGamePatches_ApplyRuntime. Split (engine x concern) from
// per_game_patches.cpp; FM2K-only.

#if !defined(ENGINE_FM95)

#include "per_game_patches.h"

#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include "MinHook.h"
#include <safetyhook.hpp>  // SafetyHook MidHook for KOF HP-init patch
#include "round_events.h"  // RoundEvents_KofSnapshot* accessors
#include "combat_state.h"  // combat_state::ShouldGuardP2 for state-driven Guard
#include "../netplay/netplay.h"          // Netplay_IsConnected (BTB handshake hold)
#include "../netplay/control_channel.h"  // ControlChannel_Poll/SendHello
#include "../netplay/game_hash.h"        // fm2k::game_hash::Compute
extern int g_player_index;               // core/globals.h (avoid full include: it
                                         // re-declares original_get_player_input,
                                         // which this TU already forward-declares)
#include "per_game_patches_internal.h"

// =============================================================================
// AI FIELD WRITES — replicate vs_round_function's story-mode RSS_BATTLE_INIT
// =============================================================================
//
// `ai_input_processor` (0x411270) is the engine's per-frame AI driver, called
// from `character_action_controller` for every fighter object every frame. It
// dispatches based on three char_data fields:
//
//   [slot + 0xDF5D]  AI gate — must be non-zero or the whole function bails.
//                    Story init writes 1; meter logic may also write here.
//   [slot + 0xDF61]  AI difficulty (clamped 0..100). Controls how aggressively
//                    the AI picks moves vs. waits. Story init reads it from
//                    `stage_fighter_entry[-2]` per the stage script.
//   [slot + 0xDF65]  AI mode (switch index, dec then 0..3):
//                       1 = full script-driven AI (the normal CPU opponent)
//                       2 = mirror P1's input
//                       4 = force-up input (jump constantly)
//                       other = no-op (chars act on real player input)
//
// `vs_round_function`'s story-mode RSS_BATTLE_INIT branch (flag==0) sets all
// three for every spawned fighter via the loop at 0x408B72:
//     mov dword ptr [esi-4], 1     ; gate enable
//     mov [esi], <stage difficulty>
//     mov dword ptr [esi+4], 1     ; AI mode = full script
//
// The VS-mode branch (flag==1) and team-mode branch (flag==2) skip those
// writes — they only spawn the char objects. Our story-init MidHook at
// 0x411C8F flips `character_state_machine`'s flag-read view to 0, but
// vs_round_function still sees flag=1 (we don't override there because the
// VS round flow — single fight, no stage progression — is what we WANT), so
// the AI-field loop never runs and ai_input_processor's gate stays at 0.
//
// Patch: at the RSS_BATTLE_INIT → RSS_ROUND_ANNOUNCE_WAIT (100 → 110)
// substate transition, write the three fields directly for the AI-driven
// slot(s). One-shot per round entry; the engine's natural HP/meter resets
// between rounds don't touch these offsets so they persist. round_events.
// cpp's Hook_vs_round_function calls PerGamePatches_OnBattleInitComplete
// after orig runs when it detects pre==100 && post!=100.

constexpr uintptr_t CHAR_SLOT0_BASE       = 0x004D1D80;
constexpr uintptr_t CHAR_SLOT_STRIDE      = 0xE03F;     // 57407 bytes per slot
constexpr uintptr_t OFF_AI_GATE           = 0x0000DF5D;
constexpr uintptr_t OFF_AI_DIFFICULTY     = 0x0000DF61;
constexpr uintptr_t OFF_AI_MODE           = 0x0000DF65;
constexpr uint32_t  AI_DEFAULT_DIFFICULTY = 50;  // mid; story uses stage data

void ApplyAiFieldsForSlot(int slot, uint32_t difficulty) {
    const uintptr_t base = CHAR_SLOT0_BASE + (uintptr_t)slot * CHAR_SLOT_STRIDE;
    *(volatile uint32_t*)(base + OFF_AI_GATE)       = 1;
    *(volatile uint32_t*)(base + OFF_AI_DIFFICULTY) = difficulty;
    *(volatile uint32_t*)(base + OFF_AI_MODE)       = 1;
}

// Story-init AI hijack handler — see big comment near the MidHook decl
// (top of file). Fires AFTER `mov eax, g_game_mode_flag` so ctx.eax
// already holds the loaded flag; we override to 0 when a hijack mode
// is active and we're in battle, which makes the trampoline's cmp set
// ZF and the upcoming jz take the story-init / 1P-arcade-AI branch.
void OnCharStateMachineFlagDispatch(SafetyHookContext& ctx) {
    const bool any_hijack =
        g_vs_cpu_mode.load(std::memory_order_relaxed) ||
        g_training_mode.load(std::memory_order_relaxed) ||
        g_cpu_vs_cpu_mode.load(std::memory_order_relaxed);
    if (!any_hijack) return;

    // Only override during battle. CSS (game_mode==2000) still needs
    // flag=1 visible to the cursor-creation dispatch so 2P CSS appears;
    // we already wrote flag=1 to the global at title→CSS edge.
    const uint32_t mode = *(const uint32_t*)ADDR_GAME_MODE;
    if (mode < 3000u || mode >= 4000u) return;

    ctx.eax = 0;
}

bool PerGamePatches_InstallStoryInitHijack() {
    // DISABLED — was counterproductive in practice. Story-init branch in
    // character_state_machine reads `g_stage_script_index[g_css_active_-
    // player]` to derive char_data->something_xor_mask (0xDFB7), which
    // character_facing_controller uses as the opponent bitmask. In our
    // hijacked flow that index is 0/stale, so the resulting xor_mask
    // didn't include P1's slot bit → character_facing_controller failed
    // to find an opponent → P2 faced default direction and AI scripts
    // ran without an opponent reference.
    //
    // Without this MidHook the engine takes the VS-init branch, which
    // writes the correct xor_mask = -1 - (1 << slot_id). The AI fields
    // we WANT are now written directly by ApplyAiFieldsForSlot — gate,
    // difficulty, and mode — invoked every battle frame from round_-
    // events.cpp's Hook_vs_round_function. This separates "tell the
    // engine to run AI for this slot" (direct field writes) from "let
    // the engine think the round is 1P arcade" (story-init hijack);
    // we only need the former.
    //
    // The handler and constants are kept in case we revisit, but the
    // install is a no-op.
    (void)STORY_INIT_HIJACK_SITE;
    (void)&OnCharStateMachineFlagDispatch;
    return true;
}

// Map a training-mode P2 behavior selector value to the ai_input_processor
// switch case it should drive. The engine has built-in cases for our
// Imitate (case 2 — mirror P1's history into P2's) and Jump (case 4 —
// force the UP bit each frame), so we just point ai_mode at them; for
// Player and Guard we keep ai_mode=0 so the input pipeline runs raw
// (and Guard's "force back" is applied by PerGamePatches_BattleInputOverride).
static uint32_t TrainingBehaviorToAiMode(int behavior) {
    switch (behavior) {
        case 0: return 0;  // Player — raw input, no engine override
        case 1: return 1;  // CPU — full script AI
        case 2: return 2;  // Imitate — engine mirrors P1's history to P2
        case 3: return 0;  // Guard — handled by BattleInputOverride (back dir)
        case 4: return 4;  // Jump-up — engine writes UP each frame
        default: return 0;
    }
}

void ApplyAiFieldsForSlotWithMode(int slot, uint32_t difficulty, uint32_t ai_mode) {
    const uintptr_t base = CHAR_SLOT0_BASE + (uintptr_t)slot * CHAR_SLOT_STRIDE;
    *(volatile uint32_t*)(base + OFF_AI_GATE)       = 1;
    *(volatile uint32_t*)(base + OFF_AI_DIFFICULTY) = difficulty;
    *(volatile uint32_t*)(base + OFF_AI_MODE)       = ai_mode;
}

void PerGamePatches_OnBattleInitComplete() {
    // Called every frame from round_events.cpp's Hook_vs_round_function
    // (the round-state object's per-frame tick). The function name is a
    // misnomer — it was originally a one-shot at RSS_BATTLE_INIT → 110
    // but health_damage_manager has a write at 0x40EA63 that clobbers
    // [slot+0xDF65] when the CPU takes damage, and char_state_machine
    // init runs late enough that a one-shot pre-init write gets stomped.
    // Per-frame writes are robust: ~3 dword writes per active AI slot,
    // negligible cost.
    //
    // Slot + ai_mode selection by submode:
    //   vs_cpu_mode      → P2 ai_mode=1 (full script AI)
    //   cpu_vs_cpu_mode  → P1+P2 ai_mode=1 (both AI-driven; P1 still gets
    //                      VS-init "player char" markers but ai_input_-
    //                      processor's case-1 doesn't care about slot id)
    //   training_mode    → P2 ai_mode mapped from g_training_p2_behavior
    //                      via TrainingBehaviorToAiMode; F2 cycles update
    //                      the atomic and the next frame reflects it.
    const bool vs_cpu   = g_vs_cpu_mode.load(std::memory_order_relaxed);
    const bool cpu_cpu  = g_cpu_vs_cpu_mode.load(std::memory_order_relaxed);
    const bool training = g_training_mode.load(std::memory_order_relaxed);

    if (!vs_cpu && !cpu_cpu && !training) return;

    // Determine P2's desired ai_mode. 0 means "don't drive the slot via
    // the engine" — input override handles it (Player/Guard).
    uint32_t p2_ai_mode = 0;
    if (vs_cpu || cpu_cpu) {
        p2_ai_mode = 1;
    } else if (training) {
        const int beh = g_training_p2_behavior.load(std::memory_order_relaxed);
        p2_ai_mode = TrainingBehaviorToAiMode(beh);
    }

    // P1 gets case-1 AI only in cpu_vs_cpu.
    if (cpu_cpu) {
        ApplyAiFieldsForSlotWithMode(0, AI_DEFAULT_DIFFICULTY, 1);
    }

    if (p2_ai_mode != 0) {
        // Engine drives P2: write gate + difficulty + mode every frame.
        ApplyAiFieldsForSlotWithMode(1, AI_DEFAULT_DIFFICULTY, p2_ai_mode);
    } else {
        // Player / Guard: input pipeline + BattleInputOverride drive P2.
        //
        // We MUST clear BOTH the AI gate (0xDF5D) AND the mode (0xDF65) —
        // not just the mode. The gate field doubles as a discriminator in
        // hit_detection_system @ 0x40F3DA:
        //
        //     if (*(target_char_data + 0xDF5D)) {          // AI gate
        //         if (game_rand() % 100 >=
        //               *(target_char_data + 0xDF61))      // AI difficulty
        //             goto LABEL_71;  // hit lands (RNG-fail to block)
        //     }
        //
        // Leaving gate=1 (residual from a prior CPU/Imitate/Jump phase)
        // makes the engine RNG-block at difficulty% instead of running
        // the actual input-based block check. With difficulty=50 that's
        // a 50/50 block rate — exactly the symptom that motivated this
        // patch. Clearing the gate forces the engine to fall through to
        // the section-4 block path (input-driven, deterministic).
        //
        // Trade-off: this clobbers char_data+0xDF5D every frame. If that
        // field is also used as P2's super meter, P2 loses meter
        // accumulation during Player/Guard mode. Per the combat-state
        // analysis (docs/analysis/fm2k_combat_state.md section A.6) the
        // true super meter is at +0xDF55 (`pre_super_meter`); 0xDF5D is
        // the AI-gate dedicated field. Confirmed via vs_round_function
        // story-init writing a literal `1` here, not a meter ramp value.
        const uintptr_t slot1_base = CHAR_SLOT0_BASE + CHAR_SLOT_STRIDE;
        *(volatile uint32_t*)(slot1_base + OFF_AI_GATE) = 0;
        *(volatile uint32_t*)(slot1_base + OFF_AI_MODE) = 0;
    }
}

// =============================================================================
// TEAM SIZE OVERRIDE + ENV VAR INTAKE
// =============================================================================

void PerGamePatches_ApplyRuntime() {
    // ---- Team size override (FM2K_TEAM_SIZE) ----
    // Stash the value as an atomic; css_autoconfirm.cpp's game_state_manager
    // detour writes it to g_team_round_setting (0x470064) every frame
    // AFTER the engine's natural copy from g_team_round (0x430128). We
    // can't write at hook init because hit_judge_set_function runs later
    // in boot and re-loads the INI default into g_team_round, blowing
    // away any direct write we did.
    //
    // Range clamp [2, 4]: the engine's CSS state machine indexes its
    // character-data pool as `4 * player_idx + round_count` (hardcoded
    // *4 stride in game_state_manager @ 0x406fc0), and the pool itself
    // is exactly 8 statically-allocated 57407-byte slots based at
    // g_character_data_base @ 0x4d1d80. P1 fills slots 0..N-1, P2 fills
    // slots 4..4+N-1 — so N>4 collides P1 into P2's slot 4. The
    // g_team_char_objects portrait array @ 0x424e80 is similarly only
    // 8 dwords before bumping into g_team_cursor_array @ 0x424e90.
    if (const char* e = std::getenv("FM2K_TEAM_SIZE"); e && *e) {
        int n = std::atoi(e);
        if (n >= 2 && n <= 4) {
            g_team_size_override.store(n, std::memory_order_relaxed);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: team size override armed → %d "
                "(applied per-frame in game_state_manager detour)", n);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: FM2K_TEAM_SIZE=%s out of range [2,4] — ignored", e);
        }
    }

    // ---- Mode toggles ----
    auto on = [](const char* env_var) -> bool {
        const char* e = std::getenv(env_var);
        return e && std::strcmp(e, "1") == 0;
    };

    g_vs_cpu_mode.store(on("FM2K_VS_CPU_MODE"), std::memory_order_relaxed);
    g_cpu_vs_cpu_mode.store(on("FM2K_CPU_VS_CPU_MODE"), std::memory_order_relaxed);
    g_training_mode.store(on("FM2K_TRAINING_MODE"), std::memory_order_relaxed);
    g_option_mode_selector.store(on("FM2K_OPTION_MODE_SELECTOR"),
                                 std::memory_order_relaxed);

    if (g_vs_cpu_mode.load() || g_cpu_vs_cpu_mode.load() ||
        g_training_mode.load() || g_option_mode_selector.load()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: modes — vs_cpu=%d cpu_vs_cpu=%d training=%d "
            "option_selector=%d",
            (int)g_vs_cpu_mode.load(),
            (int)g_cpu_vs_cpu_mode.load(),
            (int)g_training_mode.load(),
            (int)g_option_mode_selector.load());
    }
}

// =============================================================================
// BOOT-TO-BATTLE PRIME — populate g_iniFile_nameOverride for /F path
// =============================================================================
//
// The engine has a built-in dev shortcut: pass /F on the command line and
// WinMain sets g_debug_mode=3. The slot-0 boot dispatcher (Initialize-
// GameFromCommandLine @ 0x409a60) then skips splash/title/CSS and fires
// `create_game_object(14, 127, 0, 0)` straight into battle.
//
// The catch: with g_debug_mode set, the dispatcher SKIPS the cmdline
// parser and reads the kgt filename from g_iniFile_nameOverride @
// 0x43012c. The FM2K editor populates that global via shared state
// before invoking `KGT2nd_GAME.exe /F`; shipped games don't.
//
// We can't just write the global from DllMain because hit_judge_set_-
// function (called by InitializeMainWindow during boot, AFTER our DllMain
// but BEFORE the slot-0 dispatcher fires) reads the kgt's [File].Filename
// from the editor INI (which doesn't exist for shipped games) and writes
// the default (empty string) into the same global — clobbering our prime.
//
// So we hook the boot dispatcher itself and write the global at entry,
// after the stomp, before the original body reads it. Single hook fires
// exactly once on the first main-loop tick.

constexpr uintptr_t ADDR_INIT_FROM_CMD       = 0x00409A60;
constexpr uintptr_t ADDR_INI_NAME_OVERRIDE   = 0x0043012C;
constexpr size_t    INI_NAME_BUF_SIZE        = 256;

typedef int (__cdecl *InitGameFromCmdFn)();
InitGameFromCmdFn g_orig_init_game_from_cmd = nullptr;

// Derive "<exe_basename>.kgt" from GetModuleFileNameA(NULL) and write it
// to g_iniFile_nameOverride. Returns true on success. Game-agnostic; any
// FM2K title whose .kgt sits next to the .exe (the standard shipped
// layout — WonderfulWorld, vanpri, etc.) works.
bool WriteKgtNameToOverride() {
    char exe_path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (n == 0 || n >= sizeof(exe_path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: WriteKgtName — GetModuleFileNameA failed (%lu)",
            GetLastError());
        return false;
    }

    const char* basename = exe_path;
    for (const char* p = exe_path; *p; ++p) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    char kgt_name[INI_NAME_BUF_SIZE] = {};
    std::strncpy(kgt_name, basename, sizeof(kgt_name) - 1);
    if (char* dot = std::strrchr(kgt_name, '.')) *dot = '\0';
    if (std::strlen(kgt_name) + 4 >= sizeof(kgt_name)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: WriteKgtName — basename too long ('%s')",
            basename);
        return false;
    }
    std::strcat(kgt_name, ".kgt");

    auto* dst = reinterpret_cast<char*>(ADDR_INI_NAME_OVERRIDE);
    std::memset(dst, 0, INI_NAME_BUF_SIZE);
    std::memcpy(dst, kgt_name, std::strlen(kgt_name));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: WriteKgtName — '%s' → g_iniFile_nameOverride "
        "(0x%08lX)", kgt_name, (unsigned long)ADDR_INI_NAME_OVERRIDE);
    return true;
}

// Runtime BTB overrides — preferred over FM2K_BTB_* env vars when set.
// Spec hook populates these from SPEC_JOIN_ACK so the slot-0 dispatcher
// loads the host's actual chars instead of the launcher's placeholder.
// 0xFF sentinel = "not set, fall through to env var".
static uint8_t g_runtime_btb_p1_char = 0xFF;
static uint8_t g_runtime_btb_p2_char = 0xFF;
static uint8_t g_runtime_btb_stage   = 0xFF;
uint8_t g_runtime_btb_p1_color = 0xFF;
static volatile bool g_btb_natural_boot_abort = false;
uint8_t g_runtime_btb_p2_color = 0xFF;

void PerGamePatches_AbortBtbNaturalBoot() {
    g_btb_natural_boot_abort = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: BTB abort -> natural boot (host pre-battle; "
        "viewer will replay from frame 0)");
}

void PerGamePatches_SetRuntimeBtbOverrides(uint8_t p1_char,
                                           uint8_t p2_char,
                                           uint8_t stage,
                                           uint8_t p1_color,
                                           uint8_t p2_color) {
    g_runtime_btb_p1_char  = p1_char;
    g_runtime_btb_p2_char  = p2_char;
    g_runtime_btb_stage    = stage;
    g_runtime_btb_p1_color = p1_color;
    g_runtime_btb_p2_color = p2_color;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: runtime BTB overrides set (p1=%u/c%u p2=%u/c%u stage=%u)",
        (unsigned)p1_char, (unsigned)p1_color,
        (unsigned)p2_char, (unsigned)p2_color, (unsigned)stage);
}

// Char / stage / meter overrides applied at the same hook entry point.
// These globals are populated by hit_judge_set_function from kgt INI
// defaults (all 0 for shipped games with no editor INI). We re-stamp
// them here, after hit_judge_set_function and before the slot-0
// dispatcher reads them.
//
//   g_config_value1 @ 0x4300e0 — P1 char grid index (Player0Nb)
//   g_config_value2 @ 0x4300e4 — P1 super-meter init (Player0Cpu)
//   g_config_value3 @ 0x4300f0 — P2 char grid index (Player1Nb)
//   g_config_value4 @ 0x4300f4 — P2 super-meter init (Player1Cpu)
//   wParam         @ 0x43010c — stage index (StageNb); also written
//                                to g_fm2k_game_mode @ 0x470040 by
//                                the original dispatcher
//
// Env vars: FM2K_BTB_P1_CHAR / _P2_CHAR / _STAGE / _P1_METER / _P2_METER.
// Unset env var means no override (engine keeps the kgt-default value).
// Char/stage range-checked [0, 49] to match engine's grid-slot bound;
// out-of-range values are ignored. Meter values pass through unchecked.
void ApplyBootToBattleStateOverrides() {
    auto get_env_int = [](const char* name, int& out) -> bool {
        const char* e = std::getenv(name);
        if (!e || !*e) return false;
        out = std::atoi(e);
        return true;
    };

    int v;
    // Runtime override wins over env var — see PerGamePatches_SetRuntimeBtbOverrides.
    if (g_runtime_btb_p1_char != 0xFF && g_runtime_btb_p1_char < 50) {
        *(uint32_t*)0x004300E0 = (uint32_t)g_runtime_btb_p1_char;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 char → %u (runtime override)",
            (unsigned)g_runtime_btb_p1_char);
    } else if (get_env_int("FM2K_BTB_P1_CHAR", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x004300E0 = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 char → %d", v);
    }
    if (g_runtime_btb_p2_char != 0xFF && g_runtime_btb_p2_char < 50) {
        *(uint32_t*)0x004300F0 = (uint32_t)g_runtime_btb_p2_char;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 char → %u (runtime override)",
            (unsigned)g_runtime_btb_p2_char);
    } else if (get_env_int("FM2K_BTB_P2_CHAR", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x004300F0 = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 char → %d", v);
    }
    if (g_runtime_btb_stage != 0xFF && g_runtime_btb_stage < 50) {
        *(uint32_t*)0x0043010C = (uint32_t)g_runtime_btb_stage;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB stage → %u (runtime override)",
            (unsigned)g_runtime_btb_stage);
    } else if (get_env_int("FM2K_BTB_STAGE", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x0043010C = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB stage → %d", v);
    }
    if (get_env_int("FM2K_BTB_P1_METER", v)) {
        *(uint32_t*)0x004300E4 = (uint32_t)v;  // g_config_value2
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 meter init → %d", v);
    }
    if (get_env_int("FM2K_BTB_P2_METER", v)) {
        *(uint32_t*)0x004300F4 = (uint32_t)v;  // g_config_value4
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 meter init → %d", v);
    }

    // One-shot stage-table dump (FM2K_DUMP_STAGES=1) -- prints index -> filename
    // for every stage so a profiler knows which FM2K_BTB_STAGE index is the
    // light stage (Grid) vs a heavy one (Gurish/Baraga/...). g_stage_file_buffer
    // is 256-byte filename entries at 0x43A29C on FM2K; LoadStageFile sprintf's
    // the name from [256*idx]. Stops at the first empty entry.
    {
        static bool s_dumped = false;
        const char* de = std::getenv("FM2K_DUMP_STAGES");
        if (!s_dumped && de && de[0] == '1') {
            s_dumped = true;
            constexpr uintptr_t kStageNameBuf = 0x43A29Cu;
            constexpr size_t    kStageStride  = 256u;
            for (int i = 0; i < 50; ++i) {
                const char* name = (const char*)(kStageNameBuf + (size_t)i * kStageStride);
                if (name[0] == '\0') break;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[STAGE-TABLE] idx=%d name=\"%.255s\"", i, name);
            }
        }
    }
}

// MinHook detour for InitializeGameFromCommandLine. Restamps the kgt
// name into g_iniFile_nameOverride and applies char/stage/meter
// overrides at function entry, then calls original. The detour body is
// fall-through fast; only fires on the first main-loop tick (the
// function gates itself on g_object_data_ptr[338] and runs exactly
// once per process).
int __cdecl Hook_InitializeGameFromCommandLine() {
    // Spectator BTB hold (Phase F): a viewer's /F boot must load the
    // HOST'S character files, which arrive via the JOIN_ACK runtime
    // overrides -- but this dispatcher fires within ~1s of boot, long
    // before any ACK can land when the host is still at CSS (its first
    // ACK says kind=CSS with no chars; the battle-entry re-broadcast
    // carries the real ones). Booting early loaded default chars and the
    // later snapshot applied onto the wrong .player data (join-during-CSS
    // = ryu/ryu garbage, 2026-06-11).
    //
    // The hold must wait IN PLACE: the engine does NOT reliably re-call
    // this function -- an early-return hold left the game at mode 0
    // (black screen, queue piling up) when the ACK missed the boot
    // window. We pump the control channel ourselves so the JOIN_ACK can
    // arrive and seed the overrides while we wait.
    {
        static int s_is_spec = -1;
        if (s_is_spec < 0) {
            const char* v = std::getenv("FM2K_SPECTATOR_MODE");
            s_is_spec = (v && v[0] == '1') ? 1 : 0;
            // Offline replay (FM2K_REPLAY_FILE): no host, no JOIN_ACK,
            // ever -- the hold below would burn its full 8s timeout
            // pumping a control channel that doesn't exist, then drop /F
            // into a natural title walk: ~113 extra walked frames in the
            // parity capture and a divergent battle-start state (A5 soak
            // red, 2026-06-11 17:33). The harness's FM2K_BOOT_TO_BATTLE
            // IS the battle authority here; engage /F immediately.
            if (s_is_spec == 1) {
                const char* rp = std::getenv("FM2K_REPLAY_FILE");
                if (rp && rp[0]) s_is_spec = 0;
            }
        }
        if (s_is_spec == 1 && g_runtime_btb_p1_char == 0xFF) {
            extern void ControlChannel_Poll();
            const uint64_t start = GetTickCount64();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: holding /F dispatch in place -- pumping "
                "control channel until JOIN_ACK seeds battle chars");
            uint64_t last_log = start;
            extern void SpectatorNode_KickJoin();
            for (;;) {
                SpectatorNode_KickJoin();  // hold may have beaten the
                                           // init path's JOIN_REQ send
                ControlChannel_Poll();   // delivers JOIN_ACK -> overrides
                if (g_btb_natural_boot_abort) {
                    *(uint32_t*)0x424744 = 0;  // g_debug_mode: drop /F
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch aborted after %llums "
                        "-- natural boot (host pre-battle)",
                        (unsigned long long)(GetTickCount64() - start));
                    break;
                }
                if (g_runtime_btb_p1_char != 0xFF) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch released after %llums "
                        "(battle chars seeded)",
                        (unsigned long long)(GetTickCount64() - start));
                    break;
                }
                const uint64_t now = GetTickCount64();
                if (now - start >= 4000) {
                    // task #58: no JOIN_ACK on the direct path in 4s -- if
                    // the launcher armed a relay fallback, engage it HERE,
                    // inside the hold (the trampoline's own 4s trigger
                    // can't run yet -- it only ticks after this hold
                    // releases; observed live: fallback fired at +12s,
                    // AFTER the 8s natural-boot default had already
                    // committed char 0/0). One-shot, no-op without the
                    // relay env, so direct joins are unaffected.
                    extern bool SpectatorNode_EngageRelayFallback();
                    SpectatorNode_EngageRelayFallback();
                }
                if (now - start >= 8000) {
                    // Natural boot is the DEFAULT; /F is the exception
                    // that only the host's explicit "battle in progress"
                    // answer (kind=2 ACK seeding the chars) may engage.
                    // The old fallthrough booted /F with env-default
                    // chars (ryu/ryu garbage) whenever the ACK was slow
                    // or lost -- a spectator joining players at CSS must
                    // never battle-boot.
                    *(uint32_t*)0x424744 = 0;  // g_debug_mode: drop /F
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch hold timed out (8s) "
                        "-- defaulting to NATURAL boot (no ACK = no "
                        "battle authority)");
                    break;
                }
                if (now - last_log > 2000) {
                    last_log = now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: still holding /F dispatch (%llums)",
                        (unsigned long long)(now - start));
                }
                Sleep(10);
            }
        }
    }

    // Player-netplay BTB hold: boot-to-battle skips the title/CSS ticks that
    // normally resend HELLO until the peer connects (RunNativeTick /
    // RunCssTick). Without that, the two peers fire a single HELLO each at
    // init -- and whichever launches first sends into a dead port (the other
    // process hasn't bound its socket yet) and never retries, so the handshake
    // never completes and both wedge at battle entry ">>> waiting for sync"
    // (black screen). Hold the /F dispatch here, pumping the control channel
    // and resending HELLO, until both peers complete the handshake. Then they
    // enter battle already CONNECTED, the game_mode edge in hooks.cpp can
    // signal BATTLE_ENTERING, and Netplay_ArmBattleEntryBarrier lets them sync.
    // Gated to real netplay players (not offline / stress / spectator) so it
    // never burns the timeout when there is no peer.
    {
        static int s_netplay_player = -1;
        if (s_netplay_player < 0) {
            auto on = [](const char* n){ const char* v = std::getenv(n);
                                         return v && v[0] == '1' && v[1] == '\0'; };
            s_netplay_player = (!on("FM2K_TRUE_OFFLINE") && !on("FM2K_STRESS_MODE") &&
                                !on("FM2K_SPECTATOR_MODE")) ? 1 : 0;
        }
        if (s_netplay_player == 1 && !Netplay_IsConnected()) {
            const uint64_t start = GetTickCount64();
            uint64_t last_hello = 0, last_log = start;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: holding /F dispatch -- pumping HELLO until peer connects");
            for (;;) {
                ControlChannel_Poll();
                const uint64_t now = GetTickCount64();
                if (Netplay_IsConnected()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch released after %llums (peer connected)",
                        (unsigned long long)(now - start));
                    break;
                }
                if (now - last_hello >= 250) {
                    ControlChannel_SendHello((uint8_t)g_player_index,
                                             fm2k::game_hash::Compute());
                    last_hello = now;
                }
                if (now - start >= 15000) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F handshake hold timed out (15s) -- "
                        "booting to battle unconnected (peer missing?)");
                    break;
                }
                if (now - last_log > 2000) {
                    last_log = now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: still holding /F dispatch for handshake (%llums)",
                        (unsigned long long)(now - start));
                }
                Sleep(5);
            }
        }
    }

    WriteKgtNameToOverride();
    ApplyBootToBattleStateOverrides();
    int result = g_orig_init_game_from_cmd ? g_orig_init_game_from_cmd() : 0;

    // Stamp the host's real confirm-button colors over the /F hardcodes
    // (original sets slot0=0 / slot1=1 at 0x409CB7/0x409CBD; the type-14
    // battle-init object consumes them on a later tick, and
    // character_state_machine reads them live during battle). Button
    // choice IS the color in this engine -- /F never presses one.
    {
        constexpr uintptr_t kSlotColor0 = 0x4DFD8Bu;
        constexpr size_t    kSlotStride = 0xE03Fu;
        if (g_runtime_btb_p1_color != 0xFF && g_runtime_btb_p1_color < 8) {
            *(int32_t*)(kSlotColor0 + 0 * kSlotStride) = g_runtime_btb_p1_color;
        }
        if (g_runtime_btb_p2_color != 0xFF && g_runtime_btb_p2_color < 8) {
            *(int32_t*)(kSlotColor0 + 1 * kSlotStride) = g_runtime_btb_p2_color;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: BTB colors stamped (p1=c%u p2=c%u)",
                (unsigned)g_runtime_btb_p1_color,
                (unsigned)g_runtime_btb_p2_color);
        }
    }

    // /F leaves g_debug_mode (0x424744) == 3 for the entire process
    // lifetime. The engine's LoadGameSystemFile (called from inside
    // the original we just returned from) sprintfs a "%s -テストプレイ-"
    // suffix into the window title and probes for ".t" variants of
    // every file (kgt, stage, player) — both are editor-only artifacts
    // that bleed into the spectator view. Zero the flag now so any
    // FUTURE engine code that branches on debug_mode takes the
    // production path (per-round LoadGameSystemFile, etc.). The one-
    // shot title leak from this initial call is overwritten within
    // 500ms by the launcher's BBBR-style stats updater.
    //
    // Note: tempted to also SetWindowTextA here to skip the 500ms
    // flash, but the engine's hWnd global address differs across FM2K
    // games (wanwan IDA shows 0x4246F8, pkmncc may differ) and writing
    // through a wrong-pointer-cast HWND can corrupt heap and cause
    // delayed crashes inside the engine's battle init. Trust the
    // launcher's title updater instead.
    *(uint32_t*)0x424744 = 0;

    return result;
}

bool PerGamePatches_InstallBootToBattleHook() {
    const char* e = std::getenv("FM2K_BOOT_TO_BATTLE");
    if (!e || std::strcmp(e, "1") != 0) return true;  // disabled — no-op

    if (MH_CreateHook((void*)ADDR_INIT_FROM_CMD,
                      (void*)Hook_InitializeGameFromCommandLine,
                      (void**)&g_orig_init_game_from_cmd) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_CreateHook(InitGameFromCmd) failed");
        return false;
    }
    if (MH_QueueEnableHook((void*)ADDR_INIT_FROM_CMD) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_QueueEnableHook(InitGameFromCmd) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: /F boot-to-battle hook installed @ 0x%08lX "
        "(re-stamps g_iniFile_nameOverride at slot-0 dispatcher entry)",
        (unsigned long)ADDR_INIT_FROM_CMD);
    return true;
}


#endif  // !ENGINE_FM95

