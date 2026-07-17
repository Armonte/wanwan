// hooks_vfs.cpp -- file-I/O hooks: CreateFile*/ReadFile/SetFilePointer*/CloseHandle
// share-mode override + FPK asset VFS + fast-player-load + FM95 LoadStageFile_alt.
// Split from hooks.cpp (functions + their MinHook install via InstallVfsHooks).
#include "hooks.h"
#include "hooks_internal.h"
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
#include "hooks_internal.h"

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

// VC runtimes route through CreateFileA but newer code paths may use W.
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static CreateFileA_t original_CreateFileA = nullptr;
static CreateFileW_t original_CreateFileW = nullptr;

static constexpr DWORD kRelaxedShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// FM95 (CPW) loads BMP/DIB graphics via the legacy Win16-compat file APIs
// _lopen (ddraw_load_palette_from_bmp @0x419CD0) and OpenFile
// (load_dib_file_or_resource @0x40D2E0). Those do NOT route through CreateFileA,
// so the CreateFileA/W relaxed-share fix missed them: two instances launched
// from the same folder hit a sharing violation on those opens -> the loader
// gets a bad HFILE -> reads garbage -> intermittent heap/AV crash during boot
// (the two-instance-only "player error"). Force OF_SHARE_DENY_NONE on both.
// The OF_SHARE_* field is bits 4-6 (mask 0x70); OF_SHARE_DENY_NONE = 0x40.
using lopen_t    = HFILE(WINAPI*)(LPCSTR, int);
using OpenFile_t = HFILE(WINAPI*)(LPCSTR, LPOFSTRUCT, UINT);
static lopen_t    original_lopen    = nullptr;
static OpenFile_t original_OpenFile = nullptr;
static constexpr int kOfShareMask     = 0x70;
static constexpr int kOfShareDenyNone = 0x40;  // OF_SHARE_DENY_NONE

static HFILE WINAPI Hook_lopen(LPCSTR path, int readWrite) {
    return original_lopen(path, (readWrite & ~kOfShareMask) | kOfShareDenyNone);
}
static HFILE WINAPI Hook_OpenFile(LPCSTR name, LPOFSTRUCT buf, UINT style) {
    return original_OpenFile(name, buf, (UINT)((style & ~kOfShareMask) | kOfShareDenyNone));
}

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

bool InstallVfsHooks() {
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

        // FM95-only: the legacy _lopen / OpenFile graphics loaders don't route
        // through CreateFileA, so force OF_SHARE_DENY_NONE on them too, or two
        // instances crash on a shared BMP/DIB (see Hook_lopen comment). FM2K
        // doesn't use these APIs, so leave its hook set untouched.
        if constexpr (FM2K::kIsFM95) {
            if (void* real_lopen = (void*)GetProcAddress(kernel32, "_lopen")) {
                if (MH_CreateHook(real_lopen, (void*)Hook_lopen,
                                  (void**)&original_lopen) == MH_OK &&
                    MH_QueueEnableHook(real_lopen) == MH_OK) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Hooks: _lopen hooked (FM95 relaxed share)");
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook _lopen");
                }
            }
            if (void* real_OpenFile = (void*)GetProcAddress(kernel32, "OpenFile")) {
                if (MH_CreateHook(real_OpenFile, (void*)Hook_OpenFile,
                                  (void**)&original_OpenFile) == MH_OK &&
                    MH_QueueEnableHook(real_OpenFile) == MH_OK) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Hooks: OpenFile hooked (FM95 relaxed share)");
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook OpenFile");
                }
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
    return true;
}

