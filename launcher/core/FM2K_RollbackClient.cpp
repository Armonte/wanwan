#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include "SDL3/SDL.h"
#include <SDL3_image/SDL_image.h>
#include "app_icon_data.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "game_discovery.h"  // game scan/cache/sniff + async discovery (moved out of this file)
#include "launcher_cli.h"  // CLI parse + headless probes + launch-mode application
#include "launcher_log.h"  // always-on launcher.log disk sink + crash breadcrumb
#include "FM2KHook/src/util/pii_scrub.h"  // fm2k::pii::Init -- redact before first log line
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/ui/input_binder.h"  // RefreshGamepads() on SDL hot-plug events
#include "FM2KHook/src/netplay/spec_relay_queue.h"  // hub-relay outbound drain (Phase 2c)
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include "LocalSession.h"
#include "OnlineSession.h"
#include "FM2K_PortMapper.h"  // --upnp-test self-contained router validation

#include <chrono>
#include <string>
#include <cstring>
#include <cstdlib>  // std::getenv for FM2K_FULL_CRCS perf-run override
#include <optional>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <locale>    // std::locale::global -- make std::filesystem non-ASCII-path safe
#include <codecvt>   // std::codecvt_utf8_utf16 (deprecated but present in libstdc++)
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <system_error>


// FM2K Input Structure (11-bit input mask)
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

// Global variables
SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
bool running = false;

// FM2K Process Management (Launcher Model)
HANDLE fm2k_process = nullptr;
DWORD fm2k_process_id = 0;
PROCESS_INFORMATION fm2k_process_info = {};
bool game_launched = false;

// Timing (100 FPS for FM2K)
using micro = std::chrono::microseconds;
using fm2k_frame = std::chrono::duration<unsigned int, std::ratio<1, 100>>;  // 100 FPS
using gclock = std::chrono::steady_clock;

// Global launcher instance (since callbacks need global access)
static std::unique_ptr<FM2KLauncher> g_launcher = nullptr;


// Timestamp (ns) of the last input/window event, stamped in SDL_AppEvent.
// SDL_AppIterate uses it to keep painting at full rate for ~0.5s after the
// last input (so click/hover transitions finish), then fall to the idle path.
// Seeded to "now" at the end of SDL_AppInit (NOT left at 0 -- a zero here makes
// the very first iteration look ~100s idle and boots straight into the parked
// path before the first event arrives).
static Uint64 g_last_input_activity_ns = 0;

// Repaint-needed flag for the event-driven idle path. Set true by any SDL
// event (input/focus/window/gamepad/async-discovery completion) and at
// startup; consumed by SDL_AppIterate. The key difference from the old
// "lower fps" throttle: when this is clear AND the user is idle, we SKIP the
// whole ImGui frame (no NewFrame/Render/Present) and leave the last presented
// image on the SDL_Renderer backbuffer -- ~zero CPU/GPU instead of a full UI
// rebuild every tick. That is what stops the launcher pegging weak CPUs and
// stealing cycles from the game. A periodic safety-net repaint (see
// SDL_AppIterate) bounds how stale non-input-driven UI (hub lobby, relay
// counters) can get. Plain bool: SDL_AppEvent and SDL_AppIterate run on the
// same thread, and the discovery thread wakes us via SDL_PushEvent.
static bool g_ui_dirty = true;

// SDL Callback Implementation
extern "C" {

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    // Hide the console window for release. The launcher EXE is currently
    // console-subsystem (so it gets a console handle inherited by the
    // game subprocesses we spawn) but the user-facing window is the SDL
    // ImGui app -- the black console alongside is purely visual noise +
    // the synchronous WriteFile pacing on a Windows console actively
    // lags printf-heavy code paths.
    //
    // Strategy: keep the console subsystem (so child processes can
    // inherit a usable stdout/stderr if needed) but hide the WINDOW
    // immediately. Then redirect our own stdout/stderr to NUL so any
    // legacy printf still goes somewhere safe instead of trying to
    // write to a hidden console (which can wedge with SetConsoleMode
    // edge cases). FM2K_DEV_MODE=1 keeps the console visible for
    // diagnostic runs.
    {
        const char* dev = std::getenv("FM2K_DEV_MODE");
        const bool show_console = (dev && dev[0] == '1' && dev[1] == '\0');
        if (!show_console) {
            if (HWND con = GetConsoleWindow()) {
                ShowWindow(con, SW_HIDE);
            }
            // Detach our own stdio from the (now hidden) console. Child
            // processes still have their own handles via inheritance --
            // this only affects the launcher's printf/cout. Redirect to
            // NUL so any leftover prints don't block.
            FILE* dummy = nullptr;
            freopen_s(&dummy, "NUL", "w", stdout);
            freopen_s(&dummy, "NUL", "w", stderr);
        } else {
            SetConsoleTitleA("FM2K Rollback Launcher (DEV)");
        }
    }

    // Always-on launcher-side disk log next to the EXE (launcher.log). Opened
    // HERE -- before the window/renderer/launcher object -- so a crash anywhere
    // in startup (the "opens, shows the window, then closes" reports) still
    // leaves a file to ask testers for. pii::Init() first so even these earliest
    // lines are redacted. SDL_SetLogOutputFunction routes every SDL_Log to the
    // file; LauncherUI::Init later chains its scrubbed in-UI logger to this sink
    // (SDL_GetLogOutputFunction captures it as the "original"). The crash filter
    // records the faulting module+offset into the same file.
    fm2k::pii::Init();
    fm2k::launcher_log::Init();
    fm2k::launcher_log::InstallCrashHandler();
    SDL_SetLogOutputFunction(fm2k::launcher_log::SdlLogOutput, nullptr);

    // Make std::filesystem locale-robust for non-ASCII paths. libstdc++'s
    // path narrow<->wide conversion uses the GLOBAL locale's codecvt; the
    // default "C" locale THROWS std::filesystem_error ("Cannot convert
    // character sequence: Illegal byte sequence") the moment a path carries a
    // byte >= 0x80 -- i.e. ANY install/profile path under a non-ASCII Windows
    // username (accented or CJK). That uncaught throw on a worker thread is
    // what silently closed the window for those users. A UTF-8<->UTF-16 codecvt
    // makes wide->narrow (path::string()) total and narrow->wide interpret
    // bytes as UTF-8, matching fm2k::utf8path. Guarded: never fatal.
    try {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        std::locale::global(std::locale(std::locale::classic(),
                                        new std::codecvt_utf8_utf16<wchar_t>()));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    } catch (...) {
        // Keep the classic locale; the explicit fm2k::utf8path helpers still work.
    }

    // Rename the console window we get for the console-subsystem EXE.
    // Default title is the full EXE path or, on some launches, the
    // MinGW-w64 toolchain string ("POSIX WinThreads") inherited from
    // the runtime. Override with something meaningful (only visible
    // when FM2K_DEV_MODE=1 keeps the console window up).
    SetConsoleTitleA("FM2K Rollback Launcher");

    // Pin the AppUserModelID for this process. Without an explicit AUMID
    // Windows derives one from the EXE path and caches the displayed
    // name from whichever VERSIONINFO it sees first -- and once cached,
    // toasts (Action Center), the taskbar grouping, and the "right-click
    // → app name" surface keep showing the cached entry even after we
    // ship a fixed VERSIONINFO. Setting our own AUMID gives Windows a
    // stable identity it associates with our PE's actual FileDescription
    // ("FM2K Rollback Launcher") instead of the libwinpthread-derived
    // legacy one. Loaded dynamically because the symbol is in
    // shobjidl_core.h which not every MinGW SDK ships.
    {
        using SetAumidFn = HRESULT (WINAPI*)(PCWSTR);
        if (HMODULE sh = GetModuleHandleW(L"shell32.dll")) {
            auto SetAumid = reinterpret_cast<SetAumidFn>(
                GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID"));
            if (SetAumid) {
                SetAumid(L"FM2K.Rollback.Launcher");
            }
        }
    }

    std::cout << "=== FM2K Rollback Launcher ===\n";
    std::cout << "Initializing with SDL callbacks...\n\n";

    // Parse the command line into args (see launcher_cli.cpp).
    LauncherCliArgs args;
    if (SDL_AppResult parse_r = LauncherCli_ParseArgs(argc, argv, args);
        parse_r != SDL_APP_CONTINUE) {
        return parse_r;
    }
    // Aliases for the inline --upnp-test / --nat-test probes below.
    const bool         upnp_test_cli  = args.upnp_test_cli;
    const uint16_t     upnp_test_port = args.upnp_test_port;
    const bool         nat_test_cli   = args.nat_test_cli;
    const std::string& nat_test_host  = args.nat_test_host;
    const uint16_t     nat_test_port  = args.nat_test_port;

    // --upnp-test: self-contained UPnP probe, runs before any launcher /
    // SDL window / game setup and exits the process. PortMapper uses
    // SDL_Log* internally which works against SDL's default log output
    // without SDL_Init, so we get the discovery/IGD/error lines for free.
    if (upnp_test_cli) {
        std::cout << "[upnp-test] mapping UDP port " << upnp_test_port
                  << " via UPnP (up to ~6s for SSDP + map)...\n";
        // Winsock isn't up yet in this early-exit path (SDL_Init, which
        // calls WSAStartup, hasn't run). miniupnpc creates raw sockets for
        // the SSDP discover, so initialize Winsock ourselves first --
        // otherwise upnpDiscover fails with WSANOTINITIALISED (10093) and
        // we'd report a false NoIgd. In normal launcher operation the
        // mapper runs at the hub Connected event, long after WSAStartup.
        WSADATA wsa{};
        const bool wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        if (!wsa_ok) {
            std::cout << "[upnp-test] WARNING: WSAStartup failed -- "
                         "discovery may not work\n";
        }
        fm2k::PortMapper mapper;
        mapper.StartAsync(upnp_test_port);
        // Poll the status until it leaves Discovering or the ~6s budget
        // elapses (SSDP is a 2s budget + description fetch + AddPortMapping
        // round-trips; 6s comfortably covers a responsive home router).
        fm2k::PortMapper::Status st;
        for (int waited_ms = 0; waited_ms < 6000; waited_ms += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            st = mapper.Snapshot();
            if (st.state != fm2k::PortMapper::State::Discovering &&
                st.state != fm2k::PortMapper::State::Idle) {
                break;
            }
        }
        st = mapper.Snapshot();
        auto state_name = [](fm2k::PortMapper::State s) -> const char* {
            switch (s) {
                case fm2k::PortMapper::State::Idle:        return "Idle";
                case fm2k::PortMapper::State::Discovering: return "Discovering";
                case fm2k::PortMapper::State::Mapped:      return "Mapped";
                case fm2k::PortMapper::State::NoIgd:       return "NoIgd";
                case fm2k::PortMapper::State::Failed:      return "Failed";
                case fm2k::PortMapper::State::Cgnat:       return "Cgnat";
            }
            return "?";
        };
        std::cout << "[upnp-test] result:\n"
                  << "  state        = " << state_name(st.state)   << "\n"
                  << "  backend      = " << st.backend             << "\n"
                  << "  ext_ip       = " << st.ext_ip              << "\n"
                  << "  ext_udp_port = " << st.ext_udp_port        << "\n"
                  << "  igd          = " << st.igd_desc            << "\n";
        mapper.Stop();
        if (wsa_ok) WSACleanup();
        std::cout << "[upnp-test] done (mapping torn down).\n";
        return SDL_APP_SUCCESS;
    }

    // --nat-test: self-contained NAT classification probe. Runs the exact
    // dual STUN probe the lobby uses (fm2k::LauncherStunClassify) against the
    // given hub, prints the verdict + reflexive ports, and exits. This is the
    // harness for the Phase 2a plumbing: point it at a local hub.py
    // (--host 127.0.0.1, which binds 7711-7714) and expect "cone" with
    // port_a == port_b. LauncherStunClassify calls WSAStartup internally, so
    // no Winsock bring-up is needed here (mirrors the launcher path, where it
    // runs at the hub Connected event with Winsock already up).
    if (nat_test_cli) {
        if (nat_test_host.empty()) {
            std::cout << "[nat-test] usage: --nat-test <hub_host> [local_port]\n";
            return SDL_APP_FAILURE;
        }
        // A synthetic user id so the probe doesn't early-out on empty; the
        // classification reflector (:7714) ignores it entirely and the
        // primary (:7711) just records a throwaway mapping for an unknown
        // user (harmless -- no such User exists on a fresh local hub).
        const std::string test_uid = "nat-test-harness";
        std::cout << "[nat-test] probing hub '" << nat_test_host
                  << "' (UDP-STUN :7711, classify :7714) from local port "
                  << nat_test_port << " ...\n";
        fm2k::NatClassifyResult r = fm2k::LauncherStunClassify(
            nat_test_port, nat_test_host, 7711, test_uid);
        std::cout << "[nat-test] result:\n"
                  << "  nat_type = " << r.nat_type << "\n"
                  << "  port_a   = " << r.port_a   << "  (reflected ext port from :7711)\n"
                  << "  port_b   = " << r.port_b   << "  (reflected ext port from :7714)\n";
        std::cout << "[nat-test] done.\n";
        return SDL_APP_SUCCESS;
    }

    // Create launcher instance
    g_launcher = std::make_unique<FM2KLauncher>();

    if (!g_launcher->Initialize()) {
        std::cerr << "Failed to initialize launcher\n";
        return SDL_APP_FAILURE;
    }

    // Apply the CLI launch mode (replay / stress / offline / direct). These
    // fall through to the headless main loop on success; FAILURE aborts.
    if (SDL_AppResult mode_r = LauncherCli_ApplyLaunchMode(g_launcher.get(), args);
        mode_r != SDL_APP_CONTINUE) {
        return mode_r;
    }

    // Store launcher in appstate for other callbacks
    *appstate = g_launcher.get();

    // Seed the activity clock to "now" so the first SDL_AppIterate doesn't see
    // a ~100s idle gap (g_last_input_activity_ns left at 0) and park the UI
    // before the first event. Start dirty so the first frame paints.
    g_last_input_activity_ns = SDL_GetTicksNS();
    g_ui_dirty = true;

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }

    const Uint64 loop_start_ns = SDL_GetTicksNS();

    // Calculate delta time
    static auto last_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    float delta_time = std::chrono::duration<float>(current_time - last_time).count();
    last_time = current_time;

    // IPC-critical slice: drain DLL events, forward TCP-STUN / session-kind,
    // pump the spec hub-relay ring, and watch for game exit. This is the live
    // spectator data plane plus the game-termination watchdog, so it MUST run
    // every tick regardless of visibility/idle/render-skip. Update() no longer
    // touches ImGui (NewFrame moved into Render()), so it's safe to call
    // without painting. It's cheap when no game is running (target pid == 0).
    launcher->Update(delta_time);

    // Window visibility + session state drive an event-driven repaint model.
    // Instead of redrawing the ENTIRE ImGui UI every frame and merely lowering
    // the rate when idle (the old throttle, which still pegged a Pentium Gold
    // 8505 because each frame did a full ImGui::Render()+present), we SKIP the
    // frame entirely when nothing changed and leave the last image on the
    // backbuffer. Tiers:
    //   MINIMIZED/HIDDEN     -> never paint, 10 Hz IPC wake
    //   BACKGROUND_GAME      -> game has foreground; never paint so we stop
    //                           stealing cycles from the game, 30 Hz IPC wake
    //   IDLE_VISIBLE         -> visible+focused but idle; paint only on dirty/
    //                           animation/safety-net, 60 Hz wake (low latency)
    //   UNFOCUSED_VISIBLE    -> visible but not focused; same, 30 Hz wake
    //   ACTIVE               -> input within last 0.5s; paint every wake, vsync-
    //                           paced (soft 60fps cap only when vsync is off)
    SDL_Window* w = launcher->GetWindow();
    const SDL_WindowFlags flags = w ? SDL_GetWindowFlags(w) : 0;
    const bool minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    const bool hidden    = (flags & SDL_WINDOW_HIDDEN) != 0;
    const bool unfocused = !(flags & SDL_WINDOW_INPUT_FOCUS);
    const bool in_game   = (launcher->GetState() == LauncherState::InGame);

    enum Tier { TIER_MINIMIZED, TIER_BACKGROUND_GAME, TIER_IDLE_VISIBLE,
                TIER_UNFOCUSED_VISIBLE, TIER_ACTIVE };

    static Uint64 last_render_ns = 0;
    const Uint64  kSafetyNetNs = 250'000'000ULL;  // 4 Hz forced repaint when visible
    const bool    recent_input =
        (loop_start_ns - g_last_input_activity_ns) < 500'000'000ULL;

    Tier   tier;
    Uint64 wake_target_ns;   // minimum loop period; 0 = let vsync pace
    bool   should_render;

    if (minimized || hidden) {
        tier = TIER_MINIMIZED;
        wake_target_ns = 100'000'000ULL;   // ~10 Hz; events still wake us
        should_render = false;
    } else if (in_game && unfocused) {
        // Game has the foreground. Don't paint per-frame -- that full-UI
        // rebuild every tick is what tanked the game on weak HW. Keep only a
        // slow 2 Hz safety-net repaint so any launcher stats the user is
        // watching (e.g. on a second monitor) stay vaguely live, at ~1/30th
        // the cost of the old 60fps path. IPC tick stays brisk for spec relay.
        tier = TIER_BACKGROUND_GAME;
        wake_target_ns = 33'333'333ULL;    // ~30 Hz IPC wake
        should_render = (last_render_ns == 0) ||
            (loop_start_ns - last_render_ns) >= 500'000'000ULL;
    } else {
        const bool safety_net = (last_render_ns == 0) ||
            (loop_start_ns - last_render_ns) >= kSafetyNetNs;
        const bool want = recent_input || g_ui_dirty ||
            launcher->UiWantsContinuousRedraw() || safety_net;
        g_ui_dirty = false;   // consume

        if (!unfocused && recent_input) {
            tier = TIER_ACTIVE;
            wake_target_ns = launcher->IsVsyncAvailable() ? 0ULL : 16'666'666ULL;
        } else if (unfocused) {
            tier = TIER_UNFOCUSED_VISIBLE;
            wake_target_ns = 33'333'333ULL;  // ~30 Hz wake; paint only when want
        } else {
            tier = TIER_IDLE_VISIBLE;
            wake_target_ns = 16'666'666ULL;  // ~60 Hz wake (latency); paint when want
        }
        should_render = want;
    }

    if (should_render) {
        launcher->Render();   // NewFrame + build + present (matched ImGui pair)
        last_render_ns = SDL_GetTicksNS();
    }

    // Observability: log tier transitions (rate-limited to changes) so the
    // ThinkPad / Pentium Gold logs prove the launcher actually parks rather
    // than spinning. Renderer name + vsync state are logged once at init.
    static Tier last_logged_tier = (Tier)-1;
    if (tier != last_logged_tier) {
        static const char* const kTierName[] = {
            "MINIMIZED", "BACKGROUND_GAME", "IDLE_VISIBLE",
            "UNFOCUSED_VISIBLE", "ACTIVE" };
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "UI tier -> %s (paint=%s)", kTierName[tier],
                    should_render ? "on" : "skip");
        last_logged_tier = tier;
    }

    // Pace: sleep so this iteration spans at least wake_target_ns. When we
    // painted with vsync available in ACTIVE, wake_target_ns is 0 and the
    // blocking present already paced us.
    if (wake_target_ns != 0) {
        const Uint64 elapsed = SDL_GetTicksNS() - loop_start_ns;
        if (elapsed < wake_target_ns) {
            SDL_DelayNS(wake_target_ns - elapsed);
        }
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }

    // Any event = user/window activity. Stamp the activity clock (keeps us in
    // the ACTIVE tier for ~0.5s) and mark the UI dirty so the event-driven
    // idle path repaints this iteration. Covers mouse/keyboard/focus/window,
    // gamepad hot-plug, and the async-discovery completion event (pushed via
    // SDL_PushEvent from the discovery thread), so all of them snap the UI
    // back to a fresh frame instantly.
    g_last_input_activity_ns = SDL_GetTicksNS();
    g_ui_dirty = true;

    // Gamepad hot-plug: refresh the binder's pad list so the input
    // bindings window (and the SOCD picker's "gamepad N" labels)
    // update without requiring the user to close & reopen the
    // launcher (Suicidal Muffin's bug report). The hook-side binder
    // gets the same treatment via a 1 s periodic refresh in
    // hooks.cpp + input.cpp, since events don't cross the process
    // boundary into the injected DLL's SDL context.
    if (event->type == SDL_EVENT_GAMEPAD_ADDED ||
        event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        FM2KInputBinder::RefreshGamepads();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SDL_AppEvent: gamepad %s -- binder refreshed",
            event->type == SDL_EVENT_GAMEPAD_ADDED ? "ADDED" : "REMOVED");
    }

    // Let launcher handle the event
    launcher->HandleEvent(event);

    // Check for quit
    if (event->type == SDL_EVENT_QUIT) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_QUIT: Quitting application");
        return SDL_APP_SUCCESS;
    }

    // Note: async discovery completion is handled inside FM2KLauncher::HandleEvent.

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate SDL_UNUSED, SDL_AppResult result SDL_UNUSED) {
    // Clean-exit marker: if launcher.log ends with this line, the process shut
    // down normally (window closed / SDL_APP_SUCCESS|FAILURE returned) -- NOT a
    // crash. Its absence (no [CRASH]/[TERMINATE]/[CRT] either) means the process
    // was killed from outside. This routes through SDLCustomLogOutput -> the
    // file sink, which is still installed at AppQuit entry.
    SDL_Log("SDL_AppQuit: clean shutdown (result=%d)", static_cast<int>(result));
    std::cout << "Shutting down FM2K launcher...\n";
    
    if (g_launcher) {
        // Perform shutdown
        g_launcher->Shutdown();
        g_launcher.reset();
    }

    std::cout << "LauncherUI shutdown\n";

    // Close launcher.log last: g_launcher->Shutdown() restores the SDL log sink
    // to ours (LauncherUI::Shutdown's SDL_SetLogOutputFunction), so teardown
    // lines still reach the file up to this point.
    fm2k::launcher_log::Shutdown();
}

} // extern "C"
