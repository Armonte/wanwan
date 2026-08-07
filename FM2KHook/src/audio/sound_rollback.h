#pragma once
#include <cstdint>

// ============================================================================
// Rollback-safe sound layer (Mike Z "desired vs actual" design).
//
// FM2K's sound API is single-chokepoint -- every SFX play goes through
// DispatchScriptSoundCommand @ 0x403430 → PlaySoundFromBufferArray @ 0x415DF0.
// The per-channel round-robin cursor at offset +12 of each SoundBufferArray
// is sim-mutable, which is why naive rollback replay clips sounds (replay
// re-advances the cursor and force-stops still-playing buffers).
//
// This layer interposes between the sim and DSound: during battle, the hook
// only updates `desired[channel]`. Once per displayed frame (after the
// advance batch completes, before render) we reconcile desired ↔ actual and
// trigger real DSound stops/plays, gated by Mike Z's rollback-window rules:
//
//   - If desired was written OUTSIDE the rollback window, rollback didn't
//     change it.  But if the currently-playing actual was set INSIDE the
//     window, rollback erased that play -- stop it.
//   - If desired was written INSIDE the window, play it for real now.
//
// Channel identity = slot index in g_sound_channel_table @ 0x430640..0x433240
// (2816 DWORD-sized pointer slots). Populated once at battle start by scanning
// for non-null entries; each SoundBufferArray pointer maps to a stable slot id.
// ============================================================================

namespace SoundRollback {

// Per-channel desired state. Saved in the rollback ring.
struct DesiredState {
    uint32_t script_item_ptr;  // script item ptr for re-invoking DispatchScriptSoundCommand at sync time
    uint32_t wave_ptr;         // identity: SoundBufferArray.wave_data pointer
    uint32_t play_frame;       // frame this was triggered on
    uint16_t seq_in_frame;     // tiebreaker when the same chan plays twice in one frame
    uint8_t  stopped;          // 1 = "silence was played on play_frame" (post-rollback-erase)
    uint8_t  _pad;
};

// BGM (MIDI cmd_low 2 / CD cmd_low 3 / full-stop cmd_low 0) desired state -- a
// SINGLE global stream (unlike SFX's per-channel array). Same rollback model as
// SFX: the dispatcher only records `desired`; SyncAfterAdvance reconciles it to
// the actually-playing MCI stream once per displayed frame, so the save-ring's
// forward-replay across a music-trigger frame no longer restarts MCI (the old
// cutoff), and a rollback that crosses a music start restores the pre-rollback
// desired from the ring instead of losing it (the old dedup couldn't).
// Looping-WAV BGM (cmd_low 1 + loop) is NOT here -- it rides the per-channel SFX
// layer already; battle-end stop covers it via ControlSoundSystem(0).
struct DesiredBgm {
    uint32_t script_item_ptr;  // re-invoke the dispatcher at sync time (game plays MCI)
    uint32_t payload;          // identity: (script_item+36) ^ (script_item+41)
    uint32_t play_frame;       // frame this BGM command was recorded on
    uint8_t  cmd_low;          // 0=stop 2=MIDI 3=CD
    uint8_t  valid;            // 0 = no BGM command recorded this battle yet
    uint8_t  _pad[2];
};

constexpr int MAX_CHANNELS = 2816;  // = (0x433240 - 0x430640) / 4

// Lifecycle -- called from Netplay_StartBattle / Netplay_EndBattle.
void Init();
void OnBattleEnd();

// Hook/sync thunk registration. Called once at MinHook install so the sync
// step can invoke the original DispatchScriptSoundCommand trampoline for
// each channel at sync time (which issues the full Stop+Prepare+Play+Volume
// sequence that DSound actually needs -- PlaySoundFromBufferArray alone only
// prepares the buffer, it never calls IDirectSoundBuffer::Play).
typedef int(__cdecl* OriginalDispatcherFn)(int script_item);
void SetOriginalDispatcher(OriginalDispatcherFn fn);

// Mute gates. Set by the launcher via SetMuteState() at hook init
// (reads %APPDATA%\FM2K_Rollback\audio.ini once). When set, the
// dispatcher hook drops the corresponding command type before
// calling MCI / DirectSound.
//
// LilithPort attaches as a debugger and overrides the EDX value
// pushed into IDirectSoundBuffer::SetVolume at addresses 0x40347E
// (BGM) / 0x40348C (SE), which gives a 21-step centibel slider
// (-10000 = silent, log-spaced up to 0). We don't need a slider;
// "off" is enough for now, and an early-return in the dispatcher
// covers MCI music + WAV SFX without needing the debugger /
// centibel-table machinery.
struct MuteState {
    bool bgm = false;
    bool se  = false;
};
void      SetMuteState(const MuteState& m);
MuteState GetMuteState();
bool      IsMusicMuted();   // BGM (MIDI / CD)
bool      IsSfxMuted();     // SFX (WAV)

// Re-read %APPDATA%\FM2K_Rollback\audio.ini and apply. Cheap (one
// stat + ~50-byte read). Hook calls this lazily, ~once per second,
// from Hook_DispatchScriptSoundCommand so the launcher's toggle
// reaches the running game without IPC.
void RefreshMuteFromDisk();

// Called from Hook_DispatchScriptSoundCommand on the SFX branch.
// `arr` is the SoundBufferArray pointer (script_item + 36).
// `script_item` is the 42-byte script item -- stored in desired[] so the sync
// step can replay the full original dispatcher for this channel.
// Returns true if the channel was recognised (caller should skip the real play);
// false means the caller MUST fall through to the original dispatcher so the
// sound still plays -- unknown channels just aren't rollback-tracked.
bool RecordDesired(void* arr, int script_item, uint32_t current_frame);

// Called once per displayed frame from Netplay_ProcessBattleInputPhase,
// AFTER the advance batch completes and BEFORE render. Walks desired[] vs
// actual[], applies Mike Z's window check, stops/starts DSound buffers via
// the original PlaySoundFromBufferArray / StopAllSoundsInBufferArray.
void SyncAfterAdvance(uint32_t earliest_frame, uint32_t current_frame);

// Savestate integration -- called from SaveState_Save / SaveState_Load.
void CaptureDesired(DesiredState* out);
void RestoreDesired(const DesiredState* in);
void CaptureBgm(DesiredBgm* out);
void RestoreBgm(const DesiredBgm* in);

// Diagnostic (FM2K_BGM_TRACE=1): looping-WAV music dispatch from the SFX branch.
void TraceWavMusicDispatch(uint32_t frame, bool in_battle);

// Diagnostic (FM2K_BGM_TRACE=1): top-of-dispatcher log of EVERY BGM/stop
// dispatch BEFORE any routing, with netplay + in-battle-phase state.
void TraceBgmDispatch(uint8_t cmd, bool netplay_active, bool in_battle);

// Called from Hook_DispatchScriptSoundCommand on the BGM branch (cmd_low 2/3)
// ONLY while in the battle phase. Records the desired MIDI/CD stream; the real
// MCI play is deferred to SyncAfterAdvance. `payload` is
// (script_item+36) ^ (script_item+41). Always returns true (caller defers).
bool RecordDesiredBgm(int script_item, uint8_t cmd_low, uint32_t payload,
                      uint32_t current_frame);

} // namespace SoundRollback
