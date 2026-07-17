#include "sound_rollback.h"
#include "globals.h"  // FM2K::ADDR_SOUND_* / ADDR_CONTROL_SOUND (engine-routed)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <SDL3/SDL_log.h>
#include <windows.h>  // GetTickCount

// ============================================================================
// Engine-routed constants (globals.h). FM2K values verified via IDA 2026-04-23:
//   g_sound_channel_table     @ 0x430640 — array of SoundBufferArray*
//   g_sound_channel_table_end @ 0x433240 — end marker (used by ReleaseAllSoundBuffers)
//   PlaySoundFromBufferArray  @ 0x415DF0 — core DSound play + round-robin
//   StopAllSoundsInBufferArray@ 0x415F00 — stop every buffer in one array
// FM95: the play/stop/control FUNCTIONS are known (0x401590 / 0x4016A0 /
// 0x4020A0) but the channel TABLE is not yet RE'd (RE-1 in
// docs/FM95_Support_Status.md) -- its ADDR_SOUND_CHANNEL_TABLE is a 0 sentinel,
// which makes CHANNEL_TABLE_SLOTS 0 so Init()/ChannelFor() scans no-op and the
// SFX desired/actual layer is inert-but-safe. The BGM desired path has no
// table dependency and stays live on both engines.
// ============================================================================
namespace {

constexpr uintptr_t ADDR_CHANNEL_TABLE     = FM2K::ADDR_SOUND_CHANNEL_TABLE;
constexpr uintptr_t ADDR_CHANNEL_TABLE_END = FM2K::ADDR_SOUND_CHANNEL_TABLE_END;
constexpr size_t    CHANNEL_TABLE_SLOTS    =
    (ADDR_CHANNEL_TABLE_END - ADDR_CHANNEL_TABLE) / sizeof(void*);

// Real DSound plays happen by invoking the ORIGINAL DispatchScriptSoundCommand
// trampoline on the saved script_item. That covers the full StopAllSounds +
// PlaySoundFromBufferArray + IDirectSoundBuffer::Play + volume sequence — none
// of which PlaySoundFromBufferArray alone performs (it only preps the buffer).
SoundRollback::OriginalDispatcherFn g_original_dispatcher = nullptr;

SoundRollback::DesiredState g_desired[SoundRollback::MAX_CHANNELS];
SoundRollback::DesiredState g_actual [SoundRollback::MAX_CHANNELS];

std::unordered_map<void*, int> g_ptr_to_chan;

uint16_t g_seq_counter = 0;
uint32_t g_seq_anchor_frame = 0;

// Diagnostics: tally how many plays land on known vs unknown channels. Logged
// periodically so we can see whether the Mike Z layer is actually covering the
// SFX we care about, or whether characters allocate buffer_arrays outside
// g_sound_channel_table and we're all passthrough.
uint32_t g_stat_record_known = 0;
uint32_t g_stat_record_unknown = 0;
uint32_t g_stat_last_log_tick = 0;

// BGM (MIDI/CD/stop) rollback state — single global stream. desired is written
// by the dispatcher hook and saved in the ring; actual is what MCI is really
// playing (NOT saved — reconstructed by SyncAfterAdvance). Cleared by
// OnBattleEnd. Replaces the old (cmd,payload) dedup with a proper
// desired-vs-actual reconcile, so music survives save-ring scroll + rollback.
SoundRollback::DesiredBgm g_desired_bgm = {};
SoundRollback::DesiredBgm g_actual_bgm  = {};

// The engine's own sound control (FM2K ControlSound @ 0x4034D0 / FM95
// ControlSoundSystem @ 0x4020A0 -- routed via globals.h): 0 = stop all channels
// + MIDI + CD (scene-transition teardown — safe/idempotent, only Stop calls,
// no free), 2 = stop MIDI, 3 = stop CD. Used for battle-end stop and the
// (rare) rollback-erased-music case.
using ControlSoundFn = int(__cdecl*)(int);
ControlSoundFn ControlSound = reinterpret_cast<ControlSoundFn>(FM2K::ADDR_CONTROL_SOUND);

bool BgmEqual(const SoundRollback::DesiredBgm& a, const SoundRollback::DesiredBgm& b) {
    return a.valid == b.valid && a.cmd_low == b.cmd_low && a.payload == b.payload;
}

// Optional BGM event trace (FM2K_BGM_TRACE=1) so we can see, in the log, every
// BGM dispatch + whether it deferred or played immediately + reconcile plays.
bool BgmTrace() {
    static int s = -1;
    if (s < 0) { const char* v = std::getenv("FM2K_BGM_TRACE"); s = (v && v[0] == '1'); }
    return s == 1;
}

// Mute state — atomic so launcher-driven toggles are visible without
// locks. The launcher writes %APPDATA%\FM2K_Rollback\audio.ini and the
// dispatcher refreshes from it ~once per second.
SoundRollback::MuteState g_mute = {};
uint32_t                 g_mute_last_check_tick = 0;

bool StatesEqual(const SoundRollback::DesiredState& a,
                 const SoundRollback::DesiredState& b) {
    // Channel identity is (wave_ptr, play_frame, stopped). seq_in_frame is
    // NOT part of identity — it's a within-frame tiebreaker that can drift
    // across stress-mode replay batches because g_seq_counter is global and
    // the number of channels firing in a given frame anchor varies across
    // batches. Including seq here caused constant stop+restart cycles on
    // stable sounds (audible as buzzing).
    return a.wave_ptr   == b.wave_ptr
        && a.play_frame == b.play_frame
        && a.stopped    == b.stopped;
}

inline bool FrameInWindow(uint32_t f, uint32_t lo, uint32_t hi) {
    return f >= lo && f <= hi;
}

int ChannelFor(void* arr) {
    auto it = g_ptr_to_chan.find(arr);
    if (it != g_ptr_to_chan.end()) return it->second;
    // Lazy fallback — a buffer array might have been populated after Init()
    // (e.g. mid-match asset load, unusual but handle cleanly).
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);
    for (int i = 0; i < static_cast<int>(CHANNEL_TABLE_SLOTS); i++) {
        if (table[i] == arr) {
            g_ptr_to_chan[arr] = i;
            return i;
        }
    }
    return -1;
}

} // anonymous

namespace SoundRollback {

void SetOriginalDispatcher(OriginalDispatcherFn fn) {
    g_original_dispatcher = fn;
}

// ----- Mute API ------------------------------------------------------------

void SetMuteState(const MuteState& m) {
    g_mute = m;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SoundRollback: mute bgm=%d se=%d", m.bgm ? 1 : 0, m.se ? 1 : 0);
}

MuteState GetMuteState() { return g_mute; }
bool IsMusicMuted() { return g_mute.bgm; }
bool IsSfxMuted()   { return g_mute.se;  }

namespace {
std::string MuteIniPath() {
    char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\audio.ini";
}
}

void RefreshMuteFromDisk() {
    const std::string path = MuteIniPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;  // file doesn't exist yet — keep current state
    MuteState m{};
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        // tolerant key=value parser; skip whitespace + comments
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#' || *p == ';' || *p == '\n' || *p == '\r') continue;
        const char* eq = std::strchr(p, '=');
        if (!eq) continue;
        std::string k(p, eq);
        std::string v(eq + 1);
        while (!v.empty() && (v.back()=='\n' || v.back()=='\r' ||
                              v.back()==' ' || v.back()=='\t')) v.pop_back();
        const bool truthy = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (k == "bgm_muted") m.bgm = truthy;
        else if (k == "se_muted") m.se = truthy;
    }
    std::fclose(f);
    if (m.bgm != g_mute.bgm || m.se != g_mute.se) {
        SetMuteState(m);
    }
}

void TraceWavMusicDispatch(uint32_t frame, bool in_battle) {
    if (BgmTrace())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BGM-WAV: loop-WAV music dispatch frame=%u in_battle=%d", frame,
            (int)in_battle);
}

void TraceBgmDispatch(uint8_t cmd, bool netplay_active, bool in_battle) {
    if (BgmTrace())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BGM-DISP: cmd=0x%02X cmd_low=%u loop=%d netplay=%d in_battle=%d",
            cmd, cmd & 0xF, (cmd & 0x10) != 0, (int)netplay_active,
            (int)in_battle);
}

bool RecordDesiredBgm(int script_item, uint8_t cmd_low, uint32_t payload,
                      uint32_t current_frame) {
    g_desired_bgm.script_item_ptr = static_cast<uint32_t>(script_item);
    g_desired_bgm.payload         = payload;
    g_desired_bgm.play_frame      = current_frame;
    g_desired_bgm.cmd_low         = cmd_low;
    g_desired_bgm.valid           = 1;
    if (BgmTrace())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BGM: record desired cmd_low=%u payload=0x%08X frame=%u (deferred)",
            cmd_low, payload, current_frame);
    return true;  // deferred to SyncAfterAdvance (caller only reaches here in-battle)
}

void Init() {
    g_ptr_to_chan.clear();
    std::memset(g_desired, 0, sizeof(g_desired));
    std::memset(g_actual,  0, sizeof(g_actual));
    g_seq_counter = 0;
    g_seq_anchor_frame = 0;
    g_desired_bgm = {};
    g_actual_bgm  = {};

    // FM95: CHANNEL_TABLE_SLOTS == 0 (table unmapped, RE-1) -- the scan
    // no-ops and `table` is never dereferenced.
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);
    int populated = 0;
    for (int i = 0; i < static_cast<int>(CHANNEL_TABLE_SLOTS); i++) {
        if (table[i]) {
            g_ptr_to_chan[table[i]] = i;
            populated++;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SoundRollback: Init — %d non-null channels in %zu-slot table",
        populated, CHANNEL_TABLE_SLOTS);
}

void OnBattleEnd() {
    if (g_stat_record_known + g_stat_record_unknown > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: battle end — total plays known=%u unknown=%u",
            g_stat_record_known, g_stat_record_unknown);
    }
    g_stat_record_known = 0;
    g_stat_record_unknown = 0;
    g_stat_last_log_tick = 0;
    if (BgmTrace())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BGM: OnBattleEnd -> clear desired (no stop-all)");
    // Do NOT stop sound here. The game stops the battle BGM with its own
    // cmd_low=0 dispatch on the battle->CSS transition, then its freshly
    // re-created CSS controller re-dispatches the CSS BGM (game_mode==2000).
    // An earlier ControlSoundSystem(0) here ran AFTER that and silenced the
    // CSS-return music — we respect the game's own stop/start and just reset our
    // per-battle BGM tracking for the next match.
    g_desired_bgm = {};
    g_actual_bgm  = {};
    g_ptr_to_chan.clear();
    std::memset(g_desired, 0, sizeof(g_desired));
    std::memset(g_actual,  0, sizeof(g_actual));
}

bool RecordDesired(void* arr, int script_item, uint32_t current_frame) {
    int chan = ChannelFor(arr);
    if (chan < 0 || chan >= MAX_CHANNELS) {
        // Unknown channel — not in g_sound_channel_table. Caller must fall
        // through to the original dispatcher so the sound still plays; it just
        // won't be rollback-tracked.
        g_stat_record_unknown++;
        return false;
    }
    g_stat_record_known++;

    if (current_frame != g_seq_anchor_frame) {
        g_seq_counter = 0;
        g_seq_anchor_frame = current_frame;
    }

    // SoundBufferArray layout: {wave_ptr, wave_len, buf_count, cur_index, buffers[]}.
    // Identity uses wave_ptr — same wave on same channel = "no change", lets the
    // dedupe path skip re-triggering.
    uint32_t wave_ptr = *reinterpret_cast<uint32_t*>(arr);

    g_desired[chan].script_item_ptr = static_cast<uint32_t>(script_item);
    g_desired[chan].wave_ptr        = wave_ptr;
    g_desired[chan].play_frame      = current_frame;
    g_desired[chan].seq_in_frame    = ++g_seq_counter;
    g_desired[chan].stopped         = 0;
    return true;
}

void SyncAfterAdvance(uint32_t earliest_frame, uint32_t current_frame) {
    // SFX desired/actual reconcile needs the engine's channel table. On FM95
    // it is unmapped (RE-1, ADDR_SOUND_CHANNEL_TABLE == 0) so this whole
    // layer is compiled out there -- inert-but-safe. The BGM reconcile at the
    // bottom has no table dependency and stays live on both engines.
    if constexpr (FM2K::ADDR_SOUND_CHANNEL_TABLE != 0) {
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);

    // Once-per-second coverage log. If unknown >> known, the Mike Z layer is
    // mostly inert and the pre-scanned g_sound_channel_table isn't where
    // character SFX actually live — we need to widen the channel identity.
    uint32_t now_tick = GetTickCount();
    if (now_tick - g_stat_last_log_tick >= 1000 &&
        (g_stat_record_known + g_stat_record_unknown) > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: coverage — known=%u unknown=%u (passthrough ratio %.1f%%)",
            g_stat_record_known, g_stat_record_unknown,
            100.0f * g_stat_record_unknown /
                (float)(g_stat_record_known + g_stat_record_unknown));
        g_stat_last_log_tick = now_tick;
    }

    // Per-sync breakdown of which branch each divergent channel takes, so we
    // can see why "known=331 unknown=0" still produces silence. Logged at the
    // same 1-Hz cadence as the coverage line, bounded to 4 entries/sync.
    static uint32_t s_branch_log_tick = 0;
    bool verbose = (now_tick - s_branch_log_tick >= 1000);
    int verbose_remaining = 4;
    uint32_t branch_plays = 0;
    uint32_t branch_stops = 0;
    uint32_t branch_skips = 0;

    for (int chan = 0; chan < MAX_CHANNELS; chan++) {
        if (StatesEqual(g_desired[chan], g_actual[chan])) {
            branch_skips++;
            continue;
        }

        const bool des_in = FrameInWindow(g_desired[chan].play_frame, earliest_frame, current_frame);
        const bool act_in = FrameInWindow(g_actual [chan].play_frame, earliest_frame, current_frame);

        if (verbose && verbose_remaining > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "  chan=%d des{wav=0x%08X f=%u s=%u stop=%u} act{wav=0x%08X f=%u s=%u stop=%u} win=[%u,%u] des_in=%d act_in=%d",
                chan,
                g_desired[chan].wave_ptr, g_desired[chan].play_frame,
                g_desired[chan].seq_in_frame, g_desired[chan].stopped,
                g_actual[chan].wave_ptr,  g_actual[chan].play_frame,
                g_actual[chan].seq_in_frame, g_actual[chan].stopped,
                earliest_frame, current_frame, (int)des_in, (int)act_in);
            verbose_remaining--;
        }

        if (!des_in) {
            // Desired was set outside the rollback window — rollback couldn't
            // have changed it. But if actual is inside the window, rollback
            // erased a play that's currently audible: stop it by stopping
            // every buffer on the channel.
            if (act_in) {
                void* arr = table[chan];
                if (arr) {
                    // Re-invoke the dispatcher with a synthesised "stop" script
                    // item would require allocation; instead just clobber the
                    // channel's buffers directly via the engine's own
                    // StopAllSoundsInBufferArray (engine-routed, globals.h).
                    using StopFn = int(__cdecl*)(void*);
                    ((StopFn)FM2K::ADDR_STOP_ALL_SOUNDS_IN_BUFFER_ARRAY)(arr);
                }
                g_actual[chan].script_item_ptr = 0;
                g_actual[chan].wave_ptr        = 0;
                g_actual[chan].play_frame      = current_frame;
                g_actual[chan].seq_in_frame    = 0;
                g_actual[chan].stopped         = 1;
                branch_stops++;
            }
            // Sync desired forward so savestates capture "what's really playing."
            g_desired[chan] = g_actual[chan];
        } else {
            // Desired was set during the window — trigger the actual play via
            // the original dispatcher, which runs the full
            // Stop+Prepare+Play+Volume sequence. PlaySoundFromBufferArray
            // alone only preps the buffer; this is why the previous version
            // recorded 100% known channels but produced zero audible output.
            if (!g_desired[chan].stopped &&
                g_original_dispatcher &&
                g_desired[chan].script_item_ptr) {
                g_original_dispatcher(
                    static_cast<int>(g_desired[chan].script_item_ptr));
            } else {
                // Explicit "play nothing" or missing dispatcher — just stop.
                void* arr = table[chan];
                if (arr) {
                    using StopFn = int(__cdecl*)(void*);
                    ((StopFn)FM2K::ADDR_STOP_ALL_SOUNDS_IN_BUFFER_ARRAY)(arr);
                }
            }
            g_actual[chan] = g_desired[chan];
            branch_plays++;
        }
    }

    if (verbose && (branch_plays + branch_stops) > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: sync window=[%u,%u] plays=%u stops=%u skips=%u",
            earliest_frame, current_frame, branch_plays, branch_stops, branch_skips);
        s_branch_log_tick = now_tick;
    }
    }  // if constexpr (ADDR_SOUND_CHANNEL_TABLE != 0) -- end SFX reconcile

    // --- BGM (MIDI/CD/stop) reconcile: single global stream, same window rule
    // as SFX. Identity is (cmd_low, payload): an unchanged desired == actual is
    // a no-op, so the save-ring scrolling forward across the music-trigger frame
    // no longer restarts MCI (the old cut-in/out). A rollback restores desired
    // from the ring (RestoreBgm) -> reconcile brings the real MCI stream back to
    // the confirmed track.
    if (!BgmEqual(g_desired_bgm, g_actual_bgm)) {
        const bool des_in =
            FrameInWindow(g_desired_bgm.play_frame, earliest_frame, current_frame);
        if (des_in) {
            // Confirmed new/changed BGM this batch — issue the real MCI play/stop
            // via the game's own dispatcher (WriteTempMIDIAndPlay /
            // InitializeCDAudio / ControlSoundSystem(0) for cmd_low 0), once.
            if (BgmTrace())
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "BGM: sync PLAY cmd_low=%u payload=0x%08X frame=%u",
                    g_desired_bgm.cmd_low, g_desired_bgm.payload,
                    g_desired_bgm.play_frame);
            if (g_desired_bgm.valid && g_original_dispatcher &&
                g_desired_bgm.script_item_ptr) {
                g_original_dispatcher(static_cast<int>(g_desired_bgm.script_item_ptr));
            }
        } else {
            // Desired stable (set outside the window) but actual diverged — a
            // play landed in the window then got rolled back. Restore truth: a
            // real track re-asserts via the dispatcher; a stop stops MCI.
            // (Music starts almost never land in the rollback window; degenerate
            // but kept correct.)
            if (g_desired_bgm.valid && g_desired_bgm.cmd_low != 0 &&
                g_original_dispatcher && g_desired_bgm.script_item_ptr) {
                g_original_dispatcher(static_cast<int>(g_desired_bgm.script_item_ptr));
            } else if (ControlSound) {
                ControlSound(2);  // stop MIDI
                ControlSound(3);  // stop CD
            }
        }
        g_actual_bgm = g_desired_bgm;
    }
}

void CaptureDesired(DesiredState* out) {
    std::memcpy(out, g_desired, sizeof(g_desired));
}

void RestoreDesired(const DesiredState* in) {
    std::memcpy(g_desired, in, sizeof(g_desired));
}

// BGM desired is deterministic (cmd_low/payload/play_frame + a script_item_ptr
// into saved sim memory). actual is NOT saved — SyncAfterAdvance reconstructs
// the real MCI stream from desired after a load.
void CaptureBgm(DesiredBgm* out) {
    std::memcpy(out, &g_desired_bgm, sizeof(g_desired_bgm));
}

void RestoreBgm(const DesiredBgm* in) {
    std::memcpy(&g_desired_bgm, in, sizeof(g_desired_bgm));
}

} // namespace SoundRollback
