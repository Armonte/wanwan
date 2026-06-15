// Simplified hooks - detect battle mode transitions, delegate to netplay
// Sync barrier: block game until both clients connected (CCCaster-style)
#include "hooks.h"
#include "round_events.h"     // C3.5 — vs_round_function detour install
#include "css_autoconfirm.h"  // CSS lock-and-confirm for offline replay playback
#include "css_fastsound.h"    // FM2K_FPK_CSS_FASTSOUND: lazy DSound buffers (CSS dip fix)
#include "per_game_patches.h" // damage multiplier MinHook + team-size override
#include "render_simd.h"      // FM2K_BLIT_SIMD: blit + case -10 blur reimplementation
#include "globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <list>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "netplay.h"
#include "control_channel.h"
#include "../netplay/game_hash.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop — owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
#include "../ui/screenshot.h"               // FM2KCapture::SaveScreenshot for the auto-banner pipeline
#include "../ui/fc_hud.h"                   // IsChatInputActive — gate local input during typing
#include "../vfs/fpk_reader.h"              // FM2K_FPK_VFS: inflate a slim .fpk -> original asset bytes
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cfloat>   // _controlfp_s, _PC_53, _MCW_PC, _RC_NEAR, _MCW_RC, _MCW_EM
#include <cstdint>
#include <string>

// Pin the x87 FPU control word to a fixed precision + rounding mode on the
// game thread. IDA audit found the binary never calls _controlfp / fldcw and
// DirectDraw's SetCooperativeLevel is invoked without DDSCL_FPUPRESERVE, so
// the default precision is whatever DirectDraw/driver/OS happens to leave.
// That varies across machines and is almost certainly why peer simulations
// diverge on movement (velocity, collision, normalization all use floats).
// Call this before every gameplay tick to override any mid-frame changes.
// MXCSR bit layout (SSE control/status register):
//   bit 15 FZ (flush-to-zero)
//   bits 13-14 RC (round control): 00 nearest, 01 down, 10 up, 11 truncate
//   bits 7-12 exception masks (we set all = masked)
//   bit 6 DAZ (denormals-are-zero)
//   bits 0-5 exception flags (sticky, we clear)
// We want: round-to-nearest-even, all exceptions masked, no FZ/DAZ, flags clear.
// Value 0x1F80 is the x86 default but we pin it explicitly to ensure both
// peers use the same value regardless of prior state.
static inline void SetMXCSR(unsigned int v) {
    __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

static inline void PinFPUControlWord() {
    unsigned int cur = 0;
    // x87: 53-bit precision, round-to-nearest-even, all exceptions masked.
    _controlfp_s(&cur, _PC_53 | _RC_NEAR | _MCW_EM,
                       _MCW_PC | _MCW_RC | _MCW_EM);
    // SSE: also pin MXCSR. FM2K's hit-detection and physics likely emit SSE
    // float ops under -mfpmath or vectorizer, and MXCSR rounding mode is
    // independent of the x87 control word. Both peers must agree.
    // #63 TEST: FM2K_FTZ_TEST=1 enables FZ(0x8000)+DAZ(0x0040) to check whether
    // denormal stalls in the game's render math are the Robot Heroes slowdown.
    static const unsigned int s_mxcsr = []{
        const char* v = std::getenv("FM2K_FTZ_TEST");
        return (v && v[0] == '1') ? 0x9FC0u : 0x1F80u;
    }();
    SetMXCSR(s_mxcsr);
}

// Deterministic timeGetTime: during an active GekkoNet battle session the
// return value is derived from the authoritative advance count, NOT wall
// clock. main_game_loop writes timeGetTime() into g_last_frame_time @
// 0x447DD4 every iteration, which lives inside our saved "afterimage_pool"
// region. If forward-sim wrote wall-clock T1 and replay-sim wrote T2 at
// the same frame, the saved afterimage_pool diverges by that timestamp
// byte — this is the exact "REPLAY DIFF AfterimagePool +0x4A4" signature
// we caught at f=9 in the stress test.
//
// Virtual clock is advanced by 10 ms EACH TIME an AdvanceEvent completes
// (see netplay.cpp). Within a single main_game_loop iteration the game
// polls timeGetTime() multiple times — we return the same value on every
// call until the next advance. Forward-sim and replay-sim both consume
// the same advance sequence, so both produce identical virtual timestamps
// at the same logical frame.
//
// Outside an active session we pass through — menus/CSS rely on real wall
// clock for music/animation pacing, and determinism doesn't matter there.
extern bool Netplay_IsActive();
using timeGetTime_t = DWORD(WINAPI*)();
static timeGetTime_t original_timeGetTime = nullptr;

// FM95 vs-mode random-stage hook.
// FM2K's selected_stage scalar (0x43010c) doesn't exist on FM95 — vs/story
// mode reads stage_id from g_char_stage_per_round[char][round] table at
// LoadStageFile_alt call time. To override, we trampoline the function
// itself and rewrite arg0 (stage_id) when random-stage is enabled. The
// rolled value comes from the same xorshift sequence the FM2K random-stage
// block uses in netplay.cpp; here we just consume the next pre-rolled
// value via Netplay_RandomStagePeekRoll() and write it through.
//
// Practice mode uses g_practice_stage_id (which our existing random-stage
// write target ADDR_SELECTED_STAGE points at on FM95), so this hook is
// only load-bearing for vs/story-mode random-stage. Practice mode still
// works via the direct memory write — this hook is a NO-OP in that path
// because the override comes back as the same value.
using LoadStageFileAlt_t = int(__cdecl*)(int, int, int);
static LoadStageFileAlt_t original_LoadStageFileAlt = nullptr;
extern "C" uint32_t Netplay_PeekNextRolledStage();  // 0xFFFFFFFF if random off
// Forward decl — definition at line ~280. Used by Hook_CreateFileA/W to
// register virtual-file aliases for .player loads (other agent's WIP).
extern "C" void MaybeRegisterPlayerVFileA(HANDLE h, LPCSTR name);
static int __cdecl Hook_LoadStageFileAlt(int stage_id, int slot, int palette) {
    if constexpr (FM2K::kIsFM95) {
        const uint32_t override_id = Netplay_PeekNextRolledStage();
        if (override_id != 0xFFFFFFFFu && (int)override_id != stage_id) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM95 random-stage: LoadStageFile_alt override %d -> %u",
                stage_id, override_id);
            stage_id = (int)override_id;
        }
    }
    return original_LoadStageFileAlt
        ? original_LoadStageFileAlt(stage_id, slot, palette)
        : 0;
}
uint32_t g_virtual_time_ms = 0;  // bumped by 10 per AdvanceEvent in netplay.cpp

static DWORD WINAPI Hook_timeGetTime() {
    // Host: virtual clock during an active GekkoNet session so the per-peer
    // simulation evolves on a deterministic 10 ms/frame schedule.
    // Spectator: same — must return virtual clock once playback is driving
    // the sim, otherwise game code that consumes timeGetTime (animations,
    // particle pacing, etc.) sees wall-clock time and diverges from the
    // host's recorded execution every single frame. RunSpectatorTick is
    // responsible for bumping g_virtual_time_ms each successful advance.
    if (Netplay_IsActive() || SpectatorNode_IsPlayingBack()) {
        return g_virtual_time_ms;
    }
    return original_timeGetTime ? original_timeGetTime() : 0;
}

// ============================================================================
// CreateFile share-mode override
// ============================================================================
// FM2K opens character files (`.player`, etc.) with dwShareMode=0 — exclusive.
// When two instances launch from the same folder, the second hits
// ERROR_SHARING_VIOLATION ("Player Open error[…]"). Force-OR the shared-read
// flags so multiple readers can coexist. Writes are still serialized by
// the OS — we only widen sharing, never narrow it.
//
// Hooked at the kernel32 entry points; both A and W variants because old
// VC runtimes route through CreateFileA but newer code paths may use W.
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static CreateFileA_t original_CreateFileA = nullptr;
static CreateFileW_t original_CreateFileW = nullptr;

static constexpr DWORD kRelaxedShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// FPK VFS redirect (defined after the temp-inflate helpers further down). If the
// asset has a sibling ".fpk", returns a handle to its inflated temp file; else
// INVALID_HANDLE_VALUE so the caller falls through to the normal open.
extern "C" HANDLE FpkTryRedirectOpenA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl);

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    // Log the first ~32 CreateFileA calls so we can trace startup file I/O.
    // FM95 games (CPW) fail to find their .kgt because Windows resolves
    // CreateFileA paths via the system codepage (1252 on US Windows), NOT
    // via GetACP. Even with our GetACP hook, ＣＰＷ.kgt's SJIS bytes get
    // mangled to a non-existent path. Fix: translate SJIS -> wide via
    // CP932 ourselves and forward to CreateFileW.
    static int s_logged = 0;
    if (s_logged < 32 && name) {
        // Convert SJIS path -> wide -> UTF-8 for the log so multi-byte
        // filenames render in any UTF-8-aware viewer instead of garbling.
        // Falls back to the raw bytes if conversion fails.
        char utf8[1024] = {0};
        wchar_t wide[512] = {0};
        if (MultiByteToWideChar(932, 0, name, -1, wide, 512) > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CreateFileA[%d]: '%s'", s_logged,
                    utf8[0] ? utf8 : name);
        ++s_logged;
    }
    // FPK VFS: redirect an asset open to its inflated temp file (built on disk,
    // read by the engine at native speed -- nothing held in our 2GB heap).
    if (name) {
        HANDLE rd = FpkTryRedirectOpenA(name, access, share, sa, disp, flags, tmpl);
        if (rd != INVALID_HANDLE_VALUE) return rd;
    }
    // SJIS -> wide -> CreateFileW path. Required for Japanese-named files in
    // BOTH FM95 (CPW.exe / ＣＰＷ.kgt) and FM2K (e.g. Otepuri's
    // 「おてんばプリンセスの大冒険第0話+.exe」 / .kgt). CreateFileA's kernel-side
    // path conversion uses the SYSTEM ANSI codepage (1252 on US Windows), NOT
    // our user-mode GetACP hook, so SJIS bytes get reinterpreted as CP1252
    // and the file lookup fails silently. ASCII paths round-trip through
    // CP932 unchanged, so this is safe for all callers — and CreateFileA
    // internally just calls CreateFileW after its own path conversion anyway.
    if (name) {
        int wlen = MultiByteToWideChar(932, 0, name, -1, nullptr, 0);
        if (wlen > 1) {
            std::vector<wchar_t> wide(static_cast<size_t>(wlen));
            if (MultiByteToWideChar(932, 0, name, -1, wide.data(), wlen) > 0) {
                HANDLE h = CreateFileW(wide.data(), access,
                                       share | kRelaxedShareMode,
                                       sa, disp, flags, tmpl);
                if (h != INVALID_HANDLE_VALUE) {
                    extern void MaybeRegisterPlayerVFile(HANDLE, LPCWSTR);
                    MaybeRegisterPlayerVFile(h, wide.data());
                    return h;
                }
                // W path failed — fall through to A so a system-codepage
                // path (e.g. ASCII like "log.txt") still has a chance.
            }
        }
    }
    HANDLE h = original_CreateFileA(name, access, share | kRelaxedShareMode,
                                    sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE && name) {
        MaybeRegisterPlayerVFileA(h, name);
    }

    // KGT fallback: if both the W and A paths failed for a *.kgt request,
    // scan the same directory for any *.kgt and open that. Handles the
    // case where the exe has a hardcoded kgt filename baked in by the
    // FM2K editor (e.g. "Bishi Bashi Touhou 1.kgt") but the user's local
    // copy was renamed to a JP basename ("びしばし東方.kgt"). Single-kgt
    // FM2K installs are the norm, so picking the first match is safe.
    if (h == INVALID_HANDLE_VALUE && name) {
        size_t name_len = SDL_strlen(name);
        bool is_kgt = (name_len >= 4 && name[name_len - 4] == '.' &&
                       (name[name_len - 3] == 'k' || name[name_len - 3] == 'K') &&
                       (name[name_len - 2] == 'g' || name[name_len - 2] == 'G') &&
                       (name[name_len - 1] == 't' || name[name_len - 1] == 'T'));
        if (is_kgt) {
            int wlen = MultiByteToWideChar(932, 0, name, -1, nullptr, 0);
            if (wlen > 1) {
                std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
                if (MultiByteToWideChar(932, 0, name, -1, wpath.data(), wlen) > 0) {
                    // Carve off the directory component, default to "" (CWD).
                    std::wstring dir(wpath.data());
                    size_t sep = dir.find_last_of(L"\\/");
                    dir = (sep != std::wstring::npos) ? dir.substr(0, sep + 1)
                                                      : std::wstring(L"");
                    std::wstring search = dir + L"*.kgt";
                    WIN32_FIND_DATAW fd = {};
                    HANDLE find = FindFirstFileW(search.c_str(), &fd);
                    if (find != INVALID_HANDLE_VALUE) {
                        std::wstring alt = dir + fd.cFileName;
                        FindClose(find);
                        HANDLE h2 = CreateFileW(alt.c_str(), access,
                                                share | kRelaxedShareMode,
                                                sa, disp, flags, tmpl);
                        if (h2 != INVALID_HANDLE_VALUE) {
                            char utf8[1024] = {0};
                            WideCharToMultiByte(CP_UTF8, 0, alt.c_str(), -1,
                                                utf8, sizeof(utf8), nullptr, nullptr);
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "KgtFallback: '%s' not found, opened '%s' instead",
                                name, utf8);
                            extern void MaybeRegisterPlayerVFile(HANDLE, LPCWSTR);
                            MaybeRegisterPlayerVFile(h2, alt.c_str());
                            return h2;
                        }
                    }
                }
            }
        }
    }
    return h;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    HANDLE h = original_CreateFileW(name, access, share | kRelaxedShareMode,
                                    sa, disp, flags, tmpl);
    // VFile slurp registration handled below via the path-aware extension.
    extern void MaybeRegisterPlayerVFile(HANDLE, LPCWSTR);
    if (h != INVALID_HANDLE_VALUE && name) {
        MaybeRegisterPlayerVFile(h, name);
    }
    return h;
}

// ============================================================================
// Fast .player loader: VFile syscall-collapse intercept
// ============================================================================
// FM2K's `character_data_loader@0x403600` issues 200+ tiny ReadFile syscalls
// per character (one per sound header + one per sound payload). On modern
// Windows each syscall costs ~150 µs of kernel ping-pong; for a 100-150
// sound character that's 30 ms of syscall overhead PER LOAD — and a CSS
// cursor flick triggers one of these synchronously every time the user
// moves to a new character.
//
// This intercept slurps the entire .player file in ONE big ReadFile on
// open, then serves all subsequent ReadFile / SetFilePointer / etc. calls
// from RAM. The original loader runs unchanged but its inner loop becomes
// memcpy-bound (~50 ns per "syscall") instead of kernel-bound. Net effect:
// ~30-62 ms cold first hover → ~5 ms (NVMe disk-read time only). Repeat
// hovers within a session hit the OS page cache for <1 ms.
//
// Gated on `FM2K_FAST_PLAYER_LOAD=1`. Off by default so behavior is bit-
// identical to vanilla until explicitly opted in via the launcher dev
// panel. Doesn't change parse semantics — every byte the original loader
// would see, it still sees, just from RAM not disk.

namespace {

struct VFile {
    // Shared so the path-keyed .fpk cache (g_fpk_cache) and the active handle
    // reference ONE allocation -- a cache hit costs a pointer copy, not a
    // 100MB memcpy, which matters in the game's 32-bit address space.
    std::shared_ptr<std::vector<uint8_t>> buf;
    size_t offset = 0;
};

// Toggle initialized at hook install time from FM2K_FAST_PLAYER_LOAD env var.
bool g_fast_player_load = false;

// Toggle initialized at hook install time from FM2K_FPK_VFS env var. When set,
// MaybeRegisterPlayerVFileA inflates a sibling "<path>.fpk" via fpk_reconstruct
// instead of slurping the original .player/.stage/.demo. Independent of the
// fast-player gate, but it shares the same VFile serve-hooks, so the serve
// fast-paths test (g_fast_player_load || g_fpk_vfs). See InstallFileHooks.
bool g_fpk_vfs = false;

// Either gate activates the VFile machinery (register + serve from RAM).
inline bool VfsActive() { return g_fast_player_load || g_fpk_vfs; }

// Ceiling for an inflated .fpk / slurped asset held in RAM. Inflated originals
// reach ~240MB (BOSS_Miriann.player); cap generously but bound the single 32-bit
// allocation so a runaway file can't exhaust the game's address space. The old
// 64MB .player-only slurp cap was too small for .stage/.demo + inflated content.
constexpr long long kVfsMaxBytes = 768LL * 1024 * 1024;

// Map of OS handle → buffered .player content. Real Windows handles are
// returned to the game (no synthetic-handle plumbing) so any other API
// the game might call on the handle (GetFileSize, etc.) still works.
std::mutex                                          g_vfile_mtx;
std::unordered_map<HANDLE, std::unique_ptr<VFile>>  g_vfiles;

// Path-keyed LRU cache of inflated .fpk results. player_data_file_loader
// (@0x4039F0) re-opens each asset per load event, and CSS auto-browse re-opens
// the same .player as the cursor cycles -- a fresh inflate is ~0.3-4.4s, so the
// observed 2-4x consecutive re-inflations were pure waste. Caching by .fpk path
// turns a re-open into a shared-pointer copy. Bounded by total bytes to stay
// safe in the game's 32-bit address space; entries share ownership with any
// live VFile, so eviction never frees a buffer still in use. Guarded by
// g_vfile_mtx.
struct FpkCacheEntry {
    std::string                            path;
    std::shared_ptr<std::vector<uint8_t>>  bytes;
};
std::list<FpkCacheEntry>  g_fpk_cache;          // front = most-recently used
size_t                    g_fpk_cache_bytes = 0;
// Soft byte cap, tunable via FM2K_FPK_CACHE_MB. The game is often a 32-bit, NON
// large-address-aware process (RoHe = 2GB user space) with ~100MB characters, so
// this must stay well under that or prefetch starves the game and it OOM-crashes.
// 384MB holds ~4 of RoHe's big characters with safe headroom.
size_t                    g_fpk_cache_cap = 384u * 1024 * 1024;

// Set while a SYNCHRONOUS (hover) inflate is running, so the background prefetch
// worker yields instead of allocating a second ~100MB buffer concurrently --
// concurrent big allocations in a 2GB process were what crashed RoHe on CSS.
std::atomic<bool>         g_foreground_inflating{false};

// Background prefetch. The engine loads each character fully on every CSS cursor
// move (player_data_file_loader @0x4039F0), so a cold hover pays the 0.3-0.9s
// inflate synchronously. We enqueue the whole sibling ".player.fpk" roster the
// first time one is opened and inflate them into the cache on a low-priority
// worker, so by the time the cursor reaches a character it is already a cache
// hit. See InflateFpkCached / PrefetchWorker / EnqueueRosterPrefetch.
std::list<std::string>    g_prefetch_queue;
std::mutex                g_prefetch_mtx;
std::condition_variable   g_prefetch_cv;
bool                      g_prefetch_stop = false;
bool                      g_prefetch_roster_enqueued = false;
bool                      g_prefetch_started = false;

// Aggregate timing across the session so we can see the win in the log.
LONGLONG g_qpc_freq      = 0;
LONGLONG g_qpc_load_sum  = 0;
uint32_t g_qpc_load_count = 0;

// FM2K asset extensions served by the file-VFS: character data, stage data, and
// the menu/intro/ending demo scripts. All three share player_data_file_loader's
// read pattern, so the same slurp / .fpk-inflate path applies to each.
inline bool ends_with_asset_a(const char* path) {
    if (!path) return false;
    size_t n = std::strlen(path);
    static const char* kExts[] = { ".player", ".stage", ".demo" };
    for (const char* e : kExts) {
        size_t el = std::strlen(e);
        if (n >= el && _strnicmp(path + n - el, e, el) == 0) return true;
    }
    return false;
}

inline bool ends_with_asset_w(LPCWSTR path) {
    if (!path) return false;
    size_t n = wcslen(path);
    static const wchar_t* kExts[] = { L".player", L".stage", L".demo" };
    for (const wchar_t* e : kExts) {
        size_t el = wcslen(e);
        if (n >= el && _wcsnicmp(path + n - el, e, el) == 0) return true;
    }
    return false;
}

}  // anon

namespace {

// Read an entire file off disk into `out` via a FRESH handle (NOT the game's
// handle). Used for the sibling "<path>.fpk": reusing the game's .player handle
// would read the wrong file. Returns false on open/size/read failure or if the
// file exceeds kVfsMaxBytes. The .fpk itself is the slim packed image (a few MB),
// so the cap here is a sanity bound, not the inflated-size bound.
bool ReadWholeFileFresh(const char* path, std::vector<uint8_t>& out) {
    HANDLE fh = original_CreateFileA
        ? original_CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
        : ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    LARGE_INTEGER sz_li{};
    if (::GetFileSizeEx(fh, &sz_li) &&
        sz_li.QuadPart > 0 && sz_li.QuadPart <= kVfsMaxBytes) {
        DWORD sz = static_cast<DWORD>(sz_li.QuadPart);
        try {
            out.resize(sz);
        } catch (...) {
            ::CloseHandle(fh);
            return false;
        }
        DWORD got = 0;
        BOOL r = ::ReadFile(fh, out.data(), sz, &got, nullptr);
        ok = (r && got == sz);
        if (!ok) out.clear();
    }
    ::CloseHandle(fh);
    return ok;
}

// Serialize inflations so only ONE ~100MB transient buffer exists at a time --
// the concurrent foreground+worker inflate was what OOM-crashed RoHe (2GB).
std::mutex g_inflate_mtx;

// True if the inflated temp exists, is non-empty, and is at least as new as the
// .fpk it was built from (a re-packed .fpk invalidates a stale temp).
bool TempIsFresh(const std::string& temp, const std::string& fpk) {
    WIN32_FILE_ATTRIBUTE_DATA td{}, fd{};
    if (!::GetFileAttributesExA(temp.c_str(), GetFileExInfoStandard, &td)) return false;
    if ((((uint64_t)td.nFileSizeHigh << 32) | td.nFileSizeLow) == 0) return false;
    if (!::GetFileAttributesExA(fpk.c_str(), GetFileExInfoStandard, &fd)) return true;
    return ::CompareFileTime(&td.ftLastWriteTime, &fd.ftLastWriteTime) >= 0;
}

// Write `data` to `path` (CREATE_ALWAYS) in 8MB chunks. ::WriteFile is not hooked.
bool WriteWholeFile(const std::string& path, const std::vector<uint8_t>& data) {
    HANDLE fh = original_CreateFileA
        ? original_CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
        : ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = (DWORD)((data.size() - off > 8u * 1024 * 1024)
                              ? 8u * 1024 * 1024 : (data.size() - off));
        DWORD wrote = 0;
        if (!::WriteFile(fh, data.data() + off, chunk, &wrote, nullptr) ||
            wrote != chunk) { ok = false; break; }
        off += wrote;
    }
    ::CloseHandle(fh);
    return ok;
}

// Inflate <fpk_path> to a sibling temp "<fpk_path>.inflated" if missing/stale and
// return the temp path (or "" on failure). The engine then reads the temp as a
// NORMAL file -- the inflated bytes live on disk (pageable, OS page-cached), NOT
// in our heap, so we never hold a ~100MB buffer and can't OOM the 2GB game; and
// the engine's read runs at native (vanilla) speed. Serialized via g_inflate_mtx
// so at most one transient inflate buffer exists. *was_built=true if we inflated
// (cold), false if the temp was already fresh (the fast path).
std::string EnsureInflatedTemp(const std::string& fpk_path, bool* was_built) {
    std::string temp = fpk_path + ".inflated";
    if (TempIsFresh(temp, fpk_path)) { if (was_built) *was_built = false; return temp; }

    std::lock_guard<std::mutex> lk(g_inflate_mtx);
    if (TempIsFresh(temp, fpk_path)) { if (was_built) *was_built = false; return temp; }

    std::vector<uint8_t> raw;
    if (!ReadWholeFileFresh(fpk_path.c_str(), raw)) return {};
    std::string err;
    std::vector<uint8_t> inflated = fpk_reconstruct(raw.data(), raw.size(), &err);
    if (inflated.empty() || (long long)inflated.size() > kVfsMaxBytes) return {};

    std::string part = temp + ".part";
    bool ok = WriteWholeFile(part, inflated);
    inflated.clear();
    inflated.shrink_to_fit();             // release the ~100MB transient now
    if (!ok) { ::DeleteFileA(part.c_str()); return {}; }
    ::DeleteFileA(temp.c_str());
    if (!::MoveFileA(part.c_str(), temp.c_str())) { ::DeleteFileA(part.c_str()); return {}; }
    if (was_built) *was_built = true;
    return temp;
}

// Background worker: drains g_prefetch_queue, building each .fpk's inflated temp
// file on DISK at below-normal priority. Disk has no 2GB wall, so the whole
// roster can be pre-built; each hover then just opens an existing temp.
void PrefetchWorker() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(g_prefetch_mtx);
            g_prefetch_cv.wait(lk, [] {
                return g_prefetch_stop || !g_prefetch_queue.empty();
            });
            if (g_prefetch_stop) return;
            path = std::move(g_prefetch_queue.front());
            g_prefetch_queue.pop_front();
        }
        // Yield to an in-flight hover inflate so we never run two decodes at
        // once (g_inflate_mtx also serializes, but yielding avoids the stall).
        while (g_foreground_inflating.load(std::memory_order_relaxed)) {
            ::Sleep(15);
            if (g_prefetch_stop) return;
        }
        EnsureInflatedTemp(path, nullptr);  // build temp on disk (idempotent)
        ::Sleep(5);  // gentle pacing -- prefetch is a nicety, never a priority
    }
}

// On the first ".player.fpk" open, enumerate the sibling roster
// ("<dir>\\*.player.fpk") and enqueue every entry for background prefetch, so
// CSS browsing finds each character already decoded. Runs once per session.
void EnqueueRosterPrefetch(const char* player_path) {
    {
        std::lock_guard<std::mutex> lk(g_prefetch_mtx);
        if (g_prefetch_roster_enqueued) return;
        g_prefetch_roster_enqueued = true;
    }
    std::string p(player_path);
    size_t slash = p.find_last_of("\\/");
    std::string dir = (slash == std::string::npos) ? std::string()
                                                   : p.substr(0, slash + 1);
    std::string pattern = dir + "*.player.fpk";

    WIN32_FIND_DATAA fd{};
    HANDLE hf = ::FindFirstFileA(pattern.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    std::list<std::string> found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        found.push_back(dir + fd.cFileName);
    } while (::FindNextFileA(hf, &fd));
    ::FindClose(hf);

    {
        std::lock_guard<std::mutex> lk(g_prefetch_mtx);
        for (auto& f : found) g_prefetch_queue.push_back(std::move(f));
    }
    g_prefetch_cv.notify_all();
}

}  // anon

// FPK VFS redirect entry point (forward-declared near Hook_CreateFileA). If the
// engine opens an asset with a sibling ".fpk", ensure its inflated temp exists
// (building it on disk if needed) and return a handle to the TEMP -- the engine
// then reads a normal file at native speed. Also kicks the background roster
// prefetch. Returns INVALID_HANDLE_VALUE to fall through to the normal open.
extern "C" HANDLE FpkTryRedirectOpenA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    if (!g_fpk_vfs || !name || !ends_with_asset_a(name))
        return INVALID_HANDLE_VALUE;
    std::string fpk = std::string(name) + ".fpk";
    DWORD fa = ::GetFileAttributesA(fpk.c_str());
    if (fa == INVALID_FILE_ATTRIBUTES || (fa & FILE_ATTRIBUTE_DIRECTORY))
        return INVALID_HANDLE_VALUE;

    g_foreground_inflating.store(true, std::memory_order_relaxed);
    bool built = false;
    std::string temp = EnsureInflatedTemp(fpk, &built);
    g_foreground_inflating.store(false, std::memory_order_relaxed);

    EnqueueRosterPrefetch(name);  // background-build the rest of the roster

    if (temp.empty()) return INVALID_HANDLE_VALUE;
    HANDLE th = original_CreateFileA
        ? original_CreateFileA(temp.c_str(), access, share | kRelaxedShareMode,
                               sa, disp, flags, tmpl)
        : ::CreateFileA(temp.c_str(), access, share | kRelaxedShareMode,
                        sa, disp, flags, tmpl);
    if (th == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[FpkVfs] %s '%s'",
                built ? "built+served temp" : "served temp", name);
    return th;
}

// Fill a VFile and register the handle. Called from Hook_CreateFileA/W with the
// real OS handle returned by the original CreateFile.
//
// Two sources:
//   (1) FM2K_FPK_VFS + a sibling "<name>.fpk" exists -> inflate the .fpk via
//       fpk_reconstruct and serve those bytes (the original .player/.stage/.demo
//       on disk is never read). The game's loader reads the reconstructed
//       original-format bytes from RAM via the hooked ReadFile.
//   (2) otherwise, under FM2K_FAST_PLAYER_LOAD -> slurp the original file whole.
//
// Either way the bytes are keyed under the ORIGINAL file's handle `h` (the one
// the game holds). Failures leave the handle alone — the game's existing
// per-byte ReadFile path runs as a graceful fallback.
extern "C" void MaybeRegisterPlayerVFileA(HANDLE h, LPCSTR name) {
    if (!VfsActive()) return;
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    if (!ends_with_asset_a(name)) return;

    // FPK assets are served by the CreateFileA redirect (FpkTryRedirectOpenA ->
    // inflated temp file). Under FPK VFS we only reach here when the redirect did
    // NOT fire (no sibling .fpk, or it failed and fell through to the original
    // open), so there's nothing to do unless the legacy fast-load slurp is on.
    if (g_fpk_vfs && !g_fast_player_load) return;

    // ── (2) legacy slurp path (FM2K_FAST_PLAYER_LOAD) ──────────────────
    LARGE_INTEGER sz_li{};
    if (!::GetFileSizeEx(h, &sz_li)) return;
    if (sz_li.QuadPart <= 0 || sz_li.QuadPart > kVfsMaxBytes) return;

    DWORD sz = static_cast<DWORD>(sz_li.QuadPart);
    auto vf = std::make_unique<VFile>();
    try {
        vf->buf = std::make_shared<std::vector<uint8_t>>();
        vf->buf->resize(sz);
    } catch (...) {
        return;  // address-space exhaustion -> fall back to on-disk reads.
    }

    LARGE_INTEGER t0{}, t1{};
    if (g_qpc_freq) ::QueryPerformanceCounter(&t0);

    DWORD got = 0;
    BOOL ok = ::ReadFile(h, vf->buf->data(), sz, &got, nullptr);
    if (!ok || got != sz) return;

    if (g_qpc_freq) ::QueryPerformanceCounter(&t1);

    // Reset OS file position to 0 so anything that bypasses the
    // intercept (e.g. another DLL holding the same handle) sees the
    // start of the stream.
    LARGE_INTEGER zero{};
    LARGE_INTEGER cur{};
    ::SetFilePointerEx(h, zero, &cur, FILE_BEGIN);

    {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        g_vfiles[h] = std::move(vf);
    }

    if (g_qpc_freq) {
        LONGLONG dt_us = (t1.QuadPart - t0.QuadPart) * 1'000'000LL / g_qpc_freq;
        g_qpc_load_sum += dt_us;
        ++g_qpc_load_count;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[FastPlayer] slurp '%s' %u B in %lld us",
                    name, sz, dt_us);
    }
}

void MaybeRegisterPlayerVFile(HANDLE h, LPCWSTR name) {
    if (!VfsActive()) return;
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    if (!ends_with_asset_w(name)) return;
    // Reuse the A path. NOTE: the A path now also uses the name on the
    // filesystem (sibling "<name>.fpk" probe via GetFileAttributesA /
    // CreateFileA), so transcode to the ACTIVE ANSI code page (CP_ACP), not
    // UTF-8 -- the *A filesystem calls interpret bytes in CP_ACP. With the
    // game's CP932 locale spoof active this round-trips JP basenames correctly.
    char ansi[1024] = {0};
    ::WideCharToMultiByte(CP_ACP, 0, name, -1, ansi, sizeof(ansi), nullptr, nullptr);
    MaybeRegisterPlayerVFileA(h, ansi[0] ? ansi : "<wide>");
}

// ─── ReadFile / SetFilePointer / CloseHandle hooks ──────────────────────
// All four hot-path handle APIs get a fast lookup. Non-VFile handles fall
// through to the original via a single map.find() — typically ~30 ns,
// invisible against the work the game is doing on the same call.

using ReadFile_t          = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using SetFilePointer_t    = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using SetFilePointerEx_t  = BOOL (WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
using CloseHandle_t       = BOOL (WINAPI*)(HANDLE);

static ReadFile_t          original_ReadFile         = nullptr;
static SetFilePointer_t    original_SetFilePointer   = nullptr;
static SetFilePointerEx_t  original_SetFilePointerEx = nullptr;
static CloseHandle_t       original_CloseHandle      = nullptr;

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n,
                                 LPDWORD got, LPOVERLAPPED ov) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            DWORD remaining = (vf.offset >= vf.buf->size())
                              ? 0u
                              : (DWORD)(vf.buf->size() - vf.offset);
            DWORD avail = (n < remaining) ? n : remaining;
            if (avail && buf) {
                std::memcpy(buf, vf.buf->data() + vf.offset, avail);
                vf.offset += avail;
            }
            if (got) *got = avail;
            return TRUE;
        }
    }
    return original_ReadFile(h, buf, n, got, ov);
}

static DWORD WINAPI Hook_SetFilePointer(HANDLE h, LONG dist, PLONG hi,
                                        DWORD method) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            int64_t dist64 = dist;
            if (hi) {
                dist64 = (int64_t)((uint64_t)(uint32_t)dist
                                  | ((uint64_t)(uint32_t)*hi << 32));
            }
            int64_t newpos;
            switch (method) {
                case FILE_BEGIN:   newpos = dist64; break;
                case FILE_CURRENT: newpos = (int64_t)vf.offset + dist64; break;
                case FILE_END:     newpos = (int64_t)vf.buf->size() + dist64; break;
                default:           return INVALID_SET_FILE_POINTER;
            }
            if (newpos < 0) newpos = 0;
            if ((uint64_t)newpos > vf.buf->size()) newpos = vf.buf->size();
            vf.offset = (size_t)newpos;
            if (hi) *hi = (LONG)((uint64_t)newpos >> 32);
            return (DWORD)((uint64_t)newpos & 0xFFFFFFFFu);
        }
    }
    return original_SetFilePointer(h, dist, hi, method);
}

static BOOL WINAPI Hook_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist,
                                         PLARGE_INTEGER newpos_out,
                                         DWORD method) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            int64_t newpos;
            switch (method) {
                case FILE_BEGIN:   newpos = dist.QuadPart; break;
                case FILE_CURRENT: newpos = (int64_t)vf.offset + dist.QuadPart; break;
                case FILE_END:     newpos = (int64_t)vf.buf->size() + dist.QuadPart; break;
                default:           return FALSE;
            }
            if (newpos < 0) newpos = 0;
            if ((uint64_t)newpos > vf.buf->size()) newpos = vf.buf->size();
            vf.offset = (size_t)newpos;
            if (newpos_out) newpos_out->QuadPart = newpos;
            return TRUE;
        }
    }
    return original_SetFilePointerEx(h, dist, newpos_out, method);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        g_vfiles.erase(h);  // no-op if not a VFile handle
    }
    return original_CloseHandle(h);
}

// ============================================================================
// GAME MODE DETECTION
// ============================================================================

static uint32_t g_last_game_mode = 0;

// Engine-aware phase detection.
// FM2K encodes phase via g_game_mode magic numbers (2000=CSS, 3000+=Battle).
// FM95 keeps g_game_mode near 0/1/10 — phase lives inside per-object slots
// in the 256-entry pool (type==19 sub_state ∈ [0x28,0xC9] = CSS, type==16
// sub_state ∈ [10,31] = Battle). Walk the pool once per call; could be
// frame-cached if it shows up hot in profiles.
//
// The `mode` argument is preserved so existing call sites keep compiling
// without change. On FM2K it's still load-bearing; on FM95 it's ignored
// and we read the object pool directly.
namespace {
    enum class FM95Phase { Boot, Title, CSS, PostCSS, Battle, MatchEnd, Other };

    inline FM95Phase Fm95ClassifyPhase() {
        const uint8_t* pool = (const uint8_t*)FM2K::ADDR_OBJECT_POOL;
        for (size_t i = 0; i < FM2K::OBJECT_POOL_COUNT; ++i) {
            const uint8_t* slot = pool + i * FM2K::OBJECT_POOL_STRIDE;
            uint32_t type = *reinterpret_cast<const uint32_t*>(slot);
            if (type < 2) continue;            // 0=empty, 1=disabled
            uint32_t sub  = *reinterpret_cast<const uint32_t*>(slot + 108);
            if (type == 19) {                  // title_screen_state_machine
                if (sub >= 0x28 && sub <= 0xC9) return FM95Phase::CSS;
                return FM95Phase::Title;
            }
            if (type == 16) {                  // vs_round_function
                if (sub >= 10 && sub <= 31)    return FM95Phase::Battle;
                return FM95Phase::MatchEnd;
            }
            if (type == 21) return FM95Phase::PostCSS;
            if (type == 30 || type == 15) return FM95Phase::Boot;
        }
        return FM95Phase::Other;
    }
}

// Exported (non-static) so main_loop_trampoline.cpp's ClassifyPhase can use
// the same engine-aware logic. Forward-declared in hooks.h.
bool IsCSSMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode == 2000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::CSS;
    }
}

bool IsBattleMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode >= 3000 && mode < 4000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::Battle;
    }
}

// Battle sync state - ensures both clients start GekkoNet together.
// Exposed non-static so the trampoline (main_loop_trampoline.cpp) can see it;
// the trampoline replaces main_game_loop wholesale and needs to drive the
// battle-entry handshake.
bool g_battle_entry_signaled_pub = false;
#define g_battle_entry_signaled g_battle_entry_signaled_pub

// Called every frame to check for game mode transitions
// Public shim so the trampoline (main_loop_trampoline.cpp) can invoke the
// same transition detector the hooks use.
extern "C" void Hook_CheckGameModeTransition_Public();
static void CheckGameModeTransition();
extern "C" void Hook_CheckGameModeTransition_Public() { CheckGameModeTransition(); }

// =============================================================================
// SOCD (Simultaneous Opposite Cardinal Direction) cleaner
// =============================================================================
//
// Ported from CCCaster's filterSimulDirState() in
// targets/DllControllerUtils.hpp. Applied to the 11-bit FM2K input before
// returning from Hook_GetPlayerInput, AFTER the facing-direction swap so
// modes operate on game-internal F/B/U/D semantics rather than raw stick.
//
// FM2K bits: LEFT=0x001 RIGHT=0x002 UP=0x004 DOWN=0x008.
//
// Modes (FM2K_SOCD_MODE env var, default = 1):
//   0 — Default        L+R held → R wins  | U+D held → U wins
//   1 — L/R Cancel     L+R held → neutral | U+D held → U wins   <-- DEFAULT (Hitbox-style)
//   2 — U/D Cancel     L+R held → R wins  | U+D held → neutral
//   3 — Both Cancel    L+R held → neutral | U+D held → neutral
//   4 — Up Bias        L+R held → R wins  | U+D held → U wins   (= 0 today)
//   5 — Hitbox+Up      L+R held → neutral | U+D held → U wins   (= 1 today)
//
// Modes 4/5 are kept for CCCaster compat; behavior currently matches 0/1
// because FM2K already drops DOWN on U+D (no separate "down-wins" branch
// to disambiguate). If we want a DOWN-priority crouch-bias mode later
// it'd be a 7th value.
static int g_socd_mode_runtime = -1;  // -1 = uninitialized; runtime override of env var.

static int Hook_GetSOCDMode() {
    if (g_socd_mode_runtime < 0) {
        const char* env = std::getenv("FM2K_SOCD_MODE");
        g_socd_mode_runtime = 1;  // tournament default
        if (env && env[0] >= '0' && env[0] <= '5' && env[1] == '\0') {
            g_socd_mode_runtime = env[0] - '0';
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode=%d (FM2K_SOCD_MODE override='%s')",
            g_socd_mode_runtime, env ? env : "<unset>");
    }
    return g_socd_mode_runtime;
}

// Set the SOCD mode at runtime. Used by HOST_CONFIG receiver to adopt
// host's mode, and by future settings UI. Mode must be 0..5; out-of-range
// values are ignored.
extern "C" void Hook_SetSOCDMode(int mode) {
    if (mode < 0 || mode > 5) return;
    if (g_socd_mode_runtime != mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode change %d -> %d", g_socd_mode_runtime, mode);
        g_socd_mode_runtime = mode;
    }
}

// Public accessor for the SOCD mode — needed by netplay.cpp's HOST_CONFIG
// broadcaster which can't reach the static.
extern "C" int Hook_GetSOCDModePublic() {
    return Hook_GetSOCDMode();
}

// Public wrapper around Hook_ApplySOCD (declared in hooks.h). Used by
// netplay.cpp to pre-apply SOCD on the host's confirmed inputs before
// storing them into the spectator stream / .fm2krep file — eliminates
// SOCD-mode-mismatch between host and spec (or original-vs-replay) as
// a source of sim divergence. Defined later in this TU but forward-
// declared here so the trivial wrapper compiles.
static inline uint16_t Hook_ApplySOCD(uint16_t input);
uint16_t Hook_ApplySOCD_Public(uint16_t input) {
    return Hook_ApplySOCD(input);
}

// Public autoplay-input computer. Mirrors the body of the
// FM2K_PARITY_AUTOPLAY_BATTLE block inside Hook_GetPlayerInput so the
// stress-mode `gekko_add_local_input` call site can feed gekko the
// same per-player input the engine's Hook_GetPlayerInput would
// dispatch. Keep these two implementations identical — they are the
// shape of an autoplay determinism contract.
uint16_t Hook_ComputeAutoplayBattleInput(int player_id) {
    static int s_cache = -1;
    if (s_cache < 0) {
        const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
        s_cache = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_cache != 1) return 0;
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode < 3000u || game_mode >= 4000u) return 0;

    uint32_t seed = *(uint32_t*)0x447EE0;
    seed ^= (uint32_t)player_id * 0x9E3779B9u;
    seed = (seed ^ (seed >> 16)) * 0x7feb352du;
    seed = (seed ^ (seed >> 15)) * 0x846ca68bu;
    seed = seed ^ (seed >> 16);
    const uint32_t phase = (seed >> 28) & 0x7u;
    const uint32_t dirbits = (seed >> 8) & 0xFu;
    const uint32_t btnbits = (seed >> 4) & 0xFu;
    uint16_t out = 0;
    switch (phase) {
        case 0:
        case 1:
            out = 0; break;
        case 2:
            out = (uint16_t)(dirbits & 0xFu); break;
        case 3:
            out = (uint16_t)((1u << (4 + (btnbits & 3u)))); break;
        case 4:
            out = (uint16_t)((dirbits & 0xFu) | (1u << (4 + (btnbits & 3u)))); break;
        case 5:
            out = (uint16_t)((1u << (4 + (btnbits & 3u))) |
                             (1u << (4 + ((btnbits >> 2) & 3u))));
            break;
        case 6:
            out = (uint16_t)(1u << (dirbits & 3u)); break;
        case 7:
            out = (uint16_t)(0x4u | (1u << (4 + (btnbits & 3u))));
            break;
    }
    if ((out & 0x3u) == 0x3u) out &= ~0x3u;
    if ((out & 0xCu) == 0xCu) out &= ~0xCu;
    return out;
}

// Dwell anchor state for Hook_ComputeAutoplayCssInput — file scope so
// the netplay CSS-sync path can reset it deterministically (the
// in-function gap heuristic only covers the offline path).
static bool g_css_autoplay_in_css = false;
void Hook_AutoplayCssResetDwell() {
    g_css_autoplay_in_css = false;
}

// CSS counterpart of Hook_ComputeAutoplayBattleInput: the wander/dwell/
// confirm synthetic input for ONE player. Two call sites:
//   - Hook_GetPlayerInput's FM2K_PARITY_AUTOPLAY block (offline / stress:
//     called for both player_ids, drives the engine directly), and
//   - Netplay_ProcessCSS (netplay: called for the LOCAL player only, fed
//     into the CSS GekkoSession so BOTH peers' sims consume the identical
//     lockstep stream).
// The netplay call site is the fix for the 2026-06-11 split-brain: the
// old in-hook block short-circuited the CSS netplay branch and each
// peer's sim ran on locally-hashed inputs for BOTH players. The hashes
// key off local counters (buf_idx, dwell entry tick) which skew under
// packet loss, so the two sims locked different chars at different
// frames (P1 flipped at css_frame=733, P2 at 3796) and every rematch
// downstream failure traced here. All cadence/state below derives from
// g_input_buffer_index (0x447EE0), never call counters, so the value is
// stable within a tick and replay-deterministic.
uint16_t Hook_ComputeAutoplayCssInput(int player_id) {
    static int s_dwell = -1;
    if (s_dwell < 0) {
        const char* v = std::getenv("FM2K_AUTOPLAY_CSS_DWELL");
        s_dwell = v ? (int)(std::atof(v) * 100.0) : 0;
        if (s_dwell < 0) s_dwell = 0;
    }
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode != 2000u) {
        g_css_autoplay_in_css = false;
        return 0;
    }
    // g_input_buffer_index is an index into the 1024-frame input ring --
    // it WRAPS (975 -> 39 observed mid-CSS, 2026-06-11 16:11), so it can
    // never be used as an elapsed-time anchor: the old `buf - anchor`
    // dwell wrapped to ~4e9 at the ring boundary and the function fell
    // into the confirm phase by accident (that wrap, not the dwell, is
    // what had been locking chars all along). Instead keep our own
    // monotonic per-CSS tick counter, incremented whenever the ring
    // index moves.
    const uint32_t buf_idx = *(uint32_t*)0x447EE0;
    static uint32_t s_prev_buf = 0;
    static uint32_t s_ticks = 0;
    static uint32_t s_session_salt = 0;
    if (!g_css_autoplay_in_css) {
        // Fresh CSS phase: reset elapsed time, bump the salt so the
        // browse path and confirm buttons differ between matches.
        g_css_autoplay_in_css = true;
        s_prev_buf = buf_idx;
        s_ticks = 0;
        ++s_session_salt;
    } else if (buf_idx != s_prev_buf) {
        ++s_ticks;
        s_prev_buf = buf_idx;
    }
    const uint32_t in_css = s_ticks;
    const uint32_t salt = s_session_salt * 0x68E31DA4u;
    // 1Hz diag: which phase is the autoplay in, and is the clock sane?
    {
        static uint32_t s_diag_last_ms = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_diag_last_ms >= 1000) {
            s_diag_last_ms = now_ms;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSAP] buf=%u ticks=%u salt=%u dwell=%d phase=%s",
                buf_idx, in_css, s_session_salt, s_dwell,
                (s_dwell > 0 && in_css < (uint32_t)s_dwell) ? "wander"
                                                            : "confirm");
        }
    }
    if (s_dwell > 0 && in_css < (uint32_t)s_dwell) {
        // Wander: direction (or idle) stable for ~10-frame steps (rapid browse
        // -- 2x the old 20-frame cadence) and only idle 1/4 of the time so the
        // cursor keeps moving back-to-back. Per-player offset + per-match salt
        // give independent, varied browsing. This stress-walks the per-move
        // char reload as fast as the engine will load.
        const uint32_t step = in_css / 10u;
        uint32_t h = (step + 1u) * 0x9E3779B9u ^ ((uint32_t)player_id << 16)
                   ^ salt;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        h ^= h >> 15;
        return (h & 3u) ? (uint16_t)(1u << ((h >> 1) & 3u)) : (uint16_t)0u;
    }
    if (s_dwell == 0) {
        return ((in_css % 30u) == 0u) ? (uint16_t)0x010u : (uint16_t)0u;
    }
    // Post-dwell lock-in walk: confirm pulse every 30 ticks; if the
    // cursor parked on an empty grid cell (sel=-1, confirm is a no-op)
    // the offset direction nudge steps it to a neighbor before the next
    // pulse. Once the engine latches the confirm (action state set) it
    // stops reading this player's input, so the nudges are inert after
    // lock-in.
    const uint32_t k = in_css % 30u;
    if (k == 0u) {
        uint32_t h = (in_css / 64u + 1u) * 0x85EBCA6Bu
                   ^ ((uint32_t)player_id << 8) ^ salt;
        h = (h ^ (h >> 13)) * 0xC2B2AE35u;
        return (uint16_t)(1u << (4 + (h & 3u)));
    }
    if (k == 15u) {
        uint32_t h = (in_css / 30u + 1u) * 0x9E3779B9u
                   ^ ((uint32_t)player_id << 16) ^ salt;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        return (uint16_t)(1u << (h & 3u));
    }
    return 0;
}

static inline uint16_t Hook_ApplySOCD(uint16_t input) {
    constexpr uint16_t LEFT  = 0x001;
    constexpr uint16_t RIGHT = 0x002;
    constexpr uint16_t UP    = 0x004;
    constexpr uint16_t DOWN  = 0x008;
    const int mode = Hook_GetSOCDMode();

    if ((input & (LEFT | RIGHT)) == (LEFT | RIGHT)) {
        if (mode & 1) {
            input &= ~(LEFT | RIGHT);  // cancel both
        } else {
            input &= ~LEFT;            // R wins
        }
    }
    if ((input & (UP | DOWN)) == (UP | DOWN)) {
        if (mode & 2) {
            input &= ~(UP | DOWN);     // cancel both
        } else {
            input &= ~DOWN;            // U wins
        }
    }
    return input;
}

// Pending input pair captured by Hook_GetPlayerInput's capture_and_return,
// at file scope so Hook_FlushPendingCapture can drain it from outside the
// hook (specifically at CSS→battle transition, before the battle session
// gates capture_and_return out). Without this drain the trailing CSS frame
// — the one carrying the confirm input that flips game_mode 2000→3000 —
// stays trapped in g_capture_p[] and the spectator never receives it.
static uint16_t g_capture_p[2] = {0, 0};
static uint32_t g_capture_recorded_idx = UINT32_MAX;

// Flush the pending (p1, p2) capture to the spectator stream and clear
// the pending state so the next capture starts from scratch. Idempotent:
// safe to call when nothing is pending (no-op).
extern "C" void Hook_FlushPendingCapture() {
    if (g_capture_recorded_idx == UINT32_MAX) return;
    SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
    g_capture_p[0] = g_capture_p[1] = 0;
    g_capture_recorded_idx = UINT32_MAX;
}

// Battle-transition diagnostic. Activates a frame-by-frame state-dump
// window centered on every game_mode transition so we can compare what
// games like StudioS Fighters / SFZ are doing differently from
// WonderfulWorld at battle entry. The window is a few hundred frames so
// we catch the bail-back-to-CSS pattern (typical: ~140 frames in
// "battle" before reverting).
static int  g_battle_diag_frames_remaining = 0;  // counts down per call
static uint32_t g_battle_diag_frame_idx = 0;     // counter for log lines

extern "C" void Hook_BattleDiag_TickIfActive() {
    if (g_battle_diag_frames_remaining <= 0) return;

    uint32_t mode  = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    uint32_t rng   = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    // g_input_buffer_index @ 0x447EE0
    uint32_t buf_idx = *(uint32_t*)0x447EE0;
    // g_last_frame_time @ 0x447DD4
    uint32_t lft   = *(uint32_t*)0x447DD4;
    // g_frame_time_ms @ 0x41E2F0
    uint32_t fr_ms = *(uint32_t*)0x41E2F0;

    // Char slot active flags (0xDF41 from each slot base) + state flags
    // (0x7CA6) — battle init reads these to decide whether to stay in
    // battle. Slots 0 and 1 are P1/P2.
    constexpr uintptr_t SLOT_BASE = 0x4D1D90;
    constexpr size_t    SLOT_SZ   = 57407;
    uint8_t s0_active = *(uint8_t*)(SLOT_BASE + 0xDF41);
    uint8_t s1_active = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0xDF41);
    uint8_t s0_flags  = *(uint8_t*)(SLOT_BASE + 0x7CA6);
    uint8_t s1_flags  = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0x7CA6);
    // The two main_game_loop prologue writes — slot+0xDF75, slot+0xDF79
    uint32_t s0_dF75 = *(uint32_t*)(SLOT_BASE + 0xDF75);
    uint32_t s0_dF79 = *(uint32_t*)(SLOT_BASE + 0xDF79);

    // Round timer + HPs (real addresses from IDA: g_p1_hp = 0x4DFC85,
    // p2 = +0xDF40 stride. The ones at 0x47010C / 0x47030C are
    // g_demo_mode_player_id, NOT player HP. Wrong addresses earlier
    // showed hp=0/0 always which masked an important signal.)
    uint32_t round_timer = *(uint32_t*)0x470060;
    constexpr uintptr_t HP_BASE = 0x4DFC85;
    constexpr uintptr_t HP_STRIDE = 57407;
    uint32_t p1_hp = *(uint32_t*)(HP_BASE);
    uint32_t p2_hp = *(uint32_t*)(HP_BASE + HP_STRIDE);

    // vs_round_function state machine — these decide whether battle
    // proceeds or bails back to CSS:
    //   g_score_value @ 0x470050 — countdown timer; case 200 decrements
    //     each frame, reaching -1 routes to case 300 (round end → CSS).
    //     If battle init sets this to a small/negative value, battle
    //     immediately bails. Almost certainly StudioS's bug signature.
    //   round_state_field — *(g_object_data_ptr + 338) — the case
    //     selector itself (1 → 100 → 110 → 200 → 300 etc).
    //   g_round_end_flag @ 0x424718 — case 200 also bails to 300 if
    //     this is set.
    //   g_round_state @ 0x47004C — top-level round phase (0/1/2).
    //   g_game_mode_flag @ 0x470058 — VS / Story / Team selector that
    //     changes which init-path runs in case 1.
    //   g_round_limit @ 0x470048 — number of rounds to play.
    //   g_game_timer @ 0x470044 — match clock.
    int32_t score_value     = *(int32_t*)0x470050;
    uint32_t round_end_flag = *(uint32_t*)0x424718;
    uint32_t round_state    = *(uint32_t*)0x47004C;
    uint8_t  game_mode_flag = *(uint8_t*)0x470058;
    uint32_t round_limit    = *(uint32_t*)0x470048;
    uint32_t game_timer     = *(uint32_t*)0x470044;
    // LABEL_201 in vs_round_function: bail to CSS when these hit round_limit
    // in Story/Team mode. Both incremented by case 410/420/430/510/520/530.
    // g_match_phase @ 0x4DFC6D (in P1 char slot), g_round_sub_state @ 0x4EDCAC
    uint32_t match_phase    = *(uint32_t*)0x4DFC6D;
    uint32_t round_sub      = *(uint32_t*)0x4EDCAC;
    // g_round_timer_counter @ 0x424F00 — used by game_state_manager's
    // CSS-confirm timer (counts to 100 then transitions to battle)
    uint32_t round_tick_ctr = *(uint32_t*)0x424F00;
    uint32_t obj0_state338  = 0;
    int32_t  obj0_state342  = 0;  // case-110/112 countdown timer
    void* obj_data_ptr = *(void**)0x4CFA00;
    if (obj_data_ptr) {
        obj0_state338 = *(uint32_t*)((uint8_t*)obj_data_ptr + 338);
        obj0_state342 = *(int32_t*)((uint8_t*)obj_data_ptr + 342);
    }
    // lParam @ 0x430114 — case-100 Story-mode score = 100*lParam - 1
    uint32_t lparam_save = *(uint32_t*)0x430114;
    // Object pool active type-4 fighter count — case-200 Story-mode bails
    // when this < 2. Replicate the EXACT walk from vs_round_function:
    //   if (entry.type == 4 &&
    //       entry[+4] == 0 &&
    //       hp[char_slot[entry[+342]]] != 0) ++count;
    // Pool: 0x4701E0, 1024 entries × 382 bytes. Per-entry +342 holds
    // the char-slot index this object belongs to; HP at HP_BASE +
    // slot_idx * 57407.
    // Re-derived from the actual asm at 0x408E90:
    //   pool base = 0x4701E0 (g_object_pool.payload)
    //   eax starts at base + 0x156, iterates +0x17E (382) per entry.
    //   cmp [eax-0x156], 4   → entry+0   = type     (uint32)
    //   cmp [eax+4],     0   → entry+0x15A (=346) = alive_flag (uint32, 0=alive)
    //   mov ebp, [eax]       → entry+0x156 (=342) = slot_idx (uint32)
    //   * 57407, +g_p1_hp, hp != 0
    int active_type4 = 0;
    int total_type4 = 0;
    {
        const uint8_t* pool = (const uint8_t*)0x4701E0;
        for (int i = 0; i < 1024; i++) {
            const uint8_t* e = pool + i * 382;
            uint32_t type = *(const uint32_t*)(e + 0);
            if (type != 4) continue;
            total_type4++;
            uint32_t alive_flag = *(const uint32_t*)(e + 346);
            if (alive_flag != 0) continue;
            uint32_t slot_idx = *(const uint32_t*)(e + 342);
            if (slot_idx >= 8) continue;
            uint32_t slot_hp = *(const uint32_t*)(HP_BASE + slot_idx * HP_STRIDE);
            if (slot_hp == 0) continue;
            active_type4++;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BD %4u] mode=%u rng=0x%08X "
        "mp=%u rs=%u rtc=%u "         // LABEL_201 trigger: mp/rs hitting rlim
        "rs338=%u score=%d t4=%d/%d "
        "ref=%u gmf=%u rlim=%u gt=%u "
        "hp=%u/%u",
        g_battle_diag_frame_idx, mode, rng,
        match_phase, round_sub, round_tick_ctr,
        obj0_state338, score_value, active_type4, total_type4,
        round_end_flag, game_mode_flag, round_limit, game_timer,
        p1_hp, p2_hp);
    (void)fr_ms; (void)s0_dF75; (void)s0_dF79;
    (void)buf_idx; (void)lft; (void)round_timer;
    (void)obj0_state342; (void)round_state; (void)lparam_save;
    (void)s0_active; (void)s0_flags; (void)s1_active; (void)s1_flags;

    g_battle_diag_frame_idx++;
    g_battle_diag_frames_remaining--;
}

static void CheckGameModeTransition() {
    uint32_t current_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        // Publish session_kind to SharedMem so the launcher can forward
        // it to the hub. Used by the spectator-join /F decision: when
        // someone requests to spectate us, the hub returns our current
        // session_kind in spectate_grant so their launcher knows
        // whether to set FM2K_BOOT_TO_BATTLE=1 (we're in battle) or
        // not (we're in CSS — spec needs natural CSS init for the
        // CSS-state snapshot to apply cleanly at mode==2000).
        uint8_t kind = 0;  // menu / unknown
        if (current_mode == 2000u) kind = 1;            // CSS
        else if (current_mode >= 3000u && current_mode < 4000u)
            kind = 2;                                    // battle
        SharedMem_PublishSessionKind(kind);

        // Auto-capture banner pipeline. Drives one screenshot at each
        // mode-boundary the launcher's capture-runner cares about,
        // then writes a "DONE" sentinel file the launcher polls
        // before terminating the game. No-op when FM2K_AUTO_CAPTURE
        // wasn't set (FM2KCapture::IsActive() short-circuits).
        if (FM2KCapture::IsActive()) {
            static bool s_captured_title = false;
            static bool s_captured_css   = false;
            static bool s_captured_battle = false;
            if (!s_captured_title && current_mode == 1000) {
                FM2KCapture::SaveScreenshot("title.png");
                s_captured_title = true;
            }
            if (!s_captured_css && current_mode == 2000) {
                FM2KCapture::SaveScreenshot("css_initial.png");
                s_captured_css = true;
            }
            if (!s_captured_battle && current_mode >= 3000
                && current_mode < 4000) {
                FM2KCapture::SaveScreenshot("battle.png");
                s_captured_battle = true;
                // All three core captures done — touch a sentinel so
                // the launcher's capture-runner sees "ready to kill".
                // Empty zero-byte file; the launcher polls for its
                // existence on a 250 ms cadence.
                FILE* f = std::fopen(
                    (std::string(std::getenv("FM2K_CAPTURE_DIR") ?
                                 std::getenv("FM2K_CAPTURE_DIR") : ".")
                     + "/.capture_done").c_str(), "wb");
                if (f) std::fclose(f);
            }
        }

        // Whenever the game crosses any CSS↔battle boundary, kick off a
        // 300-frame (3 second @ 100 Hz) state dump so we can diff
        // working games (WonderfulWorld) vs broken ones (SFZ, StudioS
        // Fighters) at battle entry. Reset the per-window counter.
        // BATTLE-DIAG window: gated on FM2K_BATTLE_DIAG=1. Off by default
        // because each open-window dumps 300 frames of [BD ##] state into
        // the log per CSS↔battle transition — useful for diffing broken
        // FM2K variants at battle entry but pure noise during normal play.
        // Cached once at first call.
        static int s_battle_diag_enabled = -1;
        if (s_battle_diag_enabled < 0) {
            const char* v = std::getenv("FM2K_BATTLE_DIAG");
            s_battle_diag_enabled = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }
        bool boundary = s_battle_diag_enabled &&
            ((IsCSSMode(g_last_game_mode)    && IsBattleMode(current_mode)) ||
             (IsBattleMode(g_last_game_mode) && IsCSSMode(current_mode)));
        if (boundary) {
            g_battle_diag_frames_remaining = 300;
            g_battle_diag_frame_idx = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> BATTLE-DIAG window OPEN (300 frames) <<<");
        }

        // Spectator: state init no longer mirrors via local game_mode flips.
        // Host emits PIN_RNG / RESET_INPUT_STATE / SOUND_INIT ops as part of
        // the SessionEvent stream (see Netplay_StartBattle, Netplay_ProcessCSS,
        // CheckFullyConnected); the spectator applies them in
        // SpectatorNode_PopFrameInputs's head-drain at the moment its local
        // sim is about to consume the corresponding INPUT — same logical
        // frame the host's pin happened. Eliminates the off-by-N race
        // between host-side write and spectator-side game_mode flip.
        //
        // Just bail before any host-only player-state-machine work runs,
        // and let the BATTLE-DIAG window + capture pipeline above still
        // observe the boundary for diagnostics.
        if (g_spectator_mode) {
            g_last_game_mode = current_mode;
            return;
        }

        bool was_battle = IsBattleMode(g_last_game_mode);
        bool is_battle = IsBattleMode(current_mode);

        if (!was_battle && is_battle) {
            // ENTERING BATTLE MODE - Signal entry, but DON'T start GekkoNet yet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> ENTERING BATTLE MODE - Signaling remote, waiting for sync");

            // Drain the trailing CSS-frame capture before the battle session
            // gates capture_and_return out. The pair sitting in g_capture_p[]
            // is the LAST CSS frame — the one whose confirm flipped game_mode.
            // Without this flush, spectator never sees that frame, never
            // flips its own game_mode, and desyncs at battle entry.
            extern void Hook_FlushPendingCapture();
            Hook_FlushPendingCapture();

            if (!g_offline_mode && Netplay_IsConnected()) {
                // Boot-to-battle (test/dev) skips the CSS rendezvous that
                // normally arms the battle-entry barrier. Arm it here so the
                // two direct-to-battle peers accept each other's signal
                // instead of deadlocking at "waiting for sync". No-op (and
                // never reached as an arm) on the production CSS path.
                static const bool s_btb = []{
                    const char* e = std::getenv("FM2K_BOOT_TO_BATTLE");
                    return e && e[0] == '1' && e[1] == '\0';
                }();
                if (s_btb) {
                    extern void Netplay_ArmBattleEntryBarrier();
                    Netplay_ArmBattleEntryBarrier();
                }
                Netplay_SignalBattleEntry();
                g_battle_entry_signaled = true;
                // NOTE: GekkoNet will be started in Hook_UpdateGameState
                // after both clients have entered battle mode
            }
        } else if (was_battle && !is_battle) {
            // LEAVING BATTLE MODE - Signal exit; trampoline tears down the
            // GekkoNet session at the agreed swap_frame so both peers
            // (and any spectators) destroy in lockstep. Synchronous
            // teardown here would leave a few frames of mismatched session
            // state on the wire and risk spectator desync at the boundary.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "<<< LEAVING BATTLE MODE - Signaling swap_frame exit");
            if (!g_offline_mode && Netplay_IsActive()) {
                Netplay_SignalBattleEnd();
            } else if (Netplay_IsActive()) {
                // Offline / stress paths still tear down synchronously —
                // there's no remote peer to negotiate with.
                Netplay_EndBattle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session stopped (offline path)");
            }
            g_battle_entry_signaled = false;
        }

        g_last_game_mode = current_mode;
    }
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// ============================================================================
// FM95 INPUT HOOKS
// FM95 splits the input read into two single-arg functions instead of FM2K's
// dispatched (player_id, input_type) pair. Each FM95 hook drives the same
// path the FM2K Hook_GetPlayerInput offline branch does:
//   1. Sample our binder (FM2KInputBinder::Sample_Win32) for the local
//      player's slot. Picks up our remappable keyboard + XInput gamepad
//      bindings — eventually the only input surface the user sees once
//      we hide CPW's titlebar and wrap it in our UI.
//   2. Apply facing flip via g_p_facing_snap[25*player_idx] — the SAME
//      logic CPW's native get_player_input_p1/p2 does, ported here so
//      we can return the engine-relative bits the input ring expects.
//   3. Mask START on CSS (matches FM2K's ~0x400 strip on game_mode 2000).
//   4. Apply SOCD.
// FM95 doesn't have CSS-magic-mode 2000, so the START mask uses the
// engine-aware IsCSSMode (object pool walk) instead.
//
// Netplay rollback path: same as FM2K — the rollback driver overrides
// ProcessGameInputs (single-arg, FM95-compat) to write into the input
// history rings post-poll. Hook_GetPlayerInput_FM95_P*'s job here is the
// LOCAL input read for both offline and the local input queued into
// GekkoNet via AddLocalInput.
// ============================================================================
//
// Note on facing flip: FM95's input ring stores engine-relative bits
// (forward/back), not screen-relative (left/right). The native fn applies
// L↔R swap when facing is flipped. We do the same so our binder output
// matches what CPW's downstream (motion table, character_state_machine)
// expects.

constexpr uintptr_t FM95_FACING_SNAP_BASE = 0x5E98A8;  // g_p_facing_snap

static uint16_t Fm95SampleBinderForPlayer(int binder_slot, int facing_idx) {
    // binder_slot picks which set of bindings to read (0 = P1 bindings, 1 =
    // P2 bindings). Earlier this was hardcoded to 0 for both players, which
    // meant P2 mirrored P1 and any second controller went unused.
    //
    // facing_idx is the host's per-player index (1 or 2 on FM95) used as
    // the multiplier into g_p_facing_snap[25 * idx]. Same fold the native
    // get_player_input_p1/p2 functions perform.
    uint16_t bound = FM2KInputBinder::Sample_Win32(binder_slot);

    const uint8_t facing = *(const uint8_t*)
        (FM95_FACING_SNAP_BASE + (uintptr_t)facing_idx * 25u);
    if (facing) {
        const uint16_t left_bit  = (bound & 0x001);
        const uint16_t right_bit = (bound & 0x002);
        bound = (bound & ~0x003) | (left_bit << 1) | (right_bit >> 1);
    }

    if (IsCSSMode(*(uint32_t*)FM2K::ADDR_GAME_MODE)) {
        bound &= (uint16_t)~0x400u;
    }
    return Hook_ApplySOCD(bound);
}

int __cdecl Hook_GetPlayerInput_FM95_P1(int player_idx) {
    // Slice F: while the chat input box is open the local fighter
    // shouldn't react to typed keys. Suppressed here for offline /
    // single-client testing — full netplay correctness needs the
    // gate at the input-binder Sample call instead so the zero
    // makes it onto the wire (otherwise the peer sees real inputs).
    if (fc_hud::IsChatInputActive() && g_player_index == 0) return 0;
    // First-time binder init mirrors the FM2K path. Done lazily so we don't
    // race with CPW's window/SDL/etc. init. Cheap once warmed up.
    static bool s_warmed = false;
    if (!s_warmed) {
        char buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
            const char* slash = std::strrchr(buf, '\\');
            if (!slash) slash = std::strrchr(buf, '/');
            const char* base = slash ? slash + 1 : buf;
            std::string stem = base;
            auto dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem.resize(dot);
            FM2KInputBinder::SetGameProfile(stem.c_str());
        }
        FM2KInputBinder::Init();
        FM2KInputBinder::Load();
        s_warmed = true;
    }
    return (int)Fm95SampleBinderForPlayer(/*binder_slot=*/0, player_idx);
}

int __cdecl Hook_GetPlayerInput_FM95_P2(int player_idx) {
    if (fc_hud::IsChatInputActive() && g_player_index == 1) return 0;
    // Reads the P2 binder slot (slot 1) so a second device — or fallback
    // to keyboard P2 bindings — drives the second player. For netplay,
    // ProcessGameInputs overwrites this post-poll with GekkoNet's synced
    // remote input, so this only matters on offline / dual-client / stress
    // tests where both players are local.
    return (int)Fm95SampleBinderForPlayer(/*binder_slot=*/1, player_idx);
}

// Hook: GetPlayerInput
// CSS: return synced input from control channel
// Battle: return synchronized input from GekkoNet with facing adjustment
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // Slice F: chat-mode input gate. Same caveat as the FM95 hooks
    // above — works for offline single-client testing; netplay
    // correctness wants the gate at the input-binder Sample call.
    if (fc_hud::IsChatInputActive() && player_id == g_player_index) return 0;
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // (Removed FM2K_INPUT_DUMP block — calling original_get_player_input
    // for diagnostic purposes had a SIDE EFFECT: it ran FM2K's keyboard-
    // poll → .ini-binding pipeline, which leaked .ini-bound key presses
    // into FM2K's internal edge-detection state EVEN WHEN our binder was
    // supposed to be the sole input source. Symptom was that custom
    // binder binds AND game.ini binds both fired simultaneously. Bit
    // mappings are confirmed (A=0x010 .. F=0x200, START=0x400 by
    // induction); diagnostic served its purpose, now it's gone.)

    // Single capture-and-return funnel. EVERY return path goes through
    // this lambda so we record the (p1, p2) input pair into the host's
    // session_history every time the input-buffer-index ticks (= one
    // full FM2K frame). Recording starts at FM2K boot — captures title-
    // screen no-ops, auto-mash, pre-rendezvous CSS, post-rendezvous
    // GekkoNet-merged inputs, battle frames — one canonical log spanning
    // the entire connection. Late-joining spectators get this whole log
    // via SendSessionBackfillTo and replay deterministically from frame 0.
    //
    // Spectators (g_spectator_mode) DO NOT record — they consume from
    // pb_queue, not produce. Stress / offline DO record but the log is
    // never sent (no subscribers).
    // capture_and_return: every returned input from this hook on the HOST
    // side is the source of truth for the spectator stream. We pair the
    // current frame's (p1, p2) returns and emit them via
    // SpectatorNode_OnFrameConfirmed at the moment the frame boundary
    // ticks (g_input_buffer_index advances). The spectator drives its
    // local FM2K from that exact same input pair, popped one per sim
    // tick. Because every change to FM2K's state is input-driven from a
    // canonical default state at boot, replaying the input log in order
    // produces a 1:1 sim — title-screen auto-mash, CSS cursor moves,
    // battle commands all included.
    //
    // The pending pair lives at file scope (g_capture_*) instead of
    // lambda statics so Hook_FlushPendingCapture() can drain the trailing
    // CSS frame at CSS→battle transition — without that flush, the LAST
    // CSS frame's pair (the one whose confirm input flips game_mode) sits
    // in g_capture_p[] forever because the next frame's capture is gated
    // out by Netplay_IsActive once the battle session starts. Spectator
    // never sees that frame, never flips game_mode, desync.
    //
    // SKIP CONDITIONS:
    //   * g_spectator_mode: spectator only consumes, never produces.
    //   * Netplay_IsActive() (battle): GekkoNet runahead+rollback fires
    //       this hook ~5x per real frame. Battle confirmed-frame capture
    //       is gated in netplay.cpp's AdvanceEvent handler instead.
    auto capture_and_return = [player_id](int result) -> int {
        if (g_spectator_mode) return result;
        if (Netplay_IsActive()) return result;
        const uint32_t cur_idx = *(uint32_t*)0x447EE0;
        if (cur_idx != g_capture_recorded_idx) {
            if (g_capture_recorded_idx != UINT32_MAX) {
                SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
            }
            g_capture_recorded_idx = cur_idx;
        }
        g_capture_p[player_id & 1] = (uint16_t)result;
        return result;
    };

    // Title-screen menu-cursor write. Must fire on BOTH host AND spectator —
    // it's a state side-effect of the auto-title-skip protocol, not an
    // input. session_history only records returned input values, so a
    // spectator replaying host's recorded auto-mash button-A pulses would
    // navigate from g_menu_selection=0 (default = "VS CPU"/first option)
    // and end up in the wrong scene tree. We force g_menu_selection=1
    // ("VS Player") on every node so the same recorded input pattern
    // resolves to the same menu transitions everywhere.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_cursor_set_global = false;
        if (auto_skip && !s_cursor_set_global && game_mode == 1000) {
            *(uint32_t*)0x424780 = 1;  // g_menu_selection = VS Player
            s_cursor_set_global = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "TitleMenuCursor: pre-set g_menu_selection=1 (host or spectator)");
        }
    }

    // Spectator process — SINGLE source of truth: the popped input pair
    // (host's recorded p1/p2 for the current sim frame). No keyboard read,
    // no auto-mash, no fall-through to anything else. The hook is only
    // ever called from inside RunSpectatorTick → original_process_game_inputs,
    // which we only invoke after popping a frame from the queue.
    //
    // Battle-mode facing fix mirrors host's branch — same 11-bit input,
    // same left/right swap when char_active && !state_flag_8.
    if (g_spectator_mode) {
        uint16_t input = (player_id == 0)
            ? SpectatorNode_GetCurrentP1Input()
            : SpectatorNode_GetCurrentP2Input();

        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;
        bool facing_reversed = true;
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) facing_reversed = false;
            }
        }
        if (!facing_reversed) {
            uint16_t left_bit  = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }
        input = Hook_ApplySOCD(input);
        return (int)input;  // spectator path doesn't record
    }

    // Auto-mash through title screen → menu → CSS. Default ON unless
    // FM2K_AUTO_TITLE_SKIP=0. With the boot-to-title patch (push 0x0C)
    // the game starts in title_screen_manager (g_game_mode=1000). We
    // pre-set g_menu_selection=1 (VS Player is always index 1 in
    // g_titleMenu_modeList[]) and pulse button A (bit 4 = 0x010) on
    // alternate frames until g_game_mode flips to 2000 (CSS reached).
    //
    // Critical: alternate per-FRAME, not per-CALL. get_player_input
    // is called twice per frame (once for each player) — if we
    // increment a counter on every call and use parity, P1 and P2
    // get opposite values and neither sees a rising edge after frame
    // 1. Use g_input_buffer_index @ 0x447EE0 instead — it ticks once
    // per frame from the game's own input pipeline, so both players'
    // calls in the same frame return the same value.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_done = false;
        static bool s_cursor_set = false;
        static uint32_t s_started_frame = 0;
        if (auto_skip && !s_done) {
            if (game_mode >= 2000) {
                s_done = true;
                uint32_t now = *(uint32_t*)0x447EE0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "AutoTitleSkip: reached g_game_mode=%u, handing input "
                    "back to user (took %u input-buffer ticks)",
                    game_mode, now - s_started_frame);
            } else if (game_mode == 1000) {
                if (!s_cursor_set) {
                    // Menu cursor itself is now written in the hoisted
                    // block at the top of this function (runs on host AND
                    // spectator). Here we just record the start-frame for
                    // the auto-mash duration log and flip s_cursor_set.
                    s_cursor_set = true;
                    s_started_frame = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "AutoTitleSkip: starting button-A mash from input_buf=%u",
                        s_started_frame);
                }
                // 4-tick pattern (0x010, 0x010, 0, 0) ensures a rising
                // edge every 4 frames: prev=0 → cur=0x010 fires the
                // edge detector. Holding the button for 2 frames lets
                // the title menu's "any-button" check (& 0x3F0) sample
                // a stable value before the release. ~25 Hz of
                // confirms — fast enough to march title → menu → CSS
                // in ~16 frames, slow enough not to skip past states
                // the menu hasn't latched yet.
                uint32_t buf_idx = *(uint32_t*)0x447EE0;
                return capture_and_return(((buf_idx >> 1) & 1) ? 0 : 0x010);
            }
        }
    }


    // FM2K_PARITY_AUTOPLAY: drive title→CSS→battle via a deterministic
    // input sequence. Short-circuits the netplay/CSS/spectator branches
    // — autoplay owns input completely. Phase-aware (uses live
    // game_mode to pick inputs per CSS section). The game's CSS state
    // machine (game_state_manager @ 0x406FC0) reads:
    //   g_processed_input[i] for direction (bits 0..3 = L/R/U/D)
    //   g_input_changes[i] & 0x3F0 for attack-button rising-edge (confirm)
    // The original get_player_input returns RAW input; the game's
    // process_game_inputs converts raw → processed_input and computes
    // changes from prev frame. So returning a clean rising-edge of
    // bit 0x10 (button A) on a CSS frame triggers the confirm path.
    {
        static const char* env_autoplay = std::getenv("FM2K_PARITY_AUTOPLAY");
        if (env_autoplay && std::strcmp(env_autoplay, "1") == 0) {
            static uint32_t s_call_count = 0;
            const uint32_t call = s_call_count++;
            const uint32_t frame = call / 2u;
            const uint16_t Z = 0x010u;
            uint16_t out = 0u;

            // WW game_mode states (per vs_round_function @ 0x4086A0 +
            // game_state_manager @ 0x406FC0):
            //   0     = boot/title intro
            //   2000  = CSS active
            //   3000  = battle active
            // No 4000+ values; battle stays at 3000 throughout match.
            // For clean idle-parity capture, send Z every 30 frames
            // until we reach battle (advances title prompts + confirms
            // both CSS slots) and then idle (out=0) so the captured
            // frames show pure-idle physics with no synthetic attack
            // inputs polluting the comparison against kgt's idle run.
            if (game_mode < 3000u) {
                // FM2K_AUTOPLAY_CSS_DWELL=<seconds>: browse the CSS like a
                // human before locking in -- wander the cursor (d-pad
                // only, no confirm bits) for the dwell window, then
                // confirm with a deterministic per-player button so color
                // variety still gets exercised. Default 0 = legacy
                // instant-confirm. Real players move around at CSS for
                // 5-30s; the instant mash never exercised the spectator
                // seam hold (or the host's own CSS phase) at realistic
                // durations. Body lives in Hook_ComputeAutoplayCssInput so
                // the netplay path (Netplay_ProcessCSS) can feed the SAME
                // values into the CSS GekkoSession for the local player.
                if (game_mode == 2000u) {
                    out = Hook_ComputeAutoplayCssInput(player_id);
                } else if ((frame % 30u) == 0u) {
                    out = Z;
                }
            }
            // mode >= 3000: idle (out stays 0)
            //
            // FM2K_PARITY_AUTOPLAY_BATTLE=1 keeps injecting inputs during
            // battle. Pattern: every 12 frames pulse button A + a random-
            // looking direction so character_state_machine fires attack
            // edges and projectile spawns. Used by autonomous Phase F
            // stress runs to exercise RNG-consuming actions that idle
            // stress doesn't cover (~hours of idle stress passes clean,
            // but user-reported desyncs cluster on active gameplay).
            else if (game_mode >= 3000u && game_mode < 4000u) {
                static int s_battle_play_cached = -1;
                if (s_battle_play_cached < 0) {
                    const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
                    s_battle_play_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
                }
                // Stress + autoplay coexistence: when a battle netplay
                // session is active, SKIP this autoplay path entirely and
                // let the netplay branch downstream pick up g_p?_input
                // (which netplay.cpp populated with the same autoplay
                // values via gekko). Without this skip, the engine and the
                // .fm2krep see different input streams: engine sims with
                // autoplay-computed values, .fm2krep records the
                // gekko-delivered values. The replay re-runs with the
                // .fm2krep values and produces different state.
                if (s_battle_play_cached == 1 && Netplay_IsActive()) {
                    // out stays 0 here; the netplay-active gate at the
                    // bottom of this block will skip the autoplay return
                    // so the netplay branch can supply g_p?_input.
                } else if (s_battle_play_cached == 1) {
                    // Pseudo-random battle inputs DETERMINISTIC under
                    // rollback. Derived from g_input_buffer_index (the
                    // engine's authoritative per-tick counter at
                    // 0x447EE0, part of saved InputTracking region) +
                    // player_id, hashed through a splitmix32-like
                    // function. Same frame → same input, every time,
                    // forward and replay both. Different per-frame
                    // entropy than a fixed pattern, so the test
                    // exercises hit-stop, super-cancel, projectile
                    // spawn, throw whiff, etc. — the RNG-consuming
                    // code paths a fixed cycle wouldn't reach.
                    //
                    // ABSOLUTELY do not pull from a static counter
                    // (s_call_count etc.) — that counter advances
                    // EVERY Hook_GetPlayerInput call including replay
                    // re-invocations, so forward sim_N and replay
                    // sim_N would see different "random" outputs and
                    // the test would diverge from itself. Last
                    // iteration of this code did exactly that and
                    // produced a frame-4 false-positive desync.
                    uint32_t seed = *(uint32_t*)0x447EE0;
                    seed ^= (uint32_t)player_id * 0x9E3779B9u;
                    // splitmix32
                    seed = (seed ^ (seed >> 16)) * 0x7feb352du;
                    seed = (seed ^ (seed >> 15)) * 0x846ca68bu;
                    seed = seed ^ (seed >> 16);
                    // Holdable buttons: A (0x10), B (0x20), C (0x40),
                    // D (0x80). Hold for ~10-15 frames so commands
                    // register, then release. Directions: L (0x01),
                    // R (0x02), U (0x04), D (0x08).
                    //
                    // Phase split into 3 bits: which input "mode" the
                    // PRNG picks this frame.
                    const uint32_t phase = (seed >> 28) & 0x7u;
                    const uint32_t dirbits = (seed >> 8) & 0xFu;
                    const uint32_t btnbits = (seed >> 4) & 0xFu;
                    switch (phase) {
                        case 0:  // idle (give engine time to settle)
                        case 1:
                            out = 0; break;
                        case 2:  // pure directional (movement)
                            out = (uint16_t)(dirbits & 0xFu); break;
                        case 3:  // single button tap
                            out = (uint16_t)((1u << (4 + (btnbits & 3u)))); break;
                        case 4:  // direction + button (special move setup)
                            out = (uint16_t)((dirbits & 0xFu) | (1u << (4 + (btnbits & 3u)))); break;
                        case 5:  // multi-button (super / parry attempt)
                            out = (uint16_t)((1u << (4 + (btnbits & 3u))) | (1u << (4 + ((btnbits >> 2) & 3u))));
                            break;
                        case 6:  // long-hold of one direction (walk)
                            out = (uint16_t)(1u << (dirbits & 3u)); break;
                        case 7:  // jump-cancel-style (UP + button)
                            out = (uint16_t)(0x4u | (1u << (4 + (btnbits & 3u))));
                            break;
                    }
                    // SOCD-cleaner expects no L+R or U+D simultaneously;
                    // strip the conflicts at the source so the engine
                    // doesn't have to reject them (same effect as
                    // Hook_ApplySOCD mode 1, applied earlier).
                    if ((out & 0x3u) == 0x3u) out &= ~0x3u;
                    if ((out & 0xCu) == 0xCu) out &= ~0xCu;
                }
            }

            /* Diagnostic: log on first hit + every game_mode change AND
             * every 120 frames once we hit battle (game_mode >= 3000) so
             * we can see HP/active-flag populating after CSS exit.
             * HP source corrected: g_p1_hp=0x4DFC85, g_p2_hp=0x4EDCC4
             * (verified via IDA xref of vs_round_function @ 0x4086A0). */
            static uint32_t s_last_logged_mode = 0xFFFFFFFFu;
            static uint32_t s_last_periodic = 0u;
            const bool mode_changed = (game_mode != s_last_logged_mode);
            const bool periodic = (game_mode >= 3000u) &&
                                  ((frame - s_last_periodic) >= 120u);
            if (mode_changed || periodic) {
                const uint32_t p1_action = *(uint32_t*)0x47019Cu;
                const uint32_t p2_action = *(uint32_t*)0x4701A0u;
                // Selected character indexes — IDA-renamed to
                // g_p1_selected_char_idx / g_p2_selected_char_idx
                // (was misleadingly g_player_stage_positions[]).
                const int32_t  p1_char   = *(int32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
                const int32_t  p2_char   = *(int32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
                const uint32_t timer     = *(uint32_t*)0x424F00u;
                const uint8_t  char0_act = *(uint8_t*) 0x4DFCD1u;
                const uint8_t  char1_act = *(uint8_t*)(0x4DFCD1u + 57407u);
                const uint32_t p1_hp     = *(uint32_t*)0x4DFC85u;
                const uint32_t p1_max_hp = *(uint32_t*)0x4DFC91u;
                const uint32_t p2_hp     = *(uint32_t*)0x4EDCC4u;
                const uint32_t p2_max_hp = *(uint32_t*)0x4EDCD0u;
                const int32_t  cam_x     = *(int32_t*) 0x447F2Cu;
                const int32_t  cam_y     = *(int32_t*) 0x447F30u;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[AUTOPLAY] frame=%u mode=%u p1act=%u p2act=%u "
                    "p1char=%d p2char=%d ctimer=%u "
                    "char0_act=%u char1_act=%u "
                    "p1_hp=%u/%u p2_hp=%u/%u cam=(%d,%d) out=0x%03X",
                    frame, game_mode, p1_action, p2_action,
                    p1_char, p2_char, timer,
                    char0_act, char1_act,
                    p1_hp, p1_max_hp, p2_hp, p2_max_hp,
                    cam_x, cam_y, out);
                s_last_logged_mode = game_mode;
                if (periodic) s_last_periodic = frame;
            }
            // Stress + autoplay + active battle netplay: don't short-circuit
            // here. Fall through to the netplay branch below so the engine
            // consumes g_p?_input (the gekko-delivered value, which is the
            // SAME autoplay value via the netplay.cpp gekko_add_local_input
            // path). End result: engine input == spec-stream input == .fm2krep
            // input — replay reproduces record deterministically.
            //
            // Same rule for CSS netplay (2026-06-11 split-brain fix): the
            // engine must consume Netplay_GetCSSInput (the lockstep-
            // delivered pair) — Netplay_ProcessCSS feeds our local
            // autoplay value into the session via
            // Hook_ComputeAutoplayCssInput. Short-circuiting here ran each
            // peer's sim on locally-hashed inputs for BOTH players; local
            // counter skew under packet loss made P1's sim lock chars at
            // css_frame=733 while P2's sim browsed to 3796 — different
            // chars, different colors, doomed rematch.
            if (game_mode >= 3000u && game_mode < 4000u && Netplay_IsActive()) {
                // Skip return; let the netplay branch handle it.
            } else if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
                // Skip return; let the CSS netplay branch handle it.
            } else {
                return capture_and_return((int)out);
            }
        }
    }

    // (Spectator branch lifted to the top of the function — runs before
    // auto-mash so spectator's local FM2K replays host's recorded inputs
    // instead of generating its own auto-mash sequence.)

    // Battle mode with GekkoNet active - return synced input with facing fix
    if (Netplay_IsActive()) {
        // Swap-window input guard (CSS-2 confirm leak, 2026-06-11 15:50):
        // the battle session outlives the local game's exit from battle
        // mode by a few hundred ms (battle-end barrier + EndBattle drain).
        // Hook calls in that window served the LAST gekko-delivered battle
        // inputs to the freshly-initialized rematch CSS -- held attack
        // bits read as confirm edges, asymmetrically per peer (each froze
        // on a different final input pair), locking ghost chars before
        // lockstep even started (P1 opened CSS-2 at act=1/1 timer=53, P2
        // at act=0/1: P1 auto-flipped to battle at css_frame=50, P2
        // waited forever). game_state_manager's CSS init already
        // canonicalizes act/timer/sel, so neutral here keeps the new CSS
        // pristine; battle re-sims read game_mode 3000 from restored
        // state and are unaffected.
        if (!IsBattleMode(game_mode) && Netplay_IsConnected()) {
            return capture_and_return(0);
        }
        uint16_t input = Netplay_GetInput(player_id);

        // Apply facing direction swap (same logic as original get_player_input).
        // During battle (3000-3999), if character is active and not in special
        // state, left/right are swapped based on facing direction.
        //
        // CRITICAL: these are OFFSETS inside the character slot, NOT absolute
        // addresses. Hard-coding absolute addresses broke when we corrected
        // CHAR_SLOT_BASE from 0x4D1D80 to 0x4D1D90 — the hook was reading
        // from 16 bytes into the wrong memory, decisions were garbage, and
        // the two peers could pick different facing-swap values from
        // non-deterministic residue. This is almost certainly the "HP
        // differs by 2 after a hit" signature we've been chasing.
        //
        // Offsets are relative to the CORRECTED base CHAR_SLOT_BASE=0x4D1D90.
        // First attempt computed these against the old 0x4D1D80 base, which was
        // 16 bytes too low for the new base — that made facing-swap read the
        // wrong bytes and the symptom was "left/right flip when you switch
        // sides". Absolute addresses of the fields are unchanged:
        //   0x4DFCD1 - 0x4D1D90 = 0xDF41   (char_active)
        //   0x4D9A36 - 0x4D1D90 = 0x7CA6   (char_state_flags)
        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;

        bool facing_reversed = true;  // Default: no swap (normal directions)
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) {
                    facing_reversed = false;  // Character active, facing applies
                }
            }
        }

        if (!facing_reversed) {
            // Swap left (bit 0 = 0x001) and right (bit 1 = 0x002)
            uint16_t left_bit = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }
        input = Hook_ApplySOCD(input);

        // Log only the first 4 calls (initial handshake verification). After
        // that stay silent — Hook_GetPlayerInput fires 2x per sim tick, and
        // during stress-mode rollback replay that's thousands of calls per
        // second. Per-100 throttling was still showing up on screen.
        static uint32_t battle_log_count = 0;
        if (battle_log_count < 4) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[BATTLE INPUT #%u] player=%d type=%d -> 0x%03X (facing=%s)",
                battle_log_count, player_id, input_type, input,
                facing_reversed ? "normal" : "swapped");
        }
        battle_log_count++;

        return capture_and_return((int)input);
    }

    // CSS mode with connection - return CSS input from control channel
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        uint16_t input = Netplay_GetCSSInput(player_id);
        input = Hook_ApplySOCD(input);
        return capture_and_return((int)input);
    }

    // Offline or menu: use the binder if active, else fall through to FM2K's
    // own get_player_input. Same gating as Input_CaptureLocal — Init() is
    // idempotent and resolves to %APPDATA%\FM2K_Rollback\fm2k_inputs.ini
    // (matching the launcher's save path) so launcher-bound keys / pads
    // drive offline play here, GekkoNet-online play through Input_CaptureLocal.
    {
        static int  s_last_check_tick = 0;
        static bool s_binder_active   = false;
        static bool s_profile_routed  = false;
        const int now_tick = (int)GetTickCount();
        if ((now_tick - s_last_check_tick) > 1000 || s_last_check_tick == 0) {
            s_last_check_tick = now_tick;
            // Per-game profile routing — v0.2.43 fix (Sheriel's bug
            // report) restored here after an intervening edit removed
            // it. Without this, Hook_GetPlayerInput's binder Init/Load
            // resolves to the DEFAULT fm2k_inputs.ini and the launcher's
            // "Use override for X" per-game profile is silently
            // ignored offline. Mirrors input.cpp's Input_CaptureLocal.
            if (!s_profile_routed) {
                s_profile_routed = true;
                char buf[MAX_PATH] = {};
                if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
                    const char* slash = std::strrchr(buf, '\\');
                    if (!slash) slash = std::strrchr(buf, '/');
                    const char* base = slash ? slash + 1 : buf;
                    std::string stem = base;
                    auto dot = stem.find_last_of('.');
                    if (dot != std::string::npos) stem.resize(dot);
                    FM2KInputBinder::SetGameProfile(stem.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hook_GetPlayerInput: routed binder to per-game profile '%s'",
                        stem.c_str());
                }
            }
            FM2KInputBinder::Init();
            // Re-Load every tick so launcher-side Save() reaches the
            // running game without restart. Init() only Load()s once
            // (gated by g_initialized); we need the periodic re-read.
            FM2KInputBinder::Load();
            // Hot-plug refresh (#25, Suicidal Muffin's bug): pick up
            // freshly-attached pads + drop removed handles on the same
            // 1 s cadence as the binder reload, so users don't have to
            // restart the session after plugging in a controller.
            FM2KInputBinder::RefreshGamepads();
            const auto& pb = FM2KInputBinder::Bindings(0);
            s_binder_active = false;
            for (const auto& b : pb.bits) {
                if (b.source != FM2KInputBinder::Binding::Source::NONE) {
                    s_binder_active = true;
                    break;
                }
            }
        }
        if (s_binder_active) {
            // input_type is the character slot (same convention the battle /
            // spectator branches above use to compute slot_base for facing-fix).
            // 0 = P1 character → P1 bindings, 1 = P2 character → P2 bindings.
            // Without this distinction both players get the SAME input from
            // P1's bindings — the bug we just fixed.
            int slot = (input_type & 1);
            uint16_t bound = FM2KInputBinder::Sample_Win32(slot);
            // OPTION title-screen submode cycle — fires on the binder path
            // too, before masking off meta-bits so PerGamePatches sees the
            // full 14-bit value (OPTION = 0x800).
            if (player_id == 0) {
                PerGamePatches_OnTitleInputTick(bound, game_mode);
            }
            // Solo-driver CSS takeover (vs_cpu / cpu_vs_cpu / training):
            // when CSS is open AND we're being asked for P2's input AND
            // any solo-driver mode is engaged, the pipe is GATED via
            // PerGamePatches_GatedP2CssInput:
            //   - P1 not confirmed → P2 = 0
            //   - P1 confirmed but attack still held → P2 = 0
            //   - P1 confirmed AND released attack → P2 = P1's input
            // The override has to live INSIDE the binder branch because
            // the binder path returns before PerGamePatches_TryOverrideInput
            // is reached.
            if (game_mode == 2000u && player_id == 1) {
                const bool any_solo_driver =
                    PerGamePatches_IsVsCpuModeActive() ||
                    PerGamePatches_IsCpuVsCpuModeActive() ||
                    PerGamePatches_IsTrainingModeActive();
                if (any_solo_driver) {
                    const uint16_t p1_bound =
                        FM2KInputBinder::Sample_Win32(0)
                            & FM2KInputBinder::kEngineInputMask;
                    bound = PerGamePatches_GatedP2CssInput(p1_bound);
                    slot = 0;
                }
            }

            // Solo-driver BATTLE override. Same reason — binder path
            // shortcircuits PerGamePatches_TryOverrideInput, so we have
            // to invoke the battle helper here too. Zeros P2 for VS CPU /
            // CPU vs CPU so the engine's script-driven AI takes over;
            // applies training-mode P2 behavior for training. P1 is
            // overridden too in CPU vs CPU.
            if (game_mode >= 3000u && game_mode < 4000u &&
                (PerGamePatches_IsVsCpuModeActive() ||
                 PerGamePatches_IsCpuVsCpuModeActive() ||
                 PerGamePatches_IsTrainingModeActive())) {
                // Gated on an active solo-driver mode so normal binder users
                // don't pay the extra Sample_Win32(0) every battle frame (#63
                // sibling to the no-binder DirectInput fix).
                const uint16_t p1_bound_battle =
                    FM2KInputBinder::Sample_Win32(0)
                        & FM2KInputBinder::kEngineInputMask;
                const int over = PerGamePatches_BattleInputOverride(
                    player_id, p1_bound_battle);
                if (over >= 0) {
                    bound = (uint16_t)over;
                }
            }
            // Apply the same battle facing-fix the original_get_player_input
            // path applies (so offline matches behave the same way as the
            // game's own input flow).
            constexpr size_t CHAR_ACTIVE_FLAG_OFFSET_OFL = 0xDF41;
            constexpr size_t CHAR_STATE_FLAGS_OFFSET_OFL = 0x7CA6;
            bool facing_reversed = true;
            if (game_mode >= 3000 && game_mode < 4000) {
                uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
                uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET_OFL);
                if (char_active != 0) {
                    uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET_OFL);
                    if ((char_flags & 8) == 0) facing_reversed = false;
                }
            }
            if (!facing_reversed) {
                uint16_t left_bit  = (bound & 0x001);
                uint16_t right_bit = (bound & 0x002);
                bound = (bound & ~0x003) | (left_bit << 1) | (right_bit >> 1);
            }
            bound = Hook_ApplySOCD(bound);
            // Strip meta-bits (OPTION/FN1/FN2) before passing to engine so
            // they don't leak into game state. Hook-side features that
            // consume those bits did so above (or read via Sample directly).
            bound &= FM2KInputBinder::kEngineInputMask;
            return capture_and_return((int)bound);
        }
    }

    // OPTION-button title-screen submode cycle (no-binder fallback path).
    // Gated on the OPTION-mode selector actually being active. When it's
    // off (the default) OnTitleInputTick is a no-op, but the
    // original_get_player_input(0,0) read below still cost a redundant
    // DirectInput poll EVERY frame for player 0 -- a chunk of the 0.2.46
    // fps regression (#63), since that poll's cost scales with the user's
    // HID/driver stack (machine-specific 95fps). Skip the read entirely
    // unless the selector is engaged.
    if (player_id == 0 && original_get_player_input &&
        PerGamePatches_IsOptionModeSelectorActive()) {
        const uint16_t raw = (uint16_t)(original_get_player_input(0, 0) & 0x7FF);
        PerGamePatches_OnTitleInputTick(raw, game_mode);
    }

    // Per-game mode overrides — VS CPU, CPU vs CPU, training. Returns -1
    // when no toggle applies. Only fires on the offline path (netplay /
    // spectator branches return earlier above), so we don't accidentally
    // override authoritative input streams during a hub match. SOCD is
    // applied uniformly on the override path too.
    {
        int o = PerGamePatches_TryOverrideInput(player_id, game_mode);
        if (o >= 0) {
            o = (int)Hook_ApplySOCD((uint16_t)o);
            return capture_and_return(o);
        }
    }

    // No binder config — vanilla FM2K input path.
    int orig = original_get_player_input
        ? original_get_player_input(player_id, input_type)
        : 0;

    orig = (int)Hook_ApplySOCD((uint16_t)orig);
    return capture_and_return(orig);
}

// Hook: UpdateGameState
// Main control point - check transitions, process netplay
int __cdecl Hook_UpdateGameState() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // FM95 host-driven trampoline activation (opt-in via FM95_TRAMPOLINE=1).
    // FM2K's main_game_loop is replaced wholesale by TrampolineMainLoop,
    // but FM95 keeps its natural WinMain pump. Drive the trampoline tick
    // from inside Hook_UpdateGameState instead. For non-NATIVE phases the
    // tick handles update + render itself (via AdvanceEvent +
    // RenderFrameWithSnapshot); set g_fm95_skip_next_render so
    // Hook_RenderGame suppresses the host's natural render call right
    // after we return. NATIVE phase falls through to the existing logic.
    if constexpr (FM2K::kIsFM95) {
        static const bool s_use_trampoline = []() {
            const char* e = std::getenv("FM95_TRAMPOLINE");
            const bool on = (e && std::strcmp(e, "1") == 0);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "FM95 trampoline gate: env=\"%s\" -> %s",
                        e ? e : "(unset)", on ? "ACTIVE" : "off (host-driven)");
            return on;
        }();
        if (s_use_trampoline) {
            LoopPhase phase = TrampolineFrameTick();
            // Log first-seen-per-phase so the log shows the engine-aware
            // classifier picking up FM95 phase transitions in real time.
            static LoopPhase s_last_logged_phase = LoopPhase::NATIVE;
            static bool      s_phase_logged_once = false;
            if (!s_phase_logged_once || phase != s_last_logged_phase) {
                const char* name =
                    phase == LoopPhase::TRAMPOLINE_BATTLE ? "BATTLE" :
                    phase == LoopPhase::CSS               ? "CSS" :
                    phase == LoopPhase::SPECTATOR_PLAYBACK? "SPECTATOR" :
                                                            "NATIVE";
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "FM95 trampoline tick phase = %s", name);
                s_last_logged_phase = phase;
                s_phase_logged_once = true;
            }
            if (phase != LoopPhase::NATIVE) {
                g_fm95_skip_next_render = true;
                return 0;
            }
            // NATIVE: fall through to existing logic so original_update_game
            // gets called via the normal offline / netplay path below.
        }
    }

    // Offline mode - just pass through
    if (g_offline_mode) {
        // T4 probe: when FM2K_T4_PROBE=1, walk the fighter object pool
        // before each update_game tick using the EXACT same logic as
        // vs_round_function case-200 (type==4, flag@+346==0, HP@slot>0).
        // Log when count<2 with details on which entry failed which
        // condition. This captures what case-200 will see when it runs
        // inside this update_game call, so we can pinpoint why the t4
        // walk false-trips on StudioS games (whereas WW always shows 2).
        static const char* env_t4probe = std::getenv("FM2K_T4_PROBE");
        if (game_mode >= 3000 && game_mode < 4000
            && env_t4probe && std::strcmp(env_t4probe, "1") == 0)
        {
            const uint8_t* pool = (const uint8_t*)0x4701E0;
            constexpr uintptr_t HP_BASE   = 0x4DFC85;
            constexpr uintptr_t HP_STRIDE = 57407;
            int count = 0;
            int t4_seen = 0;
            int fail_flag = 0, fail_hp = 0, fail_slot = 0;
            uint32_t fail_e[4] = {0,0,0,0};
            uint32_t fail_why[4] = {0,0,0,0};  // 1=flag, 2=hp, 3=slot
            int fail_n = 0;
            for (int i = 0; i < 1024; i++) {
                const uint8_t* e = pool + i * 382;
                uint32_t type = *(const uint32_t*)(e + 0);
                if (type != 4) continue;
                t4_seen++;
                uint32_t flag346 = *(const uint32_t*)(e + 346);
                uint32_t slot    = *(const uint32_t*)(e + 342);
                if (flag346 != 0) {
                    fail_flag++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=1; fail_n++; }
                    continue;
                }
                if (slot >= 8) {
                    fail_slot++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=3; fail_n++; }
                    continue;
                }
                uint32_t hp = *(const uint32_t*)(HP_BASE + slot * HP_STRIDE);
                if (hp == 0) {
                    fail_hp++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=2; fail_n++; }
                    continue;
                }
                count++;
            }
            static int s_last_count = -1;
            if (count < 2 && (count != s_last_count || (g_frame_counter % 60) == 0)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[T4-PROBE f=%u] count=%d t4_seen=%d "
                    "fails: flag@346=%d hp=%d slot=%d "
                    "first4=[e=%u why=%u, e=%u why=%u, e=%u why=%u, e=%u why=%u] "
                    "(why: 1=flag@+346!=0, 2=HP[slot]==0, 3=slot>=8)",
                    g_frame_counter, count, t4_seen,
                    fail_flag, fail_hp, fail_slot,
                    fail_e[0], fail_why[0], fail_e[1], fail_why[1],
                    fail_e[2], fail_why[2], fail_e[3], fail_why[3]);
            }
            s_last_count = count;
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // Spectator mode: the trampoline's RunSpectatorTick owns the sim drive
    // (it pops streamed inputs and calls original_update_game itself). This
    // hook still fires because update_game runs from inside that trampoline
    // call — but we must not run any of the player-side battle-sync /
    // Netplay_StartBattle / GekkoStressSession paths below. Just bump the
    // frame counter and pass through to the real update_game.
    if (g_spectator_mode) {
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // STRESS-TEST MODE (FM2K_STRESS_MODE=1) - single-instance determinism check
    // GekkoStressSession artificially rolls back every check_distance frames.
    // No network, no sync barriers. Menu/CSS run pass-through; battle mode
    // starts a GekkoStressSession and drives sim via the Save/Load/Advance
    // event loop (same path as online, minus the network).
    // Any desync fired here = local determinism bug. Pure repro.
    // ========================================================================
    if (g_stress_mode) {
        if (IsBattleMode(game_mode)) {
            if (!Netplay_IsActive()) {
                if (!Netplay_StartStressBattle()) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Stress: Failed to start GekkoStressSession, falling through");
                    g_frame_counter++;
                    return original_update_game ? original_update_game() : 0;
                }
            }
            if (!Netplay_ProcessBattleInputPhase()) {
                return 0;
            }
            g_frame_counter++;
            return 0;
        }
        // Menu / CSS / results: run game normally
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // SYNC BARRIER - Block game until both clients are connected
    // CCCaster-style: return 0 to freeze game at menu until connection
    // ========================================================================
    if (!Netplay_IsConnected()) {
        // Keep trying to connect
        static uint32_t last_poll = 0;
        static uint32_t block_count = 0;
        uint32_t now = GetTickCount();

        // Poll control channel to process HELLO/HELLO_ACK
        ControlChannel_Poll();

        // Send HELLO periodically until connected
        if (now - last_poll > 500) {
            ControlChannel_SendHello(static_cast<uint8_t>(g_player_index),
                                     fm2k::game_hash::Compute());
            last_poll = now;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SYNC BARRIER: Blocking game (P%d, mode=%u, blocked %u times)",
                g_player_index + 1, game_mode, block_count);
        }

        block_count++;

        // BLOCK GAME - return 0 to prevent any game state updates
        // This keeps both clients at the same starting point
        return 0;
    }

    // Log when we first pass the barrier
    static bool barrier_passed = false;
    if (!barrier_passed) {
        barrier_passed = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SYNC BARRIER PASSED: P%d connected, game_mode=%u, frame=%u",
            g_player_index + 1, game_mode, g_frame_counter);
    }

    // Check for game mode transitions (CSS <-> Battle)
    CheckGameModeTransition();

    // ========================================================================
    // CSS MODE - Delay-based with stall when remote is behind
    // Game loop calls: ProcessGameInputs -> UpdateGameState -> InputHistory
    // We must block ALL of them during stalls to prevent edge detection desync
    // ========================================================================
    if (IsCSSMode(game_mode)) {
        // ProcessCSS handles everything: poll, stall, capture, send batch.
        // Returns false if stalling (waiting for remote input + resending ours).
        if (!Netplay_ProcessCSS()) {
            return 0;  // Stall - don't update game state
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // BATTLE MODE - Sync barrier then GekkoNet rollback
    // ========================================================================
    if (IsBattleMode(game_mode)) {
        // ----------------------------------------------------------------
        // BATTLE SYNC BARRIER - Block until both clients enter battle mode
        // This ensures both start GekkoNet at the same frame
        // ----------------------------------------------------------------
        if (g_battle_entry_signaled && !Netplay_IsActive()) {
            // Poll for BATTLE_ENTERING from remote
            Netplay_PollBattleSync();

            if (!Netplay_IsBattleSynced()) {
                // Still waiting for remote - block game
                static uint32_t last_log = 0;
                uint32_t now = GetTickCount();
                if (now - last_log > 500) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "BATTLE SYNC BARRIER: Waiting for remote to enter battle mode...");
                    last_log = now;
                }
                return 0;  // Block game until synced
            }

            // Both clients synced - NOW start GekkoNet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE SYNC BARRIER PASSED: Starting GekkoNet session");
            if (Netplay_StartBattle()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session started for battle");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to start GekkoNet session!");
            }
        }

        // ----------------------------------------------------------------
        // GekkoNet active - process rollback
        // ----------------------------------------------------------------
        if (Netplay_IsActive()) {
            // Process GekkoNet frame - runs full game ticks inside each AdvanceEvent
            // (process_game_inputs + update_game), matching GekkoNet examples.
            // We do NOT call original_update_game here - it already ran.
            if (!Netplay_ProcessBattleInputPhase()) {
                // No advance event yet - keep polling
                return 0;
            }

            // GekkoNet already ran the tick(s). Just update our frame counter.
            g_frame_counter++;
            return 0;  // Skip game loop's own update - already done
        }
    }

    // ========================================================================
    // OTHER MODES (menu, results, etc.)
    // ========================================================================
    Netplay_ProcessMenu();

    g_frame_counter++;
    return original_update_game ? original_update_game() : 0;
}

// Find our game window
static HWND g_cached_window = NULL;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            if (strcmp(class_name, "KGT2KGAME") == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}

static HWND GetOurGameWindow() {
    if (g_cached_window && IsWindow(g_cached_window)) {
        return g_cached_window;
    }
    g_cached_window = NULL;
    EnumWindows(EnumWindowsProc, (LPARAM)&g_cached_window);
    return g_cached_window;
}

// FPS tracking. `g_current_fps` is read by fc_hud (via extern decl
// in imgui_overlay.cpp) so the always-on HUD shows live framerate;
// it's still file-scope-visible for everything inside this TU.
static DWORD g_last_fps_time = 0;
static int g_fps_frame_count = 0;
int g_current_fps = 0;

// Sim-fps: sampled from g_sim_step_count (incremented once per update_game
// tick across all paths). Render-fps counts renders; sim-fps counts logic
// ticks. On a heavy stage render drops but sim must hold 100 -- showing both
// makes that explicit instead of one ambiguous number. g_current_sim_fps is
// read by the title bar + overlay HUD.
static uint32_t s_last_sim_step_sample = 0;
int g_current_sim_fps = 0;

// Hook: RenderGame
// Set in the GekkoNet AdvanceEvent handler (netplay.cpp). Each advance
// produces exactly one new simulation tick; this flag says "that tick is
// unrendered". Cleared inside Hook_RenderGame after original_render_game()
// has drawn it. Any extra Hook_RenderGame invocations between advances skip
// the real render entirely, so render count cannot outpace sim count on
// either peer — both peers render exactly as many frames as GekkoNet has
// advanced. Without this gate, render mutates object-pool animation counters
// on wall-clock cadence, producing asymmetric state that feeds back into
// the next sim tick's RNG draws.
bool g_frame_pending_render = false;

// Public — called by the trampoline's render step so FPS + title bar stats
// keep updating even though Hook_RenderGame is bypassed in battle mode.
extern "C" void Hook_RenderDiagnostics_Tick();
extern "C" void Hook_RenderDiagnostics_Tick() {
    CheckOverlayHotkey();

    // Track FPS -- render (this tick fires once per render) + sim (delta of
    // the global logic-tick counter over the same 1s window).
    g_fps_frame_count++;
    DWORD now = GetTickCount();
    if (now - g_last_fps_time >= 1000) {
        g_current_fps = g_fps_frame_count;
        g_fps_frame_count = 0;
        const uint32_t sim_now = g_sim_step_count;
        g_current_sim_fps = (int)(sim_now - s_last_sim_step_sample);
        s_last_sim_step_sample = sim_now;
        g_last_fps_time = now;
    }

    // Update window title with BBBR-style stats (throttled to 500ms).
    // Format follows the layout user-spec'd 2026-05-05:
    //   <game> | P1 (BATTLE) | 100fps RTT 12ms | D2 FA0.5 RB5 | vs Nick 12-3-1
    // Game-name prefix is the executable's stem (e.g. "WonderfulWorld_
    // ver_0946") instead of a literal "FM2K", so multi-game lobbies
    // are visually distinct in the taskbar. P1/P2 replaces [HOST]/
    // [CLIENT] for casual readability. W/L/D suffix comes from the
    // launcher via shared mem (FM2KSharedMemData::ui_*); -1 sentinels
    // mean "not yet known" → suffix is omitted.
    static DWORD last_title_update = 0;
    DWORD title_now = GetTickCount();
    if (title_now - last_title_update >= 500) {
        last_title_update = title_now;
        HWND game_window = GetOurGameWindow();
        if (game_window) {
            // Cache the game-name prefix once. Use the .exe's stem so it
            // works for any FM2K game, not just WonderfulWorld. Strips
            // the directory, drops the ".exe" suffix.
            //
            // Stored as UTF-8 because the rest of the title-format code
            // (peer nicks from shm, the final MultiByteToWideChar at the
            // bottom) assumes UTF-8. Going through GetModuleFileNameW
            // first avoids the SJIS-vs-UTF-8 mojibake we'd hit if we
            // pulled bytes directly via GetModuleFileNameA — those bytes
            // are SJIS (our locale spoof preserves SJIS via
            // WC_NO_BEST_FIT_CHARS) and re-interpreting them as UTF-8
            // turns JP names into garbage in the title bar.
            static char s_game_prefix[256] = {};
            if (s_game_prefix[0] == '\0') {
                wchar_t wbuf[MAX_PATH];
                DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    wchar_t* wslash = wcsrchr(wbuf, L'\\');
                    if (!wslash) wslash = wcsrchr(wbuf, L'/');
                    wchar_t* wstem = wslash ? wslash + 1 : wbuf;
                    wchar_t* wdot = wcsrchr(wstem, L'.');
                    if (wdot) *wdot = L'\0';
                    WideCharToMultiByte(CP_UTF8, 0, wstem, -1,
                                        s_game_prefix, sizeof(s_game_prefix),
                                        nullptr, nullptr);
                }
                if (s_game_prefix[0] == '\0') {
                    snprintf(s_game_prefix, sizeof(s_game_prefix), "FM2K");
                }
            }

            // Format spec (user-visible 2026-05-05):
            //   <game> | P1 vs <peer> 0-0-0 (BATTLE) | 100fps RTT 12ms | D2 FA0.5 RB5
            //   <game> | P2 vs <peer> 0-0-0 (CSS)    | 100fps RTT 12ms
            //   <game> | P1 (TITLE)   | 100fps RTT 12ms | 47-30-2
            //   <game> | Offline | 100fps
            // The peer + W/L/D moves up to live next to the role tag; the
            // (STATE) trails. Falls back to overall W-L-D when no active
            // peer (idle in title / between sessions).
            char title[384];
            const char* role =
                g_spectator_mode      ? "Spec" :
                (g_player_index == 0) ? "P1"   : "P2";
            bool active = Netplay_IsActive();
            bool connected = Netplay_IsConnected();

            // Build the role+vs+wld lead chunk that sits between the game
            // prefix and the STATE / fps section. Three forms:
            //   (a) "P1 vs Armonté 12-3-1" — active match, peer + record known
            //   (b) "P1 (overall 47-30-2)" — no active peer, only personal record
            //   (c) "P1"                   — no record yet (pre-first-query)
            char lead[160] = {};
            FM2KSharedMemData* shm = GetSharedMemory();
            const bool have_peer = shm && shm->ui_peer_nick[0] != '\0' &&
                                   shm->ui_vs_wins >= 0;
            const bool have_overall = shm && shm->ui_wins >= 0;
            if (have_peer) {
                snprintf(lead, sizeof(lead),
                    "%s vs %s %d-%d-%d",
                    role, shm->ui_peer_nick,
                    shm->ui_vs_wins, shm->ui_vs_losses, shm->ui_vs_draws);
            } else if (have_overall) {
                snprintf(lead, sizeof(lead),
                    "%s (%d-%d-%d)",
                    role, shm->ui_wins, shm->ui_losses, shm->ui_draws);
            } else {
                snprintf(lead, sizeof(lead), "%s", role);
            }

            if (g_spectator_mode) {
                size_t qd = SpectatorNode_PendingFrameCount();
                // Real playback rate (popped sim frames per second), NOT
                // the trampoline loop rate: during a q:0 hold the loop
                // keeps spinning at 100/s re-rendering the same frame,
                // so g_current_fps read "100fps" while playback was
                // visibly frozen (user report 2026-06-11 18:1x). pops/s
                // is the truth the viewer cares about.
                extern uint32_t g_spec_pop_total;
                static uint32_t s_pop_prev    = 0;
                static DWORD    s_pop_prev_ms = 0;
                uint32_t play_fps = 0;
                {
                    const DWORD nowp = GetTickCount();
                    if (s_pop_prev_ms != 0 && nowp > s_pop_prev_ms) {
                        play_fps = (g_spec_pop_total - s_pop_prev) * 1000u /
                                   (nowp - s_pop_prev_ms);
                    }
                    s_pop_prev    = g_spec_pop_total;
                    s_pop_prev_ms = nowp;
                }
                // Differentiate offline-replay (file-driven, no peer) from
                // live-spec (subscribed to a host's stream). Replay mode
                // never has an upstream so "Connecting..." would be a lie
                // — it's already loaded everything from disk.
                static int s_replay_cached = -1;
                if (s_replay_cached < 0) {
                    const char* rp = std::getenv("FM2K_REPLAY_FILE");
                    s_replay_cached = (rp && rp[0]) ? 1 : 0;
                }
                if (s_replay_cached == 1) {
                    uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                    const char* phase =
                        (mode == 0)                          ? "BOOT" :
                        (mode == 1000)                       ? "TITLE" :
                        (mode == 2000)                       ? "CSS" :
                        (mode >= 3000 && mode < 4000)        ? "BATTLE" :
                                                               "POST";
                    snprintf(title, sizeof(title),
                        "%s | Replay (%s) | %ufps | q:%zu",
                        s_game_prefix, phase, play_fps, qd);
                } else {
                    // Three-state: a TCP heal while the subscription rides
                    // on UDP is "Resyncing..." -- NOT a cold "Connecting..."
                    // (the old binary label flapped to Connecting during
                    // transport churn even though playback never stopped,
                    // which read as a full drop to the user).
                    const bool sub = SpectatorNode_IsSubscribedUpstream();
                    const char* status =
                        !sub                                   ? "Connecting..."
                        : SpectatorNode_IsTcpRejoinPending()   ? "Resyncing..."
                                                               : "Subscribed";
                    snprintf(title, sizeof(title),
                        "%s | %s %s | %ufps | q:%zu",
                        s_game_prefix, role, status,
                        play_fps, qd);
                }
            } else if (active) {
                GekkoNetworkStats stats = Netplay_GetNetworkStats();
                float ahead = Netplay_GetFramesAhead();
                int delay = Netplay_GetLocalDelay();
                uint32_t desyncs = Netplay_GetDesyncCount();
                uint32_t rollbacks = Netplay_GetRollbackCount();
                const char* tag = g_stress_mode ? "STRESS" : "BATTLE";
                if (desyncs > 0) {
                    snprintf(title, sizeof(title),
                        "%s | %s (%s) | %dsim/%drdr RTT %ums | D%d FA%.1f RB%u | DESYNC x%u",
                        s_game_prefix, lead, tag, g_current_sim_fps, g_current_fps,
                        stats.last_ping, delay, ahead, rollbacks, desyncs);
                } else {
                    snprintf(title, sizeof(title),
                        "%s | %s (%s) | %dsim/%drdr RTT %ums | D%d FA%.1f RB%u",
                        s_game_prefix, lead, tag, g_current_sim_fps, g_current_fps,
                        stats.last_ping, delay, ahead, rollbacks);
                }
            } else if (connected) {
                uint32_t ping = Netplay_GetPingMs();
                uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                const char* phase = nullptr;
                char phase_buf[16] = {};
                if      (mode == 0)               phase = "BOOT";
                else if (mode == 1000)            phase = "TITLE";
                else if (mode == 2000)            phase = "CSS";
                else if (mode >= 3000 && mode < 4000) phase = "BATTLE";
                else {
                    std::snprintf(phase_buf, sizeof(phase_buf), "MODE %u",
                                  (unsigned)mode);
                    phase = phase_buf;
                }
                snprintf(title, sizeof(title),
                    "%s | %s (%s) | %dsim/%drdr RTT %ums",
                    s_game_prefix, lead, phase, g_current_sim_fps, g_current_fps, ping);
            } else if (!g_offline_mode) {
                snprintf(title, sizeof(title),
                    "%s | %s | Connecting... | %dsim/%drdr",
                    s_game_prefix, lead, g_current_sim_fps, g_current_fps);
            } else {
                snprintf(title, sizeof(title),
                    "%s | Offline | %dsim/%drdr", s_game_prefix,
                    g_current_sim_fps, g_current_fps);
            }

            // SetWindowTextW + UTF-8 → UTF-16 conversion: SetWindowTextA
            // interprets the byte stream as the system's ANSI codepage,
            // turning UTF-8 "é" (C3 A9) into garbled "Ã©" on a CP1252
            // install. Using the wide path keeps "Armonté" / "ＣＰＷ" /
            // "テスト" intact regardless of system locale.
            wchar_t wtitle[384];
            int wn = MultiByteToWideChar(CP_UTF8, 0, title, -1,
                                         wtitle,
                                         (int)(sizeof(wtitle) / sizeof(wtitle[0])));
            if (wn > 0) {
                // DefWindowProcW(WM_SETTEXT) bypasses the WNDPROC chain so
                // a third-party subclass (cnc-ddraw) that reverted the
                // window's IsUnicode flag can't apply a CP_ACP=1252 W→A
                // bridge here and destroy JP chars on the way to storage.
                DefWindowProcW(game_window, WM_SETTEXT, 0, (LPARAM)wtitle);
            } else {
                SetWindowTextA(game_window, title);  // shouldn't happen
            }
        }
    }
}

// Mike Z sound rollback: intercept the SFX branch of FM2K's script sound
// dispatcher. During battle, instead of playing immediately we record the
// requested play into `desired[channel]`. Once per displayed frame (after
// the advance batch completes) SoundRollback::SyncAfterAdvance reconciles
// desired ↔ actual and issues real DSound stops/plays with the rollback-
// window filter that prevents erased/re-triggered sounds from clipping.
//
// Script item layout (42 bytes, from DispatchScriptSoundCommand decomp):
//   +36  void*  SoundBufferArray ptr       (SFX case)
//   +40  uint8  cmd byte (low nibble: 0=stop 1=SFX 2=MIDI 3=CD; bit 0x10=volume flag)
//   +41  uint8  CD track number             (CD case)
//
// MIDI and CD paths (music) pass through unchanged — music-restart on
// rollback is a v2 concern.
typedef int(__cdecl* DispatchScriptSoundFunc)(int);
static DispatchScriptSoundFunc original_dispatch_script_sound = nullptr;

int __cdecl Hook_DispatchScriptSoundCommand(int script_item) {
    // Spectator catch-up mute (C5.5). While the spectator is burning
    // through queued events to reach live edge, we run sim only — no
    // render, no audio. Without this, joining 30k events late would
    // blast 5 minutes of compressed audio in the few seconds it takes
    // to drain. SoundRollback's dedup table still updates on the host
    // pass (RecordDesired runs as part of the sim), so steady-state
    // dispatch stays correct once catch-up clears.
    if (g_spectator_catchup) {
        return 0;
    }

    // Mute gates — applied UNCONDITIONALLY (offline + online + spectator).
    // Re-checks the audio.ini file ~once per second so the launcher's
    // toggle reaches the running game without a separate IPC channel.
    {
        static uint32_t s_last_check = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_last_check > 1000) {
            s_last_check = now_ms;
            SoundRollback::RefreshMuteFromDisk();
        }
    }
    if (script_item != 0) {
        const uint8_t cmd_byte = *reinterpret_cast<uint8_t*>(script_item + 40);
        const uint8_t cmd_low  = cmd_byte & 0xF;
        const bool    looping  = (cmd_byte & 0x10) != 0;

        // Audio classification — verified against WonderfulWorld disasm
        // (0x403430 dispatcher) + LilithPort's BGM_VOLUME / SE_VOLUME
        // hook sites (stdafx.h:152-159, MainForm.cpp:2719-2733):
        //   cmd_low 0           → stop everything (never muted)
        //   cmd_low 1, loop=0   → SFX (one-shot WAV, vtable Play(..., 0))
        //   cmd_low 1, loop=1   → BGM as looping WAV (Play(..., 1))
        //   cmd_low 2           → BGM via MIDI (WriteTempMIDIAndPlay)
        //   cmd_low 3           → BGM via CD audio (InitializeCDAudio)
        // Most 2DFM games play BGM via case 1 + loop; case 2/3 are rare.
        const bool is_bgm = (cmd_low == 2) ||
                            (cmd_low == 3) ||
                            (cmd_low == 1 && looping);
        const bool is_sfx = (cmd_low == 1 && !looping);
        if (is_bgm && SoundRollback::IsMusicMuted()) return 0;
        if (is_sfx && SoundRollback::IsSfxMuted())   return 0;
    }

    if (!Netplay_IsActive() || script_item == 0) {
        return original_dispatch_script_sound(script_item);
    }

    uint8_t cmd = *reinterpret_cast<uint8_t*>(script_item + 40);
    if ((cmd & 0xF) != 1) {
        // Not SFX — MIDI (case 2), CD audio (case 3), or full stop (case 0).
        // These paths use MCI (mciSendCommandA), which is heavy/stateful and
        // doesn't survive the rapid-fire repeats that rollback replays cause.
        // In stress mode every displayed frame replays ~10 sim frames, so if
        // a music trigger is anywhere in that window it fires ~10 times per
        // displayed frame (1 forward + 9 replay). Even after we suppress the
        // replay branch, the FORWARD pass still re-fires every time the save
        // ring scrolls past that frame — music cuts in and out.
        //
        // Apply a "dedup by payload" filter: a (cmd, buf_ptr_or_track)
        // dispatch identical to the previous non-replay dispatch is treated
        // as a no-op. Any change — new track, stop-then-same-track, fanfare
        // switch, CD ↔ MIDI — updates the stored key and fires normally, so
        // mid-match music transitions still work. Only the GekkoNet save-ring
        // scroll's identical re-trigger gets filtered.
        // Also skip during replay so the forward-first dispatch wins.
        if (g_is_rolling_back) {
            return 0;
        }
        // +36 = buffer_array ptr (MIDI/CD paths don't use it but reading is
        // harmless since the script item is always 42 bytes of valid memory)
        // +41 = CD track number (case 3 only)
        uint32_t payload = *reinterpret_cast<uint32_t*>(script_item + 36)
                         ^ *reinterpret_cast<uint8_t*> (script_item + 41);
        if (SoundRollback::IsRedundantMusicDispatch(cmd, payload)) {
            return 0;  // identical music command as last time — leave MCI alone
        }
        return original_dispatch_script_sound(script_item);
    }

    void* arr = *reinterpret_cast<void**>(script_item + 36);
    if (!arr || !SoundRollback::RecordDesired(arr, script_item, Netplay_GetFrame())) {
        // Unknown / null channel — not in g_sound_channel_table. Fall through
        // to the original dispatcher so the sound still plays (without
        // rollback tracking). The vast majority of FM2K SFX buffer_arrays
        // appear to be allocated outside the system table; we only Mike-Z the
        // ones we can identify.
        return original_dispatch_script_sound(script_item);
    }
    // Known channel — desired[] updated; defer the real play to
    // SoundRollback::SyncAfterAdvance at end of the displayed frame.
    return 1;
}

// Render reimplementation config (FM2K_BLIT_SIMD) + profiler gate, set at
// hook install. g_blit_cfg.mode==Off means profiler-only (FM2K_RENDER_PROFILE).
static fm2k::render_simd::Config g_blit_cfg;

// Verify mode: render the original blit, snapshot the dirty box, restore the
// pre-blit pixels, render OUR reimplementation, and compare. Leaves OUR output
// in the framebuffer (so the displayed frame is the reimpl, eyeballable) and
// logs the first mismatches. Box-scoped so the per-blit cost stays bounded.
static void BlitVerify(int sprite_desc, int src_pixels, int palette_lut,
                       int dst_x, int dst_y, int width, int height, short flags) {
    static uint16_t s_pre[640 * 480];
    static uint16_t s_org[640 * 480];
    static int s_logged = 0;
    uint8_t* fb = *(uint8_t**)0x4246CC;
    int bx0 = dst_x < 0 ? 0 : dst_x, by0 = dst_y < 0 ? 0 : dst_y;
    int bx1 = dst_x + width, by1 = dst_y + height;
    if (bx1 > 640) bx1 = 640;
    if (by1 > 480) by1 = 480;
    if (bx0 >= bx1 || by0 >= by1) {
        original_blit_sprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
        return;
    }
    const int bw = bx1 - bx0, bh = by1 - by0;
    const int rowbytes = bw * 2;
    for (int y = 0; y < bh; ++y) memcpy(&s_pre[y*bw], fb + 1280*(by0+y) + 2*bx0, rowbytes);
    original_blit_sprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
    for (int y = 0; y < bh; ++y) memcpy(&s_org[y*bw], fb + 1280*(by0+y) + 2*bx0, rowbytes);
    for (int y = 0; y < bh; ++y) memcpy(fb + 1280*(by0+y) + 2*bx0, &s_pre[y*bw], rowbytes);
    // allow_threads=true so verify exercises the row-band path -> validates the
    // band-split math is bit-exact against the original, not just the inline path.
    fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y,
                                  width, height, flags,
                                  g_blit_cfg.mode == fm2k::render_simd::Mode::Simd, true);
    for (int y = 0; y < bh; ++y) {
        const uint16_t* row = (const uint16_t*)(fb + 1280*(by0+y) + 2*bx0);
        const uint16_t* org = &s_org[y*bw];
        for (int xx = 0; xx < bw; ++xx) {
            if (row[xx] != org[xx]) {
                if (s_logged < 40) {
                    ++s_logged;
                    const uint32_t obj = *(uint32_t*)0x4CFA00;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[BLIT-VERIFY] MISMATCH blend=%d flags=0x%04X @ (%d,%d) "
                        "expected=0x%04X got=0x%04X (box %dx%d at %d,%d)",
                        obj ? *(int*)(obj+0x54) : -1, (unsigned)(uint16_t)flags,
                        bx0+xx, by0+y, org[xx], row[xx], bw, bh, bx0, by0);
                }
                return;  // one report per blit
            }
        }
    }
}

// BlitSpriteWithBlendMode @ 0x40C140 hook. Three roles by env:
//   FM2K_BLIT_SIMD=simd/scalar -> replace with our reimplementation (the fix)
//   ...,verify                 -> double-render + pixel-diff vs original
//   FM2K_RENDER_PROFILE=1 only -> time/count/area/blend census (g_blit_cfg Off)
// Pure display path — never touches sim/rollback state, so this is desync-safe.
static int __cdecl Hook_BlitSpriteWithBlendMode(int sprite_desc, int src_pixels,
                                                int palette_lut, int dst_x, int dst_y,
                                                int width, int height, short flags) {
    if (g_blit_cfg.mode != fm2k::render_simd::Mode::Off) {
        if (g_blit_cfg.verify) {
            BlitVerify(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
            return 0;
        }
        if (g_blit_cfg.mode == fm2k::render_simd::Mode::Simd) {
            // LARGE blits (>=128kpx, >=64 tall) -> our reimpl with row-band
            // threading across cores, ALL modes: this is the only way to beat
            // copy mode (at the scalar floor) and what gets the copy-bound
            // heaviest stages to 100fps.
            const int aw = width  < 0 ? -width  : width;
            const int ah = height < 0 ? -height : height;
            if ((long)aw * ah >= 131072 && ah >= 64)
                return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut,
                                                     dst_x, dst_y, width, height, flags, true, true);
            // Small blits: SSE2 wins only on the read-modify-write blend modes
            // (50%/add/sub). Copy (0) needs NO dst read (engine just does
            // `if(c) *dst=c`) and alpha (4) needs 32-bit products epi16 can't
            // hold -- both stay on the engine's already-optimal scalar loops.
            const uint32_t obj = *(volatile uint32_t*)0x4CFA00;
            const int bm = obj ? *(volatile int*)(obj + 0x54) : 0;
            if (bm >= 1 && bm <= 3)
                return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut,
                                                     dst_x, dst_y, width, height, flags, true, false);
            return original_blit_sprite(sprite_desc, src_pixels, palette_lut,
                                        dst_x, dst_y, width, height, flags);
        }
        // Scalar reimplementation (FM2K_BLIT_SIMD=scalar) -- debug/verify path.
        return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y,
                                             width, height, flags, false);
    }
    // Profiler-only census path.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const int ret = original_blit_sprite(sprite_desc, src_pixels, palette_lut,
                                         dst_x, dst_y, width, height, flags);
    QueryPerformanceCounter(&t1);
    g_rp_blit_ns += (uint64_t)(t1.QuadPart - t0.QuadPart);
    ++g_rp_blit_calls;
    int w = width  < 0 ? -width  : width;
    int h = height < 0 ? -height : height;
    g_rp_blit_area += (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
    const uint32_t obj = *(volatile uint32_t*)0x4CFA00;  // g_object_data_ptr
    if (obj) {
        const int m = *(volatile int*)(obj + 0x54);      // 0=copy 1=50% 2=add 3=sub 4=alpha
        if (m >= 0 && m <= 4) ++g_rp_blit_mode[m];
    }
    return ret;
}

// sprite_rendering_engine @ 0x40CC30 hook (FM2K_BLIT_SIMD only). Intercepts
// the case -10 full-screen feedback blur (render_mode obj[+0x10]==-10, which
// does ONLY the blur), runs our reimplementation, and skips the original. All
// other render modes fall through untouched.
static void __cdecl Hook_SpriteRenderingEngine(int mode_arg) {
    const uint32_t obj = *(uint32_t*)0x4CFA00;
    if (obj && *(int*)(obj + 0x10) == -10) {
        const int passes = *(int*)(obj + 342) / 20;       // obj[+342]/20 (case -10)
        if (passes > 0) {
            uint16_t* fb = *(uint16_t**)0x4246CC;
            const bool fmt565 = (*(int*)0x424704 != 0);
            const bool simd = (g_blit_cfg.mode == fm2k::render_simd::Mode::Simd);
            if (g_blit_cfg.verify) {
                // Full-screen verify: original blur, snapshot, restore, ours, diff.
                static uint16_t s_pre[640 * 480];
                static uint16_t s_org[640 * 480];
                static int s_blurlog = 0;
                const int N = 640 * 480;
                memcpy(s_pre, fb, N * 2);
                original_sprite_render_engine(mode_arg);   // runs original blur
                memcpy(s_org, fb, N * 2);
                memcpy(fb, s_pre, N * 2);
                fm2k::render_simd::BlurFullscreen(fb, passes, fmt565, simd);
                if (memcmp(fb, s_org, N * 2) != 0 && s_blurlog < 10) {
                    ++s_blurlog;
                    int fi = 0; for (; fi < N; ++fi) if (fb[fi] != s_org[fi]) break;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[BLUR-VERIFY] MISMATCH passes=%d fmt565=%d @px%d expected=0x%04X got=0x%04X",
                        passes, (int)fmt565, fi, s_org[fi], fb[fi]);
                }
            } else {
                fm2k::render_simd::BlurFullscreen(fb, passes, fmt565, simd);
            }
            return;  // skip original (case -10 does nothing but the blur)
        }
    }
    original_sprite_render_engine(mode_arg);
}

void __cdecl Hook_RenderGame() {
    // FM95 host-driven trampoline: if Hook_UpdateGameState already drove
    // RenderFrameWithSnapshot inside the tick (non-NATIVE phase), the
    // host's natural render_game call would render twice. Skip the body
    // and clear the flag.
    if constexpr (FM2K::kIsFM95) {
        if (g_fm95_skip_next_render) {
            g_fm95_skip_next_render = false;
            return;
        }
    }

    // In trampoline mode, render goes through RenderFrameWithSnapshot in
    // main_loop_trampoline.cpp. This hook still catches direct calls from
    // the game (e.g. init/menu paths) — run diagnostics and pass through.
    // Also fires under FM2K_BYPASS_TRAMPOLINE=1 (vanilla main_game_loop)
    // so [EB] diag works for A/B testing the trampoline against the
    // vanilla render path.
    Hook_RenderDiagnostics_Tick();
    EbDiag_Dump("PRE-SAVE");

    // CRITICAL: Save/restore RNG around render to prevent render-path RNG
    // consumption from breaking determinism. ProcessShakeEffect and
    // ProcessColorInterpolation call game_rand() during rendering, and
    // Render-path state protection.
    //
    // Stress-mode desync dump (FM2K_stress_desync_f158.log) showed that
    // after a LOAD+replay the four regions below diverged from the forward
    // save even though memcpy restore ran correctly. Cause: the render
    // path (ProcessShakeEffect / ProcessColorInterpolation / sprite
    // updates) mutates these regions, and our render gate skips render
    // during replay frames. Forward ran N renders, replay ran 0 renders,
    // so render-side mutations accumulated only in forward. Result:
    //   RNG_Seed, ObjectPool, AfterimagePool, InputTracking all drifted.
    // CharDynamic / GameState / Object topology stayed matched because
    // render doesn't touch them.
    //
    // Fix: snapshot these regions before render, restore after render.
    // Same idea as the existing RNG protection, extended to the other
    // three. That way render can freely update visual counters but the
    // gameplay-authoritative memory image is unchanged across renders.
    //
    // SPECTATOR FIX: include SpectatorNode_IsPlayingBack() — without it,
    // Hook_RenderGame (this function) ran UNPROTECTED on spectators when
    // FM2K-internal code triggered render directly (instead of through the
    // trampoline's RenderFrameWithSnapshot, which already had this check).
    // ProcessShakeEffect / ProcessColorInterpolation / sprite_rendering_engine
    // call game_rand() — those calls accumulated into spectator's RNG but
    // were rolled back on host. RNG drifted by exactly that delta over time,
    // showing up as paired [HOST-FP]/[SPEC-FP] divergence with all other
    // sim state matching (HP/timer/pos/input identical, only RNG differed).
    bool protect_regions = Netplay_IsActive() || SpectatorNode_IsPlayingBack();

    static uint8_t s_saved_object_pool[0x5F800];
    static uint8_t s_saved_afterimage_pool[WaveCAddrs::AFTERIMAGE_POOL_SZ];
    static uint8_t s_saved_input_tracking[0xA0];

    // Carve-outs: SHAKE_EFFECTS @ 0x447DA9 (40 B) and EFFECT_SYS1 @
    // 0x447D7D (42 B, palette flash 1) MUST NOT be restored, or render's
    // per-frame mutations to those regions get reverted every render.
    // Mirrors the carve-outs in main_loop_trampoline.cpp::
    // RenderFrameWithSnapshot. Both regions sit inside the afterimage_pool
    // slice; EFFECT_SYS1 is just before the shake block.
    constexpr uintptr_t kPflash1Addr   = 0x447D7D;
    constexpr size_t    kPflash1Size   = 42;
    constexpr size_t    kPflash1Offset = kPflash1Addr - WaveCAddrs::AFTERIMAGE_POOL;
    constexpr size_t    kPflash1End    = kPflash1Offset + kPflash1Size;
    constexpr uintptr_t kShakeAddr     = 0x447DA9;
    constexpr size_t    kShakeSize     = 40;
    constexpr size_t    kShakeOffset   = kShakeAddr - WaveCAddrs::AFTERIMAGE_POOL;  // 0x479
    constexpr size_t    kShakeEnd      = kShakeOffset + kShakeSize;
    static_assert(kPflash1End <= kShakeOffset, "EFFECT_SYS1 must end before shake block");
    if (protect_regions) {
        // RNG intentionally NOT saved/restored — render's game_rand calls
        // need to propagate so palette mode 3 / shake mode 4 produce
        // animated random values matching vanilla. See trampoline comment.
        //
        // Object pool also NOT saved/restored — palette mode 1
        // (Tyrogue fade-to-black) needs ProcessColorInterpolation's per-frame
        // writes to g_object_data_ptr+68/72/76/80 to PERSIST into the next
        // frame's object pool, otherwise the fade visually undoes itself
        // when the timer reaches 0.
        (void)s_saved_object_pool;
        // Afterimage save: three slices skipping both EFFECT_SYS1 and shake.
        memcpy(s_saved_afterimage_pool,
               (void*)WaveCAddrs::AFTERIMAGE_POOL,
               kPflash1Offset);
        memcpy(s_saved_afterimage_pool + kPflash1End,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + kPflash1End),
               kShakeOffset - kPflash1End);
        memcpy(s_saved_afterimage_pool + kShakeEnd,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + kShakeEnd),
               WaveCAddrs::AFTERIMAGE_POOL_SZ - kShakeEnd);
        memcpy(s_saved_input_tracking,  (void*)0x447EE0,                     0xA0);
    }

    // In battle mode under GekkoNet, render only when a new sim tick has
    // been produced since the last render. Otherwise render mutates
    // object-pool animation state on wall-clock cadence and desyncs peers.
    bool gate_render = Netplay_IsActive();
    bool do_render = !gate_render || g_frame_pending_render;
    EbDiag_Dump("PRE-RENDER");
    if (do_render && original_render_game) {
        // Render RNG isolation: re-seed the render stream from the gameplay
        // seed, then route render's game_rand draws to it (see globals.h /
        // Hook_GameRand). Gameplay seed is left untouched by render.
        g_render_rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        g_in_render_rng = true;
        original_render_game();
        g_in_render_rng = false;
        g_frame_pending_render = false;
    }
    EbDiag_Dump("POST-RENDER");

    if (protect_regions) {
        // (RNG and object_pool restores removed — see save block.)
        // Afterimage restore: mirror of the 3-slice split save — both
        // EFFECT_SYS1 and shake regions in live memory keep whatever
        // render-side code just wrote.
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
               s_saved_afterimage_pool,
               kPflash1Offset);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + kPflash1End),
               s_saved_afterimage_pool + kPflash1End,
               kShakeOffset - kPflash1End);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + kShakeEnd),
               s_saved_afterimage_pool + kShakeEnd,
               WaveCAddrs::AFTERIMAGE_POOL_SZ - kShakeEnd);
        memcpy((void*)0x447EE0,                    s_saved_input_tracking,  0xA0);
    }
    EbDiag_Dump("POST-RESTORE");

    // Update shared memory with current stats for launcher
    SharedMem_Update();

    // GekkoNet frame-pacing drift correction now lives in the
    // trampoline's SleepToTarget — that function applies the 1.6%
    // slowdown when ahead of peer. The Sleep(extra_ms) trick that
    // used to be in Netplay_HandleFrameTime was unreliable (Sleep
    // granularity, fights with QPC-based outer loop) and didn't
    // actually converge frames_ahead.
}

// Hook: RunGameLoop
// Detours to the main-loop trampoline; we own the outer game loop from this
// point forward. Pre-trampoline side effects (VS-mode patch) fire before the
// hand-off so CSS behavior is unchanged.
BOOL __cdecl Hook_RunGameLoop() {
    // Set VS player mode once — FM2K-only: 0x470058 is the FM2K char-select
    // mode flag, not anything meaningful on FM95.
    if constexpr (FM2K::kIsFM2K) {
        static bool vs_mode_set = false;
        if (!vs_mode_set) {
            uint8_t* char_select_mode = (uint8_t*)0x470058;
            DWORD old_protect;
            if (VirtualProtect(char_select_mode, 1, PAGE_READWRITE, &old_protect)) {
                *char_select_mode = 1;  // VS player mode
                VirtualProtect(char_select_mode, 1, old_protect, &old_protect);
                vs_mode_set = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Set VS player mode");
            }
        }
    }

    // Diagnostic: FM2K_BYPASS_TRAMPOLINE=1 falls through to vanilla
    // main_game_loop. All other hooks (input, update, render, RNG) still
    // fire as detours, so we can isolate the trampoline as a cause vs the
    // individual hooks. Use only for offline tests — netplay/spectator
    // require the trampoline's phase dispatcher to drive Save/Load/Advance.
    static const char* env_bypass = std::getenv("FM2K_BYPASS_TRAMPOLINE");
    static bool bypass = (env_bypass && std::strcmp(env_bypass, "1") == 0);
    if (bypass) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: FM2K_BYPASS_TRAMPOLINE=1 — calling vanilla "
                    "main_game_loop. Trampoline phase dispatcher "
                    "DISABLED. Netplay/spectator will not work.");
        return original_run_game_loop ? original_run_game_loop() : TRUE;
    }

    return TrampolineMainLoop();
}

// RNG-call trace — gated on FM2K_RNG_TRACE=1 env var. Each call records
// (call_index, caller_pc, rng_pre, rng_post) as 16-byte little-endian
// records to FM2K_rng_trace_pid<PID>.bin in cwd. Used to diff host vs
// spectator processes and find the first divergent rng call site.
//
// Off-by-default (no env var): trampoline branch is just two predictable
// loads + a never-taken branch, single-digit nanoseconds added to the hook.
namespace {
static FILE*    g_rng_trace_fp        = nullptr;
static uint64_t g_rng_call_index      = 0;
static bool     g_rng_trace_resolved  = false;
static bool     g_rng_trace_enabled   = false;
static uint32_t g_rng_trace_max_calls = 0;  // 0 = unlimited

static void RngTrace_ResolveOnce() {
    if (g_rng_trace_resolved) return;
    g_rng_trace_resolved = true;
    const char* env = std::getenv("FM2K_RNG_TRACE");
    g_rng_trace_enabled = (env && std::strcmp(env, "1") == 0);
    const char* env_max = std::getenv("FM2K_RNG_TRACE_MAX");
    if (env_max) {
        g_rng_trace_max_calls = (uint32_t)std::strtoul(env_max, nullptr, 10);
    } else {
        g_rng_trace_max_calls = 2'000'000;  // ~32 MB cap default
    }
    if (g_rng_trace_enabled) {
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_rng_trace_pid%lu.bin",
                      (unsigned long)GetCurrentProcessId());
        char path[MAX_PATH];
        if (!Fm2k_BuildLogPath(path, sizeof(path), base)) {
            std::snprintf(path, sizeof(path), "%s", base);
        }
        g_rng_trace_fp = std::fopen(path, "wb");
        if (g_rng_trace_fp) {
            // Larger buffer keeps the per-call overhead amortized.
            std::setvbuf(g_rng_trace_fp, nullptr, _IOFBF, 1 << 20);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "RngTrace: enabled — writing to %s (max=%u calls)",
                        path, g_rng_trace_max_calls);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "RngTrace: failed to open %s for write", path);
            g_rng_trace_enabled = false;
        }
    }
}
}

// Public API: flush the rng-trace FILE buffer. Called from netplay.cpp's
// auto-terminate path before TerminateProcess, so the trace records that
// are still in stdio's user-space buffer reach disk. Safe no-op when the
// trace isn't enabled.
void Hook_FlushRngTrace() {
    if (g_rng_trace_fp) {
        std::fflush(g_rng_trace_fp);
    }
}

// Hook: GameRand
// Records the call to the trace file (if enabled) then forwards.
uint32_t __cdecl Hook_GameRand() {
    // Render RNG stream isolation (see globals.h). During render_game, draw
    // from g_render_rng_seed via a seed-swap so render's color/effect
    // randomness NEVER advances the gameplay seed (0x41FB1C). That keeps the
    // gameplay rng cadence-independent -> deterministic across rollback and
    // across peers (the root fix for the palette/afterimage desync + crash).
    // The render seed is re-seeded from the gameplay seed each render, so
    // colors stay deterministic per confirmed frame and identical on both
    // peers. Render rng is intentionally NOT traced (it's a separate stream).
    if (g_in_render_rng) {
        ++g_render_rand_calls;  // #63 diag: count render-side rng draws
        const uint32_t gameplay = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = g_render_rng_seed;
        const uint32_t r = original_game_rand ? original_game_rand() : 0;
        g_render_rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = gameplay;  // gameplay seed untouched
        return r;
    }
    RngTrace_ResolveOnce();
    if (!g_rng_trace_enabled) {
        return original_game_rand ? original_game_rand() : 0;
    }
    // Read direct caller (0x004139A8 wrapper). For the caller-OF-caller
    // (which would identify the actual game function), walk the frame
    // pointer chain manually with a guard. __builtin_return_address(1)
    // AVs inside MinHook's trampoline because the trampoline doesn't
    // preserve EBP. Instead we read EBP, dereference for the caller's
    // saved EBP, then read [EBP+4] for the caller-of-caller's return
    // address. Wrapped with IsBadReadPtr to avoid AV on broken chains.
    const uint32_t ret_addr = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t ret_addr_2 = 0;
    {
        uintptr_t cur_bp = (uintptr_t)__builtin_frame_address(0);
        // Walk one frame up: *(bp) = caller's saved bp; *(bp+4) = caller's return addr
        // Then walk one more: *(caller_bp) = caller-of-caller's saved bp; *(caller_bp+4) = its return
        // We want caller-of-caller's return addr.
        if (cur_bp && !IsBadReadPtr((void*)cur_bp, 8)) {
            uintptr_t caller_bp = *(uintptr_t*)cur_bp;
            if (caller_bp && !IsBadReadPtr((void*)caller_bp, 8)) {
                ret_addr_2 = *(uint32_t*)(caller_bp + 4);
            }
        }
    }
    const uint32_t pre  = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    const uint32_t r    = original_game_rand ? original_game_rand() : 0;
    const uint32_t post = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    if (g_rng_trace_fp &&
        (g_rng_trace_max_calls == 0 || g_rng_call_index < g_rng_trace_max_calls))
    {
        uint32_t rec[6] = {
            (uint32_t)g_rng_call_index, ret_addr, ret_addr_2, pre, post, 0,
        };
        std::fwrite(rec, 1, sizeof(rec), g_rng_trace_fp);
        if ((g_rng_call_index & 0x1FFFu) == 0) std::fflush(g_rng_trace_fp);
    }
    ++g_rng_call_index;
    return r;
}

// Hook: ProcessGameInputs
// During battle: get synced inputs from GekkoNet and write to game memory
int __cdecl Hook_ProcessGameInputs() {
    // Re-pin the FPU control word on every game tick. DirectDraw's
    // SetCooperativeLevel is called without DDSCL_FPUPRESERVE, so DD is
    // allowed to mutate x87 precision at fullscreen toggle / driver callback
    // time. Without this line, two peers can run at different float
    // precision and float-heavy code (movement vectors, hit-rect math)
    // diverges on the first substantial physics tick — which matches the
    // "desync starts when you move" signature exactly.
    PinFPUControlWord();

    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Stress-test mode: block game's own process_game_inputs during battle -
    // GekkoNet drives sim via AdvanceEvent (which calls original_process_game_inputs
    // internally). Outside battle, pass through normally.
    if (g_stress_mode) {
        if (IsBattleMode(game_mode) && Netplay_IsActive()) {
            return 0;
        }
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Battle mode with GekkoNet - block during sync, override inputs when active
    if (IsBattleMode(game_mode) && !g_offline_mode && Netplay_IsConnected()) {
        // Block ProcessGameInputs during battle sync barrier and GekkoNet handshake
        // Same reason as CSS: prevents buf_idx advance and edge detection desync
        if (!Netplay_IsActive() || !Netplay_IsSessionReady()) {
            return 0;
        }

        // GekkoNet active: ProcessBattleInputPhase handles process_game_inputs
        // inside each AdvanceEvent. Don't call original here - it would double-tick.
        // Just log periodically.
        static uint32_t log_count = 0;
        if (log_count < 10 || log_count % 200 == 0) {
            uint32_t p1_stored = *(uint32_t*)FM2K::ADDR_P1_INPUT;
            uint32_t p2_stored = *(uint32_t*)FM2K::ADDR_P2_INPUT;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PROCESS_INPUTS] Synced: P1=0x%03X P2=0x%03X (buf_idx=%u)",
                p1_stored, p2_stored, *(uint32_t*)0x447EE0);
        }
        log_count++;

        return 0;  // Skip - GekkoNet drives input processing
    }

    // CSS mode - block ProcessGameInputs during stalls!
    // Game loop calls ProcessGameInputs BEFORE UpdateGameState.
    // If we let it run during stalls, it advances g_input_buffer_index
    // and runs edge detection out of sync between clients.
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        Netplay_PollCSS();  // Receive pending data

        if (!Netplay_CanAdvanceCSS()) {
            // STALL: Don't call original - prevents buffer index advance
            // and edge detection from consuming inputs during stall
            return 0;
        }

        // Not stalling - let original run (it calls GetPlayerInput which
        // returns synced CSS input through our hook)
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Connection barrier - block while waiting for connection
    // Prevents buf_idx divergence before game even starts
    if (!g_offline_mode && !Netplay_IsConnected()) {
        return 0;
    }

    // Offline or connected non-CSS/non-battle: use original
    return original_process_game_inputs ? original_process_game_inputs() : 0;
}

// ============================================================================
// HOOK SETUP
// ============================================================================

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Initializing MinHook...");

    PinFPUControlWord();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: Pinned x87 FPU control word to _PC_53 | _RC_NEAR");

    // The locale spoof (InstallLocaleSpoof) already initializes MinHook on
    // FM95 builds and on any FM2K build with FM2K_JP_LOCALE=1, so accept
    // MH_ERROR_ALREADY_INITIALIZED as a no-op success here.
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Hooks: MH_Initialize failed: %d", (int)s);
            return false;
        }
    }

    // Hook GetPlayerInput — FM2K only.
    //
    // FM2K's get_player_input is `int __cdecl(int player_id, int input_type)`,
    // a single function called for both players with input_type selecting
    // which control mode to use. Our Hook_GetPlayerInput matches that
    // signature.
    //
    // FM95 splits this into two SEPARATE single-arg functions —
    // get_player_input_p1 (0x408AE0) and get_player_input_p2 (0x408D60),
    // each `int __cdecl(int player_idx)`. Hooking either with our 2-arg
    // shape would corrupt the stack on entry and inject the wrong
    // FM2K-style keybindings into CPW's native input read.
    //
    // The right surface for FM95 input injection is Hook_ProcessGameInputs
    // (0x408FF0, single arg, hooked below) — it writes directly into
    // g_p1/p2_input_history[buf_idx] AFTER the natural keyboard/joystick
    // poll, so we can override for netplay without disrupting the host's
    // ini-driven key bindings or joyGetPosEx gamepad path.
    if constexpr (FM2K::kIsFM2K) {
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT, (void*)Hook_GetPlayerInput,
                          (void**)&original_get_player_input) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GetPlayerInput");
            return false;
        }
    } else {
        // FM95: hook BOTH split single-arg functions so our binder layer
        // applies to both players. Same effect as FM2K's single hook —
        // user-rebindable keyboard + XInput gamepad, facing flip applied
        // engine-relative, START stripped on CSS.
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT,    // 0x408AE0 P1
                          (void*)Hook_GetPlayerInput_FM95_P1,
                          (void**)&original_get_player_input_p1) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook get_player_input_p1");
            return false;
        }
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT_P2, // 0x408D60 P2
                          (void*)Hook_GetPlayerInput_FM95_P2,
                          (void**)&original_get_player_input_p2) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT_P2) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook get_player_input_p2");
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: Installed FM95 split input hooks (p1=0x%08X, p2=0x%08X) "
                    "→ FM2KInputBinder + facing flip via g_p_facing_snap",
                    (unsigned)FM2K::ADDR_GET_PLAYER_INPUT,
                    (unsigned)FM2K::ADDR_GET_PLAYER_INPUT_P2);
    }

    // Hook UpdateGameState
    if (MH_CreateHook((void*)FM2K::ADDR_UPDATE_GAME, (void*)Hook_UpdateGameState,
                      (void**)&original_update_game) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_UPDATE_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook UpdateGameState");
        return false;
    }

    // Hook RenderGame
    if (MH_CreateHook((void*)FM2K::ADDR_RENDER_GAME, (void*)Hook_RenderGame,
                      (void**)&original_render_game) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_RENDER_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RenderGame");
        return false;
    }

    // Render reimplementation (FM2K_BLIT_SIMD) and/or profiler census
    // (FM2K_RENDER_PROFILE). FM2K-only; installed only when one of those envs
    // is set, so normal play carries zero extra hooks. The blit-leaf hook
    // serves both roles; the sprite-engine hook (case -10 blur) only matters
    // for the reimplementation. All display-only -> desync-safe.
    if constexpr (FM2K::kIsFM2K) {
        g_blit_cfg = fm2k::render_simd::ParseConfig();
        const char* rp = std::getenv("FM2K_RENDER_PROFILE");
        const bool want_profile = (rp && rp[0] == '1');
        const bool want_simd    = (g_blit_cfg.mode != fm2k::render_simd::Mode::Off);
        if (want_profile || want_simd) {
            if (MH_CreateHook((void*)FM2K::ADDR_BLIT_SPRITE,
                              (void*)Hook_BlitSpriteWithBlendMode,
                              (void**)&original_blit_sprite) != MH_OK ||
                MH_QueueEnableHook((void*)FM2K::ADDR_BLIT_SPRITE) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook BlitSpriteWithBlendMode");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: BlitSpriteWithBlendMode @ 0x%08X hooked (simd=%d verify=%d profile=%d)",
                    (unsigned)FM2K::ADDR_BLIT_SPRITE,
                    (int)(g_blit_cfg.mode == fm2k::render_simd::Mode::Simd),
                    (int)g_blit_cfg.verify, (int)want_profile);
            }
        }
        if (want_simd) {
            if (MH_CreateHook((void*)FM2K::ADDR_SPRITE_RENDER_ENGINE,
                              (void*)Hook_SpriteRenderingEngine,
                              (void**)&original_sprite_render_engine) != MH_OK ||
                MH_QueueEnableHook((void*)FM2K::ADDR_SPRITE_RENDER_ENGINE) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook sprite_rendering_engine (case -10 blur)");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: sprite_rendering_engine @ 0x%08X hooked (case -10 blur reimpl)",
                    (unsigned)FM2K::ADDR_SPRITE_RENDER_ENGINE);
            }
        }
    }

    // Hook RunGameLoop — FM2K only. On FM95, ADDR_RUN_GAME_LOOP IS WinMain
    // (the frame loop is inlined into WinMain); detouring it intercepts the
    // process entry point BEFORE init runs, so the trampoline takes over
    // with an uninitialized window/game state and CPW silently dies.
    // Until the trampoline is taught to coexist with FM95's WinMain-driven
    // loop, leave the natural WinMain alone — the per-frame hooks
    // (Hook_UpdateGameState / Hook_RenderGame / Hook_ProcessGameInputs)
    // still fire from inside FM95's loop and that's enough to drive a
    // basic boot.
    if constexpr (FM2K::kIsFM2K) {
        if (MH_CreateHook((void*)FM2K::ADDR_RUN_GAME_LOOP, (void*)Hook_RunGameLoop,
                          (void**)&original_run_game_loop) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_RUN_GAME_LOOP) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RunGameLoop");
            return false;
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: SKIP RunGameLoop hook on FM95 — frame loop is inlined into WinMain");
    }

    // FM95-only: hook LoadStageFile_alt so vs-mode random-stage can rewrite
    // arg0 at call time. FM2K has no equivalent — its stage selection
    // already routes through ADDR_SELECTED_STAGE (0x43010c) which the
    // random-stage block writes directly.
    if constexpr (FM2K::kIsFM95) {
        if (FM2K::ADDR_LOAD_STAGE_FILE_ALT != 0) {
            if (MH_CreateHook((void*)FM2K::ADDR_LOAD_STAGE_FILE_ALT,
                              (void*)Hook_LoadStageFileAlt,
                              (void**)&original_LoadStageFileAlt) != MH_OK ||
                MH_QueueEnableHook((void*)FM2K::ADDR_LOAD_STAGE_FILE_ALT) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: Failed to hook LoadStageFile_alt — vs-mode "
                    "random-stage will not override game's per-character "
                    "stage table (practice-mode random still works via "
                    "direct g_practice_stage_id write).");
                // Non-fatal: random-stage is opt-in. Continue init.
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: LoadStageFile_alt hooked at 0x%08X (FM95 vs-mode random-stage)",
                    (unsigned)FM2K::ADDR_LOAD_STAGE_FILE_ALT);
            }
        }
    }

    // Hook GameRand
    if (MH_CreateHook((void*)FM2K::ADDR_GAME_RAND, (void*)Hook_GameRand,
                      (void**)&original_game_rand) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_GAME_RAND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GameRand");
        return false;
    }

    // Hook ProcessGameInputs
    if (MH_CreateHook((void*)FM2K::ADDR_PROCESS_INPUTS, (void*)Hook_ProcessGameInputs,
                      (void**)&original_process_game_inputs) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_PROCESS_INPUTS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook ProcessGameInputs");
        return false;
    }

    // Hook DispatchScriptSoundCommand — Mike Z desired/actual sound layer.
    // During battle the hook records `desired[channel]` instead of playing;
    // SoundRollback::SyncAfterAdvance reconciles once per displayed frame by
    // calling back through the original trampoline.
    if (MH_CreateHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND,
                      (void*)Hook_DispatchScriptSoundCommand,
                      (void**)&original_dispatch_script_sound) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Hooks: Failed to hook DispatchScriptSoundCommand");
        return false;
    }
    SoundRollback::SetOriginalDispatcher(original_dispatch_script_sound);

    // C3.5 — vs_round_function detour. Emits ROUND_START / ROUND_END
    // SessionEvents at the round-state-machine substate edges (host only;
    // FM95 builds compile to a no-op). Best-effort install: if the hook
    // fails we keep going so the rest of the engine works (round events
    // are diagnostic, not load-bearing).
    if (!RoundEvents_Install()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: RoundEvents_Install failed — round events will be missing "
            "from session_events / .fm2krep round_offsets");
    }

    // CSS auto-lock-and-confirm — installs idle, only activates when
    // SpectatorNode's MATCH_START apply path arms it for offline replay
    // playback (FM2K_REPLAY_FILE set). Live-spectator paths walk CSS via the
    // host's full input stream and don't need this. FM95 builds compile to a
    // no-op (its CSS state machine is structured differently — separate
    // hand-off).
    if (!CssAutoConfirm_Install()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: CssAutoConfirm_Install failed — offline replay will fall "
            "back to natural CSS traversal (likely picks wrong chars)");
    }
    // Test-harness CSS auto-confirm: arm the same auto-confirm path used
    // by .fm2krep replay, but driven by env var so a 2-instance loopback
    // netplay test can advance through CSS without keyboard input. Both
    // peers must see the SAME values for gekko CSS sync to land on the
    // same chars/stage. Format: FM2K_TEST_AUTO_CSS=p1char,p1color,p2char,p2color,stage
    // (decimal bytes). Default chars/colors/stage = 0 (= first option).
    // Test-harness FM2K_TEST_AUTO_CSS now ONLY enables the gekko input
    // pulse in Netplay_ProcessCSSInputPhase — no direct CssAutoConfirm
    // memory pinning. CssAutoConfirm was designed for single-instance
    // offline replay; in 2-peer netplay it produced asymmetric CSS-state
    // transitions (P1 reached battle, P2 didn't) because gekko's
    // confirmed-input stream and CssAutoConfirm's direct-memory writes
    // race differently per peer. With pulse-only, both peers' engines
    // see the same gekko-delivered confirm rising edges and transition
    // CSS→battle through normal engine code, in lockstep.
    {
        const char* env = std::getenv("FM2K_TEST_AUTO_CSS");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: FM2K_TEST_AUTO_CSS env='%s' (pulse-only mode)",
            env ? env : "(null)");
    }

    // Per-game damage multiplier — only installs the hook if
    // FM2K_DAMAGE_MULT_PCT is set and != 100, so default users don't pay
    // the trampoline cost on every damage event. FM95 build is a no-op.
    if constexpr (FM2K::kIsFM2K) {
        if (!PerGamePatches_InstallDamageMultiplierHook()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: damage multiplier hook install failed — damage "
                "scaling won't apply this session");
        }
        // Option-A KOF retention: code-cave patch on the engine's
        // CSMK_PLAYER HP-init instruction so the engine never overwrites
        // the winner's slot with max_hp. Only installs when
        // FM2K_TEAM_KOF_RETENTION is enabled.
        if (!PerGamePatches_InstallKofHpInitPatch()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: KOF HP-init patch install failed — retention "
                "will fall back to vanilla (HP resets between rounds)");
        }
        // /F boot-to-battle prime: MinHook on the slot-0 boot dispatcher
        // (InitializeGameFromCommandLine @ 0x409a60). At entry, restamps
        // the kgt name into g_iniFile_nameOverride — counters hit_judge_-
        // set_function's earlier empty-default stomp. Only installs when
        // FM2K_BOOT_TO_BATTLE=1 (launcher dev checkbox).
        if (!PerGamePatches_InstallBootToBattleHook()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: boot-to-battle hook install failed — /F path "
                "will fail with 'GameSystem Open error[]'");
        }
        // Story-init AI hijack: MidHook char_state_machine's
        // g_game_mode_flag dispatch read so battle-phase calls see
        // flag=0 (1P arcade) — drives stage-script CPU AI init for
        // P2 in our hijacked 1P→VS-CSS modes. Idempotent; only
        // installs if option_mode_selector or one of the mode flags
        // was on at startup (PerGamePatches_ApplyRuntime ran first
        // in DllMain, so atomics are already populated).
        if (!PerGamePatches_InstallStoryInitHijack()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: story-init hijack install failed — VS CPU / "
                "Training will leave P2 as a non-AI standing dummy");
        }
    }

    // Hook timeGetTime (winmm.dll) — make the game's frame-skip pacing
    // deterministic across peers. See comment on Hook_timeGetTime for the
    // rationale. Resolve the real address dynamically so the hook works
    // regardless of IAT layout.
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (!winmm) winmm = LoadLibraryA("winmm.dll");
    if (winmm) {
        void* real_timeGetTime = (void*)GetProcAddress(winmm, "timeGetTime");
        if (real_timeGetTime) {
            if (MH_CreateHook(real_timeGetTime, (void*)Hook_timeGetTime,
                              (void**)&original_timeGetTime) != MH_OK ||
                MH_QueueEnableHook(real_timeGetTime) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook timeGetTime");
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: timeGetTime hooked for deterministic frame pacing");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: GetProcAddress(timeGetTime) failed");
        }
    }

    // CreateFileA/W share-mode override — force shared reads so two
    // instances launched from the same game folder don't get
    // ERROR_SHARING_VIOLATION on .player / .kgt opens.
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        void* real_CreateFileA = (void*)GetProcAddress(kernel32, "CreateFileA");
        void* real_CreateFileW = (void*)GetProcAddress(kernel32, "CreateFileW");
        if (real_CreateFileA) {
            if (MH_CreateHook(real_CreateFileA, (void*)Hook_CreateFileA,
                              (void**)&original_CreateFileA) != MH_OK ||
                MH_QueueEnableHook(real_CreateFileA) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook CreateFileA");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: CreateFileA hooked (relaxed share mode)");
            }
        }
        if (real_CreateFileW) {
            if (MH_CreateHook(real_CreateFileW, (void*)Hook_CreateFileW,
                              (void**)&original_CreateFileW) != MH_OK ||
                MH_QueueEnableHook(real_CreateFileW) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook CreateFileW");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: CreateFileW hooked (relaxed share mode)");
            }
        }

        // ── Fast .player load: collapse syscall storm ──
        // Gated on FM2K_FAST_PLAYER_LOAD env var. Hooks always install (so
        // the toggle can be flipped without re-injecting); the per-hook
        // fast-paths early-out when the flag is false.
        if (const char* env_fpl = std::getenv("FM2K_FAST_PLAYER_LOAD")) {
            g_fast_player_load = (env_fpl[0] == '1');
        }
        // FM2K_FPK_VFS: serve .player/.stage/.demo from a sibling "<path>.fpk"
        // (inflated via fpk_reconstruct) instead of the original on disk. Shares
        // the VFile serve-hooks below; the register + serve fast-paths test
        // VfsActive() = (g_fast_player_load || g_fpk_vfs).
        if (const char* env_fpk = std::getenv("FM2K_FPK_VFS")) {
            g_fpk_vfs = (env_fpk[0] == '1');
        }
        // FPK VFS serves assets by inflating each ".fpk" to a sibling
        // "<path>.inflated" temp on DISK once and handing the engine that file --
        // no ~100MB buffers in our 2GB heap. Start the low-priority worker that
        // pre-builds the rest of the roster's temps in the background.
        if (g_fpk_vfs && !g_prefetch_started) {
            g_prefetch_started = true;
            std::thread(PrefetchWorker).detach();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: FPK VFS (temp-redirect) + prefetch worker started");
        }
        // QPC frequency for the slurp timing log; if it ever fails, we
        // disable the timing path (load still works, just no instrumentation).
        LARGE_INTEGER freq{};
        if (::QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
            g_qpc_freq = freq.QuadPart;
        }
        void* real_ReadFile         = (void*)GetProcAddress(kernel32, "ReadFile");
        void* real_SetFilePointer   = (void*)GetProcAddress(kernel32, "SetFilePointer");
        void* real_SetFilePointerEx = (void*)GetProcAddress(kernel32, "SetFilePointerEx");
        void* real_CloseHandle      = (void*)GetProcAddress(kernel32, "CloseHandle");

        if (real_ReadFile) {
            if (MH_CreateHook(real_ReadFile, (void*)Hook_ReadFile,
                              (void**)&original_ReadFile) != MH_OK ||
                MH_QueueEnableHook(real_ReadFile) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook ReadFile");
            }
        }
        if (real_SetFilePointer) {
            if (MH_CreateHook(real_SetFilePointer, (void*)Hook_SetFilePointer,
                              (void**)&original_SetFilePointer) != MH_OK ||
                MH_QueueEnableHook(real_SetFilePointer) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook SetFilePointer");
            }
        }
        if (real_SetFilePointerEx) {
            if (MH_CreateHook(real_SetFilePointerEx, (void*)Hook_SetFilePointerEx,
                              (void**)&original_SetFilePointerEx) != MH_OK ||
                MH_QueueEnableHook(real_SetFilePointerEx) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook SetFilePointerEx");
            }
        }
        if (real_CloseHandle) {
            if (MH_CreateHook(real_CloseHandle, (void*)Hook_CloseHandle,
                              (void**)&original_CloseHandle) != MH_OK ||
                MH_QueueEnableHook(real_CloseHandle) != MH_OK) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hooks: Failed to hook CloseHandle");
            }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: FM2K_FAST_PLAYER_LOAD=%s FM2K_FPK_VFS=%s "
                    "(ReadFile/SetFP/CloseHandle hooked)",
                    g_fast_player_load ? "1 (active)" : "0 (passthrough)",
                    g_fpk_vfs ? "1 (active)" : "0 (off)");
    }

    // CSS fast-sound: defer character DirectSound buffer creation until a sound
    // first plays (FM2K_FPK_CSS_FASTSOUND=1), killing the ~150ms per-hover dip
    // from rebuilding ~80 sound buffers on every CSS cursor move. Queues its
    // hooks here; applied in the batch below. See css_fastsound.cpp.
    CssFastSound_Install();

    // Single thread-freeze for every hook queued during this function and by
    // InstallLocaleSpoof/RoundEvents_Install/CssAutoConfirm_Install. One call
    // beats ~12 individual MH_EnableHook(target) freezes (~80–120ms each).
    MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Hooks: MH_ApplyQueued failed: %d", (int)apply);
        return false;
    }

    // CSM dispatch-loop diagnostic. Off by default; FM2K_CSM_DIAG=1 installs
    // a SafetyHook MidHook at 0x412564 that dumps obj state per call. Used
    // by the replay-self-test bisect to find the char_dynamic field that
    // differs between host and replay at the script-divergence frame.
    extern void Hook_InstallCsmDiag();
    Hook_InstallCsmDiag();

    // Camera-operand diagnostic (task #34). FM2K_CAM_DIAG=1 installs a
    // MidHook at camera_manager's battle path logging the camera formula's
    // exact inputs per tick (record-vs-replay diff localizes the drift).
    extern void Hook_InstallCamDiag();
    Hook_InstallCamDiag();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: All hooks installed successfully");
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Shutdown complete");
}
