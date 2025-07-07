#pragma once

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "MinHook.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include "FM2KHook/src/ipc.h"
#include <unordered_map>

// Forward declarations
class FM2KGameInstance;
class NetworkSession;
class LauncherUI;

// FM2K Memory addresses and structures (from research)
namespace FM2K {
    // Memory addresses for critical game state
    constexpr uintptr_t INPUT_BUFFER_INDEX_ADDR = 0x470000;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    
    // Player state addresses
    constexpr uintptr_t P1_INPUT_ADDR = 0x470100;
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x470104;
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x470108;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;
    constexpr uintptr_t P1_MAX_HP_ADDR = 0x470110;
    constexpr uintptr_t P1_INPUT_HISTORY_ADDR = 0x470200;
    
    constexpr uintptr_t P2_INPUT_ADDR = 0x470300;
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;
    constexpr uintptr_t P2_MAX_HP_ADDR = 0x470310;
    constexpr uintptr_t P2_INPUT_HISTORY_ADDR = 0x470400;
    
    // Global state addresses
    constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
    constexpr uintptr_t GAME_TIMER_ADDR = 0x470064;

    // Sprite effect system addresses
    constexpr uintptr_t EFFECT_ACTIVE_FLAGS = 0x40CC30;  // Bitfield of active effects
    constexpr uintptr_t EFFECT_TIMERS_BASE = 0x40CC34;   // Array of 8 effect timers
    constexpr uintptr_t EFFECT_COLORS_BASE = 0x40CC54;   // Array of 8 RGB color sets
    constexpr uintptr_t EFFECT_TARGETS_BASE = 0x40CCD4;  // Array of 8 target IDs
    
    // Game State
    constexpr DWORD OBJECT_POOL_ADDR = 0x4701E0;
    
    // Hook Points
    constexpr DWORD FRAME_HOOK_ADDR = 0x4146D0;
    constexpr DWORD UPDATE_GAME_STATE_ADDR = 0x404CD0;
    
    // Game instance info
    struct FM2KGameInfo {
        std::string exe_path;
        std::string dll_path;
        uint32_t process_id;
        bool is_host;
    };

    // Utility functions
    bool FileExists(const std::string& path);
    uint32_t Fletcher32(const uint16_t* data, size_t len);
    float GetFM2KFrameTime(float frames_ahead);
    std::chrono::milliseconds GetFrameDuration();

    struct GameState {
        // Frame and timing state
        uint32_t frame_number;
        uint32_t input_buffer_index;
        uint32_t last_frame_time;
        uint32_t frame_time_delta;
        uint8_t frame_skip_count;
        uint8_t frame_sync_flag;
        
        // Random number generator state
        uint32_t random_seed;
        
        // Player states
        struct PlayerState {
            uint32_t input_current;
            uint32_t input_history[1024];
            uint32_t stage_x, stage_y;
            uint32_t hp, max_hp;
            uint32_t meter, max_meter;
            uint32_t combo_counter;
            uint32_t hitstun_timer;
            uint32_t blockstun_timer;
            uint32_t anim_timer;
            uint32_t move_id;
            uint32_t state_flags;
        } players[2];

        // Visual effects state
        IPC::VisualState visual_state;

        // Hit detection tables
        struct HitBox {
            int32_t x, y, w, h;
            uint32_t type;
            uint32_t damage;
            uint32_t flags;
        };
        
        static constexpr size_t MAX_HITBOXES = 32;
        HitBox hit_boxes[MAX_HITBOXES];
        uint32_t hit_box_count;

        // Calculate state checksum for rollback verification
        uint32_t CalculateChecksum() const {
            return Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(GameState));
        }
    };

    // Input structure (11-bit input mask)
    struct Input {
        union {
            struct {
                uint16_t left     : 1;
                uint16_t right    : 1;
                uint16_t up       : 1;
                uint16_t down     : 1;
                uint16_t button1  : 1;
                uint16_t button2  : 1;
                uint16_t button3  : 1;
                uint16_t button4  : 1;
                uint16_t button5  : 1;
                uint16_t button6  : 1;
                uint16_t button7  : 1;
                uint16_t reserved : 5;
            } bits;
            uint16_t value;
        };
    };

    // Memory access functions
    template<typename T>
    bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
        SIZE_T bytes_read;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), 
            &value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
    }

    template<typename T>
    bool WriteMemory(HANDLE process, uintptr_t address, const T& value) {
        SIZE_T bytes_written;
        return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address),
            &value, sizeof(T), &bytes_written) && bytes_written == sizeof(T);
    }
}

// Launcher states
enum class LauncherState {
    GameSelection,      // Selecting which FM2K game to launch
    Configuration,      // Setting up network/input options
    Connecting,         // Establishing network connection
    InGame,            // Game running with rollback active
    Disconnected       // Connection lost, can reconnect
};

// Network configuration
struct NetworkConfig {
    std::string local_address = "127.0.0.1";
    int local_port = 7000;
    std::string remote_address = "127.0.0.1:7001";
    int local_player = 0;  // 0 or 1
    int input_delay = 2;
    int max_spectators = 8;
    bool enable_spectators = true;
};

// Main launcher class
class FM2KLauncher {
public:
    FM2KLauncher();
    ~FM2KLauncher();
    
    bool Initialize();
    void Shutdown();
    void Update(float delta_time);
    void Render();
    void HandleEvent(SDL_Event* event);
    
    bool LaunchGame(const FM2K::FM2KGameInfo& game);
    void TerminateGame();
    bool StartNetworkSession(const NetworkConfig& config);
    void StopNetworkSession();
    
    std::vector<FM2K::FM2KGameInfo> DiscoverGames();
    const std::vector<FM2K::FM2KGameInfo>& GetDiscoveredGames() const { return discovered_games_; }
    
    void SetState(LauncherState state);
    bool IsRunning() const { return running_; }
    
    // Games directory management
    const std::string& GetGamesRootPath() const { return games_root_path_; }
    void SetGamesRootPath(const std::string& path);
    
    // ----- Asynchronous game discovery -----
    SDL_Thread* discovery_thread_ = nullptr; // Worker thread handle
    bool discovery_in_progress_ = false;     // Flag so we don't launch multiple scans

    // Starts a background SDL thread that will run DiscoverGames() and notify the main
    // thread when done. Implemented in FM2K_RollbackClient.cpp.
    void StartAsyncDiscovery();
    
    // Scan progress accessors for UI
    void SetScanning(bool scanning);

private:
    bool InitializeSDL();
    bool InitializeImGui();
    
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::unique_ptr<NetworkSession> network_session_;
    std::vector<FM2K::FM2KGameInfo> discovered_games_;
    NetworkConfig network_config_;
    LauncherState current_state_;
    bool running_;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2K::FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
    
    // Games directory (root where FM2K games are located)
    std::string games_root_path_;
};

// Game instance management
class FM2KGameInstance {
public:
    FM2KGameInstance();
    ~FM2KGameInstance();
    
    bool Initialize();
    bool Launch(const FM2K::FM2KGameInfo& game);
    void Terminate();
    bool IsRunning() const { return process_handle_ != nullptr; }
    
    // Memory access
    template<typename T>
    inline bool ReadMemory(DWORD address, T* value) {
        if (!process_handle_ || !value) return false;
        return FM2K::ReadMemory(process_handle_, static_cast<uintptr_t>(address), *value);
    }
    
    template<typename T>
    inline bool WriteMemory(DWORD address, const T* value) {
        if (!process_handle_ || !value) return false;
        return FM2K::WriteMemory(process_handle_, static_cast<uintptr_t>(address), *value);
    }
    
    // Hook management
    bool InstallHooks();
    bool UninstallHooks();
    
    // State management
    bool SaveState(void* buffer, size_t buffer_size);
    bool LoadState(const void* buffer, size_t buffer_size);
    bool AdvanceFrame();
    
    // Input injection
    void InjectInputs(uint32_t p1_input, uint32_t p2_input);
    
    // Event handlers
    void OnFrameAdvanced(const FM2K::IPC::Event& event);
    void OnStateSaved(const FM2K::IPC::Event& event);
    void OnStateLoaded(const FM2K::IPC::Event& event);
    void OnHitTablesInit(const FM2K::IPC::Event& event);
    void OnVisualStateChanged(const FM2K::IPC::Event& event);
    void OnHookError(const FM2K::IPC::Event& event);

protected:
    // Process management
    bool SetupProcessForHooking();
    bool LoadGameExecutable(const std::filesystem::path& exe_path);
    void ProcessIPCEvents();

private:
    HANDLE process_handle_;
    DWORD process_id_;
    PROCESS_INFORMATION process_info_;
    std::unique_ptr<FM2K::GameState> game_state_;
};

// Network session management
class NetworkSession {
public:
    NetworkSession();
    ~NetworkSession();
    
    // Network configuration
    struct NetworkConfig {
        std::string remote_address;
        uint16_t local_port;
        uint16_t remote_port;
        uint8_t input_delay;
        uint8_t max_spectators;
    };
    
    // Network statistics
    struct NetworkStats {
        uint32_t ping;
        uint32_t jitter;
        uint32_t frames_ahead;
        uint32_t rollbacks_per_second;
        bool connected;
        
        NetworkStats()
            : ping(0)
            , jitter(0)
            , frames_ahead(0)
            , rollbacks_per_second(0)
            , connected(false)
        {}
    };
    
    // Session management
    bool Start(const NetworkConfig& config);
    void Stop();
    void Update();
    bool IsActive() const { return session_ != nullptr; }
    
    // Input management
    void AddLocalInput(uint32_t input);
    NetworkStats GetStats() const;
    
    // Game instance binding
    void SetGameInstance(FM2KGameInstance* instance) {
        game_instance_ = instance;
    }
    
private:
    // GekkoNet session
    GekkoSession* session_;
    int local_player_handle_;
    FM2KGameInstance* game_instance_;
    
    // Synchronization
    SDL_Mutex* state_mutex_;
    SDL_RWLock* input_buffer_lock_;
    SDL_Thread* rollback_thread_;
    SDL_Thread* network_thread_;
    
    // State tracking
    SDL_AtomicInt frame_counter_;
    SDL_AtomicInt rollback_flag_;
    SDL_AtomicInt running_;
    SDL_AtomicInt last_confirmed_frame_;
    SDL_AtomicInt prediction_window_;
    NetworkStats cached_stats_;
    
    // State buffer for rollbacks
    static constexpr size_t STATE_BUFFER_SIZE = 128;
    std::vector<uint8_t> state_buffer_;
    std::unordered_map<int, std::vector<uint8_t>> saved_states_;  // Frame number -> State data
    
    // Thread functions
    static int RollbackThreadFunction(void* data);
    static int NetworkThreadFunction(void* data);
    
    // Internal methods
    void HandleSessionEvents();
    void HandleGameEvents(FM2KGameInstance* game);
    void HandleGameEvent(GekkoGameEvent* ev);
    void HandleSessionEvent(GekkoSessionEvent* ev);
    void ProcessEvents(FM2KGameInstance* game);
    
    // Rollback management
    bool SaveGameState(int frame_number);
    bool LoadGameState(int frame_number);
    void ProcessRollback(int target_frame);
    
    // Frame management
    void AdvanceFrame();
    void UpdatePredictionWindow();
    bool ShouldRollback(uint32_t remote_input, int frame_number);
};

// Modern ImGui launcher interface
class LauncherUI {
public:
    LauncherUI();
    ~LauncherUI();
    
    bool Initialize(SDL_Window* window, SDL_Renderer* renderer);
    void Shutdown();
    
    void NewFrame();
    void Render();
    
    // UI state callbacks
    std::function<void(const FM2K::FM2KGameInfo&)> on_game_selected;
    std::function<void(const NetworkConfig&)> on_network_start;
    std::function<void()> on_network_stop;
    std::function<void()> on_exit;
    std::function<void(const std::string&)> on_games_folder_set;
    
    // Data binding
    void SetGames(const std::vector<FM2K::FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetNetworkStats(const NetworkSession::NetworkStats& stats);
    void SetLauncherState(LauncherState state);
    // Update scanning progress (0-1). Only meaningful while scanning flag is true.
    void SetScanning(bool scanning);

private:
    // UI state
    std::vector<FM2K::FM2KGameInfo> games_;
    NetworkConfig network_config_;
    NetworkSession::NetworkStats network_stats_;
    LauncherState launcher_state_;
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    
    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    
    // Helper methods
    void ShowGameValidationStatus(const FM2K::FM2KGameInfo& game);
    void ShowNetworkDiagnostics();
    bool ValidateNetworkConfig();
    
    bool scanning_games_ = false;  // True while background discovery is running
}; 