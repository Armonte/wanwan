#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#define GEKKONET_STATIC
#include "gekkonet.h"
#include "MinHook.h"
#include "FM2K_Integration.h"

#include <chrono>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <filesystem>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>



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
        std::vector<std::string> files;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (entry.path().extension() == extension) {
                    files.push_back(entry.path().string());
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error scanning directory: " << e.what() << std::endl;
        }
        return files;
    }
    
    bool FileExists(const std::string& path) {
        return std::filesystem::exists(path);
    }
    
    std::string GetFileVersion(const std::string& exe_path) {
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
        auto games = g_launcher->DiscoverGames();
        if (games.empty()) {
            std::cerr << "No FM2K games found for direct mode\n";
            return SDL_APP_FAILURE;
        }
        
        // Launch first valid game
        bool game_launched = false;
        for (const auto& game : games) {
            if (game.is_valid() && g_launcher->LaunchGame(game)) {
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
    
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    std::cout << "Shutting down FM2K launcher...\n";
    
    if (g_launcher) {
        g_launcher->Shutdown();
        g_launcher.reset();
    }
    
    std::cout << "? Launcher shutdown complete\n";
}

} // extern "C"

// FM2KLauncher Implementation
FM2KLauncher::FM2KLauncher() 
    : window_(nullptr)
    , renderer_(nullptr)
    , current_state_(LauncherState::GameSelection)
    , running_(true) {
}

FM2KLauncher::~FM2KLauncher() {
    Shutdown();
}

bool FM2KLauncher::Initialize() {
    if (!InitializeSDL()) {
        std::cerr << "Failed to initialize SDL3\n";
        return false;
    }
    
    if (!InitializeImGui()) {
        std::cerr << "Failed to initialize ImGui\n";
        return false;
    }
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        std::cerr << "Failed to initialize MinHook\n";
        return false;
    }
    
    // Create subsystems
    ui_ = std::make_unique<LauncherUI>();
    if (!ui_->Initialize(window_, renderer_)) {
        std::cerr << "Failed to initialize UI\n";
        return false;
    }
    
    // Setup UI callbacks
    ui_->on_game_selected = [this](const FM2KGameInfo& game) {
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
    
    // Discover available games
    discovered_games_ = DiscoverGames();
    ui_->SetGames(discovered_games_);
    
    std::cout << "? Launcher initialized successfully\n";
    std::cout << "? Found " << discovered_games_.size() << " FM2K games\n";
    
    return true;
}

void FM2KLauncher::HandleEvent(SDL_Event* event) {
    if (event->type == SDL_EVENT_KEY_DOWN) {
        if (event->key.key == SDLK_ESCAPE) {
            running_ = false;
        }
    }
}

void FM2KLauncher::Update(float delta_time) {
    if (network_session_) {
        network_session_->Update();
    }
    ui_->NewFrame();
    ui_->Render();
}

void FM2KLauncher::Render() {
    // Start ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    
    // Render UI
    ui_->Render();
    
    // Render to screen
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    
    SDL_RenderPresent(renderer_);
}

bool FM2KLauncher::InitializeSDL() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init Error: %s\n", SDL_GetError());
        return false;
    }
    
    window_ = SDL_CreateWindow(
        "FM2K Rollback Launcher",
        800, 600,
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window_) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    return true;
}

bool FM2KLauncher::InitializeImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    ImGui::StyleColorsDark();
    
    if (!ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_)) {
        return false;
    }
    
    if (!ImGui_ImplSDLRenderer3_Init(renderer_)) {
        return false;
    }
    
    return true;
}

void FM2KLauncher::Shutdown() {
    if (network_session_) {
        network_session_->Stop();
        network_session_.reset();
    }
    
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    
    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }
    
    if (renderer_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    MH_Uninitialize();
}

std::vector<FM2KGameInfo> FM2KLauncher::DiscoverGames() {
    std::vector<FM2KGameInfo> games;
    
    // Look for .kgt files with matching .exe files
    auto kgt_files = Utils::FindFilesWithExtension(".", ".kgt");
    for (const auto& kgt_path : kgt_files) {
        FM2KGameInfo game;
        game.kgt_path = kgt_path;
        
        // Look for matching exe file
        auto exe_path = std::filesystem::path(kgt_path);
        exe_path.replace_extension(".exe");
        
        if (Utils::FileExists(exe_path.string())) {
            game.exe_path = exe_path.string();
            game.name = exe_path.stem().string();
            game.version = Utils::GetFileVersion(game.exe_path);
            
            if (ValidateGameFiles(game)) {
                games.push_back(game);
                std::cout << "? Found FM2K game: " << game.name << std::endl;
            }
        }
    }
    
    // Also look for standalone executables
    auto exe_files = Utils::FindFilesWithExtension(".", ".exe");
    for (const auto& exe_path : exe_files) {
        // Skip if already found via .kgt
        bool already_found = false;
        for (const auto& game : games) {
            if (game.exe_path == exe_path) {
                already_found = true;
                break;
            }
        }
        
        if (!already_found) {
            FM2KGameInfo game;
            game.exe_path = exe_path;
            game.name = std::filesystem::path(exe_path).stem().string();
            game.version = Utils::GetFileVersion(game.exe_path);
            
            if (ValidateGameFiles(game)) {
                games.push_back(game);
                std::cout << "? Found FM2K executable: " << game.name << std::endl;
            }
        }
    }
    
    return games;
}

bool FM2KLauncher::ValidateGameFiles(FM2KGameInfo& game) {
    // Basic validation - check if executable exists and is readable
    if (!Utils::FileExists(game.exe_path)) {
        return false;
    }
    
    // TODO: Add more sophisticated validation
    // - Check PE header for expected characteristics
    // - Look for FM2K-specific strings or patterns
    // - Verify file size is reasonable
    
    game.validated = true;
    return true;
}

std::string FM2KLauncher::DetectGameVersion(const std::string& exe_path) {
    // TODO: Implement version detection based on file properties
    return "Unknown";
}

bool FM2KLauncher::LaunchGame(const FM2KGameInfo& game) {
    if (!game.is_valid()) {
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
        std::cerr << "Failed to launch game: " << game.name << std::endl;
        game_instance_.reset();
        return false;
    }
    
    std::cout << "? Game launched: " << game.name << std::endl;
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
    
    if (!network_session_->Start(config)) {
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

// Template implementations for memory access
template<typename T>
bool FM2KGameInstance::ReadMemory(DWORD address, T* value) {
    if (!process_handle_ || !value) return false;
    
    SIZE_T bytes_read;
    return ReadProcessMemory(process_handle_, (LPVOID)address, value, sizeof(T), &bytes_read) 
           && bytes_read == sizeof(T);
}

template<typename T>
bool FM2KGameInstance::WriteMemory(DWORD address, const T* value) {
    if (!process_handle_ || !value) return false;
    
    SIZE_T bytes_written;
    return WriteProcessMemory(process_handle_, (LPVOID)address, value, sizeof(T), &bytes_written) 
           && bytes_written == sizeof(T);
}

// Move CalculateChecksum implementation to after class definitions
uint32_t FM2KGameState::CalculateChecksum() const {
    return Utils::Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(*this));
} 