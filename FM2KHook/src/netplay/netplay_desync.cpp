// netplay_desync.cpp -- HandleDesyncDetected: the common real+synthetic
// desync handler (diagnostic dump -> RNG flush -> ZIP bundle -> upload
// manifest -> TerminateProcess). Split from netplay.cpp; declared in
// netplay_internal.h. Shares file-scope state via that header.
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "control_channel.h"
#include "game_hash.h"
#include "input.h"
#include "savestate.h"
#include "seam_trace.h"   // Phase 1bc seam instruments (diagnostic)
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

// Common handler for both real (GekkoDesyncDetected) and synthetic
// (FM2K_FORCE_DESYNC_AT_FRAME) desync events. Same diagnostic dump,
// same upload manifest, same TerminateProcess -- the synthetic path
// exercises the full end-to-end pipeline (Dump → RNG flush → ZIP
// bundle → manifest → launcher upload → server pairing) so we can
// validate fixes without waiting for a real-world determinism leak.
//
// `synthetic` only affects log wording -- the file-write + terminate
// path is identical.
void HandleDesyncDetected(int frame, uint32_t local_chk,
                                 uint32_t remote_chk, bool synthetic) {
    // Phantom-checksum guard (2026-06-11 16:31, battle-2 f=1536): gekko's
    // SendSessionHealthCheck reads _storage.GetState(confirmed) and only
    // ASSERTS the slot actually holds that frame -- compiled out in
    // release, so a wrapped/unwritten slot transmits checksum 0 and the
    // peer kills itself on a phantom mismatch (P2 saw remote=0x00000000
    // while P1's own comparator stayed silent, i.e. the real checksums
    // matched). Our save callback always writes a nonzero fingerprint,
    // so a zero on either side is never a genuine state hash; a real
    // divergence keeps firing with nonzero pairs on subsequent frames.
    if (!synthetic && (local_chk == 0 || remote_chk == 0)) {
        static uint32_t s_phantom_count = 0;
        if (s_phantom_count++ < 8 || (s_phantom_count & 0x3F) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DESYNC ignored (phantom #%u): f=%d local=0x%08X "
                "remote=0x%08X -- zero checksum is a gekko health-slot "
                "artifact, not a state hash",
                s_phantom_count, frame, local_chk, remote_chk);
        }
        return;
    }
    g_desync_count++;
    uint32_t now_tick = GetTickCount();

    // Always log the first desync with full detail.
    if (g_desync_count <= 5) {
        auto& rc = SaveState_GetRegionChecksums();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "%sDESYNC #%u f=%d: local=0x%08X remote=0x%08X",
            synthetic ? "SYNTHETIC " : "",
            g_desync_count, frame, local_chk, remote_chk);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  SAVED: rng=0x%08X game=0x%08X obj=0x%08X char=0x%08X inp=0x%08X",
            rc.rng, rc.game_state, rc.object_pool, rc.char_dynamic,
            rc.input_tracking);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  UNSAVED: eff1=0x%08X eff2=0x%08X shake=0x%08X",
            rc.effect_sys1, rc.effect_sys2, rc.shake_effects);
        // NOTE: this line used to claim "HP/pos/rng/timer". There are NO
        // POSITION FIELDS in the fingerprint -- SaveState_CalculateFingerprint
        // hashes exactly rng, p1_hp, p2_hp, round_timer(0x470060),
        // game_timer(0x470044), p1_input, p2_input. Anyone reasoning "positions
        // are covered so a position desync would have tripped this" was
        // reasoning from a comment that did not match the code (GDC tooling
        // gap #1).
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "  FINGERPRINT: gameplay=0x%08X (rng/HP/round+game timer/current "
            "input ONLY -- no positions, no object pool -- if this MATCHES "
            "across peers the desync is a memory-residue false positive)",
            rc.gameplay_fingerprint);
    } else if (now_tick - g_last_desync_log_tick > 1000) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DESYNC #%u f=%d: local=0x%08X remote=0x%08X",
            g_desync_count, frame, local_chk, remote_chk);
        g_last_desync_log_tick = now_tick;
    }

    // First desync only: dump diagnostics, enqueue upload, terminate.
    if (g_desync_count != 1) return;

    SaveState_DumpDesyncDiagnostic(frame, local_chk, remote_chk,
                                   g_player_index);
    // force=true: this is the forensic the ring exists for, and the process
    // is about to terminate, so the CSV cost is not a mid-match hitch.
    SaveState_FlushRngTrace(g_player_index, "first desync", /*force=*/true);
    // Phase 1bc (DIAGNOSTIC): [SEAM] episode list + per-save fingerprint ring.
    // Always emits its one-line counts summary; writes the CSV only when the
    // ring was armed (FM2K_SEAM_TRACE=1) or a crossing actually happened.
    SeamTrace_Dump(g_player_index, "first desync");

    // Escape hatch: FM2K_NO_DESYNC_KILL=1 keeps the game running for
    // diagnostic sessions. Off by default.
    const char* no_kill = std::getenv("FM2K_NO_DESYNC_KILL");
    const bool kill_on_desync = !(no_kill && std::strcmp(no_kill, "1") == 0);

    if (!kill_on_desync) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DESYNC: FM2K_NO_DESYNC_KILL=1 -- staying alive for diagnostic "
            "observation. Game state will corrupt further; expect a crash "
            "within a few thousand frames.");
        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "%sDESYNC: terminating game on first divergence (frame %d). "
        "Dump written to FM2K_P%d_desync_f%d.log. Set FM2K_NO_DESYNC_KILL=1 "
        "to keep running for diagnostic inspection.",
        synthetic ? "SYNTHETIC " : "",
        frame, g_player_index, frame);

    // Drop an upload manifest for the launcher to pick up.
    //
    // Paths and game_id go through the UTF-8 helpers -- GetCurrent-
    // DirectoryA / GetModuleFileNameA return Shift-JIS bytes on
    // Japanese-locale Windows with Japanese-named game folders, which
    // produces non-UTF-8 JSON. Pre-v0.2.44 launchers crashed on those
    // manifests; v0.2.44+ launchers quarantine them, but our own
    // manifests should obviously be valid.
    {
        std::string cwd_utf8;
        if (!fm2k::upload_queue::GetCurrentDirectoryUtf8(cwd_utf8)) {
            cwd_utf8 = ".";
        }
        char debug_path[MAX_PATH * 2];
        char desync_path[MAX_PATH * 2];
        char rng_path[MAX_PATH * 2];
        std::snprintf(debug_path, sizeof(debug_path),
            "%s\\logs\\FM2K_P%d_Debug.log",
            cwd_utf8.c_str(), g_player_index + 1);
        std::snprintf(desync_path, sizeof(desync_path),
            "%s\\FM2K_P%d_desync_f%d.log",
            cwd_utf8.c_str(), g_player_index + 1, frame);
        std::snprintf(rng_path, sizeof(rng_path),
            "%s\\FM2K_P%d_rngtrace.csv",
            cwd_utf8.c_str(), g_player_index + 1);

        std::string exe_utf8;
        fm2k::upload_queue::GetModuleFileNameUtf8(exe_utf8);
        // Strip directory + .exe to get the game_id stem.
        std::string game_id_str;
        {
            size_t slash = exe_utf8.find_last_of("\\/");
            std::string base = (slash == std::string::npos)
                ? exe_utf8 : exe_utf8.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            game_id_str = (dot == std::string::npos)
                ? base : base.substr(0, dot);
        }

        fm2k::upload_queue::Manifest mfst;
        mfst.kind = synthetic ? "desync_synthetic" : "desync";
        mfst.frame = frame;
        mfst.session_id = SpectatorNode_GetSessionId();
        mfst.player_index = g_player_index;
        mfst.game_id = game_id_str.c_str();
        mfst.file_paths.emplace_back(debug_path);
        mfst.file_paths.emplace_back(desync_path);
        mfst.file_paths.emplace_back(rng_path);
        fm2k::upload_queue::Enqueue(mfst);
    }

    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DESYNC);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
}
