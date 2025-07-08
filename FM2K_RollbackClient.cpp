#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
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
        char* pattern = static_cast<char*>(SDL_malloc(SDL_strlen(extension.c_str()) + 2));
        if (!pattern) return files;
        
        SDL_snprintf(pattern, SDL_strlen(extension.c_str()) + 2, "*%s", extension.c_str());
        char **list = SDL_GlobDirectory(directory.c_str(), pattern, /*flags=*/0, &count);
        SDL_free(pattern);

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
        std::string dir;
        
        if (pref) {
            dir = pref;
            SDL_free(const_cast<char*>(pref));
        } else {
            const char* base = SDL_GetBasePath();
            dir = base ? base : "";
            if (base) SDL_free(const_cast<char*>(base));
        }

        size_t len = SDL_strlen(dir.c_str());
        if (len > 0 && dir[len-1] != '/' && dir[len-1] != '\\') {
            dir.push_back('/');
        }

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

        SDL_IOStream* io = SDL_IOFromFile(cfg.c_str(), "r");
        if (!io) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open config file: %s", cfg.c_str());
            return {};
        }

        char buffer[1024];
        size_t read = SDL_ReadIO(io, buffer, sizeof(buffer) - 1);
        SDL_CloseIO(io);

        if (read > 0) {
            buffer[read] = '\0';
            // Trim newlines
            while (read > 0 && (buffer[read-1] == '\n' || buffer[read-1] == '\r')) {
                buffer[--read] = '\0';
            }
            return std::string(buffer);
        }
        return {};
    }

    void SaveGamesRootPath(const std::string& path) {
        std::string cfg = GetConfigFilePath();
        SDL_IOStream* io = SDL_IOFromFile(cfg.c_str(), "w");
        if (!io) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write config file: %s", cfg.c_str());
            return;
        }

        SDL_WriteIO(io, path.c_str(), SDL_strlen(path.c_str()));
        SDL_WriteIO(io, "\n", 1);
        SDL_CloseIO(io);
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
        char* normalized = static_cast<char*>(SDL_malloc(SDL_strlen(path.c_str()) + 1));
        if (!normalized) return path;
        
        SDL_strlcpy(normalized, path.c_str(), SDL_strlen(path.c_str()) + 1);
        
        // Special handling for Windows drive letters (C:\ etc)
        bool has_drive_letter = SDL_strlen(normalized) >= 2 && 
                              normalized[1] == ':' &&
                              ((normalized[0] >= 'A' && normalized[0] <= 'Z') ||
                               (normalized[0] >= 'a' && normalized[0] <= 'z'));
        
        // Convert all backslashes to forward slashes, except after drive letter
        for (size_t i = (has_drive_letter ? 2 : 0); i < SDL_strlen(normalized); ++i) {
            if (normalized[i] == '\\') normalized[i] = '/';
        }
        
        // Remove any double slashes, but preserve network paths
        char* src = normalized;
        char* dst = normalized;
        bool last_was_slash = false;
        bool is_network_path = SDL_strlen(normalized) >= 2 && 
                             normalized[0] == '/' && normalized[1] == '/';
        
        if (is_network_path) {
            // Copy first two slashes for network paths
            *dst++ = *src++;
            *dst++ = *src++;
        }
        
        while (*src) {
            if (*src == '/') {
                if (!last_was_slash) {
                    *dst++ = *src;
                }
                last_was_slash = true;
            } else {
                *dst++ = *src;
                last_was_slash = false;
            }
            src++;
        }
        *dst = '\0';
        
        std::string result(normalized);
        SDL_free(normalized);
        return result;
    }

    // Recursively search for files that match the provided extension
    inline std::vector<std::string> FindFilesWithExtensionRecursive(const std::string& directory,
                                                                    const std::string& extension) {
        std::vector<std::string> files;
        std::string normalized_dir = NormalizePath(directory);
        
        int count = 0;
        char* pattern = static_cast<char*>(SDL_malloc(SDL_strlen(extension.c_str()) + 2));
        if (!pattern) return files;
        
        SDL_snprintf(pattern, SDL_strlen(extension.c_str()) + 2, "*%s", extension.c_str());
        char **list = SDL_GlobDirectory(normalized_dir.c_str(), pattern, /*flags=*/0, &count);
        SDL_free(pattern);

        if (list) {
            for (int i = 0; i < count; ++i) {
                if (list[i]) files.emplace_back(list[i]);
            }
            SDL_free(list);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FindFilesWithExtensionRecursive: found %d '%s' under %s", 
                        (int)files.size(), extension.c_str(), normalized_dir.c_str());
            return files;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_GlobDirectory failed for %s: %s", 
                    normalized_dir.c_str(), SDL_GetError());
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
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_DEBUG);
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
        ui_->SetGamesRootPath(games_root_path_);  // Update UI with current path
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
    // Remove update spam
    
    // Check game process status and process IPC events
    if (game_instance_ && game_instance_->IsRunning()) {
        static uint32_t last_check_time = 0;
        uint32_t current_time = SDL_GetTicks();
        
        // Remove IPC process spam
        
        // Process IPC events every frame to prevent buffer overflow
        game_instance_->ProcessIPCEvents();
        
        // Check process status every 100ms
        if (current_time - last_check_time > 100) {
            if (!game_instance_->IsRunning()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game process has terminated unexpectedly");
                game_instance_.reset();
            }
            last_check_time = current_time;
        }
    }
    
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

static SDL_EnumerationResult DirectoryEnumerator(void* userdata, const char* origdir, const char* name) {
    auto* games = static_cast<std::vector<FM2K::FM2KGameInfo>*>(userdata);
    char* path = nullptr;
    if (SDL_asprintf(&path, "%s\\%s", origdir, name) < 0 || !path) {
        return SDL_ENUM_FAILURE; // Allocation failed, stop with failure.
    }

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        SDL_free(path);
        return SDL_ENUM_CONTINUE; // Cannot stat, but continue to next.
    }

    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (SDL_strcmp(name, ".") != 0 && SDL_strcmp(name, "..") != 0) {
            // For directories, look for KGT files directly inside
            int count = 0;
            char **list = SDL_GlobDirectory(path, "*.kgt", /*flags=*/0, &count);
            
            if (list) {
                for (int i = 0; i < count; ++i) {
                    if (!list[i]) continue;
                    
                    const char* kgt_name = SDL_strrchr(list[i], '/');
                    if (!kgt_name) kgt_name = SDL_strrchr(list[i], '\\');
                    kgt_name = kgt_name ? kgt_name + 1 : list[i];  // Skip the slash
                    
                    char* exe_path = nullptr;
                    char* kgt_path = nullptr;
                    if (SDL_asprintf(&exe_path, "%s\\%.*s.exe", path,
                                   (int)(SDL_strlen(kgt_name) - 4), // Length without .kgt
                                   kgt_name) < 0 || !exe_path) {
                        continue; // Skip if allocation fails
                    }
                    
                    if (SDL_asprintf(&kgt_path, "%s\\%s", path, kgt_name) < 0 || !kgt_path) {
                        SDL_free(exe_path);
                        continue; // Skip if allocation fails
                    }

                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Found KGT in '%s': '%s', checking for EXE: '%s'", 
                                name, kgt_path, exe_path);

                    if (SDL_GetPathInfo(exe_path, nullptr)) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found valid game pair in '%s': EXE='%s', KGT='%s'", 
                                  name, exe_path, kgt_path);
                        games->emplace_back(FM2K::FM2KGameInfo{exe_path, kgt_path, 0, true});
                    }
                    
                    SDL_free(exe_path);
                    SDL_free(kgt_path);
                }
                SDL_free(list);
            }
        }
    }

    SDL_free(path);
    return SDL_ENUM_CONTINUE; // Continue enumeration
}

static void DiscoverGamesRecursive(const std::string& dir, std::vector<FM2K::FM2KGameInfo>& games) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Scanning directory: '%s'", dir.c_str());
    SDL_EnumerateDirectory(dir.c_str(), DirectoryEnumerator, &games);
}

std::vector<FM2K::FM2KGameInfo> FM2KLauncher::DiscoverGames() {
    std::vector<FM2K::FM2KGameInfo> games;
    const std::string& games_root = games_root_path_;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting game discovery in directory: '%s'", games_root.c_str());

    if (games_root.empty() || !SDL_GetPathInfo(games_root.c_str(), nullptr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Games root path is empty or does not exist: '%s'", games_root.c_str());
        return games;
    }

    DiscoverGamesRecursive(games_root, games);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DiscoverGames: %d game(s) found under '%s'", (int)games.size(), games_root.c_str());
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Attempting to launch game: %s", game.exe_path.c_str());
    
    if (!game.is_host) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot launch invalid game - is_host flag is false");
        return false;
    }
    
    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before new launch");
        game_instance_->Terminate();
    }
    
    // Create new game instance
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating new FM2KGameInstance");
    game_instance_ = std::make_unique<FM2KGameInstance>();
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Launching game with EXE: %s, KGT: %s", 
                 game.exe_path.c_str(), game.dll_path.c_str());
                 
    if (!game_instance_->Launch(game)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game: %s", game.exe_path.c_str());
        game_instance_.reset();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game launched successfully: %s", game.exe_path.c_str());
    
    // Wait a moment and check if process is still running
    SDL_Delay(100);
    if (!game_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game process terminated immediately after launch!");
        game_instance_.reset();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process confirmed running after 100ms");
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
    session_config.session_mode = config.session_mode;
    session_config.remote_address = config.remote_address;
    session_config.local_port = static_cast<uint16_t>(config.local_port);
    session_config.remote_port = 7001;  // Default remote port if not specified
    session_config.input_delay = static_cast<uint8_t>(config.input_delay);
    session_config.max_spectators = static_cast<uint8_t>(config.max_spectators);
    session_config.local_player = config.local_player;
    
    // Connect network session to game instance for state management
    if (game_instance_) {
        network_session_->SetGameInstance(game_instance_.get());
        game_instance_->SetNetworkSession(network_session_.get());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "NetworkSession and GameInstance connected bidirectionally");
    }
    
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
    if (ui_) ui_->SetGamesRootPath(path);  // Update UI with new path

    // Kick off background discovery so the UI stays responsive
    StartAsyncDiscovery();
} 

