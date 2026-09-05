// host.cpp -- the C++ half of the skeleton: SDL3 owns the window, the event
// loop and presentation; Slint owns the widget tree and draws into memory.
//
// This is Option A from the port plan, made concrete. The alternative
// (letting Slint's default backend own the window through winit) would put
// two window systems in one process and strand the SDL3 input and
// rendering the launcher already depends on. Here Slint never learns a
// window exists -- it is handed a pixel buffer and asked to fill it.
//
// Two properties worth stating because they are the reason for the shape:
// no GPU backend is involved, so this works over remote desktop and on the
// old hardware some of these games run on; and nothing in this file is
// Windows-specific, so the same arrangement is what a native Linux build
// would use.
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

extern "C" {
int  slint_ui_init(unsigned width, unsigned height);
void slint_ui_set_scale(unsigned phys_w, unsigned phys_h, float scale);
void slint_ui_text(const char* utf8);
void slint_ui_key(int code, int pressed);
int  slint_ui_take_browse_request(void);
int  slint_ui_take_onboarding_done(void);
int  slint_ui_take_leave_room(void);
void slint_ui_set_captures(const char* utf8);
int  slint_ui_take_challenge(void);
void slint_ui_set_peers(const char* utf8);
void slint_ui_set_ingame(int on, int round, int rounds_total, int elapsed_s,
                         int rtt, int rollbacks, int drops);
int  slint_ui_take_roster_action(void);
void slint_ui_set_settings(const char* utf8);
void slint_ui_set_input_players(const char* utf8);
void slint_ui_set_input_rows(const char* utf8);
int  slint_ui_take_input_action(char* buf, int cap);
void slint_ui_set_dev_tools(int on);
int  slint_ui_view(void);
void slint_ui_set_browse_layout(int cols, int tile_w, int tile_h);
int  slint_ui_pump_images(void);
int  slint_ui_take_setting(char* kbuf, int kcap, char* vbuf, int vcap);
int  slint_ui_take_config_dirty(void);
int  slint_ui_get_display_name(char* buf, int cap);
void slint_ui_set_display_name(const char* utf8);
void slint_ui_set_replays(const char* utf8);
void slint_ui_set_rail(const char* utf8);
void slint_ui_set_browse_rows(const char* utf8);
int  slint_ui_take_connect_hub(char* buf, int cap);
int  slint_ui_take_window_action(void);
int  slint_ui_take_replay_source(void);
void slint_ui_set_replay_note(const char* utf8);
void slint_ui_set_config_tab(int tab);
void slint_ui_set_replay_source(int src);
void slint_ui_set_maximized(int on);
void slint_ui_set_hub_host(const char* utf8);
void slint_ui_set_hub_note(const char* utf8);
int  slint_ui_take_watch(char* buf, int cap);
int  slint_ui_take_match_action(void);
int  slint_ui_take_launch(void);
int  slint_ui_selected(void);
void slint_ui_set_connected(int on);
void slint_ui_set_chat(const char* utf8);
int  slint_ui_take_chat_send(char* buf, int cap);
int  slint_ui_take_filter(char* buf, int cap);
void slint_ui_set_chat_notice(const char* utf8);
}

// Set by the resize path when the number of 320px tiles per row changes;
// the browse publish path picks it up and re-packs.
static bool g_browse_relayout = false;

#include "host_binder.h"
#include "host_discord.h"
#include "host_hub.h"
#include "host_launch.h"
#include "FM2K_FolderPicker.h"
#include "host_captures.h"
#include "host_services.h"
#include "launcher_log.h"
#include "host_settings.h"
#include "host_todo.h"

extern "C" {
void slint_ui_add_games_folder(const char* utf8);
void slint_ui_set_pads(const char* utf8);
void slint_ui_set_scene(int scene);
int  slint_ui_scene(void);
void slint_ui_set_games(const char* utf8);
void slint_ui_set_status(const char* utf8);
void slint_ui_set_scanning(int on);
void slint_ui_set_view(int view);
void slint_ui_set_match(int stage, const char* nick, int rtt, int seconds,
                        int sync_pct, const char* detail);
void slint_ui_set_match_result(int score_self, int score_opp, const char* format,
                               int rollbacks, const char* avg_rtt,
                               const char* jitter, int drops, const char* desync,
                               const char* streak);
int  slint_ui_render(unsigned* pixels, unsigned width, unsigned height,
                     unsigned stride_px, int* out_rect /* x y w h, may be null */);
// The GPU window: FemtoVG over SDL3's OpenGL context (lib.rs GlWindow).
int  slint_ui_init_gl(unsigned width, unsigned height,
                      int (*make_current)(void*), void (*swap)(void*),
                      const void* (*get_proc)(void*, const char*), void* user);
int  slint_ui_render_gl(void);
// button: SDL's index -- 1 left, 2 middle, 3 right.
void slint_ui_pointer(float x, float y, int pressed, int released, int button);
void slint_ui_scroll(float x, float y, float dx, float dy);
int  slint_ui_selected(void);
}

// --bench W H N: force N full redraws at WxH and report per-frame cost.
//
// SOFTWARE RENDERER, deliberately: this measures the CPU rasterizer that
// the headless tools still use (`shots`, the test backend), not the GPU
// path the launcher window runs on (lib.rs GlWindow). A number from here
// is not a frame time the user ever sees -- for that, run the launcher
// with FM2K_SPIKE_FRAMESTATS=1.
// Exists because "software rendering is fast enough for a launcher" is an
// assertion until someone measures it at a size people actually use. It
// deliberately measures the FULL redraw -- the worst case, every pixel --
// since the idle case is already free (slint_ui_render returns 0 and the
// host skips the frame entirely).
static int Bench(int w, int h, int frames) {
    if (slint_ui_init((unsigned)w, (unsigned)h) != 0) {
        std::fprintf(stderr, "slint_ui_init failed\n");
        return 1;
    }
    std::vector<unsigned> buf((size_t)w * h, 0);
    double total = 0.0, worst = 0.0;
    int drawn = 0;
    for (int i = 0; i < frames; ++i) {
        // Actually CHANGE the UI every frame -- click a different row --
        // rather than just moving the pointer. A move over inert space
        // dirties nothing, draw_if_needed correctly skips, and the
        // benchmark ends up timing an empty function: the first attempt
        // here redrew 1 frame in 200 and reported a one-sample "average".
        // Clicking alternating rows moves the selection highlight, which
        // is a real repaint.
        const float row_y = 90.0f + (float)(i % 3) * 30.0f;
        slint_ui_pointer(60.0f, row_y, 1, 0, 1);
        slint_ui_pointer(60.0f, row_y, 0, 1, 1);
        const auto t0 = std::chrono::steady_clock::now();
        const int did = slint_ui_render(buf.data(), (unsigned)w, (unsigned)h, (unsigned)w, nullptr);
        const auto t1 = std::chrono::steady_clock::now();
        if (!did) continue;
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total += ms;
        if (ms > worst) worst = ms;
        ++drawn;
    }
    if (!drawn) {
        std::printf("bench %dx%d: nothing redrew\n", w, h);
        return 1;
    }
    std::printf("bench %dx%d  frames=%d  avg=%.2fms  worst=%.2fms  "
                "(%.0f fps sustained on one core)\n",
                w, h, drawn, total / drawn, worst, 1000.0 / (total / drawn));
    return 0;
}

// Forced scale for testing the DPI path. Exists because you otherwise
// cannot exercise scale != 1.0 without owning a HiDPI monitor or changing
// the developer's display settings, and "the scale code is probably fine"
// is how blurry text ships. `--scale 1.5` rasterizes at 1.5x the logical
// size with the factor dispatched to Slint, which is exactly what a 150%
// display does.
static float g_forced_scale = 0.0f;

#include "host_scan.h"
#include "host_replays.h"
#include "host_library.h"

// Replay scan results, published by a worker and drained by the main loop.
static std::mutex g_replays_mutex;
static std::string g_replays_result;
static double g_loop_sum = 0, g_loop_max = 0; static int g_loop_n = 0;   // FM2K_SPIKE_FRAMESTATS

// The three things Slint's GPU renderer needs from the window system,
// forwarded to SDL (lib.rs SdlGl). `user` is the SDL_Window.
static SDL_GLContext g_glctx = nullptr;
static int GlMakeCurrent(void* user) {
    return SDL_GL_MakeCurrent(static_cast<SDL_Window*>(user), g_glctx) ? 0 : 1;
}
static void GlSwap(void* user) { SDL_GL_SwapWindow(static_cast<SDL_Window*>(user)); }
static const void* GlGetProc(void*, const char* name) {
    return reinterpret_cast<const void*>(SDL_GL_GetProcAddress(name));
}
static std::atomic<bool> g_replays_done{false};
static std::atomic<bool> g_replays_running{false};
// Did the hub answer, and which game did we ask about. Without these the
// completion handler cannot tell "nobody has uploaded" from "we never got
// to ask", so it said neither and the tab sat on "loading..." forever.
static std::atomic<bool> g_replays_reached{false};
static std::string g_replays_game;


int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--bench") == 0)
        return Bench(std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]));
    int start_view = -1;
    int match_demo = -1;
    int ingame_demo = 0;
    // The hub FIXTURE (fake roster + chat) is opt-in: `--demo`. By default
    // the launcher connects to the real hub, and a signed-out or unreachable
    // hub shows as exactly that. Fake peers on a real user's screen were a
    // lie the status bar then endorsed with HUB ONLINE.
    bool g_demo_hub = false;
    const char* hub_host = nullptr;
    const char* hub_nick = nullptr;
    const char* hub_room = nullptr;
    // Screen selectors for the capture harness. EVERY screen has to be
    // reachable by flag: driving the UI with synthetic clicks to
    // photograph it is unreliable -- half of them never landed -- and a
    // screen nobody can capture is a screen nobody reviews.
    int start_tab = -1;
    int start_scene = -1;
    int start_source = -1;
    // --offline drops the hub fixture. The signed-out states are as real
    // as the signed-in ones and were previously the ONLY states this
    // spike could show, since `connected` had no setter and sat false.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offline") == 0) g_demo_hub = false;
        if (std::strcmp(argv[i], "--demo") == 0) g_demo_hub = true;
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0)
            g_forced_scale = (float)std::atof(argv[i + 1]);
        if (std::strcmp(argv[i], "--view") == 0)
            start_view = std::atoi(argv[i + 1]);
        // Render one match-flow stage for inspection. Six modal states that
        // otherwise only appear during a live challenge are impossible to
        // review otherwise, and screenshots are how this UI gets checked.
        if (std::strcmp(argv[i], "--match") == 0)
            match_demo = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--ingame") == 0)
            ingame_demo = std::atoi(argv[i + 1]);
        // --hub is the ONLY way anything connects. There is no default
        // host on purpose: a UI spike that quietly dialed production
        // would drop a test client into the real lobby under the
        // operator's own identity.
        if (std::strcmp(argv[i], "--hub") == 0)  hub_host = argv[i + 1];
        if (std::strcmp(argv[i], "--nick") == 0) hub_nick = argv[i + 1];
        if (std::strcmp(argv[i], "--room") == 0) hub_room = argv[i + 1];
        if (std::strcmp(argv[i], "--tab") == 0)    start_tab = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--scene") == 0)  start_scene = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--source") == 0) start_source = std::atoi(argv[i + 1]);
    }

    // SDL'S LOG HAS TO GO SOMEWHERE A PERSON CAN READ. This is a
    // -mwindows build, so it has no console, and SDL's default output
    // then goes to the debugger via OutputDebugString -- invisible to
    // anyone running the exe normally, and invisible in a redirected
    // stderr. Every SDL_LogWarn in this launcher and in the launcher
    // code it links was therefore going nowhere, which cost an hour of
    // chasing a bug whose diagnostics could not print.
    //
    // TWO SINKS, because they answer different questions. stdout is for
    // whoever is running this from a shell right now; launcher.log next
    // to the exe is what a tester can attach to a report, and what the
    // crash handlers write their breadcrumb into when the process dies.
    // The disk log opens FIRST so it captures the whole startup.
    hostgui::ServicesInitLog();
    SDL_SetLogOutputFunction([](void* u, int cat, SDL_LogPriority p, const char* msg) {
        std::printf("%s: %s\n",
                    p >= SDL_LOG_PRIORITY_ERROR ? "ERROR" :
                    p == SDL_LOG_PRIORITY_WARN  ? "WARN"  : "INFO", msg);
        std::fflush(stdout);
        fm2k::launcher_log::SdlLogOutput(u, cat, p, msg);
    }, nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // 1280x720 LOGICAL -- the size the shipping launcher already opens at
    // (launcher_init.cpp:209), not a spike-sized guess.
    const int LOGICAL_W = 1280, LOGICAL_H = 720;
    // The window is an OpenGL window: Slint renders with FemtoVG on the GPU
    // through the context SDL creates (lib.rs GlWindow). FemtoVG fills
    // paths through the stencil buffer, so ask for one.
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* win = SDL_CreateWindow("2DFM Launcher",
                                       LOGICAL_W, LOGICAL_H,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
                                       | SDL_WINDOW_OPENGL);
    if (!win) {
        std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    g_glctx = SDL_GL_CreateContext(win);
    if (!g_glctx || !SDL_GL_MakeCurrent(win, g_glctx)) {
        std::fprintf(stderr, "SDL GL context: %s\n", SDL_GetError());
        return 1;
    }
    // One swap per display refresh: the loop only draws when something
    // changed, and a drawn frame waits for vblank inside the swap.
    // Adaptive first (-1: vsync, but never stall a late frame a whole
    // refresh), plain vsync when the driver has no adaptive mode.
    if (!SDL_GL_SetSwapInterval(-1)) {
        std::printf("swap interval -1 refused: %s\n", SDL_GetError());
        if (!SDL_GL_SetSwapInterval(1)) std::printf("swap interval 1 refused: %s\n", SDL_GetError());
    }
    int swap_interval = 0;
    if (!SDL_GL_GetSwapInterval(&swap_interval)) std::printf("get swap interval: %s\n", SDL_GetError());
    // When the driver refuses vsync (a control-panel override reads back
    // 0 here) the loop paces drawn frames to the panel itself: a glide
    // at 230 frames a second on a 144 Hz display is pure GPU heat.
    Uint64 frame_ns = 1'000'000'000ull / 60;
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win))) {
        if (dm->refresh_rate > 1.0f) frame_ns = (Uint64)(1e9 / dm->refresh_rate);
        std::printf("display %dx%d @ %.1f Hz, OpenGL via SDL, swap interval %d%s\n",
                    dm->w, dm->h, dm->refresh_rate, swap_interval,
                    swap_interval == 0 ? " (no vsync from the driver: pacing to the panel in software)" : "");
        std::fflush(stdout);
    }
    const bool pace_in_software = (swap_interval == 0);

    if (const int rc = slint_ui_init_gl((unsigned)LOGICAL_W, (unsigned)LOGICAL_H,
                                        GlMakeCurrent, GlSwap, GlGetProc, win); rc != 0) {
        std::fprintf(stderr, "slint_ui_init_gl failed: %d\n", rc);
        return 1;
    }

    // Gamepads for the onboarding step. Queried once here; a real launcher
    // would also watch ADDED/REMOVED events, which is deliberately left to
    // the port rather than faked in the skeleton.
    {
        std::string pads;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) {
                const char* nm = SDL_GetGamepadNameForID(ids[i]);
                if (!nm) nm = "(unnamed gamepad)";
                if (!pads.empty()) pads += "\n";
                pads += nm;
            }
            SDL_free(ids);
        }
        slint_ui_set_pads(pads.c_str());
    }

    // Text input is off by default in SDL3 and nothing arrives without
    // this. It is also what routes IME composition, so Japanese entry
    // depends on it rather than on keycodes.
    slint_ui_set_scanning(0);

    // Restore previous setup. A returning user must not be shown first-run
    // setup again -- that is the difference between a launcher and a demo.
    LoadCache();
    LoadThumbIndex();
    bool onboarded = false;
    bool rescan_pending = false;   // a folder landed while a scan was running
    std::vector<std::string> roots = LoadRoots(&onboarded);
    for (const std::string& r : roots) slint_ui_add_games_folder(r.c_str());
    if (onboarded && !roots.empty()) {
        slint_ui_set_scene(1);
        slint_ui_set_status("SCANNING");
        slint_ui_set_scanning(1);
        StartScan(roots);
        // The cache-first library: what the last run saw, on screen now;
        // the walk in the background replaces it when it finishes.
        {
            std::string cached = RowsFromCache();
            if (!cached.empty()) {
                std::lock_guard<std::mutex> lk(g_scan_mutex);
                g_scan_result = std::move(cached);
                g_scan_done = true;
            }
        }
    }

    // Demo roster. The launcher will fill this from fm2k::HubUser rows;
    // until the hub client is linked in, the fixture deliberately covers
    // every branch the row can take -- all five tier labels, all three
    // hub statuses, an unmeasured ping and a never-played opponent -- so
    // that a colour or a label that is wrong is wrong ON SCREEN rather
    // than only once a real room happens to contain that case.
    struct DemoPeer { const char* nick; const char* status; int rtt;
                      const char* tier; int self_; const char* vs;
                      const char* opponent; };
    static const DemoPeer kRoster[] = {
        { "armonte",   "idle",        0,   "monte",  1, "84-31-2", ""        },
        { "kuroneko",  "idle",        42,  "thanks", 0, "12-9-1",  ""        },
        { "sloth",     "in_match",    88,  "tester", 0, "3-5-0",   "yuudai"  },
        { "yuudai",    "in_match",    54,  "",       0, "--",      "sloth"   },
        { "patrick",   "challenging", 61,  "tester", 0, "0-1-0",   ""        },
        { "hanako",    "idle",        140, "guest",  0, "--",      ""        },
        { "anon_1874", "idle",        23,  "",       0, "--",      ""        },
    };
    {
        std::string tsv;
        for (const DemoPeer& p : kRoster) {
            tsv += p.nick; tsv += '\t'; tsv += p.status; tsv += '\t';
            tsv += std::to_string(p.rtt); tsv += '\t'; tsv += p.tier;
            tsv += '\t'; tsv += (p.self_ ? "1" : "0"); tsv += '\t';
            tsv += p.vs; tsv += '\t'; tsv += p.opponent;
            tsv += "\t0\t0\n";
        }
        slint_ui_set_peers(g_demo_hub ? tsv.c_str() : "");
    }

    // A live hub REPLACES the fixture outright rather than merging with
    // it: half-real data is the one state that cannot be reasoned about
    // from a screenshot.
    // Not const: the hub can be connected and dropped from the Network tab
    // at runtime, which is the only way a real user could ever reach one.
    // The hub to connect to: the NETWORK tab's saved host, else --hub, else
    // the public hub. The connection is real from the first frame.
    // Precedence: --hub on the command line, then the saved host, then the
    // public hub. The flag used to lose to the saved host, which made a
    // local test hub unreachable on any machine that had ever connected.
    std::string hub_seeded = (hub_host && hub_host[0]) ? std::string(hub_host) : LoadHubHost();
    if (hub_seeded.empty()) hub_seeded = "hub.2dfm.org";
    bool live_hub = !g_demo_hub && hostgui::HubStart(hub_seeded.c_str(), hub_nick, hub_room);
    if (live_hub) g_demo_hub = false;

    // The hub fixture stands in for a HubClient when none is connected.
    // It is ON by default because exercising the connected states is the
    // whole point of the spike, and OFF-able because the disconnected
    // states -- empty roster, disabled compose box, HUB OFFLINE -- are
    // real UI that has to be looked at too.
    slint_ui_set_connected(g_demo_hub ? 1 : 0);

    // Lobby transcript. The hub keeps no history, so in the real launcher
    // this vector IS the scrollback; the fixture mirrors that by owning
    // the list here rather than pushing lines at the UI one at a time.
    std::vector<std::string> chat;
    auto push_chat = [&](const char* time, const char* who, const char* text,
                         bool system, bool me) {
        std::string l = time; l += '\t'; l += who; l += '\t'; l += text;
        l += '\t'; l += (system ? "1" : "0"); l += '\t'; l += (me ? "1" : "0");
        chat.push_back(l);
    };
    auto flush_chat = [&]() {
        std::string all;
        for (const std::string& l : chat) { all += l; all += '\n'; }
        slint_ui_set_chat(all.c_str());
    };
    if (g_demo_hub) {
        push_chat("02:31", "hub",       "kuroneko joined channel", true,  false);
        push_chat("02:33", "kuroneko",  "anyone up for a few games", false, false);
        push_chat("02:34", "hanako",    "give me 10min", false, false);
        push_chat("02:36", "armonte",   "im around", false, true);
        push_chat("02:41", "hub",       "sloth vs yuudai -- room #6F2A -- 18ms", true, false);
        push_chat("02:44", "patrick",   "gl hf", false, false);
        flush_chat();
    }

    // Settings, restored before anything paints so the screen never shows
    // a default the user already changed. The rows come from the launcher's
    // real stores (host_settings.cpp); only the nick is spike-local.
    {
        int bits = 0; std::string nick;
        LoadPrefs(&bits, &nick);
        slint_ui_set_settings(hostgui::SettingsRows("").c_str());
        slint_ui_set_dev_tools(hostgui::DevToolsEnabled() ? 1 : 0);
        // The binder: the shipping core, opened here so pads are live from
        // the first frame of the INPUT tab.
        hostgui::BinderInit("");
        slint_ui_set_input_players(hostgui::BinderPlayers().c_str());
        slint_ui_set_input_rows(hostgui::BinderRows().c_str());
        if (!nick.empty()) slint_ui_set_display_name(nick.c_str());
        // Seed the box with whatever was last used, falling back to the
        // --hub flag, then to nothing. There is deliberately no default
        // host baked in here: see host_hub.h.
        const std::string seeded = hub_seeded;
        slint_ui_set_hub_host(seeded.c_str());
        hostgui::DiscordSetHubHost(seeded);
    }

    if (ingame_demo) {
        // A real session arrives here THROUGH the match flow, which is
        // what sets the opponent and the format. The demo jumps straight
        // in, so it has to supply what the flow would have left behind --
        // otherwise the dock reads "vs" with nobody on the other side.
        slint_ui_set_match(0, "kuroneko", 42, 0, 0, "");
        slint_ui_set_match_result(0, 0, "BO5", 0, "", "", 0, "none", "");
        slint_ui_set_ingame(1, 3, 5, 214, 42, 142, 0);
    }

    if (start_view >= 0) slint_ui_set_view(start_view);
    if (start_tab >= 0) slint_ui_set_config_tab(start_tab);
    if (start_scene >= 0) slint_ui_set_scene(start_scene);
    if (start_source >= 1) {
        // Same path the tab click takes, so a captured community view is
        // the real thing rather than a arrangement only the harness sees.
        slint_ui_set_replay_source(1);
    }
    if (match_demo >= 0) {
        slint_ui_set_match(match_demo, "kuroneko", 42, 12, 99,
                           "Rollback 16f - stage random - first to 5");
        slint_ui_set_match_result(3, 2, "BO5", 142, "42.4ms", "+/-0.6ms", 0,
                                  "none", "+6");
    }

    // ── custom titlebar ──
    //
    // Borderless, with our own buttons in the top bar. The OS titlebar is
    // a different design system bolted to the top of this one, and on a
    // 1280x720 shell it also costs a row of chrome we would rather spend
    // on content.
    //
    // Losing the border means losing what it did for free: dragging and
    // resizing. SDL's hit test gives both back, and it must be able to
    // answer WITHOUT asking the UI -- it runs during the OS's own drag
    // loop, where our frame is not being drawn. So the draggable strip is
    // a fixed rectangle both sides agree on (Chrome.drag_w), not something
    // measured from the live layout.
    SDL_SetWindowBordered(win, false);
    SDL_SetWindowResizable(win, true);
    // The 2DFM frame is 640x480 and the layout is designed down to it;
    // smaller than that nothing fits and the rails overlap.
    SDL_SetWindowMinimumSize(win, 640, 480);
    SDL_SetWindowHitTest(win, [](SDL_Window* w, const SDL_Point* pt, void*) -> SDL_HitTestResult {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        constexpr int kEdge = 4;      // resize border, in logical px
        constexpr int kTopH = 30;     // Chrome.top_h
        constexpr int kRailW = 80;    // Chrome.rail_w
        constexpr int kDragW = 240;   // Chrome.drag_w

        const bool left = pt->x < kEdge;
        const bool right = pt->x >= ww - kEdge;
        const bool top = pt->y < kEdge;
        const bool bottom = pt->y >= wh - kEdge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;

        // The whole bar drags except where a control sits, and the controls
        // depend on the view (app.slint): on a game's screens (lobby,
        // replays) the two view tabs follow the title strip and BOOT +
        // GAME SETTINGS sit left of the window buttons; on Browse and
        // Config only the three window buttons exist. A draggable control
        // cannot be clicked, so exactly those rectangles stay NORMAL.
        (void)kDragW;
        if (pt->y < kTopH) {
            const int view = slint_ui_view();
            const bool game_scoped = (view == 0 || view == 2);
            constexpr int kTabsW   = 1 + 2 * 78;                 // line + LOBBY + REPLAYS
            constexpr int kWinBtns = 8 + 3 * 30;                  // gap + three 30px buttons
            constexpr int kGameBtns = 84 + 6 + 118;               // BOOT, gap, GAME SETTINGS
            const int right_w = kWinBtns + (game_scoped ? kGameBtns : 0);
            const bool over_tabs  = game_scoped && pt->x >= kRailW + kDragW &&
                                    pt->x < kRailW + kDragW + kTabsW;
            const bool over_right = pt->x >= ww - right_w;
            if (!over_tabs && !over_right) return SDL_HITTEST_DRAGGABLE;
        }
        return SDL_HITTEST_NORMAL;
    }, nullptr);

    SDL_StartTextInput(win);

    // The unfiltered library, kept so the search box can narrow it and
    // widen it again without a rescan.
    std::string all_games;
    std::string last_filter;
    bool republish = false;

    // The local list, kept so switching back from the community view does
    // not require another walk of thirteen thousand files.
    std::string last_local_replays;
    // Exe stem of the selected game -- the hub keys rooms and replays by
    // it, so it is what a community query asks for.
    std::string selected_stem;
    bool selected_stem_valid = false;
    // Which list is on screen. Tracked here rather than read back from the
    // UI, because the drain is one-shot -- reading it to answer a question
    // would consume the user's next toggle.
    int replay_source = 0;

    // THE RAIL'S ROOMS, by title, in the order they were joined -- the
    // channel list, which is why nothing here reorders it. Persisted, so
    // the rail is populated on the first frame after a restart rather
    // than only once something has been clicked this session, and sent to
    // the hub on every change (PublishRail). The storage key is still
    // "mru" for compatibility with lists already on disk.
    std::vector<std::string> mru = LoadMru();
    int last_selected = -1;

    // Titles of the rows currently published, in publish order, so a title
    // from the MRU can be turned back into the index the UI indexes by.
    std::vector<std::string> shown_titles;
    std::vector<std::string> shown_stems;
    // Every installed game's folder, refreshed with the library. Handed to
    // the background services each frame.
    std::vector<std::string> service_dirs;
    std::vector<std::string> shown_paths;    // what BOOT runs
    std::vector<std::string> shown_engines;  // FM2K / FM95, picks the hook DLL
    // The library transforms live in host_library -- see that header for
    // why. The host keeps only the STATE they operate on.
    auto PublishRail = [&]() {
        slint_ui_set_rail(hostgui::RailRows(shown_titles, mru, 12,
            [&](size_t i) {
                return (i < shown_stems.size()) ? hostgui::HubUnreadFor(shown_stems[i]) : 0;
            }).c_str());
        // The rail IS the watch set: ask the hub to stream the chat and
        // rosters of every room on it, so switching rooms shows a
        // conversation that is already there. Stems, because that is what
        // the hub keys rooms by.
        std::vector<std::string> stems;
        for (const std::string& t : mru) {
            for (size_t i = 0; i < shown_titles.size() && i < shown_stems.size(); ++i) {
                if (shown_titles[i] == t) { stems.push_back(shown_stems[i]); break; }
            }
        }
        hostgui::HubSetRooms(stems);
    };
    auto NoteTitles = [&](const std::string& tsv) {
        shown_titles = hostgui::RowTitles(tsv);
    };
    auto NoteStems = [&](const std::string& tsv) {
        shown_stems = hostgui::RowStems(tsv);
        shown_paths = hostgui::RowPaths(tsv);
        shown_engines = hostgui::RowEngines(tsv);
    };
    auto PublishBrowse = [&](const std::string& rows_tsv) {
        slint_ui_set_browse_rows(rows_tsv.c_str());
    };
    auto WithRoomCounts = [&](const std::string& tsv) -> std::string {
        if (!live_hub) return tsv;
        std::string out;
        std::istringstream in(tsv);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::vector<std::string> f;
            size_t s0 = 0;
            for (;;) {
                const size_t t = line.find('\t', s0);
                f.push_back(line.substr(s0, t == std::string::npos ? t : t - s0));
                if (t == std::string::npos) break;
                s0 = t + 1;
            }
            // A row without the stem field is from an older cache. Leave
            // its count alone rather than zeroing a number we did not check.
            if (f.size() > 7 && f.size() > 5) {
                f[5] = std::to_string(hostgui::HubRoomUsers(f[7].c_str()));
            }
            for (size_t i = 0; i < f.size(); ++i) {
                out += f[i];
                if (i + 1 < f.size()) out += '\t';
            }
            out += '\n';
        }
        return out;
    };

    int phys_w = 0, phys_h = 0;

    // RASTERIZE AT PHYSICAL SIZE, LAYOUT IN LOGICAL. Rendering at logical
    // size and letting the GPU stretch the result is what makes text blurry
    // on a scaled display: glyphs get hinted for the smaller grid and then
    // upscaled. Handing Slint the scale factor keeps layout in logical
    // coordinates while glyphs rasterize at output resolution.
    //
    // This is why scale is baseline and not polish -- the buffer size, the
    // texture size and the scale factor are one decision. Deferring it
    // means rewriting this loop later, not adding to it.
    auto resync = [&]() {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (pw <= 0 || ph <= 0 || (pw == phys_w && ph == phys_h)) return;
        phys_w = pw;
        phys_h = ph;

        int lw = 0, lh = 0;
        SDL_GetWindowSize(win, &lw, &lh);
        float scale = (lw > 0) ? (float)pw / (float)lw : 1.0f;
        {
            // Tiles are a fixed 320x240; the COLUMN COUNT follows the
            // width of the content area (window minus both rails and the
            // browse padding), never the tile size.
            // Browse has no right panel: the rail, then 10px of padding a side.
            const int content = lw - 80 - 20;
            const int cols = std::max(1, std::min(5, (content + 8) / (320 + 8)));
            // Whole pixels: the art is pre-scaled to this exact size.
            const int tile_w = std::max(320, (content - (cols - 1) * 8) / cols);
            const int tile_h = tile_w * 3 / 4;
            static int s_tile_w = -1;
            if (cols != hostgui::BrowseColumns() || tile_w != s_tile_w) {
                s_tile_w = tile_w;
                hostgui::SetBrowseColumns(cols);
                slint_ui_set_browse_layout(cols, tile_w, tile_h);
                g_browse_relayout = true;
            }
        }
        if (g_forced_scale > 0.0f) {
            scale = g_forced_scale;
            pw = (int)((float)lw * scale);
            ph = (int)((float)lh * scale);
            phys_w = pw;
            phys_h = ph;
        }

        slint_ui_set_scale((unsigned)pw, (unsigned)ph, scale);
        std::printf("surface %dx%d physical, %dx%d logical, scale %.2f\n",
                    pw, ph, lw, lh, scale);
        std::fflush(stdout);
    };
    resync();

    // SDL keycode -> the small stable enum in lib.rs. Deliberately mapped
    // here rather than passing SDL keycodes across the FFI, so the Rust ABI
    // does not change when SDL does.
    auto key_code = [](SDL_Keycode k) -> int {
        switch (k) {
            case SDLK_BACKSPACE: return 1;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:  return 2;
            case SDLK_TAB:       return 3;
            case SDLK_ESCAPE:    return 4;
            case SDLK_LEFT:      return 5;
            case SDLK_RIGHT:     return 6;
            case SDLK_UP:        return 7;
            case SDLK_DOWN:      return 8;
            case SDLK_HOME:      return 9;
            case SDLK_END:       return 10;
            case SDLK_DELETE:    return 11;
            default:             return 0;
        }
    };

    for (bool running = true; running;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    resync();
                    break;

                // Pointer coordinates stay LOGICAL. SDL3 reports mouse
                // position in logical window coordinates and Slint expects
                // LogicalPosition, so scaling them here would double-apply
                // the factor and put the cursor in the wrong place on any
                // scaled display.
                case SDL_EVENT_MOUSE_MOTION:
                    slint_ui_pointer(ev.motion.x, ev.motion.y, 0, 0, 0);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    slint_ui_pointer(ev.button.x, ev.button.y, 1, 0, ev.button.button);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    slint_ui_pointer(ev.button.x, ev.button.y, 0, 1, ev.button.button);
                    break;

                case SDL_EVENT_MOUSE_WHEEL: {
                    // SDL reports wheel motion in NOTCHES; Slint wants
                    // logical pixels, so scale by a conventional line height.
                    //
                    // SIGN: pass SDL's value through UNCHANGED. Slint's
                    // delta_y already matches SDL's convention here, and
                    // negating it inverts the wheel. I negated it once while
                    // chasing a dead scrollbar; the scrollbar was dead for an
                    // unrelated reason (the ScrollView had no viewport-height)
                    // and the negation then survived as a second bug on top
                    // of the first -- a fix applied to a symptom that another
                    // fault was producing.
                    //
                    // Position comes from SDL rather than from the event that
                    // triggered it: SDL reports the REAL cursor position here
                    // and ignores any coordinate a synthetic message carries.
                    constexpr float kLinesToPx = 40.0f;
                    slint_ui_scroll(ev.wheel.mouse_x, ev.wheel.mouse_y,
                                    ev.wheel.x * kLinesToPx,
                                    ev.wheel.y * kLinesToPx);
                    break;
                }

                case SDL_EVENT_TEXT_INPUT:
                    slint_ui_text(ev.text.text);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (const int c = key_code(ev.key.key)) slint_ui_key(c, 1);
                    break;
                case SDL_EVENT_KEY_UP:
                    if (const int c = key_code(ev.key.key)) slint_ui_key(c, 0);
                    break;

                default:
                    break;
            }
        }

        // Drain UI requests that need the HOST to act. The folder picker
        // lives here and not in Slint because SDL owns the window and
        // therefore owns the native dialog -- and because a modal dialog
        // must not be opened from inside Slint's event dispatch, where it
        // would pump a nested message loop mid-update.
        // The launcher's own picker (FM2K_FolderPicker.cpp): the modern
        // Explorer dialog with a path bar and multi-select. SDL's folder
        // dialog turns into the stubby "Browse For Folder" tree on Windows.
        if (slint_ui_take_browse_request()) {
            HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            fm2k::folder_picker::OpenAsync(hwnd);
        }
        {
            std::vector<std::string> picked_all;
            fm2k::folder_picker::Take(&picked_all);
            bool added = false;
            for (const std::string& picked : picked_all) {
                if (picked.empty()) continue;
                slint_ui_add_games_folder(picked.c_str());
                if (std::find(roots.begin(), roots.end(), picked) == roots.end())
                    roots.push_back(picked);
                added = true;
            }
            if (added) {
                SaveRoots(roots, onboarded);
                // Scan NOW, in setup and in the shell alike: the folder's
                // games are the only proof the folder was the right one.
                // One scan runs at a time; a folder added mid-scan queues
                // a second pass over the full root list.
                if (g_scanning.load()) {
                    rescan_pending = true;
                } else {
                    slint_ui_set_status("SCANNING");
                    slint_ui_set_scanning(1);
                    StartScan(roots);
                }
            }
        }
        // A roster click opens the outgoing-challenge stage against that
        // peer. The real launcher also sends the challenge on the wire
        // here; the modal is the half that is ours, and it has to come up
        // immediately either way -- the hub round-trip is exactly the
        // wait the "waiting for response" stage exists to cover.
        {
            int ci = slint_ui_take_challenge();
            if (ci >= 0 && live_hub) {
                hostgui::HubChallenge(ci);
            } else if (ci >= 0 && ci < (int)(sizeof(kRoster) / sizeof(kRoster[0]))) {
                // Stage 2 is OUTGOING. Passing 1 here put up the
                // INCOMING modal -- ACCEPT/DECLINE on a challenge we
                // ourselves had just sent.
                slint_ui_set_match(2, kRoster[ci].nick, kRoster[ci].rtt, 20, 0,
                                   "Rollback 16f - stage random - first to 5");
                slint_ui_set_scene(1);
            }
        }
        {
            char rp[1024];
            const int seek = slint_ui_take_watch(rp, (int)sizeof(rp));
            if (seek >= 0 && std::strncmp(rp, "hub:", 4) == 0) {
                // A community row references a recording on the hub, not a
                // file on disk. Fetched on a worker: it is a network round
                // trip, and the frame loop is not the place for one.
                std::string host = hub_host ? hub_host : "";
                if (host.empty()) host = LoadHubHost();
                if (host.empty()) host = "hub.2dfm.org";
                const std::string ref = rp;
                slint_ui_set_replay_note("downloading...");
                std::thread([host, ref]() {
                    const std::string local = FetchCommunityFile(host, ref);
                    if (local.empty()) {
                        // The note is the only surface a background
                        // download has; saying nothing would look like the
                        // button did nothing at all.
                        slint_ui_set_replay_note("download failed");
                        return;
                    }
                    // From here a community replay IS a local one -- same
                    // file, same player, same seek contract. Anything else
                    // would be a second way to watch a replay, and the two
                    // would drift.
                    ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", nullptr);
                    slint_ui_set_replay_note("downloaded");
                    slint_ui_set_status(("watch " + local).c_str());
                }).detach();
            } else if (seek >= 0) {
                // The shipping launcher's contract: FM2K_REPLAY_SEEK_MATCH
                // is SET-OR-CLEARED every time, so a previous "from here"
                // cannot leak its seek into an unrelated watch.
                if (seek > 0) {
                    ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH",
                                              std::to_string(seek).c_str());
                } else {
                    ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", nullptr);
                }
                slint_ui_set_status(seek > 0
                    ? ("watch set from match " + std::to_string(seek)).c_str()
                    : "watch replay");
            }
        }
        if (slint_ui_take_config_dirty()) {
            // Only the nick lives here now; every stored setting goes
            // through the (key, value) drain below.
            char nm[128];
            const int n = slint_ui_get_display_name(nm, (int)sizeof(nm));
            SavePrefs(0, std::string(nm, (size_t)n));
        }
        // Art the worker finished since last frame (images.rs).
        slint_ui_pump_images();
        {
            // Binder: clicks in, captures out, republish on any change.
            char act[96];
            bool bchanged = false;
            while (slint_ui_take_input_action(act, (int)sizeof act)) {
                if (!hostgui::BinderAction(act)) HOST_TODO(act);
                bchanged = true;
            }
            if (hostgui::BinderTick()) bchanged = true;
            if (bchanged) {
                slint_ui_set_input_players(hostgui::BinderPlayers().c_str());
                slint_ui_set_input_rows(hostgui::BinderRows().c_str());
            }
        }
        {
            // Config edits: write the store, or say loudly that we cannot,
            // then republish so the screen shows what is on disk.
            char k[128], v[512];
            bool any = false;
            while (slint_ui_take_setting(k, (int)sizeof k, v, (int)sizeof v)) {
                any = true;
                const std::string game = selected_stem_valid ? selected_stem : std::string();
                // Screenshot review: picks.txt + index.tsv, then the tile
                // follows the new lead without a rescan.
                if (std::strncmp(k, "cap.", 4) == 0) {
                    const int sel = slint_ui_selected();
                    const std::string exe = (sel >= 0 && sel < (int)shown_paths.size()) ? shown_paths[(size_t)sel] : std::string();
                    if (!exe.empty() && hostgui::CaptureAction(k, exe)) {
                        slint_ui_set_captures(hostgui::CaptureRows(exe).c_str());
                        const std::string thumb = hostgui::CaptureThumbFor(exe);
                        if (!thumb.empty()) {
                            // Field 6 of the library row is the thumb path.
                            std::string rebuilt;
                            std::istringstream in(all_games);
                            std::string line;
                            while (std::getline(in, line)) {
                                if (line.empty()) continue;
                                std::vector<std::string> f;
                                size_t s0 = 0;
                                while (true) {
                                    const size_t t = line.find('\t', s0);
                                    f.push_back(line.substr(s0, t == std::string::npos ? std::string::npos : t - s0));
                                    if (t == std::string::npos) break;
                                    s0 = t + 1;
                                }
                                // Fields: 6 thumb, 7 stem, 8 exe, 9 slideshow.
                                if (f.size() > 8 && f[8] == exe) {
                                    f[6] = thumb;
                                    while (f.size() < 10) f.push_back(std::string());
                                    f[9] = hostgui::CaptureSlideshow(exe);
                                }
                                std::string j;
                                for (size_t i = 0; i < f.size(); ++i) { if (i) j += '\t'; j += f[i]; }
                                rebuilt += j + '\n';
                            }
                            all_games.swap(rebuilt);
                            republish = true;
                        }
                    } else {
                        HOST_TODO(k);
                    }
                    continue;
                }
                // Developer tools: the host has the selected exe and engine.
                if (std::strncmp(k, "tool.", 5) == 0 && std::strcmp(k, "tool.direct_addr") != 0) {
                    const int sel = slint_ui_selected();
                    const std::string exe = (sel >= 0 && sel < (int)shown_paths.size()) ? shown_paths[(size_t)sel] : std::string();
                    const std::string eng = (sel >= 0 && sel < (int)shown_engines.size()) ? shown_engines[(size_t)sel] : std::string("FM2K");
                    std::string err;
                    bool ok = true;
                    const std::string key = k;
                    if      (key == "tool.dual")        ok = hostgui::DevLaunchDual(exe, eng, &err);
                    else if (key == "tool.spectator1")  ok = hostgui::DevLaunchSpectator(1, exe, eng, &err);
                    else if (key == "tool.spectator2")  ok = hostgui::DevLaunchSpectator(2, exe, eng, &err);
                    else if (key == "tool.stress")      ok = hostgui::DevLaunchStress(exe, eng, &err);
                    else if (key == "tool.direct_host") ok = hostgui::DevLaunchDirect(exe, eng, true, "", &err);
                    else if (key == "tool.direct_join") ok = hostgui::DevLaunchDirect(exe, eng, false, hostgui::ToolsDirectAddr(), &err);
                    else if (key == "tool.terminate")   hostgui::DevTerminateAll();
                    else { HOST_TODO(k); continue; }
                    slint_ui_set_status(ok ? (key + ": started").c_str() : (key + ": " + err).c_str());
                    continue;
                }
                if (!hostgui::ApplySetting(k, v, game)) HOST_TODO(k);
            }
            // The cnc-ddraw installer works on its own thread; re-read the
            // rows whenever its state or progress moves so the DISPLAY tab
            // shows the download rather than a stale "not checked yet".
            static int s_installer_stamp = -1;
            const int stamp = hostgui::InstallerStamp();
            if (stamp != s_installer_stamp) { s_installer_stamp = stamp; any = true; }
            if (hostgui::SettingsTick()) {
                any = true;
                // A completed sign-in names the player: the HUB tab's
                // display name follows the Discord name when asked to.
                const auto d = hostgui::DiscordGet();
                if (d.signed_in && d.use_discord_name && !d.display.empty())
                    slint_ui_set_display_name(d.display.c_str());
            }
            if (any) {
                const std::string game = selected_stem_valid ? selected_stem : std::string();
                slint_ui_set_settings(hostgui::SettingsRows(game).c_str());
                slint_ui_set_dev_tools(hostgui::DevToolsEnabled() ? 1 : 0);
            }
        }
        if (int ra = slint_ui_take_roster_action()) {
            const int idx = ra >> 4;
            const int verb = ra & 0xF;
            // NOT a missing feature when offline: the roster is the hub's,
            // so with no hub there is nobody on it to act on. Saying
            // "not connected" is the truth; HOST_TODO here claimed a hole
            // in the launcher that does not exist.
            if (!live_hub) {
                slint_ui_set_status("not connected to the hub");
            } else if (verb == 1) {
                hostgui::HubSpectate(idx);
            } else if (verb == 2) {
                hostgui::HubIgnore(idx);
            } else if (verb == 3) {
                hostgui::HubBlock(idx);
            } else {
                HOST_TODO("roster action");
            }
        }
        // Match-modal buttons. Every one of these used to go nowhere, so
        // the modal was a trap: it covered the shell and nothing on it
        // could put it away. Closing is the shared behaviour; the real
        // launcher additionally sends accept/decline/cancel to the hub.
        if (int act = slint_ui_take_match_action()) {
            // 6 resync and 7 abort are GONE, not unimplemented: a desync
            // cannot be reconciled and the hook has already terminated the
            // process by then (netplay_desync.cpp). 8 was SAVE REPLAY.
            static const char* kActName[] = { "", "accept", "decline", "cancel",
                                              "rematch", "dismiss", "", "",
                                              "", "end match" };
            // 4 rematch reaches HubMatchAction's default and does nothing to
            // the peer. Say so rather than printing the verb, which reads
            // as "done".
            const bool wired = (act == 1 || act == 2 || act == 3
                                || act == 5 || act == 9);
            if (!wired) {
                HOST_TODO(act >= 1 && act <= 9 ? kActName[act] : "match action");
            } else if (act >= 1 && act <= 9) {
                slint_ui_set_status(kActName[act]);
            }
            if (live_hub) hostgui::HubMatchAction(act);
            // Ending the session hands the content area back to the tab
            // the user was on, and only then shows the result. The nick and
            // ping are whatever the match was ACTUALLY played against --
            // this used to hardcode a fixture name into the live path.
            if (act == 9) {
                slint_ui_set_ingame(0, 0, 0, 0, 0, 0, 0);
                slint_ui_set_match(5,
                                   live_hub ? hostgui::HubMatchNick() : "",
                                   live_hub ? hostgui::HubMatchRtt()  : 0,
                                   0, 100, "");
            }
            if (act != 9) {
                slint_ui_set_match(0, "", 0, 0, 0, "");
                slint_ui_set_scene(1);
            }
        }
        // BOOT: the shipping launcher's game-instance code, offline contract
        // (host_launch.cpp). Status says what happened, never just "BOOT".
        if (slint_ui_take_launch()) {
            const int sel = slint_ui_selected();
            if (sel < 0 || sel >= (int)shown_paths.size() || shown_paths[(size_t)sel].empty()) {
                slint_ui_set_status("pick a game first");
            } else {
                std::string err;
                const std::string stem = sel < (int)shown_stems.size() ? shown_stems[(size_t)sel] : std::string();
                const std::string eng  = sel < (int)shown_engines.size() ? shown_engines[(size_t)sel] : std::string("FM2K");
                if (hostgui::LaunchOffline(shown_paths[(size_t)sel], eng, stem, &err)) {
                    slint_ui_set_status(("running " + shown_titles[(size_t)sel] + " (offline)").c_str());
                    // Playing a game puts its room on the rail if it is
                    // not there, and otherwise leaves the rail alone. The
                    // rail is a channel list, not a history: chips that
                    // reshuffle themselves under the cursor every time
                    // you launch something are the behaviour the owner
                    // asked us to stop.
                    const std::string& t = shown_titles[(size_t)sel];
                    if (!t.empty() && std::find(mru.begin(), mru.end(), t) == mru.end()) {
                        mru.push_back(t);
                        SaveMru(mru);
                        PublishRail();
                    }
                } else {
                    slint_ui_set_status(("could not start: " + err).c_str());
                }
            }
        }
        {
            // While a game runs, the shell shows the in-game dock; when it
            // exits, the dock goes away and game.ini is restored.
            static int s_was_running = 0;
            const int running = hostgui::LaunchTick();
            if (running) {
                slint_ui_set_ingame(1, 0, 0, hostgui::LaunchElapsedSeconds(), 0, 0, 0);
                // An online game: report what the hook says about the match.
                if (hostgui::LaunchWasOnline()) hostgui::HubPollOutcome(hostgui::LaunchPid());
            } else if (s_was_running) {
                slint_ui_set_ingame(0, 0, 0, 0, 0, 0, 0);
                slint_ui_set_status("game closed");
                // An online match ends when the game exits: tell the hub,
                // or both players stay "in match" in everyone's lobby.
                if (hostgui::LaunchWasOnline()) hostgui::HubMatchEnded();
            }
            s_was_running = running;
        }
        switch (slint_ui_take_window_action()) {
            case 1: SDL_MinimizeWindow(win); break;
            case 2: {
                // One button, two jobs, and the UI has to be told which it
                // is doing -- the glyph differs.
                const bool max = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
                if (max) SDL_RestoreWindow(win); else SDL_MaximizeWindow(win);
                slint_ui_set_maximized(max ? 0 : 1);
                break;
            }
            case 3: running = false; break;
            default: break;
        }
        {
            char msg[1024];
            int n = slint_ui_take_chat_send(msg, (int)sizeof(msg));
            if (n > 0 && live_hub) {
                hostgui::HubSendChat(msg);
            } else if (n > 0) {
                // Appended on send because the fixture stands in for the
                // hub, which broadcasts a message back to its own sender.
                // The real client must NOT also echo, or every line
                // typed would appear twice.
                push_chat("--:--", "armonte", msg, false, true);
                flush_chat();
            }
        }
        // Picking a game is what makes it recent. Polled rather than
        // pushed because selection also changes from the browse grid and
        // from the rail itself, and one place to notice it beats three.
        {
            const int sel = slint_ui_selected();
            // Only latch once the selection can actually be RESOLVED. The
            // first poll happens while the scan is still running and
            // shown_titles is empty, so latching there consumed the initial
            // selection against an empty library and the game you opened
            // never reached the recent list.
            if (sel >= 0 && sel < (int)shown_stems.size()) {
                const std::string before = selected_stem;
                selected_stem = shown_stems[(size_t)sel];
                selected_stem_valid = !selected_stem.empty();
                // The GAME tab is per game: re-read its rows on a change.
                if (selected_stem_valid && selected_stem != before) {
                    // The lobby IS this game's room on the hub.
                    hostgui::HubJoinRoom(selected_stem.c_str(),
                                         sel < (int)shown_titles.size() ? shown_titles[(size_t)sel].c_str() : "");
                    slint_ui_set_settings(hostgui::SettingsRows(selected_stem).c_str());
                    // The GAME tab's screenshot review follows the game.
                    if (sel < (int)shown_paths.size())
                        slint_ui_set_captures(hostgui::CaptureRows(shown_paths[(size_t)sel]).c_str());
                    // Bindings follow the game too (fm2k_inputs_<stem>.ini when
                    // the player made one).
                    hostgui::BinderSetGame(selected_stem);
                    slint_ui_set_input_players(hostgui::BinderPlayers().c_str());
                    slint_ui_set_input_rows(hostgui::BinderRows().c_str());
                }
            }
            // Picking a game JOINS its room: it goes on the rail (the end,
            // never reordering what is there -- a list that rearranges
            // under the cursor is disorienting) and stays until you leave
            // it. BOOT moves it to the front (below).
            if (sel != last_selected && sel >= 0 &&
                sel < (int)shown_titles.size()) {
                last_selected = sel;
                const std::string& t = shown_titles[(size_t)sel];
                if (!t.empty() && std::find(mru.begin(), mru.end(), t) == mru.end()) {
                    mru.push_back(t);
                    SaveMru(mru);
                    PublishRail();
                }
            }
        }
        // "Leave room" from a rail chip: off the rail, off the list; the
        // game stays selected (it is still installed), only the rail moves.
        if (const int lr = slint_ui_take_leave_room()) {
            const int idx = lr - 1;
            if (idx >= 0 && idx < (int)shown_titles.size()) {
                const std::string t = shown_titles[(size_t)idx];
                // Off the rail AND out of the room: the rail is the
                // channel list, so a chip leaving it is a part.
                if (idx < (int)shown_stems.size()) hostgui::HubLeaveRoom(shown_stems[(size_t)idx]);
                mru.erase(std::remove(mru.begin(), mru.end(), t), mru.end());
                SaveMru(mru);
                PublishRail();
                slint_ui_set_status(("left " + t).c_str());
            }
        }
        // Connect / disconnect from the Network tab. Done out here rather
        // than in an event handler because dropping the client joins its
        // I/O thread, which must not happen mid-dispatch.
        {
            char hh[256];
            if (slint_ui_take_connect_hub(hh, (int)sizeof(hh))) {
                hostgui::DiscordSetHubHost(hh);  // the pairing exchange follows the typed hub
                if (live_hub) {
                    hostgui::HubStop();
                    live_hub = false;
                    g_demo_hub = false;
                    slint_ui_set_hub_note("disconnected");
                } else if (hh[0]) {
                    live_hub = hostgui::HubRestart(hh, hub_nick, hub_room);
                    if (live_hub) {
                        g_demo_hub = false;
                        SaveHubHost(hh);
                        slint_ui_set_hub_note((std::string("dialling ") + hh).c_str());
                    }
                } else {
                    slint_ui_set_hub_note("enter a hub address first");
                }
            }
        }
        // Replay source toggle. The community list is an HTTP round trip,
        // so it goes on a worker -- the same reason the local walk does.
        if (int src = slint_ui_take_replay_source(); src >= 0) {
            replay_source = src;
            if (src == 0) {
                slint_ui_set_replay_note("");
                slint_ui_set_replays(last_local_replays.c_str());
            } else {
                std::string host = hub_host ? hub_host : "";
                if (host.empty()) host = LoadHubHost();
                if (host.empty()) host = "hub.2dfm.org";
                std::string game;
                if (selected_stem_valid) game = selected_stem;
                slint_ui_set_replay_note(game.empty()
                    ? "pick a game to see its uploads"
                    : "loading...");
                if (!game.empty() && !g_replays_running.exchange(true)) {
                    slint_ui_set_replays("");
                    g_replays_game = game;
                    std::thread([host, game]() {
                        bool reached = false;
                        std::string tsv = FetchCommunityReplays(host, game, &reached);
                        {
                            std::lock_guard<std::mutex> lk(g_replays_mutex);
                            g_replays_result.swap(tsv);
                        }
                        g_replays_reached.store(reached);
                        g_replays_done.store(true);
                        g_replays_running.store(false);
                    }).detach();
                }
            }
        }
        if (g_replays_done.exchange(false)) {
            std::string tsv;
            {
                std::lock_guard<std::mutex> lk(g_replays_mutex);
                tsv.swap(g_replays_result);
            }
            // Only the LOCAL walk feeds the cache; a community result is
            // not what "this pc" should show when switching back.
            if (replay_source == 0) last_local_replays = tsv;
            slint_ui_set_replays(tsv.c_str());
            // Retire "loading...". It used to survive the result, so an
            // empty community list and a dead hub both looked like a fetch
            // that never finished.
            if (replay_source == 1) {
                if (!g_replays_reached.load()) {
                    slint_ui_set_replay_note(
                        ("could not reach " + std::string(hub_host ? hub_host
                                                                  : "the hub")).c_str());
                } else if (tsv.empty()) {
                    slint_ui_set_replay_note(
                        ("no uploads yet for " + g_replays_game).c_str());
                } else {
                    slint_ui_set_replay_note("");
                }
            }
        }
        hostgui::HubTick();
        if (slint_ui_take_onboarding_done()) {
            onboarded = true;
            SaveRoots(roots, onboarded);
            slint_ui_set_scene(1);
            // First sight of the shell is the wall of games setup just
            // found, not an empty lobby.
            slint_ui_set_view(1);
            // Setup already scanned the folders as they were added; only a
            // library that never got scanned starts one here.
            if (all_games.empty() && !g_scanning.load()) {
                slint_ui_set_status("SCANNING");
                slint_ui_set_scanning(1);
                StartScan(roots);
            }
        }

        // The background services: diagnostics bundles and replays the
        // hook left on disk. Both are throttled and go off-thread, so
        // this costs nothing on a frame with an empty queue.
        hostgui::ServicesTick(service_dirs);

        // Live progress. A scan that says nothing is indistinguishable
        // from a hang -- which is exactly how this looked a moment ago.
        if (g_scanning.load()) {
            static int last_seen = -1;
            const int seen = g_scan_seen.load();
            if (seen != last_seen) {
                last_seen = seen;
                slint_ui_set_status(("SCANNING -- " + std::to_string(seen) +
                                     " checked, " + std::to_string(g_scan_hits.load()) +
                                     " found, " + std::to_string(g_scan_cached.load()) + " cached").c_str());
            }
        }

        if (g_scan_done.exchange(false)) {
            {
                std::lock_guard<std::mutex> lk(g_scan_mutex);
                all_games.swap(g_scan_result);
            }
            const size_t n =
                (size_t)std::count(all_games.begin(), all_games.end(), '\n');
            {
                const std::string published = WithRoomCounts(all_games);
                slint_ui_set_games(published.c_str());
                NoteTitles(published);
                NoteStems(published);
                // The folders the background services work in. The hook
                // writes upload_queue/, replays/ and sessions/ next to
                // each game's exe, so the library IS the work list.
                service_dirs.clear();
                for (const std::string& exe : hostgui::RowPaths(published)) {
                    const size_t cut = exe.find_last_of("\\/");
                    if (cut != std::string::npos) service_dirs.push_back(exe.substr(0, cut));
                }
                PublishRail();
                PublishBrowse(hostgui::BrowseRows(published, mru, !last_filter.empty()));
            }
            slint_ui_set_scanning(0);
            // Replays are walked off the back of the game scan rather than
            // on their own timer: they live inside the game folders, so
            // there is nothing to look at until those are known.
            // OFF THE MAIN THREAD. This walks every game's replays and
            // sessions folder and reads a 256-byte header from each -- on
            // a real collection that is over thirteen thousand files, and
            // running it inline froze the window for the whole walk. The
            // game scan was threaded for exactly this reason; this one was
            // not, and it is the slower of the two.
            if (!g_replays_running.exchange(true)) {
                std::thread([roots]() {
                    std::string tsv = ScanReplays(roots);
                    {
                        std::lock_guard<std::mutex> lk(g_replays_mutex);
                        g_replays_result.swap(tsv);
                    }
                    g_replays_done.store(true);
                }).detach();
            }
            slint_ui_set_status((std::to_string(n) + " GAME(S)").c_str());
            if (rescan_pending) {
                rescan_pending = false;
                slint_ui_set_status("SCANNING");
                slint_ui_set_scanning(1);
                StartScan(roots);
            }
        }

        // Live room populations. The hub owns this number, the library
        // rows are built by a filesystem scan that cannot know it, so the
        // two are joined here on the exe stem the scan appends.
        if (live_hub && hostgui::HubTakeRoomsDirty()) republish = true;
        // Unread marks live on the rail, which the library republish does
        // not touch.
        if (live_hub && hostgui::HubTakeUnreadDirty()) PublishRail();

        // Library search. Filtering happens HERE and not in the .slint
        // because the tile grid is laid out from a host-computed row
        // count -- a UI-side filter would leave the rows and the tiles
        // disagreeing about how many there are.
        {
            char f[256];
            const int fn = slint_ui_take_filter(f, (int)sizeof(f));
            if (fn >= 0) { last_filter.assign(f, (size_t)fn); republish = true; }
            if (g_browse_relayout) { g_browse_relayout = false; republish = true; }
            if (republish) {
                republish = false;
                const std::string out = hostgui::FilterByTitle(all_games, last_filter);
                {
                    const std::string published = WithRoomCounts(out);
                    slint_ui_set_games(published.c_str());
                    NoteTitles(published);
                    NoteStems(published);
                    PublishRail();
                    PublishBrowse(hostgui::BrowseRows(published, mru, !last_filter.empty()));
                }
            }
        }

        // Whole-iteration time (everything above this line: events, hub,
        // scan publish, settings) -- the part the frame numbers hide.
        {
            static Uint64 s_loop_t0 = 0;
            const Uint64 now = SDL_GetTicksNS();
            if (s_loop_t0) {
                const double ms = (double)(now - s_loop_t0) / 1e6;
                g_loop_sum += ms; if (ms > g_loop_max) g_loop_max = ms; ++g_loop_n;
            }
            s_loop_t0 = now;
        }

        // FM2K_SPIKE_SCROLLTEST=1: the launcher scrolls ITSELF. Eight
        // seconds after start (the art burst is over) it maximizes, then
        // feeds Slint a wheel step every 120 ms, forty down and forty up,
        // straight into slint_ui_scroll -- no OS input, no focus theft,
        // the window can sit behind whatever you are doing. Read the
        // FRAMESTATS lines for the result.
        {
            static const bool s_selftest = [] { const char* e = std::getenv("FM2K_SPIKE_SCROLLTEST"); return e && e[0] == '1'; }();
            if (s_selftest) {
                static const Uint64 s_t0 = SDL_GetTicks();
                static int s_step = -1;
                static Uint64 s_last = 0;
                const Uint64 now = SDL_GetTicks();
                if (s_step < 0 && now - s_t0 > 8000) {
                    SDL_MaximizeWindow(win);
                    s_step = 0; s_last = now + 1500;
                    std::printf("scrolltest: start\n"); std::fflush(stdout);
                } else if (s_step >= 0 && s_step < 80 && now - s_last >= 120) {
                    s_last = now;
                    int lw = 0, lh = 0;
                    SDL_GetWindowSize(win, &lw, &lh);
                    slint_ui_scroll((float)lw * 0.55f, (float)lh * 0.6f, 0.0f, s_step < 40 ? -40.0f : 40.0f);
                    if (++s_step == 80) { std::printf("scrolltest: done\n"); std::fflush(stdout); }
                }
            }
        }

        // Draw ONLY when Slint says something changed: an idle launcher
        // sitting on a game list costs nothing. A drawn frame renders on
        // the GPU and swaps, and the swap waits for vblank.
        const Uint64 t_frame0 = SDL_GetTicksNS();
        if (slint_ui_render_gl()) {
            Uint64 t_frame1 = SDL_GetTicksNS();
            if (pace_in_software && t_frame1 - t_frame0 < frame_ns) {
                SDL_DelayNS(frame_ns - (t_frame1 - t_frame0));
                t_frame1 = SDL_GetTicksNS();
            }
            if (t_frame1 - t_frame0 > 40'000'000ull) {
                std::printf("slow frame: %.1f ms at %dx%d\n",
                            (double)(t_frame1 - t_frame0) / 1e6, phys_w, phys_h);
                std::fflush(stdout);
            }
            // FM2K_SPIKE_FRAMESTATS=1: one line per second with the frame
            // count and the average / worst frame (render + swap, so the
            // vsync wait is inside) and loop iteration times.
            static const bool s_stats = [] { const char* e = std::getenv("FM2K_SPIKE_FRAMESTATS"); return e && e[0] == '1'; }();
            if (s_stats) {
                static Uint64 s_win0 = 0; static int s_n = 0;
                static double s_sum = 0, s_max = 0;
                const double fms = (double)(t_frame1 - t_frame0) / 1e6;
                if (!s_win0) s_win0 = t_frame1;
                ++s_n; s_sum += fms; if (fms > s_max) s_max = fms;
                if (t_frame1 - s_win0 >= 1'000'000'000ull) {
                    std::printf("frames/s %d  frame avg %.1f max %.1f ms (render+swap)  loop avg %.1f max %.1f ms  at %dx%d\n",
                                s_n, s_sum / s_n, s_max,
                                g_loop_sum / std::max(1, g_loop_n), g_loop_max, phys_w, phys_h);
                    std::fflush(stdout);
                    s_win0 = t_frame1; s_n = 0; s_sum = s_max = 0;
                    g_loop_sum = g_loop_max = 0; g_loop_n = 0;
                }
            }
        } else {
            // Nothing to draw: sleep a little rather than spin. Four
            // milliseconds keeps input latency under a frame at 144 Hz.
            SDL_Delay(4);
        }
    }

    SDL_StopTextInput(win);
    if (g_glctx) SDL_GL_DestroyContext(g_glctx);
    hostgui::LaunchShutdown();
    hostgui::BinderShutdown();
    hostgui::SettingsShutdown();
    hostgui::ServicesPortMapStop();
    hostgui::ServicesShutdownLog();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
