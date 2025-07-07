#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "MinHook.h"
#include "FM2K_Integration.h"

#include <chrono>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>

// -----------------------------------------------------------------------------
// Async game discovery support
// -----------------------------------------------------------------------------

// Custom SDL event sent from the worker thread once discovery finishes.
static Uint32 g_event_discovery_complete = 0;

// Worker thread entry-point. Performs blocking discovery on a background thread
// and notifies the main thread with the resulting vector.
static int DiscoveryThreadFunc(void* userdata) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(userdata);
    if (!launcher) {
        return -1;
    }

    // The heavy lifting ? this call walks the filesystem and builds the list.
    auto games = new std::vector<FM2K::FM2KGameInfo>(launcher->DiscoverGames());

    SDL_Event ev{};
    ev.type = g_event_discovery_complete;
    ev.user.data1 = games;   // Ownership transferred to main thread
    ev.user.code = 0;
    SDL_PushEvent(&ev);

    return 0;
}

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

// Utility implementations
namespace Utils {
    std::vector<std::string> FindFilesWithExtension(const std::string& directory, const std::string& extension) {
        // Non-recursive scan using SDL3's filesystem helper.
        std::vector<std::string> files;
        int count = 0;
        std::string pattern = "*" + extension; // e.g., "*.kgt"
        char **list = SDL_GlobDirectory(directory.c_str(), pattern.c_str(), /*flags=*/0, &count);
        if (list) {
            for (int i = 0; i < count; ++i) {
                if (list[i]) files.emplace_back(list[i]);
            }
            SDL_free(list);
        }
        return files;
    }
    
    bool FileExists(const std::string& path) {
        if (SDL_GetPathInfo(path.c_str(), nullptr)) {
            return true;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FileExists check failed for %s: %s", path.c_str(), SDL_GetError());
        return false;
    }
    
    std::string GetFileVersion(const std::string& exe_path SDL_UNUSED) {
        // TODO: Implement proper version detection
        return "Unknown";
    }
    
    uint32_t Fletcher32(const uint16_t* data, size_t len) {
        uint32_t c0, c1;
        len = (len + 1) & ~1;
        
        for (c0 = c1 = 0; len > 0; ) {
            size_t blocklen = len;
            if (blocklen > 360 * 2) {
                blocklen = 360 * 2;
            }
            len -= blocklen;
            do {
                c0 = c0 + *data++;
                c1 = c1 + c0;
            } while ((blocklen -= 2));
            c0 = c0 % 65535;
            c1 = c1 % 65535;
        }
        return (c1 << 16 | c0);
    }
    
    float GetFM2KFrameTime(float frames_ahead) {
        const float base_frame_time = 1.0f / 100.0f;  // 10ms per frame
        
        if (frames_ahead >= 0.75f) {
            return base_frame_time * 1.02f;  // Slow down if too far ahead
        } else {
            return base_frame_time;
        }
    }
    
    std::chrono::milliseconds GetFrameDuration() {
        return std::chrono::milliseconds(10);  // 100 FPS = 10ms per frame
    }

    // ---------------------------------------------------------------------
    // Config handling (persistent games folder)
    // ---------------------------------------------------------------------
    static std::string GetConfigDir() {
        const char *pref = SDL_GetPrefPath("FM2K", "RollbackLauncher");
        std::string dir = pref ? pref : SDL_GetBasePath();
        if (pref) SDL_free(const_cast<char*>(pref));

        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');

        // Ensure directory exists
        SDL_CreateDirectory(dir.c_str());
        return dir;
    }

    static std::string GetConfigFilePath() {
        return GetConfigDir() + "launcher.cfg";
    }

    std::string LoadGamesRootPath() {
        std::string cfg = GetConfigFilePath();
        if (!SDL_GetPathInfo(cfg.c_str(), nullptr)) {
            return {};
        }
        std::ifstream in(cfg);
        if (!in) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open config file: %s", cfg.c_str());
            return {};
        }
        std::string line;
        std::getline(in, line);
        in.close();
        return line;
    }

    void SaveGamesRootPath(const std::string& path) {
        std::string cfg = GetConfigFilePath();
        // Ensure directory exists
        // Directory is guaranteed to exist from GetConfigDir
        std::ofstream out(cfg, std::ios::trunc);
        if (!out) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write config file: %s", cfg.c_str());
            return;
        }
        out << path << std::endl;
    }

    // -------------------------------------------------------------
    // Lightweight games cache so we can show results instantly on
    // next launch and avoid rescanning unchanged paths.
    // -------------------------------------------------------------

    static std::string GetCacheFilePath() {
        return GetConfigDir() + "games.cache";
    }

    void SaveGameCache(const std::vector<FM2K::FM2KGameInfo>& games) {
        std::ofstream out(GetCacheFilePath(), std::ios::trunc);
        if (!out) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to write game cache");
            return;
        }

        for (const auto& g : games) {
            out << g.exe_path << "|" << g.dll_path << "\n";
        }
    }

    std::vector<FM2K::FM2KGameInfo> LoadGameCache() {
        std::vector<FM2K::FM2KGameInfo> cached;
        std::ifstream in(GetCacheFilePath());
        if (!in) return cached; // No cache yet

        std::string line;
        while (std::getline(in, line)) {
            size_t sep = line.find('|');
            if (sep == std::string::npos) continue;
            std::string exe = line.substr(0, sep);
            std::string dll = line.substr(sep + 1);

            // Validate paths still exist
            if (SDL_GetPathInfo(exe.c_str(), nullptr) && SDL_GetPathInfo(dll.c_str(), nullptr)) {
                FM2K::FM2KGameInfo game;
                game.exe_path = exe;
                game.dll_path = dll;
                game.is_host = true;
                game.process_id = 0;
                cached.push_back(std::move(game));
            }
        }
        return cached;
    }

    // Helper to normalize paths for SDL (convert backslashes to forward slashes)
    inline std::string NormalizePath(std::string path) {
        for (auto& ch : path) {
            if (ch == '\\') ch = '/';
        }
        return path;
    }

    // Recursively search for files that match the provided extension. This allows the
    // launcher to pick up games that live inside their own folders under the common
    // "games" directory (e.g. games/SomeGame/SomeGame.exe + .kgt).
    inline std::vector<std::string> FindFilesWithExtensionRecursive(const std::string& directory,
                                                                    const std::string& extension) {
        std::vector<std::string> files;

        // Prefer SDL3's cross-platform glob helper if available. This will enumerate the
        // entire directory tree for us and handle platform quirks internally.
        std::string normalized_dir = NormalizePath(directory);
        int count = 0;
        std::string pattern = "*" + extension; // e.g., "*.kgt"
        char **list = SDL_GlobDirectory(normalized_dir.c_str(), pattern.c_str(), /*flags=*/0, &count);
        if (list) {
            for (int i = 0; i < count; ++i) {
                if (list[i]) files.emplace_back(list[i]);
            }
            SDL_free(list);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FindFilesWithExtensionRecursive: found %d '%s' under %s", (int)files.size(), extension.c_str(), normalized_dir.c_str());
            return files;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_GlobDirectory failed for %s: %s", normalized_dir.c_str(), SDL_GetError());

        // No fallback ? SDL3 is mandatory for this project.
        return files;
    }
}

// SDL Callback Implementation
extern "C" {

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    std::cout << "=== FM2K Rollback Launcher ===\n";
    std::cout << "Initializing with SDL callbacks...\n\n";
    
    // Parse command line arguments for backward compatibility
    NetworkConfig config;
    bool direct_mode = false;
    
    if (argc >= 5) {
        std::cout << "Command line mode detected:\n";
        config.local_player = std::stoi(argv[1]);
        config.local_port = std::stoi(argv[2]);
        config.remote_address = argv[3];
        config.input_delay = std::stoi(argv[4]);
        direct_mode = true;
        
        std::cout << "  Local player: " << config.local_player << std::endl;
        std::cout << "  Local port: " << config.local_port << std::endl;
        std::cout << "  Remote address: " << config.remote_address << std::endl;
        std::cout << "  Input delay: " << config.input_delay << std::endl;
    }
    
    // Create launcher instance
    g_launcher = std::make_unique<FM2KLauncher>();
    
    if (!g_launcher->Initialize()) {
        std::cerr << "Failed to initialize launcher\n";
        return SDL_APP_FAILURE;
    }
    
    // If direct mode, skip UI and go straight to game launch + network
    if (direct_mode) {
        if (g_launcher->GetDiscoveredGames().empty()) {
            std::cerr << "No FM2K games found for direct mode\n";
            return SDL_APP_FAILURE;
        }
        
        // Launch first valid game
        bool game_launched = false;
        for (const auto& game : g_launcher->GetDiscoveredGames()) {
            if (game.is_host && g_launcher->LaunchGame(game)) {
                game_launched = true;
                break;
            }
        }
        
        if (!game_launched) {
            std::cerr << "Failed to launch any FM2K game\n";
            return SDL_APP_FAILURE;
        }
        
        // Start network session
        if (!g_launcher->StartNetworkSession(config)) {
            std::cerr << "Failed to start network session\n";
            return SDL_APP_FAILURE;
        }
        
        g_launcher->SetState(LauncherState::InGame);
        std::cout << "? Direct mode: Game launched and network started\n";
    }
    
    // Store launcher in appstate for other callbacks
    *appstate = g_launcher.get();
    
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }
    
    // Calculate delta time
    static auto last_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    float delta_time = std::chrono::duration<float>(current_time - last_time).count();
    last_time = current_time;
    
    // Update launcher
    launcher->Update(delta_time);
    
    // Render
    launcher->Render();
    
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }
    
    // Let launcher handle the event
    launcher->HandleEvent(event);
    
    // Check for quit
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    
    // Note: async discovery completion is handled inside FM2KLauncher::HandleEvent.

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate SDL_UNUSED, SDL_AppResult result SDL_UNUSED) {
    std::cout << "Shutting down FM2K launcher...\n";
    
    if (g_launcher) {
        // Perform shutdown
        g_launcher->Shutdown();
        g_launcher.reset();
    }
    
    std::cout << "LauncherUI shutdown\n";
}

} // extern "C"

// FM2KLauncher Implementation
FM2KLauncher::FM2KLauncher() 
    : window_(nullptr)
    , renderer_(nullptr)
    , current_state_(LauncherState::GameSelection)
    , running_(true) {
    // Register the custom event type exactly once per process.
    if (g_event_discovery_complete == 0) {
        g_event_discovery_complete = SDL_RegisterEvents(1);
        if (g_event_discovery_complete == (Uint32)-1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to register discovery completion event: %s", SDL_GetError());
        }
    }

    discovery_thread_ = nullptr;
    discovery_in_progress_ = false;
    // Load saved games directory (if any) so it can be used before Initialize() completes.
    games_root_path_ = Utils::LoadGamesRootPath();
}

FM2KLauncher::~FM2KLauncher() {
    Shutdown();
}

bool FM2KLauncher::Initialize() {
    // Set log priorities using SDL_SetLogPriority instead of SDL_LogSetPriority
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_INFO);
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2K Launcher...");

    if (!InitializeSDL()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize SDL3: %s", SDL_GetError());
        return false;
    }
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }
    
    // Create subsystems
    ui_ = std::make_unique<LauncherUI>();
    if (!ui_->Initialize(window_, renderer_)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize UI");
        return false;
    }
    
    // Setup UI callbacks
    ui_->on_game_selected = [this](const FM2K::FM2KGameInfo& game) {
        if (LaunchGame(game)) {
            SetState(LauncherState::Configuration);
        }
    };
    
    ui_->on_network_start = [this](const NetworkConfig& config) {
        network_config_ = config;
        if (StartNetworkSession(config)) {
            SetState(LauncherState::Connecting);
        }
    };
    
    ui_->on_network_stop = [this]() {
        StopNetworkSession();
        SetState(LauncherState::Configuration);
    };
    
    ui_->on_exit = [this]() {
        running_ = false;
    };
    
    ui_->on_games_folder_set = [this](const std::string& folder) {
        SetGamesRootPath(folder);
    };
    
    // If no games directory stored, default to <base>/games before first discovery
    if (games_root_path_.empty()) {
        std::string base_path;
        if (const char *sdl_base = SDL_GetBasePath()) {
            base_path = sdl_base;
            SDL_free(const_cast<char *>(sdl_base));
        } else {
            const char* cwd = SDL_GetCurrentDirectory();
            base_path = cwd ? cwd : "";
            if (cwd) SDL_free(const_cast<char*>(cwd));
        }
        if (!base_path.empty() && base_path.back() != '/' && base_path.back() != '\\') {
            base_path += '/';
        }
        games_root_path_ = base_path + "games";
    }
    
    // Kick-off background discovery so the UI stays responsive. The results
    // will be delivered via the custom SDL event handled in HandleEvent().
    {
        auto cached_games = Utils::LoadGameCache();
        ui_->SetGames(cached_games);
    }
    StartAsyncDiscovery();
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher initialized successfully");
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found %d FM2K games", (int)discovered_games_.size());
    
    return true;
}

void FM2KLauncher::HandleEvent(SDL_Event* event) {
    // Let ImGui handle events first
    ImGui_ImplSDL3_ProcessEvent(event);
    
    // Handle window events for DPI changes
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        if (event->window.windowID == SDL_GetWindowID(window_)) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(event->window.data1);
            io.DisplaySize.y = static_cast<float>(event->window.data2);
        }
    }
    
    // Handle completion of background discovery
    if (event->type == g_event_discovery_complete) {
        auto games_ptr = static_cast<std::vector<FM2K::FM2KGameInfo>*>(event->user.data1);
        if (games_ptr) {
            discovered_games_ = std::move(*games_ptr);
            delete games_ptr;
        }

        discovery_in_progress_ = false;
        if (discovery_thread_) {
            SDL_WaitThread(discovery_thread_, nullptr);
            discovery_thread_ = nullptr;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Async discovery complete: %d games found", (int)discovered_games_.size());
        if (ui_) {
            ui_->SetGames(discovered_games_);
        }
        Utils::SaveGameCache(discovered_games_);
        if (ui_) ui_->SetScanning(false);
        return; // Event handled; skip further processing
    }
    
    // Only process our events if ImGui isn't capturing input
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !io.WantCaptureKeyboard) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
                running_ = false;
            }
        }
    }
}

void FM2KLauncher::Update(float delta_time SDL_UNUSED) {
    if (network_session_) {
        network_session_->Update();
    }
    ui_->NewFrame();
}

void FM2KLauncher::Render() {
    // Clear screen
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    
    // Render UI
    ui_->Render();
    
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    
    SDL_RenderPresent(renderer_);
}

bool FM2KLauncher::InitializeSDL() {
    // Initialize SDL with all necessary subsystems
    SDL_InitFlags init_flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    
    if (!SDL_Init(init_flags)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    
    // Create window with SDL_Renderer graphics context
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    
    window_ = SDL_CreateWindow("FM2K Rollback Launcher", 
        (int)(1280 * main_scale), (int)(720 * main_scale), 
        window_flags);
        
    if (!window_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    SDL_SetRenderVSync(renderer_, 1);
    
    if (!renderer_) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }
    
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    
    return true;
}

void FM2KLauncher::Shutdown() {
    // Stop network and game first
    if (network_session_) {
        network_session_->Stop();
        network_session_.reset();
    }
    
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    
    // Ensure UI cleanup happens before ImGui shutdown
    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }
    
    // SDL cleanup
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    // Make sure discovery thread is finished before quitting SDL
    if (discovery_thread_) {
        SDL_WaitThread(discovery_thread_, nullptr);
        discovery_thread_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    MH_Uninitialize();
}

void FM2KLauncher::StartAsyncDiscovery() {
    // Prevent overlapping scans
    if (discovery_in_progress_) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Discovery already in progress ? ignoring new request");
        return;
    }

    discovery_in_progress_ = true;
    if (ui_) ui_->SetScanning(true);

    // If a previous thread handle exists (shouldn't) ensure it is cleaned up.
    if (discovery_thread_) {
        SDL_WaitThread(discovery_thread_, nullptr);
        discovery_thread_ = nullptr;
    }

    discovery_thread_ = SDL_CreateThread(DiscoveryThreadFunc, "FM2KDiscovery", this);
    if (!discovery_thread_) {
        discovery_in_progress_ = false;
        if (ui_) ui_->SetScanning(false);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread failed: %s", SDL_GetError());
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Started background discovery thread...");
    }
}

std::vector<FM2K::FM2KGameInfo> FM2KLauncher::DiscoverGames() {
    std::vector<FM2K::FM2KGameInfo> games;

    const std::string& games_root = games_root_path_;

    if (games_root.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Games root path is empty; skipping discovery");
        return games;
    }
    if (!SDL_GetPathInfo(games_root.c_str(), nullptr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Games directory does not exist: %s (%s)", games_root.c_str(), SDL_GetError());
        return games;
    }

    // ------------------------------------------------------------------
    // Recursive directory traversal using SDL_EnumerateDirectory
    // ------------------------------------------------------------------

    struct Context {
        std::vector<std::string> pending_dirs;
        std::vector<FM2K::FM2KGameInfo>* out_games;
    } ctx;

    ctx.pending_dirs.push_back(Utils::NormalizePath(games_root));
    ctx.out_games = &games;

    auto callback = [](void* ud, const char* dirname, const char* fname) -> SDL_EnumerationResult {
        Context* c = static_cast<Context*>(ud);
        std::string full_path = std::string(dirname) + fname;

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(full_path.c_str(), &info)) {
            return SDL_ENUM_CONTINUE;
        }

        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            // Skip "." and ".." entries
            if (fname[0] == '.' && (fname[1] == '\0' || (fname[1] == '.' && fname[2] == '\0'))) {
                return SDL_ENUM_CONTINUE;
            }
            // Queue subdirectory for later enumeration
            std::string subdir = full_path;
            if (subdir.back() != '/') subdir.push_back('/');
            c->pending_dirs.push_back(std::move(subdir));
        } else if (info.type == SDL_PATHTYPE_FILE) {
            // Check for .exe files and look for matching .kgt
            size_t len = full_path.size();
            if (len > 4 && SDL_strcasecmp(full_path.c_str() + len - 4, ".exe") == 0) {
                std::string kgt_path = full_path;
                kgt_path.replace(len - 4, 4, ".kgt");
                if (SDL_GetPathInfo(kgt_path.c_str(), nullptr)) {
                    FM2K::FM2KGameInfo game;
                    game.exe_path = full_path;
                    game.dll_path = kgt_path;
                    game.process_id = 0;
                    game.is_host = true;
                    c->out_games->push_back(game);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Discovered FM2K game: %s", fname);
                }
            }
        }
        return SDL_ENUM_CONTINUE;
    };

    while (!ctx.pending_dirs.empty()) {
        std::string dir = std::move(ctx.pending_dirs.back());
        ctx.pending_dirs.pop_back();

        if (!SDL_EnumerateDirectory(dir.c_str(), callback, &ctx)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to enumerate %s: %s", dir.c_str(), SDL_GetError());
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DiscoverGames: %d game(s) found under %s", (int)games.size(), games_root.c_str());
    return games;
}

bool FM2KLauncher::ValidateGameFiles(FM2K::FM2KGameInfo& game) {
    // Basic validation - check if executable exists and is readable
    if (!Utils::FileExists(game.exe_path)) {
        return false;
    }
    
    // TODO: Add more sophisticated validation
    game.is_host = true;
    return true;
}

std::string FM2KLauncher::DetectGameVersion(const std::string& exe_path SDL_UNUSED) {
    // TODO: Implement version detection based on file properties
    return "Unknown";
}

bool FM2KLauncher::LaunchGame(const FM2K::FM2KGameInfo& game) {
    if (!game.is_host) {
        std::cerr << "Cannot launch invalid game\n";
        return false;
    }
    
    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        game_instance_->Terminate();
    }
    
    // Create new game instance
    game_instance_ = std::make_unique<FM2KGameInstance>();
    
    if (!game_instance_->Launch(game)) {
        std::cerr << "Failed to launch game: " << game.exe_path << std::endl;
        game_instance_.reset();
        return false;
    }
    
    std::cout << "? Game launched: " << game.exe_path << std::endl;
    return true;
}

void FM2KLauncher::TerminateGame() {
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
        std::cout << "? Game terminated\n";
    }
}

bool FM2KLauncher::StartNetworkSession(const NetworkConfig& config) {
    if (network_session_ && network_session_->IsActive()) {
        network_session_->Stop();
    }
    
    network_session_ = std::make_unique<NetworkSession>();
    
    // Convert global NetworkConfig to NetworkSession::NetworkConfig
    NetworkSession::NetworkConfig session_config;
    session_config.remote_address = config.remote_address;
    session_config.local_port = static_cast<uint16_t>(config.local_port);
    session_config.remote_port = 7001;  // Default remote port if not specified
    session_config.input_delay = static_cast<uint8_t>(config.input_delay);
    session_config.max_spectators = static_cast<uint8_t>(config.max_spectators);
    
    if (!network_session_->Start(session_config)) {
        std::cerr << "Failed to start network session\n";
        network_session_.reset();
        return false;
    }
    
    std::cout << "? Network session started\n";
    std::cout << "  Local player: " << config.local_player << std::endl;
    std::cout << "  Local port: " << config.local_port << std::endl;
    std::cout << "  Remote address: " << config.remote_address << std::endl;
    
    return true;
}

void FM2KLauncher::StopNetworkSession() {
    if (network_session_) {
        network_session_->Stop();
        network_session_.reset();
        std::cout << "? Network session stopped\n";
    }
}

void FM2KLauncher::SetState(LauncherState state) {
    current_state_ = state;
    if (ui_) {
        ui_->SetLauncherState(state);
    }
}

void FM2KLauncher::SetGamesRootPath(const std::string& path) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Set games root path: %s", path.c_str());
    games_root_path_ = path;
    Utils::SaveGamesRootPath(path);

    // Kick off background discovery so the UI stays responsive
    StartAsyncDiscovery();
} 
