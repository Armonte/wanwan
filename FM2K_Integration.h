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

// Forward declarations
class FM2KGameInstance;
class NetworkSession;
class LauncherUI;

// FM2K Memory addresses and structures (from research)
namespace FM2K {
    // Input System
    constexpr DWORD P1_INPUT_ADDR = 0x4259C0;
    constexpr DWORD P2_INPUT_ADDR = 0x4259C4;
    constexpr DWORD P1_INPUT_HISTORY_ADDR = 0x4280E0;
    constexpr DWORD P2_INPUT_HISTORY_ADDR = 0x4290E0;
    constexpr DWORD INPUT_BUFFER_INDEX_ADDR = 0x447EE0;
    
    // Player State
    constexpr DWORD P1_HP_ADDR = 0x4DFC85;
    constexpr DWORD P2_HP_ADDR = 0x4EDCC4;
    constexpr DWORD P1_MAX_HP_ADDR = 0x4DFC91;
    constexpr DWORD P2_MAX_HP_ADDR = 0x4EDCD0;
    constexpr DWORD P1_STAGE_X_ADDR = 0x424E68;
    constexpr DWORD P1_STAGE_Y_ADDR = 0x424E6C;
    
    // Game State
    constexpr DWORD ROUND_TIMER_ADDR = 0x470060;
    constexpr DWORD GAME_TIMER_ADDR = 0x470044;
    constexpr DWORD RANDOM_SEED_ADDR = 0x41FB1C;
    constexpr DWORD OBJECT_POOL_ADDR = 0x4701E0;
    
    // Hook Points
    constexpr DWORD FRAME_HOOK_ADDR = 0x4146D0;
    constexpr DWORD UPDATE_GAME_STATE_ADDR = 0x404CD0;
    
    // Utility functions
    namespace Utils {
        std::vector<std::string> FindFilesWithExtension(const std::string& directory, const std::string& extension);
        bool FileExists(const std::string& path);
        std::string GetFileVersion(const std::string& exe_path);
        uint32_t Fletcher32(const uint16_t* data, size_t len);
        
        // Timing utilities for 100 FPS
        float GetFM2KFrameTime(float frames_ahead);
        std::chrono::milliseconds GetFrameDuration();
    }
    
    // Complete game state structure
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
            uint32_t round_count;
            uint32_t round_state;
            uint32_t action_state;
        } players[2];
        
        // Global game state
        uint32_t round_timer;
        uint32_t game_timer;
        uint32_t hit_effect_timer;
        
        // Input system state
        uint32_t input_repeat_timer[8];
        
        // Critical object pool state (simplified)
        struct GameObject {
            uint32_t object_type;
            int32_t pos_x, pos_y;
            int32_t vel_x, vel_y;
            uint16_t state_flags;
            uint16_t animation_frame;
            uint16_t facing_direction;
        } character_objects[2];
        
        uint32_t CalculateChecksum() const {
            return Utils::Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(*this));
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
}

// Game discovery and management
struct FM2KGameInfo {
    std::string name;
    std::string exe_path;
    std::string kgt_path;
    std::string version;
    bool validated = false;
    
    bool is_valid() const {
        return !exe_path.empty() && !kgt_path.empty() && validated;
    }
};

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
    
    // Event handling for SDL callbacks
    void HandleEvent(SDL_Event* event);
    void Update(float delta_time);
    void Render();
    
    // Game management
    std::vector<FM2KGameInfo> DiscoverGames();
    bool LaunchGame(const FM2KGameInfo& game);
    void TerminateGame();
    
    // Network management
    bool StartNetworkSession(const NetworkConfig& config);
    void StopNetworkSession();
    
    // State management
    LauncherState GetState() const { return current_state_; }
    void SetState(LauncherState state) { current_state_ = state; }
    
private:
    // Core systems
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::unique_ptr<NetworkSession> network_session_;
    
    // State
    LauncherState current_state_;
    std::vector<FM2KGameInfo> discovered_games_;
    NetworkConfig network_config_;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    bool running_;
    
    // Internal methods
    bool InitializeSDL();
    bool InitializeImGui();
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
};

// Game instance management
class FM2KGameInstance {
public:
    FM2KGameInstance();
    ~FM2KGameInstance();
    
    bool Initialize();
    bool Launch(const FM2KGameInfo& game);
    void Terminate();
    bool IsRunning() const { return process_handle_ != nullptr; }
    
    // Memory access
    template<typename T>
    inline bool ReadMemory(DWORD address, T* value) {
        if (!process_handle_ || !value) return false;
        
        SIZE_T bytes_read;
        return ReadProcessMemory(process_handle_, (LPVOID)address, value, sizeof(T), &bytes_read) 
               && bytes_read == sizeof(T);
    }
    
    template<typename T>
    inline bool WriteMemory(DWORD address, const T* value) {
        if (!process_handle_ || !value) return false;
        
        SIZE_T bytes_written;
        return WriteProcessMemory(process_handle_, (LPVOID)address, value, sizeof(T), &bytes_written) 
               && bytes_written == sizeof(T);
    }
    
    // Hook management
    bool InstallHooks();
    void RemoveHooks();
    
    // State management
    bool SaveState(void* buffer, size_t buffer_size);
    bool LoadState(const void* buffer, size_t buffer_size);
    
    // Input injection
    void InjectInputs(uint32_t p1_input, uint32_t p2_input);
    
    HANDLE GetProcessHandle() const { return process_handle_; }
    DWORD GetProcessId() const { return process_id_; }
    
private:
    HANDLE process_handle_;
    DWORD process_id_;
    PROCESS_INFORMATION process_info_;
    std::vector<void*> installed_hooks_;
    std::unique_ptr<FM2K::GameState> game_state_;
    
    bool SetupProcessForHooking();
    bool LoadGameExecutable(const std::filesystem::path& exe_path);
    static DWORD WINAPI ProcessMonitorThread(LPVOID param);
};

// Network session management
class NetworkSession {
public:
    NetworkSession();
    ~NetworkSession();
    
    bool Start(const NetworkConfig& config);
    void Stop();
    bool IsActive() const { return session_ != nullptr; }
    
    // GekkoNet integration
    void Update();
    void AddLocalInput(uint32_t input);
    void ProcessEvents(FM2KGameInstance* game);
    
    // Statistics
    struct NetworkStats {
        float ping = 0.0f;
        float jitter = 0.0f;
        int rollbacks_per_second = 0;
        float frames_ahead = 0.0f;
        bool connected = false;
    };
    
    NetworkStats GetStats() const;
    
private:
    GekkoSession* session_;
    NetworkConfig config_;
    int local_player_handle_;
    FM2KGameInstance* game_instance_;
    
    // SDL3 synchronization primitives
    SDL_Mutex* state_mutex_;           // Protects game state during rollback
    SDL_RWLock* input_buffer_lock_;    // Protects input history buffer
    SDL_Thread* rollback_thread_;      // Dedicated rollback thread
    SDL_Thread* network_thread_;       // Network processing thread
    SDL_AtomicInt frame_counter_;      // Current frame number
    SDL_AtomicInt rollback_flag_;      // Signals when rollback is needed
    SDL_AtomicInt running_;            // Thread control flag
    
    // Timing and synchronization
    SDL_AtomicInt last_confirmed_frame_;  // Last frame confirmed by remote
    SDL_AtomicInt prediction_window_;     // Number of frames to predict ahead
    
    // State management
    std::vector<FM2K::GameState> state_buffer_;  // Circular buffer of game states
    static const int STATE_BUFFER_SIZE = 128;    // Store 128 frames (~1.28 seconds at 100fps)
    
    // Statistics tracking
    mutable NetworkStats cached_stats_;
    std::chrono::steady_clock::time_point last_stats_update_;
    
    // Thread functions
    static int RollbackThreadFunction(void* data);
    static int NetworkThreadFunction(void* data);
    
    // Internal methods
    void HandleSessionEvents();
    void HandleGameEvents(FM2KGameInstance* game);
    void HandleGameEvent(GekkoGameEvent* ev);
    void HandleSessionEvent(GekkoSessionEvent* ev);
    
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
    std::function<void(const FM2KGameInfo&)> on_game_selected;
    std::function<void(const NetworkConfig&)> on_network_start;
    std::function<void()> on_network_stop;
    std::function<void()> on_exit;
    
    // Data binding
    void SetGames(const std::vector<FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetNetworkStats(const NetworkSession::NetworkStats& stats);
    void SetLauncherState(LauncherState state);
    
private:
    // UI state
    std::vector<FM2KGameInfo> games_;
    NetworkConfig network_config_;
    NetworkSession::NetworkStats network_stats_;
    LauncherState launcher_state_;
    
    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    
    // Helper methods
    void ShowGameValidationStatus(const FM2KGameInfo& game);
    void ShowNetworkDiagnostics();
    bool ValidateNetworkConfig();
}; 