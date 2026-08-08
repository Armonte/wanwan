#include "FM2K_Integration.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// Persistence helpers (dev_flags / game-patches / settings.ini) moved to
// launcher_ui_settings_io.cpp; pull them into scope so the method bodies below
// keep calling them unqualified.
using namespace lui;


// Case-insensitive match of `room_id` against installed games. A
// match is either an exact stem hit (room "SCWU" -> "SCWU.exe") or
// the room id followed by a non-letter on the stem
// (room "WonderfulWorld" -> "WonderfulWorld_ver_0946.exe", with '_'
// being non-alpha). The non-letter gate avoids overmatching on
// unrelated games whose stems happen to start with the same word
// ("Strip" -> "StripFighter5CE" vs "StripFighter_Zero" both pass
// when 'F' is non-alpha -- but "Strip" wouldn't be a real room id).
//
// Phase-2 master game list will replace this heuristic with a
// canonical-id → exe-aliases table. Until then this gets us through
// versioned exes without a manual selection step.
// Launcher-side preflight: bidirectional 0xCD CTRL_PUNCH on the same
// UDP port the spawned game's hook will bind. Confirms peer reachability
// AND opens the NAT pinhole before launch -- the hook's own punch is then
// redundant in the happy path but stays as a safety net. Closes the socket
// on return so the game DLL can re-bind via SO_REUSEADDR.
//
// Returns true if at least one authentic peer punch was observed.
// Synchronous and bounded by `timeout_ms`; UI freezes briefly while it
// runs (≤ ~50 ms on loopback, <1 s typical LAN/Internet).
// File-scope mirror of the dev "auto-upload diagnostics" checkbox so
// PollUploadQueue (a class method) can read it without piping a class
// member through. Initialized from dev_flags.ini on first Render() and
// kept in sync when the checkbox flips. See FM2K_UploadQueue.cpp for
// the upload pipeline. Default ON (meta is PII-scrubbed before transmit);
// the loaded dev_flags.ini value overrides this once the user opts out.





// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : games_{}
    , network_config_{} // Zero-initialize network config
    , frames_ahead_(0.0f)
    , launcher_state_(LauncherState::GameSelection)
    , renderer_(nullptr)
    , window_(nullptr)
    , scanning_games_(false)
    , games_root_paths_{}
    , selected_game_index_(-1)
    , current_theme_(UITheme::Dark)
    , scroll_to_bottom_(true)
    , original_log_function_(nullptr)
    , original_log_userdata_(nullptr)
{
    // Developer mode: opt-in via env var. End users see a simplified
    // panel with just Online (Hub) / Offline / Replay; developers see
    // the full battery of bisect checkboxes, stress, dual-client, etc.
    if (const char* env_dev = std::getenv("FM2K_DEV_MODE");
        env_dev && std::strcmp(env_dev, "1") == 0) {
        developer_mode_ = true;
    }

    hub_state_ = std::make_unique<HubState>();

    // Initialize callbacks to null
    on_game_selected = nullptr;
    on_offline_session_start = nullptr;
    on_online_session_start = nullptr;
    on_stress_session_start = nullptr;
    on_session_stop = nullptr;
    on_exit = nullptr;
    on_games_folders_set = nullptr;
    on_debug_save_state = nullptr;
    on_debug_load_state = nullptr;
    on_debug_force_rollback = nullptr;
    on_frame_step_pause = nullptr;
    on_frame_step_single = nullptr;
    on_frame_step_multi = nullptr;
    on_debug_save_to_slot = nullptr;
    on_debug_load_from_slot = nullptr;
    on_debug_auto_save_config = nullptr;
    on_get_slot_status = nullptr;
    on_get_auto_save_config = nullptr;
    on_get_enhanced_actions = nullptr;
    on_set_production_mode = nullptr;
    on_set_input_recording = nullptr;
    on_set_minimal_gamestate_testing = nullptr;
    // on_set_save_profile removed - now using optimized FastGameState system
    
    // Initialize multi-client testing callbacks
    on_launch_local_client1 = nullptr;
    on_launch_local_client2 = nullptr;
    on_launch_local_spectator = nullptr;
    on_launch_local_spectator2 = nullptr;
    on_terminate_all_clients = nullptr;
    on_get_client_status = nullptr;
    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    on_get_rollback_stats = nullptr;

    log_buffer_mutex_ = SDL_CreateMutex();
}

LauncherUI::~LauncherUI() {
    if (log_buffer_mutex_) {
        SDL_DestroyMutex(log_buffer_mutex_);
        log_buffer_mutex_ = nullptr;
    }
    Shutdown();
}

bool LauncherUI::Initialize(SDL_Window* window, SDL_Renderer* renderer) {
    if (!window || !renderer) {
        std::cerr << "Invalid SDL window or renderer" << std::endl;
        return false;
    }
    renderer_ = renderer;
    window_ = window;
    
    // NUCLEAR: Exact copy of official SDL3 renderer example initialization
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup scaling - THIS IS CRITICAL FOR FONT STACK
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Two-font atlas: a Latin-first font for ASCII / Latin-1 (so backslash
    // stays a backslash and Spanish accented chars render natively), then
    // MS Gothic / Meiryo merged on top for Japanese coverage.
    //
    // Why this matters: Japanese fonts follow JIS X 0201, which maps
    // codepoint 0x5C to the yen sign (¥) instead of backslash. If we load
    // a JP font first with `GetGlyphRangesJapanese()` (which includes
    // ASCII), every backslash in the UI renders as ¥ -- visible in file
    // paths, escape characters in tooltips, etc. By loading Segoe UI (or
    // any Latin font) first to claim ASCII slots, then merging the JP
    // font with MergeMode=true, ImGui keeps the Latin font's glyph for
    // any codepoint already in the atlas and only pulls JP glyphs from
    // MS Gothic. Backslash stays a backslash, hiragana/kanji come from JP.
    {
        // Latin font candidates in priority order. Segoe UI is the modern
        // Windows UI font (Vista+); Tahoma is a universal fallback that
        // ships on every Windows install.
        const char* latin_font_paths[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
        };
        ImFontConfig latin_config;
        latin_config.OversampleH = 2;
        latin_config.OversampleV = 2;
        latin_config.PixelSnapH = false;
        // Range: ASCII + Latin-1 Supplement (Armonté etc.) + Halfwidth
        // and Fullwidth Forms (FM2K games shipped by Japanese authors
        // commonly have full-width-titled exes like ＣＰＷ.exe). Segoe
        // UI / Tahoma / Arial all ship with the full-width block in
        // their CMAPs, so requesting it here pulls those glyphs into
        // the atlas without needing the MS Gothic merge to backfill.
        // Belt-and-suspenders: the JP merge below ALSO requests FF00-
        // FFEF, but ImGui's packed accumulator decompression has been
        // observed to drop ranges silently -- claiming the block from
        // the Latin font directly is the reliable path.
        static const ImWchar latin_range[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0xFF00, 0xFFEF,   // Halfwidth + Fullwidth Forms
            0,
        };
        ImFont* latin_font = nullptr;
        for (const char* p : latin_font_paths) {
            latin_font = io.Fonts->AddFontFromFileTTF(p, 16.0f, &latin_config,
                                                     latin_range);
            if (latin_font) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Loaded Latin UI font: %s", p);
                break;
            }
        }
        if (!latin_font) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No Latin system font loaded -- falling back to "
                        "ImGui default. Backslash and accented characters "
                        "may render with the bundled bitmap font.");
            io.Fonts->AddFontDefault();
        }

        // Japanese fonts merged on top. MergeMode=true means glyphs already
        // claimed by the Latin font (ASCII / Latin-1 supplement) stay with
        // the Latin font; only codepoints not yet in the atlas (kana,
        // kanji, half-width katakana, etc) get pulled from MS Gothic.
        ImFontConfig jp_config;
        jp_config.MergeMode    = true;
        jp_config.OversampleH  = 2;
        jp_config.OversampleV  = 2;
        jp_config.PixelSnapH   = true;
        const char* jp_font_paths[] = {
            "C:\\Windows\\Fonts\\msgothic.ttc",
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttf",
            "C:\\Windows\\Fonts\\YuGothM.ttc",
        };
        bool jp_loaded = false;
        const char* jp_loaded_path = nullptr;
        for (const char* p : jp_font_paths) {
            if (io.Fonts->AddFontFromFileTTF(p, 16.0f, &jp_config,
                                             io.Fonts->GetGlyphRangesJapanese())) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Merged Japanese font: %s", p);
                jp_loaded = true;
                jp_loaded_path = p;
                break;
            }
        }
        // Belt-and-suspenders: explicitly request the Halfwidth and
        // Fullwidth Forms block (U+FF00-FFEF) from the same JP font.
        // GetGlyphRangesJapanese() *should* include this range, but the
        // packed accumulator decompression has historically dropped it
        // on some ImGui revisions. Game directories commonly contain
        // full-width-titled exes (e.g. ＣＰＷ.exe), so missing glyphs
        // here render as visible underscores in the Settings panel --
        // very specifically the bug we just hit. Adding the range
        // again with MergeMode=true is a no-op when it was already
        // included; otherwise it backfills the missing glyphs.
        if (jp_loaded && jp_loaded_path) {
            static const ImWchar fullwidth_range[] = {
                0xFF00, 0xFFEF,   // Halfwidth + Fullwidth Forms
                0,
            };
            io.Fonts->AddFontFromFileTTF(jp_loaded_path, 16.0f, &jp_config,
                                         fullwidth_range);
        }
        if (!jp_loaded) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No Japanese-capable system font found -- Japanese "
                        "text will render as '?'. Install East Asian "
                        "language pack to fix.");
        }
    }

    // Locale: load translation tables and pick the active language. Must
    // happen AFTER the font is configured (so ImGui has glyphs ready when
    // the first frame renders) but BEFORE any T() call.
    fm2k::Locale::Init();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    
    // Setup our logging to capture SDL logs. Init the PII scrubber
    // BEFORE the first SDL_Log so even the "Launcher UI Initialized"
    // line goes through redaction.
    fm2k::pii::Init();
    SDL_GetLogOutputFunction(&original_log_function_, &original_log_userdata_);
    SDL_SetLogOutputFunction(SDLCustomLogOutput, this);

    SDL_Log("Launcher UI Initialized");
    
    return true;
}

void LauncherUI::Shutdown() {
    // Tear down the updater's background worker so we don't leak the
    // thread on exit (and so a mid-flight download is cancelled cleanly
    // rather than racing with shutdown).
    fm2k::updater::Shutdown();
    fm2k::cnc_ddraw::Shutdown();

    // Tear down the UPnP mapping (DeletePortMapping + join the worker) so we
    // don't leave a stale router forward behind on exit and don't leak the
    // discovery/renewal thread. Idempotent + a no-op if we never mapped.
    if (port_mapper_) {
        port_mapper_->Stop();
    }

    // Restore original logger
    SDL_SetLogOutputFunction(original_log_function_, original_log_userdata_);

    // Cleanup ImGui
    if (ImGui::GetCurrentContext()) {
        // Make sure we finish any pending viewport operations
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    
    std::cout << "LauncherUI shutdown" << std::endl;
}

void LauncherUI::NewFrame() {
    // Start the Dear ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

bool LauncherUI::WantsContinuousRedraw() const {
    // Animations that must keep ticking even when the user is idle:
    //  - scanning_games_: the "Scanning for games..." spinner (short-lived).
    //  - input-binder windows: they show live analog-stick positions and a
    //    "press a button" capture state that aren't always event-driven.
    // The Discord-pill pulse and transient modal dots intentionally fall back
    // to the 250ms safety-net repaint -- keeping the CPU spinning for a menu-
    // bar pulse is exactly the weak-CPU idle drain we're eliminating.
    return scanning_games_ || show_input_binder_p1_ || show_input_binder_p2_;
}

void LauncherUI::Render() {
    // First-run nudge: if no cached Discord session and we haven't
    // shown the prompt yet this run, auto-open the sign-in window.
    // Hub auth is mandatory for online play during testing; users
    // landing on the launcher should see the path forward right away
    // rather than discovering it via "auth_required" after a failed
    // Connect.
    {
        static bool s_did_first_run_nudge = false;
        if (!s_did_first_run_nudge) {
            s_did_first_run_nudge = true;
            const auto a = fm2k::discord_auth::LoadCached();
            if (!a.valid) {
                show_discord_auth_ = true;
            }
        }
    }

    // NOTE: PollMatchOutcome() used to live here. It now runs in
    // TickAlways() from FM2KLauncher::Update(), because Render() itself is
    // skipped entirely while the launcher is minimized -- see TickAlways().

    // Drain at most one upload manifest per tick. Throttles network
    // bandwidth/UI hitches and lets transient failures get retried on
    // a later tick. Gated on dev checkbox + non-empty queue.
    PollUploadQueue();

    // Render menu bar at application level first
    RenderMenuBar();
    
    // Create a dockspace to allow for flexible panel arrangement
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    window_flags |= ImGuiWindowFlags_NoBackground;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // First-launch default layout. ImGui persists user-edited layout into
    // imgui.ini on quit, so this only fires on a fresh install (or after
    // the user deletes imgui.ini). DockBuilder gates on whether the node
    // already has children -- if any prior layout exists, we leave it
    // alone so users keep their customizations across versions.
    {
        static bool s_layout_built = false;
        if (!s_layout_built) {
            s_layout_built = true;
            ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
            if (!root || (root->Windows.Size == 0 && !root->IsSplitNode())) {
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

                // Two-pane default -- narrow left rail for Games &
                // Configuration + Debug & Diagnostics (as tabs); wide
                // right pane holds the Hub. Mirrors the operator's
                // own working layout (~270 / ~1010 split at 1280×720).
                // The split ratio is chosen to keep the rail >= 220
                // even at our 640×480 minimum window size, so the rail
                // tabs stay clickable on small displays.
                ImGuiID main_id = dockspace_id;
                ImGuiID left_id = 0, right_id = 0;
                // Left rail = ~22% of the dockspace width. Floor at 220
                // for usability on tiny windows.
                const float work_w   = viewport->WorkSize.x;
                const float left_pct = (work_w > 0.0f)
                    ? std::max(0.18f, std::min(0.30f, 220.0f / work_w))
                    : 0.21f;
                ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left,
                                            left_pct, &left_id, &right_id);
                ImGui::DockBuilderDockWindow("Games & Configuration", left_id);
                ImGui::DockBuilderDockWindow("Debug & Diagnostics",   left_id);
                ImGui::DockBuilderDockWindow("Hub",                   right_id);
                // Settings windows are popups (NoDocking) -- they
                // intentionally float above the dockspace and aren't
                // listed here.
                ImGui::DockBuilderFinish(dockspace_id);
            }
        }
    }

    ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Games & Configuration", nullptr, panel_flags)) {
        RenderGameSelection();
        ImGui::Separator();
        // Network-config panel is dev-mode only -- end users use the Hub
        // panel for matchmaking and don't manually configure ports/IPs.
        if (developer_mode_) {
            RenderNetworkConfig();
            ImGui::Separator();
        }
        RenderSessionControls();
    }
    ImGui::End();

    if (ImGui::Begin("Hub", nullptr, panel_flags)) {
        RenderHubPanel();
    }
    ImGui::End();

    if (developer_mode_) {
        if (ImGui::Begin("Debug & Diagnostics", nullptr, panel_flags)) {
            RenderDebugTools();
        }
        ImGui::End();
    }
    
    ImGui::End(); // End DockSpace

    // Render connection status popups
    RenderConnectionStatus();
}















// Audio-mute persistence. Same file the hook DLL reads from inside
// Hook_DispatchScriptSoundCommand. We deliberately use a tiny flat
// key=value format so a textedit-the-ini fallback works for users
// without a launcher rebuild.



// ---------------------------------------------------------------------------
// Notification settings + delivery
// ---------------------------------------------------------------------------
// Persists three independent toggles to the launcher's settings.ini next to
// the Locale module's `language` key. Defaults are all-on so users never miss
// a challenge while tabbed out -- they can dial it back per-channel from
// Settings → Notifications.













// Note: ShowGameValidationStatus removed ? UI simplified
/* void LauncherUI::ShowGameValidationStatus(const FM2K::FM2KGameInfo& game) {} */



// Data binding methods
void LauncherUI::SetGames(const std::vector<FM2K::FM2KGameInfo>& games) {
    games_ = games;
}

void LauncherUI::SetNetworkConfig(const NetworkConfig& config) {
    network_config_ = config;
}


void LauncherUI::SetLauncherState(LauncherState state) {
    launcher_state_ = state;
}

void LauncherUI::SetScanning(bool scanning) {
    scanning_games_ = scanning;
}

void LauncherUI::SetGamesRootPaths(const std::vector<std::string>& paths) {
    games_root_paths_ = paths;
}

void LauncherUI::SendHubTcpAddr(uint32_t ip_be, uint16_t port) {
    if (!hub_state_) return;
    char ip_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &ip_be, ip_str, sizeof(ip_str));
    hub_state_->client.SendTcpAddr(std::string(ip_str), (int)port);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: forwarded TCP-STUN result %s:%u to hub", ip_str,
                (unsigned)port);
}

void LauncherUI::SendHubSessionKind(uint8_t kind) {
    if (!hub_state_) return;
    const char* kind_str = (kind == 2) ? "battle"
                         : (kind == 1) ? "css"
                         :               "menu";
    hub_state_->client.UpdateSessionKind(kind_str);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: forwarded session_kind=%s to hub", kind_str);
}

void LauncherUI::SendHubSpecRelayFrame(std::vector<uint8_t> frame) {
    if (!hub_state_) return;
    // No verbose log per call -- this fires many times per match
    // during a snapshot transfer. Hub-side log via SPEC_RELAY shows
    // overall traffic; if we want diagnostics here add a throttled
    // counter.
    hub_state_->client.SendSpecRelayFrame(std::move(frame));
}

void LauncherUI::SetSpecRelayStatus(const SpecRelayStatus& st) {
    spec_relay_status_ = st;
}

uint64_t LauncherUI::HubSpecRelayDropped() const {
    if (!hub_state_) return 0;
    return hub_state_->client.SpecRelayDropped();
}

void LauncherUI::SetFramesAhead(float frames_ahead) {
    frames_ahead_ = frames_ahead;
}

// Simplified theme - always use Dark
void LauncherUI::SetTheme(UITheme theme) {
    current_theme_ = UITheme::Dark;
    ImGui::StyleColorsDark();
}
















