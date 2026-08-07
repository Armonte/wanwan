// Character-state-machine per-frame diagnostic logger. SafetyHook MidHook
// placed at 0x412564 (the dispatch-loop entry inside character_state_machine).
// Dumps obj state fields outside the parity-recorder's 92-byte snap so we
// can compare host vs replay at the script-divergence frame.
//
// Activated by env FM2K_CSM_DIAG=1. Off by default -- adds a per-frame log
// line which is noisy in production builds.
//
// Layout (from IDA on WonderfulWorld_ver_0946):
//   esi = g_object_data_ptr (current object being state-machined)
//   obj+0x14  : flags byte tested at 0x412238 (sprite-flip bit)
//   obj+0x2C  : item_idx (current position in script -- captured by parity)
//   obj+0x30  : script_idx (script slot -- captured by parity)
//   obj+0x38  : queued attack-state-change (eax read at 0x412327)
//   obj+0x3C  : per-frame dispatch counter (caps iteration to 0x12C)
//   obj+0x40  : freeze counter (captured by parity as "hitstop")
//   obj+0x5C  : facing (captured)
//   obj+0x151 : sprite-related byte tested at 0x412271
//   obj+0x152 : state_progression (3-state init/run/skip)
//   obj+0x15A : action_state (= CharStateMachineKind 0..5)
//   obj+0x15E : state_flags (captured)
//
// All offsets are EXCLUDED from the parity recorder's per-player snap
// except where noted "(captured)". The dispatcher reads several of them
// before deciding "should I iterate the script this frame?" so any
// drift between host and replay would manifest as different script
// progression with everything-else-matching parity -- which is exactly
// what we observed at frame 64 of the replay_selftest run.
#include "../core/globals.h"

#include <safetyhook.hpp>
#include <SDL3/SDL_log.h>
#include <cstdlib>
#include <cstdio>

namespace {

constexpr uintptr_t CSM_DISPATCH_LOOP_ENTRY = 0x412564;
// Per-opcode iteration trace: hook at the opcode-read site INSIDE the
// dispatch loop. csm_pre_dispatch_loop_entry runs once per call; this
// hook fires once per opcode iteration so we can see the exact sequence
// of opcodes processed at the divergent frame.
constexpr uintptr_t CSM_PER_OPCODE_SITE     = 0x4125FC;
SafetyHookMid g_csm_diag_hook{};
SafetyHookMid g_csm_per_opcode_hook{};
bool g_csm_diag_enabled = false;
FILE* g_csm_diag_fp = nullptr;
uint32_t g_csm_diag_frame = 0;  // approximate, derived from buf_idx

// Per-opcode trace. Hook fires at 0x4125FC (`mov al, [edi]`) where edi
// already holds the current script-item pointer (= item_idx * 16 +
// script_data_base). Logs opcode byte + raw item bytes so we can see
// exactly what the engine reads at each iteration of the dispatch loop.
// Gate on battle phase + p1 slot only -- otherwise this fires once per
// opcode per object per frame which is hundreds of lines/frame.
void OnCsmPerOpcode(SafetyHookContext& ctx) {
    if (!g_csm_diag_fp) return;
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode < 3000u || game_mode >= 4000u) return;
    // ctx.esi is the object_data_ptr; only log if p1 (slot 0 + PLAYER).
    const uint8_t* obj = (const uint8_t*)ctx.esi;
    const uint32_t slot_id = *(uint32_t*)(obj + 0x156);
    const uint32_t f15A    = *(uint32_t*)(obj + 0x15A);
    if (slot_id != 0 || f15A != 0) return;

    const uint32_t buf_idx = *(uint32_t*)0x447EE0;
    const uint32_t item_idx = *(uint32_t*)(obj + 0x2C);
    const uint32_t script_idx = *(uint32_t*)(obj + 0x30);
    const uint32_t f3C = *(uint32_t*)(obj + 0x3C);
    const uint8_t* edi = (const uint8_t*)ctx.edi;
    // CheckMotionInputCommand reads:
    //   - obj+0x5C (facing raw, 4 bytes)
    //   - g_charslot0_props_field732_10 (0x4D9A36, slot_stride 0xE03F) -- & 8 = facing-lock flag
    //   - dword_4DFCD1 (0x4DFCD1, slot_stride 0xE03F) -- current facing direction
    //   - input_history at buf_idx and backward (0x4280E0 for p1, 0x4290E0 p2)
    // Capturing these so we can see which path the motion check takes
    // and what input bytes it sees per side at the divergent opcode.
    const uint32_t obj_facing_raw = *(uint32_t*)(obj + 0x5C);
    const uint32_t slot_dyn_base = 0x4D1D80 + slot_id * 0xE03F;
    // props_field732_10 is at struct offset 0x7CB6 (not 0x7CA6 -- was off by 16).
    // Bit 3 (0x08) = facing-locked. When set, character_facing_controller
    // SKIPS the input_history rewrite path. When clear, facing decisions
    // can XOR-flip bits 0|1 of g_p1_input_history[buf+slot<<10].
    const uint8_t facing_lock = *(const uint8_t*)(slot_dyn_base + 0x7CB6);
    // dword_4DFCD1[slot] = cached facing direction (0 = left, 1 = right).
    // Address 0x4DFCD1; offset from char_dynamic base = 0xDF51.
    // Rewritten by character_facing_controller when facing flips this frame.
    const uint32_t facing_cached = *(const uint32_t*)(slot_dyn_base + 0xDF51);
    const uint32_t buf_masked = buf_idx & 0x3FF;
    const uint32_t hist_cur   = *(uint32_t*)(0x4280E0 + slot_id * 0x1000 + buf_masked * 4);
    const uint32_t hist_prev1 = *(uint32_t*)(0x4280E0 + slot_id * 0x1000 + ((buf_masked - 1) & 0x3FF) * 4);
    const uint32_t hist_prev2 = *(uint32_t*)(0x4280E0 + slot_id * 0x1000 + ((buf_masked - 2) & 0x3FF) * 4);

    std::fprintf(g_csm_diag_fp,
        "[CSM-OP] buf=%u item=%u script=%u f3C=%d "
        "opcode=0x%02X bytes=%02X %02X %02X %02X %02X %02X %02X %02X "
        "facing=%u facing_lock=0x%02X facing_cached=%u "
        "hist[buf]=0x%03X hist[buf-1]=0x%03X hist[buf-2]=0x%03X\n",
        buf_idx, item_idx, script_idx, (int32_t)f3C,
        edi[0], edi[1], edi[2], edi[3], edi[4], edi[5], edi[6], edi[7],
        obj_facing_raw, facing_lock, facing_cached,
        hist_cur, hist_prev1, hist_prev2);
}

void OnCsmDispatchEntry(SafetyHookContext& ctx) {
    if (!g_csm_diag_fp) return;
    // esi is reloaded from g_object_data_ptr right before our hook site
    // (see disasm at 0x411BF9: mov esi, g_object_data_ptr). ctx.esi reflects
    // that load. Read all the not-in-parity obj fields plus buf_idx as a
    // per-frame anchor so we can align the log with parity captures.
    const uint8_t* obj = (const uint8_t*)ctx.esi;
    const uint32_t buf_idx = *(uint32_t*)0x447EE0;
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // Only log during battle; CSS dispatch fires here too and floods output.
    if (game_mode < 3000u || game_mode >= 4000u) return;

    const uint32_t f14   = *(uint32_t*)(obj + 0x14);
    const uint32_t f2C   = *(uint32_t*)(obj + 0x2C);  // item_idx
    const uint32_t f30   = *(uint32_t*)(obj + 0x30);  // script_idx
    const uint32_t f38   = *(uint32_t*)(obj + 0x38);
    const uint32_t f3C   = *(uint32_t*)(obj + 0x3C);
    const uint32_t f40   = *(uint32_t*)(obj + 0x40);
    const uint8_t  f151  = *(obj + 0x151);
    const uint32_t f152  = *(uint32_t*)(obj + 0x152);
    const uint32_t f15A  = *(uint32_t*)(obj + 0x15A);
    const uint32_t f15E  = *(uint32_t*)(obj + 0x15E);
    // obj+0x156 selects which char_data slot this object belongs to
    // (multiplied by 0xE03F + g_character_data_base). 0 = p1 slot, 1 = p2.
    const uint32_t slot_id = *(uint32_t*)(obj + 0x156);
    // Game-speed-derived multipliers (suspected divergence source).
    // 0x445704 = g_delay_frame_multiplier (op_PIC scalar) -- should be 100
    //            on both sides per FixGameSpeedDesync() patch.
    // 0x430104 = uValue (Editor.TestPlay.GameSpeed setting, 1..20 typ).
    // 0x445700 = g_char_velocity_multiplier (derived).
    const uint32_t delay_mult = *(uint32_t*)0x445704;
    const uint32_t uval       = *(uint32_t*)0x430104;
    const uint32_t vel_mult   = *(uint32_t*)0x445700;

    std::fprintf(g_csm_diag_fp,
        "[CSM] buf=%u obj=0x%08X slot=%u "
        "f14=0x%08X f2C(item)=%u f30(script)=%u "
        "f38=0x%08X f3C=%d f40=%d "
        "f151=0x%02X f152=%u f15A=%u f15E=0x%08X "
        "delay_mult=%u uval=%u vel_mult=%u\n",
        buf_idx, (unsigned)ctx.esi, slot_id,
        f14, f2C, f30, f38, (int32_t)f3C, (int32_t)f40,
        f151, f152, f15A, f15E,
        delay_mult, uval, vel_mult);
    // Flush every 200 lines (~25 frames worth) so a process kill loses
    // at most a fraction of a second of log.
    static uint32_t lines_since_flush = 0;
    if (++lines_since_flush >= 200) {
        std::fflush(g_csm_diag_fp);
        lines_since_flush = 0;
    }
}

// --------------------------------------------------------------------------
// Camera-input diagnostic (task #34 forced-rollback drift). MidHook at
// 0x40B022 -- the start of camera_manager's two-player battle path (case 13,
// g_game_mode_flag 1..2) right before it reads g_charslot0/1_pos and
// g_screen_x/y to compute the new camera. Logs the exact operands per tick
// so record-vs-replay diffs show WHICH input to the camera formula differs
// at the divergent frame (parity post-frame snapshots all matched, so the
// difference must be mid-frame / camera-object-internal).
// Activated by env FM2K_CAM_DIAG=1; writes logs/FM2K_cam_diag.log.
constexpr uintptr_t CAM_CASE13_SITE = 0x40B022;
SafetyHookMid g_cam_diag_hook{};
FILE* g_cam_diag_fp = nullptr;

void OnCamCase13(SafetyHookContext& ctx) {
    if (!g_cam_diag_fp) return;
    const uint32_t buf_idx = *(uint32_t*)0x447EE0;
    const int32_t c0x = *(int32_t*)0x4DFCC1;
    const int32_t c0y = *(int32_t*)0x4DFCC5;
    const int32_t c1x = *(int32_t*)0x4EDD00;
    const int32_t c1y = *(int32_t*)0x4EDD04;
    const int32_t sx  = *(int32_t*)0x447F2C;
    const int32_t sy  = *(int32_t*)0x447F30;
    // esi = camera object ptr; log its slot idx + state fields so we can
    // tell if the camera OBJECT's internal state diverged.
    const uint8_t* obj = (const uint8_t*)ctx.esi;
    const int slot = (int)(((uintptr_t)obj - 0x4701E0u) / 382u);
    const int32_t o8  = *(const int32_t*)(obj + 8);
    const int32_t o12 = *(const int32_t*)(obj + 12);
    const int32_t a342 = *(const int32_t*)(obj + 0x156);
    // Inputs the sim consumed THIS frame: history ring at the current
    // buf_idx (PGI wrote them before UG; camera ticks inside UG). The
    // parity recorder's input fields read post-update buf_idx and are a
    // known display artifact -- these are the authoritative values.
    const uint32_t in_p1 = *(const uint32_t*)(0x4280E0u + (buf_idx & 0x3FFu) * 4u);
    const uint32_t in_p2 = *(const uint32_t*)(0x4290E0u + (buf_idx & 0x3FFu) * 4u);
    std::fprintf(g_cam_diag_fp,
        "[CAM] buf=%u scr=(%d,%d) c0=(%d,%d) c1=(%d,%d) slot=%d o8=%d o12=%d a342=%d in1=0x%03X in2=0x%03X\n",
        buf_idx, sx, sy, c0x, c0y, c1x, c1y, slot, o8, o12, a342, in_p1, in_p2);
    static uint32_t cam_lines_since_flush = 0;
    if (++cam_lines_since_flush >= 100) {
        std::fflush(g_cam_diag_fp);
        cam_lines_since_flush = 0;
    }
}

}  // namespace

void Hook_InstallCamDiag() {
    const char* env = std::getenv("FM2K_CAM_DIAG");
    if (!(env && env[0] == '1')) return;
    // pid-suffixed so the record and replay phases (same game dir, run
    // back-to-back) don't clobber each other's log.
    char base[64];
    std::snprintf(base, sizeof(base), "FM2K_cam_diag_pid%lu.log",
                  (unsigned long)GetCurrentProcessId());
    char path[MAX_PATH] = {};
    if (Fm2k_BuildLogPath(path, sizeof(path), base) == 0) {
        std::snprintf(path, sizeof(path), "%s", base);
    }
    g_cam_diag_fp = std::fopen(path, "w");
    if (!g_cam_diag_fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CamDiag: failed to open %s for write", path);
        return;
    }
    std::setvbuf(g_cam_diag_fp, nullptr, _IOFBF, 1 << 16);
    g_cam_diag_hook = safetyhook::create_mid((void*)CAM_CASE13_SITE, OnCamCase13);
    if (!g_cam_diag_hook) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CamDiag: safetyhook::create_mid @ 0x%X failed",
            (unsigned)CAM_CASE13_SITE);
        std::fclose(g_cam_diag_fp);
        g_cam_diag_fp = nullptr;
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CamDiag: MidHook installed @ 0x%X -- log: %s",
        (unsigned)CAM_CASE13_SITE, path);
}

void Hook_InstallCsmDiag() {
    const char* env = std::getenv("FM2K_CSM_DIAG");
    g_csm_diag_enabled = (env && env[0] == '1');
    if (!g_csm_diag_enabled) return;

    // Open a side file (so it's separate from the main Debug.log). The
    // logs/ dir is the game's working dir.
    char path[MAX_PATH] = {};
    if (Fm2k_BuildLogPath(path, sizeof(path), "FM2K_csm_diag.log") == 0) {
        std::snprintf(path, sizeof(path), "FM2K_csm_diag.log");
    }
    g_csm_diag_fp = std::fopen(path, "w");
    if (!g_csm_diag_fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CsmDiag: failed to open %s for write", path);
        g_csm_diag_enabled = false;
        return;
    }
    std::setvbuf(g_csm_diag_fp, nullptr, _IOFBF, 1 << 16);

    g_csm_diag_hook = safetyhook::create_mid(
        (void*)CSM_DISPATCH_LOOP_ENTRY, OnCsmDispatchEntry);
    if (!g_csm_diag_hook) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CsmDiag: safetyhook::create_mid @ 0x%X failed",
            (unsigned)CSM_DISPATCH_LOOP_ENTRY);
        std::fclose(g_csm_diag_fp);
        g_csm_diag_fp = nullptr;
        g_csm_diag_enabled = false;
        return;
    }
    // Per-opcode hook -- gated to fire on the actual mov al,[edi]
    // instruction inside the loop body. Each opcode iteration logs one
    // line with the script-item bytes and current item_idx.
    g_csm_per_opcode_hook = safetyhook::create_mid(
        (void*)CSM_PER_OPCODE_SITE, OnCsmPerOpcode);
    if (!g_csm_per_opcode_hook) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "CsmDiag: per-opcode MidHook @ 0x%X failed (continuing without)",
            (unsigned)CSM_PER_OPCODE_SITE);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CsmDiag: per-opcode MidHook installed @ 0x%X",
            (unsigned)CSM_PER_OPCODE_SITE);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CsmDiag: MidHook installed @ 0x%X -- log: %s",
        (unsigned)CSM_DISPATCH_LOOP_ENTRY, path);
}

void Hook_FlushCsmDiag() {
    if (g_csm_diag_fp) std::fflush(g_csm_diag_fp);
}
