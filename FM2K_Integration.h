#pragma once

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "MinHook.h"
#include "ISession.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <unordered_map>

// Forward declarations
class FM2KGameInstance;
class LauncherUI;

// FM2K Memory addresses and structures (from research)
namespace FM2K {
    // Forward declarations for FM2K namespace classes
    class GekkoNetBridge;
    
    // Memory addresses for critical game state
    constexpr uintptr_t INPUT_BUFFER_INDEX_ADDR = 0x470000;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    
    // Player state addresses (working addresses for this game version)
    constexpr uintptr_t P1_INPUT_ADDR = 0x470100;          // Current input state (working)
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x470104;
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x470108;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;
    constexpr uintptr_t P1_MAX_HP_ADDR = 0x470110;
    constexpr uintptr_t P1_INPUT_HISTORY_ADDR = 0x470200;  // Input history buffer
    
    constexpr uintptr_t P2_INPUT_ADDR = 0x470300;          // Current input state (working)
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

        // Helper to get just the filename from the full path
        std::string GetExeName() const {
            const char* path = exe_path.c_str();
            const char* last_slash = SDL_strrchr(path, '/');
            if (!last_slash) last_slash = SDL_strrchr(path, '\\');
            return last_slash ? std::string(last_slash + 1) : exe_path;
        }

        // Helper to get the directory containing the exe
        std::string GetExeDir() const {
            const char* path = exe_path.c_str();
            char* dir = SDL_strdup(path);
            if (!dir) return "";

            char* last_slash = SDL_strrchr(dir, '/');
            if (!last_slash) last_slash = SDL_strrchr(dir, '\\');
            if (last_slash) {
                *last_slash = '\0';
                std::string result(dir);
                SDL_free(dir);
                return result;
            }
            SDL_free(dir);
            return "";
        }
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

        // Visual effects state (simplified - no IPC)
        uint32_t visual_state_flags;

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
    Connecting,         // Establishing network connection
    InGame,            // Game running with rollback active
    Disconnected       // Connection lost, can reconnect
};

// Network configuration
struct NetworkConfig {
    SessionMode session_mode;
    int local_port;
    std::string remote_address;
    int input_delay;
    bool is_host; // True if hosting, false if joining

    NetworkConfig() 
        : session_mode(SessionMode::LOCAL)  // Default to LOCAL for testing
        , local_port(7000)
        , remote_address("127.0.0.1:7001")
        , input_delay(2)
        , is_host(false)
    {
        // Use SDL string functions for initialization
        char remote_addr[32];
        SDL_strlcpy(remote_addr, "127.0.0.1:7001", sizeof(remote_addr));
        remote_address = remote_addr;
    }
};

// Rollback performance statistics (standalone, used by both launcher and UI)
struct RollbackStats {
    uint32_t rollbacks_per_second; // Current rollback frequency
    uint32_t max_rollback_frames;  // Maximum rollback distance
    uint32_t avg_rollback_frames;  // Average rollback distance
    float frame_advantage;         // Current frame advantage
    uint32_t input_delay_frames;   // Current input delay
    uint32_t confirmed_frames;     // Number of confirmed frames
    uint32_t speculative_frames;   // Number of speculative frames
};

// Forward declaration
class LauncherUI;

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

    void StartOfflineSession();
    void StartOnlineSession(const NetworkConfig& config, bool is_host);
    void StopSession();
    
    std::vector<FM2K::FM2KGameInfo> DiscoverGames();
    const std::vector<FM2K::FM2KGameInfo>& GetDiscoveredGames() const { return discovered_games_; }
    
    void SetState(LauncherState state);
    bool IsRunning() const { return running_; }
    void SetRunning(bool running) { running_ = running; }
    
    // Games directory management
    const std::string& GetGamesRootPath() const { return games_root_path_; }
    void SetGamesRootPath(const std::string& path);
    void SetSelectedGame(const FM2K::FM2KGameInfo& game);
    
    // ----- Asynchronous game discovery -----
    SDL_Thread* discovery_thread_ = nullptr; // Worker thread handle
    bool discovery_in_progress_ = false;     // Flag so we don't launch multiple scans

    // Starts a background SDL thread that will run DiscoverGames() and notify the main
    // thread when done. Implemented in FM2K_RollbackClient.cpp.
    void StartAsyncDiscovery();
    
    // Scan progress accessors for UI
    void SetScanning(bool scanning);

    SDL_Window* GetWindow() const { return window_; }

private:
    bool InitializeSDL();
    bool InitializeImGui();
    
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::vector<FM2K::FM2KGameInfo> discovered_games_;
    
    // Multi-client testing instances
    std::unique_ptr<FM2KGameInstance> client1_instance_;
    std::unique_ptr<FM2KGameInstance> client2_instance_;
    FM2K::FM2KGameInfo selected_game_;
    NetworkConfig network_config_;
    LauncherState current_state_;
    bool running_;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2K::FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
    
    // Multi-client testing helpers
    bool LaunchLocalClient(const std::string& game_path, bool is_host, int port);
    bool TerminateAllClients();
    
    // GekkoNet session management
    bool InitializeGekkoSession();
    void ShutdownGekkoSession();
    bool StartLocalSession();
    void StopLocalSession();
    
    // Multi-client testing
    uint32_t client1_process_id_;
    uint32_t client2_process_id_;
    
    // GekkoNet session management
    GekkoSession* gekko_session_;
    GekkoConfig gekko_config_;
    bool gekko_initialized_;
    
    // Games directory (root where FM2K games are located)
    std::string games_root_path_;
    
    // Helper method to read rollback statistics from hook shared memory  
    bool ReadRollbackStatsFromSharedMemory(RollbackStats& stats);
};

// Game instance management - see FM2K_GameInstance.h for full definition

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
    std::function<void()> on_offline_session_start;
    std::function<void(const NetworkConfig&)> on_online_session_start;
    std::function<void()> on_session_stop;
    std::function<void()> on_exit;
    std::function<void(const std::string&)> on_games_folder_set;
    
    // Debug state callbacks
    std::function<bool()> on_debug_save_state;
    std::function<bool()> on_debug_load_state;
    std::function<bool(uint32_t)> on_debug_force_rollback;
    
    // Slot-based save/load callbacks
    std::function<bool(uint32_t)> on_debug_save_to_slot;
    std::function<bool(uint32_t)> on_debug_load_from_slot;
    std::function<bool(bool, uint32_t)> on_debug_auto_save_config;  // (enabled, interval_frames)
    
    // Slot status callback
    struct SlotStatusInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
    };
    std::function<bool(uint32_t, SlotStatusInfo&)> on_get_slot_status;  // (slot, status_out)
    
    // Auto-save configuration callbacks
    struct AutoSaveConfig {
        bool enabled;
        uint32_t interval_frames;
    };
    std::function<bool(AutoSaveConfig&)> on_get_auto_save_config;  // Get current auto-save settings
    
    // Multi-client testing data structures
    struct NetworkStats {
        uint32_t ping_ms;              // Current ping in milliseconds
        float packet_loss_rate;        // Packet loss rate (0.0-1.0)
        uint32_t bytes_sent;           // Total bytes sent
        uint32_t bytes_received;       // Total bytes received
        uint32_t packets_sent;         // Total packets sent
        uint32_t packets_received;     // Total packets received
        uint32_t connection_quality;   // Connection quality (0-100)
    };
    
    // RollbackStats is now defined at global scope
    
    // Save state profile callback
    enum class SaveStateProfile : uint32_t {
        MINIMAL = 0,    // ~50KB - Core state + active objects only
        STANDARD = 1,   // ~200KB - Essential runtime state  
        COMPLETE = 2    // ~850KB - Everything (current implementation)
    };
    std::function<bool(SaveStateProfile)> on_set_save_profile;  // Set save state profile
    
    // ======= Multi-Client Testing Infrastructure =======
    
    // Multi-client process management  
    std::function<bool(const std::string&)> on_launch_local_client1;     // Launch first client as host
    std::function<bool(const std::string&)> on_launch_local_client2;     // Launch second client as guest
    std::function<bool()> on_terminate_all_clients;                      // Kill all launched clients
    std::function<bool(uint32_t&, uint32_t&)> on_get_client_status;      // Get client process IDs (client1_pid, client2_pid)
    
    // Network simulation and testing
    std::function<bool(uint32_t)> on_set_simulated_latency;              // Set artificial latency in ms
    std::function<bool(float)> on_set_packet_loss_rate;                  // Set packet loss percentage (0.0-1.0)
    std::function<bool(uint32_t)> on_set_jitter_variance;                // Set latency jitter variance in ms
    
    // Network and rollback monitoring  
    std::function<bool(NetworkStats&)> on_get_network_stats;             // Get real-time network statistics
    std::function<bool(RollbackStats&)> on_get_rollback_stats;           // Get rollback performance data
    
    // Data binding
    void SetGames(const std::vector<FM2K::FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetNetworkStats(const GekkoNetworkStats& stats);
    void SetLauncherState(LauncherState state);
    void SetFramesAhead(float frames_ahead);
    // Update scanning progress (0-1). Only meaningful while scanning flag is true.
    void SetScanning(bool scanning);
    void SetGamesRootPath(const std::string& path);

private:
    // Logging
    void AddLog(const char* message);
    void ClearLog();
    static void SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message);

    // UI state
    std::vector<FM2K::FM2KGameInfo> games_;
    NetworkConfig network_config_;
    GekkoNetworkStats network_stats_;
    float frames_ahead_;
    LauncherState launcher_state_;
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    std::string games_root_path_;  // Current games root directory
    int selected_game_index_ = -1; // -1 means no selection
    bool scanning_games_ = false;  // True while background discovery is running
    
    // Console Log
    ImGuiTextBuffer log_buffer_;
    SDL_Mutex* log_buffer_mutex_;
    bool scroll_to_bottom_;
    SDL_LogOutputFunction original_log_function_;
    void* original_log_userdata_;

    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    void RenderSessionControls();
    void RenderDebugTools();
    void RenderSaveStateTools();        // Save state management tab
    void RenderMultiClientTools();      // Multi-client testing tab
    void RenderNetworkTools();          // Network simulation tab
    void RenderPerformanceStats();      // Performance statistics tab
    
    // Helper methods
    void ShowGameValidationStatus(const FM2K::FM2KGameInfo& game);
    void ShowNetworkDiagnostics();
    bool ValidateNetworkConfig();
    
    enum class UITheme { Dark, Light, System, DarkCyan };
    void SetTheme(UITheme theme);
    UITheme current_theme_;

    // int selected_game_index_ = -1; // -1 means no selection
    // bool scanning_games_ = false;  // True while background discovery is running

private:
    void ApplyDarkCyanThemeStyle();
}; 