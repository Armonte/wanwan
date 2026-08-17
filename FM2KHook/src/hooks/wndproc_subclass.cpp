#include "wndproc_subclass.h"
#include "../netplay/control_channel.h"
#include "../netplay/netplay.h"   // Netplay_RequestRunaheadToggle (F8 hotkey)
#include "../core/globals.h"   // FM2K::kIsFM2K / kIsFM95, g_spectator_*
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace FM2KWndProc {

// Win32 timer ID for our modal-loop pump. Picked to be high so we
// don't collide with anything FM2K's own WindowProc might register.
constexpr UINT_PTR kModalPumpTimerId = 0x46324B01;  // 'F2K\x01'

// Modal pump cadence in ms. 5ms is well under PING_TIMEOUT_MS, so a
// drag of any realistic length stays inside our recv-deadline budget,
// and we still drain queued UDP packets fast enough that GekkoNet's
// own peer-timeout (5s) never trips during the drag.
constexpr UINT     kModalPumpInterval = 5;

static HWND    g_hwnd            = nullptr;
static WNDPROC g_orig_wndproc    = nullptr;
static bool    g_in_modal_loop   = false;

// Original window title captured at install time. We append " [FF]" when
// the user toggles fast-forward via F12 in spectator mode and restore the
// plain title on toggle off. Stored as wide so JP titles round-trip
// without loss across the FF toggle (the locale-spoof wrapper promoted
// the window to Unicode at create time -- ANSI snapshot here would
// hard-fold JP chars to '?' via the system codepage).
static wchar_t g_original_title[256] = {};

static void UpdateSpectatorTitle() {
    if (!g_hwnd || !g_spectator_mode) return;
    wchar_t buf[320] = {};
    if (g_spectator_ff_user) {
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%ls [FF]", g_original_title);
    } else {
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%ls", g_original_title);
    }
    SetWindowTextW(g_hwnd, buf);
}

// cnc-ddraw's own fullscreen toggle (util_toggle_fullscreen @ 2DFMD.dll+0x8770).
//
// For F4 this is the PRIMARY path: the managed ddraw.ini deliberately leaves
// keytogglefullscreen2 unbound (0x00) because cnc-ddraw's single-key matcher
// has no Alt check -- binding F4 there made its WH_KEYBOARD hook swallow
// Alt+F4 (the close chord) before dispatch. With nothing bound, every F4
// reaches our WM_KEYDOWN handler below, and Alt+F4 flows untouched through
// the normal close chain (cnc-ddraw's wndproc even fast-paths WM_SYSKEYDOWN
// VK_F4 to DefWindowProc).
//
// For Alt+Enter this is a FALLBACK: cnc-ddraw's hook owns it, but on
// JP-titled games that hook goes dead (its window/hook attach races our
// locale window handling) -- cnc-ddraw *swallows* the key whenever it DOES
// handle it, so any Alt+Enter that reaches this subclass means cnc-ddraw did
// not, and we call the toggle directly. cnc-ddraw's own hook runs on this
// same window thread, so the call context is identical and safe; the toggle
// self-guards if ddraw isn't ready (SrcWidth==0). Offset is valid for the
// PINNED cnc-ddraw build (kPinnedTag v7.1.0.0 in FM2K_CncDDraw.cpp);
// signature-guarded so a different build is skipped, not mis-called.
static void TriggerCncDdrawFullscreenToggle(HWND our_hwnd) {
    HMODULE mod = GetModuleHandleA("2DFMD.dll");
    if (!mod) return;
    // Prologue of util_toggle_fullscreen: push ebp; mov ebp,esp; sub esp,8;
    // cmp dword ptr [g_in_toggle],0 -- first 8 bytes carry no relocated address.
    static const unsigned char kSig[8] =
        {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x83, 0x3D};
    auto* base = reinterpret_cast<unsigned char*>(mod);
    auto* fn   = base + 0x8770;
    if (std::memcmp(fn, kSig, sizeof(kSig)) != 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDdrawToggle: signature mismatch at 2DFMD.dll+0x8770 -- "
                "cnc-ddraw build changed; F4 fullscreen fallback disabled");
        }
        return;
    }
    // Diagnostic: cnc-ddraw's toggle self-guards on `g_in_toggle(0x5F064) ||
    // !SrcWidth(0x5EDC4) || menu`. If hWndTo(0x5F02C) isn't our window or
    // SrcWidth is 0, cnc-ddraw's ddraw state was never bound to the visible
    // window -- the real root of the dead JP-game hotkey. Log it once so we see
    // exactly why the toggle no-ops. (Offsets are RVAs from the 2DFMD IDB.)
    static int s_diag = 0;
    if (s_diag < 3) {
        ++s_diag;
        uint32_t f064   = *reinterpret_cast<uint32_t*>(base + 0x5F064);
        uint32_t src_w  = *reinterpret_cast<uint32_t*>(base + 0x5EDC4);
        HWND     hwndTo = *reinterpret_cast<HWND*>(base + 0x5F02C);
        uint32_t win    = *reinterpret_cast<uint32_t*>(base + 0x594CC); // dword_100594CC (windowed flag)
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CncDdrawToggle: pre-call g_in_toggle=%u SrcWidth=%u windowed=%u "
            "hWndTo=%p ourHwnd=%p (bails if in_toggle!=0 or SrcWidth==0)",
            f064, src_w, win, (void*)hwndTo, (void*)our_hwnd);
    }
    reinterpret_cast<void (*)()>(fn)();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CncDdrawToggle: invoked cnc-ddraw fullscreen toggle (F4/Alt+Enter fallback)");
}

static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg,
                                     WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        // -- Alt / F10 menu mode suppression --------------------------
        // FM2K has no application menu, so DefWindowProc's default
        // "menu active" mode is pure friction: tapping Alt freezes
        // input until the user presses Esc, F10 does the same.
        // Swallow at the source.
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_SYSKEYDOWN:
            // Alt+Enter fullscreen -- fall back to cnc-ddraw's own toggle when
            // its hotkey hook didn't fire (JP-titled games). Non-repeat only.
            if (wparam == VK_RETURN && (lparam & 0x40000000) == 0) {
                TriggerCncDdrawFullscreenToggle(hwnd);
                return 0;
            }
            if (wparam == VK_MENU || wparam == VK_F10) return 0;
            break;
        case WM_SYSKEYUP:
            if (wparam == VK_MENU || wparam == VK_F10) return 0;
            break;

        // -- F12 fast-forward toggle (spectator mode only) ----------------
        // Spectators sit at whatever delay the network gave them; F12
        // requests one-burst catchup-to-live. Toggle off to drop back to
        // 1x. Title-bar shows "[FF]" while active. Swallowed before
        // game-input layer so it doesn't accidentally drive any FM2K
        // CSS/title menu (none of our games bind F12, but we're polite).
        //
        // F8: runahead on/off toggle (live, mid-match). Queues a
        // request; the actual gekko_set_runahead call happens on the
        // trampoline thread at the top of the next battle tick via
        // Netplay_PollRunaheadToggle so we never touch GekkoNet state
        // from this WindowProc thread. user_pref is set by the
        // FM2K_RUNAHEAD env var (default 6); F8 flips between 0 and
        // that value. Swallow so the game's input layer never sees
        // the keystroke.
        case WM_KEYDOWN:
            // F4 fullscreen -- we OWN this hotkey (the managed ddraw.ini
            // leaves cnc-ddraw's single-key toggle unbound so Alt+F4 stays
            // a close chord; see TriggerCncDdrawFullscreenToggle's comment).
            // Alt+F4 never lands here -- with Alt held the key arrives as
            // WM_SYSKEYDOWN, which we pass through to the close chain.
            // Non-repeat only.
            if (wparam == VK_F4 && (lparam & 0x40000000) == 0) {
                TriggerCncDdrawFullscreenToggle(hwnd);
                return 0;
            }
            if (wparam == VK_F12 && g_spectator_mode) {
                g_spectator_ff_user = !g_spectator_ff_user;
                UpdateSpectatorTitle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator: F12 fast-forward %s",
                            g_spectator_ff_user ? "ON" : "OFF");
                return 0;
            }
            if (wparam == VK_F8) {
                Netplay_RequestRunaheadToggle();
                return 0;
            }
            break;
        case WM_KEYUP:
            if (wparam == VK_F12 && g_spectator_mode) return 0;
            if (wparam == VK_F8) return 0;
            break;
        case WM_SYSCHAR:
            // Suppresses the "ding" Alt+letter would otherwise emit.
            return 0;

        // -- Modal title-drag pump ------------------------------------
        // DefWindowProc's SC_MOVE / SC_SIZE modal loop blocks
        // DispatchMessage on the main thread. The MM-timer in
        // control_channel.cpp keeps OUR pings flowing outbound, but
        // the receive side and timeout housekeeping are still tied
        // to ControlChannel_Poll(). Driving Poll() from a fast
        // WM_TIMER inside the modal loop is the canonical Win32
        // recipe for "keep the network alive while the user drags
        // the title bar". Same trick covers WM_INITMENU / system-
        // menu open, since those also fire ENTERSIZEMOVE-equivalent
        // modal loops via WM_ENTERMENULOOP -- handled below.
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            if (!g_in_modal_loop) {
                g_in_modal_loop = true;
                SetTimer(hwnd, kModalPumpTimerId, kModalPumpInterval, nullptr);
            }
            break;
        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            if (g_in_modal_loop) {
                KillTimer(hwnd, kModalPumpTimerId);
                g_in_modal_loop = false;
            }
            break;
        case WM_TIMER:
            if (wparam == kModalPumpTimerId) {
                // Drive networking only -- no game-sim tick from here.
                // The sim itself naturally pauses while the user drags
                // (FM2K's render is single-threaded and the trampoline
                // hasn't been audited for reentrance from WM_TIMER).
                // What matters for the disconnect bug is the network
                // I/O, which is reentrant-safe.
                ControlChannel_Poll();
                return 0;
            }
            break;

        case WM_NCDESTROY:
            // Belt-and-suspenders timer cleanup; subclass uninstall
            // happens via Uninstall() from the trampoline shutdown
            // path, but if the window is destroyed without going
            // through that we still want the timer dead.
            KillTimer(hwnd, kModalPumpTimerId);
            g_in_modal_loop = false;
            break;

        // -- Menu Item 2320 redirect ---------------------------------
        // FM2K's window menu has a "Full Screen" entry whose handler
        // (HandleMainMenuCommand case 2320 @ 0x4177ae) toggles
        // g_graphics_mode and reinits DirectDraw -- same fight with
        // cnc-ddraw as the F4 / Alt+Enter keyboard paths. When
        // cnc-ddraw is loaded (i.e. our IAT redirect succeeded and
        // 2DFMD.dll is present in the process), translate the menu
        // command into a synthetic VK_F4 keypress -- dispatched by the
        // message pump into our own WM_KEYDOWN VK_F4 handler above,
        // which drives cnc-ddraw's util_toggle_fullscreen directly.
        // PostMessage rather than SendInput because we want this to
        // look like a normal queued keystroke.
        //
        // wparam high word holds the source flag (0=menu, 1=accelerator,
        // > 0 if from a control); we only redirect menu invocations.
        case WM_COMMAND: {
            const WORD id   = LOWORD(wparam);
            const WORD from = HIWORD(wparam);
            if (id == 2320 && from == 0 && lparam == 0 &&
                GetModuleHandleA("2DFMD.dll") != nullptr)
            {
                PostMessageA(hwnd, WM_KEYDOWN, VK_F4, 0);
                PostMessageA(hwnd, WM_KEYUP,   VK_F4, 0xC0000000);
                return 0;
            }
            break;
        }
    }
    // CallWindowProcW handles W→A conversion if g_orig_wndproc was
    // registered as ANSI; the locale-spoof wrapper sits between us and
    // any actual ANSI proc so text-content messages are intercepted
    // there before reaching this layer.
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

// Find the KGT2KGAME window owned by THIS process. FindWindowA is
// process-blind and returns the first match globally -- when two
// instances are running on the same PC, instance B would otherwise
// pick up instance A's HWND and SetWindowLongPtr would fail with
// ERROR_ACCESS_DENIED (cross-process subclass not allowed).
struct FindOwnWindowCtx {
    DWORD pid;
    HWND  result;
};
static BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindOwnWindowCtx*>(lparam);
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != ctx->pid) return TRUE;
    char cls[32] = {0};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
    // FM2K uses "KGT2KGAME"; FM95/CPW uses "KGT95GAME". Same hook DLL
    // source builds twice (FM2KHook.dll + FM95Hook.dll); the engine
    // constants pick the right one at compile time.
    const char* expect_cls = FM2K::kIsFM95 ? "KGT95GAME" : "KGT2KGAME";
    if (lstrcmpA(cls, expect_cls) != 0) return TRUE;
    ctx->result = hwnd;
    return FALSE;  // stop enumeration
}

void TryInstall() {
    if (g_hwnd != nullptr) return;       // already installed
    FindOwnWindowCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindOwnWindowProc, reinterpret_cast<LPARAM>(&ctx));
    HWND hwnd = ctx.result;
    if (hwnd == nullptr) return;          // window not yet up; try again later
    // SetWindowLongPtrW keeps the window flagged Unicode (the locale-spoof
    // wrapper promoted it at creation). Switching to A here would flip
    // the flag back, which would re-introduce the W→A bridge that mangles
    // JP titles via CP_ACP.
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc));
    if (prev == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "WndProcSubclass: SetWindowLongPtr failed (err=%lu)",
                    GetLastError());
        return;
    }
    g_hwnd         = hwnd;
    g_orig_wndproc = prev;

    // Snapshot the original title (wide) so the F12 spectator FF toggle
    // can append/strip "[FF]" without losing JP characters.
    GetWindowTextW(hwnd, g_original_title,
                   sizeof(g_original_title) / sizeof(g_original_title[0]) - 1);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "WndProcSubclass: installed on hwnd=%p prev_wndproc=%p "
                "(Alt-mute + modal-pump)",
                hwnd, (void*)prev);
}

HWND GetHwnd() { return g_hwnd; }

void Uninstall() {
    if (g_hwnd == nullptr) return;
    if (g_in_modal_loop) {
        KillTimer(g_hwnd, kModalPumpTimerId);
        g_in_modal_loop = false;
    }
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_hwnd         = nullptr;
    g_orig_wndproc = nullptr;
}

}  // namespace FM2KWndProc
