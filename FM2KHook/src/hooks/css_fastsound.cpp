// css_fastsound.cpp -- "lazy DirectSound buffer" optimization for CSS.
//
// THE PROBLEM (verified in the WonderfulWorld / shared 2dfm binary):
//   player_data_file_loader -> character_data_loader, for EVERY WAVE sound,
//   calls AllocateSoundBufferCopiesFromMem @0x415CD0 which builds a polyphonic
//   DirectSound sound object (CreateSoundBufferFromMemory + DuplicateSoundBuffer
//   per voice). An audio-heavy character has ~80 sounds, so every CSS cursor
//   move pays ~80+ COM CreateSoundBuffer calls -- a ~150ms hover dip -- for sound
//   that never plays on the select screen.
//
// THE FIX (correct by construction -- no mode/caller heuristics):
//   Defer buffer creation. AllocateSoundBufferCopiesFromMem builds the real
//   sound-object struct but points its buffer slots at a single shared, silent
//   DUMMY DirectSound buffer (NOT null) and stashes a copy of the WAV. The first
//   time the script engine actually plays the sound (PlaySoundFromBufferArray
//   @0x415DF0, sole external caller is the SFX dispatcher), we build the real
//   buffer(s) from the stashed WAV and swap them in, then let the play proceed.
//   CSS never plays character SFX -> never realized -> instant; battle realizes
//   each on first play (~1ms).
//
//   Why a dummy buffer and not null: not every engine accessor null-checks.
//   StopAllSoundsInBufferArray @0x415F00 calls Stop/SetCurrentPosition on each
//   buffer with NO null-check (it ran at boot and dereferenced a null buffer ->
//   crash). Pointing deferred slots at a valid silent buffer makes EVERY
//   accessor safe without having to enumerate them. The dummy is created once
//   and never released; realize/release overwrite/skip those slots.
//
// Sound-object layout (DWORD indices), from the binary:
//   [0]=wave_data ptr  [1]=wave_len  [2]=voice count  [3]=round-robin cursor
//   [4..]=IDirectSoundBuffer*[voices]
//
// Gated on FM2K_FPK_CSS_FASTSOUND=1. FM2K engine only (addresses are WW-family).

#include "css_fastsound.h"

#ifndef ENGINE_FM95  // FM2K-family addresses; not valid for the FM95 prototype

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>

#include "MinHook.h"
#include <SDL3/SDL.h>

namespace {

// ── engine addresses (FM2K / WonderfulWorld-family runtime) ─────────────
constexpr uintptr_t kAddr_AllocateSoundBufferCopiesFromMem = 0x415CD0;
constexpr uintptr_t kAddr_PlaySoundFromBufferArray         = 0x415DF0;
constexpr uintptr_t kAddr_ReleaseSoundBufferCopies         = 0x415DB0;
constexpr uintptr_t kAddr_CreateSoundBufferFromMemory      = 0x415C20;
constexpr uintptr_t kAddr_ParseWaveData                    = 0x415BF0;

using AllocFn     = uint32_t* (__cdecl*)(void* dsound, uint32_t wav, int voices);
using PlayFn      = int       (__cdecl*)(uint32_t* obj);
using ReleaseFn   = void      (__cdecl*)(uint32_t* obj);
using ParseFn     = int       (__cdecl*)(int, uint32_t wav, int*, uint32_t* dataOut, uint32_t* lenOut);
using CreateBufFn = uint32_t  (__cdecl*)(void* dsound, uint32_t wav);
// IDirectSound::DuplicateSoundBuffer is vtable index 5 (offset 20).
using DupFn       = long      (__stdcall*)(void* self, uint32_t src, uint32_t* dst);
// Diagnostic-only: bracket resource_cleanup_manager and watch every GlobalFree
// it makes, to capture the exact pointer whose free smashes the heap.
constexpr uintptr_t kAddr_ResourceCleanupManager = 0x403520;
constexpr uintptr_t kAddr_CharacterDataLoader    = 0x403600;
constexpr uintptr_t kAddr_PlayerDataFileLoader   = 0x4039F0;
using CleanupFn    = HGLOBAL (__cdecl*)(int a1);
using GlobalFreeFn = HGLOBAL (__stdcall*)(HGLOBAL);
using CharLoadFn   = int     (__cdecl*)(void* buf, int hFile);
using PlayerLoadFn = int     (__cdecl*)(int slot, int charId);

AllocFn      g_orig_Alloc       = nullptr;
PlayFn       g_orig_Play        = nullptr;
ReleaseFn    g_orig_Release     = nullptr;
CleanupFn    g_orig_Cleanup     = nullptr;
GlobalFreeFn g_orig_GlobalFree  = nullptr;
CharLoadFn   g_orig_CharLoad    = nullptr;
PlayerLoadFn g_orig_PlayerLoad  = nullptr;
bool         g_in_cleanup       = false;

// ── CSS load profiler (FM2K_FPK_CSS_PROFILE=1) ──────────────────────────
// Times the per-cursor-move character reload to find the 40fps dip's real
// cost: total move vs character_data_loader vs sound-buffer creation. The
// remainder (charload - sound) is sprite-frame + palette + file I/O.
bool          g_profile       = false;
LARGE_INTEGER g_qpf           = {};
double        g_charload_ms   = 0.0;   // last character_data_loader duration
uint64_t      g_sound_ticks   = 0;     // sound-alloc QPC ticks within current move
uint32_t      g_sound_cnt     = 0;
uint64_t      g_cleanup_ticks = 0;     // resource_cleanup_manager ticks within move
uint32_t      g_move_seq      = 0;

// ── CSS parsed-char cache (FM2K_CSS_CACHE=1) ────────────────────────────
// The engine has 8 character slots (g_character_data_base .. end, stride
// 57407). CSS only uses slots 0/1 (p1/p2 preview), so 2..7 sit idle -- we
// use them as an LRU cache of recently-viewed chars. On revisit we exchange
// the 57407-byte slot blob into the active preview slot: the sprite-buffer
// pointers travel WITH the blob (absolute addresses, valid from any slot),
// so it's an instant swap with no realloc and no aliasing (exchange, not
// copy -- each slot still owns its buffers uniquely). The engine's own
// portrait setup runs right after the load call and reads the now-correct
// slot, so no display glue is needed. Sounds are skipped in CSS (Hook_Alloc
// returns 0) so the per-physical-slot sound channel region never aliases.
constexpr uintptr_t kAddr_ClearCharacterSlot = 0x4039B0;
constexpr uintptr_t kAddr_GameMode           = 0x470054;  // 1000 title/2000 css/3000 battle
constexpr uintptr_t kAddr_CharDataBase       = 0x4D1D80;
constexpr uintptr_t kAddr_CharDataEnd        = 0x541F78;
constexpr uintptr_t kAddr_LoadedCharSlot     = 0x4CF9E0;  // int[8]
constexpr size_t    kSlotStride   = 57407;
constexpr int       kNumSlots     = 8;
constexpr int       kFirstCacheSlot = 2;                  // 0,1 = preview; 2..7 = cache
using ClearSlotFn = int (__cdecl*)(int slot);

bool g_cache = false;
int  g_slot_char[kNumSlots] = {-1,-1,-1,-1,-1,-1,-1,-1};  // charId resident in each slot
uint64_t g_cache_lru[kNumSlots] = {0};                    // last-use tick per cache slot
uint64_t g_cache_clock = 0;
uint64_t g_cache_hits = 0, g_cache_miss = 0;

inline uint8_t*  SlotBlob(int s)  { return reinterpret_cast<uint8_t*>(kAddr_CharDataBase + kSlotStride * s); }
inline int*      LoadedChar()     { return reinterpret_cast<int*>(kAddr_LoadedCharSlot); }
inline uint32_t  GameMode()       { return *reinterpret_cast<uint32_t*>(kAddr_GameMode); }

// Exchange two slots' 57407-byte char-data blobs (and the loaded-char
// tracking). Buffer pointers are absolute, so they stay valid post-swap.
// NOTE: sound channel state is handled separately (CSS sounds are skipped in
// cache mode for now -- see Hook_Alloc); preserving the swap audio needs the
// per-slot channel region to travel too, which crashed under churn and is
// being reworked. This blob-only swap is the validated, stable path.
void SwapSlotBlobs(int a, int b) {
    if (a == b) return;
    static thread_local std::vector<uint8_t> tmp;
    tmp.resize(kSlotStride);
    std::memcpy(tmp.data(),  SlotBlob(a), kSlotStride);
    std::memcpy(SlotBlob(a), SlotBlob(b), kSlotStride);
    std::memcpy(SlotBlob(b), tmp.data(),  kSlotStride);
    std::swap(LoadedChar()[a], LoadedChar()[b]);
    std::swap(g_slot_char[a], g_slot_char[b]);
}

// ── async CSS load (FM2K_CSS_ASYNC=1) ───────────────────────────────────
// Loading an 80MB char is ~60ms (read-bound, can't beat the format), so to
// keep CSS at 100fps we load on a worker into a spare slot and swap it into
// the preview slot on the main thread once ready. Safe because: cache mode
// returns 0 for CSS sounds (worker never touches DirectSound/COM), the worker
// writes ONLY a spare slot the render thread never reads, the heap + VFS file
// hooks are internally locked, and the main thread reads the spare slot only
// AFTER the worker publishes (happens-before via g_async_ready). Portrait may
// be momentarily stale after a swap (glue TODO) -- this pass validates that
// the frame never blocks and nothing corrupts.
constexpr uintptr_t kAddr_ProcessCharSelectHandler = 0x407D70;
using CssHandlerFn = char (__cdecl*)();
CssHandlerFn g_orig_CssHandler = nullptr;

bool                  g_async = false;
std::thread           g_async_worker;
std::mutex            g_async_mtx;
std::condition_variable g_async_cv;
bool                  g_async_stop = false;
// request: latest char the cursor wants per preview slot (-1 = none)
int                   g_async_req_char[2]  = {-1, -1};
// result: a spare slot finished loading req_char for a preview slot
std::atomic<bool>     g_async_ready{false};
int                   g_async_done_preview = -1;
int                   g_async_done_spare   = -1;
int                   g_async_done_char    = -1;
int                   g_async_next_spare   = kFirstCacheSlot;  // rotate 2..7
uint64_t              g_async_loads = 0;

void AsyncWorkerMain() {
    for (;;) {
        int previewSlot = -1, charId = -1;
        {
            std::unique_lock<std::mutex> lk(g_async_mtx);
            g_async_cv.wait(lk, [] {
                return g_async_stop || g_async_req_char[0] >= 0 || g_async_req_char[1] >= 0;
            });
            if (g_async_stop) return;
            // take the lowest preview slot with a pending request
            for (int p = 0; p < 2; ++p) {
                if (g_async_req_char[p] >= 0) { previewSlot = p; charId = g_async_req_char[p]; g_async_req_char[p] = -1; break; }
            }
        }
        if (charId < 0) continue;
        int spare = g_async_next_spare;
        g_async_next_spare = (g_async_next_spare + 1 - kFirstCacheSlot) % (kNumSlots - kFirstCacheSlot) + kFirstCacheSlot;
        g_orig_PlayerLoad(spare, charId);   // ~60ms off the main thread
        ++g_async_loads;
        // publish (only if no newer request supersedes this preview slot)
        bool published = false;
        {
            std::lock_guard<std::mutex> lk(g_async_mtx);
            if (g_async_req_char[previewSlot] < 0) {  // not superseded
                g_async_done_preview = previewSlot;
                g_async_done_spare   = spare;
                g_async_done_char    = charId;
                g_async_ready.store(true, std::memory_order_release);
                published = true;
            }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSSASYNC] worker loaded char=%d->spare=%d preview=%d %s (loads=%llu)",
            charId, spare, previewSlot, published ? "PUBLISHED" : "superseded",
            (unsigned long long)g_async_loads);
    }
}

// Main-thread per-CSS-frame: swap a finished async load into its preview slot.
char __cdecl Hook_CssHandler() {
    if (g_async && g_async_ready.load(std::memory_order_acquire)) {
        int preview, spare, ch;
        {
            std::lock_guard<std::mutex> lk(g_async_mtx);
            preview = g_async_done_preview; spare = g_async_done_spare; ch = g_async_done_char;
            g_async_ready.store(false, std::memory_order_relaxed);
        }
        if (preview >= 0 && LoadedChar()[preview] != ch) {
            SwapSlotBlobs(preview, spare);     // preview now shows ch; old preview char parked in spare
            LoadedChar()[preview] = ch;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSASYNC] swapped char=%d into preview slot=%d (loads=%llu)",
                ch, preview, (unsigned long long)g_async_loads);
        }
    }
    return g_orig_CssHandler();
}
auto      g_ParseWaveData               = reinterpret_cast<ParseFn>(kAddr_ParseWaveData);
auto      g_CreateSoundBufferFromMemory = reinterpret_cast<CreateBufFn>(kAddr_CreateSoundBufferFromMemory);

bool g_enabled = false;

// Heap-corruption hunt (FM2K_FPK_CSS_FASTSOUND_HEAPCHK=1). Validates the process
// heap at each hook's entry/exit; logs the FIRST transition from valid->invalid
// so we know exactly which call (and whose op -- ours vs the engine's, by
// entry-vs-exit) smashes the heap. Off by default; HeapValidate is expensive.
bool        g_heapchk = false;
bool        g_heap_was_bad = false;
uint64_t    g_chk_seq = 0;
const char* g_last_clean_probe = "(none)";  // most recent probe that saw a valid heap

void HeapCheck(const char* where) {
    if (!g_heapchk || g_heap_was_bad) return;
    uint64_t n = g_chk_seq++;
    if (!HeapValidate(GetProcessHeap(), 0, nullptr)) {
        g_heap_was_bad = true;  // log once
        // The PREVIOUS clean probe + this one bracket the engine code that
        // smashed the heap: 'release.after_orig'->'release.entry' = the
        // cleanup loop body (GlobalFree of the sound_item field); 'alloc.exit'
        // ->'release.entry' = char-load completion + the mode transition.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CssFastSound: HEAP CORRUPT first seen at '%s' "
                     "(prev clean probe '%s', chk #%llu)",
                     where, g_last_clean_probe, (unsigned long long)n);
    } else {
        g_last_clean_probe = where;
    }
}

struct Deferred {
    std::vector<uint8_t> wav;   // kept alive: obj[0] points into this until release
    void* dsound = nullptr;
    int   voices = 1;
};

std::mutex                               g_mtx;
std::unordered_map<uint32_t*, Deferred>  g_deferred;   // sound-obj ptr -> info

// Shared silent buffer every deferred sound's slots point to, so non-null-
// checking accessors (StopAllSoundsInBufferArray etc.) stay safe. Created once,
// never released. Its source WAV is kept alive for the buffer's lifetime.
uint32_t g_dummy_buffer = 0;
HGLOBAL  g_dummy_wav     = nullptr;

uint64_t g_deferred_count = 0;
uint64_t g_realized_count = 0;

// RAII: accumulate this Hook_Alloc call's wall time into the profiler totals
// (times whatever the call does -- real buffer creation when fastsound is off,
// or the cheap deferral when it's on).
struct SoundAllocTimer {
    LARGE_INTEGER s; bool on;
    SoundAllocTimer() : on(g_profile) { if (on) QueryPerformanceCounter(&s); }
    ~SoundAllocTimer() {
        if (!on) return;
        LARGE_INTEGER e; QueryPerformanceCounter(&e);
        g_sound_ticks += (e.QuadPart - s.QuadPart);
        ++g_sound_cnt;
    }
};

// Build the shared silent dummy buffer once. Caller holds g_mtx.
void EnsureDummyBufferLocked(void* dsound) {
    if (g_dummy_buffer || !dsound) return;
    // minimal valid PCM WAV: 8000Hz / 8-bit / mono, one sample of silence.
    static const uint8_t kWav[] = {
        'R','I','F','F', 37,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x40,0x1F,0,0, 0x40,0x1F,0,0, 1,0, 8,0,
        'd','a','t','a', 1,0,0,0, 0x80,
    };
    HGLOBAL h = GlobalAlloc(GMEM_FIXED, sizeof(kWav));
    if (!h) return;
    std::memcpy(h, kWav, sizeof(kWav));
    uint32_t buf = g_CreateSoundBufferFromMemory(dsound, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)));
    if (!buf) { GlobalFree(h); return; }
    g_dummy_wav    = h;        // keep the source WAV alive for the buffer's life
    g_dummy_buffer = buf;
}

// ── hook: defer buffer creation ─────────────────────────────────────────
uint32_t* __cdecl Hook_Alloc(void* dsound, uint32_t wav, int voices) {
    SoundAllocTimer _st;  // profiler: time the whole call (vanilla or deferred)
    // Cache/async skip CSS char sounds: returning 0 leaves the channel entry
    // null so the blob swap can't alias the per-slot sound region. CSS audio
    // is a known regression here -- preserving it needs the channel region to
    // travel with the swap, which is being reworked after it crashed.
    if ((g_cache || g_async) && GameMode() == 2000u) return 0;
    if (!g_enabled || !wav) return g_orig_Alloc(dsound, wav, voices);
    HeapCheck("alloc.entry");

    {  // need the shared dummy before we can defer; if it won't build, don't.
        std::lock_guard<std::mutex> lk(g_mtx);
        EnsureDummyBufferLocked(dsound);
        if (!g_dummy_buffer) return g_orig_Alloc(dsound, wav, voices);
    }

    SIZE_T wav_len = GlobalSize(reinterpret_cast<HGLOBAL>(static_cast<uintptr_t>(wav)));
    if (wav_len == 0) return g_orig_Alloc(dsound, wav, voices);

    std::vector<uint8_t> wav_copy;
    try {
        wav_copy.assign(reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(wav)),
                        reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(wav)) + wav_len);
    } catch (...) {
        return g_orig_Alloc(dsound, wav, voices);  // OOM -> let the engine do it
    }

    int v3 = voices < 1 ? 1 : voices;
    uint32_t* obj = static_cast<uint32_t*>(LocalAlloc(LMEM_ZEROINIT, 4 * v3 + 16));
    if (!obj) return g_orig_Alloc(dsound, wav, voices);
    obj[2] = static_cast<uint32_t>(v3);          // voice count
    for (int i = 0; i < v3; ++i) obj[4 + i] = g_dummy_buffer;  // valid silent buffers

    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        Deferred& d = g_deferred[obj];
        d.wav    = std::move(wav_copy);
        d.dsound = dsound;
        d.voices = v3;
        // parse the KEPT copy so wave_data (obj[0]) points into d.wav, not the
        // original which character_data_loader frees right after we return.
        int a3 = 0; uint32_t dataOut = 0, lenOut = 0;
        if (g_ParseWaveData(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d.wav.data())),
                            &a3, &dataOut, &lenOut)) {
            obj[0] = dataOut;
            obj[1] = lenOut;
            ok = true;
        }
    }
    if (!ok) {
        { std::lock_guard<std::mutex> lk(g_mtx); g_deferred.erase(obj); }
        LocalFree(obj);
        return g_orig_Alloc(dsound, wav, voices);  // unparseable -> normal path
    }
    ++g_deferred_count;
    HeapCheck("alloc.exit");
    return obj;
}

// Build the real DirectSound buffer(s) from the stashed WAV, overwriting the
// dummy slots. Caller holds g_mtx. The Deferred entry stays (obj[0] -> d.wav).
void RealizeLocked(uint32_t* obj, Deferred& d) {
    uint32_t wavptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d.wav.data()));
    uint32_t primary = g_CreateSoundBufferFromMemory(d.dsound, wavptr);
    if (!primary) return;  // creation failed: leave the silent dummy (no crash)
    obj[4] = primary;
    if (d.voices > 1 && d.dsound) {
        void** vtbl = *reinterpret_cast<void***>(d.dsound);
        DupFn dup = reinterpret_cast<DupFn>(vtbl[5]);  // DuplicateSoundBuffer
        for (int i = 1; i < d.voices; ++i) {
            uint32_t dst = 0;
            if (dup(d.dsound, primary, &dst) < 0)
                dst = g_CreateSoundBufferFromMemory(d.dsound, wavptr);
            obj[4 + i] = dst ? dst : g_dummy_buffer;
        }
    }
    ++g_realized_count;
}

// ── hook: realize on first play ─────────────────────────────────────────
int __cdecl Hook_Play(uint32_t* obj) {
    if (g_enabled) HeapCheck("play.entry");
    if (g_enabled && obj && obj[4] == g_dummy_buffer) {  // fast: deferred, unrealized
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_deferred.find(obj);
        if (it != g_deferred.end() && obj[4] == g_dummy_buffer) {
            RealizeLocked(obj, it->second);
            HeapCheck("play.realized");
        }
    }
    int r = g_orig_Play(obj);
    if (g_enabled) HeapCheck("play.after_orig");  // WriteWaveDataToBuffer overflow?
    return r;
}

// Bracket the per-char loader: if the heap dies across one of these, the
// smash is in sprite/sound reading (char load), not in play or cleanup.
// Also times the loader for the CSS profiler.
int __cdecl Hook_CharLoad(void* buf, int hFile) {
    HeapCheck("charload.entry");
    LARGE_INTEGER s; if (g_profile) QueryPerformanceCounter(&s);
    int r = g_orig_CharLoad(buf, hFile);
    if (g_profile) {
        LARGE_INTEGER e; QueryPerformanceCounter(&e);
        g_charload_ms = (e.QuadPart - s.QuadPart) * 1000.0 / g_qpf.QuadPart;
    }
    HeapCheck("charload.exit");
    return r;
}

// Times the whole per-cursor-move reload (ClearCharacterSlot + open +
// character_data_loader + trailing region reads). Returns 0 immediately
// when the target char is already in the slot, so only real reloads log.
int __cdecl Hook_PlayerLoad(int slot, int charId) {
    // ── async CSS load: never block the frame ───────────────────────────
    // CRITICAL: the engine dereferences the slot's char data SYNCHRONOUSLY
    // right after this call (portrait/action-table reads). So we can only
    // defer when the slot ALREADY holds a valid, fully-loaded char (the
    // engine then safely reads THAT char while the worker fetches the new
    // one). An empty/uninitialised slot (loaded==-1) must load synchronously,
    // else the immediate deref hits null. g_player_loaded_char_slot is -1
    // exactly when the slot was cleared (buffers freed), so >=0 == safe data.
    if (g_async && (slot == 0 || slot == 1) && GameMode() == 2000u) {
        if (LoadedChar()[slot] == charId) return 0;     // already the preview char
        if (LoadedChar()[slot] >= 0) {                  // slot has valid data -> safe to defer
            {
                std::lock_guard<std::mutex> lk(g_async_mtx);
                g_async_req_char[slot] = charId;        // queue latest target for this slot
            }
            g_async_cv.notify_one();
            static uint32_t s_dbg = 0;
            if ((s_dbg++ & 7) == 0)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[CSSASYNC] defer slot=%d char=%d (have=%d)", slot, charId, LoadedChar()[slot]);
            return 0;   // return immediately; worker loads, Hook_CssHandler swaps in
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSSASYNC] sync-load slot=%d char=%d (empty slot)", slot, charId);
        // empty slot: fall through to a synchronous load (engine needs data now)
    }

    // ── CSS parsed-char cache: instant swap on revisit ──────────────────
    if (g_cache && slot >= 0 && slot < kNumSlots) {
        if (GameMode() == 2000u) {
            int* loaded = LoadedChar();
            if (loaded[slot] == charId) return 0;  // already the preview char
            for (int c = kFirstCacheSlot; c < kNumSlots; ++c) {
                if (g_slot_char[c] == charId) {     // HIT: exchange into preview slot
                    SwapSlotBlobs(slot, c);
                    g_cache_lru[c] = ++g_cache_clock;
                    ++g_cache_hits;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[CSSCACHE] slot=%d char=%d HIT (instant swap) hits=%llu miss=%llu",
                        slot, charId, (unsigned long long)g_cache_hits,
                        (unsigned long long)g_cache_miss);
                    return 0;
                }
            }
            // MISS: stash the old preview char into an LRU cache slot, then load.
            int oldChar = loaded[slot];
            if (oldChar != -1) {
                int victim = kFirstCacheSlot;
                uint64_t best = ~0ull;
                for (int c = kFirstCacheSlot; c < kNumSlots; ++c) {
                    if (g_slot_char[c] == -1) { victim = c; break; }  // prefer empty
                    if (g_cache_lru[c] < best) { best = g_cache_lru[c]; victim = c; }
                }
                if (g_slot_char[victim] != -1) {     // evict victim's char (free its buffers)
                    reinterpret_cast<ClearSlotFn>(kAddr_ClearCharacterSlot)(victim);
                    g_slot_char[victim] = -1;
                }
                SwapSlotBlobs(slot, victim);         // old preview char -> cache; preview slot now empty
                g_cache_lru[victim] = ++g_cache_clock;
            }
            ++g_cache_miss;
            int r = g_orig_PlayerLoad(slot, charId); // the one real load (this dip only)
            g_slot_char[slot] = charId;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSCACHE] slot=%d char=%d MISS (loaded) hits=%llu miss=%llu",
                slot, charId, (unsigned long long)g_cache_hits,
                (unsigned long long)g_cache_miss);
            return r;
        }
        // left CSS (battle reuses slots 1..7): drop stale cache tracking once.
        for (int c = kFirstCacheSlot; c < kNumSlots; ++c) g_slot_char[c] = -1;
    }

    if (!g_profile) return g_orig_PlayerLoad(slot, charId);
    g_sound_ticks = 0; g_sound_cnt = 0; g_charload_ms = 0.0; g_cleanup_ticks = 0;
    LARGE_INTEGER s; QueryPerformanceCounter(&s);
    int r = g_orig_PlayerLoad(slot, charId);
    LARGE_INTEGER e; QueryPerformanceCounter(&e);
    double total_ms   = (e.QuadPart - s.QuadPart) * 1000.0 / g_qpf.QuadPart;
    double snd_ms     = g_sound_ticks   * 1000.0 / g_qpf.QuadPart;
    double cleanup_ms = g_cleanup_ticks * 1000.0 / g_qpf.QuadPart;
    if (total_ms >= 0.5) {  // skip the no-op "already loaded" returns
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSSPROF] move#%u slot=%d char=%d: total=%.2fms cleanup=%.2fms "
            "charload=%.2fms sound=%.2fms(%u) sprite+pal+io=%.2fms other=%.2fms",
            ++g_move_seq, slot, charId, total_ms, cleanup_ms, g_charload_ms,
            snd_ms, g_sound_cnt, g_charload_ms - snd_ms,
            total_ms - cleanup_ms - g_charload_ms);
    }
    return r;
}

// ── hook: free the kept WAV when the sound object is released ────────────
void __cdecl Hook_Release(uint32_t* obj) {
    if (g_enabled) HeapCheck("release.entry");
    if (g_enabled && obj) {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_deferred.find(obj);
        if (it != g_deferred.end()) {
            // Unrealized slots hold the SHARED dummy -- must not let the engine
            // Release it. Zero them so the original release skips them. Realized
            // slots hold real buffers and are released normally.
            for (int i = 0; i < it->second.voices; ++i)
                if (obj[4 + i] == g_dummy_buffer) obj[4 + i] = 0;
            g_deferred.erase(it);  // frees d.wav
        }
    }
    if (g_enabled) HeapCheck("release.before_orig");
    g_orig_Release(obj);  // null-checks buffers, LocalFree(obj)
    if (g_enabled) HeapCheck("release.after_orig");
}

// ── diagnostic (heapchk only): pin the exact GlobalFree that smashes ─────
// The bracket said the heap dies in resource_cleanup_manager's loop body,
// whose only heap op is GlobalFree(sound_item.field0). Bracket the function
// so we know we're inside it, then validate after each GlobalFree it issues
// and log the first pointer whose free corrupts -- with its size (0 == bad
// handle / double-free) and whether it aliases one of our deferred objects.
HGLOBAL __cdecl Hook_Cleanup(int a1) {
    HeapCheck("cleanup.entry");
    g_in_cleanup = true;
    LARGE_INTEGER s; if (g_profile) QueryPerformanceCounter(&s);
    HGLOBAL r = g_orig_Cleanup(a1);
    if (g_profile) {
        LARGE_INTEGER e; QueryPerformanceCounter(&e);
        g_cleanup_ticks += (e.QuadPart - s.QuadPart);  // free storm: old char teardown
    }
    g_in_cleanup = false;
    HeapCheck("cleanup.exit");
    return r;
}

HGLOBAL __stdcall Hook_GlobalFree(HGLOBAL h) {
    if (!(g_in_cleanup && g_heapchk && !g_heap_was_bad)) return g_orig_GlobalFree(h);
    SIZE_T sz = h ? GlobalSize(h) : 0;            // 0 => invalid handle (already freed?)
    bool is_obj = false, is_dummy = (reinterpret_cast<uint32_t>(h) == g_dummy_buffer);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        is_obj = g_deferred.find(reinterpret_cast<uint32_t*>(h)) != g_deferred.end();
    }
    HGLOBAL r = g_orig_GlobalFree(h);
    if (!HeapValidate(GetProcessHeap(), 0, nullptr)) {
        g_heap_was_bad = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CssFastSound: GlobalFree(%p) size=%zu SMASHED heap during "
                     "cleanup (deferred_obj=%d dummy=%d ret=%p)",
                     h, sz, (int)is_obj, (int)is_dummy, r);
        // Hex-dump the block tail + into the next block's header, where a
        // forward overflow lands. Pattern (RIFF / vtable / ascii) IDs the
        // culprit. Read only committed memory.
        auto dumpAt = [](const char* tag, const uint8_t* base, intptr_t off, size_t n) {
            const uint8_t* p = base + off;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "CssFastSound:   %s @%p: <not committed>", tag, p);
                return;
            }
            char line[160]; int o = 0;
            for (size_t i = 0; i < n && o < (int)sizeof(line) - 4; ++i)
                o += SDL_snprintf(line + o, sizeof(line) - o, "%02x ", p[i]);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CssFastSound:   %s @%p: %s", tag, p, line);
        };
        const uint8_t* b = reinterpret_cast<const uint8_t*>(h);
        dumpAt("hdr-8 ", b, -8, 16);                 // this block's heap header + start
        dumpAt("tail  ", b, (intptr_t)sz - 16, 16);  // user-data tail
        dumpAt("next  ", b, (intptr_t)sz, 32);        // into next block (overflow target)
    }
    return r;
}

}  // namespace

void CssFastSound_Install() {
    const char* ef = std::getenv("FM2K_FPK_CSS_FASTSOUND");
    const char* ep = std::getenv("FM2K_FPK_CSS_PROFILE");
    const char* eh = std::getenv("FM2K_FPK_CSS_FASTSOUND_HEAPCHK");
    const char* ec = std::getenv("FM2K_CSS_CACHE");
    const char* ea = std::getenv("FM2K_CSS_ASYNC");
    bool want_fastsound = (ef && ef[0] == '1');
    g_profile = (ep && ep[0] == '1');
    g_heapchk = (eh && eh[0] == '1');
    g_cache   = (ec && ec[0] == '1');
    g_async   = (ea && ea[0] == '1');
    // async needs the CSS-sound-skip too (worker must not touch DirectSound).
    if (g_async) g_cache = true;
    if (!want_fastsound && !g_profile && !g_heapchk && !g_cache && !g_async) return;
    if (g_profile || g_heapchk) QueryPerformanceFrequency(&g_qpf);

    auto hook = [](uintptr_t addr, void* detour, void** orig, const char* name) -> bool {
        if (MH_CreateHook(reinterpret_cast<void*>(addr), detour, orig) != MH_OK ||
            MH_QueueEnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "CssFastSound: failed to hook %s @0x%zx", name, addr);
            return false;
        }
        return true;
    };

    bool ok = true;
    // Hook_Alloc serves both fastsound (deferral) and the profiler (timing).
    // When fastsound is off, g_enabled stays false and Hook_Alloc just times +
    // passes through, so the profiler measures the REAL buffer-creation cost.
    if (want_fastsound || g_profile || g_cache || g_async) {
        ok &= hook(kAddr_AllocateSoundBufferCopiesFromMem, reinterpret_cast<void*>(Hook_Alloc),
                   reinterpret_cast<void**>(&g_orig_Alloc), "AllocateSoundBufferCopiesFromMem");
    }
    if (want_fastsound) {
        ok &= hook(kAddr_PlaySoundFromBufferArray, reinterpret_cast<void*>(Hook_Play),
                   reinterpret_cast<void**>(&g_orig_Play), "PlaySoundFromBufferArray");
        ok &= hook(kAddr_ReleaseSoundBufferCopies, reinterpret_cast<void*>(Hook_Release),
                   reinterpret_cast<void**>(&g_orig_Release), "ReleaseSoundBufferCopies");
    }

    // character_data_loader is shared by profiler (timing) and heapchk (bracket).
    if (g_profile || g_heapchk) {
        hook(kAddr_CharacterDataLoader, reinterpret_cast<void*>(Hook_CharLoad),
             reinterpret_cast<void**>(&g_orig_CharLoad), "character_data_loader");
    }
    if (g_profile || g_cache) {
        hook(kAddr_PlayerDataFileLoader, reinterpret_cast<void*>(Hook_PlayerLoad),
             reinterpret_cast<void**>(&g_orig_PlayerLoad), "player_data_file_loader");
    }
    if (g_async) {
        hook(kAddr_ProcessCharSelectHandler, reinterpret_cast<void*>(Hook_CssHandler),
             reinterpret_cast<void**>(&g_orig_CssHandler), "ProcessCharacterSelectHandler");
        g_async_worker = std::thread(AsyncWorkerMain);
        g_async_worker.detach();   // lives for the process; CSS-only writes to spare slots
    }
    if (g_profile || g_heapchk) {
        // profiler times it (free storm); heapchk brackets it.
        hook(kAddr_ResourceCleanupManager, reinterpret_cast<void*>(Hook_Cleanup),
             reinterpret_cast<void**>(&g_orig_Cleanup), "resource_cleanup_manager");
    }
    if (g_heapchk) {
        if (void* pgf = reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleA("kernel32.dll"), "GlobalFree"))) {
            if (MH_CreateHook(pgf, reinterpret_cast<void*>(Hook_GlobalFree),
                              reinterpret_cast<void**>(&g_orig_GlobalFree)) == MH_OK)
                MH_QueueEnableHook(pgf);
        }
    }

    if (want_fastsound && ok) {
        g_enabled = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CssFastSound: lazy DirectSound buffers ENABLED "
                    "(defer char sound-buffer creation until first play)");
    } else if (want_fastsound) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "CssFastSound: install failed; running normal sound loads");
    }
    if (g_profile) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CssFastSound: CSS load PROFILER on (fastsound=%d) -- "
                    "[CSSPROF] per-cursor-move timing", (int)want_fastsound);
    }
    if (g_cache && !g_async) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CssCharCache: ENABLED -- spare slots 2..7 cache recently-"
                    "viewed chars; revisits are instant blob swaps (visuals "
                    "correct; CSS char audio skipped for now). [CSSCACHE].");
    }
    if (g_async) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CssAsyncLoad: ENABLED -- char loads run on a worker into a "
                    "spare slot, swapped into the preview on the main thread; "
                    "the CSS frame never blocks. [CSSASYNC] per swap.");
    }
}

#else  // ENGINE_FM95

void CssFastSound_Install() {}  // FM95 prototype: addresses differ; not supported

#endif
